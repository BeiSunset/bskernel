#define pr_fmt(fmt) KBUILD_MODNAME ": compat: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/task_work.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <asm/fixmap.h>
#include <asm/pgtable.h>

#include "compat.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
/* GKI 6.1+ moved VFS helpers into this namespace; without the import the
 * kernel refuses to resolve them even though they are exported. */
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif

unsigned long (*dfm_kallsyms_lookup_name)(const char *name);
int (*dfm_kallsyms_lookup_size_offset)(unsigned long addr, unsigned long *size,
				       unsigned long *offset);
int (*dfm_task_work_add)(struct task_struct *task,
			 struct callback_head *work,
			 enum task_work_notify_mode notify);
struct file *(*dfm_filp_open)(const char *name, int flags, umode_t mode);
struct vm_struct *(*dfm_get_vm_area_caller)(unsigned long size,
					    unsigned long flags,
					    const void *caller);
int (*dfm_apply_to_page_range)(struct mm_struct *mm, unsigned long addr,
			       unsigned long size, pte_fn_t fn, void *data);
void (*dfm_free_vm_area)(struct vm_struct *area);
void (*dfm___sync_icache_dcache)(pte_t pte);
void (*dfm___set_fixmap)(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot);
long (*dfm_copy_to_kernel_nofault)(void *dst, const void *src, size_t size);
void (*dfm_dcache_clean_inval_poc)(unsigned long start, unsigned long end);
int (*dfm_set_security_override_from_ctx)(struct cred *cred, const char *secctx);
struct file *(*dfm_get_task_exe_file)(struct task_struct *task);
struct group_info *(*dfm_groups_alloc)(int gidsetsize);
int (*dfm_groups_search)(const struct group_info *group_info, kgid_t grp);
void (*dfm_groups_sort)(struct group_info *group_info);
int (*dfm_set_current_groups)(struct group_info *group_info);
struct mm_struct *dfm_init_mm;

#define RESOLVE(fn)								\
	do {									\
		*(unsigned long *)&dfm_##fn = dfm_kallsyms_lookup_name(#fn);	\
		if (!dfm_##fn) {						\
			pr_warn("missing symbol: %s\n", #fn);			\
			missing++;						\
		}								\
	} while (0)

int dfm_compat_init(void)
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
	dfm_kallsyms_lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);
	if (!dfm_kallsyms_lookup_name)
		return -ENOENT;

	RESOLVE(kallsyms_lookup_size_offset);
	RESOLVE(task_work_add);
	RESOLVE(filp_open);
	RESOLVE(get_vm_area_caller);
	RESOLVE(apply_to_page_range);
	RESOLVE(free_vm_area);
	RESOLVE(__sync_icache_dcache);
	RESOLVE(__set_fixmap);
	RESOLVE(copy_to_kernel_nofault);
	RESOLVE(dcache_clean_inval_poc);
	RESOLVE(set_security_override_from_ctx);
	RESOLVE(get_task_exe_file);
	RESOLVE(groups_alloc);
	RESOLVE(groups_search);
	RESOLVE(groups_sort);
	RESOLVE(set_current_groups);

	addr = dfm_kallsyms_lookup_name("init_mm");
	dfm_init_mm = (void *)addr;
	if (!dfm_init_mm) {
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
