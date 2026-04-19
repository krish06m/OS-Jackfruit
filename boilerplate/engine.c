#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <limits.h>
#include <ctype.h>
#include <semaphore.h>
#include "monitor_ioctl.h"

/* ─── constants ─────────────────────────────────────────────── */
#define MAX_CONTAINERS   16
#define LOG_BUF_SLOTS    256
#define LOG_SLOT_SIZE    512
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine_logs"
#define MONITOR_DEV      "/dev/container_monitor"

/* ─── container states ───────────────────────────────────────── */
typedef enum {
    ST_EMPTY = 0,
    ST_STARTING,
    ST_RUNNING,
    ST_STOPPING,
    ST_STOPPED,
    ST_EXITED,
    ST_KILLED,
    ST_HARD_LIMIT_KILLED,
    ST_FAILED,
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case ST_EMPTY:    return "empty";
        case ST_STARTING: return "starting";
        case ST_RUNNING:  return "running";
        case ST_STOPPING: return "stopping";
        case ST_STOPPED:  return "stopped";
        case ST_EXITED:   return "exited";
        case ST_KILLED:   return "killed";
        case ST_HARD_LIMIT_KILLED: return "hard_limit_killed";
        case ST_FAILED:   return "failed";
        default:          return "unknown";
    }
}

typedef struct {
    char   id[64];
    char   rootfs[PATH_MAX];
    char   command[1024];
    long   soft_limit;
    long   hard_limit;
    int    nice_value;
} StartRequest;

/* ─── container metadata ─────────────────────────────────────── */
typedef struct {
    int            used;
    char           id[64];
    char           rootfs[PATH_MAX];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_limit;
    long           hard_limit;
    int            nice_value;
    char           log_path[256];
    int            exit_status_raw;
    int            exit_code;
    int            exit_signal;
    int            stop_requested;
    int            completed;
    int            log_pipe[2];   /* [0]=read  [1]=write */
    pthread_t      logger_tid;
    int            logger_started;
    int            logger_joined;
    pthread_mutex_t lock;
    pthread_cond_t finished;
} Container;

static Container containers[MAX_CONTAINERS];
static pthread_mutex_t containers_lock = PTHREAD_MUTEX_INITIALIZER;
static char supervisor_rootfs[PATH_MAX] = {0};

/* ─── bounded log buffer ─────────────────────────────────────── */
typedef struct {
    char   data[LOG_SLOT_SIZE];
    int    len;
    char   log_path[256];
} LogSlot;

typedef struct {
    LogSlot         slots[LOG_BUF_SLOTS];
    int             head, tail, count;
    pthread_mutex_t lock;
    sem_t           empty_slots;
    sem_t           filled_slots;
    int             shutdown;
} LogBuffer;

static LogBuffer logbuf;

/* ─── globals ────────────────────────────────────────────────── */
static volatile sig_atomic_t g_shutdown = 0;
static int monitor_fd = -1;
static int server_fd = -1;
static pthread_t log_consumer_tid;
static pthread_t signal_tid;
static int signal_thread_started = 0;
static sigset_t supervisor_signal_set;

/* ─── forward declarations ───────────────────────────────────── */
static void supervisor_loop(const char *rootfs);
static void handle_cli_command(int client_fd, const char *base_rootfs);
static int  do_start(const StartRequest *req, int foreground);
static void do_ps(int fd);
static void do_logs(int fd, const char *name);
static void do_stop(int fd, const char *name);
static void *log_consumer(void *arg);
static void *container_logger(void *arg);
static void *signal_watcher(void *arg);
static void reap_children(void);
static Container *find_container(const char *id);
static Container *alloc_container(void);
static Container *find_container_by_pid(pid_t pid);
static int container_is_active(const Container *c);
static int rootfs_in_use_locked(const char *rootfs);
static int parse_start_request(char **argv, int argc, const char *base_rootfs,
                               StartRequest *req, char *err, size_t errlen);
static int wait_for_container(Container *c);
static void join_container_logger(Container *c);
static void format_start_time(time_t t, char *buf, size_t len);
static void describe_result(const Container *c, char *buf, size_t len);
static void write_all(int fd, const char *buf);
static void write_status_line(int fd, int status);
static void register_with_monitor(Container *c);
static void unregister_with_monitor(pid_t pid);

