#define pr_fmt(fmt) fmt
#include "bs_log.h"

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "abi.h"
#include "touch_v2.h"

/*
 * Touch Aim V2 follows the event-pool design from the supplied kernel
 * touch.h: user requests are cached, then spliced into the real touchscreen
 * stream immediately before its next SYN_REPORT.  The existing read-proxy
 * backend in touch.c remains independent and unchanged.
 */
#define TOUCH_V2_MAX_SLOTS  10
#define TOUCH_V2_MAX_EVENTS 1024
#define TOUCH_V2_TRACKING_BASE 666
/* Largest batch still spliced into the real frame from the kprobe handler;
 * bigger batches fall back to the workqueue instead of extending the
 * IRQ-disabled window. */
#define TOUCH_V2_INLINE_MAX 32

struct touch_v2_cached_event {
    unsigned int type;
    unsigned int code;
    int value;
};

static DEFINE_SPINLOCK(g_touch_v2_pool_lock);
static struct touch_v2_cached_event
    g_touch_v2_events[TOUCH_V2_MAX_EVENTS];
static unsigned int g_touch_v2_event_count;
static bool g_touch_v2_slot_active[TOUCH_V2_MAX_SLOTS];
/* Index of the pending ABS_MT_POSITION_X event for a slot in the current
 * batch (or -1). Lets a second update for the same slot within one frame
 * refresh the coordinates in place instead of appending another triplet. */
static int g_touch_v2_pos_idx[TOUCH_V2_MAX_SLOTS];

/* Deferred delivery: the kprobe handler only marks a batch and queues work;
 * input_event() itself always runs in the workqueue's process context, so no
 * input/evdev lock is ever taken from an IRQ-disabled kprobe handler. */
static struct work_struct g_touch_v2_flush_work;
static bool g_touch_v2_work_initialized;
static struct input_dev *g_touch_v2_deliver_dev;
static bool g_touch_v2_work_queued;
static bool g_touch_v2_injecting;
static unsigned long g_touch_v2_injected_batches;
static unsigned long g_touch_v2_injected_events;

static DEFINE_MUTEX(g_touch_v2_init_lock);
static struct kprobe g_touch_v2_input_event_kp;
static struct kprobe g_touch_v2_input_inject_event_kp;
static bool g_touch_v2_input_event_registered;
static bool g_touch_v2_input_inject_event_registered;

static bool touch_v2_is_touchscreen(struct input_dev *dev)
{
    if (!dev)
        return false;

    return test_bit(EV_ABS, dev->evbit) &&
           test_bit(ABS_MT_SLOT, dev->absbit) &&
           test_bit(ABS_MT_TRACKING_ID, dev->absbit) &&
           test_bit(ABS_MT_POSITION_X, dev->absbit) &&
           test_bit(ABS_MT_POSITION_Y, dev->absbit) &&
           test_bit(EV_KEY, dev->evbit) &&
           (test_bit(BTN_TOUCH, dev->keybit) ||
            test_bit(BTN_TOOL_FINGER, dev->keybit));
}

static void touch_v2_reset_pos_idx_locked(void)
{
    int i;

    for (i = 0; i < TOUCH_V2_MAX_SLOTS; ++i)
        g_touch_v2_pos_idx[i] = -1;
}

static bool touch_v2_make_room_locked(unsigned int needed)
{
    if (needed > TOUCH_V2_MAX_EVENTS)
        return false;

    if (g_touch_v2_event_count + needed > TOUCH_V2_MAX_EVENTS) {
        /* Match the reference event-pool recovery: discard a stale full
         * batch and keep accepting the newest aim state. Reset the announced
         * slots too, so the next down is a complete tracking-id frame rather
         * than an orphaned position-only move. */
        g_touch_v2_event_count = 0;
        memset(g_touch_v2_slot_active, 0,
               sizeof(g_touch_v2_slot_active));
        touch_v2_reset_pos_idx_locked();
        bs_warn_ratelimited("event pool full; stale batch discarded\n");
        return true;
    }

    return false;
}

static void touch_v2_append_locked(unsigned int type,
                                   unsigned int code, int value)
{
    struct touch_v2_cached_event *event;

    if (g_touch_v2_event_count >= TOUCH_V2_MAX_EVENTS)
        return;

    event = &g_touch_v2_events[g_touch_v2_event_count++];
    event->type = type;
    event->code = code;
    event->value = value;
}

