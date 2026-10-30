#include <bs/driver.h>
#include <bs/loader.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits.h>
#include <string>
#include <string_view>
#include <unistd.h>

static void usage(const char* program) {
    std::printf(
        "Usage: %s [--module FILE | --dir DIRECTORY] [--params STRING]\n"
        "       %s --status\n"
        "       %s --touch-v2-test [--slot N] [--x N] [--y N]\n"
        "       %s --mem-bench PID ADDR SIZE ITERS\n"
        "       %s --help\n\n"
        "With no path, selects a matching .ko from ../modules beside this binary.\n"
        "--status only checks the existing BS interface; it never loads a module.\n"
        "Exit codes: 0 = ready/help, 1 = load/connection failure, 2 = usage error.\n\n"
        "Device example:\n"
        "  adb shell su -c '/data/local/tmp/bskernel/bin/bs_loader'\n"
        "  adb shell su -c '/data/local/tmp/bskernel/bin/bs_loader --module /data/local/tmp/bs.ko'\n",
        program, program, program, program, program);
}

static int run_touch_v2_test(int slot, int x, int y) {
    if (!bs::init()) {
        std::fputs("BS interface is unavailable; load the module first.\n", stderr);
        return 1;
    }
    std::puts("BS interface ready.");
    if (!bs::touch_v2_init()) {
        std::printf("touch_v2_init failed (ioctl=%ld)\n", bs::last_ioctl_result());
        bs::close();
        return 1;
    }
    std::puts("touch_v2_init ok (deferred workqueue delivery armed).");
    const bool down = bs::touch_v2_event(slot, x, y, true);
    std::printf("touch_v2_event(down slot=%d x=%d y=%d) = %s\n",
                slot, x, y, down ? "ok" : "FAILED");
    usleep(150000);
    const bool up = bs::touch_v2_event(slot, x, y, false);
    std::printf("touch_v2_event(up slot=%d) = %s\n", slot, up ? "ok" : "FAILED");
    bs::close();
    if (!down || !up) {
        std::fputs("touch-v2 ioctl failed; check: dmesg | grep touch-v2\n", stderr);
        return 1;
    }
    std::puts("");
    std::puts("Events queued in the kernel pool.");
    std::puts("Now tap the touchscreen once: the panel frame emits SYN_REPORT, the");
    std::puts("kprobe queues the batch and the workqueue injects it. Verify with:");
    std::puts("  su -c \"dmesg | grep -E 'touch-v2.*injected' | tail -5\"");
    std::puts("Expected: 'injected N events (batch 1, dev=<touchscreen>)'.");
    return 0;
}