/* ══════════════════════════════════════════════════════════════
   SIGNAL HANDLERS
══════════════════════════════════════════════════════════════ */
static void sigterm_handler(int sig) { (void)sig; g_shutdown = 1; }

static int container_is_active(const Container *c)
{
    return c && c->used &&
           (c->state == ST_STARTING || c->state == ST_RUNNING ||
            c->state == ST_STOPPING);
}

static Container *find_container_by_pid(pid_t pid)
{
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].used && containers[i].host_pid == pid)
            return &containers[i];
    }
    return NULL;
}

static int rootfs_in_use_locked(const char *rootfs)
{
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (container_is_active(&containers[i]) &&
            strcmp(containers[i].rootfs, rootfs) == 0) {
            return 1;
        }
    }
    return 0;
}

static void format_start_time(time_t t, char *buf, size_t len)
{
    struct tm tm_info;
    if (!buf || len == 0)
        return;
    if (!localtime_r(&t, &tm_info)) {
        snprintf(buf, len, "-");
        return;
    }
    if (strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_info) == 0)
        snprintf(buf, len, "%ld", (long)t);
}

static void describe_result(const Container *c, char *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (!c || !c->completed) {
        snprintf(buf, len, "-");
        return;
    }

    switch (c->state) {
        case ST_EXITED:
            snprintf(buf, len, "exit=%d", c->exit_code);
            break;
        case ST_STOPPED:
            snprintf(buf, len, "stopped");
            break;
        case ST_HARD_LIMIT_KILLED:
            snprintf(buf, len, "hard_limit_killed");
            break;
        case ST_KILLED:
            snprintf(buf, len, "signal=%d", c->exit_signal);
            break;
        case ST_FAILED:
            snprintf(buf, len, "failed");
            break;
        default:
            snprintf(buf, len, "done");
            break;
    }
}

static void write_all(int fd, const char *buf)
{
    size_t len;

    if (fd < 0 || !buf)
        return;

    len = strlen(buf);
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        buf += n;
        len -= (size_t)n;
    }
}

static void write_status_line(int fd, int status)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%d\n", status);
    write_all(fd, buf);
}

static void copy_cstring(char *dst, size_t dstlen, const char *src)
{
    if (!dst || dstlen == 0)
        return;
    snprintf(dst, dstlen, "%s", src ? src : "");
}

static int is_numeric_token(const char *s)
{
    if (!s || *s == '\0')
        return 0;
    if (*s == '-' || *s == '+')
        s++;
    if (*s == '\0')
        return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s))
            return 0;
        s++;
    }
    return 1;
}

static void close_extra_fds(void)
{
    long max_fd = sysconf(_SC_OPEN_MAX);

    if (max_fd < 0)
        max_fd = 1024;
    if (max_fd > 4096)
        max_fd = 4096;

    for (int fd = 3; fd < max_fd; fd++)
        close(fd);
}

static void update_container_exit(Container *c, pid_t pid, int status)
{
    if (!c || c->host_pid != pid)
        return;

    pthread_mutex_lock(&c->lock);
    c->exit_status_raw = status;
    c->exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    c->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + c->exit_signal);

    if (c->stop_requested) {
        c->state = ST_STOPPED;
    } else if (WIFSIGNALED(status) && c->exit_signal == SIGKILL) {
        c->state = ST_HARD_LIMIT_KILLED;
    } else if (WIFSIGNALED(status)) {
        c->state = ST_KILLED;
    } else {
        c->state = ST_EXITED;
    }
    c->completed = 1;
    pthread_cond_broadcast(&c->finished);
    pthread_mutex_unlock(&c->lock);

    unregister_with_monitor(pid);
}

static void join_container_logger(Container *c)
{
    pthread_t tid;
    int should_join = 0;

    if (!c)
        return;

    pthread_mutex_lock(&c->lock);
    if (c->logger_started && !c->logger_joined) {
        c->logger_joined = 1;
        tid = c->logger_tid;
        should_join = 1;
    }
    pthread_mutex_unlock(&c->lock);

    if (should_join)
        pthread_join(tid, NULL);
}

static void reap_children(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        Container *c;
        pthread_mutex_lock(&containers_lock);
        c = find_container_by_pid(pid);
        if (c)
            update_container_exit(c, pid, status);
        pthread_mutex_unlock(&containers_lock);

        if (c)
            join_container_logger(c);
    }
}

