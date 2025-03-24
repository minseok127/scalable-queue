#define _GNU_SOURCE
#include <sys/mman.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "atomsnap.h"
#include "scalable_queue.h"

#define MAX_SCQ_NUM (1024)
#define HUGE_PAGE_COUNT	(512)
#define HUGE_PAGE_SIZE	(1024 * 1024 * 2ULL)

/* 
 * During initialization, the scalable_queue is assigned a unique ID. 
 * This ID is later used when threads access the node pool.
 */
_Atomic int global_scq_id_flag;
static int global_scq_id_arr[MAX_SCQ_NUM];

/*
 * scq_node_pool - pre-allocated scq_node memory
 * @base_addr: base address of the contigous huge pages.
 * @phys_huge_page_count: number of physical huge pages.
 * @node_count_per_huge_page; number of nodes per huge page.
 * @current_huge_page_idx: huge page index of the new node.
 * @current_node_idx: index in the huge page of the new node.
 *
 * When a thread calls scq_create_tls_node_pool(), nodes are allocated from the
 * scq_node_pool instead of using malloc.
 *
 * The scq_node_pool reserves 512 contiguous 2MB huge pages in virtual memory,
 * while the physical pages are gradually expanded as needed.
 */
struct scq_node_pool {
	void *base_addr;
	uint32_t phys_huge_page_count;
	uint32_t node_count_per_huge_page;
	uint32_t current_huge_page_idx;
	uint32_t current_node_idx;
};

/*
 * The scq ID allows access to the thread-local node pool. The node pool is
 * created using scq_create_tls_node_pool() and freed with
 * scq_destroy_tls_node_pool(). If the node pool is not used, malloc and free
 * are used by default.
 */
_Thread_local static struct scq_node_pool *scq_node_pool_ptrs[MAX_SCQ_NUM];

/* 
 * States of scq_nodes.
 * Even after a node is dequeued, it is not immediately freed.
 * So distinct states are maintained.
 */
typedef enum scq_node_state {
	SCQ_NODE_FREE = 0,
	SCQ_NODE_ENQUEUED = 1,
	SCQ_NODE_DEQUEUED = 2
} scq_node_state;

/*
 * scq_node - Linked list node
 * @next: pointer to the next inserted node
 * @datum: 8 bytes scalar or pointer
 * @state: SCQ_NODE_ENQUEUED / SCQ_NODE_DEQUEUED / SCQ_NODE_FREE
 * @is_node_pool: flag indicating whether the node is allocated from node pool
 *
 * When scq_enqueue is called, an scq_node is allocated and inserted into the
 * linked list queue. When scq_dequeue is called, the state is change to the
 * SCQ_NODE_DEQUEUED, and its memory is managed based on RCU.
 */
struct scq_node {
	struct scq_node *next;
	void *datum;
	_Atomic scq_node_state state;
	bool is_node_pool;
};

/*
 * scq_tail_version - Data structure to cover the lifetime of the nodes.
 * @version: atomsnap_version to manage grace-period
 * @head_version_prev: previously created tail version
 * @head_version_next: next head version
 * @tail_node: most recent node covered the lifetime by this head verison
 * @head_node: oldest node covered the lifetime by this head version
 *
 * scq_nodes are managed as a linked list, and the head of the list is managed
 * in an RCU-like manner. This means that when moving the head, intermediate
 * nodes are not immediately freed but are given a grace period, which is
 * managed using the atomsnap library.
 *
 * When the tail is updated, a range of nodes covering the lifetime of the
 * previous tail is created. If this range is at the end of the linked list, it
 * indicates that other threads no longer traverse this range of nodes.
 *
 * To verify this, each tail version is also linked via a pointer, and
 * this pointer is used to determine whether the current version represents the
 * last segment of the linked list.
 */
struct scq_head_version {
	struct atomsnap_version version;
	struct scq_head_version *head_version_prev;
	struct scq_head_version *head_version_next;
	struct scq_node *tail_node;
	struct scq_node *head_node;
};

/*
 * scalable_queue - main data structure to manage queue
 * @tail: point where a new node is inserted into the linked list
 * @head: point where the oldest node is located
 * @head_init_flag: whether or not the head is initialized
 * @scq_id: global id of the scalable_queue
 *
 * Unlike the tail of the linked list, the head is managed in an RCU-like
 * manner. So the atomsnap library is used.
 */
