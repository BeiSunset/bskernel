#define pr_fmt(fmt) fmt
#include "dfm_log.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/version.h>
#include <linux/anon_inodes.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/proc_fs.h>
#include <linux/task_work.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>

#include "abi.h"
#include "install.h"
#include "kgsl.h"
#include "mem.h"
#include "process_mem.h"
#include "syscall.h"
#include "touch.h"
#include "touch_v2.h"
#include "dfm_hwb.h"
#include "udp_capture.h"
#include "file_io.h"
#include "sf_groups.h"
#include "compat.h"
#include "dfm_module.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DFM");
MODULE_DESCRIPTION("DFM lab debug module");
MODULE_VERSION(DFM_MODULE_VERSION);

/* ---- ioctl dispatcher ---- */

static long drv_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    void __user *argp = (void __user *)arg;

    switch (cmd) {
    case MEM_IOC_OPEN_PROCESS_MEM:
        return mem_open_process_mem(argp);
    case MEM_IOC_READ:
        return mem_read(argp);
    case MEM_IOC_READ_CACHE:
        return mem_read_cache(argp);
    case MEM_IOC_READ_BATCH:
        return mem_read_batch(argp);
    case MEM_IOC_GET_PID:
        return mem_get_pid(argp);
    case MEM_IOC_GET_PIDS:
        return mem_get_pids(argp);
    case MEM_IOC_GET_MODULE_BASE:
        return mem_get_module_base(argp);
    case MEM_IOC_GET_MODULE_BASE2:
        return mem_get_module_base2(argp);
    case MEM_IOC_GET_MODULE_PATH:
        return mem_get_module_path(argp);
    case MEM_IOC_QUERY_MAPPING:
        return mem_query_mapping(argp);
    case MEM_IOC_QUERY_ACCESS:
        return mem_query_access(argp);
    case MEM_IOC_QUERY_SIGNAL:
        return mem_query_signal(argp);
    case TOUCH_IOC_INIT:
        return touch_init();
    case TOUCH_IOC_EVENT:
        return touch_event(argp);
    case TOUCH_IOC_INIT_V2:
        return touch_v2_init();
    case TOUCH_IOC_EVENT_V2:
        return touch_v2_event(argp);
    case TOUCH_IOC_GET_STATUS:
        return touch_get_status(argp);
    case MEM_IOC_GET_TLS:
        return mem_get_tls(argp);
    case MEM_IOC_GET_THREAD_TID:
        return mem_get_thread_tid(argp);
    case MEM_IOC_GET_THREAD_TIDS:
        return mem_get_thread_tids(argp);
    case MEM_IOC_GET_THREAD_STACK:
        return mem_get_thread_stack(argp);
    case MEM_IOC_EXECUTE_PACGA:
        return mem_execute_pacga(argp);
    case DFM_IOCTL_HWB_INFO:
        return dfm_hwb_info(argp);
    case DFM_IOCTL_HWB_SLOTS:
        return dfm_hwb_slots(argp);
    case DFM_IOCTL_HWB_SET:
        return dfm_hwb_set(filp, argp);
    case DFM_IOCTL_HWB_READ:
        return dfm_hwb_read(argp);
    case DFM_IOCTL_HWB_CLEAR:
        return dfm_hwb_clear();
    case DFM_IOCTL_KGSL_SYNCSOURCE_PEEK:
        return kgsl_syncsource_peek(argp);
    case DFM_IOCTL_KGSL_QUERY:
        return kgsl_query(argp);
    case UDP_IOC_INIT:
        return udp_capture_init(argp);
    case UDP_IOC_READ:
        return udp_capture_read(argp);
    case UDP_IOC_READ_V2:
        return udp_capture_read_v2(argp);
    case UDP_IOC_STOP:
        return udp_capture_stop();
    case DFM_FILE_IOC_READ:
        return dfm_file_read(argp);
    case DFM_FILE_IOC_WRITE:
        return dfm_file_write(argp);
    case DFM_CRED_IOC_ACQUIRE_SF_GROUPS:
        return dfm_sf_groups_acquire(argp);
    default:
        return -ENOTTY;
    }
}

