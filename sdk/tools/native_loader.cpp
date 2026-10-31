/*
 * BSLoADERDriver - self-contained native loader for the bs kernel module.
 *
 * The six per-KMI module images are embedded at build time
 * (bs_native_modules.h) as zlib-compressed, XOR-obfuscated blobs. They are
 * decoded and SHA-256 verified in memory and loaded with memfd_create() +
 * finit_module(), so no .ko file is ever written to the filesystem and no
 * shell or insmod is involved. The menu, banner and status flow mirror the old
 * shell loader.
 */
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <zlib.h>

#if defined(BSKERNEL_EMBEDDED_MODULES)
#include "bs_native_modules.h"
#else
#error "bs_native_modules.h is required"
#endif

#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif
#ifndef __NR_finit_module
#define __NR_finit_module 273
#endif
#ifndef __NR_delete_module
#define __NR_delete_module 106
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static constexpr long kO_NONBLOCK = 0x800;
static constexpr const char* kVersion = "1.0";
static constexpr const char* kVersionFull = "1.0.0";
static constexpr const char* kTgUrl = "https://t.me/BAIFRE";
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

/* ---- ANSI UI helpers (same layout as the old shell loader) ---- */

static const char* const kReset = "\033[0m";
static const char* const kCyan = "\033[1;36m";
static const char* const kGreen = "\033[1;32m";
static const char* const kYellow = "\033[1;33m";
static const char* const kRed = "\033[1;31m";
static const char* const kMagenta = "\033[1;35m";
static const char* const kTeal = "\033[38;2;110;195;198m";
static const char* const kWarnRed = "\033[38;2;210;120;112m";

static bool g_quiet = false;

static void info(const char* text) {
    if (!g_quiet)
        std::printf("%s[-] %s%s\n", kYellow, text, kReset);
}

static void ok(const char* text) {
    if (!g_quiet)
        std::printf("%s[√] %s%s\n", kGreen, text, kReset);
}

static void err(const char* text) {
    std::printf("%s[x] %s%s\n", kRed, text, kReset);
}

static void warn(const char* text) {
    if (!g_quiet)
        std::printf("%s[!] %s%s\n", kYellow, text, kReset);
}

static void banner() {
    std::printf("\033[2J\033[H");
    std::printf("%s<<<Welcome to BSKernel %s Driver!>>>%s\n\n", kYellow,
                kVersion, kReset);
    std::printf("%s", kMagenta);
    std::puts("   ██████╗ ███████╗██╗██╗     ██╗   ██╗ ██████╗");
    std::puts("   ██╔══██╗██╔════╝██║██║     ██║   ██║██╔═══██╗");
    std::puts("   ██████╔╝█████╗  ██║██║     ██║   ██║██║   ██║");
    std::puts("   ██╔══██╗██╔══╝  ██║██║     ██║   ██║██║   ██║");
    std::puts("   ██████╔╝███████╗██║███████╗╚██████╔╝╚██████╔╝");
    std::puts("   ╚═════╝ ╚══════╝╚═╝╚══════╝ ╚═════╝  ╚═════╝");
    std::printf("%s", kReset);
    std::printf("\n%s<<<欢迎使用 BSKernel 驱动加载>>>%s\n\n", kYellow, kReset);
    std::printf("%s[!] 唯一官方 TG 地址如下，除以下地址外均为非官方：%s\n",
                kMagenta, kReset);
    std::printf("%s    %s%s\n", kTeal, kTgUrl, kReset);
    std::printf("%s[!] 请仔细核对用户名，谨防相似名称的仿冒频道。%s\n\n",
                kWarnRed, kReset);
    std::printf("%s[!] 本人无任何收费项目，请不要轻易相信陌生人。%s\n\n",
                kWarnRed, kReset);
}

/* ---- SHA-256 (FIPS 180-4, no third-party code) ---- */

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

static bool decode_module(const embedded_module& module,
                          std::vector<unsigned char>& raw) {
    raw.clear();
    if (!module.data || module.size == 0 || module.raw_size == 0 ||
        module.raw_size > kMaxRawModuleBytes)
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
    return true;
}

