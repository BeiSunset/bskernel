#ifndef BS_COMPAT_H
#define BS_COMPAT_H

#include <linux/version.h>


#include <linux/types.h>
#include <linux/kprobes.h>
#include <linux/version.h>
#include <linux/cred.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/task_work.h>
#include <linux/uidgid.h>
#include <asm/pgtable.h>
#include <asm/fixmap.h>

struct vm_struct;
struct mm_struct;
struct group_info;

extern unsigned long (*bs_kallsyms_lookup_name)(const char *name);
extern int (*bs_kallsyms_lookup_size_offset)(unsigned long addr,
                                              unsigned long *size,
                                              unsigned long *offset);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
extern __typeof__(&close_fd) bs_close_fd;
#endif
extern __typeof__(&kern_path) bs_kern_path;
extern __typeof__(&override_creds) bs_override_creds;
extern __typeof__(&revert_creds) bs_revert_creds;
extern __typeof__(&kernel_read) bs_kernel_read;
extern __typeof__(&kernel_write) bs_kernel_write;
extern int (*bs_task_work_add)(struct task_struct *task,
                                struct callback_head *work,
                                enum task_work_notify_mode notify);
extern struct file *(*bs_filp_open)(const char *name, int flags, umode_t mode);
extern struct vm_struct *(*bs_get_vm_area_caller)(unsigned long size,
                                                   unsigned long flags,
                                                   const void *caller);
extern int (*bs_apply_to_page_range)(struct mm_struct *mm,
                                      unsigned long addr,
                                      unsigned long size,
                                      pte_fn_t fn,
                                      void *data);
extern void (*bs_free_vm_area)(struct vm_struct *area);
extern void (*bs___sync_icache_dcache)(pte_t pte);
extern void (*bs___set_fixmap)(enum fixed_addresses idx, phys_addr_t phys,
                                pgprot_t prot);
extern long (*bs_copy_to_kernel_nofault)(void *dst, const void *src,
                                          size_t size);
extern void (*bs_dcache_clean_inval_poc)(unsigned long start,
                                          unsigned long end);
extern int (*bs_set_security_override_from_ctx)(struct cred *cred,
                                                 const char *secctx);
extern struct file *(*bs_get_task_exe_file)(struct task_struct *task);
extern struct group_info *(*bs_groups_alloc)(int gidsetsize);
extern int (*bs_groups_search)(const struct group_info *group_info,
                                kgid_t grp);
extern void (*bs_groups_sort)(struct group_info *group_info);
extern int (*bs_set_current_groups)(struct group_info *group_info);
extern struct mm_struct *bs_init_mm;

int bs_compat_init(void);

#define kallsyms_lookup_name bs_kallsyms_lookup_name
#define kallsyms_lookup_size_offset (*bs_kallsyms_lookup_size_offset)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define close_fd (*bs_close_fd)
#endif
#define kern_path (*bs_kern_path)
#define override_creds (*bs_override_creds)
#define revert_creds (*bs_revert_creds)
#define kernel_read (*bs_kernel_read)
#define kernel_write (*bs_kernel_write)
#define task_work_add (*bs_task_work_add)
#define filp_open (*bs_filp_open)
#define get_vm_area_caller (*bs_get_vm_area_caller)
#define apply_to_page_range (*bs_apply_to_page_range)
#define free_vm_area (*bs_free_vm_area)
#define __sync_icache_dcache (*bs___sync_icache_dcache)
#define __set_fixmap (*bs___set_fixmap)
#define copy_to_kernel_nofault (*bs_copy_to_kernel_nofault)
#define dcache_clean_inval_poc (*bs_dcache_clean_inval_poc)
#define set_security_override_from_ctx (*bs_set_security_override_from_ctx)
#define get_task_exe_file (*bs_get_task_exe_file)
#define groups_alloc (*bs_groups_alloc)
#define groups_search (*bs_groups_search)
#define groups_sort (*bs_groups_sort)
#define set_current_groups (*bs_set_current_groups)
#define init_mm (*bs_init_mm)


#endif /* BS_COMPAT_H */
