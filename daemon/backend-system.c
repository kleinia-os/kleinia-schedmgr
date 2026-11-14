// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * backend-system.c — real backend for kleinia.
 *
 * All three spawn-model domains share one async loader state machine:
 *   CPU   scx_*   (sched_ext; confirm also requires
 *                 /sys/kernel/sched_ext/state == "enabled")
 *   Io    iox_*   (global iosched_ext policy; kernel must list the
 *                 scx_io elevator on some block device scheduler file)
 *   Cache cax_*   (cache_ext; spawns `cax_X -c <cgroup> [-w <WatchDir>]`)
 * Stop-old is SIGINT -> 3s -> SIGKILL, apply is confirmed after the
 * child survives 2s, and a g_subprocess_wait_async child watch reports
 * unexpected exits (Running -> Failed/Stopped + StrategyFailed +
 * PropertiesChanged).
 *
 * Device objects keep the raw sysfs elevator in PmObj::raw and expose
 * the effective strategy (elevator != scx_io -> elevator; scx_io ->
 * running iox name, or "scx_io" when no loader runs).  Applying an
 * iox_* policy on a device (a) ensures the global loader runs, (b)
 * writes scx_io to the device, (c) confirms by read-back; a failure
 * rolls both phases back best-effort.
 *
 * Nothing here runs at build/test time: the unit tests only exercise
 * the pure helpers in schedmgr-core.c against temporary directories.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "schedmgr-core.h"

#define STOP_GRACE_USEC   (3 * G_USEC_PER_SEC)
#define CONFIRM_MS        2000
#define WATCH_MS          1000
#define STEP_MS           100

/* ---------------- generic loader state machine ---------------- */

typedef struct {
    gboolean check_sched_ext; /* CPU: confirmation needs sched_ext enabled */
    GSubprocess *proc;        /* current loader child, NULL if none */
    guint        confirm_id;  /* 2s confirmation timer */
    guint        watch_id;    /* adopted-pid liveness poll */
    guint        step_id;     /* stop-phase polling timer */
    pid_t        adopted_pid; /* loader adopted from a previous daemon */
    gboolean     busy;
    /* pending operation (in flight) */
    gchar       *pending;     /* NULL = pure stop, "" = apply "none" */
    gchar      **pending_params;
    PmDoneFn     done;
    gpointer     done_ud;
    gint64       stop_deadline;
    gboolean     kill_sent;
} SysLoader;

static gchar *sys_sched_ext_state(void)
{
    return pm_read_sched_ext_state(PM_SYSFS_ROOT);
}

static pid_t sys_find_pid_of_loader(const gchar *exe_name);

static const gchar *loader_dirs(PmObj *o)
{
    switch (o->kind) {
    case PM_KIND_CPU:   return o->app->cfg.sched_dirs;
    case PM_KIND_IO:    return o->app->cfg.iox_dirs;
    case PM_KIND_CACHE: return o->app->cfg.cax_dirs;
    default:            return NULL;
    }
}

static const gchar *loader_prefix(PmObj *o)
{
    switch (o->kind) {
    case PM_KIND_CPU:   return "scx_";
    case PM_KIND_IO:    return "iox_";
    case PM_KIND_CACHE: return "cax_";
    default:            return NULL;
    }
}

static gchar *loader_find_exe(PmObj *o, const gchar *strategy)
{
    const gchar *prefix = loader_prefix(o);
    gchar **dirs, **d, *found = NULL;

    if (!prefix || !g_str_has_prefix(strategy, prefix))
        return NULL;
    dirs = g_strsplit(loader_dirs(o), ":", 0);
    for (d = dirs; *d; d++) {
        gchar *p;

        g_strstrip(*d);
        if (!**d)
            continue;
        p = g_build_filename(*d, strategy, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE) &&
            !g_file_test(p, G_FILE_TEST_IS_DIR)) {
            found = p;
            break;
        }
        g_free(p);
    }
    g_strfreev(dirs);
    return found;
}

static void loader_fail(PmObj *o, GError *error)
{
    SysLoader *c = o->priv;

    if (c->confirm_id) {
        g_source_remove(c->confirm_id);
        c->confirm_id = 0;
    }
    c->busy = FALSE;
    g_free(c->pending);
    c->pending = NULL;
    g_strfreev(c->pending_params);
    c->pending_params = NULL;
    /* section 8.5: the previous strategy was already stopped before the
     * new one failed to start — do not keep reporting the dead one */
    if (o->current && *o->current)
        pm_obj_set_current(o, "", NULL);
    c->done(o->app, o, "", NULL, error, c->done_ud);
}

