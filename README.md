# Scalable Queue (scq)

Multi-producer, multi-consumer concurrent linked list queue.

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

## Compared Projects

[concurrentqueue](https://github.com/cameron314/concurrentqueue): A fast multi-producer, multi-consumer lock-free queue for C++.

## Environment

- Hardware
	- CPU: Intel Core i5-13400F (16 cores)
	- RAM: 16GB DDR5 5600MHz

- Software
	- OS: Ubuntu 24.04.1 LTS
	- Compiler: GCC 13.3.0
	- Build System: GNU Make 4.3

## Multiple Producer, Multiple Consumer

| # of Producer / Consumer  |      SCQ Enqueue (ops/sec)   |      SCQ Dequeue (ops/sec)     |   concurrentqueue Enqueue (ops/sec)   |      concurrentqueue Dequeue (ops/sec)     |
|:-------------------------:|:----------------------------:|:------------------------------:|:-------------------------------------:|:------------------------------------------:|
|	      1 / 1         |          15,373,582	   |           15,373,582           |                 19,148,068	    |                   11,229,945               |
|	      2 / 2         |          16,020,885	   |           16,020,884           |                 14,738,278	    |                    9,143,494               |
|	      4 / 4         |          17,484,357	   |           17,484,354           |                 18,741,483	    |                    7,395,087               |
|	      8 / 8         |          21,610,680	   |           21,610,678           |                 29,511,743	    |                    9,333,479               |
