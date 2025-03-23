#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "atomsnap.h"
#include "scalable_queue.h"

struct scq_node {
	struct scq_node *next;
	void *datum;
	_Atomic int is_dequeued;
};

struct scq_head_version {
	struct atomsnap_version version;
	struct scq_head_version *head_version_prev;
	struct scq_head_version *head_version_next;
	struct scq_node *tail_node;
	struct scq_node *head_node;
};

struct scalable_queue {
	struct scq_node *tail;
	struct atomsnap_gate *head;
	_Atomic int head_init_flag;
};

struct atomsnap_version *scq_head_version_alloc(void *)
{
	struct scq_head_version *head_version
		= calloc(1, sizeof(struct scq_head_version));
	return (struct atomsnap_version *)head_version;
}

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

void scq_enqueue(struct scalable_queue *scq, void *datum)
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

void *scq_dequeue(struct scalable_queue *scq)
{
	struct scq_head_version *head_version = NULL;
	struct scq_node *node = NULL;
	void *datum = NULL;
	bool found = false;

	if (atomic_load(&scq->head_init_flag) == 0)
		return NULL;
	
retry:

	head_version
		= (struct scq_head_version *)atomsnap_acquire_version(scq->head);

	node = head_version->head_node;

	while (node != NULL && atomic_load(&head_version->tail_node) == NULL) {
		if (atomic_load(&node->is_dequeued) == 0) {
			if (atomic_exchange(&node->is_dequeued, 1) == 0) {
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
