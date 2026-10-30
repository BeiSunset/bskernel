#define pr_fmt(fmt) KBUILD_MODNAME ": compat: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/cred.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/task_work.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <asm/fixmap.h>
#include <asm/pgtable.h>

#include "compat.h"

#include <linux/version.h>


unsigned long (*bs_kallsyms_lookup_name)(const char *name);
int (*bs_kallsyms_lookup_size_offset)(unsigned long addr, unsigned long *size,
				       unsigned long *offset);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
__typeof__(&close_fd) bs_close_fd;
#endif
__typeof__(&kern_path) bs_kern_path;
__typeof__(&override_creds) bs_override_creds;
__typeof__(&revert_creds) bs_revert_creds;
__typeof__(&kernel_read) bs_kernel_read;
__typeof__(&kernel_write) bs_kernel_write;
int (*bs_task_work_add)(struct task_struct *task,
			 struct callback_head *work,
			 enum task_work_notify_mode notify);
struct file *(*bs_filp_open)(const char *name, int flags, umode_t mode);
struct vm_struct *(*bs_get_vm_area_caller)(unsigned long size,
					    unsigned long flags,
					    const void *caller);
int (*bs_apply_to_page_range)(struct mm_struct *mm, unsigned long addr,
			       unsigned long size, pte_fn_t fn, void *data);
void (*bs_free_vm_area)(struct vm_struct *area);
void (*bs___sync_icache_dcache)(pte_t pte);
void (*bs___set_fixmap)(enum fixed_addresses idx, phys_addr_t phys,
			 pgprot_t prot);
long (*bs_copy_to_kernel_nofault)(void *dst, const void *src, size_t size);
void (*bs_dcache_clean_inval_poc)(unsigned long start, unsigned long end);
int (*bs_set_security_override_from_ctx)(struct cred *cred, const char *secctx);
struct file *(*bs_get_task_exe_file)(struct task_struct *task);
struct group_info *(*bs_groups_alloc)(int gidsetsize);
int (*bs_groups_search)(const struct group_info *group_info, kgid_t grp);
void (*bs_groups_sort)(struct group_info *group_info);
int (*bs_set_current_groups)(struct group_info *group_info);
struct mm_struct *bs_init_mm;

#define RESOLVE(fn)								\
	do {									\
		*(unsigned long *)&bs_##fn = bs_kallsyms_lookup_name(#fn);	\
		if (!bs_##fn) {						\
			pr_warn("missing symbol: %s\n", #fn);			\
			missing++;						\
		}								\
	} while (0)

int bs_compat_init(void)
{
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	unsigned long addr;
	int ret;
	int missing = 0;

	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_err("cannot resolve kallsyms_lookup_name via kprobe: %d\n",
		       ret);
		return ret;
	}
	bs_kallsyms_lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);
	if (!bs_kallsyms_lookup_name)
		return -ENOENT;

	RESOLVE(kallsyms_lookup_size_offset);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	RESOLVE(close_fd);
#endif
	RESOLVE(kern_path);
	RESOLVE(override_creds);
	RESOLVE(revert_creds);
	RESOLVE(kernel_read);
	RESOLVE(kernel_write);
	RESOLVE(task_work_add);
	RESOLVE(filp_open);
	RESOLVE(get_vm_area_caller);
	RESOLVE(apply_to_page_range);
	RESOLVE(free_vm_area);
	RESOLVE(__sync_icache_dcache);
	RESOLVE(__set_fixmap);
	RESOLVE(copy_to_kernel_nofault);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	RESOLVE(dcache_clean_inval_poc);
#endif
	RESOLVE(set_security_override_from_ctx);
	RESOLVE(get_task_exe_file);
	RESOLVE(groups_alloc);
	RESOLVE(groups_search);
	RESOLVE(groups_sort);
	RESOLVE(set_current_groups);

	addr = bs_kallsyms_lookup_name("init_mm");
	bs_init_mm = (void *)addr;
	if (!bs_init_mm) {
		pr_warn("missing symbol: init_mm\n");
		missing++;
	}

	if (missing) {
		pr_err("%d required symbol(s) missing\n", missing);
		return -ENOENT;
	}
	pr_info("symbol compatibility layer ready\n");
	return 0;
}

