#ifndef BS_UDP_CAPTURE_H
#define BS_UDP_CAPTURE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* UDP_IOC_READ_V2 and its request live in abi.h so the SDK can use them. */

void udp_capture_exit(void);
long udp_capture_init(void __user *arg);
long udp_capture_read(void __user *arg);
long udp_capture_read_v2(void __user *arg);
long udp_capture_stop(void);

#endif
