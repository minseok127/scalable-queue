#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>

#include "scalable_queue.h"

#define MAX_SCQ_NUM (1024)
#define MAX_THREAD_NUM (1024)

/*
 * scq_node - Linked list node
 * @next: pointer to the next inserted node
 * @datum: 8 bytes scalar or pointer
 *
 * When scq_enqueue is called, an scq_node is allocated and inserted into the
 * linked list queue. When scq_dequeue is called, the nodes are detached from
 * the shared linked list and attached into thread-local linked list.
 */
struct scq_node {
	struct scq_node *next;
	uint64_t datum;
};

/* 
 * During initialization, the scalable_queue is assigned a unique ID. 
 * This ID is later used when threads access the dequeued nodes.
 */
_Atomic int global_scq_id_flag;
static int global_scq_id_arr[MAX_SCQ_NUM];

/*
 * Dequeue thread detaches nodes from the shared linked list and brings them
 * into its thread-local linked list.
 */
struct scq_dequeued_node_list {
	struct scq_node *head;
	struct scq_node *tail;
};

struct scq_tls_data {
	struct scq_dequeued_node_list dequeued_node_list;
};

_Thread_local static struct scq_tls_data *tls_data_ptr_arr[MAX_SCQ_NUM];

/*
 * scalable_queue - main data structure to manage queue
 * @tail: point where a new node is inserted into the linked list
 * @sentinel: dummy node
 * @spinlock: spinlock to manage thread-local data structures
 * @scq_id: global id of the scalable_queue
 */
struct scalable_queue {
	struct scq_node *tail;
	struct scq_node sentinel;
	struct scq_tls_data *tls_data_ptr_list[MAX_SCQ_NUM];
	pthread_spinlock_t spinlock;
	int scq_id;
	int thread_num;
};

_Thread_local static struct scalable_queue *tls_scq_ptr_arr[MAX_SCQ_NUM];

/*
 * Returns pointer to an scalable_queue, or NULL on failure.
 */
struct scalable_queue *scq_init(void)
{
	struct scalable_queue *scq = calloc(1, sizeof(struct scalable_queue));

	if (scq == NULL) {
		fprintf(stderr, "scalable_queue_init: queue allocation failed\n");
		return NULL;
	}

	scq->sentinel.next = NULL;
	scq->tail = &scq->sentinel;
	scq->thread_num = 0;

	if (pthread_spin_init(&scq->spinlock, PTHREAD_PROCESS_PRIVATE) != 0) {
		fprintf(stderr, "scalable_queue_init: spinlock init failed\n");
		free(scq);
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
	struct scq_tls_data *tls_data_ptr;
	struct scq_dequeued_node_list *dequeued_node_list;
	struct scq_node *node, *prev_node;

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

	/* Free shared linked list */
	node = scq->sentinel.next;
	while (node != NULL) {
		prev_node = node;
		node = node->next;
		free(prev_node);
	}

	/* Free thread-local linked lists */
	for (int i = 0; i < scq->thread_num; i++) {
		tls_data_ptr = scq->tls_data_ptr_list[i];
		dequeued_node_list = &tls_data_ptr->dequeued_node_list;

		if (dequeued_node_list->head == NULL) {
			continue;
		}

		node = dequeued_node_list->head;
		while (node != dequeued_node_list->tail) {
			prev_node = node;
			node = node->next;
			free(prev_node);
		}
		free(dequeued_node_list->tail);
		free(tls_data_ptr);
	}

	pthread_spin_destroy(&scq->spinlock);

	free(scq);
}

/*
 * Has this scalable_queue been accessed by this thread before?
 * If not, initialize thread local data.
 */
static void check_and_init_scq_tls_data(struct scalable_queue *scq)
{
	struct scq_tls_data *tls_data = NULL;

	if (tls_scq_ptr_arr[scq->scq_id] == scq) {
		return;
	}

	tls_data = (struct scq_tls_data *)calloc(1, sizeof(struct scq_tls_data));
	tls_data_ptr_arr[scq->scq_id] = tls_data;

	tls_data->dequeued_node_list.head = NULL;
	tls_data->dequeued_node_list.tail = NULL;

	pthread_spin_lock(&scq->spinlock);
	scq->tls_data_ptr_list[scq->thread_num] = tls_data;
	scq->thread_num++;
	pthread_spin_unlock(&scq->spinlock);

	tls_scq_ptr_arr[scq->scq_id] = scq;
}

/*
 * Enqueue the given datum into the queue.
 */
void scq_enqueue(struct scalable_queue *scq, uint64_t datum)
{
	struct scq_node *node = NULL;
	struct scq_node *prev_tail = NULL;

	check_and_init_scq_tls_data(scq);

	node = (struct scq_node *)malloc(sizeof(struct scq_node));
	node->datum = datum;
	node->next = NULL;

	prev_tail = atomic_exchange(&scq->tail, node);
	assert(prev_tail != NULL);

	__sync_synchronize();
	prev_tail->next = node;
}

/*
 * Dequeue the datum from the scalable_queue.
 * Return true if there is dequeued node.
 */
bool scq_dequeue(struct scalable_queue *scq, uint64_t *datum)
{
	struct scq_dequeued_node_list *tls_node_list = NULL;
	struct scq_tls_data *tls_data = NULL;
	struct scq_node *node = NULL;

	check_and_init_scq_tls_data(scq);

	tls_data = tls_data_ptr_arr[scq->scq_id];
	tls_node_list = &tls_data->dequeued_node_list;

	if (tls_node_list->head != NULL) {
		node = tls_node_list->head;
		*datum = node->datum;

		if (node == tls_node_list->tail) {
			tls_node_list->head = NULL;
			tls_node_list->tail = NULL;
		} else {
			while (node->next == NULL) {
				__asm__ __volatile__("pause");
			}

			tls_node_list->head = node->next;
		}

		free(node);

		return true;
	}

	if (scq->sentinel.next == NULL) {
		return false;
	}

	tls_node_list->head = atomic_exchange(&scq->sentinel.next, NULL);
	if (tls_node_list->head == NULL) {
		return false;
	}

	tls_node_list->tail = atomic_exchange(&scq->tail, &scq->sentinel);

	node = tls_node_list->head;
	*datum = node->datum;

	if (node == tls_node_list->tail) {
		tls_node_list->head = NULL;
		tls_node_list->tail = NULL;
	} else {
		while (node->next == NULL) {
			__asm__ __volatile__("pause");
		}

		tls_node_list->head = node->next;
	}

	free(node);

	return true;
}