/* Control fds are handed out per magic reboot() call; the capture/touch
 * engines behind them are process-global singletons, so tear them down only
 * when the last control fd closes instead of on the first one. */
static atomic_t g_drv_fds = ATOMIC_INIT(0);
/* Marks a file that was successfully counted in g_drv_fds. install_fd() can
 * fput() a file whose fd it never installed, and __fput() still calls
 * ->release(): that call must not decrement the counter. */
static const int g_drv_fd_marker;
/* Serialises (inc + fd_install) against (dec + engine teardown), so a previous
 * session's tail cannot stop a session a new fd just started. */
static DEFINE_MUTEX(g_drv_lock);

static int drv_release(struct inode *inode, struct file *filp)
{
    int last;

    /* Per-owner state dies with this fd; the capture/touch engines are
     * process-global, so only the last control fd may stop them. */
    dfm_hwb_release_owner(filp);

    if (filp->private_data != &g_drv_fd_marker)
        return 0;
    filp->private_data = NULL;

    mutex_lock(&g_drv_lock);
    last = atomic_dec_and_test(&g_drv_fds);
    if (last) {
        udp_capture_stop();
        touch_v2_release_all();
        touch_release_all();
    }
    mutex_unlock(&g_drv_lock);
    return 0;
}

static const struct file_operations drv_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = drv_ioctl,
    .compat_ioctl   = drv_ioctl,
    .release        = drv_release,
};

static int g_reboot_kprobe_registered;

/* ---- fd installation ---- */

static int install_fd(void)
{
    struct file *filp;
    int fd;

    /* Create the file before reserving the fd: its fops pin THIS_MODULE, so the
     * temporary pin taken in reboot_handler_pre() is never the last reference
     * when the task_work callback drops it. */
    filp = anon_inode_getfile("[eventpoll]", &drv_fops, NULL,
                              O_RDWR | O_CLOEXEC);
    if (IS_ERR(filp))
        return PTR_ERR(filp);

    fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0) {
        /* __fput() -> drv_release() still runs; private_data is not the marker
         * so the counter is left untouched. */
        fput(filp);
        return fd;
    }

    mutex_lock(&g_drv_lock);
    filp->private_data = (void *)&g_drv_fd_marker;
    atomic_inc(&g_drv_fds);
    fd_install(fd, filp);
    mutex_unlock(&g_drv_lock);
    return fd;
}

int install_fd_to_user(int __user *outp)
{
    int fd;

    if (!outp)
        return -EFAULT;

    fd = install_fd();
    if (copy_to_user(outp, &fd, sizeof(fd))) {
        if (fd >= 0) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,11, 0)
            close_fd(fd);
#else
            ksys_close(fd);
#endif
        }
        return -EFAULT;
    }

    return fd < 0 ? fd : 0;
}

/* ---- task_work callback to install fd from process context ---- */

struct install_tw {
    struct callback_head cb;
    int __user *outp;
};

static void install_fd_tw_func(struct callback_head *cb)
{
    struct install_tw *tw =
        container_of(cb, struct install_tw, cb);

    install_fd_to_user(tw->outp);

    kfree(tw);
    module_put(THIS_MODULE);
}

/* ---- reboot kprobe handler ---- */

