#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

/* Register a container PID with soft and hard memory limits (in bytes) */
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_register_cmd)
/* Unregister a container by PID */
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_unregister_cmd)
/* Query status of a container by PID */
#define MONITOR_QUERY      _IOWR(MONITOR_MAGIC, 3, struct monitor_query_cmd)

#define CONTAINER_ID_MAX 64

struct monitor_register_cmd {
    pid_t  pid;
    char   container_id[CONTAINER_ID_MAX];
    long   soft_limit_bytes;
    long   hard_limit_bytes;
};

struct monitor_unregister_cmd {
    pid_t pid;
};

struct monitor_query_cmd {
    pid_t  pid;
    long   current_rss_bytes;
    int    soft_limit_exceeded;
    int    hard_limit_exceeded;
    int    alive;
};

#endif /* MONITOR_IOCTL_H */
