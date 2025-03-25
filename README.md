# Scalable Queue (scq)
- The enqueue logic uses only a single atomic instruction.
- The dequeue logic uses only two atomic instructions.
- Multi-producer, multi-consumer linked list queue.

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
|	      1 / 1         |          10,728,590	   |           10,728,590           |
|	      2 / 2         |           9,772,263	   |            9,772,262           |
|	      4 / 4         |          11,710,232	   |           11,707,917           |
|	      8 / 8         |          14,310,577	   |           14,310,561           |
