# Scalable Queue (scq)
- Linked list queue
- Multi-producer, multi-consumer

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
|	        1 / 1 	          |          10,210,016          |            4,107,069           |
|	        1 / 2 	          |           6,860,537          |            4,431,729           |
|	        1 / 4 	          |           5,532,060          |            5,532,060           |

## Multiple Producer, Single Consumer

| # of Producer / Consumer  | Enqueue Throughput (ops/sec) |  Dequeue Throughput (ops/sec)  |
|:-------------------------:|:----------------------------:|:------------------------------:|
|	        1 / 1 	          |          10,210,016          |            4,107,069           |
|	        2 / 1 	          |          14,124,430          |            3,113,037           |
|	        4 / 1 	          |          16,572,306          |            2,862,134           |

## Multiple Producer, Multiple Consumer

| # of Producer / Consumer  | Enqueue Throughput (ops/sec) |  Dequeue Throughput (ops/sec)  |
|:-------------------------:|:----------------------------:|:------------------------------:|
|	        1 / 1 	          |          10,210,016          |            4,107,069           |
|	        2 / 2 	          |          11,567,288          |            3,912,030           |
|	        4 / 4 	          |          12,324,252          |            4,258,163           |
|	        8 / 8 	          |          13,842,130          |            6,241,685           |
