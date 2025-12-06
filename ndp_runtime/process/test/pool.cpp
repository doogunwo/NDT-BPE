#include "../include/pool.h"

// ✅ 생성자: 공유 메모리 초기화
Pool::Pool() {
    // 0x1000 ~ 0x103F (64개의 공유 메모리 키 생성)
    for (int i = 0x1000; i < 0x1000 + MAX_SHM_KEYS; i++) {
        available_keys.push(i);
    }

    // ✅ 단일 쓰기 공유 메모리 블록 생성
    write_shm_id = shmget(0x2000, SHM_SIZE, IPC_CREAT | 0666);
    if (write_shm_id < 0) {
        perror("[ERROR] 쓰기 공유 메모리 생성 실패");
        exit(1);
    }
    write_shm_ptr = static_cast<char*>(shmat(write_shm_id, NULL, 0));
    if (write_shm_ptr == (char *)(-1)) {
        perror("[ERROR] 쓰기 공유 메모리 연결 실패");
        exit(1);
    }
}

// ✅ 소멸자: 할당된 공유 메모리 해제
Pool::~Pool() {
    while (!available_keys.empty()) {
        available_keys.pop();
    }

    for (auto& kv : allocated_keys) {
        shmctl(kv.second, IPC_RMID, nullptr);
    }

    // 단일 쓰기 공유 메모리 해제
    shmdt(write_shm_ptr);
    shmctl(write_shm_id, IPC_RMID, nullptr);
}

// ✅ 키 할당
int Pool::allocate_key() {
    if (available_keys.empty()) {
        std::cerr << "[POOL] 할당 가능한 공유 메모리 키가 없습니다." << std::endl;
        return -1;
    }

    int shm_key = available_keys.front();
    available_keys.pop();

    // 공유 메모리 블록 생성
    int shm_id = shmget(shm_key, SHM_SIZE, IPC_CREAT | 0666);
    if (shm_id < 0) {
        perror("[ERROR] 공유 메모리 생성 실패");
        return -1;
    }

    allocated_keys[shm_key] = shm_id;
    std::cout << "[POOL] 공유 메모리 키 할당: " << shm_key << std::endl;
    return shm_key;
}

// ✅ 키 반환
void Pool::release_key(int shm_key) {
    auto it = allocated_keys.find(shm_key);
    if (it != allocated_keys.end()) {
        shmctl(it->second, IPC_RMID, nullptr);
        allocated_keys.erase(it);
        available_keys.push(shm_key);
        std::cout << "[POOL] 공유 메모리 키 반환: " << shm_key << std::endl;
    } else {
        std::cerr << "[ERROR] 존재하지 않는 키 반환 요청: " << shm_key << std::endl;
    }
}

// ✅ 특정 키의 공유 메모리 블록 가져오기
char* Pool::get_block_ptr(int shm_key) {
    auto it = allocated_keys.find(shm_key);
    if (it == allocated_keys.end()) {
        std::cerr << "[ERROR] 요청한 공유 메모리 키(" << shm_key << ")가 존재하지 않음." << std::endl;
        return nullptr;
    }

    char* shm_ptr = static_cast<char*>(shmat(it->second, NULL, 0));
    if (shm_ptr == (char *)(-1)) {
        perror("[ERROR] 공유 메모리 연결 실패");
        return nullptr;
    }
    return shm_ptr;
}

// ✅ 단일 쓰기 공유 메모리 블록 가져오기
char* Pool::get_write_shm() {
    return write_shm_ptr;
}