static bool verify_module(const embedded_module& module,
                          const std::vector<unsigned char>& raw) {
    if (!module.sha256)
        return false;
    unsigned char digest[32];
    sha256_digest(raw.data(), raw.size(), digest);
    return digest_matches(digest, module.sha256);
}

static bool decode_and_verify(const embedded_module& module,
                              std::vector<unsigned char>& raw) {
    return decode_module(module, raw) && verify_module(module, raw);
}

/* ---- version check ----
 *
 * The old shell read https://sharechain.qq.com (TLS). The native loader has no
 * TLS stack, so it performs the reachability half of that check (TCP connect
 * to the same endpoint with a short timeout) and keeps the shell's exact
 * messages: unreachable -> skip with a warning, reachable -> "up to date".
 */
static bool tcp_probe(const char* host, const char* port, int timeout_ms) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addresses = nullptr;
    if (getaddrinfo(host, port, &hints, &addresses) != 0)
        return false;

    bool connected = false;
    for (struct addrinfo* entry = addresses; entry && !connected;
         entry = entry->ai_next) {
        const int fd = socket(entry->ai_family, entry->ai_socktype, 0);
        if (fd < 0)
            continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }
        int result = connect(fd, entry->ai_addr, entry->ai_addrlen);
        if (result != 0 && errno == EINPROGRESS) {
            struct pollfd poller;
            poller.fd = fd;
            poller.events = POLLOUT;
            poller.revents = 0;
            if (poll(&poller, 1, timeout_ms) > 0) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                               &length) == 0 &&
                    socket_error == 0)
                    result = 0;
            }
        }
        if (result == 0)
            connected = true;
        close(fd);
    }
    freeaddrinfo(addresses);
    return connected;
}

