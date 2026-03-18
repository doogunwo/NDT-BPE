#include "common.h"

#include <cstdio>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [--mode=txt|arrow]\n"
                 "  txt   : payload is UTF-8 text\n"
                 "  arrow : payload is Arrow text-buffer chunk, not full Arrow IPC/file\n",
                 prog);
}

bool parse_mode_arg(const std::string& arg, InputMode& mode) {
    if (arg == "--mode=txt" || arg == "-mode=txt") {
        mode = InputMode::kText;
        return true;
    }
    if (arg == "--mode=arrow" || arg == "-mode=arrow") {
        mode = InputMode::kArrow;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    InputMode mode = InputMode::kText;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (!parse_mode_arg(arg, mode)) {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    const int msg_id = MessageQueueDispatcher::OpenQueue(MSG_KEY, IPC_CREAT | 0660);
    if (msg_id < 0) {
        std::perror("[BPE] msgget");
        return 1;
    }

    SharedMemorySlots shm_slots;
    MessageQueueDispatcher dispatcher(msg_id, shm_slots.CreateWorkers(mode));
    dispatcher.Run();
    return 0;
}

// main() 진입점
// common.h -> 공유 메모리 읽기 쓰기, 메시지 큐 디스패처