static gboolean loader_confirm_cb(gpointer ud)
{
    PmObj *o = ud;
    SysLoader *c = o->priv;
    gboolean alive = c->proc && !g_subprocess_get_if_exited(c->proc);
    gboolean enabled = TRUE;
    const gchar *reason;

    c->confirm_id = 0;
    if (c->check_sched_ext) {
        gchar *state = sys_sched_ext_state();

        enabled = state && g_strcmp0(state, "enabled") == 0;
        g_free(state);
    }

    if (alive && enabled) {
        gchar *strategy = c->pending;
        gchar **params = c->pending_params;

        c->pending = NULL;
        c->pending_params = NULL;
        c->busy = FALSE;
        c->done(o->app, o, strategy, params, NULL, c->done_ud);
        g_free(strategy);       /* params ownership moved into done() */
        return G_SOURCE_REMOVE;
    }

    if (c->proc)
        g_subprocess_force_exit(c->proc);   /* wait_cb reports / cleans */
    reason = !alive ? "strategy process exited before confirmation"
                    : "sched_ext did not report 'enabled' within 2s";
    loader_fail(o, g_error_new(PM_ERROR, PM_ERR_FAILED, "%s", reason));
    return G_SOURCE_REMOVE;
}

/* unexpected death of a confirmed, running loader (section 8.5) */
static void loader_exited_running(PmObj *o, gboolean signaled, gint code)
{
    if (!(o->current && *o->current))
        return;                 /* already reported as stopped/failed */
    if (signaled || code != 0) {
        gchar *reason = g_strdup_printf(
            "strategy process exited unexpectedly (%s %d)",
            signaled ? "killed by signal" : "exit status", code);

        pm_obj_report_failure(o, o->current, reason);
        g_free(reason);
    } else {
        /* clean exit: the loader stopped itself */
        pm_obj_set_current(o, "", NULL);
        pm_app_persist_state(o->app);
    }
}

static void loader_wait_cb(GObject *source, GAsyncResult *res, gpointer ud)
{
    PmObj *o = ud;
    SysLoader *c = o->priv;
    GSubprocess *p = G_SUBPROCESS(source);
    gboolean signaled;
    gint code;

    g_subprocess_wait_finish(p, res, NULL);
    if (c->proc != p)
        return;                 /* stale watch of an already-replaced child */
    signaled = g_subprocess_get_if_signaled(p);
    code = signaled ? (gint)g_subprocess_get_term_sig(p)
                    : g_subprocess_get_exit_status(p);
    g_clear_object(&c->proc);

    if (c->confirm_id) {        /* died inside the 2s confirmation window */
        g_source_remove(c->confirm_id);
        c->confirm_id = 0;
        loader_fail(o, g_error_new(PM_ERROR, PM_ERR_FAILED,
                                   "strategy process exited before "
                                   "confirmation (%s %d)",
                                   signaled ? "killed by signal"
                                            : "exit status", code));
        return;
    }
    if (c->busy)
        return;                 /* stop phase: the step poll continues */
    loader_exited_running(o, signaled, code);
}