static void *signal_watcher(void *arg)
{
    sigset_t *set = (sigset_t *)arg;

    while (!g_shutdown) {
        siginfo_t info;
        int sig = sigwaitinfo(set, &info);
        if (sig < 0) {
            if (errno == EINTR)
                continue;
            continue;
        }

        if (sig == SIGCHLD) {
            reap_children();
        } else if (sig == SIGINT || sig == SIGTERM) {
            g_shutdown = 1;
            break;
        }
    }

    return NULL;
}

typedef struct {
    int  fd;
    char base_rootfs[PATH_MAX];
} ClientTask;

static void *client_worker(void *arg)
{
    ClientTask *task = (ClientTask *)arg;

    if (task) {
        handle_cli_command(task->fd, task->base_rootfs);
        close(task->fd);
        free(task);
    }

    return NULL;
}

static int parse_start_request(char **argv, int argc, const char *base_rootfs,
                               StartRequest *req, char *err, size_t errlen)
{
    int idx = 2;
    int consumed[64] = {0};
    int any_flags = 0;
    char rootfs_buf[PATH_MAX];

    if (!req || argc < 3) {
        if (err && errlen)
            snprintf(err, errlen, "missing arguments");
        return -1;
    }

    memset(req, 0, sizeof(*req));
    req->soft_limit = 40L * 1024L * 1024L;
    req->hard_limit = 64L * 1024L * 1024L;
    req->nice_value = 0;

    copy_cstring(req->id, sizeof(req->id), argv[1]);

    if (idx < argc && argv[idx][0] != '-' && strlen(argv[idx]) > 0) {
        struct stat st;
        if (stat(argv[idx], &st) == 0 && S_ISDIR(st.st_mode)) {
            copy_cstring(rootfs_buf, sizeof(rootfs_buf), argv[idx]);
            idx++;
        } else {
            copy_cstring(rootfs_buf, sizeof(rootfs_buf), base_rootfs);
        }
    } else {
        copy_cstring(rootfs_buf, sizeof(rootfs_buf), base_rootfs);
    }

    if (rootfs_buf[0] == '\0') {
        if (err && errlen)
            snprintf(err, errlen, "rootfs not specified and no supervisor default");
        return -1;
    }

    for (int i = idx; i < argc; i++) {
        if (strcmp(argv[i], "--soft-mib") == 0 && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
            req->soft_limit = atol(argv[i + 1]) * 1024L * 1024L;
            consumed[i] = consumed[i + 1] = 1;
            any_flags = 1;
            i++;
        } else if (strcmp(argv[i], "--hard-mib") == 0 && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
            req->hard_limit = atol(argv[i + 1]) * 1024L * 1024L;
            consumed[i] = consumed[i + 1] = 1;
            any_flags = 1;
            i++;
        } else if (strcmp(argv[i], "--nice") == 0 && i + 1 < argc &&
                   (isdigit((unsigned char)argv[i + 1][0]) || argv[i + 1][0] == '-')) {
            req->nice_value = atoi(argv[i + 1]);
            consumed[i] = consumed[i + 1] = 1;
            any_flags = 1;
            i++;
        }
    }

    if (!any_flags) {
        int trail = 0;
        int j = argc - 1;

        while (j >= idx && trail < 3 && is_numeric_token(argv[j])) {
            trail++;
            j--;
        }

        if (trail == 2 || trail == 3) {
            req->soft_limit = atol(argv[argc - trail]) * 1024L * 1024L;
            req->hard_limit = atol(argv[argc - trail + 1]) * 1024L * 1024L;
            consumed[argc - trail] = consumed[argc - trail + 1] = 1;
            if (trail == 3) {
                req->nice_value = atoi(argv[argc - 1]);
                consumed[argc - 1] = 1;
            }
        }
    }

    req->rootfs[0] = '\0';
    if (!realpath(rootfs_buf, req->rootfs)) {
        if (err && errlen)
            snprintf(err, errlen, "invalid rootfs");
        return -1;
    }

    for (int i = idx; i < argc; i++) {
        if (consumed[i])
            continue;
        if (req->command[0] != '\0')
            strncat(req->command, " ", sizeof(req->command) - strlen(req->command) - 1);
        strncat(req->command, argv[i], sizeof(req->command) - strlen(req->command) - 1);
    }

    if (req->command[0] == '\0') {
        if (err && errlen)
            snprintf(err, errlen, "missing command");
        return -1;
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   MONITOR HELPERS
══════════════════════════════════════════════════════════════ */
static void register_with_monitor(Container *c)
{
    if (monitor_fd < 0) return;
    struct monitor_register_cmd cmd = {0};
    cmd.pid = c->host_pid;
    cmd.soft_limit_bytes = c->soft_limit;
    cmd.hard_limit_bytes = c->hard_limit;
    copy_cstring(cmd.container_id, sizeof(cmd.container_id), c->id);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &cmd) < 0)
        perror("ioctl MONITOR_REGISTER");
}

