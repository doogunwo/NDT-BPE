#include "common.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ipc.h>
#include <sys/shm.h>

namespace {

constexpr std::uint64_t kStatsMagic = 0x4250455354415431ULL; // "BPESTAT1"
constexpr std::uint64_t kIpcStateMagic = 0x4250455354415445ULL; // "BPESTATE"

volatile std::sig_atomic_t g_stop = 0;

void handle_sigint(int) {
    g_stop = 1;
}

std::uint64_t load_u64(const std::uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

std::uint32_t load_u32(const std::uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

double bytes_to_mb(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [--interval-ms N] [--cpu-cores 0-5] [--net-iface IFACE] "
        "[--block-dev DEV] [--csv-path FILE] [--no-clear]\n",
        prog);
}

struct CpuTimes {
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;
};

struct NetCounters {
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
    std::uint64_t rx_packets = 0;
    std::uint64_t tx_packets = 0;
    std::uint64_t rx_drop = 0;
    std::uint64_t tx_drop = 0;
};

struct BlockCounters {
    std::uint64_t read_sectors = 0;
    std::uint64_t write_sectors = 0;
    std::uint64_t io_ticks_ms = 0;
    std::uint64_t in_flight = 0;
};

bool read_proc_stat(std::vector<CpuTimes>& out) {
    std::ifstream in("/proc/stat");
    if (!in.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu", 0) != 0 || line.size() < 4 || !std::isdigit(line[3])) {
            continue;
        }
        std::istringstream iss(line);
        std::string label;
        iss >> label;
        int cpu_id = std::atoi(label.c_str() + 3);
        if (cpu_id < 0) {
            continue;
        }
        CpuTimes t{};
        iss >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >> t.steal;
        if (cpu_id >= static_cast<int>(out.size())) {
            out.resize(cpu_id + 1);
        }
        out[cpu_id] = t;
    }
    return true;
}

bool read_u64_file(const std::string& path, std::uint64_t& out) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }
    in >> out;
    return !in.fail();
}

bool read_net_counters(const std::string& iface, NetCounters& out) {
    if (iface.empty()) {
        return false;
    }
    const std::string base = "/sys/class/net/" + iface + "/statistics/";
    return read_u64_file(base + "rx_bytes", out.rx_bytes) &&
           read_u64_file(base + "tx_bytes", out.tx_bytes) &&
           read_u64_file(base + "rx_packets", out.rx_packets) &&
           read_u64_file(base + "tx_packets", out.tx_packets) &&
           read_u64_file(base + "rx_dropped", out.rx_drop) &&
           read_u64_file(base + "tx_dropped", out.tx_drop);
}

bool read_block_counters(const std::string& block_dev, BlockCounters& out) {
    if (block_dev.empty()) {
        return false;
    }
    std::ifstream in("/sys/class/block/" + block_dev + "/stat");
    if (!in.is_open()) {
        return false;
    }

    std::uint64_t read_ios = 0;
    std::uint64_t read_merges = 0;
    std::uint64_t read_ticks = 0;
    std::uint64_t write_ios = 0;
    std::uint64_t write_merges = 0;
    std::uint64_t write_ticks = 0;
    std::uint64_t time_in_queue = 0;
    in >> read_ios
       >> read_merges
       >> out.read_sectors
       >> read_ticks
       >> write_ios
       >> write_merges
       >> out.write_sectors
       >> write_ticks
       >> out.in_flight
       >> out.io_ticks_ms
       >> time_in_queue;
    return !in.fail();
}

std::vector<int> parse_cores(const std::string& spec) {
    std::set<int> cores;
    std::istringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        auto dash = token.find('-');
        if (dash == std::string::npos) {
            cores.insert(std::atoi(token.c_str()));
            continue;
        }
        int start = std::atoi(token.substr(0, dash).c_str());
        int end = std::atoi(token.substr(dash + 1).c_str());
        if (end < start) std::swap(start, end);
        for (int c = start; c <= end; ++c) {
            cores.insert(c);
        }
    }
    return std::vector<int>(cores.begin(), cores.end());
}

