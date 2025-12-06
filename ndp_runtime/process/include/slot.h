#pragma once

#include <atomic>

// 슬롯 상태 정의
enum SlotStatus {
    EMPTY,
    RESERVED
};

// 슬롯 구조체 정의
struct Slot {
    int shm_id;                      // 공유 메모리 식별자 (슬롯 ID)
    char* ptr;                       // 공유 메모리 포인터
    std::atomic<SlotStatus> status; // 슬롯 상태 (atomic 처리)
};

// 슬롯 초기화 및 상태 관리 함수들
void init_all_slots();

// 슬롯 접근자
Slot& get_read_slot(int slot_id);
Slot& get_write_slot(int slot_id);

// 슬롯 할당
int allocate_read_slot();
int allocate_write_slot();

// 슬롯 반납
void free_read_slot(int slot_id);
void free_write_slot(int slot_id);

// 슬롯 상태 설정
void set_read_slot_status(int slot_id, SlotStatus status);
void set_write_slot_status(int slot_id, SlotStatus status);

// 공유 메모리 포인터 접근자
char* get_read_slot_ptr(int slot_id);
char* get_write_slot_ptr(int slot_id);
