// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static pid_t g_child;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_child > 0) kill(g_child, SIGTERM);
}

static void runtime_path(const char *suffix, char *out, size_t n)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sid = getenv("XDG_SESSION_ID");
    const char *wd = getenv("WAYLAND_DISPLAY");
    char safe[96] = "default";
    if (wd && *wd) {
        size_t j = 0;
        if (sid && *sid) {
            for (const char *p = sid; *p && j + 1 < sizeof(safe); p++)
                safe[j++] = (*p == '/' || *p == ' ') ? '_' : *p;
            if (j + 1 < sizeof(safe)) safe[j++] = '-';
        }
        for (const char *p = wd; *p && j + 1 < sizeof(safe); p++)
            safe[j++] = (*p == '/' || *p == ' ') ? '_' : *p;
        safe[j] = 0;
    }
    snprintf(out, n, "%s/armdeck-osk/osk-%s.%s",
             runtime && *runtime ? runtime : "/tmp", safe, suffix);
}

static void log_line(const char *text)
{
    char path[700];
    runtime_path("supervisor.log", path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    dprintf(fd, "%s %s\n", ts, text);
    close(fd);
}

static void stop_existing(void)
{
    char path[700];
    runtime_path("pid", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    long pid = 0;
    if (fscanf(f, "%ld", &pid) != 1) { fclose(f); return; }
    fclose(f);
    if (pid <= 1) return;
    char proc[64], cmd[512] = "";
    snprintf(proc, sizeof(proc), "/proc/%ld/cmdline", pid);
    FILE *c = fopen(proc, "r");
    if (c) {
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, c);
        cmd[n] = 0;
        fclose(c);
    }
    if (!strstr(cmd, "armdeck-osk")) { unlink(path); return; }
    kill((pid_t)pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (access(proc, F_OK) != 0) break;
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }
    kill((pid_t)pid, SIGKILL);
    unlink(path);
}

static void ensure_runtime_dir(void)
{
    char path[700];
    runtime_path("lock", path, sizeof(path));
    char *slash = strrchr(path, '/');
    if (slash) { *slash = 0; mkdir(path, 0700); }
}

int main(void)
{
    ensure_runtime_dir();
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    char lock_path[700];
    runtime_path("supervisor.lock", lock_path, sizeof(lock_path));
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0) return 1;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(lock_fd);
        return 0;
    }

    stop_existing();
    int backoff = 1;
    while (!g_stop) {
        char vpath[700];
        runtime_path("visible", vpath, sizeof(vpath));
        unlink(vpath);
        log_line("starting armdeck-osk");
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execl("/usr/bin/armdeck-osk", "armdeck-osk", (char *)NULL);
            _exit(127);
        }
        if (pid < 0) {
            log_line("fork failed");
            sleep(backoff);
            continue;
        }
        g_child = pid;
        time_t started = time(NULL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        g_child = 0;
        if (g_stop) break;
        int secs = (int)(time(NULL) - started);
        char msg[160];
        if (WIFEXITED(status))
            snprintf(msg, sizeof(msg), "armdeck-osk exited code=%d after %ds",
                     WEXITSTATUS(status), secs);
        else if (WIFSIGNALED(status))
            snprintf(msg, sizeof(msg), "armdeck-osk killed signal=%d after %ds",
                     WTERMSIG(status), secs);
        else
            snprintf(msg, sizeof(msg), "armdeck-osk exited after %ds", secs);
        log_line(msg);
        if (secs >= 5) backoff = 1;
        sleep((unsigned)backoff);
        if (backoff < 5) backoff++;
    }
    if (g_child > 0) {
        kill(g_child, SIGTERM);
        waitpid(g_child, NULL, 0);
    }
    close(lock_fd);
    return 0;
}
