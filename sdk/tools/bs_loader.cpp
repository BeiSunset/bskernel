#include <bs/driver.h>
#include <bs/loader.h>

#include <algorithm>
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
        "       %s --mem-bench PID|NAME ADDR SIZE ITERS   (ADDR=0 picks a mapping)\n"
        "       %s --mem-bench-batch PID|NAME ADDR SIZE COUNT ITERS\n"
        "       %s --memcpy-bench SIZE ITERS   (userspace memcpy ceiling)\n"
        "       %s --selftest [PID|NAME]   (runtime scenario checks)\n"
        "       %s --help\n\n"
        "With no path, selects a matching .ko from ../modules beside this binary.\n"
        "--status only checks the existing BS interface; it never loads a module.\n"
        "Exit codes: 0 = ready/help, 1 = load/connection failure, 2 = usage error.\n\n"
        "Device example:\n"
        "  adb shell su -c '/data/local/tmp/bskernel/bin/bs_loader'\n"
        "  adb shell su -c '/data/local/tmp/bskernel/bin/bs_loader --module /data/local/tmp/bs.ko'\n",
        program, program, program, program, program, program, program, program);
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

static int resolve_bench_pid(const char* target)
{
    if (!target || !*target)
        return -1;
    bool numeric = true;
    for (const char* p = target; *p; ++p) {
        if (*p < '0' || *p > '9') {
            numeric = false;
            break;
        }
    }
    if (numeric)
        return int(std::strtol(target, nullptr, 10));
    return bs::get_process_id(target);
}

static uint64_t pick_bench_address(int pid, size_t size)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* maps = std::fopen(path, "r");
    if (!maps)
        return 0;
    char line[512];
    unsigned long long best = 0;
    unsigned long long best_len = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3)
            continue;
        if (perms[0] != 'r')
            continue;
        const unsigned long long len = end > start ? end - start : 0;
        if (len < size)
            continue;
        if (len > best_len) {
            best_len = len;
            best = start;
        }
    }
    std::fclose(maps);
    return best;
}

static bool bench_probe(int pid, uint64_t addr, size_t size,
                        std::vector<char>& buffer)
{
    return bs::read_memory(pid, addr, buffer.data(), size);
}

/* Walk every readable VMA from largest to smallest and shrink the request
 * until a block actually reads back, so a sparse or not-yet-faulted mapping
 * does not turn the benchmark into an EFAULT. */
static bool pick_bench_range(int pid, size_t want, std::vector<char>& buffer,
                             uint64_t& out_addr, size_t& out_size)
{
    struct Range {
        uint64_t start;
        uint64_t len;
    };
    std::vector<Range> ranges;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* maps = std::fopen(path, "r");
    if (!maps)
        return false;
    char line[512];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3)
            continue;
        if (perms[0] != 'r' || end <= start)
            continue;
        ranges.push_back({uint64_t(start), uint64_t(end - start)});
    }
    std::fclose(maps);
    std::sort(ranges.begin(), ranges.end(),
              [](const Range& a, const Range& b) { return a.len > b.len; });
    for (const auto& range : ranges) {
        size_t candidate = want;
        if (candidate > range.len)
            candidate = size_t(range.len) & ~size_t(0xfff);
        /* Halving a non power-of-two block size can step straight past 4096,
         * so force a final 4 KiB probe instead of silently skipping the range. */
        while (candidate >= 4096) {
            if (bench_probe(pid, range.start, candidate, buffer)) {
                out_addr = range.start;
                out_size = candidate;
                return true;
            }
            if (candidate == 4096)
                break;
            candidate = (candidate / 2 < 4096) ? 4096 : candidate / 2;
        }
    }
    return false;
}