static void touch_v2_queue_down_locked(int slot, int x, int y)
{
    bool first_down = !g_touch_v2_slot_active[slot];
    int idx;

    if (first_down) {
        (void)touch_v2_make_room_locked(8U);
        touch_v2_append_locked(EV_ABS, ABS_MT_SLOT, slot);
        touch_v2_append_locked(EV_ABS, ABS_MT_TRACKING_ID,
                               TOUCH_V2_TRACKING_BASE + slot);
        touch_v2_append_locked(EV_ABS, ABS_MT_TOOL_TYPE,
                               MT_TOOL_FINGER);
        g_touch_v2_pos_idx[slot] = (int)g_touch_v2_event_count;
        touch_v2_append_locked(EV_ABS, ABS_MT_POSITION_X, x);
        touch_v2_append_locked(EV_ABS, ABS_MT_POSITION_Y, y);
        touch_v2_append_locked(EV_ABS, ABS_MT_PRESSURE, 30);
        touch_v2_append_locked(EV_ABS, ABS_MT_TOUCH_MAJOR, 30);
        touch_v2_append_locked(EV_ABS, ABS_MT_TOUCH_MINOR, 30);
        g_touch_v2_slot_active[slot] = true;
        return;
    }

    /* Coalesce: a slot that already has a pending triplet in this frame only
     * needs its position refreshed, so a 120 Hz stream stays at one SLOT plus
     * an X/Y pair per frame instead of growing the batch. */
    idx = g_touch_v2_pos_idx[slot];
    if (idx >= 0 && idx + 1 < (int)g_touch_v2_event_count &&
        g_touch_v2_events[idx].type == EV_ABS &&
        g_touch_v2_events[idx].code == ABS_MT_POSITION_X &&
        g_touch_v2_events[idx + 1].code == ABS_MT_POSITION_Y) {
        g_touch_v2_events[idx].value = x;
        g_touch_v2_events[idx + 1].value = y;
        return;
    }

    if (touch_v2_make_room_locked(3U))
        first_down = true;
    if (first_down) {
        touch_v2_append_locked(EV_ABS, ABS_MT_SLOT, slot);
        touch_v2_append_locked(EV_ABS, ABS_MT_TRACKING_ID,
                               TOUCH_V2_TRACKING_BASE + slot);
        touch_v2_append_locked(EV_ABS, ABS_MT_TOOL_TYPE,
                               MT_TOOL_FINGER);
        g_touch_v2_pos_idx[slot] = (int)g_touch_v2_event_count;
        touch_v2_append_locked(EV_ABS, ABS_MT_POSITION_X, x);
        touch_v2_append_locked(EV_ABS, ABS_MT_POSITION_Y, y);
        touch_v2_append_locked(EV_ABS, ABS_MT_PRESSURE, 30);
        touch_v2_append_locked(EV_ABS, ABS_MT_TOUCH_MAJOR, 30);
        touch_v2_append_locked(EV_ABS, ABS_MT_TOUCH_MINOR, 30);
        g_touch_v2_slot_active[slot] = true;
        return;
    }

    touch_v2_append_locked(EV_ABS, ABS_MT_SLOT, slot);
    g_touch_v2_pos_idx[slot] = (int)g_touch_v2_event_count;
    touch_v2_append_locked(EV_ABS, ABS_MT_POSITION_X, x);
    touch_v2_append_locked(EV_ABS, ABS_MT_POSITION_Y, y);
}

static void touch_v2_queue_up_locked(int slot)
{
    if (!g_touch_v2_slot_active[slot])
        return;

    (void)touch_v2_make_room_locked(2U);
    touch_v2_append_locked(EV_ABS, ABS_MT_SLOT, slot);
    touch_v2_append_locked(EV_ABS, ABS_MT_TRACKING_ID, -1);
    g_touch_v2_pos_idx[slot] = -1;
    g_touch_v2_slot_active[slot] = false;
}

static void touch_v2_flush_workfn(struct work_struct *work)
{
    struct touch_v2_cached_event *batch;
    struct input_dev *dev;
    unsigned int n;
    unsigned int i;
    unsigned long flags;

    batch = kmalloc_array(TOUCH_V2_MAX_EVENTS, sizeof(*batch), GFP_KERNEL);

    for (;;) {
        spin_lock_irqsave(&g_touch_v2_pool_lock, flags);
        n = g_touch_v2_event_count;
        dev = g_touch_v2_deliver_dev;
        if (!n || !dev) {
            spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
            break;
        }
        if (!batch) {
            /* Allocation failed: drop the batch instead of injecting a
             * partial frame. */
            g_touch_v2_event_count = 0;
            spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
            break;
        }
        memcpy(batch, g_touch_v2_events, n * sizeof(*batch));
        g_touch_v2_event_count = 0;
        touch_v2_reset_pos_idx_locked();
        spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);

        /* Process context: input_event() takes input/evdev locks and may
         * wake waiters, which is illegal in the kprobe handler. */
        g_touch_v2_injecting = true;
        for (i = 0; i < n; ++i)
            input_event(dev, batch[i].type, batch[i].code,
                        batch[i].value);
        /* Close our own frame so the batch is a complete packet rather than
         * leaking into the next real touch frame. */
        input_event(dev, EV_SYN, SYN_REPORT, 0);
        g_touch_v2_injecting = false;

        g_touch_v2_injected_batches++;
        g_touch_v2_injected_events += n;
        bs_info_ratelimited("injected %u events (batch %lu, dev=%s, ctx=%s/%d)\n",
                            n, g_touch_v2_injected_batches, dev->name,
                    current->comm, current->pid);
    }

    spin_lock_irqsave(&g_touch_v2_pool_lock, flags);
    dev = g_touch_v2_deliver_dev;
    g_touch_v2_deliver_dev = NULL;
    g_touch_v2_work_queued = false;
    spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
    if (dev)
        put_device(&dev->dev);
    kfree(batch);
}

