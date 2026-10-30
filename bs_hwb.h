#ifndef BS_HWB_H
#define BS_HWB_H

#include <linux/types.h>

struct file;

long bs_hwb_info(void __user *arg);
long bs_hwb_slots(void __user *arg);
long bs_hwb_set(struct file *owner, void __user *arg);
long bs_hwb_read(void __user *arg);
long bs_hwb_clear(void);
void bs_hwb_release_owner(struct file *owner);
void bs_hwb_exit(void);

#endif /* BS_HWB_H */