static gboolean loader_adopted_watch_cb(gpointer ud)
{
    PmObj *o = ud;
    SysLoader *c = o->priv;

    if (c->adopted_pid <= 0) {
        c->watch_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (kill(c->adopted_pid, 0) == -1 && errno == ESRCH) {
        c->watch_id = 0;
        c->adopted_pid = 0;
        if (o->current && *o->current) {
            pm_obj_report_failure(o, o->current,
                                  "strategy process (adopted pid) exited "
                                  "unexpectedly");
        } else {
            pm_obj_set_current(o, "", NULL);
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void loader_spawn(PmObj *o)
{
    SysLoader *c = o->priv;
    const gchar *strategy = c->pending;
    gchar **params = c->pending_params;
    gchar *exe = loader_find_exe(o, strategy);
    GPtrArray *argv;
    GError *e = NULL;
    gint i;

    if (!exe) {
        loader_fail(o, g_error_new(PM_ERROR, PM_ERR_NO_SUCH_STRATEGY,
                                   "no such strategy: %s", strategy));
        return;
    }
    argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, exe);   /* array takes ownership */
    if (o->kind == PM_KIND_CACHE) {
        /* `cax_X -c <cgroup> [-w <WatchDir>]`; params are
         * appended after and may override -w (last one wins) */
        g_ptr_array_add(argv, g_strdup("-c"));
        g_ptr_array_add(argv, g_strdup(o->app->cfg.cache_cgroup));
        if (o->app->cfg.cache_watch_dir && *o->app->cfg.cache_watch_dir) {
            g_ptr_array_add(argv, g_strdup("-w"));
            g_ptr_array_add(argv, g_strdup(o->app->cfg.cache_watch_dir));
        }
    }
    for (i = 0; params && params[i]; i++)
        g_ptr_array_add(argv, g_strdup(params[i]));
    g_ptr_array_add(argv, NULL);

    c->proc = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_NONE, &e);
    g_ptr_array_free(argv, TRUE);
    if (!c->proc) {
        GError *err = g_error_new(PM_ERROR, PM_ERR_FAILED,
                                  "failed to spawn %s: %s", strategy,
                                  e ? e->message : "unknown error");
        g_clear_error(&e);
        loader_fail(o, err);
        return;
    }
    /* section 8.5: child watch for SIGKILL / abnormal exits */
    g_subprocess_wait_async(c->proc, NULL, loader_wait_cb, o);
    c->confirm_id = g_timeout_add(CONFIRM_MS, loader_confirm_cb, o);
}

/* the previous loader is gone (or there was none): finish the stop,
 * then either complete (pure stop / "none") or spawn the pending one */
static void loader_stop_done(PmObj *o)
{
    SysLoader *c = o->priv;

    if (c->step_id) {
        g_source_remove(c->step_id);
        c->step_id = 0;
    }
    g_clear_object(&c->proc);
    c->adopted_pid = 0;
    if (c->watch_id) {
        g_source_remove(c->watch_id);
        c->watch_id = 0;
    }

    if (!c->pending || !*c->pending) {
        /* pure stop, or ApplyStrategy("none") — both end with no policy */
        g_free(c->pending);
        c->pending = NULL;
        g_strfreev(c->pending_params);
        c->pending_params = NULL;
        c->busy = FALSE;
        c->done(o->app, o, "", NULL, NULL, c->done_ud);
        return;
    }
    loader_spawn(o);
}

static gboolean loader_step_cb(gpointer ud)
{
    PmObj *o = ud;
    SysLoader *c = o->priv;
    gboolean exited = FALSE;

    if (c->proc)
        exited = g_subprocess_get_if_exited(c->proc);
    else if (c->adopted_pid > 0) {
        if (kill(c->adopted_pid, 0) == -1 && errno == ESRCH)
            exited = TRUE;
    } else
        exited = TRUE;

    if (!exited && g_get_monotonic_time() >= c->stop_deadline) {
        if (!c->kill_sent) {
            if (c->proc)
                g_subprocess_send_signal(c->proc, SIGKILL);
            else if (c->adopted_pid > 0)
                kill(c->adopted_pid, SIGKILL);
            c->kill_sent = TRUE;
            c->stop_deadline += G_USEC_PER_SEC;
        } else {
            exited = TRUE; /* give up waiting; nothing more we can do */
        }
    }
    if (exited) {
        c->step_id = 0;
        loader_stop_done(o);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void loader_stop_old_start(PmObj *o)
{
    SysLoader *c = o->priv;

    if (c->watch_id) {
        g_source_remove(c->watch_id);
        c->watch_id = 0;
    }
    c->kill_sent = FALSE;
    c->stop_deadline = g_get_monotonic_time() + STOP_GRACE_USEC;

    if (c->proc)
        g_subprocess_send_signal(c->proc, SIGINT);
    else if (c->adopted_pid > 0)
        kill(c->adopted_pid, SIGINT);
    else if (o->current && *o->current &&
             g_strcmp0(o->current, "unknown") != 0) {
        /* loader adopted without a pid at cold start; find it */
        c->adopted_pid = sys_find_pid_of_loader(o->current);
        if (c->adopted_pid > 0)
            kill(c->adopted_pid, SIGINT);
        else {
            loader_stop_done(o);
            return;
        }
    } else {
        loader_stop_done(o);
        return;
    }
    c->step_id = g_timeout_add(STEP_MS, loader_step_cb, o);
}

static void loader_arm_adopted_watch(PmObj *o)
{
    SysLoader *c = o->priv;

    if (c->adopted_pid > 0 && !c->watch_id)
        c->watch_id = g_timeout_add(WATCH_MS, loader_adopted_watch_cb, o);
}

/* ---------------- cache_ext cgroup preparation ---------------- */

/* plain write(2): cgroupfs/sysfs do not support rename, so
 * g_file_set_contents cannot be used on them */
static gboolean write_sys_line(const gchar *path, const gchar *value,
                               GError **error)
{
    gint fd = g_open(path, O_WRONLY, 0);
    gsize len = strlen(value);
    gssize n;

    if (fd < 0) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                    "cannot open %s for writing: %s", path,
                    g_strerror(errno));
        return FALSE;
    }
    n = write(fd, value, len);
    close(fd);
    if (n != (gssize)len) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED, "short write to %s: %s",
                    path, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

/* ensure <dir>/cgroup.subtree_control lists "memory", enabling it via
 * "+memory" when needed */
static gboolean ensure_memory_controller(const gchar *dir, GError **error)
{
    gchar *path = g_build_filename(dir, "cgroup.subtree_control", NULL);
    gchar *buf = NULL;
    gchar **toks, **t;
    gboolean has = FALSE;

    if (!g_file_get_contents(path, &buf, NULL, NULL)) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                    "cannot read %s: %s", path, g_strerror(errno));
        g_free(path);
        return FALSE;
    }
    toks = g_strsplit_set(buf, " \t\r\n", -1);
    for (t = toks; *t; t++)
        if (g_strcmp0(*t, "memory") == 0) {
            has = TRUE;
            break;
        }
    g_strfreev(toks);
    g_free(buf);
    if (!has && !write_sys_line(path, "+memory", error)) {
        g_prefix_error(error, "cannot enable memory controller on %s: ",
                       dir);
        g_free(path);
        return FALSE;
    }
    g_free(path);
    return TRUE;
}

static gboolean sys_cache_ensure_cgroup(PmApp *app, GError **error)
{
    const gchar *cg = app->cfg.cache_cgroup;
    gchar *parent;
    gboolean ok;

    if (!pm_cache_cgroup_valid(cg)) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                    "cache cgroup '%s' is invalid: the kernel requires the "
                    "cgroup name to start with 'cache_ext'", cg);
        return FALSE;
    }
    if (g_mkdir(cg, 0755) != 0 && errno != EEXIST) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED, "cannot create %s: %s",
                    cg, g_strerror(errno));
        return FALSE;
    }
    parent = g_path_get_dirname(cg);
    ok = ensure_memory_controller(parent, error) &&
         ensure_memory_controller(cg, error);
    g_free(parent);
    return ok;
}

