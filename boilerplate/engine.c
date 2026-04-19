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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sched.h>
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
    ST_STOPPED,
    ST_KILLED,
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case ST_EMPTY:    return "empty";
        case ST_STARTING: return "starting";
        case ST_RUNNING:  return "running";
        case ST_STOPPED:  return "stopped";
        case ST_KILLED:   return "killed";
        default:          return "unknown";
    }
}

/* ─── container metadata ─────────────────────────────────────── */
typedef struct {
    int            used;
    char           id[64];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_limit;
    long           hard_limit;
    char           log_path[256];
    int            exit_status;
    int            log_pipe[2];   /* [0]=read  [1]=write */
    pthread_mutex_t lock;
} Container;

static Container containers[MAX_CONTAINERS];
static pthread_mutex_t containers_lock = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int             shutdown;
} LogBuffer;

static LogBuffer logbuf;

/* ─── globals ────────────────────────────────────────────────── */
static volatile sig_atomic_t g_shutdown = 0;
static int monitor_fd = -1;
static pthread_t log_consumer_tid;

/* ─── forward declarations ───────────────────────────────────── */
static void supervisor_loop(const char *rootfs);
static void handle_cli_command(int client_fd, const char *rootfs);
static int  do_start(const char *name, const char *rootfs,
                     const char *cmd, long soft, long hard, int foreground);
static void do_ps(int fd);
static void do_logs(int fd, const char *name);
static void do_stop(int fd, const char *name);
static void *log_consumer(void *arg);
static void *container_logger(void *arg);
static void reap_children(void);
static Container *find_container(const char *id);
static Container *alloc_container(void);
static void register_with_monitor(Container *c);
static void unregister_with_monitor(pid_t pid);

/* ══════════════════════════════════════════════════════════════
   SIGNAL HANDLERS
══════════════════════════════════════════════════════════════ */
static void sigchld_handler(int sig) { (void)sig; reap_children(); }
static void sigterm_handler(int sig) { (void)sig; g_shutdown = 1; }

static void reap_children(void)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].used && containers[i].host_pid == pid) {
                pthread_mutex_lock(&containers[i].lock);
                if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                    containers[i].state = ST_KILLED;
                else
                    containers[i].state = ST_STOPPED;
                containers[i].exit_status = status;
                pthread_mutex_unlock(&containers[i].lock);
                unregister_with_monitor(pid);
                break;
            }
        }
        pthread_mutex_unlock(&containers_lock);
    }
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
    strncpy(cmd.container_id, c->id, CONTAINER_ID_MAX - 1);
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
    pthread_cond_init(&logbuf.not_empty, NULL);
    pthread_cond_init(&logbuf.not_full, NULL);
}

/* Producer: called from per-container logger threads */
static void logbuf_push(const char *log_path, const char *data, int len)
{
    if (len <= 0) return;
    if (len >= LOG_SLOT_SIZE) len = LOG_SLOT_SIZE - 1;

    pthread_mutex_lock(&logbuf.lock);
    while (logbuf.count == LOG_BUF_SLOTS && !logbuf.shutdown)
        pthread_cond_wait(&logbuf.not_full, &logbuf.lock);
    if (logbuf.shutdown) { pthread_mutex_unlock(&logbuf.lock); return; }

    LogSlot *slot = &logbuf.slots[logbuf.tail];
    memcpy(slot->data, data, len);
    slot->data[len] = '\0';
    slot->len = len;
    strncpy(slot->log_path, log_path, 255);

    logbuf.tail = (logbuf.tail + 1) % LOG_BUF_SLOTS;
    logbuf.count++;
    pthread_cond_signal(&logbuf.not_empty);
    pthread_mutex_unlock(&logbuf.lock);
}