static void unregister_with_monitor(pid_t pid)
{
    if (monitor_fd < 0) return;
    struct monitor_unregister_cmd cmd = { .pid = pid };
    ioctl(monitor_fd, MONITOR_UNREGISTER, &cmd);
}

/* ══════════════════════════════════════════════════════════════
   LOG BUFFER  (bounded producer-consumer)
══════════════════════════════════════════════════════════════ */
static void logbuf_init(void)
{
    memset(&logbuf, 0, sizeof(logbuf));
    pthread_mutex_init(&logbuf.lock, NULL);
    sem_init(&logbuf.empty_slots, 0, LOG_BUF_SLOTS);
    sem_init(&logbuf.filled_slots, 0, 0);
}

/* Producer: called from per-container logger threads */
static void logbuf_push(const char *log_path, const char *data, int len)
{
    if (len <= 0) return;
    if (len >= LOG_SLOT_SIZE) len = LOG_SLOT_SIZE - 1;

    while (sem_wait(&logbuf.empty_slots) < 0) {
        if (errno == EINTR)
            continue;
        return;
    }

    pthread_mutex_lock(&logbuf.lock);
    if (logbuf.shutdown) {
        pthread_mutex_unlock(&logbuf.lock);
        sem_post(&logbuf.empty_slots);
        return;
    }

    LogSlot *slot = &logbuf.slots[logbuf.tail];
    memcpy(slot->data, data, len);
    slot->data[len] = '\0';
    slot->len = len;
    copy_cstring(slot->log_path, sizeof(slot->log_path), log_path);

    logbuf.tail = (logbuf.tail + 1) % LOG_BUF_SLOTS;
    logbuf.count++;
    pthread_mutex_unlock(&logbuf.lock);
    sem_post(&logbuf.filled_slots);
}

/* Consumer thread: drains buffer to log files */
static void *log_consumer(void *arg)
{
    (void)arg;
    while (1) {
        while (sem_wait(&logbuf.filled_slots) < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }

        pthread_mutex_lock(&logbuf.lock);
        if (logbuf.count == 0 && logbuf.shutdown) {
            pthread_mutex_unlock(&logbuf.lock);
            break;
        }
        if (logbuf.count == 0) {
            pthread_mutex_unlock(&logbuf.lock);
            continue;
        }
        LogSlot slot = logbuf.slots[logbuf.head];
        logbuf.head = (logbuf.head + 1) % LOG_BUF_SLOTS;
        logbuf.count--;
        pthread_mutex_unlock(&logbuf.lock);

        FILE *f = fopen(slot.log_path, "a");
        if (f) { fwrite(slot.data, 1, slot.len, f); fclose(f); }
        sem_post(&logbuf.empty_slots);
    }
    return NULL;
}

/* Per-container thread: reads pipe, pushes to bounded buffer */
typedef struct { int read_fd; char log_path[256]; } LoggerArg;

static void *container_logger(void *arg)
{
    LoggerArg *la = (LoggerArg *)arg;
    char buf[LOG_SLOT_SIZE];
    ssize_t n;
    while ((n = read(la->read_fd, buf, sizeof(buf) - 1)) > 0)
        logbuf_push(la->log_path, buf, (int)n);
    close(la->read_fd);
    free(la);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
   CONTAINER HELPERS
══════════════════════════════════════════════════════════════ */
static Container *find_container(const char *id)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].used && strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

static Container *alloc_container(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].used) return &containers[i];
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
   CONTAINER LAUNCH  (Tasks 1 + 2 + 3)
══════════════════════════════════════════════════════════════ */
typedef struct {
    const char *rootfs;
    const char *cmd;
    int         pipe_write;
    int         nice_value;
} ChildArg;

