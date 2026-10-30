#ifndef DFM_COMPAT_H
#define DFM_COMPAT_H

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

extern unsigned long (*dfm_kallsyms_lookup_name)(const char *name);
extern int (*dfm_kallsyms_lookup_size_offset)(unsigned long addr,
                                              unsigned long *size,
                                              unsigned long *offset);
extern __typeof__(&close_fd) dfm_close_fd;
extern __typeof__(&kern_path) dfm_kern_path;
extern __typeof__(&override_creds) dfm_override_creds;
extern __typeof__(&revert_creds) dfm_revert_creds;
extern __typeof__(&kernel_read) dfm_kernel_read;
extern __typeof__(&kernel_write) dfm_kernel_write;
extern int (*dfm_task_work_add)(struct task_struct *task,
                                struct callback_head *work,
                                enum task_work_notify_mode notify);
extern struct file *(*dfm_filp_open)(const char *name, int flags, umode_t mode);
extern struct vm_struct *(*dfm_get_vm_area_caller)(unsigned long size,
                                                   unsigned long flags,
                                                   const void *caller);
extern int (*dfm_apply_to_page_range)(struct mm_struct *mm,
                                      unsigned long addr,
                                      unsigned long size,
                                      pte_fn_t fn,
                                      void *data);
extern void (*dfm_free_vm_area)(struct vm_struct *area);
extern void (*dfm___sync_icache_dcache)(pte_t pte);
extern void (*dfm___set_fixmap)(enum fixed_addresses idx, phys_addr_t phys,
                                pgprot_t prot);
extern long (*dfm_copy_to_kernel_nofault)(void *dst, const void *src,
                                          size_t size);
extern void (*dfm_dcache_clean_inval_poc)(unsigned long start,
                                          unsigned long end);
extern int (*dfm_set_security_override_from_ctx)(struct cred *cred,
                                                 const char *secctx);
extern struct file *(*dfm_get_task_exe_file)(struct task_struct *task);
extern struct group_info *(*dfm_groups_alloc)(int gidsetsize);
extern int (*dfm_groups_search)(const struct group_info *group_info,
                                kgid_t grp);
extern void (*dfm_groups_sort)(struct group_info *group_info);
extern int (*dfm_set_current_groups)(struct group_info *group_info);
extern struct mm_struct *dfm_init_mm;

int dfm_compat_init(void);

#define kallsyms_lookup_name dfm_kallsyms_lookup_name
#define kallsyms_lookup_size_offset (*dfm_kallsyms_lookup_size_offset)
#define close_fd (*dfm_close_fd)
#define kern_path (*dfm_kern_path)
#define override_creds (*dfm_override_creds)
#define revert_creds (*dfm_revert_creds)
#define kernel_read (*dfm_kernel_read)
#define kernel_write (*dfm_kernel_write)
#define task_work_add (*dfm_task_work_add)
#define filp_open (*dfm_filp_open)
#define get_vm_area_caller (*dfm_get_vm_area_caller)
#define apply_to_page_range (*dfm_apply_to_page_range)
#define free_vm_area (*dfm_free_vm_area)
#define __sync_icache_dcache (*dfm___sync_icache_dcache)
#define __set_fixmap (*dfm___set_fixmap)
#define copy_to_kernel_nofault (*dfm_copy_to_kernel_nofault)
#define dcache_clean_inval_poc (*dfm_dcache_clean_inval_poc)
#define set_security_override_from_ctx (*dfm_set_security_override_from_ctx)
#define get_task_exe_file (*dfm_get_task_exe_file)
#define groups_alloc (*dfm_groups_alloc)
#define groups_search (*dfm_groups_search)
#define groups_sort (*dfm_groups_sort)
#define set_current_groups (*dfm_set_current_groups)
#define init_mm (*dfm_init_mm)

#endif /* DFM_COMPAT_H */