/* Consumer thread: drains buffer to log files */
static void *log_consumer(void *arg)
{
    (void)arg;
    while (1) {
        pthread_mutex_lock(&logbuf.lock);
        while (logbuf.count == 0 && !logbuf.shutdown)
            pthread_cond_wait(&logbuf.not_empty, &logbuf.lock);
        if (logbuf.count == 0 && logbuf.shutdown) {
            pthread_mutex_unlock(&logbuf.lock);
            break;
        }
        LogSlot slot = logbuf.slots[logbuf.head];
        logbuf.head = (logbuf.head + 1) % LOG_BUF_SLOTS;
        logbuf.count--;
        pthread_cond_signal(&logbuf.not_full);
        pthread_mutex_unlock(&logbuf.lock);

        FILE *f = fopen(slot.log_path, "a");
        if (f) { fwrite(slot.data, 1, slot.len, f); fclose(f); }
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
} ChildArg;

/* This runs inside the new namespaces */
static int container_child(void *arg)
{
    ChildArg *ca = (ChildArg *)arg;

    /* Redirect stdout+stderr into the logging pipe */
    dup2(ca->pipe_write, STDOUT_FILENO);
    dup2(ca->pipe_write, STDERR_FILENO);
    close(ca->pipe_write);

    /* Mount proc */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");

    /* chroot into rootfs */
    if (chroot(ca->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/") < 0)         { perror("chdir"); return 1; }

    /* Set hostname to something unique */
    sethostname("container", 9);

    /* Exec the requested command */
    char *argv[] = { "/bin/sh", "-c", (char *)ca->cmd, NULL };
    execv("/bin/sh", argv);
    perror("execv");
    return 1;
}

#define STACK_SIZE (1024 * 1024)

static int do_start(const char *name, const char *rootfs,
                    const char *cmd, long soft, long hard, int foreground)
{
    pthread_mutex_lock(&containers_lock);
    if (find_container(name)) {
        pthread_mutex_unlock(&containers_lock);
        fprintf(stderr, "Container '%s' already exists\n", name);
        return -1;
    }
    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        fprintf(stderr, "Max containers reached\n");
        return -1;
    }

    memset(c, 0, sizeof(*c));
    strncpy(c->id, name, 63);
    c->soft_limit = soft;
    c->hard_limit = hard;
    c->state      = ST_STARTING;
    c->used       = 1;
    c->start_time = time(NULL);
    pthread_mutex_init(&c->lock, NULL);

    /* Create log file */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, name);

    /* Pipe: container stdout/stderr -> supervisor */
    if (pipe(c->log_pipe) < 0) { perror("pipe"); c->used = 0;
        pthread_mutex_unlock(&containers_lock); return -1; }

    /* Clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { c->used = 0; pthread_mutex_unlock(&containers_lock); return -1; }
    char *stack_top = stack + STACK_SIZE;

    ChildArg ca = { .rootfs = rootfs, .cmd = cmd, .pipe_write = c->log_pipe[1] };

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
    strncpy(la->log_path, c->log_path, 255);
    pthread_t tid;
    pthread_create(&tid, NULL, container_logger, la);
    pthread_detach(tid);

    /* Register with kernel monitor */
    register_with_monitor(c);

    pthread_mutex_unlock(&containers_lock);

    printf("[engine] Started container '%s' pid=%d log=%s\n", name, pid, c->log_path);

    if (foreground) {
        int status;
        waitpid(pid, &status, 0);
        pthread_mutex_lock(&containers_lock);
        c->state = ST_STOPPED;
        c->exit_status = status;
        pthread_mutex_unlock(&containers_lock);
        unregister_with_monitor(pid);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   CLI COMMANDS
══════════════════════════════════════════════════════════════ */
static void do_ps(int fd)
{
    char buf[4096] = {0};
    int  off = 0;
    off += snprintf(buf+off, sizeof(buf)-off,
        "%-16s %-8s %-10s %-10s %-10s %s\n",
        "NAME","PID","STATE","SOFT(MB)","HARD(MB)","LOG");

    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &containers[i];
        if (!c->used) continue;
        off += snprintf(buf+off, sizeof(buf)-off,
            "%-16s %-8d %-10s %-10ld %-10ld %s\n",
            c->id, c->host_pid, state_str(c->state),
            c->soft_limit / (1024*1024),
            c->hard_limit / (1024*1024),
            c->log_path);
    }
    pthread_mutex_unlock(&containers_lock);

    write(fd, buf, strlen(buf));
}

static void do_logs(int fd, const char *name)
{
    pthread_mutex_lock(&containers_lock);
    Container *c = find_container(name);
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        const char *msg = "Container not found\n";
        write(fd, msg, strlen(msg));
        return;
    }
    char path[256];
    strncpy(path, c->log_path, 255);
    pthread_mutex_unlock(&containers_lock);

    FILE *f = fopen(path, "r");
    if (!f) { const char *m = "Log file not found\n"; write(fd,m,strlen(m)); return; }
    char line[512];
    while (fgets(line, sizeof(line), f))
        write(fd, line, strlen(line));
    fclose(f);
}

static void do_stop(int fd, const char *name)
{
    pthread_mutex_lock(&containers_lock);
    Container *c = find_container(name);
    if (!c) {
        pthread_mutex_unlock(&containers_lock);
        const char *msg = "Container not found\n";
        write(fd, msg, strlen(msg));
        return;
    }
    if (c->state != ST_RUNNING) {
        pthread_mutex_unlock(&containers_lock);
        const char *msg = "Container not running\n";
        write(fd, msg, strlen(msg));
        return;
    }
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&containers_lock);

    /* Graceful SIGTERM first, then SIGKILL */
    kill(pid, SIGTERM);
    usleep(300000);
    if (waitpid(pid, NULL, WNOHANG) == 0)
        kill(pid, SIGKILL);

    const char *msg = "Stopped\n";
    write(fd, msg, strlen(msg));
}

