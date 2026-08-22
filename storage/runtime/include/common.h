#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <sys/ipc.h>

constexpr key_t SHM_READ_KEY = 0x3000;
constexpr key_t SHM_WRITE_KEY = 0x4000;
constexpr key_t SHM_BANK_STRIDE = 0x100;
constexpr std::size_t INPUT_SHM_SIZE = 131072;  // 128 KiB per input bank
constexpr std::size_t OUTPUT_SHM_SIZE = 524288; // worst case: one int32 token per input byte
constexpr std::size_t NUM_SLOTS = 64;
constexpr std::size_t NUM_BUFFERS = 16;

constexpr key_t STATS_SHM_KEY = 0x2000;
constexpr std::size_t STATS_SHM_SIZE = 4096;
constexpr key_t IPC_CONFIG_SHM_KEY = 0x2100;
constexpr std::size_t IPC_CONFIG_SHM_SIZE = 256;
constexpr key_t IPC_STATE_SHM_KEY = 0x2200;
constexpr std::size_t IPC_STATE_SHM_SIZE = 65536;
inline constexpr const char* EVENTFD_SOCKET_PATH = "/var/tmp/ndt_bpe_eventfd.sock";
constexpr std::uint32_t BPE_OUTPUT_ERROR = 0xFFFFFFFFU;

struct BpeRuntimeStats {
    std::uint64_t magic = 0;
    std::uint32_t version = 1;
    std::uint32_t reserved = 0;

    std::uint64_t start_ts_us = 0;
    std::uint64_t last_ts_us = 0;
    std::uint64_t last_latency_us = 0;
    std::uint64_t last_req_id = 0;
    std::uint32_t last_slot = 0;
    std::uint32_t last_resp_bytes = 0;

    std::uint64_t req_total = 0;
    std::uint64_t resp_total = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;

    std::array<std::uint64_t, NUM_SLOTS> per_slot_req{};
    std::array<std::uint64_t, NUM_SLOTS> per_slot_resp{};
    std::array<std::uint64_t, NUM_SLOTS> per_slot_bytes_in{};
    std::array<std::uint64_t, NUM_SLOTS> per_slot_bytes_out{};
};

static_assert(sizeof(BpeRuntimeStats) <= STATS_SHM_SIZE,
              "BpeRuntimeStats no longer fits in the stats shared memory segment");

struct bpe_msg_req {
    std::uint32_t total_len = 0;
    std::uint64_t req_id = 0;
    std::uint32_t slot = 0;
};

struct BpeRuntimeIpcConfig {
    std::uint64_t magic = 0;
    std::uint32_t version = 1;
    std::uint32_t request_workers = 1;
    std::uint32_t exec_mode = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(BpeRuntimeIpcConfig) <= IPC_CONFIG_SHM_SIZE,
              "BpeRuntimeIpcConfig no longer fits in the config shared memory segment");

enum class BpeBufferState : std::uint32_t {
    kFree = 0,
    kWriting = 1,
    kReady = 2,
    kProcessing = 3,
    kDone = 4,
};

struct BpeSlotBufferState {
    std::uint64_t req_id = 0;
    std::uint32_t input_len = 0;
    std::uint32_t output_len = 0;
    std::uint32_t state = static_cast<std::uint32_t>(BpeBufferState::kFree);
    std::uint32_t reserved = 0;
};

struct BpeRuntimeSlotState {
    std::array<BpeSlotBufferState, NUM_BUFFERS> buffers{};
};

struct BpeRuntimeIpcState {
    std::uint64_t magic = 0;
    std::uint32_t version = 1;
    std::uint32_t reserved = 0;
    std::array<BpeRuntimeSlotState, NUM_SLOTS> slots{};
};

static_assert(sizeof(BpeRuntimeIpcState) <= IPC_STATE_SHM_SIZE,
              "BpeRuntimeIpcState no longer fits in the state shared memory segment");

enum class InputMode : std::uint32_t {
    kText = 0,
    kArrow = 1,
};

enum class ExecMode : std::uint32_t {
    kInline = 0,
    kThread = 1,
    kProcess = 2,
};

// kArrow is kept as a command-line compatibility alias. Runtime input is always a pure bounded UTF-8 text payload; Arrow parsing is performed before staging on the host side.
class ShmSlotWorker {
public:
    using BankPtrs = std::array<char*, NUM_BUFFERS>;

    ShmSlotWorker() = default;
    ShmSlotWorker(std::uint32_t slot, BankPtrs read_ptrs, BankPtrs write_ptrs, InputMode mode);

    std::uint32_t Process(std::size_t bank_index, std::uint32_t input_len) const;
    std::uint32_t slot() const { return slot_; }

private:
    std::uint32_t slot_ = 0;
    BankPtrs read_ptrs_{};
    BankPtrs write_ptrs_{};
    InputMode mode_ = InputMode::kText;
};

class SharedMemorySlots {
public:
    SharedMemorySlots();
    ~SharedMemorySlots();

    SharedMemorySlots(const SharedMemorySlots&) = delete;
    SharedMemorySlots& operator=(const SharedMemorySlots&) = delete;

    std::vector<ShmSlotWorker> CreateWorkers(InputMode mode) const;
    std::vector<ShmSlotWorker> CreateWorkers() const { return CreateWorkers(InputMode::kText); }

private:
    std::array<std::array<int, NUM_BUFFERS>, NUM_SLOTS> read_ids_{};
    std::array<std::array<int, NUM_BUFFERS>, NUM_SLOTS> write_ids_{};
    std::array<std::array<char*, NUM_BUFFERS>, NUM_SLOTS> read_ptrs_{};
    std::array<std::array<char*, NUM_BUFFERS>, NUM_SLOTS> write_ptrs_{};
};

class MessageQueueDispatcher {
public:
    struct Options {
        ExecMode exec_mode = ExecMode::kInline;
        std::size_t workers = 0;
    };

    explicit MessageQueueDispatcher(std::vector<ShmSlotWorker> workers);
    MessageQueueDispatcher(std::vector<ShmSlotWorker> workers, Options options);

    void Run() const;

private:
    std::vector<ShmSlotWorker> workers_;
    Options options_{};
};
