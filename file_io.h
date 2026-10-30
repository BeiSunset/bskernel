#ifndef BS_FILE_IO_H
#define BS_FILE_IO_H

#include <linux/types.h>

int bs_file_io_init(void);
void bs_file_io_exit(void);

long bs_file_read(void __user *arg);
long bs_file_write(void __user *arg);

#endif /* BS_FILE_IO_H */
