#include "common.h"

#include <cstdio>
#include <string>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [--exec-mode=inline|thread|process] [--workers=N]\n"
                 "  inline  : one runtime loop polls all slot-local eventfds\n"
                 "  thread  : worker threads poll owned slot-local eventfds\n"
                 "  process : worker processes poll owned slot-local eventfds\n",
                 prog);
}

bool parse_exec_mode_arg(const std::string& arg, MessageQueueDispatcher::Options& options) {
    if (arg == "--exec-mode=inline" || arg == "-exec-mode=inline") {
        options.exec_mode = ExecMode::kInline;
        return true;
    }
    if (arg == "--exec-mode=thread" || arg == "-exec-mode=thread") {
        options.exec_mode = ExecMode::kThread;
        return true;
    }
    if (arg == "--exec-mode=process" || arg == "-exec-mode=process") {
        options.exec_mode = ExecMode::kProcess;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    MessageQueueDispatcher::Options options{};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg.rfind("--workers=", 0) == 0) {
            options.workers = static_cast<std::size_t>(std::stoul(arg.substr(10)));
            continue;
        }
        if (!parse_exec_mode_arg(arg, options)) {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    SharedMemorySlots shm_slots;
    MessageQueueDispatcher dispatcher(shm_slots.CreateWorkers(), options);
    dispatcher.Run();
    return 0;
}