static int run_mem_bench(int pid, uint64_t addr, size_t size, int iters)
{
    if (size == 0 || size > (4u * 1024u * 1024u) || iters <= 0) {
        std::fputs("usage: --mem-bench PID ADDR SIZE ITERS (size <= 4 MiB)\n", stderr);
        return 2;
    }
    if (!bs::init()) {
        std::fputs("BS interface is unavailable; load the module first.\n", stderr);
        return 1;
    }
    std::vector<char> buffer(size);
    /* One warm-up call: faults the caller buffer and the NC linearmap. */
    if (!bs::read_memory(pid, addr, buffer.data(), size)) {
        std::printf("read_memory(pid=%d, addr=%#llx, size=%zu) failed (ioctl=%ld)\n",
                    pid, (unsigned long long)addr, size, bs::last_ioctl_result());
        bs::close();
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        if (!bs::read_memory(pid, addr, buffer.data(), size)) {
            std::printf("read_memory failed at iteration %d (ioctl=%ld)\n",
                        i, bs::last_ioctl_result());
            bs::close();
            return 1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const double total_us = std::chrono::duration<double, std::micro>(end - start).count();
    const double per_call_us = total_us / iters;
    const double mbps = (double(size) * (double)iters) / total_us;
    std::printf("mem_read bench: pid=%d addr=%#llx size=%zu iters=%d\n",
                pid, (unsigned long long)addr, size, iters);
    std::printf("  per call: %.2f us   throughput: %.1f MB/s\n", per_call_us, mbps);
    std::printf("  total: %.1f ms\n", total_us / 1000.0);
    bs::close();
    return 0;
}

int main(int argc, char** argv) {
    const char* module = nullptr;
    const char* directory = nullptr;
    std::string_view params;
    bool status_only = false;
    bool params_given = false;
    bool touch_v2_test = false;
    bool mem_bench = false;
    int touch_slot = 0;
    int touch_x = 320;
    int touch_y = 480;
    int bench_pid = 0;
    uint64_t bench_addr = 0;
    size_t bench_size = 0;
    int bench_iters = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--help" || option == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (option == "--status") {
            status_only = true;
        } else if (option == "--touch-v2-test") {
            touch_v2_test = true;
        } else if (option == "--mem-bench") {
            if (i + 4 >= argc) {
                std::fputs("Missing arguments for --mem-bench\n", stderr);
                return 2;
            }
            mem_bench = true;
            bench_pid = int(std::strtol(argv[++i], nullptr, 0));
            bench_addr = std::strtoull(argv[++i], nullptr, 0);
            bench_size = size_t(std::strtoull(argv[++i], nullptr, 0));
            bench_iters = int(std::strtol(argv[++i], nullptr, 0));
        } else if (option == "--slot" || option == "--x" || option == "--y") {
            if (++i == argc) {
                std::fprintf(stderr, "Missing value for %.*s\n", int(option.size()), option.data());
                return 2;
            }
            const long value = std::strtol(argv[i], nullptr, 0);
            if (option == "--slot") touch_slot = int(value);
            if (option == "--x") touch_x = int(value);
            if (option == "--y") touch_y = int(value);
        } else if (option == "--module" || option == "--dir" || option == "--params") {
            if (++i == argc || (option != "--params" && !*argv[i])) {
                std::fprintf(stderr, "Missing value for %.*s\n", int(option.size()), option.data());
                return 2;
            }
            if (option == "--module") module = argv[i];
            if (option == "--dir") directory = argv[i];
            if (option == "--params") { params = argv[i]; params_given = true; }
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if ((module && directory) ||
        (status_only && (module || directory || params_given || touch_v2_test || mem_bench)) ||
        (touch_v2_test && (module || directory || params_given || status_only || mem_bench)) ||
        (mem_bench && (module || directory || params_given || status_only || touch_v2_test))) {
        std::fputs("Choose one of --module, --dir, --status, --touch-v2-test or --mem-bench.\n", stderr);
        return 2;
    }
    if (mem_bench) return run_mem_bench(bench_pid, bench_addr, bench_size, bench_iters);
    if (touch_v2_test) return run_touch_v2_test(touch_slot, touch_x, touch_y);

    if (bs::init()) {
        std::puts("Kernel interface is ready (module already loaded).");
        bs::close();
        return 0;
    }
    if (status_only) {
        std::fputs("BS interface is unavailable.\n", stderr);
        return 1;
    }

    std::string default_directory;
    if (!module && !directory) {
        char executable[PATH_MAX];
        const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable));
        if (length <= 0 || size_t(length) == sizeof(executable)) {
            std::fputs("Cannot locate this executable; specify --module or --dir.\n", stderr);
            return 1;
        }
        const std::string path(executable, size_t(length));
        const auto slash = path.rfind('/');
        if (slash == std::string::npos) return 1;
        default_directory = path.substr(0, slash) + "/../modules";
        directory = default_directory.c_str();
    }
    const bool ready = module ? loader::load_module_file(module, params)
                              : loader::load_module_from_directory(directory, params);
    bs::close();
    if (!ready) {
        std::fputs("BS load failed; see the error above or logcat -s bskernel.\n", stderr);
        return 1;
    }
    std::puts("BS module loaded; subsequent applications should call bs::init().");
    return 0;
}