struct scalable_queue {
	struct scq_node *tail;
	struct atomsnap_gate *head;
	_Atomic int head_init_flag;
	int scq_id;
};

/* atomsnap_make_version() will call this function */
struct atomsnap_version *scq_head_version_alloc(void *)
{
	struct scq_head_version *head_version
		= calloc(1, sizeof(struct scq_head_version));
	return (struct atomsnap_version *)head_version;
}

/*
 * Return the node into the node_pool, or free it.
 */
static void scq_free_node(struct scq_node *node)
{
	if (!node->is_node_pool) {
		free(node);
		return;
	}

	atomic_store(&node->state, SCQ_NODE_FREE);
}

/* 
 * The last thread that releases the reference to the head version is
 * responsible for calling this function.
 *
 * See the comments of the struct scq_head_version and adjust_head().
 */
#define HEAD_VERSION_RELEASE_MASK (0x8000000000000000ULL)
void scq_head_version_free(struct atomsnap_version *version)
{
	struct scq_head_version *head_version = (struct scq_head_version *)version;
	struct scq_head_version *next_head_version = NULL;
	struct scq_head_version *prev_ptr
		= (struct scq_head_version *)atomic_fetch_or(
			&head_version->head_version_prev, HEAD_VERSION_RELEASE_MASK);
	struct scq_node *prev_node, *node;

	/* This is not the end of linked list, so we can't free the nodes */
	if (prev_ptr != NULL) {
		return;
	}

	__sync_synchronize();

free_head_version:

	/* This range is the last. So we can free these safely */
	node = head_version->head_node;
	prev_node = node;
	while (node != head_version->tail_node) {
		node = node->next;
		scq_free_node(prev_node);
		prev_node = node;
	}
	scq_free_node(head_version->tail_node);

	next_head_version
		= (struct scq_head_version *)head_version->head_version_next;

	free(head_version);

	prev_ptr = (struct scq_head_version *)atomic_load(
		&next_head_version->head_version_prev);

	if (((uint64_t)prev_ptr & HEAD_VERSION_RELEASE_MASK) != 0 ||
		!atomic_compare_exchange_weak(&next_head_version->head_version_prev,
			&prev_ptr, NULL)) {
		head_version = next_head_version;
		goto free_head_version;
	}
}

/*
 * Returns pointer to an scalable_queue, or NULL on failure.
 */
struct scalable_queue *scq_init(void)
{
	struct atomsnap_init_context ctx = {
		.atomsnap_alloc_impl = scq_head_version_alloc,
		.atomsnap_free_impl = scq_head_version_free
	};
	struct scalable_queue *scq = calloc(1, sizeof(struct scalable_queue));

	if (scq == NULL) {
		fprintf(stderr, "scalable_queue_init: queue allocation failed\n");
		return NULL;
	}

	scq->head = atomsnap_init_gate(&ctx);
	if (scq->head == NULL) {
		fprintf(stderr, "scalable_queue_init: atomsnap_init_gate() failed\n");
		return NULL;
	}

	/* Get the spinlock to assign scq id */
	while (atomic_exchange(&global_scq_id_flag, 1) == 1) {
		__asm__ __volatile__("pause");
	}

	scq->scq_id = -1;
	for (int i = 0; i < MAX_SCQ_NUM; i++) {
		if (atomic_load(&global_scq_id_arr[i]) == 0) {
			atomic_store(&global_scq_id_arr[i], 1);
			scq->scq_id = i;
			break;
		}
	}

	atomic_store(&global_scq_id_flag, 0);

	/* Invalid id */
	if (scq->scq_id == -1) {
		fprintf(stderr, "scalable_queue_init: invalid scq id\n");
		atomsnap_destroy_gate(scq->head);
		free(scq);
		return NULL;
	}

	return scq;
}

/*
 * Destroy the given scalable_queue.
 */