/* This runs inside the new namespaces */
static int container_child(void *arg)
{
    ChildArg *ca = (ChildArg *)arg;

    /* Redirect stdout+stderr into the logging pipe */
    if (dup2(ca->pipe_write, STDOUT_FILENO) < 0 ||
        dup2(ca->pipe_write, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(ca->pipe_write);

    close_extra_fds();

    /* Set hostname for the UTS namespace */
    if (sethostname("container", 9) < 0)
        perror("sethostname");

    /* Keep mount events local to this namespace to avoid leaking /proc mounts */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount --make-rprivate /");

    /* chroot into rootfs */
    if (chroot(ca->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/") < 0)         { perror("chdir"); return 1; }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST)
        perror("mkdir /proc");

    /* Mount proc inside the container root */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");

    if (ca->nice_value != 0 && setpriority(PRIO_PROCESS, 0, ca->nice_value) < 0)
        perror("setpriority");

    /* Exec the requested command */
    char *argv[] = { "/bin/sh", "-c", (char *)ca->cmd, NULL };
    execv("/bin/sh", argv);
    perror("execv");
    return 1;
}

#define STACK_SIZE (1024 * 1024)

static int wait_for_container(Container *c)
{
    int status = -1;

    if (!c)
        return -1;

    pthread_mutex_lock(&c->lock);
    while (!c->completed)
        pthread_cond_wait(&c->finished, &c->lock);
    status = c->exit_code;
    pthread_mutex_unlock(&c->lock);

    return status;
}

static int do_start(const StartRequest *req, int foreground)
{
    if (!req)
        return -1;

    pthread_mutex_lock(&containers_lock);
    if (find_container(req->id)) {
        pthread_mutex_unlock(&containers_lock);
        fprintf(stderr, "Container '%s' already exists\n", req->id);
        return -1;
    }
    if (rootfs_in_use_locked(req->rootfs)) {
        pthread_mutex_unlock(&containers_lock);
        fprintf(stderr, "Rootfs '%s' is already in use by a live container\n", req->rootfs);
        return -1;
    }

    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        fprintf(stderr, "Max containers reached\n");
        return -1;
    }

    memset(c, 0, sizeof(*c));
    copy_cstring(c->id, sizeof(c->id), req->id);
    copy_cstring(c->rootfs, sizeof(c->rootfs), req->rootfs);
    c->soft_limit = req->soft_limit;
    c->hard_limit = req->hard_limit;
    c->nice_value = req->nice_value;
    c->state      = ST_STARTING;
    c->used       = 1;
    c->start_time = time(NULL);
    c->stop_requested = 0;
    c->completed = 0;
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->finished, NULL);

    /* Create log file */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, req->id);
    unlink(c->log_path);

    /* Pipe: container stdout/stderr -> supervisor */
    if (pipe(c->log_pipe) < 0) { perror("pipe"); c->used = 0;
        pthread_mutex_unlock(&containers_lock); return -1; }

    /* Clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { c->used = 0; pthread_mutex_unlock(&containers_lock); return -1; }
    char *stack_top = stack + STACK_SIZE;

    ChildArg ca = { .rootfs = c->rootfs, .cmd = req->command,
                    .pipe_write = c->log_pipe[1], .nice_value = req->nice_value };

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_child, stack_top, flags, &ca);
    free(stack);

    if (pid < 0) {
        perror("clone");
        close(c->log_pipe[0]); close(c->log_pipe[1]);
        c->used = 0;
        pthread_mutex_unlock(&containers_lock);
        return -1;
    }

    c->host_pid = pid;
    c->state    = ST_RUNNING;
    close(c->log_pipe[1]); /* supervisor doesn't write */

    /* Start per-container logger thread */
    LoggerArg *la = malloc(sizeof(*la));
    la->read_fd = c->log_pipe[0];
    copy_cstring(la->log_path, sizeof(la->log_path), c->log_path);
    if (pthread_create(&c->logger_tid, NULL, container_logger, la) == 0) {
        c->logger_started = 1;
        c->logger_joined = 0;
    } else {
        free(la);
    }

    /* Register with kernel monitor */
    register_with_monitor(c);

    pthread_mutex_unlock(&containers_lock);

    printf("[engine] Started container '%s' pid=%d log=%s\n", req->id, pid, c->log_path);

    if (foreground) {
        int status = wait_for_container(c);
        if (status < 0)
            return -1;
        return status;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   CLI COMMANDS
══════════════════════════════════════════════════════════════ */
static void do_ps(int fd)
{
    char buf[16384] = {0};
    size_t off = 0;
    char start_buf[64];
    char result_buf[64];
    int wrote = snprintf(buf + off, sizeof(buf) - off,
        "%-16s %-8s %-19s %-10s %-10s %-10s %-6s %-18s %s\n",
        "NAME","PID","STATE","STARTED","SOFT(MB)","HARD(MB)","NICE","RESULT","LOG");
    if (wrote < 0)
        wrote = 0;
    off += (size_t)wrote;

    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (!c->used) continue;
        format_start_time(c->start_time, start_buf, sizeof(start_buf));
        pthread_mutex_lock(&c->lock);
        describe_result(c, result_buf, sizeof(result_buf));
        if (off >= sizeof(buf) - 1) {
            pthread_mutex_unlock(&c->lock);
            break;
        }
        wrote = snprintf(buf + off, sizeof(buf) - off,
            "%-16s %-8d %-19s %-10s %-10ld %-10ld %-6d %-18s %s\n",
            c->id, c->host_pid, state_str(c->state),
            start_buf,
            c->soft_limit / (1024*1024),
            c->hard_limit / (1024*1024),
            c->nice_value,
            result_buf,
            c->log_path);
        if (wrote < 0)
            wrote = 0;
        if ((size_t)wrote >= sizeof(buf) - off)
            off = sizeof(buf) - 1;
        else
            off += (size_t)wrote;
        pthread_mutex_unlock(&c->lock);
    }
    pthread_mutex_unlock(&containers_lock);

    buf[sizeof(buf) - 1] = '\0';
    write_all(fd, buf);
}

static void do_logs(int fd, const char *name)
{
    pthread_mutex_lock(&containers_lock);
    Container *c = find_container(name);
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        const char *msg = "Container not found\n";
        write_all(fd, msg);
        return;
    }
    char path[256];
    copy_cstring(path, sizeof(path), c->log_path);
    pthread_mutex_unlock(&containers_lock);

    FILE *f = fopen(path, "r");
    if (!f) { const char *m = "Log file not found\n"; write_all(fd,m); return; }
    char line[512];
    while (fgets(line, sizeof(line), f))
        write_all(fd, line);
    fclose(f);
}

static void do_stop(int fd, const char *name)
{
    pthread_mutex_lock(&containers_lock);
    Container *c = find_container(name);
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        const char *msg = "Container not found\n";
        write_all(fd, msg);
        return;
    }
    pthread_mutex_lock(&c->lock);
    if (!container_is_active(c)) {
        pthread_mutex_unlock(&c->lock);
        pthread_mutex_unlock(&containers_lock);
        const char *msg = "Container not running\n";
        write_all(fd, msg);
        return;
    }
    pid_t pid = c->host_pid;
    c->stop_requested = 1;
    c->state = ST_STOPPING;
    pthread_mutex_unlock(&c->lock);
    pthread_mutex_unlock(&containers_lock);

    /* Graceful SIGTERM first, then SIGKILL */
    kill(pid, SIGTERM);
    usleep(300000);
    if (kill(pid, 0) == 0)
        kill(pid, SIGKILL);

    const char *msg = "Stopped\n";
    write_all(fd, msg);
}

