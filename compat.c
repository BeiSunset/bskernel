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

/*
 * Central symbol table.  Adding a new runtime dependency only requires one
 * entry here plus the matching declaration/macro in compat.h.  Version
 * guards stay next to the entry they apply to instead of being scattered
 * through the resolver function.
 */
struct bs_compat_symbol {
	const char *name;
	void **slot;
};

static const struct bs_compat_symbol bs_compat_symbols[] = {
	{ "kallsyms_lookup_size_offset", (void **)&bs_kallsyms_lookup_size_offset },
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	{ "close_fd", (void **)&bs_close_fd },
#endif
	{ "kern_path", (void **)&bs_kern_path },
	{ "override_creds", (void **)&bs_override_creds },
	{ "revert_creds", (void **)&bs_revert_creds },
	{ "kernel_read", (void **)&bs_kernel_read },
	{ "kernel_write", (void **)&bs_kernel_write },
	{ "task_work_add", (void **)&bs_task_work_add },
	{ "filp_open", (void **)&bs_filp_open },
	{ "get_vm_area_caller", (void **)&bs_get_vm_area_caller },
	{ "apply_to_page_range", (void **)&bs_apply_to_page_range },
	{ "free_vm_area", (void **)&bs_free_vm_area },
	{ "__sync_icache_dcache", (void **)&bs___sync_icache_dcache },
	{ "__set_fixmap", (void **)&bs___set_fixmap },
	{ "copy_to_kernel_nofault", (void **)&bs_copy_to_kernel_nofault },
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
	{ "dcache_clean_inval_poc", (void **)&bs_dcache_clean_inval_poc },
#endif
	{ "set_security_override_from_ctx",
	  (void **)&bs_set_security_override_from_ctx },
	{ "get_task_exe_file", (void **)&bs_get_task_exe_file },
	{ "groups_alloc", (void **)&bs_groups_alloc },
	{ "groups_search", (void **)&bs_groups_search },
	{ "groups_sort", (void **)&bs_groups_sort },
	{ "set_current_groups", (void **)&bs_set_current_groups },
	{ "init_mm", (void **)&bs_init_mm },
};

int bs_compat_init(void)
{
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name",
	};
	unsigned long addr;
	size_t i;
	size_t missing = 0;
	int ret;

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

	for (i = 0; i < ARRAY_SIZE(bs_compat_symbols); i++) {
		const struct bs_compat_symbol *sym = &bs_compat_symbols[i];

		addr = bs_kallsyms_lookup_name(sym->name);
		*sym->slot = (void *)addr;
		if (!addr) {
			pr_err("missing required symbol: %s\n", sym->name);
			missing++;
		}
	}

	if (missing) {
		pr_err("%zu required symbol(s) missing\n", missing);
		return -ENOENT;
	}

	pr_info("resolved %zu symbols\n", ARRAY_SIZE(bs_compat_symbols));
	return 0;
}