static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct pt_regs *real_regs;
    int magic1, magic2;
    unsigned long arg4;
    struct install_tw *tw;

    /* ARM64 syscall wrapper: first arg is pointer to user pt_regs */
    real_regs = (struct pt_regs *)regs->regs[0];

    magic1 = (int)real_regs->regs[0];
    magic2 = (int)real_regs->regs[1];

    if (magic1 != INSTALL_MAGIC1 || magic2 != INSTALL_MAGIC2)
        return 0;

    if (current_uid().val > 1000)
        return 0;

    /* The task_work callback runs module text after the syscall returns;
     * pin the module until it has run so rmmod cannot free that text. */
    if (!try_module_get(THIS_MODULE))
        return 0;

    arg4 = (unsigned long)real_regs->regs[3];

    tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
    if (!tw) {
        module_put(THIS_MODULE);
        return 0;
    }

    tw->outp = (int __user *)arg4;
    tw->cb.func = install_fd_tw_func;

    if (task_work_add(current, &tw->cb, TWA_RESUME)) {
        kfree(tw);
        /* Log before dropping the pin: module text/rodata must not be touched
         * after module_put() may have released the last reference. */
        dfm_warn("task_work_add failed\n");
        module_put(THIS_MODULE);
    }

    return 0;
}

static struct kprobe kp_reboot = {
    .symbol_name = "__arm64_sys_reboot",
    .pre_handler = reboot_handler_pre,
};

/* ---- module init/exit ---- */

static bool __init proc_entry_exists(const char *name)
{
    struct path path;

    if (kern_path(name, 0, &path))
        return false;
    path_put(&path);
    return true;
}

static void __init remove_inte_proc_entries(void)
{
    /* Best-effort existence checks avoid removing already absent entries.
     * Checking existence only looks up the path; it does not open/read it. */
    if (proc_entry_exists("/proc/inte_ko"))
        remove_proc_entry("inte_ko", NULL);
    if (proc_entry_exists("/proc/inte_systbl"))
        remove_proc_entry("inte_systbl", NULL);
    if (proc_entry_exists("/proc/inte_status"))
        remove_proc_entry("inte_status", NULL);
}

/* Default to 6: skip the syscall-table hook (unstable on GKI/CFI kernels)
 * and install the fd through the reboot kprobe instead. */
static int dfm_stage = DFM_STAGE_NO_SYSCALL;
/* Neutral parameter name: the sysfs node should not advertise the stage
 * machinery. The loader is the only writer. */
module_param_named(dfm_stage, dfm_stage, int, 0);
static bool g_mem_inited;
static bool g_file_inited;

static int __init mod_init(void)
{
	int ret;
	int syscall_ret;
	int reboot_ret;

	ret = dfm_compat_init();
	if (ret < 0) {
		pr_err("compat init failed: %d\n", ret);
		return ret;
	}

	dfm_info("compat ready, stage=%d\n", dfm_stage);
	if (dfm_stage == DFM_STAGE_COMPAT)
		return 0;

	ret = mem_core_init();
	if (ret < 0) {
		pr_err("mem_core_init failed: %d\n", ret);
		return ret;
	}
	g_mem_inited = true;
	if (dfm_stage == DFM_STAGE_MEM)
		return 0;

	ret = dfm_file_io_init();
	if (ret < 0) {
		pr_err("dfm_file_io_init failed: %d\n", ret);
		goto err_mem;
	}
	g_file_inited = true;
	if (dfm_stage == DFM_STAGE_FILE)
		return 0;

	if (dfm_stage == DFM_STAGE_SYSCALL) {
		syscall_ret = syscall_hook_init();
		if (syscall_ret < 0)
			dfm_warn("syscall install hook unavailable: %d\n",
				syscall_ret);
		return 0;
	}

	if (dfm_stage == DFM_STAGE_REBOOT) {
		reboot_ret = register_kprobe(&kp_reboot);
		if (reboot_ret < 0) {
			dfm_warn("failed to register kprobe on reboot: %d\n",
				reboot_ret);
			ret = reboot_ret;
			goto err_file;
		}
		g_reboot_kprobe_registered = 1;
		return 0;
	}

	if (dfm_stage == DFM_STAGE_NO_SYSCALL) {
		syscall_ret = -ENOENT;
		dfm_info("skipping syscall hook, using reboot kprobe only\n");
	} else {
		syscall_ret = syscall_hook_init();
		if (syscall_ret < 0)
			dfm_warn("syscall install hook unavailable: %d\n",
				syscall_ret);
	}

	reboot_ret = register_kprobe(&kp_reboot);
	if (reboot_ret < 0) {
		dfm_warn("failed to register kprobe on reboot: %d\n",
			reboot_ret);
	} else {
		g_reboot_kprobe_registered = 1;
	}

	/* Some vendor kernels do not expose __arm64_sys_reboot to kprobes even
	 * though the syscall-table hook works. In the default no-syscall stage,
	 * retry through the syscall hook instead of failing the whole load. */
	if (dfm_stage == DFM_STAGE_NO_SYSCALL && reboot_ret < 0) {
		syscall_ret = syscall_hook_init();
		if (syscall_ret < 0)
			dfm_warn("syscall fallback unavailable: %d\n",
				syscall_ret);
	}

	if (syscall_ret < 0 && reboot_ret < 0) {
		pr_err("no fd install entry registered\n");
		ret = reboot_ret;
		goto err_hooks;
	}

	(void)kgsl_hide_init();

	remove_inte_proc_entries();

	dfm_info("module loaded, syscall_nr=%d reboot_fallback=%d\n",
		syscall_hook_nr(), g_reboot_kprobe_registered);
	return 0;

err_hooks:
	if (dfm_stage == DFM_STAGE_FULL)
		syscall_hook_exit();
err_file:
	if (g_file_inited) {
		dfm_file_io_exit();
		g_file_inited = false;
	}
err_mem:
	if (g_mem_inited) {
		mem_core_exit();
		g_mem_inited = false;
	}
	return ret;
}