/* ---------------- strategy discovery ---------------- */

static void fill_spawn_strategies(GPtrArray *out, const gchar *dirs,
                                  const gchar *prefix,
                                  const gchar *(*desc_fn)(const gchar *),
                                  const gchar *none_desc,
                                  const gchar *fallback_fmt)
{
    GPtrArray *names = pm_scan_prefix_exes(dirs, prefix);
    guint i;

    if (none_desc) {
        PmStrategy *s = g_new0(PmStrategy, 1);

        s->name = g_strdup("none");
        s->description = g_strdup(none_desc);
        g_ptr_array_add(out, s);
    }
    for (i = 0; i < names->len; i++) {
        const gchar *n = g_ptr_array_index(names, i);
        PmStrategy *s = g_new0(PmStrategy, 1);
        const gchar *d = desc_fn(n);

        s->name = g_strdup(n);
        s->description = d ? g_strdup(d)
                           : g_strdup_printf(fallback_fmt, n);
        g_ptr_array_add(out, s);
    }
    g_ptr_array_free(names, TRUE);
}

static void cpu_fill_strategies(PmApp *app, GPtrArray *out)
{
    fill_spawn_strategies(out, app->cfg.sched_dirs, "scx_",
                          pm_cpu_strategy_description,
                          pm_cpu_strategy_description("none"),
                          "sched_ext scheduler (%s)");
}

/* ---------------- device apply (iox_*) with two-phase rollback ------- */

typedef struct {
    PmObj   *dev;
    gchar   *strategy;        /* owned; iox_X */
    gchar   *old_elevator;    /* owned; may be NULL/"" */
    gboolean started_global;  /* this op spawned the global loader */
    PmDoneFn done;
    gpointer ud;
} SysDevIoxOp;

static void sys_dev_iox_op_free(SysDevIoxOp *op)
{
    g_free(op->strategy);
    g_free(op->old_elevator);
    g_free(op);
}

static void sys_dev_iox_write(SysDevIoxOp *op)
{
    PmObj *dev = op->dev;
    gchar *path = pm_path_sched(PM_SYSFS_ROOT, dev->name);
    gchar *confirmed = NULL;
    GError *e = NULL;

    if (pm_write_scheduler_file(path, "scx_io", &confirmed, &e)) {
        g_free(confirmed);
        g_free(dev->raw);
        dev->raw = g_strdup("scx_io");
        g_free(path);
        op->done(dev->app, dev, op->strategy /* borrowed */, NULL, NULL,
                 op->ud);
        sys_dev_iox_op_free(op);
        return;
    }
    /* phase (b) failed: roll back — restore the previous elevator and
     * stop the global loader this operation started (best effort;
     * a previously running loader was already terminated by the switch
     * and is not resurrected) */
    if (op->old_elevator && *op->old_elevator)
        pm_write_scheduler_file(path, op->old_elevator, NULL, NULL);
    if (op->started_global && dev->app->io)
        pm_backend_stop_async(dev->app->io, NULL);
    g_free(path);
    op->done(dev->app, dev, "", NULL, e, op->ud);
    sys_dev_iox_op_free(op);
}

static void sys_dev_iox_global_done(PmApp *app, PmObj *o, gboolean ok,
                                    gpointer ud)
{
    SysDevIoxOp *op = ud;

    (void)o;
    if (op->dev->dead) {
        sys_dev_iox_op_free(op);
        return;
    }
    if (!ok) {
        GError *e = g_error_new(PM_ERROR, PM_ERR_FAILED,
                                "failed to start global IO policy '%s' "
                                "for device %s", op->strategy, op->dev->name);

        op->done(app, op->dev, "", NULL, e, op->ud);
        sys_dev_iox_op_free(op);
        return;
    }
    sys_dev_iox_write(op);
}

/* return the basename of argv[argn] from a NUL-separated /proc cmdline
 * buffer, or NULL when absent; points into buf (valid until buf frees) */
static const gchar *basename_of_arg(const gchar *buf, gsize len, guint argn)
{
    gsize off = 0;
    guint i;

    for (i = 0; i < argn; i++) {
        gsize l = strnlen(buf + off, len - off);

        off += l + 1;
        if (off >= len)
            return NULL;
    }
    if (strnlen(buf + off, len - off) == 0)
        return NULL;
    {
        const gchar *slash = strrchr(buf + off, '/');

        return slash ? slash + 1 : buf + off;
    }
}