/* ══════════════════════════════════════════════════════════════
   CLI HANDLER  (runs for each client connection)
══════════════════════════════════════════════════════════════ */
static void handle_cli_command(int cfd, const char *base_rootfs)
{
    char buf[4096] = {0};
    ssize_t n = read(cfd, buf, sizeof(buf)-1);
    if (n <= 0) return;
    buf[n] = '\0';

    /* tokenise */
    char *argv[16]; int argc = 0;
    char *tok = strtok(buf, " \t\n");
    while (tok && argc < 15) { argv[argc++] = tok; tok = strtok(NULL," \t\n"); }
    if (argc == 0) return;

    if (strcmp(argv[0], "ps") == 0) {
        do_ps(cfd);
    } else if (strcmp(argv[0], "logs") == 0 && argc >= 2) {
        do_logs(cfd, argv[1]);
    } else if (strcmp(argv[0], "stop") == 0 && argc >= 2) {
        do_stop(cfd, argv[1]);
    } else if (strcmp(argv[0], "start") == 0 && argc >= 2) {
        StartRequest req;
        char err[256];

        if (parse_start_request(argv, argc, base_rootfs, &req, err, sizeof(err)) < 0) {
            char msg[320];
            snprintf(msg, sizeof(msg), "ERROR: %s\n", err);
            write_all(cfd, msg);
            return;
        }

        if (do_start(&req, 0) < 0) {
            write_all(cfd, "ERROR: failed to start container\n");
            return;
        }

        write_all(cfd, "Container started\n");
    } else if (strcmp(argv[0], "run") == 0 && argc >= 2) {
        StartRequest req;
        char err[256];
        int exit_code;

        if (parse_start_request(argv, argc, base_rootfs, &req, err, sizeof(err)) < 0) {
            char msg[320];
            snprintf(msg, sizeof(msg), "ERROR: %s\n", err);
            write_all(cfd, msg);
            return;
        }

        exit_code = do_start(&req, 1);
        if (exit_code < 0) {
            write_all(cfd, "ERROR: failed to run container\n");
            return;
        }
        write_status_line(cfd, exit_code);
    } else {
        const char *msg = "Unknown command\n";
        write_all(cfd, msg);
    }
}

