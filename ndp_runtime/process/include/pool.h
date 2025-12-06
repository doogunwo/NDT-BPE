#ifndef POOL_H
#define POOL_H

#include <iostream>
#include <queue>
#include <unordered_set>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>
#include <unistd.h>

#define MAX_SHM_KEYS 64   // 최대 공유 메모리 키 개수
#define SHM_SIZE 4096     // 공유 메모리 크기
#define SHM_WRITE_SIZE 4096 // 단일 쓰기 공유 메모리 블록 크기
#define SHM_WRITE_KEY 0x2000 // 쓰기 공유 메모리 키

class Pool {
private:
    std::queue<int> available_keys;      // 사용 가능한 공유 메모리 키 큐
    std::unordered_set<int> allocated_keys; // 할당된 키 목록 관리


    // 공유 메모리 할당 함수
    int allocate_shm(int shm_key);

    // 공유 메모리 해제 함수
    void deallocate_shm(int shm_key);

public:
    Pool();  // 생성자
    ~Pool(); // 소멸자

    int allocate_key();                // 키 할당
    void release_key(int shm_key);     // 키 반환

    char* get_block_ptr(int shm_key);  // 특정 키에 대한 공유 메모리 블록 가져오기
    char* get_write_shm();             // 단일 쓰기 공유 메모리 가져오기
    size_t get_read_size() const { return SHM_SIZE; }
    size_t get_write_size() const { return SHM_SIZE; }
};

#endif // POOL_H