/* find a process running exe_name; 0 if none. Matches the basename of
 * argv[0] (ELF loaders) and argv[1] (shebang scripts: the kernel execs
 * "/bin/sh <script>", so the loader name lands in argv[1]) */
static pid_t sys_find_pid_of_loader(const gchar *exe_name)
{
    GDir *d;
    const gchar *n;
    pid_t found = 0;

    d = g_dir_open("/proc", 0, NULL);
    if (!d)
        return 0;
    while ((n = g_dir_read_name(d)) && !found) {
        gchar *path, *buf = NULL;
        gsize len;
        pid_t pid;

        if (!g_ascii_isdigit(n[0]))
            continue;
        pid = (pid_t)g_ascii_strtoll(n, NULL, 10);
        if (pid == getpid())
            continue;
        path = g_build_filename("/proc", n, "cmdline", NULL);
        if (!g_file_get_contents(path, &buf, &len, NULL)) {
            g_free(path);
            continue;
        }
        g_free(path);
        if (len > 0 && buf[0]) {
            /* argv[0] */
            const gchar *base = basename_of_arg(buf, len, 0);

            if (base && g_strcmp0(base, exe_name) == 0) {
                found = pid;
            } else {
                /* argv[1] (shebang: "/bin/sh /path/to/loader") */
                base = basename_of_arg(buf, len, 1);
                if (base && g_strcmp0(base, exe_name) == 0)
                    found = pid;
            }
        }
        g_free(buf);
    }
    g_dir_close(d);
    return found;
}

/* ---------------- generic backend entry points ---------------- */

static void sys_apply(PmApp *app, PmObj *o, gchar *strategy,
                      gchar **params, PmDoneFn done, gpointer ud)
{
    if (o->kind == PM_KIND_DEVICE) {
        if (g_str_has_prefix(strategy, "iox_")) {
            /* combined device policy: ensure the global loader, then
             * switch this device to the scx_io elevator */
            gchar *path = pm_path_sched(PM_SYSFS_ROOT, o->name);
            gchar **names = NULL, *cur = NULL;
            gboolean has_scx_io = FALSE;
            SysDevIoxOp *op;

            if (pm_read_scheduler_file(path, &names, NULL, NULL)) {
                gint i;
                for (i = 0; names && names[i]; i++)
                    if (g_strcmp0(names[i], "scx_io") == 0) {
                        has_scx_io = TRUE;
                        break;
                    }
            }
            pm_read_scheduler_file(path, NULL, &cur, NULL);
            g_strfreev(names);
            if (!has_scx_io) {
                GError *e = g_error_new(PM_ERROR, PM_ERR_NOT_AVAILABLE,
                                        "kernel lacks iosched_ext "
                                        "(scx_io elevator) on %s", o->name);

                g_free(cur);
                g_free(path);
                g_free(strategy);
                g_strfreev(params);
                done(app, o, "", NULL, e, ud);
                return;
            }
            op = g_new0(SysDevIoxOp, 1);
            op->dev = o;
            op->strategy = strategy;        /* takes ownership */
            op->old_elevator = cur;
            op->done = done;
            op->ud = ud;
            g_free(path);
            if (app->io && g_strcmp0(app->io->current, strategy) == 0) {
                op->started_global = FALSE;
                sys_dev_iox_write(op);
            } else {
                op->started_global = TRUE;
                pm_backend_apply_async_notify(app->io, strategy, NULL,
                                              sys_dev_iox_global_done, op);
            }
            g_strfreev(params);   /* devices take no params (checked earlier) */
            return;
        }

        /* native elevator: v1 write + bracket read-back confirmation */
        {
            gchar *path = pm_path_sched(PM_SYSFS_ROOT, o->name);
            gchar *confirmed = NULL;
            GError *e = NULL;

            if (pm_write_scheduler_file(path, strategy, &confirmed, &e)) {
                g_free(o->raw);
                o->raw = g_strdup(confirmed);
                done(app, o, confirmed, params, NULL, ud);   /* borrowed */
                g_free(confirmed);
            } else {
                g_strfreev(params);
                done(app, o, "", NULL, e, ud);
            }
            g_free(path);
            g_free(strategy);
            return;
        }
    }

    if (o->kind == PM_KIND_CPU || o->kind == PM_KIND_IO ||
        o->kind == PM_KIND_CACHE) {
        SysLoader *c = o->priv;

        if (o->kind == PM_KIND_CACHE) {
            GError *ce = NULL;

            if (!sys_cache_ensure_cgroup(app, &ce)) {
                g_strfreev(params);
                g_free(strategy);
                done(app, o, "", NULL, ce, ud);
                return;
            }
        }
        if (c->busy) {
            g_strfreev(params);
            g_free(strategy);
            done(app, o, "", NULL,
                 g_error_new(PM_ERROR, PM_ERR_FAILED,
                             "another strategy operation is in progress"),
                 ud);
            return;
        }
        c->busy = TRUE;
        c->done = done;
        c->done_ud = ud;
        c->pending = strategy;           /* takes ownership */
        c->pending_params = params;
        loader_stop_old_start(o);
        return;
    }

    g_strfreev(params);
    g_free(strategy);
    done(app, o, "", NULL,
         g_error_new(PM_ERROR, PM_ERR_NOT_AVAILABLE,
                     "%s domain is not available", o->name), ud);
}

