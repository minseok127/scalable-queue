#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "../scalable_queue.h"

static std::atomic<bool> g_running{true};           // 실행 중 여부
static std::atomic<long long> g_enqueueCount{0};    // 전체 enqueue 횟수
static std::atomic<long long> g_dequeueCount{0};    // 전체 dequeue 횟수

#define VAL_COUNT (20000)
static std::atomic<int> arr[VAL_COUNT] = { 0, };

// 프로듀서 스레드 함수
void producerFunc(struct scalable_queue *scq)
{
	for (uint64_t i = 1; i <= VAL_COUNT; i++) {
        // 간단히 정수 하나를 enqueue한다고 가정
        scq_enqueue(scq, i);

        // 카운트 증가
        g_enqueueCount.fetch_add(1, std::memory_order_relaxed);

		if (!g_running.load(std::memory_order_relaxed)) {
			break;
		}	
    }
}

// 컨슈머 스레드 함수
void consumerFunc(struct scalable_queue *scq)
{
    while (g_running.load(std::memory_order_relaxed)) {
		uint64_t datum;
        bool found = scq_dequeue(scq, &datum);
        if (found && (uint64_t)datum >= 1 && (uint64_t)datum <= VAL_COUNT) {
            // consumer가 성공적으로 아이템을 받았다면, 카운트 증가
            g_dequeueCount.fetch_add(1, std::memory_order_relaxed);
			arr[(uint64_t)datum - 1].fetch_add(1, std::memory_order_relaxed);
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

    // 큐 초기화
    struct scalable_queue *scq = scq_init();
    if (!scq) {
        std::cerr << "Failed to initialize scalable queue.\n";
        return 1;
    }

    // 스레드 생성
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    producers.reserve(numProducers);
    consumers.reserve(numConsumers);

    for (int i = 0; i < numProducers; i++) {
        producers.emplace_back(producerFunc, scq);
    }
    for (int i = 0; i < numConsumers; i++) {
        consumers.emplace_back(consumerFunc, scq);
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

	int invalid_value_count = 0;
	for (int i = 0; i < VAL_COUNT; i++) {
		if (arr[i].load(std::memory_order_relaxed) != numProducers) {
			invalid_value_count++;
			std::cout << "invalid index: " << i << std::endl;
		}
	}
	std::cout << "invalid count: " << invalid_value_count << std::endl;

    // 결과 계산
    long long totalEnqs = g_enqueueCount.load(std::memory_order_relaxed);
    long long totalDeqs = g_dequeueCount.load(std::memory_order_relaxed);

    double enqPerSec = (double)totalEnqs / runSeconds;
    double deqPerSec = (double)totalDeqs / runSeconds;

    // 큐 해제
    scq_destroy(scq);

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

