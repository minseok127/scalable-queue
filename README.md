# Scalable Queue (scq)
- The enqueue logic uses only a single atomic instruction.
- The dequeue logic uses only two atomic instructions.
- Multi-producer, multi-consumer concurrent linked list queue.

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

## Multiple Producer, Multiple Consumer

| # of Producer / Consumer  | Enqueue Throughput (ops/sec) |  Dequeue Throughput (ops/sec)  |
|:-------------------------:|:----------------------------:|:------------------------------:|
|	      1 / 1         |           8,407,554	   |            8,407,554           |
|	      2 / 2         |          11,409,268	   |           11,409,268           |
|	      4 / 4         |          13,093,344	   |           13,093,288           |
|	      8 / 8         |          14,310,577	   |           14,310,561           |