static void sys_stop(PmApp *app, PmObj *o, PmDoneFn done, gpointer ud)
{
    if (o->kind == PM_KIND_CPU || o->kind == PM_KIND_IO ||
        o->kind == PM_KIND_CACHE) {
        SysLoader *c = o->priv;

        if (c->busy) {
            done(app, o, "", NULL,
                 g_error_new(PM_ERROR, PM_ERR_FAILED,
                             "another strategy operation is in progress"),
                 ud);
            return;
        }
        /* section 8.5: an "unknown" scheduler cannot be stopped — fail
         * honestly instead of pretending success */
        if (o->kind == PM_KIND_CPU && g_strcmp0(o->current, "unknown") == 0) {
            done(app, o, "", NULL,
                 g_error_new(PM_ERROR, PM_ERR_FAILED,
                             "cannot stop: current scheduler is 'unknown' "
                             "(not started by schedmgrd)"), ud);
            return;
        }
        c->busy = TRUE;
        c->done = done;
        c->done_ud = ud;
        c->pending = NULL;      /* pure stop */
        c->pending_params = NULL;
        loader_stop_old_start(o);
        return;
    }
    done(app, o, "", NULL,
         g_error_new(PM_ERROR, PM_ERR_NOT_SUPPORTED,
                     "cannot stop %s directly", o->name), ud);
}

static void sys_list(PmApp *app, PmObj *o, GPtrArray *out)
{
    switch (o->kind) {
    case PM_KIND_CPU:
        cpu_fill_strategies(app, out);
        break;
    case PM_KIND_IO:
        fill_spawn_strategies(out, app->cfg.iox_dirs, "iox_",
                              pm_iox_strategy_description,
                              pm_iox_strategy_description("none"),
                              "iosched_ext policy (%s)");
        break;
    case PM_KIND_CACHE:
        fill_spawn_strategies(out, app->cfg.cax_dirs, "cax_",
                              pm_cax_strategy_description, NULL,
                              "cache_ext policy (%s)");
        break;
    case PM_KIND_DEVICE: {
        gchar *path = pm_path_sched(PM_SYSFS_ROOT, o->name);
        gchar **names = NULL;
        gchar *cur = NULL;

        if (pm_read_scheduler_file(path, &names, &cur, NULL)) {
            gint i;
            for (i = 0; names && names[i]; i++) {
                PmStrategy *s = g_new0(PmStrategy, 1);

                s->name = g_strdup(names[i]);
                s->description = g_strdup(pm_io_strategy_description(names[i]));
                g_ptr_array_add(out, s);
            }
        }
        g_strfreev(names);
        g_free(cur);
        g_free(path);
        /* union with the global iox policies ("全局 eBPF 策略") */
        {
            GPtrArray *iox = pm_scan_prefix_exes(app->cfg.iox_dirs, "iox_");
            guint i;

            for (i = 0; i < iox->len; i++) {
                const gchar *n = g_ptr_array_index(iox, i);
                PmStrategy *s = g_new0(PmStrategy, 1);
                const gchar *d = pm_iox_strategy_description(n);

                s->name = g_strdup(n);
                s->description = g_strdup_printf("%s (global eBPF policy)",
                                                 d ? d : n);
                g_ptr_array_add(out, s);
            }
            g_ptr_array_free(iox, TRUE);
        }
        break;
    }
    default:
        break;
    }
}

static gboolean sys_backend_strategy_known(PmApp *app, PmObj *o,
                                           const gchar *name)
{
    GPtrArray *l = g_ptr_array_new_with_free_func(pm_strategy_free);
    gboolean found = FALSE;
    guint i;

    if (g_strcmp0(name, "none") == 0)
        return TRUE;
    app->backend->list_strategies(app, o, l);
    for (i = 0; i < l->len; i++) {
        PmStrategy *s = g_ptr_array_index(l, i);
        if (g_strcmp0(s->name, name) == 0) {
            found = TRUE;
            break;
        }
    }
    g_ptr_array_free(l, TRUE);
    return found;
}

/* ---------------- device polling ---------------- */

static PmObj *sys_make_device_obj(PmApp *app, const gchar *dev)
{
    gchar *path = pm_path_sched(PM_SYSFS_ROOT, dev);
    gchar *desc = pm_device_description(PM_SYSFS_ROOT, dev);
    gchar **names = NULL, *cur = NULL, *status = NULL, *eff = NULL;
    GError *e = NULL;
    gboolean ok;
    PmObj *o;

    ok = pm_read_scheduler_file(path, &names, &cur, &e);
    if (!ok) {
        status = g_strdup(e ? e->message : "cannot read scheduler file");
        g_clear_error(&e);
    } else {
        gboolean no_loader = FALSE;

        eff = pm_effective_io_strategy(cur, app->io ? app->io->current : "",
                                       &no_loader);
        if (no_loader)
            status = g_strdup(
                "scx_io elevator selected but no iox policy is running");
    }
    g_strfreev(names);
    g_free(path);
    o = pm_app_add_device(app, dev, desc, ok, status ? status : "",
                          eff ? eff : "", cur ? cur : "");
    g_free(desc);
    g_free(status);
    g_free(eff);
    g_free(cur);
    return o;
}

