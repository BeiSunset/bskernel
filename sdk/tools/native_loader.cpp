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
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#if !defined(BSKERNEL_HTTPS)
#error "mbedTLS support (BSKERNEL_HTTPS) is required for the version check"
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

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
static constexpr const char* kVersion = "1.0.6";
static constexpr const char* kVersionFull = "1.0.6";
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

/* ---- version check (real HTTPS via mbedTLS, strict pass/fail) ---- */

static const char* const kVersionUrl =
    "/2687bec922cbed3dc7b54770e8337f7f?qq_aio_chat_type=2";

static long long monotonic_ms() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000 +
           static_cast<long long>(now.tv_nsec) / 1000000;
}

struct ResolveJob {
    const char* host;
    const char* port;
    struct addrinfo* result;
    std::atomic<int> done;
};

static void* resolve_thread(void* argument) {
    ResolveJob* job = static_cast<ResolveJob*>(argument);
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    job->result = nullptr;
    if (getaddrinfo(job->host, job->port, &hints, &job->result) != 0)
        job->result = nullptr;
    job->done.store(1);
    return nullptr;
}

/* getaddrinfo() has no timeout of its own; a stuck resolver would otherwise
 * freeze startup. The job is leaked when the resolver misses the deadline
 * because the thread cannot be cancelled; the process is short-lived. */
static struct addrinfo* resolve_with_timeout(const char* host,
                                             const char* port,
                                             int timeout_ms) {
    ResolveJob* job = new ResolveJob{host, port, nullptr};
    if (job == nullptr)
        return nullptr;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, resolve_thread, job) != 0) {
        delete job;
        return nullptr;
    }
    pthread_detach(thread);
    const long long deadline = monotonic_ms() + timeout_ms;
    while (!job->done.load() && monotonic_ms() < deadline)
        usleep(5000);
    if (!job->done.load())
        return nullptr;
    struct addrinfo* result = job->result;
    delete job;
    return result;
}

/* Non-blocking connect with a deadline: a dead network must not hang the UI. */
static int tcp_connect_timeout(const char* host, const char* port,
                               int timeout_ms) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* addresses = resolve_with_timeout(host, port, 3000);
    if (addresses == nullptr)
        return -1;

    int connected_fd = -1;
    for (struct addrinfo* entry = addresses; entry && connected_fd < 0;
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
            int poll_result;
            do {
                poll_result = poll(&poller, 1, timeout_ms);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result > 0) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                               &length) == 0 &&
                    socket_error == 0)
                    result = 0;
            }
        }
        if (result == 0) {
            (void)fcntl(fd, F_SETFL, flags);
            connected_fd = fd;
        } else {
            close(fd);
        }
    }
    freeaddrinfo(addresses);
    return connected_fd;
}

/* Trust anchors from the platform store; silently skip unreadable entries. */
static unsigned int load_system_ca(mbedtls_x509_crt* chain) {
    unsigned int loaded = 0;
    DIR* dir = opendir("/system/etc/security/cacerts");
    if (!dir)
        return 0;
    while (struct dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.')
            continue;
        char path[512];
        std::snprintf(path, sizeof(path), "/system/etc/security/cacerts/%s",
                      entry->d_name);
        if (mbedtls_x509_crt_parse_file(chain, path) == 0)
            ++loaded;
    }
    closedir(dir);
    return loaded;
}

static bool dechunk(const std::string& in, std::string* out);

