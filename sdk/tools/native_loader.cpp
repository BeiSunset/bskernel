/*
 * BSLoADERDriver - self-contained native loader for the bs kernel module.
 *
 * The per-KMI module binaries are embedded at build time (bs_native_modules.h)
 * as zlib-compressed, XOR-obfuscated blobs. They are decoded and verified in
 * memory and loaded through memfd_create() + finit_module(), so no .ko file is
 * ever written to the filesystem and no shell/insmod is involved.
 *
 * A missing embedded table is a hard build error: this tool deliberately has
 * no on-disk module fallback.
 */
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <zlib.h>

#if defined(BSKERNEL_EMBEDDED_MODULES)
#include "bs_native_modules.h"
#else
#error "bs_native_modules.h is required"
#endif

/* arm64 (asm-generic) syscall numbers; the macros exist on current NDKs. */
#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif
#ifndef __NR_finit_module
#define __NR_finit_module 273
#endif
#ifndef __NR_delete_module
#define __NR_delete_module 106
#endif

/* <linux/memfd.h> is not part of the allowed header set. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* asm-generic/fcntl.h: O_NONBLOCK = 00004000 == 0x800 on arm64/x86_64. */
static constexpr long kO_NONBLOCK = 0x800;

static constexpr const char* kVersion = "1.0.0";
/* A corrupt/forged embedded table must never trigger a huge allocation. */
static constexpr unsigned int kMaxRawModuleBytes = 256u * 1024u * 1024u;

static constexpr int kExitOk = 0;
static constexpr int kExitFailure = 1;
static constexpr int kExitUsage = 2;
static constexpr int kExitNotRoot = 3;

struct KmiTarget {
    const char* kmi;
    const char* series;
    int major;
    int minor;
};

static const KmiTarget kKmiTargets[] = {
    {"android12-5.10", "5.10", 5, 10},
    {"android13-5.15", "5.15", 5, 15},
    {"android14-6.1", "6.1", 6, 1},
    {"android15-6.6", "6.6", 6, 6},
    {"android16-6.12", "6.12", 6, 12},
    {"android17-6.18", "6.18", 6, 18},
};
static constexpr size_t kKmiTargetCount =
    sizeof(kKmiTargets) / sizeof(kKmiTargets[0]);
static_assert(kKmiTargetCount == 6, "exactly six KMI targets are supported");

static bool g_quiet = false;

/* ---- SHA-256 (FIPS 180-4, public-domain style, no third-party code) ---- */

