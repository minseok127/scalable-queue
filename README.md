# Scalable Queue (scq)
- Linked list queue
- Multi-producer, multi-consumer
- RCU based memory management

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

void scq_enqueue(struct scalable_queue *scq, void *datum);

void *scq_dequeue(struct scalable_queue *scq);
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

## Single Producer, Multiple Consumer

| # of Producer / Consumer  | Enqueue Throughput (ops/sec) |  Dequeue Throughput (ops/sec)  |
|:-------------------------:|:----------------------------:|:------------------------------:|
|	      1 / 1         |          10,978,814	   |            6,114,780           |
|	      1 / 2 	    |          10,732,562          |            4,888,025           |
|	      1 / 4 	    |           8,253,449          |            6,338,300           |
|	      1 / 8 	    |           6,840,558          |            6,840,558           |

## Multiple Producer, Single Consumer

| # of Producer / Consumer  | Enqueue Throughput (ops/sec) |  Dequeue Throughput (ops/sec)  |
|:-------------------------:|:----------------------------:|:------------------------------:|
|	      1 / 1         |          10,978,814	   |            6,114,780           |
|	      2 / 1         |          13,212,655	   |            4,518,071           |
|	      4 / 1         |          15,278,075	   |            2,534,580           |
|	      8 / 1         |          16,728,016	   |            1,304,189           |

## Multiple Producer, Multiple Consumer

| # of Producer / Consumer  | Enqueue Throughput (ops/sec) |  Dequeue Throughput (ops/sec)  |
|:-------------------------:|:----------------------------:|:------------------------------:|
|	      1 / 1         |          10,978,814	   |            6,114,780           |
|	      2 / 2         |          11,491,568	   |            3,845,078           |
|	      4 / 4         |          12,135,869	   |            4,384,864           |
|	      8 / 8         |          13,659,815	   |            5,259,186           |