static gboolean sys_io_poll_cb(gpointer ud)
{
    PmApp *app = ud;
    GPtrArray *names = pm_enumerate_block_devices(PM_SYSFS_ROOT,
                                                  app->cfg.io_skip);
    gboolean changed = FALSE;
    guint i, j;

    /* removals */
    for (i = 0; i < app->devices->len; ) {
        PmObj *d = g_ptr_array_index(app->devices, i);
        gboolean found = FALSE;

        for (j = 0; j < names->len; j++)
            if (g_strcmp0(g_ptr_array_index(names, j), d->name) == 0) {
                found = TRUE;
                break;
            }
        if (found)
            i++;
        else {
            pm_app_remove_device(app, d->name);
            changed = TRUE;
        }
    }
    /* additions */
    for (i = 0; i < names->len; i++) {
        const gchar *n = g_ptr_array_index(names, i);

        if (!pm_app_find_device(app, n)) {
            sys_make_device_obj(app, n);
            changed = TRUE;
        }
    }
    /* refresh raw elevators, then recompute effective names (external
     * changes emit StrategyChanged/PropertiesChanged) */
    for (i = 0; i < app->devices->len; i++) {
        PmObj *d = g_ptr_array_index(app->devices, i);
        gchar *dpath = pm_path_sched(PM_SYSFS_ROOT, d->name);
        gchar *cur = NULL;

        if (pm_read_scheduler_file(dpath, NULL, &cur, NULL) && cur &&
            g_strcmp0(cur, d->raw) != 0) {
            g_free(d->raw);
            d->raw = cur;
            cur = NULL;
        }
        g_free(cur);
        g_free(dpath);
    }
    pm_app_refresh_io_devices(app);
    pm_app_persist_state(app);

    if (changed)
        pm_app_devices_changed(app);
    g_ptr_array_free(names, TRUE);
    return G_SOURCE_CONTINUE;
}

/* ---------------- startup / shutdown ---------------- */

/* adopt the loader recorded in state.ini (find its pid so Stop can
 * signal it), or re-apply it best-effort when it is not running */
static void sys_restore_loader(PmApp *app, PmObj *o, const gchar *strategy,
                               gchar **params)
{
    SysLoader *c = o->priv;
    pid_t pid;

    if (!strategy || !*strategy || g_strcmp0(strategy, "none") == 0 ||
        !sys_backend_strategy_known(app, o, strategy))
        return;

    pid = sys_find_pid_of_loader(strategy);
    if (pid > 0) {
        pm_obj_set_current(o, strategy, g_strdupv(params));
        g_free(o->last_ok);
        o->last_ok = g_strdup(strategy);
        g_strfreev(o->last_ok_params);
        o->last_ok_params = g_strdupv(params);
        c->adopted_pid = pid;
        loader_arm_adopted_watch(o);
    } else {
        pm_backend_apply_async(o, strategy, g_strdupv(params), NULL);
    }
}

