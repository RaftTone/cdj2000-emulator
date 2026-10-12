/*
 * Small helpers the C6727 port shares, from cdj-nxs2-qemu (Stijn Jacobs),
 * hw/cdj/common/cdj_env.c and sh4_periph.c at eea1e0f. Its cached getenv()
 * (CDJ_ENVCACHE, for msvcrt) is left out: plain getenv() is used.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/notify.h"
#include "system/system.h"
#include "migration/qemu-file-types.h"
#include "cdj_c6727.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

bool cdj_report_enabled(void)
{
    const char *on = getenv("CDJ_REPORT");

    return on && strcmp(on, "0");
}

/*
 * Apple Silicon schedules a thread at the default QoS onto an efficiency core
 * when the load is light, and the DSP thread needs a performance core for the
 * whole run. Elsewhere this does nothing. Only the first call per thread acts.
 */
void cdj_thread_prefer_fast_core(void)
{
#ifdef __APPLE__
    static __thread bool done;

    if (!done) {
        done = true;
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
#endif
}

void cdj_add_exit_report(Notifier *n)
{
    if (cdj_report_enabled()) {
        qemu_add_exit_notifier(n);
    }
}

static int cdj_bytes_get(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field)
{
    GByteArray **a = pv;
    uint32_t len = qemu_get_be32(f);

    if (!*a) {
        *a = g_byte_array_new();
    }
    g_byte_array_set_size(*a, len);
    qemu_get_buffer(f, (*a)->data, len);
    return 0;
}

static int cdj_bytes_put(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field, JSONWriter *vmdesc)
{
    GByteArray *a = *(GByteArray **)pv;
    uint32_t len = a ? a->len : 0;

    qemu_put_be32(f, len);
    if (len) {
        qemu_put_buffer(f, a->data, len);
    }
    return 0;
}

const VMStateInfo cdj_vmstate_info_bytes = {
    .name = "cdj-bytes",
    .get = cdj_bytes_get,
    .put = cdj_bytes_put,
};