/* ══════════════════════════════════════════════════════════════
   SUPERVISOR LOOP
══════════════════════════════════════════════════════════════ */
static void shutdown_supervisor(void)
{
    int status;
    pid_t pid;

    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (!container_is_active(c))
            continue;
        pthread_mutex_lock(&c->lock);
        c->stop_requested = 1;
        c->state = ST_STOPPING;
        pid = c->host_pid;
        pthread_mutex_unlock(&c->lock);
        kill(pid, SIGTERM);
    }
    pthread_mutex_unlock(&containers_lock);

    usleep(300000);

    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (!container_is_active(c))
            continue;
        if (kill(c->host_pid, 0) == 0)
            kill(c->host_pid, SIGKILL);
    }
    pthread_mutex_unlock(&containers_lock);

    while ((pid = waitpid(-1, &status, 0)) > 0) {
        pthread_mutex_lock(&containers_lock);
        Container *c = find_container_by_pid(pid);
        if (c)
            update_container_exit(c, pid, status);
        pthread_mutex_unlock(&containers_lock);
    }

    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].used)
            join_container_logger(&containers[i]);
    }
    pthread_mutex_unlock(&containers_lock);

    pthread_mutex_lock(&logbuf.lock);
    logbuf.shutdown = 1;
    pthread_mutex_unlock(&logbuf.lock);

    sem_post(&logbuf.filled_slots);
    for (int i = 0; i < LOG_BUF_SLOTS; i++)
        sem_post(&logbuf.empty_slots);

    pthread_join(log_consumer_tid, NULL);
}

static void supervisor_loop(const char *rootfs)
{
    ClientTask *task;

    if (rootfs && *rootfs)
        copy_cstring(supervisor_rootfs, sizeof(supervisor_rootfs), rootfs);

    /* UNIX domain socket for CLI */
    unlink(SOCK_PATH);
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    copy_cstring(addr.sun_path, sizeof(addr.sun_path), SOCK_PATH);
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 8);
    chmod(SOCK_PATH, 0777);

    printf("[engine] Supervisor ready. Socket: %s\n", SOCK_PATH);

    /* Non-blocking accept loop */
    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    while (!g_shutdown) {
        int cfd = accept(server_fd, NULL, NULL);
        if (cfd >= 0) {
            task = calloc(1, sizeof(*task));
            if (!task) {
                close(cfd);
                continue;
            }
            task->fd = cfd;
            if (rootfs)
                copy_cstring(task->base_rootfs, sizeof(task->base_rootfs), rootfs);
            pthread_t tid;
            if (pthread_create(&tid, NULL, client_worker, task) == 0)
                pthread_detach(tid);
            else {
                close(cfd);
                free(task);
            }
        } else {
            usleep(50000); /* 50ms poll */
        }
    }

    /* Orderly shutdown */
    printf("[engine] Shutting down...\n");

    if (signal_thread_started)
        pthread_join(signal_tid, NULL);

    shutdown_supervisor();

    close(server_fd);
    unlink(SOCK_PATH);
    printf("[engine] Clean exit.\n");
}

static volatile sig_atomic_t client_forward_stop = 0;
static volatile sig_atomic_t client_stop_sent = 0;
static char client_run_id[64] = {0};

static void client_signal_handler(int sig)
{
    (void)sig;
    client_forward_stop = 1;
}