/* Returns the page body, or false when the request could not be completed. */
static bool https_get(const char* host, const char* path, std::string* body) {
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_net_init(&server);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&ca);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    bool ok_result = false;
    bool handshake_done = false;
    const char* personal = "bsloader";
    char request[512];

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              reinterpret_cast<const unsigned char*>(personal),
                              std::strlen(personal)) != 0)
        goto out;
    if (load_system_ca(&ca) == 0 || ca.version == 0) {
        err("驱动版本验证失败,无法读取系统证书库!");
        goto out;
    }

    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto out;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);

    if (mbedtls_ssl_setup(&ssl, &conf) != 0)
        goto out;
    if (mbedtls_ssl_set_hostname(&ssl, host) != 0)
        goto out;
    {
        const int fd = tcp_connect_timeout(host, "443", 5000);
        if (fd < 0)
            goto out;
        server.fd = fd;
    }

    {
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        (void)setsockopt(server.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
        (void)setsockopt(server.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
    }

    mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv,
                        nullptr);
    {
        const long long deadline = monotonic_ms() + 8000;
        for (;;) {
            const int result = mbedtls_ssl_handshake(&ssl);
            if (result == 0) {
                handshake_done = true;
                break;
            }
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE)
                break;
            if (monotonic_ms() > deadline)
                break;
        }
        if (!handshake_done)
            goto out;
    }

    if (std::snprintf(request, sizeof(request),
                      "GET %s HTTP/1.1\r\nHost: %s\r\n"
                      "User-Agent: bsloader/%s\r\nAccept: */*\r\n"
                      "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                      path, host, kVersionFull) >= int(sizeof(request))) {
        goto out;
    }
    {
        const size_t request_len = std::strlen(request);
        size_t sent = 0;
        const long long deadline = monotonic_ms() + 8000;
        while (sent < request_len) {
            const int written = mbedtls_ssl_write(
                &ssl,
                reinterpret_cast<const unsigned char*>(request) + sent,
                request_len - sent);
            if (written > 0) {
                sent += size_t(written);
                continue;
            }
            if (written == MBEDTLS_ERR_SSL_WANT_READ ||
                written == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (monotonic_ms() > deadline)
                    break;
                continue;
            }
            break;
        }
        if (sent != request_len)
            goto out;
    }

    {
        std::string response;
        unsigned char buffer[2048];
        const long long deadline = monotonic_ms() + 10000;
        for (;;) {
            const int got = mbedtls_ssl_read(&ssl, buffer, sizeof(buffer));
            if (got == MBEDTLS_ERR_SSL_WANT_READ ||
                got == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (monotonic_ms() > deadline)
                    break;
                continue;
            }
            if (got <= 0)
                break;
            response.append(reinterpret_cast<const char*>(buffer),
                            size_t(got));
            if (response.size() > 512u * 1024u)
                break;
        }
        if (response.empty())
            goto out;
        if (response.compare(0, 12, "HTTP/1.1 200") != 0)
            goto out;
        const size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos)
            goto out;
        if (response.find("Transfer-Encoding: chunked") !=
                std::string::npos ||
            response.find("transfer-encoding: chunked") !=
                std::string::npos) {
            std::string decoded;
            if (!dechunk(response.substr(header_end + 4), &decoded))
                goto out;
            *body = decoded;
        } else {
            *body = response.substr(header_end + 4);
        }
        ok_result = true;
    }

out:
    if (handshake_done)
        mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&server);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ok_result;
}

/* Decode a Transfer-Encoding: chunked body; returns false on malformed data. */
static bool dechunk(const std::string& in, std::string* out) {
    size_t pos = 0;
    out->clear();
    while (pos < in.size()) {
        const size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos)
            return false;
        const unsigned long length =
            std::strtoul(in.substr(pos, eol - pos).c_str(), nullptr, 16);
        pos = eol + 2;
        if (length == 0)
            return true;
        if (pos + length > in.size())
            return false;
        out->append(in, pos, length);
        pos += length + 2;
    }
    return true;
}

static bool extract_html_content(const std::string& page, std::string* out) {
    const std::string key = "\"html_content\"";
    size_t at = page.find(key);
    if (at == std::string::npos)
        return false;
    at = page.find('"', at + key.size());
    if (at == std::string::npos)
        return false;
    const size_t end = page.find('"', at + 1);
    if (end == std::string::npos)
        return false;
    *out = page.substr(at + 1, end - at - 1);
    return true;
}

/* Strict "major[.minor[.patch]]" parse: digits only, no empty/overlong parts. */
static bool parse_version(const std::string& text, int out[3]) {
    if (text.empty() || text.size() > 32)
        return false;
    out[0] = out[1] = out[2] = 0;
    size_t pos = 0;
    for (int index = 0; index < 3; ++index) {
        if (pos >= text.size())
            return false;
        size_t end = text.find('.', pos);
        const bool last = end == std::string::npos;
        if (last)
            end = text.size();
        if (end == pos || end - pos > 9)
            return false;
        for (size_t i = pos; i < end; ++i) {
            if (text[i] < '0' || text[i] > '9')
                return false;
        }
        out[index] = int(std::strtol(text.substr(pos, end - pos).c_str(),
                                     nullptr, 10));
        if (last)
            return true;
        pos = end + 1;
    }
    return true;
}