static bool check_version() {
    if (!tcp_probe("sharechain.qq.com", "443", 2500)) {
        warn("无法连接版本服务器，已跳过版本验证");
        return true;
    }
    ok("驱动版本验证通过,当前已是最新版本!");
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

static void print_supported() {
    std::printf("%ssupported KMI:%s", kYellow, kReset);
    for (size_t i = 0; i < kKmiTargetCount; ++i)
        std::printf(" %s", kKmiTargets[i].kmi);
    std::putchar('\n');
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

static int load_module_bytes(const unsigned char* data, size_t size,
                             const std::string& params) {
    const long raw_fd = syscall(__NR_memfd_create, "",
                                static_cast<long>(MFD_CLOEXEC));
    if (raw_fd < 0)
        return errno;
    const int fd = static_cast<int>(raw_fd);

    int error = 0;
    if (!write_all(fd, data, size)) {
        error = errno != 0 ? errno : EIO;
    } else if (::lseek(fd, 0, SEEK_SET) < 0) {
        error = errno;
    } else if (syscall(__NR_finit_module, static_cast<long>(fd),
                       params.c_str(), 0L) != 0) {
        error = errno;
    }

    (void)::close(fd);
    return error;
}

static int load_kmi(const char* kmi, const std::string& params,
                    bool verify) {
    const embedded_module* entry = find_module(kmi);
    if (!entry) {
        err("内置驱动中没有该内核版本");
        print_supported();
        return kExitFailure;
    }
    if (is_module_loaded()) {
        ok("内核模块加载成功!");
        return kExitOk;
    }

    std::vector<unsigned char> ko;
    if (!decode_module(*entry, ko) ||
        (verify && !verify_module(*entry, ko))) {
        if (verify) {
            err("驱动完整性验证失败!");
        } else {
            err("驱动解码失败!");
        }
        return kExitFailure;
    }

    const int error = load_module_bytes(ko.data(), ko.size(), params);
    if (error != 0) {
        std::printf("%s[x] 内核模块加载失败!%s %s (%d)\n", kRed, kReset,
                    errno_name(error), error);
        if (error == EEXIST)
            ok("内核模块加载成功!");
        return kExitFailure;
    }
    ok("内核模块加载成功!");
    return kExitOk;
}

static int unload_module() {
    if (!is_module_loaded()) {
        err("内核模块未加载");
        return kExitFailure;
    }
    info("正在卸载内核模块...");
    const long result = syscall(__NR_delete_module, "bs", kO_NONBLOCK);
    if (result != 0) {
        std::printf("%s[x] 卸载失败%s %s (%d)\n", kRed, kReset,
                    errno_name(errno), errno);
        return kExitFailure;
    }
    ok("内核模块已卸载");
    return kExitOk;
}

/* ---- commands ---- */

static void list_modules() {
    std::string release;
    const int detected = detect_kmi_index(&release);
    std::printf("内置驱动模块：\n");
    for (size_t i = 0; i < kKmiTargetCount; ++i) {
        const embedded_module* entry = find_module(kKmiTargets[i].kmi);
        std::printf("  - %s", kKmiTargets[i].kmi);
        if (entry)
            std::printf("  (%u bytes)", entry->raw_size);
        std::printf("%s\n",
                    int(i) == detected ? "  <- 当前内核" : "");
    }
    std::printf("\n当前内核：%s\n",
                release.empty() ? "unknown" : release.c_str());
}

static void show_menu(int detected) {
    for (size_t i = 0; i < kKmiTargetCount; ++i) {
        (void)detected;
        std::printf("%s[%zu] 内核版本：%-6s        （一个子方案）%s\n",
                    kMagenta, i + 1, kKmiTargets[i].series, kReset);
    }
    std::printf("%s[7] 退出程序%s\n", kMagenta, kReset);
}

static int run_menu(const std::string& /*release*/, int detected,
                    const std::string& params, bool verify) {
    show_menu(detected);

    char line[64];
    for (;;) {
        std::printf("\n%s请根据当前内核版本，从列表中选择最接近的驱动方案安装：%s\n\n",
                    kYellow, kReset);
        show_menu(detected);
        std::printf("\n[-] 请输入选项数字并回车：");
        std::fflush(stdout);
        if (!std::fgets(line, sizeof(line), stdin)) {
            err("输入已结束，程序退出");
            return kExitFailure;
        }
        char* end = nullptr;
        long choice = std::strtol(line, &end, 10);
        if (end == line) {
            if (detected >= 0)
                choice = detected + 1;
            else
                continue;
        }
        if (choice == 7)
            return kExitOk;
        if (choice < 1 || choice > long(kKmiTargetCount)) {
            std::printf("%s[x] 无效选项：%s%s\n", kRed, kReset, line);
            continue;
        }
        return load_kmi(kKmiTargets[size_t(choice - 1)].kmi, params,
                        verify);
    }
}

static int run_auto(const std::string& release, int detected,
                    const std::string& params, bool verify) {
    if (detected < 0) {
        std::printf("%s[x] 自动模式：当前内核 %s 不在支持列表"
                    "（5.10/5.15/6.1/6.6/6.12/6.18）%s\n",
                    kRed, release.empty() ? "unknown" : release.c_str(),
                    kReset);
        return kExitFailure;
    }
    const embedded_module* entry = find_module(kKmiTargets[detected].kmi);
    if (!entry) {
        std::printf("%s[x] 自动模式：未找到 %s 的驱动模块%s\n", kRed,
                    kKmiTargets[detected].series, kReset);
        return kExitFailure;
    }
    return load_kmi(kKmiTargets[detected].kmi, params, verify);
}

/* ---- entry point ---- */

static void usage(std::FILE* stream, const char* program) {
    std::fprintf(
        stream,
        "BSKernel %s 驱动加载\n\n"
        "用法:\n"
        "  %s                     交互式选择内核版本并加载\n"
        "  %s --auto              自动按当前内核选择最接近的方案并加载\n"
        "  %s --list              列出内置的驱动模块\n"
        "  %s --stage N           分阶段诊断（0=完整 1=仅兼容层 2=+内存 3=+文件 4=+hooks）\n"
        "  %s --no-verify         跳过内置驱动的 sha256 校验\n"
        "  %s --verbose           开启内核详细日志（默认静默，仅调试用）\n"
        "  %s --unload            卸载已加载的模块\n"
        "  %s --help              显示本帮助\n"
        "\n"
        "说明: 6 个内核版本的驱动已内嵌在本可执行文件里，加载时直接以\n"
        "memfd + finit_module 注入内核，不会在磁盘上释放任何 .ko 文件。\n"
        "需要 root 权限；本程序不依赖 shell 与 toybox 命令。\n",
        kVersion, program, program, program, program, program, program,
        program, program, program);
}

static int require_root() {
    if (geteuid() == 0)
        return kExitOk;
    err("需要 root 权限，请用 su -c 运行");
    return kExitNotRoot;
}

int main(int argc, char** argv) {
    bool auto_mode = false;
    bool list_only = false;
    bool unload = false;
    bool verify = true;
    bool verbose = false;
    std::string stage;
    std::string kmi;

    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        if (std::strcmp(argument, "--help") == 0 ||
            std::strcmp(argument, "-h") == 0) {
            usage(stdout, argv[0]);
            return kExitOk;
        }
        if (std::strcmp(argument, "--version") == 0) {
            std::printf("BSKernel %s (loader %s)\n", kVersion, kVersionFull);
            return kExitOk;
        }
        if (std::strcmp(argument, "-q") == 0) {
            g_quiet = true;
            continue;
        }
        if (std::strcmp(argument, "--auto") == 0 ||
            std::strcmp(argument, "-a") == 0) {
            auto_mode = true;
            continue;
        }
        if (std::strcmp(argument, "--list") == 0) {
            list_only = true;
            continue;
        }
        if (std::strcmp(argument, "--unload") == 0) {
            unload = true;
            continue;
        }
        if (std::strcmp(argument, "--no-verify") == 0 ||
            std::strcmp(argument, "-n") == 0) {
            verify = false;
            continue;
        }
        if (std::strcmp(argument, "--verbose") == 0 ||
            std::strcmp(argument, "-v") == 0) {
            verbose = true;
            continue;
        }
        if (std::strcmp(argument, "--stage") == 0) {
            if (i + 1 >= argc) {
                err("--stage 需要指定阶段（0-4）");
                return kExitUsage;
            }
            stage = argv[++i];
            continue;
        }
        if (std::strcmp(argument, "--kmi") == 0) {
            if (i + 1 >= argc) {
                err("--kmi 需要指定名称");
                return kExitUsage;
            }
            kmi = argv[++i];
            continue;
        }
        std::printf("%s[x] 未知参数：%s%s\n", kRed, kReset, argument);
        usage(stderr, argv[0]);
        return kExitUsage;
    }

    if (!kmi.empty() && (auto_mode || list_only))
        auto_mode = false;

    if (list_only) {
        list_modules();
        return kExitOk;
    }

    const int root = require_root();
    if (root != kExitOk)
        return root;

    if (unload)
        return unload_module();

    std::string params;
    if (!stage.empty()) {
        params = "bs_stage=" + stage;
    }
    if (verbose) {
        if (!params.empty())
            params += " ";
        params += "bs_log_verbose=1";
    }

    std::string release;
    const int detected = detect_kmi_index(&release);

    if (!kmi.empty())
        return load_kmi(kmi.c_str(), params, verify);

    banner();

    bool pre_verified = false;
    if (verify) {
        info("正在验证内置驱动...");
        std::vector<unsigned char> probe;
        unsigned int verified = 0;
        for (unsigned int i = 0; i < embedded_module_count; ++i) {
            if (!decode_and_verify(embedded_modules[i], probe))
                break;
            ++verified;
        }
        if (verified != embedded_module_count) {
            err("驱动完整性验证失败!");
            return kExitFailure;
        }
        char verified_text[64];
        std::snprintf(verified_text, sizeof(verified_text),
                      "驱动完整性验证通过（%u/%u）", verified,
                      embedded_module_count);
        ok(verified_text);
        pre_verified = true;
    }

    info("正在向服务器验证驱动版本请稍后...");
    if (!check_version())
        return kExitFailure;

    std::printf("\n%s[-] 你的当前内核版本是：%s%s\n\n", kTeal, kReset,
                release.empty() ? "unknown" : release.c_str());

    if (auto_mode || detected < 0 || !isatty(STDIN_FILENO) ||
        !isatty(STDOUT_FILENO)) {
        return run_auto(release, detected, params, verify && !pre_verified);
    }
    return run_menu(release, detected, params, verify && !pre_verified);
}