double cpu_usage(const CpuTimes& prev, const CpuTimes& cur) {
    const std::uint64_t prev_idle = prev.idle + prev.iowait;
    const std::uint64_t cur_idle = cur.idle + cur.iowait;
    const std::uint64_t prev_total = prev.user + prev.nice + prev.system + prev.idle +
                                     prev.iowait + prev.irq + prev.softirq + prev.steal;
    const std::uint64_t cur_total = cur.user + cur.nice + cur.system + cur.idle +
                                    cur.iowait + cur.irq + cur.softirq + cur.steal;
    const std::uint64_t total_delta = cur_total - prev_total;
    const std::uint64_t idle_delta = cur_idle - prev_idle;
    if (total_delta == 0) {
        return 0.0;
    }
    const double busy = static_cast<double>(total_delta - idle_delta);
    return 100.0 * busy / static_cast<double>(total_delta);
}

double jains_fairness_index(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    double sq_sum = 0.0;
    for (double v : values) {
        sum += v;
        sq_sum += v * v;
    }
    if (sq_sum == 0.0) {
        return 0.0;
    }
    return (sum * sum) / (static_cast<double>(values.size()) * sq_sum);
}

struct SlotIpcStateCounts {
    std::uint32_t writing = 0;
    std::uint32_t ready = 0;
    std::uint32_t processing = 0;
    std::uint32_t done = 0;

    std::uint32_t outstanding() const {
        return writing + ready + processing + done;
    }
};

struct IpcStateSnapshot {
    bool valid = false;
    std::uint64_t writing = 0;
    std::uint64_t ready = 0;
    std::uint64_t processing = 0;
    std::uint64_t done = 0;
    std::uint64_t outstanding = 0;
    std::array<SlotIpcStateCounts, NUM_SLOTS> per_slot{};
};

IpcStateSnapshot snapshot_ipc_state(const BpeRuntimeIpcState* ipc_state) {
    IpcStateSnapshot snapshot{};
    if (ipc_state == nullptr || load_u64(&ipc_state->magic) != kIpcStateMagic) {
        return snapshot;
    }

    snapshot.valid = true;
    for (std::size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        for (std::size_t bank = 0; bank < NUM_BUFFERS; ++bank) {
            const auto state = static_cast<BpeBufferState>(
                load_u32(&ipc_state->slots[slot].buffers[bank].state));
            switch (state) {
            case BpeBufferState::kFree:
                break;
            case BpeBufferState::kWriting:
                ++snapshot.writing;
                ++snapshot.per_slot[slot].writing;
                break;
            case BpeBufferState::kReady:
                ++snapshot.ready;
                ++snapshot.per_slot[slot].ready;
                break;
            case BpeBufferState::kProcessing:
                ++snapshot.processing;
                ++snapshot.per_slot[slot].processing;
                break;
            case BpeBufferState::kDone:
                ++snapshot.done;
                ++snapshot.per_slot[slot].done;
                break;
            }
        }
    }

    snapshot.outstanding = snapshot.writing + snapshot.ready + snapshot.processing + snapshot.done;
    return snapshot;
}

} // namespace

