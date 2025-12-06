// slot_manager.cpp - 읽기 슬롯/쓰기 슬롯 상태 추적을 위한 free/busy 큐 구조
// [설계 설명]
// - free 큐: lock-free 라이브러리로 고속 슬롯 할당 수행 (예: moodycamel::ConcurrentQueue 적용)
// - busy 큐: std::list + mutex로 관리, 이유는 슬롯이 동시 처리되며 중간에 완료될 수 있기 때문에
//   → 중간 삭제(remove)가 가능한 구조가 필요하고, 처리 완료된 슬롯을 정확히 제거할 수 있어야 함

#include "../include/slot.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>
#include <iostream>
#include <atomic>
#include <list>
#include <mutex>
#include "readerwriterqueue.h" // moodycamel::ConcurrentQueue

#define SLOT_COUNT 4
#define SLOT_SIZE 4096
#define READ_SLOT_KEY_BASE 0x2000
#define WRITE_SLOT_KEY_BASE 0x3000



static Slot read_slots[SLOT_COUNT];
static Slot write_slots[SLOT_COUNT];

// free 큐는 lock-free 구조 (moodycamel)
static moodycamel::ConcurrentQueue<int> free_read_queue;
static moodycamel::ConcurrentQueue<int> free_write_queue;

// busy 큐는 중간 삭제를 위해 std::list + mutex 사용
static std::list<int> busy_read_list;
static std::list<int> busy_write_list;
static std::mutex read_mutex;
static std::mutex write_mutex;

void init_all_slots() {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        // 읽기 슬롯 초기화
        int key_read = READ_SLOT_KEY_BASE + i;
        int id_read = shmget(key_read, SLOT_SIZE, IPC_CREAT | 0666);
        if (id_read < 0) {
            perror("[READ SLOT] shmget 실패");
            exit(1);
        }
        char* ptr_read = static_cast<char*>(shmat(id_read, NULL, 0));
        if (ptr_read == (char*)-1) {
            perror("[READ SLOT] shmat 실패");
            exit(1);
        }
        read_slots[i] = {i, ptr_read, EMPTY};
        free_read_queue.enqueue(i);

        // 쓰기 슬롯 초기화
        int key_write = WRITE_SLOT_KEY_BASE + i;
        int id_write = shmget(key_write, SLOT_SIZE, IPC_CREAT | 0666);
        if (id_write < 0) {
            perror("[WRITE SLOT] shmget 실패");
            exit(1);
        }
        char* ptr_write = static_cast<char*>(shmat(id_write, NULL, 0));
        if (ptr_write == (char*)-1) {
            perror("[WRITE SLOT] shmat 실패");
            exit(1);
        }
        write_slots[i] = {i, ptr_write, EMPTY};
        free_write_queue.enqueue(i);
    }

    std::cout << "[SLOT] 읽기/쓰기 슬롯 상태 추적 큐 구조 초기화 완료 (각각 " << SLOT_COUNT << "개)" << std::endl;
}

Slot& get_read_slot(int slot_id) {
    return read_slots[slot_id];
}

Slot& get_write_slot(int slot_id) {
    return write_slots[slot_id];
}

int allocate_read_slot(int slot_id) {
    if (!free_read_queue.try_dequeue(slot_id)) return -1;
    {
        std::lock_guard<std::mutex> lock(read_mutex);
        busy_read_list.push_back(slot_id);
    }
    read_slots[slot_id].status.store(RESERVED);
    return slot_id;
}

int allocate_write_slot(int slot_id) {
    if (!free_write_queue.try_dequeue(slot_id)) return -1;
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        busy_write_list.push_back(slot_id);
    }
    write_slots[slot_id].status.store(RESERVED);
    return slot_id;
}

void free_read_slot(int slot_id) {
    read_slots[slot_id].status.store(EMPTY);
    {
        std::lock_guard<std::mutex> lock(read_mutex);
        busy_read_list.remove(slot_id);
    }
    free_read_queue.enqueue(slot_id);
}

void free_write_slot(int slot_id) {
    write_slots[slot_id].status.store(EMPTY);
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        busy_write_list.remove(slot_id);
    }
    free_write_queue.enqueue(slot_id);
}

void set_read_slot_status(int slot_id, SlotStatus status) {
    read_slots[slot_id].status.store(status);
}

void set_write_slot_status(int slot_id, SlotStatus status) {
    write_slots[slot_id].status.store(status);
}

char* get_read_slot_ptr(int slot_id) {
    return read_slots[slot_id].ptr;
}

char* get_write_slot_ptr(int slot_id) {
    return write_slots[slot_id].ptr;
}
