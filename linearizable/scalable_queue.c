#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "atomsnap.h"
#include "scalable_queue.h"

/*
 * scq_node - Linked list node
 * @next: pointer to the next inserted node
 * @datum: 8 bytes scalar or pointer
 * @is_deququed: flag indicating whether the node is dequeued or not
 *
 * When scq_enqueue is called, an scq_node is allocated using malloc and
 * inserted into the linked list queue. When scq_dequeue is called, the
 * is_dequeued flag is set, and the memory is freed based on RCU (Read-Copy Update).
 */
struct scq_node {
	struct scq_node *next;
	uint64_t datum;
	_Atomic int is_dequeued;
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
 *
 * Unlike the tail of the linked list, the head is managed in an RCU-like
 * manner. So the atomsnap library is used.
 */
struct scalable_queue {
	struct scq_node *tail;
	struct atomsnap_gate *head;
	_Atomic int head_init_flag;
};

/* atomsnap_make_version() will call this function */
struct atomsnap_version *scq_head_version_alloc(void *)
{
	struct scq_head_version *head_version
		= calloc(1, sizeof(struct scq_head_version));
	return (struct atomsnap_version *)head_version;
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
		free(prev_node);
		prev_node = node;
	}
	free(head_version->tail_node);

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

	atomsnap_destroy_gate(scq->head);

	if (scq->tail != NULL) {
		free(scq->tail);
	}

	free(scq);
}

/*
 * Enqueue the given datum into the queue.
 */
void scq_enqueue(struct scalable_queue *scq, uint64_t datum)
{
	struct scq_node *node, *prev_tail;
	struct scq_head_version *head;

	node = calloc(1, sizeof(struct scq_node));
	node->datum = datum;

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
 * Dequeue the datum from the scalable_queue.
 */
bool scq_dequeue(struct scalable_queue *scq, uint64_t *datum)
{
	struct scq_head_version *head_version = NULL;
	struct scq_node *node = NULL;
	void *datum = NULL;
	bool found = false;

	/* Not yet initialized */
	if (atomic_load(&scq->head_init_flag) == 0)
		return false;
	
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
		if (atomic_load(&node->is_dequeued) == 0) {
			if (atomic_exchange(&node->is_dequeued, 1) == 0) {
				*datum = node->datum;
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

	return found;
}
