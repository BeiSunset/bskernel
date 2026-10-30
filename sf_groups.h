#ifndef BS_SF_GROUPS_H
#define BS_SF_GROUPS_H

#include <linux/types.h>

long bs_sf_groups_acquire(void __user *arg);
bool bs_is_surfaceflinger_caller(void);

#endif /* BS_SF_GROUPS_H */
