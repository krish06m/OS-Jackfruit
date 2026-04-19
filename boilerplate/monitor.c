#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/miscdevice.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit");
MODULE_DESCRIPTION("Container Memory Monitor");

#define DEVICE_NAME      "container_monitor"
#define CHECK_INTERVAL_MS 2000

/* Per-container tracking entry */
struct container_entry {
    pid_t  pid;
    char   container_id[CONTAINER_ID_MAX];
    long   soft_limit_bytes;
    long   hard_limit_bytes;
    int    soft_warned;
    struct list_head list;
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(container_mutex);
static struct timer_list check_timer;

/* Read RSS in bytes for a given PID */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    mm = get_task_mm(task);
    rcu_read_unlock();

    if (!mm)
        return 0;

    rss = (long)get_mm_rss(mm) << PAGE_SHIFT;
    mmput(mm);
    return rss;
}

/* Send SIGKILL to a PID */
static void kill_process(pid_t pid, const char *container_id)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        pr_warn("container_monitor: [HARD LIMIT] killing container '%s' pid=%d\n",
                container_id, pid);
        send_sig(SIGKILL, task, 0);
    }
    rcu_read_unlock();
}

/* Periodic check callback */
static void check_all_containers(struct timer_list *t)
{
    struct container_entry *entry, *tmp;
    long rss;

    mutex_lock(&container_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        rss = get_rss_bytes(entry->pid);

        if (rss < 0) {
            /* Process is gone, remove stale entry */
            pr_info("container_monitor: container '%s' pid=%d exited, removing\n",
                    entry->container_id, entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->hard_limit_bytes > 0 && rss > entry->hard_limit_bytes) {
            pr_warn("container_monitor: [HARD LIMIT] container '%s' pid=%d rss=%ld > hard=%ld\n",
                    entry->container_id, entry->pid, rss, entry->hard_limit_bytes);
            kill_process(entry->pid, entry->container_id);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->soft_limit_bytes > 0 && rss > entry->soft_limit_bytes
            && !entry->soft_warned) {
            pr_warn("container_monitor: [SOFT LIMIT] container '%s' pid=%d rss=%ld > soft=%ld\n",
                    entry->container_id, entry->pid, rss, entry->soft_limit_bytes);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&container_mutex);

    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ioctl handler */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {

    case MONITOR_REGISTER: {
        struct monitor_register_cmd reg;
        struct container_entry *entry;

        if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
            return -EFAULT;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid              = reg.pid;
        entry->soft_limit_bytes = reg.soft_limit_bytes;
        entry->hard_limit_bytes = reg.hard_limit_bytes;
        entry->soft_warned      = 0;
        strncpy(entry->container_id, reg.container_id, CONTAINER_ID_MAX - 1);
        entry->container_id[CONTAINER_ID_MAX - 1] = '\0';
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&container_mutex);
        list_add_tail(&entry->list, &container_list);
        mutex_unlock(&container_mutex);

        pr_info("container_monitor: registered '%s' pid=%d soft=%ld hard=%ld\n",
                entry->container_id, entry->pid,
                entry->soft_limit_bytes, entry->hard_limit_bytes);
        return 0;
    }

    case MONITOR_UNREGISTER: {
        struct monitor_unregister_cmd unreg;
        struct container_entry *entry, *tmp;

        if (copy_from_user(&unreg, (void __user *)arg, sizeof(unreg)))
            return -EFAULT;

        mutex_lock(&container_mutex);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == unreg.pid) {
                list_del(&entry->list);
                kfree(entry);
                pr_info("container_monitor: unregistered pid=%d\n", unreg.pid);
                break;
            }
        }
        mutex_unlock(&container_mutex);
        return 0;
    }

    case MONITOR_QUERY: {
        struct monitor_query_cmd qry;
        struct container_entry *entry;
        long rss = -1;
        int found = 0;

        if (copy_from_user(&qry, (void __user *)arg, sizeof(qry)))
            return -EFAULT;

        mutex_lock(&container_mutex);
        list_for_each_entry(entry, &container_list, list) {
            if (entry->pid == qry.pid) {
                found = 1;
                rss = get_rss_bytes(entry->pid);
                qry.current_rss_bytes   = (rss >= 0) ? rss : 0;
                qry.soft_limit_exceeded = entry->soft_warned;
                qry.hard_limit_exceeded = 0;
                qry.alive               = (rss >= 0) ? 1 : 0;
                break;
            }
        }
        mutex_unlock(&container_mutex);

        if (!found)
            return -ENOENT;

        if (copy_to_user((void __user *)arg, &qry, sizeof(qry)))
            return -EFAULT;
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static struct miscdevice monitor_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &monitor_fops,
};

static int __init monitor_init(void)
{
    int ret;

    ret = misc_register(&monitor_misc);
    if (ret) {
        pr_err("container_monitor: failed to register device (%d)\n", ret);
        return ret;
    }

    timer_setup(&check_timer, check_all_containers, 0);
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    pr_info("container_monitor: loaded, /dev/%s ready\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_entry *entry, *tmp;

    del_timer_sync(&check_timer);

    mutex_lock(&container_mutex);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&container_mutex);

    misc_deregister(&monitor_misc);
    pr_info("container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
