#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <nlohmann/json.hpp>
#include <sys/time.h>
#include "../include/pool.h"
#include "../include/agent.h"

#define MSG_KEY 1234  // 메시지 큐 키

using json = nlohmann::json;

// ✅ BPE 관련 함수 (byte_level_bpe.cpp에서 가져옴)
extern std::string LoadJSONFromFile(const std::string& path);
extern std::string LoadTXTFromFile(const std::string& path);
extern std::vector<int32_t> token(const std::string& vocab_blob, const std::string& merges_blob, 
                                  const std::string& added_token, const std::string& text);

// ✅ 현재 시간을 마이크로초(µs) 단위로 반환하는 함수
double get_time_in_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)(tv.tv_sec * 1e6 + tv.tv_usec);
}

// ✅ SPDK → Pool (읽기용 공유 메모리에서 데이터 읽기)
std::string read_from_pool(Pool& pool, int shm_key) {
    char* shm_read_ptr = pool.get_block_ptr(shm_key);
    if (!shm_read_ptr) {
        std::cerr << "[ERROR] 읽기 공유 메모리 블록을 찾을 수 없음 (키: " << shm_key << ")" << std::endl;
        return "";
    }
    return std::string(shm_read_ptr, pool.get_read_size());
}

// ✅ BPE → SPDK (쓰기용 공유 메모리에 데이터 저장)
void write_to_spdk(Pool& pool, const std::vector<int32_t>& token_ids) {
    char* shm_write_ptr = pool.get_write_shm();  // 단일 쓰기 공유 메모리 블록 사용
    if (!shm_write_ptr) {
        std::cerr << "[ERROR] 쓰기 공유 메모리 블록을 찾을 수 없음" << std::endl;
        return;
    }

    size_t token_count = token_ids.size();
    size_t byte_size = token_count * sizeof(int32_t);

    memset(shm_write_ptr, 0, pool.get_write_size());
    memcpy(shm_write_ptr, &token_count, sizeof(size_t));
    memcpy(shm_write_ptr + sizeof(size_t), token_ids.data(), byte_size);
    
    std::cout << "[BPE] 저장된 토큰 개수 : " << token_count << std::endl;
}

// ✅ BPE 토큰화 및 공유 메모리 쓰기 함수
void bpe_worker(Pool& pool, int shm_key) {
    std::string added_token = R"({
        "[PAD]": 0,
        "[UNK]": 1,
        "[CLS]": 2,
        "[SEP]": 3,
        "[MASK]": 4
    })";

    // 읽기 공유 메모리에서 데이터 가져오기
    std::cout << "[BPE] SPDK로부터 데이터 읽기..." << std::endl;
    std::string input_text = read_from_pool(pool, shm_key);
    
    if (input_text.empty()) {
        std::cerr << "[ERROR] 입력 데이터가 비어 있음." << std::endl;
        return;
    }
    
    std::cout << "[DEBUG] Input Text : " << input_text << std::endl;

    // BPE 토큰화
    uint64_t start_time = get_time_in_us();
    std::cout << "[BPE] BPE 토큰화 수행 중..." << std::endl;
    
    std::vector<int32_t> token_ids = token(pool.get_vocab(), pool.get_merges(), added_token, input_text);
    
    uint64_t end_time = get_time_in_us();
    printf("[TIME] Tokenization: %lu µs\n", end_time - start_time);

    std::cout << "[BPE] BPE 토큰화 완료, 공유 메모리에 저장 중..." << std::endl;
    
    // 쓰기 공유 메모리에 데이터 저장
    write_to_spdk(pool, token_ids);
}
