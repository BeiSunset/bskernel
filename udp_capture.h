#ifndef BS_UDP_CAPTURE_H
#define BS_UDP_CAPTURE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Buffer-size-aware replacement for UDP_IOC_READ: the legacy request in
 * abi.h has no capacity field and changing it would change the _IOWR
 * encoding, so the command lives here as an internal extension. */
struct udp_cap_read_v2_req {
    uint64_t buf;
    uint32_t max_count;
    uint32_t buf_size;
    uint32_t out_count;
    uint32_t reserved;
    uint64_t overflow;
    uint64_t captured;
};

#define UDP_IOC_READ_V2 _IOWR('D', 38, struct udp_cap_read_v2_req)

void udp_capture_exit(void);
long udp_capture_init(void __user *arg);
long udp_capture_read(void __user *arg);
long udp_capture_read_v2(void __user *arg);
long udp_capture_stop(void);

#endif