static bool valid_version(const std::string& text) {
    int parsed[3];
    return parse_version(text, parsed);
}

/* >0 when right is newer than left. */
static int compare_version(const std::string& left, const std::string& right) {
    int l[3] = {0, 0, 0};
    int r[3] = {0, 0, 0};
    if (!parse_version(left, l) || !parse_version(right, r))
        return 0;
    for (int i = 0; i < 3; ++i) {
        if (r[i] != l[i])
            return r[i] - l[i];
    }
    return 0;
}

static bool check_version() {
    std::string page;
    if (!https_get("sharechain.qq.com", kVersionUrl, &page)) {
        warn("无法连接版本服务器");
        err("驱动版本验证失败,请检查网络后重试!");
        return false;
    }
    std::string remote;
    if (!extract_html_content(page, &remote) || !valid_version(remote)) {
        err("驱动版本验证失败,版本号格式无效!");
        return false;
    }
    if (compare_version(kVersionFull, remote) > 0) {
        err("驱动版本验证失败,请更新最新版本!");
        return false;
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

/* /sys/module/bs/version is created from MODULE_VERSION; a foreign module that
 * happens to be called "bs" will not carry our version string. */
static bool loaded_module_is_ours() {
    std::FILE* file = std::fopen("/sys/module/bs/version", "r");
    if (!file)
        return false;
    char line[64] = {0};
    const bool read_ok = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read_ok)
        return false;
    std::string value(line);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value == BS_EMBEDDED_MODULE_VERSION;
}

/* Diagnostic stages 1-3 return from mod_init before any fd-install entry is
 * registered, so such a module is loaded but the control interface is absent.
 * Read the parameter (root-only file) so the loader does not report success. */
static int loaded_module_stage() {
    std::FILE* file = std::fopen("/sys/module/bs/parameters/bs_stage", "r");
    if (!file)
        return -1;
    int stage = -1;
    if (std::fscanf(file, "%d", &stage) != 1)
        stage = -1;
    std::fclose(file);
    return stage;
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
        if (result == 0) {
            errno = EIO;
            return false;
        }
        written += size_t(result);
    }
    return true;
}

/* Stage tells which step failed; only stage 3 is a kernel-side rejection. */
enum {
    kLoadMemfd = 0,
    kLoadWrite = 1,
    kLoadSeek = 2,
    kLoadFinit = 3,
};

static int load_module_bytes(const unsigned char* data, size_t size,
                             const std::string& params, int* out_stage) {
    *out_stage = kLoadMemfd;
    const long raw_fd = syscall(__NR_memfd_create, "",
                                static_cast<long>(MFD_CLOEXEC));
    if (raw_fd < 0)
        return errno;
    const int fd = static_cast<int>(raw_fd);

    int error = 0;
    errno = 0;
    *out_stage = kLoadWrite;
    if (!write_all(fd, data, size)) {
        error = errno != 0 ? errno : EIO;
    } else {
        *out_stage = kLoadSeek;
        if (::lseek(fd, 0, SEEK_SET) < 0) {
            error = errno;
        } else {
            *out_stage = kLoadFinit;
            if (syscall(__NR_finit_module, static_cast<long>(fd),
                        params.c_str(), 0L) != 0)
                error = errno;
        }
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
        if (!loaded_module_is_ours()) {
            err("已存在同名但非本驱动的模块（/sys/module/bs 版本不符）");
            return kExitFailure;
        }
        const int stage = loaded_module_stage();
        if (stage == 1 || stage == 2 || stage == 3) {
            err("当前模块是以诊断阶段加载的（无控制接口）");
            err("请先执行 --unload 再完整加载");
            return kExitFailure;
        }
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

    int stage = kLoadMemfd;
    const int error = load_module_bytes(ko.data(), ko.size(), params, &stage);
    if (error != 0) {
        switch (stage) {
        case kLoadMemfd:
            std::printf("%s[x] 创建 memfd 失败!%s %s (%d)\n", kRed, kReset,
                        errno_name(error), error);
            break;
        case kLoadWrite:
            std::printf("%s[x] 写入模块数据失败!%s %s (%d)\n", kRed, kReset,
                        errno_name(error), error);
            break;
        case kLoadSeek:
            std::printf("%s[x] 重置模块数据偏移失败!%s %s (%d)\n", kRed, kReset,
                        errno_name(error), error);
            break;
        default:
            std::printf("%s[x] 内核模块加载失败!%s %s (%d)\n", kRed, kReset,
                        errno_name(error), error);
            err("提示: 失败发生在内核装载阶段，可执行 dmesg 查看内核原因");
            if (error == ENOENT || error == EINVAL)
                err("提示: 可能是 vendor 内核缺符号或被裁剪，可先用 --stage 1 验证兼容层");
            break;
        }
        if (stage == kLoadFinit && error == EEXIST) {
            if (loaded_module_is_ours()) {
                ok("内核模块加载成功!");
                return kExitOk;
            }
            err("已存在同名但非本驱动的模块（/sys/module/bs 版本不符）");
            return kExitFailure;
        }
        return kExitFailure;
    }
    if (!is_module_loaded() || !loaded_module_is_ours()) {
        err("内核模块加载后校验失败!");
        return kExitFailure;
    }
    ok("内核模块加载成功!");
    return kExitOk;
}

static int unload_module() {
    info("正在卸载内核模块...");
    const long result = syscall(__NR_delete_module, "bs", kO_NONBLOCK);
    if (result != 0) {
        const int error = errno;
        if (error == ENOENT) {
            err("内核模块未加载");
            return kExitFailure;
        }
        std::printf("%s[x] 卸载失败%s %s (%d)\n", kRed, kReset,
                    errno_name(error), error);
        if (error == EBUSY)
            err("提示: 仍有进程持有控制 fd 或 hwb 待还原；关闭相关进程/执行 CLEAR 后重试，或重启设备");
        else if (error == EPERM)
            err("提示: 需要 root 权限执行卸载");
        return kExitFailure;
    }
    if (is_module_loaded()) {
        err("卸载命令返回成功，但模块仍在 /proc/modules 中");
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
    (void)verify;
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
                        true);
    }
}