static int run_mem_bench(const char* target, uint64_t addr, size_t size, int iters)
{
    if (size == 0 || size > (4u * 1024u * 1024u) || iters <= 0) {
        std::fputs("usage: --mem-bench PID|NAME ADDR SIZE ITERS (ADDR=0 auto, size <= 4 MiB)\n", stderr);
        return 2;
    }
    const int pid = resolve_bench_pid(target);
    if (pid <= 0) {
        std::printf("cannot resolve target '%s' (give a PID or an exact process name)\n",
                    target ? target : "");
        return 1;
    }
    if (!bs::init()) {
        std::fputs("BS interface is unavailable; load the module first.\n", stderr);
        return 1;
    }
    std::vector<char> buffer(size);
    size_t bench_size = size;
    if (addr == 0) {
        if (!pick_bench_range(pid, size, buffer, addr, bench_size)) {
            std::printf("no readable block in pid %d can satisfy this request; "
                        "try a smaller SIZE\n", pid);
            bs::close();
            return 1;
        }
        if (bench_size != size)
            std::printf("note: largest verified block in pid %d is %zu bytes "
                        "(requested %zu)\n", pid, bench_size, size);
    } else {
        while (bench_size >= 4096 && !bench_probe(pid, addr, bench_size, buffer))
            bench_size /= 2;
        if (bench_size < 4096) {
            std::printf("address %#llx is not readable in pid %d\n",
                        (unsigned long long)addr, pid);
            bs::close();
            return 1;
        }
    }
    std::printf("target: pid=%d addr=%#llx size=%zu iters=%d\n",
                pid, (unsigned long long)addr, bench_size, iters);
    /* One warm-up call: faults the caller buffer and the NC linearmap. */
    if (!bs::read_memory(pid, addr, buffer.data(), bench_size)) {
        std::printf("read_memory(pid=%d, addr=%#llx, size=%zu) failed (ioctl=%ld)\n",
                    pid, (unsigned long long)addr, bench_size, bs::last_ioctl_result());
        bs::close();
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        if (!bs::read_memory(pid, addr, buffer.data(), bench_size)) {
            std::printf("read_memory failed at iteration %d (ioctl=%ld)\n",
                        i, bs::last_ioctl_result());
            bs::close();
            return 1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const double total_us = std::chrono::duration<double, std::micro>(end - start).count();
    const double per_call_us = total_us / iters;
    const double mbps = (double(bench_size) * (double)iters) / total_us;
    std::printf("mem_read bench: pid=%d addr=%#llx size=%zu iters=%d\n",
                pid, (unsigned long long)addr, bench_size, iters);
    std::printf("  per call: %.2f us   throughput: %.1f MB/s\n", per_call_us, mbps);
    std::printf("  total: %.1f ms\n", total_us / 1000.0);
    bs::close();
    return 0;
}

static int run_memcpy_bench(size_t size, int iters)
{
    if (size == 0 || iters <= 0) {
        std::fputs("usage: --memcpy-bench SIZE ITERS\n", stderr);
        return 2;
    }
    std::vector<char> src(size);
    std::vector<char> dst(size);
    std::memset(src.data(), 0x5a, size);
    std::memcpy(dst.data(), src.data(), size); /* warm up both buffers */
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        std::memcpy(dst.data(), src.data(), size);
    const auto end = std::chrono::steady_clock::now();
    const double total_us = std::chrono::duration<double, std::micro>(end - start).count();
    const double per_call_us = total_us / iters;
    const double mbps = (double(size) * iters) / total_us;
    std::printf("memcpy bench: size=%zu iters=%d\n", size, iters);
    std::printf("  per call: %.2f us   throughput: %.1f MB/s   (userspace ceiling)\n",
                per_call_us, mbps);
    return 0;
}

static int run_mem_batch_bench(const char* target, uint64_t addr, size_t size,
                               int count, int iters)
{
    if (size == 0 || count < 1 || count > int(MEM_BATCH_MAX_SEGS) || iters <= 0 ||
        size * size_t(count) > MEM_BATCH_MAX_BYTES) {
        std::fputs("usage: --mem-bench-batch PID|NAME ADDR SIZE COUNT ITERS "
                   "(COUNT <= 64, SIZE*COUNT <= 4 MiB)\n", stderr);
        return 2;
    }
    const int pid = resolve_bench_pid(target);
    if (pid <= 0) {
        std::printf("cannot resolve target '%s'\n", target ? target : "");
        return 1;
    }
    if (!bs::init()) {
        std::fputs("BS interface is unavailable; load the module first.\n", stderr);
        return 1;
    }
    std::vector<char> buffer(size);
    size_t bench_size = size;
    if (addr == 0) {
        if (!pick_bench_range(pid, size, buffer, addr, bench_size)) {
            std::printf("no readable block in pid %d can satisfy this request\n", pid);
            bs::close();
            return 1;
        }
        if (bench_size != size) {
            std::printf("note: largest verified block is %zu bytes (requested %zu), "
                        "using it\n", bench_size, size);
            while (count > 1 && size_t(count) * bench_size > MEM_BATCH_MAX_BYTES)
                --count;
        }
    }
    std::vector<bs::MemBatchSegment> segments(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        segments[size_t(i)].addr = addr;
        segments[size_t(i)].out = buffer.data();
        segments[size_t(i)].len = uint32_t(bench_size);
    }
    long result = bs::read_memory_batch(pid, segments.data(), uint32_t(count));
    if (result != 0) {
        std::printf("read_memory_batch failed: %ld\n", result);
        bs::close();
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        if (segments[size_t(i)].err != 0) {
            std::printf("segment %d failed: err=%d\n", i, segments[size_t(i)].err);
            bs::close();
            return 1;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    for (int it = 0; it < iters; ++it) {
        if (bs::read_memory_batch(pid, segments.data(), uint32_t(count)) != 0) {
            std::printf("read_memory_batch failed at iteration %d\n", it);
            bs::close();
            return 1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const double total_us = std::chrono::duration<double, std::micro>(end - start).count();
    const double per_call_us = total_us / iters;
    const double per_segment_us = per_call_us / count;
    const double mbps = (double(bench_size) * double(count) * iters) / total_us;
    std::printf("mem_read_batch bench: pid=%d addr=%#llx seg=%zu count=%d iters=%d\n",
                pid, (unsigned long long)addr, bench_size, count, iters);
    std::printf("  per ioctl: %.2f us   per segment: %.2f us   throughput: %.1f MB/s\n",
                per_call_us, per_segment_us, mbps);
    std::printf("  syscalls used: %d (single-read equivalent: %d)\n",
                iters, iters * count);
    bs::close();
    return 0;
}

static int run_selftest(const char* target)
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };

    /* 1. init/close cycles must not leak or crash. */
    for (int i = 0; i < 3; ++i) {
        check(bs::init(), "bs::init() cycle");
        bs::close();
    }
    check(bs::init(), "bs::init() for memory scenarios");

    const int self = int(getpid());
    std::vector<unsigned char> pattern(4096);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = (unsigned char)(i * 131u + 7u);

    /* 2. single read of our own buffer must round-trip. */
    {
        std::vector<unsigned char> out(pattern.size(), 0);
        const bool ok = bs::read_memory(self, reinterpret_cast<uint64_t>(pattern.data()),
                                        out.data(), out.size());
        check(ok && out == pattern, "single read round-trip (self)");
    }

    /* 3. batch read with 1/8/64 segments, bytes checked per segment. */
    for (int count : {1, 8, 64}) {
        std::vector<bs::MemBatchSegment> segs(static_cast<size_t>(count));
        std::vector<std::vector<unsigned char>> bufs(size_t(count),
            std::vector<unsigned char>(64, 0));
        for (int i = 0; i < count; ++i) {
            segs[size_t(i)].addr = reinterpret_cast<uint64_t>(pattern.data()) + size_t(i) * 64u;
            segs[size_t(i)].out = bufs[size_t(i)].data();
            segs[size_t(i)].len = 64;
        }
        long rc = bs::read_memory_batch(self, segs.data(), uint32_t(count));
        bool ok = rc == 0;
        if (ok) {
            for (int i = 0; i < count; ++i)
                ok = ok && segs[size_t(i)].err == 0 && segs[size_t(i)].out_len == 64;
        }
        if (ok) {
            for (int i = 0; i < count; ++i)
                ok = ok && bufs[size_t(i)][0] == pattern[size_t(i) * 64u];
        }
        char label[64];
        std::snprintf(label, sizeof(label), "batch read of %d segments", count);
        check(ok, label);
    }

    /* 4. one bad segment must not abort the batch. */
    {
        std::vector<bs::MemBatchSegment> segs(2);
        std::vector<unsigned char> good(64, 0);
        std::vector<unsigned char> bad(64, 0);
        segs[0].addr = 0xdead0000ULL;
        segs[0].out = bad.data();
        segs[0].len = 64;
        segs[1].addr = reinterpret_cast<uint64_t>(pattern.data());
        segs[1].out = good.data();
        segs[1].len = 64;
        const long rc = bs::read_memory_batch(self, segs.data(), 2);
        check(rc == 0 && segs[0].err != 0 && segs[1].err == 0 && segs[1].out_len == 64,
              "bad segment isolated from good segment");
    }

    /* 5. argument validation must fail without touching the kernel. */
    check(bs::read_memory_batch(self, nullptr, 1) == -EINVAL, "rejects null segments");
    {
        std::vector<bs::MemBatchSegment> segs(1);
        check(bs::read_memory_batch(self, segs.data(), 0) == -EINVAL, "rejects count=0");
        check(bs::read_memory_batch(self, segs.data(), 65) == -EINVAL, "rejects count>64");
        segs[0].addr = reinterpret_cast<uint64_t>(pattern.data());
        segs[0].out = pattern.data();
        segs[0].len = (4u * 1024u * 1024u) + 4096u;
        check(bs::read_memory_batch(self, segs.data(), 1) == -EINVAL, "rejects >4 MiB batch");
    }

    /* 6. repeated batch reads must stay stable. */
    {
        std::vector<bs::MemBatchSegment> segs(8);
        std::vector<std::vector<unsigned char>> bufs(8, std::vector<unsigned char>(256, 0));
        for (int i = 0; i < 8; ++i) {
            segs[size_t(i)].addr = reinterpret_cast<uint64_t>(pattern.data()) + size_t(i) * 256u;
            segs[size_t(i)].out = bufs[size_t(i)].data();
            segs[size_t(i)].len = 256;
        }
        bool ok = true;
        for (int it = 0; it < 2000 && ok; ++it)
            ok = bs::read_memory_batch(self, segs.data(), 8) == 0;
        check(ok, "2000 repeated batch reads");
    }

    /* 7. touch-v2 ioctl path must initialise (injection needs a real frame). */
    check(bs::touch_v2_init(), "touch_v2_init");

    /* 8. optional real target process: single + batch read of a mapping. */
    if (target && *target) {
        const int pid = resolve_bench_pid(target);
        check(pid > 0, "resolve target pid");
        if (pid > 0) {
            std::vector<char> buffer(4096);
            uint64_t addr = 0;
            size_t size = 0;
            const bool found = pick_bench_range(pid, 4096, buffer, addr, size);
            check(found, "find readable target mapping");
            if (found) {
                check(bs::read_memory(pid, addr, buffer.data(), size),
                      "single read of target memory");
                std::vector<unsigned char> out(64, 0);
                std::vector<bs::MemBatchSegment> segs(1);
                segs[0].addr = addr;
                segs[0].out = out.data();
                segs[0].len = 64;
                const long rc = bs::read_memory_batch(pid, segs.data(), 1);
                check(rc == 0 && segs[0].err == 0 && segs[0].out_len == 64,
                      "batch read of target memory");
            }
        }
    }

    bs::close();
    std::printf("selftest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* module = nullptr;
    const char* directory = nullptr;
    std::string_view params;
    bool status_only = false;
    bool params_given = false;
    bool touch_v2_test = false;
    bool mem_bench = false;
    bool mem_batch_bench = false;
    bool memcpy_bench = false;
    bool selftest = false;
    const char* selftest_target = nullptr;
    int touch_slot = 0;
    int touch_x = 320;
    int touch_y = 480;
    const char* bench_target = nullptr;
    uint64_t bench_addr = 0;
    size_t bench_size = 0;
    int bench_iters = 0;
    int bench_count = 0;
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
            bench_target = argv[++i];
            bench_addr = std::strtoull(argv[++i], nullptr, 0);
            bench_size = size_t(std::strtoull(argv[++i], nullptr, 0));
            bench_iters = int(std::strtol(argv[++i], nullptr, 0));
        } else if (option == "--mem-bench-batch") {
            if (i + 5 >= argc) {
                std::fputs("Missing arguments for --mem-bench-batch\n", stderr);
                return 2;
            }
            mem_batch_bench = true;
            bench_target = argv[++i];
            bench_addr = std::strtoull(argv[++i], nullptr, 0);
            bench_size = size_t(std::strtoull(argv[++i], nullptr, 0));
            bench_count = int(std::strtol(argv[++i], nullptr, 0));
            bench_iters = int(std::strtol(argv[++i], nullptr, 0));
        } else if (option == "--selftest") {
            selftest = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                selftest_target = argv[++i];
        } else if (option == "--memcpy-bench") {
            if (i + 2 >= argc) {
                std::fputs("Missing arguments for --memcpy-bench\n", stderr);
                return 2;
            }
            memcpy_bench = true;
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
        (mem_bench && (module || directory || params_given || status_only || touch_v2_test)) ||
        (mem_batch_bench && (module || directory || params_given || status_only || touch_v2_test ||
                             mem_bench)) ||
        (memcpy_bench && (module || directory || params_given || status_only ||
                          touch_v2_test || mem_bench || mem_batch_bench)) ||
        (selftest && (module || directory || params_given || status_only ||
                      touch_v2_test || mem_bench || mem_batch_bench ||
                      memcpy_bench))) {
        std::fputs("Choose one of --module, --dir, --status, --touch-v2-test, --mem-bench or --mem-bench-batch.\n", stderr);
        return 2;
    }
    if (selftest) return run_selftest(selftest_target);
    if (memcpy_bench) return run_memcpy_bench(bench_size, bench_iters);
    if (mem_batch_bench)
        return run_mem_batch_bench(bench_target, bench_addr, bench_size, bench_count, bench_iters);
    if (mem_bench) return run_mem_bench(bench_target, bench_addr, bench_size, bench_iters);
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