static void __exit mod_exit(void)
{
    if (dfm_stage == DFM_STAGE_COMPAT) {
        dfm_info("stage1 module unloaded\n");
        return;
    }
    if (dfm_stage == DFM_STAGE_MEM) {
        if (g_mem_inited)
            mem_core_exit();
        dfm_info("stage2 module unloaded\n");
        return;
    }
    if (dfm_stage == DFM_STAGE_FILE) {
        if (g_file_inited)
            dfm_file_io_exit();
        if (g_mem_inited)
            mem_core_exit();
        dfm_info("stage3 module unloaded\n");
        return;
    }
    if (dfm_stage == DFM_STAGE_SYSCALL) {
        syscall_hook_exit();
        /* This stage hands out the control fd, so every hook engine behind an
         * ioctl is reachable. Without teardown the kprobes, the in-kernel text
         * hooks and the swapped file->f_op would outlive free_module(). */
        udp_capture_exit();
        dfm_hwb_exit();
        touch_v2_release_all();
        touch_exit();
        touch_v2_exit();
        if (g_file_inited)
            dfm_file_io_exit();
        if (g_mem_inited)
            mem_core_exit();
        dfm_info("stage4 module unloaded\n");
        return;
    }
    if (dfm_stage == DFM_STAGE_REBOOT) {
        if (g_reboot_kprobe_registered) {
            unregister_kprobe(&kp_reboot);
            g_reboot_kprobe_registered = 0;
        }
        /* Same as stage 4: the reboot kprobe is the fd install entry here. */
        udp_capture_exit();
        dfm_hwb_exit();
        touch_v2_release_all();
        touch_exit();
        touch_v2_exit();
        if (g_file_inited)
            dfm_file_io_exit();
        if (g_mem_inited)
            mem_core_exit();
        dfm_info("stage5 module unloaded\n");
        return;
    }

    kgsl_hide_exit();
    if (g_reboot_kprobe_registered) {
        unregister_kprobe(&kp_reboot);
        g_reboot_kprobe_registered = 0;
    }
    syscall_hook_exit();
    udp_capture_exit();
    dfm_hwb_exit();
    touch_v2_release_all();
    touch_exit();
    touch_v2_exit();
    if (g_file_inited)
        dfm_file_io_exit();
    if (g_mem_inited)
        mem_core_exit();
    dfm_info("module unloaded\n");
}

module_init(mod_init);
module_exit(mod_exit);