int main(int argc, char** argv) {
    int interval_ms = 1000;
    std::vector<int> cpu_cores = parse_cores("0-5");
    std::string net_iface;
    std::string block_dev;
    std::string csv_path;
    bool clear_screen = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            interval_ms = std::max(100, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--cpu-cores") == 0 && i + 1 < argc) {
            cpu_cores = parse_cores(argv[++i]);
        } else if (std::strcmp(argv[i], "--net-iface") == 0 && i + 1 < argc) {
            net_iface = argv[++i];
        } else if (std::strcmp(argv[i], "--block-dev") == 0 && i + 1 < argc) {
            block_dev = argv[++i];
        } else if (std::strcmp(argv[i], "--csv-path") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (std::strcmp(argv[i], "--no-clear") == 0) {
            clear_screen = false;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    const int id = shmget(STATS_SHM_KEY, STATS_SHM_SIZE, 0660);
    if (id < 0) {
        std::perror("[BPE-MON] stats shmget");
        return 1;
    }

    void* ptr = shmat(id, nullptr, SHM_RDONLY);
    if (ptr == reinterpret_cast<void*>(-1)) {
        std::perror("[BPE-MON] stats shmat");
        return 1;
    }

    auto* stats = reinterpret_cast<BpeRuntimeStats*>(ptr);
    if (load_u64(&stats->magic) != kStatsMagic) {
        std::fprintf(stderr, "[BPE-MON] stats magic mismatch (not initialized)\n");
        return 1;
    }

    BpeRuntimeIpcState* ipc_state = nullptr;
    const int ipc_state_id = shmget(IPC_STATE_SHM_KEY, IPC_STATE_SHM_SIZE, 0660);
    if (ipc_state_id >= 0) {
        void* ipc_ptr = shmat(ipc_state_id, nullptr, SHM_RDONLY);
        if (ipc_ptr == reinterpret_cast<void*>(-1)) {
            std::perror("[BPE-MON] state shmat");
        } else {
            ipc_state = reinterpret_cast<BpeRuntimeIpcState*>(ipc_ptr);
        }
    }

    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    std::uint64_t prev_req = 0;
    std::uint64_t prev_resp = 0;
    std::uint64_t prev_in = 0;
    std::uint64_t prev_out = 0;
    std::vector<CpuTimes> prev_cpu;
    std::vector<CpuTimes> cur_cpu;
    NetCounters prev_net{};
    NetCounters cur_net{};
    BlockCounters prev_blk{};
    BlockCounters cur_blk{};
    bool have_prev_net = read_net_counters(net_iface, prev_net);
    bool have_prev_blk = read_block_counters(block_dev, prev_blk);
    std::array<std::uint64_t, NUM_SLOTS> prev_slot_resp{};

    std::ofstream csv;
    if (!csv_path.empty()) {
        csv.open(csv_path, std::ios::out | std::ios::trunc);
        if (!csv.is_open()) {
            std::perror("[BPE-MON] csv open");
            return 1;
        }
        csv << "ts_us,req_total,resp_total,outstanding,last_latency_us,"
               "req_per_s,resp_per_s,in_MBps,out_MBps,fairness_resp,"
               "ipc_outstanding,ipc_writing,ipc_ready,ipc_processing,ipc_done,"
               "cpu_busy_all,net_rx_MBps,net_tx_MBps,net_rx_drop_delta,net_tx_drop_delta,"
               "blk_read_MBps,blk_write_MBps,blk_util_pct,blk_in_flight\n";
    }

    while (!g_stop) {
        cur_cpu.clear();
        const bool have_cpu = read_proc_stat(cur_cpu);
        const bool have_net = read_net_counters(net_iface, cur_net);
        const bool have_blk = read_block_counters(block_dev, cur_blk);
        const std::uint64_t req_total = load_u64(&stats->req_total);
        const std::uint64_t resp_total = load_u64(&stats->resp_total);
        const std::uint64_t bytes_in = load_u64(&stats->bytes_in);
        const std::uint64_t bytes_out = load_u64(&stats->bytes_out);
        const std::uint64_t start_ts = load_u64(&stats->start_ts_us);
        const std::uint64_t last_ts = load_u64(&stats->last_ts_us);
        const std::uint64_t last_latency = load_u64(&stats->last_latency_us);
        const std::uint64_t last_req_id = load_u64(&stats->last_req_id);
        const std::uint32_t last_slot = load_u32(&stats->last_slot);
        const std::uint32_t last_resp_bytes = load_u32(&stats->last_resp_bytes);
        const std::uint64_t counter_outstanding = (req_total >= resp_total) ? (req_total - resp_total) : 0;
        const IpcStateSnapshot ipc_snapshot = snapshot_ipc_state(ipc_state);
        const std::uint64_t outstanding = ipc_snapshot.valid ? ipc_snapshot.outstanding : counter_outstanding;

        const double delta_req = static_cast<double>(req_total - prev_req) * 1000.0 / interval_ms;
        const double delta_resp = static_cast<double>(resp_total - prev_resp) * 1000.0 / interval_ms;
        const double delta_in_mb = bytes_to_mb(bytes_in - prev_in) * 1000.0 / interval_ms;
        const double delta_out_mb = bytes_to_mb(bytes_out - prev_out) * 1000.0 / interval_ms;
        double net_rx_mb = 0.0;
        double net_tx_mb = 0.0;
        std::uint64_t net_rx_drop_delta = 0;
        std::uint64_t net_tx_drop_delta = 0;
        if (have_net && have_prev_net) {
            net_rx_mb = bytes_to_mb(cur_net.rx_bytes - prev_net.rx_bytes) * 1000.0 / interval_ms;
            net_tx_mb = bytes_to_mb(cur_net.tx_bytes - prev_net.tx_bytes) * 1000.0 / interval_ms;
            net_rx_drop_delta = cur_net.rx_drop - prev_net.rx_drop;
            net_tx_drop_delta = cur_net.tx_drop - prev_net.tx_drop;
        }
        double blk_read_mb = 0.0;
        double blk_write_mb = 0.0;
        double blk_util_pct = 0.0;
        if (have_blk && have_prev_blk) {
            constexpr double kSectorBytes = 512.0;
            blk_read_mb =
                static_cast<double>(cur_blk.read_sectors - prev_blk.read_sectors) * kSectorBytes /
                (1024.0 * 1024.0) * 1000.0 / interval_ms;
            blk_write_mb =
                static_cast<double>(cur_blk.write_sectors - prev_blk.write_sectors) * kSectorBytes /
                (1024.0 * 1024.0) * 1000.0 / interval_ms;
            blk_util_pct =
                std::min(100.0, static_cast<double>(cur_blk.io_ticks_ms - prev_blk.io_ticks_ms) *
                                    100.0 / static_cast<double>(interval_ms));
        }

        std::vector<double> slot_resp_deltas;
        slot_resp_deltas.reserve(NUM_SLOTS);
        std::array<std::uint64_t, NUM_SLOTS> cur_slot_resp{};
        for (std::size_t s = 0; s < NUM_SLOTS; ++s) {
            cur_slot_resp[s] = load_u64(&stats->per_slot_resp[s]);
            slot_resp_deltas.push_back(
                static_cast<double>(cur_slot_resp[s] - prev_slot_resp[s]));
        }
        const double fairness_resp = jains_fairness_index(slot_resp_deltas);
        double cpu_busy_all = 0.0;
        if (have_cpu && !prev_cpu.empty() && !cur_cpu.empty()) {
            cpu_busy_all = cpu_usage(prev_cpu[0], cur_cpu[0]);
        }

        if (clear_screen) {
            std::printf("\033[2J\033[H");
        }
        std::printf("BPE Runtime Monitor (interval=%dms)\n", interval_ms);
        std::printf("------------------------------------------------------------\n");
        std::printf("Totals   req=%" PRIu64 "  resp=%" PRIu64 "  in=%.2fMB  out=%.2fMB\n",
                    req_total, resp_total, bytes_to_mb(bytes_in), bytes_to_mb(bytes_out));
        std::printf("Rates    req/s=%.1f  resp/s=%.1f  in=%.2fMB/s  out=%.2fMB/s  outstanding=%" PRIu64 "\n",
                    delta_req, delta_resp, delta_in_mb, delta_out_mb, outstanding);
        std::printf("Last     req_id=%" PRIu64 " slot=%u resp_bytes=%u latency_us=%" PRIu64 "\n",
                    last_req_id, last_slot, last_resp_bytes, last_latency);
        std::printf("TS       start_us=%" PRIu64 " last_us=%" PRIu64 "\n",
                    start_ts, last_ts);
        std::printf("Fairness resp_jain=%.3f\n", fairness_resp);
        if (ipc_snapshot.valid) {
            std::printf("IPC      live=%" PRIu64 " writing=%" PRIu64 " ready=%" PRIu64
                        " processing=%" PRIu64 " done=%" PRIu64 " counter=%" PRIu64 "\n",
                        ipc_snapshot.outstanding,
                        ipc_snapshot.writing,
                        ipc_snapshot.ready,
                        ipc_snapshot.processing,
                        ipc_snapshot.done,
                        counter_outstanding);
        }
        if (have_cpu && !cpu_cores.empty()) {
            std::printf("CPU      ");
            for (std::size_t i = 0; i < cpu_cores.size(); ++i) {
                const int core = cpu_cores[i];
                double usage = 0.0;
                if (!prev_cpu.empty() &&
                    core >= 0 &&
                    core < static_cast<int>(prev_cpu.size()) &&
                    core < static_cast<int>(cur_cpu.size())) {
                    usage = cpu_usage(prev_cpu[core], cur_cpu[core]);
                }
                std::printf("%d=%.0f%%", core, usage);
                if (i + 1 < cpu_cores.size()) {
                    std::printf(" ");
                }
            }
            std::printf("\n");
        }
        if (have_net) {
            std::printf("NET      iface=%s rx=%.2fMB/s tx=%.2fMB/s rx_drop=%" PRIu64 " tx_drop=%" PRIu64 "\n",
                        net_iface.c_str(), net_rx_mb, net_tx_mb, net_rx_drop_delta, net_tx_drop_delta);
        }
        if (have_blk) {
            std::printf("BLOCK    dev=%s read=%.2fMB/s write=%.2fMB/s util=%.0f%% inflight=%" PRIu64 "\n",
                        block_dev.c_str(), blk_read_mb, blk_write_mb, blk_util_pct, cur_blk.in_flight);
        }
        std::printf("------------------------------------------------------------\n");
        std::printf("Per-slot:\n");
        for (std::size_t s = 0; s < NUM_SLOTS; ++s) {
            const std::uint64_t s_req = load_u64(&stats->per_slot_req[s]);
            const std::uint64_t s_resp = load_u64(&stats->per_slot_resp[s]);
            const std::uint64_t s_in = load_u64(&stats->per_slot_bytes_in[s]);
            const std::uint64_t s_out = load_u64(&stats->per_slot_bytes_out[s]);
            const std::uint64_t s_counter_outstanding = (s_req >= s_resp) ? (s_req - s_resp) : 0;
            const std::uint64_t s_outstanding =
                ipc_snapshot.valid ? ipc_snapshot.per_slot[s].outstanding() : s_counter_outstanding;
            const double s_resp_rate =
                static_cast<double>(cur_slot_resp[s] - prev_slot_resp[s]) * 1000.0 / interval_ms;
            if (s_req == 0 && s_resp == 0 && s_outstanding == 0) {
                continue;
            }
            if (ipc_snapshot.valid) {
                const auto& slot_ipc = ipc_snapshot.per_slot[s];
                std::printf("  slot %2zu: req=%" PRIu64 " resp=%" PRIu64 " outstd=%" PRIu64
                            " resp/s=%.1f ipc[w/r/p/d]=%u/%u/%u/%u in=%.2fMB out=%.2fMB\n",
                            s, s_req, s_resp, s_outstanding, s_resp_rate,
                            slot_ipc.writing, slot_ipc.ready, slot_ipc.processing, slot_ipc.done,
                            bytes_to_mb(s_in), bytes_to_mb(s_out));
            } else {
                std::printf("  slot %2zu: req=%" PRIu64 " resp=%" PRIu64 " outstd=%" PRIu64
                            " resp/s=%.1f in=%.2fMB out=%.2fMB\n",
                            s, s_req, s_resp, s_outstanding, s_resp_rate,
                            bytes_to_mb(s_in), bytes_to_mb(s_out));
            }
        }
        std::fflush(stdout);

        if (csv.is_open()) {
            csv << last_ts
                << "," << req_total
                << "," << resp_total
                << "," << outstanding
                << "," << last_latency
                << "," << std::fixed << std::setprecision(3) << delta_req
                << "," << delta_resp
                << "," << delta_in_mb
                << "," << delta_out_mb
                << "," << fairness_resp
                << "," << ipc_snapshot.outstanding
                << "," << ipc_snapshot.writing
                << "," << ipc_snapshot.ready
                << "," << ipc_snapshot.processing
                << "," << ipc_snapshot.done
                << "," << cpu_busy_all
                << "," << net_rx_mb
                << "," << net_tx_mb
                << "," << net_rx_drop_delta
                << "," << net_tx_drop_delta
                << "," << blk_read_mb
                << "," << blk_write_mb
                << "," << blk_util_pct
                << "," << cur_blk.in_flight
                << "\n";
            csv.flush();
        }

        prev_req = req_total;
        prev_resp = resp_total;
        prev_in = bytes_in;
        prev_out = bytes_out;
        if (have_cpu) {
            prev_cpu = cur_cpu;
        }
        if (have_net) {
            prev_net = cur_net;
            have_prev_net = true;
        }
        if (have_blk) {
            prev_blk = cur_blk;
            have_prev_blk = true;
        }
        prev_slot_resp = cur_slot_resp;

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    return 0;
}
