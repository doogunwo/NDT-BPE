#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <unistd.h>
#include <vector>
#include <nlohmann/json.hpp>
#include <errno.h>
#include <algorithm>

#define SHM_READ_KEY   0x1000
#define SHM_WRITE_KEY  0x1022
#define MSG_KEY        1002
#define SHM_SIZE       131072  // 128 KiB
#define NUM_SLOTS      16      // 슬롯 수 (필요시 8/16/32 등으로 조정)

using json = nlohmann::json;

/*** 외부 심볼(있으면 사용, 없어도 무방) ***/
extern double get_time_in_us();

extern std::vector<int> token(const std::string text);

/*** 메시지 구조: slot 포함 (SPDK와 반드시 동일) ***/
struct __attribute__((packed)) bpe_msg_req {
    long      msg_type;    // == 1
    uint32_t  total_len;   // SHM_READ에 유효하게 채운 바이트
    uint64_t  req_id;      // 요청 매칭용
    uint32_t  slot;        // cdw13 그대로
};

struct __attribute__((packed)) bpe_msg_resp {
    long      msg_type;    // == 2
    uint32_t  byte_size;   // SHM_WRITE에 기록된 바이트
    uint64_t  req_id;      // 요청 매칭용
    uint32_t  slot;        // cdw13 그대로
};

enum {
    BPE_REQ_MSZ  = (int)sizeof(bpe_msg_req)  - (int)sizeof(long),
    BPE_RESP_MSZ = (int)sizeof(bpe_msg_resp) - (int)sizeof(long),
};

/*** 슬롯별 SHM 포인터 ***/
static char* g_shm_read [NUM_SLOTS] = {nullptr};
static char* g_shm_write[NUM_SLOTS] = {nullptr};

/*** SHM attach: 프로세스 시작 시 1회만, 슬롯 전체 ***/
static inline void attach_shm_slots_once() {
    static bool inited = false;
    if (inited) return;
    for (uint32_t s = 0; s < NUM_SLOTS; s++) {
        int id_r = shmget(SHM_READ_KEY  + s, SHM_SIZE, IPC_CREAT | 0660);
        int id_w = shmget(SHM_WRITE_KEY + s, SHM_SIZE, IPC_CREAT | 0660);
        if (id_r < 0 || id_w < 0) { perror("shmget"); std::exit(1); }
        void* pr = shmat(id_r, nullptr, 0);
        void* pw = shmat(id_w, nullptr, 0);
        if (pr == (void*)-1 || pw == (void*)-1) { perror("shmat"); std::exit(1); }
        g_shm_read[s]  = static_cast<char*>(pr);
        g_shm_write[s] = static_cast<char*>(pw);
    }
    inited = true;
}

/*** 입력 읽기: 반드시 total_len 만큼만 (슬롯 선택) ***/
static inline std::string read_from_spdk(uint32_t slot, uint32_t total_len) {
    char* p = g_shm_read[slot];
    return std::string(p, p + total_len);
}

/*** 출력 쓰기: SHM_SIZE 초과 금지 (슬롯 선택) ***/
static inline uint32_t write_to_spdk(uint32_t slot, const std::vector<int>& token_ids) {
    uint16_t* dest = reinterpret_cast<uint16_t*>(g_shm_write[slot]);
    size_t max_ids_to_copy = std::min(token_ids.size(), SHM_SIZE / sizeof(uint16_t));
    //루프를 돌며 변환과 쓰기를 한 번에 처리
    for (size_t i = 0; i < max_ids_to_copy; ++i) {
        int v = token_ids[i];
        dest[i] = (v < 0) ? 0 : (v > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(v));
    }
    return static_cast<uint32_t>(max_ids_to_copy * sizeof(uint16_t));
}

/*** 메시지 큐 수신(블로킹 OK) — slot 포함 ***/
static inline bool receive_spdk_command(int msg_id,
                                        uint64_t* out_req_id,
                                        uint32_t* out_total_len,
                                        uint32_t* out_slot) {
    bpe_msg_req req{};
    ssize_t n = msgrcv(msg_id, &req, BPE_REQ_MSZ, 1, 0);
    if (n < 0) { perror("[BPE] msgrcv"); return false; }

    *out_total_len = (req.total_len > SHM_SIZE) ? SHM_SIZE : req.total_len;
    *out_req_id    = req.req_id;
    *out_slot      = req.slot;
    return true;
}

/*** 메시지 큐 송신 — slot 포함 ***/
static inline void send_spdk_response(int msg_id,
                                      uint64_t req_id,
                                      uint32_t byte_size,
                                      uint32_t slot) {
    if (byte_size > SHM_SIZE) byte_size = SHM_SIZE; // 방어
    bpe_msg_resp resp { .msg_type = 2, .byte_size = byte_size, .req_id = req_id, .slot = slot };
    if (msgsnd(msg_id, &resp, BPE_RESP_MSZ, 0) < 0) {
        perror("[BPE] msgsnd");
    }
}

/*** BPE 본체 (슬롯별 SHM 사용) ***/
static inline uint32_t bpe_worker(uint32_t slot,
                                  uint32_t input_len) {
    std::string input_text = read_from_spdk(slot, input_len);
    static const std::string added_token = R"({
        "[PAD]":0,"[UNK]":1,"[CLS]":2,"[SEP]":3,"[MASK]":4
    })";

    std::vector<int> ids = token(input_text);
    return write_to_spdk(slot, ids);
}

/*** 메인 루프: 슬롯을 메시지에서 받아 해당 슬롯 SHM으로 처리 ***/
static inline void message_loop(int msg_id) {
    for (;;) {
        uint64_t req_id = 0;
        uint32_t total_len = 0;
        uint32_t slot = 0;
        if (!receive_spdk_command(msg_id, &req_id, &total_len, &slot)) {
            continue;
        }

        uint32_t out_bytes = bpe_worker(slot, total_len);
        //std::cout << out_bytes << "\n";
        send_spdk_response(msg_id, req_id, out_bytes, slot);
    }
}

int main() {
    // 메시지 큐
    int msg_id = msgget(MSG_KEY, IPC_CREAT | 0660);
    if (msg_id < 0) { perror("[BPE] msgget"); return 1; }

    // ★ 슬롯 전체 SHM attach
    attach_shm_slots_once();
    // 루프 시작
    message_loop(msg_id);
    return 0;
}
