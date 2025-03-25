#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <unordered_map>

#include "../scalable_queue.h"

static std::atomic<bool> g_running{true};           // 실행 중 여부
static std::atomic<long long> g_enqueueCount{0};    // 전체 enqueue 횟수
static std::atomic<long long> g_dequeueCount{0};    // 전체 dequeue 횟수

std::unordered_map<int, std::vector<struct scalable_queue *>> scq_map;

// 프로듀서 스레드 함수
void producerFunc(int producer_id)
{
	std::vector<struct scalable_queue *> scq_vector = scq_map[producer_id];

    while (g_running.load(std::memory_order_relaxed)) {
		for (struct scalable_queue *scq : scq_vector) {
        	// 간단히 정수 하나를 enqueue한다고 가정
        	scq_enqueue(scq, (void*)42);

        	// 카운트 증가
        	g_enqueueCount.fetch_add(1, std::memory_order_relaxed);
		}
    }

}

// 컨슈머 스레드 함수
void consumerFunc(int producer_num)
{
    while (g_running.load(std::memory_order_relaxed)) {
		for (int i = 0; i < producer_num; i++) {
			std::vector<struct scalable_queue *> scq_vector = scq_map[i];

			for (struct scalable_queue *scq : scq_vector) {
        		void *item = scq_dequeue(scq);

				if ((uint64_t)item == 42) {
					// consumer가 성공적으로 아이템을 받았다면, 카운트 증가
            		g_dequeueCount.fetch_add(1, std::memory_order_relaxed);
        		}
			}
		}
    }
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <num_producers> <num_consumers> <run_seconds>\n";
        return 1;
    }

    int numProducers = std::atoi(argv[1]);
    int numConsumers = std::atoi(argv[2]);
    int runSeconds   = std::atoi(argv[3]);

	for (int i = 0; i < numProducers; i++) {
		for (int j = 0; j < 4; j++) {
			struct scalable_queue *scq = scq_init();
			scq_map[i].push_back(scq);
		}
	}

    // 스레드 생성
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    producers.reserve(numProducers);
    consumers.reserve(numConsumers);

    for (int i = 0; i < numProducers; i++) {
        producers.emplace_back(producerFunc, i);
    }
    for (int i = 0; i < numConsumers; i++) {
        consumers.emplace_back(consumerFunc, numProducers);
    }

    // runSeconds 만큼 대기
    std::this_thread::sleep_for(std::chrono::seconds(runSeconds));

    // 시간 만료 -> 종료 플래그 설정
    g_running.store(false, std::memory_order_relaxed);

    // 모든 스레드 조인
    for (auto &th : producers) {
        th.join();
    }
    for (auto &th : consumers) {
        th.join();
    }

	for (int i = 0; i < numProducers; i++) {
		for (int j = 0; j < 4; j++) {
			scq_destroy(scq_map[i][j]);
		}
	}

    // 결과 계산
    long long totalEnqs = g_enqueueCount.load(std::memory_order_relaxed);
    long long totalDeqs = g_dequeueCount.load(std::memory_order_relaxed);

    double enqPerSec = (double)totalEnqs / runSeconds;
    double deqPerSec = (double)totalDeqs / runSeconds;

	std::cout << std::fixed << std::setprecision(0);
    std::cout << "=== Benchmark Results ===\n";
    std::cout << "Producers: " << numProducers
              << ", Consumers: " << numConsumers
              << ", Duration(s): " << runSeconds << "\n";
    std::cout << "Total Enqueues: " << totalEnqs
              << ", Total Dequeues: " << totalDeqs << "\n";
    std::cout << "Enqueues/sec: " << enqPerSec
              << ", Dequeues/sec: " << deqPerSec << "\n";

    return 0;
}

