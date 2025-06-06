# Multi-producer, multi-consumer linked list queue

This repository contains two queue implementations:

1. Fully Linearizable Queue => scalable-queue/linearizable/scalable_queue.c
	- Provides a fully linearizable (strictly FIFO) queue implementation without lock.
	- Guarantees that data enqueued first across all threads is dequeued first, preserving global FIFO order.
	- The head of the queue (i.e., the point where dequeue occurs) is managed through versioning.
		- Traversal of the queue begins by acquiring a reference to the latest head version.
		- The last thread referencing the oldest head version performs memory deallocation, ensuring safe memory access.

2. Relaxed Queue (default) => scalable-queue/scalable_queue.c
	- Designed for enhanced scalability.
	- Global FIFO order (linearizability) may not be strictly preserved.
	- Each thread maintains its own independent queue.
		- Dequeue threads perform dequeue operations from their local queues, and when the local queue becomes empty, they detach a new batch of data in bulk from the enqueue-side queues and attach it to their local queue.
		- When inserting a new node, the enqueue thread uses a single atomic instruction, while the dequeue thread uses two branch instructions and two atomic instructions to detach a batch from the shared queue.

# Build
```
$ git clone https://github.com/minseok127/scalable-queue.git
$ cd scalable-queue
$ make
=> libscq.a, libscq.so
```

# API
```
typedef struct scalable_queue scalable_queue;

typedef struct scalable_queue scq;

struct scalable_queue *scq_init(void);

void scq_destroy(struct scalable_queue *scq);

/* datum => scalar or pointer */
void scq_enqueue(struct scalable_queue *scq, uint64_t datum);

/* return => found (true / false), (*datum) => deqeueued datum */
bool scq_dequeue(struct scalable_queue *scq, uint64_t *datum);
```

# Performance

## Environment

- Hardware
	- CPU: Intel Core i5-13400F (16 cores)
	- RAM: 16GB DDR5 5600MHz

- Software
	- OS: Ubuntu 24.04.1 LTS
	- Compiler: GCC 13.3.0
	- Build System: GNU Make 4.3

## Test code
```
$ git clone https://github.com/minseok127/scalable-queue.git
$ cd scalable-queue
$ git checkout test
$ make
$ cd test
$ make
```

## Comparison with other concurrent queue library

[concurrentqueue](https://github.com/cameron314/concurrentqueue): A fast multi-producer, multi-consumer lock-free queue for C++.

Both scalable_queue (default implementation) and concurrentqueue do not guarantee linearizability.

| # of Producer / Consumer  |      SCQ Enqueue (ops/sec)   |      SCQ Dequeue (ops/sec)     |   concurrentqueue Enqueue (ops/sec)   |      concurrentqueue Dequeue (ops/sec)     |
|:-------------------------:|:----------------------------:|:------------------------------:|:-------------------------------------:|:------------------------------------------:|
|	      1 / 1         |          15,373,582	   |           15,373,582           |                 19,148,068	    |                   11,229,945               |
|	      2 / 2         |          16,020,885	   |           16,020,884           |                 14,738,278	    |                    9,143,494               |
|	      4 / 4         |          17,484,357	   |           17,484,354           |                 18,741,483	    |                    7,395,087               |
|	      8 / 8         |          21,610,680	   |           21,610,678           |                 29,511,743	    |                    9,333,479               |

## Comparison between relaxed queue (default) and fully linearizable queue

| # of Producer / Consumer  |      SCQ Enqueue (ops/sec)   |      SCQ Dequeue (ops/sec)     |   SCQ (linearizable) Enqueue (ops/sec)  |      SCQ (linearizable) Dequeue (ops/sec)    |
|:-------------------------:|:----------------------------:|:------------------------------:|:---------------------------------------:|:--------------------------------------------:|
|	      1 / 1         |          15,373,582	   |           15,373,582           |                  9,640,310	      |                    4,256,879                 |
|	      2 / 2         |          16,020,885	   |           16,020,884           |                 10,876,095	      |                    3,774,518                 |
|	      4 / 4         |          17,484,357	   |           17,484,354           |                 12,612,972	      |                    4,556,463                 |
|	      8 / 8         |          21,610,680	   |           21,610,678           |                 14,039,995	      |                    6,256,954                 |
