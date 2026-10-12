/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Parent-death watchdog for cdj-gui-run, as patches/15 adds to bin/cdj-run:
 * the launcher keeps the write end of a pipe and passes the read end as
 * CDJ_PARENT_FD; read() returning 0 means the launcher is gone, even after
 * SIGKILL, so exit rather than run on as an orphan. Unset means no watchdog.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static void *watch(void *arg)
{
    char buf[64];
    ssize_t n;
    do
        n = read((int)(long)arg, buf, sizeof buf);
    while (n > 0 || (n < 0 && errno == EINTR));
    _exit(0);
}

__attribute__((constructor)) static void watch_init(void)
{
    const char *env = getenv("CDJ_PARENT_FD");
    char *end;
    long fd;
    pthread_t thread;
    if (!env || !*env)
        return;
    fd = strtol(env, &end, 10);
    if (*end || fd < 3 || fcntl((int)fd, F_GETFD) < 0)
        return;
    if (pthread_create(&thread, NULL, watch, (void *)fd) == 0)
        pthread_detach(thread);
}