static void sys_startup(PmApp *app)
{
    PmState st;
    gchar *state;
    gboolean had_state;
    guint i;

    /* CPU */
    app->cpu = pm_obj_new(app, PM_KIND_CPU, "Cpu");
    app->cpu->priv = g_new0(SysLoader, 1);
    ((SysLoader *)app->cpu->priv)->check_sched_ext = TRUE;
    state = sys_sched_ext_state();
    if (!state) {
        app->cpu->available = FALSE;
        g_free(app->cpu->status_message);
        app->cpu->status_message = g_strdup(
            "kernel lacks sched_ext support (/sys/kernel/sched_ext/state not found)");
    } else {
        app->cpu->available = TRUE;
        g_free(state);
    }

    /* Cache: v2 spawn model over cax_* loaders */
    app->cache = pm_obj_new(app, PM_KIND_CACHE, "Cache");
    app->cache->priv = g_new0(SysLoader, 1);
    {
        GPtrArray *cax = pm_scan_prefix_exes(app->cfg.cax_dirs, "cax_");

        if (cax->len > 0) {
            app->cache->available = TRUE;
        } else {
            app->cache->available = FALSE;
            g_free(app->cache->status_message);
            app->cache->status_message = g_strdup(
                "no cax_* policy loaders installed");
        }
        g_ptr_array_free(cax, TRUE);
    }

    /* global IO: iox_* policy domain */
    app->io = pm_obj_new(app, PM_KIND_IO, "Io");
    app->io->priv = g_new0(SysLoader, 1);
    if (pm_kernel_has_scx_io(PM_SYSFS_ROOT)) {
        app->io->available = TRUE;
    } else {
        app->io->available = FALSE;
        g_free(app->io->status_message);
        app->io->status_message = g_strdup(
            "kernel lacks iosched_ext (scx_io elevator)");
    }

    /* devices */
    {
        GPtrArray *devs = pm_enumerate_block_devices(PM_SYSFS_ROOT,
                                                     app->cfg.io_skip);
        for (i = 0; i < devs->len; i++)
            sys_make_device_obj(app, g_ptr_array_index(devs, i));
        g_ptr_array_free(devs, TRUE);
    }

    /* cold start (best effort; failures are logged, not fatal) */
    pm_state_load(&st, app->cfg.state_dir);
    had_state = st.existed;
    state = sys_sched_ext_state();
    if (state && g_strcmp0(state, "enabled") == 0) {
        if (st.cpu_strategy && *st.cpu_strategy &&
            g_strcmp0(st.cpu_strategy, "none") != 0 &&
            g_strcmp0(st.cpu_strategy, "unknown") != 0 &&
            sys_backend_strategy_known(app, app->cpu, st.cpu_strategy)) {
            SysLoader *c = app->cpu->priv;
            /* adopt the running scheduler; keep the pid so Stop works */
            pm_obj_set_current(app->cpu, st.cpu_strategy,
                               g_strdupv(st.cpu_params));
            g_free(app->cpu->last_ok);
            app->cpu->last_ok = g_strdup(st.cpu_strategy);
            g_strfreev(app->cpu->last_ok_params);
            app->cpu->last_ok_params = g_strdupv(st.cpu_params);
            c->adopted_pid = sys_find_pid_of_loader(st.cpu_strategy);
            loader_arm_adopted_watch(app->cpu);
        } else {
            g_free(app->cpu->current);
            app->cpu->current = g_strdup("unknown");
        }
    } else if (had_state && st.cpu_strategy && *st.cpu_strategy &&
               g_strcmp0(st.cpu_strategy, "none") != 0 &&
               sys_backend_strategy_known(app, app->cpu, st.cpu_strategy)) {
        /* state file says a scheduler should be running — re-apply it */
        pm_backend_apply_async(app->cpu, st.cpu_strategy,
                               g_strdupv(st.cpu_params), NULL);
    } else if (!had_state && app->cfg.cpu_default && *app->cfg.cpu_default &&
               g_strcmp0(app->cfg.cpu_default, "none") != 0) {
        pm_backend_apply_async(app->cpu, app->cfg.cpu_default, NULL, NULL);
    }
    g_free(state);

    if (app->io->available)
        sys_restore_loader(app, app->io, st.io_global, NULL);
    if (app->cache->available) {
        if (had_state)
            sys_restore_loader(app, app->cache, st.cache_strategy,
                               st.cache_params);
        else if (app->cfg.cache_default && *app->cfg.cache_default &&
                 sys_backend_strategy_known(app, app->cache,
                                            app->cfg.cache_default))
            pm_backend_apply_async(app->cache, app->cfg.cache_default,
                                   NULL, NULL);
    }

    /* device defaults when starting from a clean slate */
    if (!had_state) {
        for (i = 0; i < app->devices->len; i++) {
            PmObj *d = g_ptr_array_index(app->devices, i);
            const gchar *def = g_hash_table_lookup(app->cfg.io_dev_defaults,
                                                   d->name);
            if (!def || !*def)
                def = app->cfg.io_default;
            if (def && *def)
                pm_backend_apply_async(d, def, NULL, NULL);
        }
    }
    pm_state_clear(&st);

    app->io_poll_id = g_timeout_add_seconds(app->cfg.io_poll_sec,
                                            sys_io_poll_cb, app);
}

static void sys_shutdown(PmApp *app)
{
    PmObj *domains[3];
    guint i;

    if (app->io_poll_id) {
        g_source_remove(app->io_poll_id);
        app->io_poll_id = 0;
    }
    domains[0] = app->cpu;
    domains[1] = app->cache;
    domains[2] = app->io;
    for (i = 0; i < G_N_ELEMENTS(domains); i++) {
        SysLoader *c = domains[i] ? domains[i]->priv : NULL;

        if (!c)
            continue;
        if (c->confirm_id)
            g_source_remove(c->confirm_id);
        if (c->watch_id)
            g_source_remove(c->watch_id);
        if (c->step_id)
            g_source_remove(c->step_id);
        /* a healthy loader keeps running across the restart: drop the
         * GSubprocess reference without signalling it; state.ini records
         * what is still running so the next start adopts it (8.5) */
        g_clear_object(&c->proc);
        g_free(c->pending);
        g_strfreev(c->pending_params);
        g_free(c);
        domains[i]->priv = NULL;
    }
    pm_app_persist_state(app);
}

const PmBackend pm_system_backend = {
    .id = "system",
    .list_strategies = sys_list,
    .apply = sys_apply,
    .stop = sys_stop,
    .startup = sys_startup,
    .shutdown = sys_shutdown,
    .hotplug = NULL,   /* real hotplug arrives via the io poll */
};
