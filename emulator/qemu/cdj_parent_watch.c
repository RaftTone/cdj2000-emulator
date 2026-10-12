// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Parent-death watchdog.  The launcher holds the write end of a pipe and hands
 * the read end to this process as CDJ_PARENT_FD; the launcher never writes, so
 * read() returns 0 exactly when every holder of the write end is gone, which
 * includes SIGKILL of the launcher.  Without it a killed launcher leaves this
 * process running (and writing DSP checkpoints) until the disk is full.
 * CDJ_PARENT_FD unset means no watchdog.
 *
 * On EOF we ask for the normal shutdown, then force the exit if the main loop
 * has not got there within a few seconds.
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/module.h"
#include "system/runstate.h"

static void *cdj_parent_watch_thread(void *opaque)
{
    int fd = GPOINTER_TO_INT(opaque);
    char buf[64];
    ssize_t n;

    do {
        n = read(fd, buf, sizeof(buf));
    } while (n > 0 || (n < 0 && errno == EINTR));

    fprintf(stderr, "cdj: launcher pipe closed, shutting down\n");
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
    sleep(5);
    _exit(1);
    return NULL;
}

static void cdj_parent_watch_init(void)
{
    const char *env = getenv("CDJ_PARENT_FD");
    static QemuThread thread;
    char *end;
    long fd;

    if (!env || !*env) {
        return;
    }
    fd = strtol(env, &end, 10);
    if (*end || fd < 3 || fcntl(fd, F_GETFD) < 0) {
        return;
    }
    qemu_thread_create(&thread, "cdj-parent-watch", cdj_parent_watch_thread,
                       GINT_TO_POINTER((int)fd), QEMU_THREAD_DETACHED);
}
type_init(cdj_parent_watch_init)