void scq_destroy(struct scalable_queue *scq)
{
	if (scq == NULL) {
		return;
	}

	assert(scq->scq_id >= 0 && scq->scq_id < MAX_SCQ_NUM);

	/* Get the spinlock to return scq id */
	while (atomic_exchange(&global_scq_id_flag, 1) == 1) {
		__asm__ __volatile__("pause");
	}

	atomic_store(&global_scq_id_arr[scq->scq_id], 0);

	atomic_store(&global_scq_id_flag, 0);

	atomsnap_destroy_gate(scq->head);

	free(scq);
}

/*
 * Create scq_node_pool.
 */
void scq_create_tls_node_pool(struct scalable_queue *scq)
{
	struct scq_node_pool *node_pool = calloc(1, sizeof(struct scq_node_pool));
	
	if (node_pool == NULL) {
		fprintf(stderr, "scq_create_tls_node_pool: pool allocation failed\n");
		return;
	}

	assert(scq_node_pool_ptrs[scq->scq_id] == NULL);

	/* Allocate contigous 1GB memory */
	node_pool->base_addr = mmap(NULL, HUGE_PAGE_SIZE * HUGE_PAGE_COUNT,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
		-1, 0);

	if (node_pool->base_addr == MAP_FAILED) {
		fprintf(stderr, "scq_create_tls_node_pool: mmap failed\n");
		free(node_pool);
		return;
	}

	madvise(node_pool->base_addr, HUGE_PAGE_SIZE * HUGE_PAGE_COUNT,
		MADV_HUGEPAGE);

	node_pool->phys_huge_page_count = 1;
	node_pool->node_count_per_huge_page
		= HUGE_PAGE_SIZE / sizeof(struct scq_node);
	node_pool->current_huge_page_idx = 0;
	node_pool->current_node_idx = 0;

	/* Set the pointer */
	scq_node_pool_ptrs[scq->scq_id] = node_pool;
}

/*
 * Destroy scq_node_pool.
 */
void scq_destroy_tls_node_pool(struct scalable_queue *scq)
{
	struct scq_node_pool* node_pool = NULL;

	if (scq_node_pool_ptrs[scq->scq_id] == NULL) {
		return;
	}

	node_pool = scq_node_pool_ptrs[scq->scq_id];

	munmap(node_pool->base_addr, HUGE_PAGE_SIZE * HUGE_PAGE_COUNT);

	free(node_pool);

	scq_node_pool_ptrs[scq->scq_id] = NULL;
}

/*
 * Allocate struct scq_node from node_pool. If the thread does not use
 * scq_node_pool, use malloc.
 */
static struct scq_node *scq_allocate_node(struct scalable_queue *scq)
{
	struct scq_node_pool *node_pool = scq_node_pool_ptrs[scq->scq_id];
	struct scq_node *node = NULL, *last_node = NULL;
	int new_huge_page_idx = -1;

	if (node_pool == NULL) {
		return (struct scq_node *)calloc(1, sizeof(struct scq_node));
	}

	/* If the current huge page has space */
	if (node_pool->current_node_idx < node_pool->node_count_per_huge_page) {
		node = node_pool->base_addr
			+ HUGE_PAGE_SIZE * node_pool->current_huge_page_idx
			+ sizeof(struct scq_node) * node_pool->current_node_idx;
		node->is_node_pool = true;
		node_pool->current_node_idx += 1;
		return node;
	}

	/* This huge page is full, we should seek another page */
	for (uint32_t i = 0; i < node_pool->phys_huge_page_count; i++) {
		last_node = node_pool->base_addr 
			+ HUGE_PAGE_SIZE * i
			+ sizeof(struct scq_node) * 
				(node_pool->node_count_per_huge_page - 1);

		if (last_node->state == SCQ_NODE_FREE) {
			new_huge_page_idx = i;
			break;
		}
	}

	/* No available physical pages. Extend the physical memory */
	if (new_huge_page_idx == -1) {
		/* Node pool is full, use malloc */
		if (node_pool->phys_huge_page_count == HUGE_PAGE_COUNT) {
			return (struct scq_node *)calloc(1, sizeof(struct scq_node));
		}

		new_huge_page_idx = node_pool->phys_huge_page_count;
		node_pool->phys_huge_page_count += 1;
	}