static int run_auto(const std::string& release, int detected,
                    const std::string& params, bool verify) {
    (void)verify;
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
    return load_kmi(kKmiTargets[detected].kmi, params, true);
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
        "  %s --kmi NAME          直接加载指定 KMI（如 android15-6.6）\n"
        "  %s --stage N           分阶段诊断（0=完整 1=仅兼容层 2=+内存 3=+文件 4=+hooks）\n"

        "  %s --unload            卸载已加载的模块\n"
        "  %s --help              显示本帮助\n"
        "\n"
        "说明: 6 个内核版本的驱动已内嵌在本可执行文件里，加载时直接以\n"
        "memfd + finit_module 注入内核，不会在磁盘上释放任何 .ko 文件。\n"
        "需要 root 权限；本程序是 ELF 可执行文件，直接运行（不要用 sh）。\n"
        "例如: su -c /data/local/tmp/BSLoaderDriver.sh --auto\n"
        "退出码: 0 成功, 1 加载失败, 2 用法错误, 3 非 root\n",
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


        if (std::strcmp(argument, "--stage") == 0) {
            if (i + 1 >= argc) {
                err("--stage 需要指定阶段（0-4）");
                return kExitUsage;
            }
            stage = argv[++i];
            {
                char* stage_end = nullptr;
                const long stage_value =
                    std::strtol(stage.c_str(), &stage_end, 10);
                if (stage_end == stage.c_str() || *stage_end != '\0' ||
                    stage_value < 0 || stage_value > 4) {
                    err("--stage 需要 0-4 之间的数字");
                    return kExitUsage;
                }
            }
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
    if (unload && (!kmi.empty() || !stage.empty() || auto_mode)) {
        err("--unload 不能与加载参数同时使用");
        return kExitUsage;
    }

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
        params = "p=" + stage;
    }


    std::string release;
    const int detected = detect_kmi_index(&release);

    if (!g_quiet && isatty(STDOUT_FILENO))
        banner();

    {
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
        ok("驱动完整性验证通过!");
    }

    info("正在向服务器验证驱动版本请稍后...");
    if (!check_version())
        return kExitFailure;

    std::printf("\n%s[-] 你的当前内核版本是：%s%s\n\n", kTeal, kReset,
                release.empty() ? "unknown" : release.c_str());

    if (!kmi.empty())
        return load_kmi(kmi.c_str(), params, true);
    if (auto_mode || detected < 0 || !isatty(STDIN_FILENO) ||
        !isatty(STDOUT_FILENO)) {
        return run_auto(release, detected, params, true);
    }
    return run_menu(release, detected, params, true);
}