/* Fast path: splice the pool into the real frame while the panel''s
 * SYN_REPORT is still being processed. This keeps aiming frame-accurate:
 * the events are emitted before the real SYN reaches the input core, so they
 * belong to the same input frame (no extra frame, no scheduling delay).
 * Only the pool lock is taken, and only to copy the batch out; input_event()
 * itself runs without it. */
static bool touch_v2_flush_inline(struct input_dev *dev)
{
    struct touch_v2_cached_event batch[TOUCH_V2_INLINE_MAX];
    unsigned int n;
    unsigned int i;
    unsigned long flags;

    /* The kprobe is on the global input_event(), so it fires for every input
     * device. Never drain the touch pool into a non-touch device: a key or
     * power-button SYN_REPORT would otherwise steal the queued frame. */
    if (!touch_v2_is_touchscreen(dev))
        return false;

    if (READ_ONCE(g_touch_v2_injecting))
        return true;

    spin_lock_irqsave(&g_touch_v2_pool_lock, flags);
    n = g_touch_v2_event_count;
    if (n == 0 || n > TOUCH_V2_INLINE_MAX) {
        spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
        return false;
    }
    memcpy(batch, g_touch_v2_events, n * sizeof(*batch));
    g_touch_v2_event_count = 0;
    touch_v2_reset_pos_idx_locked();
    spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);

    g_touch_v2_injecting = true;
    for (i = 0; i < n; ++i)
        input_event(dev, batch[i].type, batch[i].code, batch[i].value);
    g_touch_v2_injecting = false;

    g_touch_v2_injected_batches++;
    g_touch_v2_injected_events += n;
    bs_info_ratelimited("injected %u events inline (batch %lu, dev=%s, ctx=%s/%d)\n",
                        n, g_touch_v2_injected_batches, dev->name,
                        current->comm, current->pid);
    return true;
}

static void touch_v2_schedule_flush(struct input_dev *dev)
{
    unsigned long flags;

    if (!dev || !touch_v2_is_touchscreen(dev))
        return;
    /* Our own injected frames must not re-enter the queueing path. */
    if (READ_ONCE(g_touch_v2_injecting))
        return;

    spin_lock_irqsave(&g_touch_v2_pool_lock, flags);
    if (!g_touch_v2_event_count || g_touch_v2_work_queued) {
        spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
        return;
    }
    /* Keep the input device alive until the workqueue is done with it. */
    get_device(&dev->dev);
    g_touch_v2_deliver_dev = dev;
    g_touch_v2_work_queued = true;
    spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);

    schedule_work(&g_touch_v2_flush_work);
}

static int touch_v2_input_event_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct input_dev *dev = (struct input_dev *)regs->regs[0];
    unsigned int type = (unsigned int)regs->regs[1];
    unsigned int code = (unsigned int)regs->regs[2];

    if (type == EV_SYN && code == SYN_REPORT && touch_v2_is_touchscreen(dev)) {
        if (!touch_v2_flush_inline(dev))
            touch_v2_schedule_flush(dev);
    }
    return 0;
}

static int touch_v2_input_inject_event_pre(struct kprobe *p,
                                           struct pt_regs *regs)
{
    struct input_handle *handle =
        (struct input_handle *)regs->regs[0];
    unsigned int type = (unsigned int)regs->regs[1];
    unsigned int code = (unsigned int)regs->regs[2];

    if (handle && type == EV_SYN && code == SYN_REPORT) {
        if (!touch_v2_flush_inline(handle->dev))
            touch_v2_schedule_flush(handle->dev);
    }
    return 0;
}