/* ══════════════════════════════════════════════════════════════
   CLI HANDLER  (runs for each client connection)
══════════════════════════════════════════════════════════════ */
static void handle_cli_command(int cfd, const char *rootfs)
{
    char buf[1024] = {0};
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
        const char *name = argv[1];
        const char *cmd  = (argc >= 3) ? argv[2] : "/bin/sh";
        long soft = (argc >= 4) ? atol(argv[3]) * 1024*1024 : 64  *1024*1024;
        long hard = (argc >= 5) ? atol(argv[4]) * 1024*1024 : 128 *1024*1024;
        do_start(name, rootfs, cmd, soft, hard, 0);
        const char *msg = "Container started\n";
        write(cfd, msg, strlen(msg));
    } else if (strcmp(argv[0], "run") == 0 && argc >= 2) {
        const char *name = argv[1];
        const char *cmd  = (argc >= 3) ? argv[2] : "/bin/sh";
        long soft = 64*1024*1024, hard = 128*1024*1024;
        do_start(name, rootfs, cmd, soft, hard, 1);
        const char *msg = "Container finished\n";
        write(cfd, msg, strlen(msg));
    } else {
        const char *msg = "Unknown command\n";
        write(cfd, msg, strlen(msg));
    }
}

/* ══════════════════════════════════════════════════════════════
   SUPERVISOR LOOP
══════════════════════════════════════════════════════════════ */
static void supervisor_loop(const char *rootfs)
{
    /* UNIX domain socket for CLI */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv, 8);
    chmod(SOCK_PATH, 0777);

    printf("[engine] Supervisor ready. Socket: %s\n", SOCK_PATH);

    /* Non-blocking accept loop */
    fcntl(srv, F_SETFL, O_NONBLOCK);

    while (!g_shutdown) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd >= 0) {
            handle_cli_command(cfd, rootfs);
            close(cfd);
        } else {
            usleep(50000); /* 50ms poll */
        }
    }

    /* Orderly shutdown */
    printf("[engine] Shutting down...\n");

    pthread_mutex_lock(&containers_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].used && containers[i].state == ST_RUNNING) {
            kill(containers[i].host_pid, SIGTERM);
            usleep(200000);
            kill(containers[i].host_pid, SIGKILL);
            waitpid(containers[i].host_pid, NULL, WNOHANG);
        }
    }
    pthread_mutex_unlock(&containers_lock);

    /* Stop log consumer */
    pthread_mutex_lock(&logbuf.lock);
    logbuf.shutdown = 1;
    pthread_cond_broadcast(&logbuf.not_empty);
    pthread_mutex_unlock(&logbuf.lock);
    pthread_join(log_consumer_tid, NULL);

    close(srv);
    unlink(SOCK_PATH);
    printf("[engine] Clean exit.\n");
}

/* ══════════════════════════════════════════════════════════════
   MAIN  — dispatcher for supervisor vs CLI client
══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  sudo %s supervisor <rootfs>\n"
            "  sudo %s start  <name> [cmd] [soft_mb] [hard_mb]\n"
            "  sudo %s run    <name> [cmd]\n"
            "  sudo %s ps\n"
            "  sudo %s logs   <name>\n"
            "  sudo %s stop   <name>\n", argv[0],argv[0],argv[0],argv[0],argv[0],argv[0]);
        return 1;
    }

    /* ── supervisor mode ── */
    if (strcmp(argv[1], "supervisor") == 0) {
        const char *rootfs = (argc >= 3) ? argv[2] : "./rootfs";

        signal(SIGCHLD, sigchld_handler);
        signal(SIGTERM, sigterm_handler);
        signal(SIGINT,  sigterm_handler);

        logbuf_init();
        pthread_create(&log_consumer_tid, NULL, log_consumer, NULL);

        monitor_fd = open(MONITOR_DEV, O_RDWR);
        if (monitor_fd < 0)
            fprintf(stderr, "[engine] Warning: cannot open %s (kernel module loaded?)\n",
                    MONITOR_DEV);

        supervisor_loop(rootfs);

        if (monitor_fd >= 0) close(monitor_fd);
        return 0;
    }

    /* ── CLI client mode — forward command to supervisor ── */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        return 1;
    }

    /* Build command string from remaining args */
    char cmd[1024] = {0};
    for (int i = 1; i < argc; i++) {
        strncat(cmd, argv[i], sizeof(cmd)-strlen(cmd)-2);
        if (i < argc-1) strncat(cmd, " ", sizeof(cmd)-strlen(cmd)-1);
    }
    write(sock, cmd, strlen(cmd));

    /* Print supervisor response */
    char buf[4096];
    ssize_t n;
    while ((n = read(sock, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(sock);
    return 0;
}