static const uint32_t kSha256Initial[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

static const uint32_t kSha256Round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

struct Sha256State {
    uint32_t hash[8];
    uint64_t bit_count;
    unsigned char buffer[64];
    size_t buffer_len;
};

static uint32_t rotate_right(uint32_t value, unsigned int bits) {
    return (value >> bits) | (value << (32u - bits));
}

static void sha256_compress(Sha256State* state, const unsigned char* block) {
    uint32_t words[64];
    for (int i = 0; i < 16; ++i) {
        words[i] = (uint32_t(block[4 * i]) << 24) |
                   (uint32_t(block[4 * i + 1]) << 16) |
                   (uint32_t(block[4 * i + 2]) << 8) |
                   uint32_t(block[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotate_right(words[i - 15], 7) ^
                            rotate_right(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
        const uint32_t s1 = rotate_right(words[i - 2], 17) ^
                            rotate_right(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state->hash[0];
    uint32_t b = state->hash[1];
    uint32_t c = state->hash[2];
    uint32_t d = state->hash[3];
    uint32_t e = state->hash[4];
    uint32_t f = state->hash[5];
    uint32_t g = state->hash[6];
    uint32_t h = state->hash[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                            rotate_right(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + ch + kSha256Round[i] + words[i];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                            rotate_right(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state->hash[0] += a;
    state->hash[1] += b;
    state->hash[2] += c;
    state->hash[3] += d;
    state->hash[4] += e;
    state->hash[5] += f;
    state->hash[6] += g;
    state->hash[7] += h;
}

static void sha256_init(Sha256State* state) {
    for (int i = 0; i < 8; ++i)
        state->hash[i] = kSha256Initial[i];
    state->bit_count = 0;
    state->buffer_len = 0;
    std::memset(state->buffer, 0, sizeof(state->buffer));
}

static void sha256_update(Sha256State* state, const unsigned char* data,
                          size_t size) {
    for (size_t i = 0; i < size; ++i) {
        state->buffer[state->buffer_len++] = data[i];
        if (state->buffer_len == sizeof(state->buffer)) {
            sha256_compress(state, state->buffer);
            state->bit_count += 512;
            state->buffer_len = 0;
        }
    }
}

static void sha256_final(Sha256State* state, unsigned char digest[32]) {
    size_t used = state->buffer_len;
    if (used < 56) {
        state->buffer[used++] = 0x80;
        while (used < 56)
            state->buffer[used++] = 0;
    } else {
        state->buffer[used++] = 0x80;
        while (used < 64)
            state->buffer[used++] = 0;
        sha256_compress(state, state->buffer);
        std::memset(state->buffer, 0, 56);
    }

    state->bit_count += uint64_t(state->buffer_len) * 8u;
    for (int i = 0; i < 8; ++i)
        state->buffer[63 - i] =
            static_cast<unsigned char>(state->bit_count >> (8 * i));
    sha256_compress(state, state->buffer);

    for (int i = 0; i < 8; ++i) {
        digest[4 * i] = static_cast<unsigned char>(state->hash[i] >> 24);
        digest[4 * i + 1] = static_cast<unsigned char>(state->hash[i] >> 16);
        digest[4 * i + 2] = static_cast<unsigned char>(state->hash[i] >> 8);
        digest[4 * i + 3] = static_cast<unsigned char>(state->hash[i]);
    }
}

static void sha256_digest(const unsigned char* data, size_t size,
                          unsigned char digest[32]) {
    Sha256State state;
    sha256_init(&state);
    sha256_update(&state, data, size);
    sha256_final(&state, digest);
}

/* ---- embedded module helpers ---- */

static const embedded_module* find_module(const char* kmi) {
    if (!kmi || !*kmi)
        return nullptr;
    for (unsigned int i = 0; i < embedded_module_count; ++i) {
        const embedded_module& entry = embedded_modules[i];
        if (entry.kmi && std::strcmp(entry.kmi, kmi) == 0)
            return &entry;
    }
    return nullptr;
}

static bool digest_matches(const unsigned char digest[32],
                           const char* expected_hex) {
    static const char kHex[] = "0123456789abcdef";
    if (!expected_hex || std::strlen(expected_hex) != 64)
        return false;
    for (int i = 0; i < 32; ++i) {
        const unsigned char value = digest[i];
        const char high = kHex[value >> 4];
        const char low = kHex[value & 0x0f];
        char expected_high = expected_hex[2 * i];
        char expected_low = expected_hex[2 * i + 1];
        if (expected_high >= 'A' && expected_high <= 'F')
            expected_high = char(expected_high - 'A' + 'a');
        if (expected_low >= 'A' && expected_low <= 'F')
            expected_low = char(expected_low - 'A' + 'a');
        if (high != expected_high || low != expected_low)
            return false;
    }
    return true;
}

/* XOR-decode, inflate, then hash. Nothing may reach finit_module() unless the
 * decompressed bytes match the table's SHA-256 exactly. */
static bool decode_and_verify(const embedded_module& module,
                              std::vector<unsigned char>& raw) {
    raw.clear();
    if (!module.data || module.size == 0 || module.raw_size == 0 ||
        module.raw_size > kMaxRawModuleBytes || !module.sha256)
        return false;

    std::vector<unsigned char> compressed(module.size);
    for (unsigned int i = 0; i < module.size; ++i)
        compressed[i] =
            static_cast<unsigned char>(module.data[i] ^ module.xor_key);

    raw.resize(module.raw_size);
    uLongf produced = static_cast<uLongf>(module.raw_size);
    const int result = uncompress(raw.data(), &produced, compressed.data(),
                                  static_cast<uLong>(compressed.size()));
    if (result != Z_OK || produced != static_cast<uLongf>(module.raw_size)) {
        raw.clear();
        return false;
    }

    unsigned char digest[32];
    sha256_digest(raw.data(), raw.size(), digest);
    if (!digest_matches(digest, module.sha256)) {
        raw.clear();
        return false;
    }
    return true;
}

/* ---- module state / KMI detection ---- */

static const char* errno_name(int error) {
    switch (error) {
    case EPERM: return "-EPERM";
    case ENOENT: return "-ENOENT";
    case EEXIST: return "-EEXIST";
    case EINVAL: return "-EINVAL";
    case EACCES: return "-EACCES";
    case EBADF: return "-EBADF";
    case EBUSY: return "-EBUSY";
    case EAGAIN: return "-EAGAIN";
    case ENOMEM: return "-ENOMEM";
    case ENOSYS: return "-ENOSYS";
    case ENOEXEC: return "-ENOEXEC";
    case EOVERFLOW: return "-EOVERFLOW";
    case ENODEV: return "-ENODEV";
#ifdef EKEYREJECTED
    case EKEYREJECTED: return "-EKEYREJECTED";
#endif
#ifdef EHWPOISON
    case EHWPOISON: return "-EHWPOISON";
#endif
    default: return "-errno";
    }
}

static bool is_module_loaded() {
    std::FILE* file = std::fopen("/proc/modules", "r");
    if (!file)
        return false;
    char line[256];
    bool found = false;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "bs ", 3) == 0) {
            found = true;
            break;
        }
    }
    std::fclose(file);
    return found;
}

static int detect_kmi_index(std::string* release) {
    struct utsname info;
    if (uname(&info) != 0) {
        if (release)
            release->clear();
        return -1;
    }
    if (release)
        release->assign(info.release);

    int major = 0;
    int minor = 0;
    if (std::sscanf(info.release, "%d.%d", &major, &minor) != 2)
        return -1;
    for (size_t i = 0; i < kKmiTargetCount; ++i) {
        if (kKmiTargets[i].major == major && kKmiTargets[i].minor == minor)
            return int(i);
    }
    return -1;
}

static void print_supported(std::FILE* stream) {
    std::fprintf(stream, "supported KMI:");
    for (size_t i = 0; i < kKmiTargetCount; ++i)
        std::fprintf(stream, " %s", kKmiTargets[i].kmi);
    std::fputc('\n', stream);
}

/* ---- load / unload ---- */

static bool write_all(int fd, const unsigned char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (result == 0)
            return false;
        written += size_t(result);
    }
    return true;
}

/* Returns 0 on success, or a positive errno describing the first failure. */
static int load_module_bytes(const unsigned char* data, size_t size) {
    const long raw_fd = syscall(__NR_memfd_create, "",
                                static_cast<long>(MFD_CLOEXEC));
    if (raw_fd < 0)
        return errno;
    const int fd = static_cast<int>(raw_fd);

    int error = 0;
    if (!write_all(fd, data, size)) {
        error = errno != 0 ? errno : EIO;
    } else if (::lseek(fd, 0, SEEK_SET) < 0) {
        /* Older kernels read the module from the current file offset. */
        error = errno;
    } else if (syscall(__NR_finit_module, static_cast<long>(fd), "", 0L) != 0) {
        error = errno;
    }

    (void)::close(fd);
    return error;
}

static int load_kmi(const char* kmi) {
    const embedded_module* entry = find_module(kmi);
    if (!entry) {
        std::fprintf(stderr, "no embedded module for %s\n", kmi);
        print_supported(stderr);
        return kExitFailure;
    }
    if (is_module_loaded()) {
        std::fputs("module 'bs' is already loaded; run --unload first\n",
                   stderr);
        return kExitFailure;
    }

    std::vector<unsigned char> ko;
    if (!decode_and_verify(*entry, ko)) {
        std::fprintf(stderr, "failed: integrity check failed for %s\n",
                     entry->kmi);
        return kExitFailure;
    }

    if (!g_quiet) {
        std::printf("loading %s ... ", entry->kmi);
        std::fflush(stdout);
    }
    const int error = load_module_bytes(ko.data(), ko.size());
    if (error != 0) {
        std::fprintf(stderr, "failed: %s (%d)\n", errno_name(error), error);
        if (error == EEXIST)
            std::fputs("hint: 'bs' is already loaded; use --unload first\n",
                       stderr);
        return kExitFailure;
    }
    if (!g_quiet)
        std::puts("ok");
    return kExitOk;
}

static int unload_module() {
    if (!is_module_loaded()) {
        std::fputs("module 'bs' is not loaded\n", stderr);
        return kExitFailure;
    }
    if (!g_quiet) {
        std::fputs("unloading bs ... ", stdout);
        std::fflush(stdout);
    }
    const long result = syscall(__NR_delete_module, "bs", kO_NONBLOCK);
    if (result != 0) {
        const int error = errno;
        std::fprintf(stderr, "failed: %s (%d)\n", errno_name(error), error);
        return kExitFailure;
    }
    if (!g_quiet)
        std::puts("ok");
    return kExitOk;
}

/* ---- informational / interactive commands ---- */

static void list_modules() {
    std::puts("embedded KMI modules (size = decompressed .ko):");
    for (size_t i = 0; i < kKmiTargetCount; ++i) {
        const embedded_module* entry = find_module(kKmiTargets[i].kmi);
        if (entry) {
            std::printf("  %zu) %s (%s)  %u bytes\n", i + 1,
                        kKmiTargets[i].kmi, kKmiTargets[i].series,
                        entry->raw_size);
        } else {
            std::printf("  %zu) %s (%s)  not embedded\n", i + 1,
                        kKmiTargets[i].kmi, kKmiTargets[i].series);
        }
    }

    std::string release;
    const int detected = detect_kmi_index(&release);
    if (detected >= 0) {
        std::printf("detected kernel: %s -> %s\n", release.c_str(),
                    kKmiTargets[detected].kmi);
    } else if (!release.empty()) {
        std::printf("detected kernel: %s (unsupported)\n", release.c_str());
    }
}

static void print_menu(const std::string& release, int detected) {
    std::printf("BSLoaderDriver %s\n", kVersion);
    if (detected >= 0) {
        std::printf("detected kernel: %s -> %s\n", release.c_str(),
                    kKmiTargets[detected].kmi);
    } else if (!release.empty()) {
        std::printf("detected kernel: %s (unsupported)\n", release.c_str());
    }
    std::puts("");
    for (size_t i = 0; i < kKmiTargetCount; ++i) {
        std::printf("  %zu) %s%s\n", i + 1, kKmiTargets[i].kmi,
                    int(i) == detected ? "  [suggested]" : "");
    }
    std::puts("  7) exit");
}

static int run_menu() {
    std::string release;
    const int detected = detect_kmi_index(&release);
    char line[64];
    for (;;) {
        print_menu(release, detected);
        if (detected >= 0) {
            std::printf("select 1-%zu [%d] (7 = exit): ", kKmiTargetCount + 1,
                        detected + 1);
        } else {
            std::printf("select 1-%zu (7 = exit): ", kKmiTargetCount + 1);
        }
        std::fflush(stdout);

        if (!std::fgets(line, sizeof(line), stdin)) {
            std::fputc('\n', stderr);
            return kExitUsage;
        }
        char* end = nullptr;
        const long choice = std::strtol(line, &end, 10);
        if (end == line) {
            if (detected >= 0)
                return load_kmi(kKmiTargets[detected].kmi);
            std::fputs("enter a number\n", stderr);
            continue;
        }
        if (choice == 7)
            return kExitOk;
        if (choice < 1 || choice > long(kKmiTargetCount)) {
            std::fputs("invalid selection\n", stderr);
            continue;
        }
        return load_kmi(kKmiTargets[size_t(choice - 1)].kmi);
    }
}

static int run_auto() {
    std::string release;
    const int detected = detect_kmi_index(&release);
    if (detected < 0) {
        std::fprintf(stderr, "unsupported kernel release: %s\n",
                     release.empty() ? "unknown" : release.c_str());
        print_supported(stderr);
        return kExitFailure;
    }
    return load_kmi(kKmiTargets[detected].kmi);
}

/* ---- entry point ---- */

static void usage(std::FILE* stream, const char* program) {
    std::fprintf(
        stream,
        "BSLoaderDriver %s - self-contained native loader for the bs module\n"
        "Usage:\n"
        "  %s [--kmi NAME | --auto | --list | --unload] [-q]\n"
        "  %s --help | --version\n"
        "\n"
        "  --kmi NAME   load the embedded module for NAME (e.g. android15-6.6)\n"
        "  --auto       detect the running kernel and load the matching module\n"
        "  --list       list embedded KMI modules and their sizes\n"
        "  --unload     remove an already loaded 'bs' module\n"
        "  -q           quiet: suppress status output\n"
        "\n"
        "No arguments shows a menu on a tty, otherwise behaves like --auto.\n"
        "Exit codes: 0 success, 1 load failure/mismatch, 2 usage error,\n"
        "            3 not root.\n",
        kVersion, program, program);
}

static int require_root() {
    if (geteuid() == 0)
        return kExitOk;
    std::fputs("must run as root\n", stderr);
    return kExitNotRoot;
}

int main(int argc, char** argv) {
    enum class Action { kNone, kList, kLoad, kAuto, kUnload };

    Action action = Action::kNone;
    bool action_set = false;
    std::string kmi;

    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        if (std::strcmp(argument, "--help") == 0 ||
            std::strcmp(argument, "-h") == 0) {
            usage(stdout, argv[0]);
            return kExitOk;
        }
        if (std::strcmp(argument, "--version") == 0) {
            std::printf("BSLoaderDriver %s\n", kVersion);
            return kExitOk;
        }
        if (std::strcmp(argument, "-q") == 0) {
            g_quiet = true;
            continue;
        }
        if (action_set) {
            std::fputs("only one of --kmi, --auto, --list or --unload is "
                       "allowed\n",
                       stderr);
            return kExitUsage;
        }
        if (std::strcmp(argument, "--list") == 0) {
            action = Action::kList;
        } else if (std::strcmp(argument, "--auto") == 0) {
            action = Action::kAuto;
        } else if (std::strcmp(argument, "--unload") == 0) {
            action = Action::kUnload;
        } else if (std::strcmp(argument, "--kmi") == 0) {
            if (i + 1 >= argc) {
                std::fputs("missing value for --kmi\n", stderr);
                return kExitUsage;
            }
            kmi = argv[++i];
            if (kmi.empty()) {
                std::fputs("empty value for --kmi\n", stderr);
                return kExitUsage;
            }
            action = Action::kLoad;
        } else {
            std::fprintf(stderr, "unknown option: %s (try --help)\n", argument);
            return kExitUsage;
        }
        action_set = true;
    }

    if (action == Action::kList) {
        list_modules();
        return kExitOk;
    }

    if (action == Action::kNone) {
        if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            const int root = require_root();
            if (root != kExitOk)
                return root;
            return run_menu();
        }
        action = Action::kAuto;
    }

    const int root = require_root();
    if (root != kExitOk)
        return root;

    if (action == Action::kUnload)
        return unload_module();
    if (action == Action::kLoad)
        return load_kmi(kmi.c_str());
    return run_auto();
}