	node = node_pool->base_addr
		+ HUGE_PAGE_SIZE * new_huge_page_idx + sizeof(struct scq_node);
	node->is_node_pool = true;

	node_pool->current_huge_page_idx = new_huge_page_idx;
	node_pool->current_node_idx = 1;

	return node;
}

/*
 * Enqueue the given datum into the queue.
 */
void scq_enqueue(struct scalable_queue *scq, void *datum)
{
	struct scq_node *node, *prev_tail;
	struct scq_head_version *head;

	node = scq_allocate_node(scq);
	node->datum = datum;
	node->state = SCQ_NODE_ENQUEUED;

	prev_tail = atomic_exchange(&scq->tail, node);

	if (prev_tail == NULL) {
		head = (struct scq_head_version *)atomsnap_make_version(scq->head,NULL);

		head->head_version_prev = NULL;
		head->head_version_next = NULL;

		head->tail_node = NULL;
		head->head_node = node;

		atomsnap_exchange_version(scq->head, (struct atomsnap_version *)head);

		atomic_store(&scq->head_init_flag, 1);
	} else {
		prev_tail->next = node;
	}
}

/*
 * adjust_head - Try to move the head of queue
 * @scq: pointer of the scalable_queue
 * @prev_head_version: previous head version of the queue
 * @new_haed_node: the linked list node to set the new head
 * @tail_node_of_prev_head_version: previous linked list node of the new head
 *
 * Calling atomsnap_compare_exchange_version() in this function starts the grace
 * period for the previous head version. The last thread to release this old
 * head version will execute aru_tail_version_free().
 *
 * The reference to the old head version is released after this function
 * returns. This ensures that the deallocation for this old version will not be
 * executed until at least that point. So it is safe to link the old version
 * with the newly created version in here.
 */
static void adjust_head(struct scalable_queue *scq,
	struct scq_head_version *prev_head_version, struct scq_node *new_head_node,
	struct scq_node *tail_node_of_prev_head_version)
{	
	struct scq_head_version *new_head_version
		= (struct scq_head_version *)atomsnap_make_version(scq->head, NULL);

	new_head_version->head_version_prev = prev_head_version;
	new_head_version->head_version_next = NULL;

	new_head_version->tail_node = NULL;
	new_head_version->head_node = new_head_node;

	if (!atomsnap_compare_exchange_version(scq->head,
			(struct atomsnap_version *)prev_head_version,
			(struct atomsnap_version *)new_head_version)) {
		free(new_head_version);
		return;
	}

	__sync_synchronize();
	atomic_store(&prev_head_version->head_version_next, new_head_version);
	atomic_store(&prev_head_version->tail_node, tail_node_of_prev_head_version);
}

/*
 * Dequeue the datum from the scalable_queue.
 * Return NULL if there is no data.
 */
void *scq_dequeue(struct scalable_queue *scq)
{
	struct scq_head_version *head_version = NULL;
	struct scq_node *node = NULL;
	void *datum = NULL;
	bool found = false;

	/* Not yet initialized */
	if (atomic_load(&scq->head_init_flag) == 0)
		return NULL;
	
retry:

	head_version
		= (struct scq_head_version *)atomsnap_acquire_version(scq->head);

	node = head_version->head_node;

	/* 
	 * If the tail_node of the head_version is not NULL, it means that this head
	 * has already become an old version. Therefore, it's better to start from a
	 * new version.
	 *
	 * If the node becomes NULL before finding the data, it means the search has
	 * reached the tail of the queue without finding the data.
	 */
	while (node != NULL && atomic_load(&head_version->tail_node) == NULL) {
		if (atomic_load(&node->state) == SCQ_NODE_ENQUEUED) {
			if (atomic_exchange(&node->state, SCQ_NODE_DEQUEUED)
					== SCQ_NODE_ENQUEUED) {
				datum = node->datum;
				found = true;
				break;
			}
		}

		node = node->next;
	}

	if (node != NULL) {
		if (!found) {
			atomsnap_release_version((struct atomsnap_version *)head_version);
			goto retry;
		} else if (node->next != NULL) {
			adjust_head(scq, head_version, node->next, node);
		}
	}

	atomsnap_release_version((struct atomsnap_version *)head_version);

	return datum;
}