static int touch_v2_register_hooks_locked(void)
{
    int event_ret = 0;
    int inject_ret = 0;

    if (g_touch_v2_input_event_registered ||
        g_touch_v2_input_inject_event_registered)
        return 0;

    memset(&g_touch_v2_input_event_kp, 0,
           sizeof(g_touch_v2_input_event_kp));
    g_touch_v2_input_event_kp.symbol_name = "input_event";
    g_touch_v2_input_event_kp.pre_handler = touch_v2_input_event_pre;
    event_ret = register_kprobe(&g_touch_v2_input_event_kp);
    if (!event_ret)
        g_touch_v2_input_event_registered = true;
    else
        bs_warn("register input_event kprobe failed: %d\n", event_ret);

    memset(&g_touch_v2_input_inject_event_kp, 0,
           sizeof(g_touch_v2_input_inject_event_kp));
    g_touch_v2_input_inject_event_kp.symbol_name = "input_inject_event";
    g_touch_v2_input_inject_event_kp.pre_handler =
        touch_v2_input_inject_event_pre;
    inject_ret = register_kprobe(&g_touch_v2_input_inject_event_kp);
    if (!inject_ret)
        g_touch_v2_input_inject_event_registered = true;
    else
        bs_warn("register input_inject_event kprobe failed: %d\n",
                inject_ret);

    /* Real panel drivers report through input_event(), so that probe is the
     * required clock for this backend. input_inject_event() is supplemental;
     * accepting it alone would report init success but never drain hardware
     * touch frames on normal devices. */
    if (!g_touch_v2_input_event_registered) {
        if (g_touch_v2_input_inject_event_registered) {
            unregister_kprobe(&g_touch_v2_input_inject_event_kp);
            g_touch_v2_input_inject_event_registered = false;
        }
        return event_ret ? event_ret : -ENOENT;
    }

    bs_info("event-pool hooks ready input_event=%d input_inject_event=%d\n",
            g_touch_v2_input_event_registered ? 1 : 0,
            g_touch_v2_input_inject_event_registered ? 1 : 0);
    return 0;
}

long touch_v2_init(void)
{
    int ret;

    mutex_lock(&g_touch_v2_init_lock);
    if (!g_touch_v2_work_initialized) {
        INIT_WORK(&g_touch_v2_flush_work, touch_v2_flush_workfn);
        g_touch_v2_work_initialized = true;
    }
    ret = touch_v2_register_hooks_locked();
    mutex_unlock(&g_touch_v2_init_lock);
    return ret;
}

long touch_v2_event(void __user *arg)
{
    struct touch_event_req req;
    unsigned long flags;
    int ret;

    if (!arg)
        return -EINVAL;
    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;
    if (req.slot < 0 || req.slot >= TOUCH_V2_MAX_SLOTS)
        return -EINVAL;

    ret = touch_v2_init();
    if (ret < 0)
        return ret;

    spin_lock_irqsave(&g_touch_v2_pool_lock, flags);
    if (req.down)
        touch_v2_queue_down_locked(req.slot, req.x, req.y);
    else
        touch_v2_queue_up_locked(req.slot);
    spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
    return 0;
}

void touch_v2_release_all(void)
{
    unsigned long flags;
    int i;

    spin_lock_irqsave(&g_touch_v2_pool_lock, flags);
    for (i = 0; i < TOUCH_V2_MAX_SLOTS; ++i)
        touch_v2_queue_up_locked(i);
    spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
}

void touch_v2_exit(void)
{
    struct input_dev *dev;
    unsigned long flags;

    mutex_lock(&g_touch_v2_init_lock);
    if (g_touch_v2_input_inject_event_registered) {
        unregister_kprobe(&g_touch_v2_input_inject_event_kp);
        g_touch_v2_input_inject_event_registered = false;
    }
    if (g_touch_v2_input_event_registered) {
        unregister_kprobe(&g_touch_v2_input_event_kp);
        g_touch_v2_input_event_registered = false;
    }
    mutex_unlock(&g_touch_v2_init_lock);

    if (g_touch_v2_work_initialized)
        cancel_work_sync(&g_touch_v2_flush_work);

    spin_lock_irqsave(&g_touch_v2_pool_lock, flags);
    dev = g_touch_v2_deliver_dev;
    g_touch_v2_deliver_dev = NULL;
    g_touch_v2_work_queued = false;
    g_touch_v2_injecting = false;
    g_touch_v2_event_count = 0;
    memset(g_touch_v2_slot_active, 0, sizeof(g_touch_v2_slot_active));
    touch_v2_reset_pos_idx_locked();
    spin_unlock_irqrestore(&g_touch_v2_pool_lock, flags);
    if (dev)
        put_device(&dev->dev);
}