static int send_control_command(const char *command, char *response, size_t response_len)
{
    int sock;
    struct sockaddr_un addr = {0};
    ssize_t n;

    if (!command)
        return -1;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    addr.sun_family = AF_UNIX;
    copy_cstring(addr.sun_path, sizeof(addr.sun_path), SOCK_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    write_all(sock, command);

    if (response && response_len > 0) {
        size_t off = 0;
        while ((n = read(sock, response + off, response_len - off - 1)) > 0) {
            off += (size_t)n;
            if (off + 1 >= response_len)
                break;
        }
        response[off] = '\0';
    }

    close(sock);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   MAIN  — dispatcher for supervisor vs CLI client
══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  sudo %s supervisor <base-rootfs>\n"
            "  sudo %s start  <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  sudo %s run    <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  sudo %s ps\n"
            "  sudo %s logs   <id>\n"
            "  sudo %s stop   <id>\n", argv[0],argv[0],argv[0],argv[0],argv[0],argv[0]);
        return 1;
    }

    /* ── supervisor mode ── */
    if (strcmp(argv[1], "supervisor") == 0) {
        const char *rootfs = (argc >= 3) ? argv[2] : "./rootfs";
        struct sigaction sa = {0};

        sigemptyset(&supervisor_signal_set);
        sigaddset(&supervisor_signal_set, SIGCHLD);
        sigaddset(&supervisor_signal_set, SIGINT);
        sigaddset(&supervisor_signal_set, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &supervisor_signal_set, NULL);

        sa.sa_handler = sigterm_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &(struct sigaction){ .sa_handler = SIG_IGN }, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        logbuf_init();
        pthread_create(&log_consumer_tid, NULL, log_consumer, NULL);

        if (pthread_create(&signal_tid, NULL, signal_watcher, &supervisor_signal_set) == 0)
            signal_thread_started = 1;

        monitor_fd = open(MONITOR_DEV, O_RDWR);
        if (monitor_fd < 0)
            fprintf(stderr, "[engine] Warning: cannot open %s (kernel module loaded?)\n",
                    MONITOR_DEV);

        supervisor_loop(rootfs);

        if (monitor_fd >= 0) close(monitor_fd);
        return 0;
    }

    /* ── CLI client mode — forward command to supervisor ── */
    char cmd[2048] = {0};
    int exit_code = 0;

    for (int i = 1; i < argc; i++) {
        strncat(cmd, argv[i], sizeof(cmd)-strlen(cmd)-2);
        if (i < argc-1) strncat(cmd, " ", sizeof(cmd)-strlen(cmd)-1);
    }

    if (strcmp(argv[1], "run") == 0 && argc >= 3) {
        struct sigaction sa = {0};
        char response[4096] = {0};
        int sock;
        struct sockaddr_un addr = {0};
        size_t off = 0;
        ssize_t n;

        copy_cstring(client_run_id, sizeof(client_run_id), argv[2]);
        sa.sa_handler = client_signal_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGPIPE, &(struct sigaction){ .sa_handler = SIG_IGN }, NULL);

        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            return 1;
        }

        addr.sun_family = AF_UNIX;
        copy_cstring(addr.sun_path, sizeof(addr.sun_path), SOCK_PATH);

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect (is supervisor running?)");
            close(sock);
            return 1;
        }

        write_all(sock, cmd);

        while (1) {
            n = read(sock, response + off, sizeof(response) - off - 1);
            if (n > 0) {
                off += (size_t)n;
                if (off + 1 >= sizeof(response))
                    break;
                continue;
            }
            if (n == 0)
                break;
            if (errno == EINTR) {
                if (client_forward_stop && !client_stop_sent) {
                    char stop_cmd[128];
                    snprintf(stop_cmd, sizeof(stop_cmd), "stop %s", client_run_id);
                    send_control_command(stop_cmd, NULL, 0);
                    client_stop_sent = 1;
                }
                continue;
            }
            perror("read");
            close(sock);
            return 1;
        }

        close(sock);
        response[off] = '\0';
        if (response[0] == '\0')
            return 0;

        printf("%s", response);
        if (strncmp(response, "ERROR", 5) == 0)
            return 1;
        exit_code = atoi(response);
        return exit_code;
    }

    {
        char response[262144] = {0};

        if (send_control_command(cmd, response, sizeof(response)) < 0) {
            perror("connect (is supervisor running?)");
            return 1;
        }
        if (response[0] != '\0')
            printf("%s", response);
        if (strncmp(response, "ERROR", 5) == 0)
            return 1;
    }
    return 0;
}
