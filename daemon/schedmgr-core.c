// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * schedmgr-core.c — pure helpers shared by schedmgrd, its backends and the
 * unit tests: config parsing, state.ini, scheduler-list parsing, sysfs
 * path composition / device filtering and the static strategy
 * description table.  No function here touches the real /sys unless it
 * is handed "/" as the root, and nothing here talks to DBus.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "schedmgr-core.h"

/* ---------------- error domain ---------------- */

GQuark pm_error_quark(void)
{
    static GQuark q;
    if (!q)
        q = g_quark_from_static_string("schedmgr-error");
    return q;
}

const gchar *pm_err_name(PmErrCode code)
{
    switch (code) {
    case PM_ERR_NO_SUCH_STRATEGY: return PM_ERR_PREFIX "NoSuchStrategy";
    case PM_ERR_NOT_SUPPORTED:    return PM_ERR_PREFIX "NotSupported";
    case PM_ERR_NOT_RUNNING:      return PM_ERR_PREFIX "NotRunning";
    case PM_ERR_NO_DEFAULT:       return PM_ERR_PREFIX "NoDefault";
    case PM_ERR_NOT_AVAILABLE:    return PM_ERR_PREFIX "NotAvailable";
    case PM_ERR_FAILED:           return PM_ERR_PREFIX "Failed";
    default:                      return PM_ERR_PREFIX "Failed";
    }
}

/* ---------------- configuration ---------------- */

void pm_config_defaults(PmConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->sched_dirs = g_strdup("/usr/bin");
    cfg->cache_policy_dir = g_strdup("/usr/lib/kleinia/schedmgr/cache-policies");
    cfg->state_dir = g_strdup("/var/lib/kleinia/schedmgr");
    cfg->io_poll_sec = 5;
    cfg->io_skip = g_strdup("zram,loop");
    cfg->cpu_default = g_strdup("none");
    cfg->cache_default = g_strdup("");
    cfg->io_default = g_strdup("");
    cfg->iox_dirs = g_strdup("/usr/bin:/usr/local/bin:/var/usrlocal/bin");
    cfg->cax_dirs = g_strdup("/usr/bin:/usr/local/bin:/var/usrlocal/bin");
    cfg->cache_watch_dir = g_strdup("/var/tmp");
    cfg->cache_cgroup = g_strdup("/sys/fs/cgroup/cache_ext_schedmgr");
    cfg->io_dev_defaults = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, g_free);
}

void pm_config_load(PmConfig *cfg, const gchar *path)
{
    GKeyFile *kf;
    gchar *s;
    gint n;

    pm_config_defaults(cfg);
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return;

    kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        g_warning("schedmgrd: cannot parse config file %s, using defaults", path);
        return;
    }

    s = g_key_file_get_string(kf, "Cpu", "SchedDirs", NULL);
    if (s && *s) { g_free(cfg->sched_dirs); cfg->sched_dirs = s; } else g_free(s);
    s = g_key_file_get_string(kf, "Cpu", "DefaultStrategy", NULL);
    if (s) { g_free(cfg->cpu_default); cfg->cpu_default = s; }

    s = g_key_file_get_string(kf, "Cache", "PolicyDir", NULL);
    if (s && *s) { g_free(cfg->cache_policy_dir); cfg->cache_policy_dir = s; } else g_free(s);
    s = g_key_file_get_string(kf, "Cache", "DefaultStrategy", NULL);
    if (s) { g_free(cfg->cache_default); cfg->cache_default = s; }
    s = g_key_file_get_string(kf, "Cache", "CaxDirs", NULL);
    if (s && *s) { g_free(cfg->cax_dirs); cfg->cax_dirs = s; } else g_free(s);
    s = g_key_file_get_string(kf, "Cache", "WatchDir", NULL);
    if (s && *s) { g_free(cfg->cache_watch_dir); cfg->cache_watch_dir = s; } else g_free(s);
    s = g_key_file_get_string(kf, "Cache", "Cgroup", NULL);
    if (s && *s) { g_free(cfg->cache_cgroup); cfg->cache_cgroup = s; } else g_free(s);

    s = g_key_file_get_string(kf, "Io", "SkipPrefixes", NULL);
    if (s) { g_free(cfg->io_skip); cfg->io_skip = s; }
    s = g_key_file_get_string(kf, "Io", "DefaultStrategy", NULL);
    if (s) { g_free(cfg->io_default); cfg->io_default = s; }
    s = g_key_file_get_string(kf, "Io", "IoxDirs", NULL);
    if (s && *s) { g_free(cfg->iox_dirs); cfg->iox_dirs = s; } else g_free(s);
    n = g_key_file_get_integer(kf, "Io", "PollIntervalSec", NULL);
    if (n > 0)
        cfg->io_poll_sec = n;

    if (g_key_file_has_group(kf, "Io.DeviceDefaults")) {
        gsize i, len = 0;
        gchar **keys = g_key_file_get_keys(kf, "Io.DeviceDefaults", &len, NULL);
        for (i = 0; keys && i < len; i++) {
            s = g_key_file_get_string(kf, "Io.DeviceDefaults", keys[i], NULL);
            if (s && *s)
                g_hash_table_insert(cfg->io_dev_defaults,
                                    g_strdup(keys[i]), s);
            else
                g_free(s);
        }
        g_strfreev(keys);
    }

    g_key_file_free(kf);
}

void pm_config_free(PmConfig *cfg)
{
    g_free(cfg->sched_dirs);
    g_free(cfg->cache_policy_dir);
    g_free(cfg->state_dir);
    g_free(cfg->io_skip);
    g_free(cfg->cpu_default);
    g_free(cfg->cache_default);
    g_free(cfg->io_default);
    g_free(cfg->iox_dirs);
    g_free(cfg->cax_dirs);
    g_free(cfg->cache_watch_dir);
    g_free(cfg->cache_cgroup);
    g_clear_pointer(&cfg->io_dev_defaults, g_hash_table_destroy);
    memset(cfg, 0, sizeof(*cfg));
}

/* ---------------- state.ini ---------------- */

void pm_state_clear(PmState *st)
{
    g_free(st->cpu_strategy);
    g_strfreev(st->cpu_params);
    g_free(st->cache_strategy);
    g_strfreev(st->cache_params);
    g_free(st->io_global);
    g_clear_pointer(&st->io, g_hash_table_destroy);
    memset(st, 0, sizeof(*st));
}

void pm_state_load(PmState *st, const gchar *state_dir)
{
    gchar *path;
    GKeyFile *kf;
    gsize len = 0, i;
    gchar **keys;

    memset(st, 0, sizeof(*st));
    path = g_build_filename(state_dir, "state.ini", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return;
    }
    kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        g_free(path);
        return;
    }
    st->existed = TRUE;
    st->cpu_strategy = g_key_file_get_string(kf, "cpu", "Strategy", NULL);
    if (!st->cpu_strategy)
        st->cpu_strategy = g_key_file_get_string(kf, "cpu", "strategy", NULL);
    st->cpu_params = g_key_file_get_string_list(kf, "cpu", "Params", &len, NULL);
    if (!st->cpu_params)
        st->cpu_params = g_key_file_get_string_list(kf, "cpu", "params", &len, NULL);

    st->cache_strategy = g_key_file_get_string(kf, "cache", "Strategy", NULL);
    if (!st->cache_strategy)
        st->cache_strategy = g_key_file_get_string(kf, "cache", "strategy", NULL);
    st->cache_params = g_key_file_get_string_list(kf, "cache", "Params", &len, NULL);
    if (!st->cache_params)
        st->cache_params = g_key_file_get_string_list(kf, "cache", "params", &len, NULL);

    keys = g_key_file_get_keys(kf, "io", &len, NULL);
    if (keys) {
        st->io = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        for (i = 0; i < len; i++) {
            gchar *v = g_key_file_get_string(kf, "io", keys[i], NULL);
            if (g_strcmp0(keys[i], "Global") == 0) {
                /* [io] Global = running iox loader */
                g_free(st->io_global);
                st->io_global = v ? v : g_strdup("");
                continue;
            }
            if (v && *v)
                g_hash_table_insert(st->io, g_strdup(keys[i]), v);
            else
                g_free(v);
        }
        g_strfreev(keys);
    }
    g_key_file_free(kf);
    g_free(path);
}

gboolean pm_state_save(const gchar *state_dir, const gchar *cpu_strategy,
                       gchar **cpu_params, const gchar *cache_strategy,
                       gchar **cache_params, const gchar *io_global,
                       GHashTable *io, GError **error)
{
    GKeyFile *kf = g_key_file_new();
    gchar *path;
    gboolean ok;
    GHashTableIter iter;
    gpointer k, v;

    if (cpu_strategy && *cpu_strategy)
        g_key_file_set_string(kf, "cpu", "Strategy", cpu_strategy);
    else
        g_key_file_set_string(kf, "cpu", "Strategy", "");
    if (cpu_params && cpu_params[0])
        g_key_file_set_string_list(kf, "cpu", "Params",
                                   (const gchar *const *)cpu_params,
                                   g_strv_length(cpu_params));

    if (cache_strategy && *cache_strategy) {
        g_key_file_set_string(kf, "cache", "Strategy", cache_strategy);
        if (cache_params && cache_params[0])
            g_key_file_set_string_list(kf, "cache", "Params",
                                       (const gchar *const *)cache_params,
                                       g_strv_length(cache_params));
    } else {
        g_key_file_set_string(kf, "cache", "Strategy", "");
    }

    if (io_global && *io_global)
        g_key_file_set_string(kf, "io", "Global", io_global);
    if (io) {
        g_hash_table_iter_init(&iter, io);
        while (g_hash_table_iter_next(&iter, &k, &v)) {
            if (*((const gchar *)v))
                g_key_file_set_string(kf, "io", k, v);
        }
    }

    g_mkdir_with_parents(state_dir, 0755);
    path = g_build_filename(state_dir, "state.ini", NULL);
    ok = g_key_file_save_to_file(kf, path, error);
    g_free(path);
    g_key_file_free(kf);
    return ok;
}

/* ---------------- scheduler list parsing ---------------- */

gchar **pm_parse_scheduler_list(const gchar *text, gchar **current_out)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    gchar **toks, **p;

    if (current_out)
        *current_out = NULL;
    if (!text) {
        g_ptr_array_add(arr, NULL);
        return (gchar **)g_ptr_array_free(arr, FALSE);
    }

    toks = g_strsplit_set(text, " \t\r\n", -1);
    for (p = toks; *p; p++) {
        gsize len;
        if (!**p)
            continue;
        len = strlen(*p);
        if ((*p)[0] == '[' && (*p)[len - 1] == ']' && len >= 2) {
            gchar *inner = g_strndup(*p + 1, len - 2);
            if (*inner) {
                g_ptr_array_add(arr, g_strdup(inner));
                if (current_out) {
                    g_free(*current_out);
                    *current_out = g_strdup(inner);
                }
            }
            g_free(inner);
        } else {
            g_ptr_array_add(arr, g_strdup(*p));
        }
    }
    g_strfreev(toks);
    g_ptr_array_add(arr, NULL);
    return (gchar **)g_ptr_array_free(arr, FALSE);
}

gboolean pm_read_scheduler_file(const gchar *path, gchar ***names_out,
                                gchar **current_out, GError **error)
{
    gchar *buf = NULL;
    gboolean ok;

    if (names_out)
        *names_out = NULL;
    if (current_out)
        *current_out = NULL;

    if (!g_file_get_contents(path, &buf, NULL, error))
        return FALSE;
    ok = TRUE;
    if (names_out)
        *names_out = pm_parse_scheduler_list(buf, current_out);
    else if (current_out) {
        gchar **tmp = pm_parse_scheduler_list(buf, current_out);
        g_strfreev(tmp);
    }
    g_free(buf);
    return ok;
}

gboolean pm_write_scheduler_file(const gchar *path, const gchar *sched,
                                 gchar **confirmed_out, GError **error)
{
    gchar *line, *buf = NULL, *current = NULL;
    gsize len;
    gssize n;
    int fd;

    if (confirmed_out)
        *confirmed_out = NULL;

    fd = g_open(path, O_WRONLY, 0);
    if (fd < 0) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                    "cannot open %s for writing: %s", path, g_strerror(errno));
        return FALSE;
    }
    line = g_strdup_printf("%s\n", sched);
    len = strlen(line);
    n = write(fd, line, len);
    g_free(line);
    close(fd);
    if (n != (gssize)len) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                    "short write to %s: %s", path, g_strerror(errno));
        return FALSE;
    }

    /* confirm by reading back the bracketed entry */
    if (!g_file_get_contents(path, &buf, NULL, error))
        return FALSE;
    {
        gchar **tmp = pm_parse_scheduler_list(buf, &current);
        g_strfreev(tmp);
    }
    g_free(buf);
    if (!current) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                    "no active scheduler reported by %s after write", path);
        return FALSE;
    }
    if (g_strcmp0(current, sched) != 0) {
        g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                    "scheduler did not switch to '%s' (still '%s')",
                    sched, current);
        g_free(current);
        return FALSE;
    }
    if (confirmed_out)
        *confirmed_out = current;
    else
        g_free(current);
    return TRUE;
}

/* ---------------- device filtering / enumeration ---------------- */

gboolean pm_device_should_skip(const gchar *dev, const gchar *skip_csv)
{
    gchar **p, **toks;
    gboolean skip = FALSE;

    if (!skip_csv || !*skip_csv)
        return FALSE;
    toks = g_strsplit(skip_csv, ",", 0);
    for (p = toks; *p; p++) {
        g_strstrip(*p);
        if (**p && g_str_has_prefix(dev, *p)) {
            skip = TRUE;
            break;
        }
    }
    g_strfreev(toks);
    return skip;
}

static int cmp_str(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(gchar *const *)a, *(gchar *const *)b);
}

GPtrArray *pm_enumerate_block_devices(const gchar *sys_root, const gchar *skip_csv)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    gchar *block = g_build_filename(sys_root, "block", NULL);
    GDir *d = g_dir_open(block, 0, NULL);
    const gchar *n;

    if (!d) {
        g_free(block);
        return out;
    }
    while ((n = g_dir_read_name(d))) {
        gchar *sched;
        if (pm_device_should_skip(n, skip_csv))
            continue;
        sched = g_build_filename(block, n, "queue", "scheduler", NULL);
        if (g_file_test(sched, G_FILE_TEST_EXISTS))
            g_ptr_array_add(out, g_strdup(n));
        g_free(sched);
    }
    g_dir_close(d);
    g_free(block);
    g_ptr_array_sort(out, cmp_str);
    return out;
}

/* ---------------- sysfs path helpers ---------------- */

gchar *pm_path_block(const gchar *sys_root, const gchar *dev)
{
    return g_build_filename(sys_root, "block", dev, NULL);
}

gchar *pm_path_sched(const gchar *sys_root, const gchar *dev)
{
    gchar *b = pm_path_block(sys_root, dev);
    gchar *p = g_build_filename(b, "queue", "scheduler", NULL);
    g_free(b);
    return p;
}

gchar *pm_path_model(const gchar *sys_root, const gchar *dev)
{
    gchar *b = pm_path_block(sys_root, dev);
    gchar *p = g_build_filename(b, "device", "model", NULL);
    g_free(b);
    return p;
}

gchar *pm_path_size(const gchar *sys_root, const gchar *dev)
{
    gchar *b = pm_path_block(sys_root, dev);
    gchar *p = g_build_filename(b, "size", NULL);
    g_free(b);
    return p;
}

/* read one trimmed line from a file, NULL on failure */
static gchar *read_trimmed(const gchar *path)
{
    gchar *buf = NULL, *t, *r;

    if (!g_file_get_contents(path, &buf, NULL, NULL))
        return NULL;
    if (!buf)
        return NULL;
    t = g_strstrip(buf);
    if (!*t) {
        g_free(buf);
        return NULL;
    }
    r = g_strdup(t);
    g_free(buf);
    return r;
}

gchar *pm_read_sched_ext_state(const gchar *sys_root)
{
    gchar *p = g_build_filename(sys_root, "kernel", "sched_ext", "state", NULL);
    gchar *s = read_trimmed(p);

    g_free(p);
    return s;
}

gchar *pm_device_description(const gchar *sys_root, const gchar *dev)
{
    gchar *mpath, *spath, *model = NULL, *desc;
    gchar *size_str;
    unsigned long long sectors = 0;

    mpath = pm_path_model(sys_root, dev);
    model = read_trimmed(mpath);
    g_free(mpath);

    spath = pm_path_size(sys_root, dev);
    size_str = read_trimmed(spath);
    g_free(spath);
    if (size_str) {
        sectors = g_ascii_strtoull(size_str, NULL, 10);
        g_free(size_str);
    }

    if (model && sectors)
        desc = g_strdup_printf("%s (%.1f GiB)", model, sectors * 512.0 / (1024.0 * 1024.0 * 1024.0));
    else if (model)
        desc = g_strdup(model);
    else if (sectors)
        desc = g_strdup_printf("%s (%.1f GiB)", dev, sectors * 512.0 / (1024.0 * 1024.0 * 1024.0));
    else
        desc = g_strdup(dev);
    g_free(model);
    return desc;
}

/* ---------------- strategy description table ---------------- */

static const struct { const gchar *name, *desc; } cpu_descs[] = {
    { "none",        "EEVDF (kernel default scheduling class)" },
    { "scx_agent",     "agent-aware sched_ext scheduler (acb)" },
    { "scx_acb",     "scx_agent (legacy scx_acb build)" },
    { "scx_simple",  "simple sched_ext demo scheduler" },
    { "scx_central", "central-pool sched_ext scheduler" },
    { "scx_flatcg",  "flat-cgroup hierarchical scheduler" },
    { "scx_pair",    "paired-core / CCX scheduler" },
    { "scx_qmap",    "queue-map demo scheduler" },
    { "scx_cpu0",    "single-CPU demo scheduler" },
    { "scx_userland","user-space-driven scheduler" },
    { "scx_rusty",   "BPF-threaded multi-domain scheduler (rusty)" },
    { "scx_layered", "layered user-defined scheduling policy" },
    { "scx_nest",    "hot-core (big-core) scheduler" },
    { "scx_tiny",    "minimal sched_ext scheduler" },
    { NULL, NULL }
};

const gchar *pm_cpu_strategy_description(const gchar *name)
{
    gint i;
    if (!name)
        return NULL;
    for (i = 0; cpu_descs[i].name; i++)
        if (g_strcmp0(cpu_descs[i].name, name) == 0)
            return cpu_descs[i].desc;
    return NULL;
}

static const struct { const gchar *name, *desc; } io_descs[] = {
    { "none",        "no scheduler" },
    { "mq-deadline", "multiqueue deadline scheduler" },
    { "kyber",       "Kyber I/O scheduler" },
    { "bfq",         "Budget Fair Queueing scheduler" },
    { "scx_io",      "sched_ext IO scheduler" },
    { NULL, NULL }
};

const gchar *pm_io_strategy_description(const gchar *name)
{
    gint i;
    if (!name)
        return NULL;
    for (i = 0; io_descs[i].name; i++)
        if (g_strcmp0(io_descs[i].name, name) == 0)
            return io_descs[i].desc;
    return NULL;
}

/* iox_* global IO policy loaders; keyed by the
 * suffix after "iox_" */
static const struct { const gchar *name, *desc; } iox_descs[] = {
    { "none",        "no global IO policy (kernel elevators only)" },
    { "fifo",        "first-in-first-out iosched_ext policy" },
    { "prio",        "priority-ordered iosched_ext policy" },
    { "mq_deadline", "multi-queue deadline iosched_ext policy" },
    { "kyber",       "Kyber-style iosched_ext policy" },
    { "bfq",         "budget-fair-queueing iosched_ext policy" },
    { "wedf",        "weighted-EDF iosched_ext policy" },
    { "sp_edf",      "sporadic-EDF iosched_ext policy" },
    { "merge",       "request-merging iosched_ext policy" },
    { NULL, NULL }
};

const gchar *pm_iox_strategy_description(const gchar *name)
{
    gint i;
    const gchar *suffix;

    if (!name)
        return NULL;
    suffix = g_str_has_prefix(name, "iox_") ? name + strlen("iox_") : name;
    for (i = 0; iox_descs[i].name; i++)
        if (g_strcmp0(iox_descs[i].name, suffix) == 0)
            return iox_descs[i].desc;
    return NULL;
}

/* cax_* cache policy loaders */
static const struct { const gchar *name, *desc; } cax_descs[] = {
    { "cax_fifo",     "first-in-first-out page-eviction policy" },
    { "cax_mru",      "most-recently-used page-eviction policy" },
    { "cax_mglru",    "multi-gen LRU page-eviction policy" },
    { "cax_lhd",      "LRU-heap-diff page-eviction policy" },
    { "cax_sampling", "sampling-based page-eviction policy" },
    { "cax_get_scan", "get_scan-rate page-eviction policy" },
    { "cax_s3fifo",   "S3-FIFO page-eviction policy" },
    { NULL, NULL }
};

const gchar *pm_cax_strategy_description(const gchar *name)
{
    gint i;
    if (!name)
        return NULL;
    for (i = 0; cax_descs[i].name; i++)
        if (g_strcmp0(cax_descs[i].name, name) == 0)
            return cax_descs[i].desc;
    return NULL;
}

void pm_strategy_free(gpointer p)
{
    PmStrategy *s = p;
    if (!s)
        return;
    g_free(s->name);
    g_free(s->description);
    g_free(s);
}

/* read the process start time (field 22 of /proc/<pid>/stat) */
guint64 pm_proc_starttime(pid_t pid)
{
    gchar *path = g_strdup_printf("/proc/%ld/stat", (long)pid);
    gchar *buf = NULL;
    guint64 t = 0;

    if (g_file_get_contents(path, &buf, NULL, NULL)) {
        gchar *p = strrchr(buf, ')');
        unsigned long v = 0;

        if (p)
            sscanf(p + 1,
                   "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s"
                   " %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu",
                   &v);
        t = (guint64)v;
    }
    g_free(buf);
    g_free(path);
    return t;
}

/* ---------------- v2 domain helpers ---------------- */

GPtrArray *pm_scan_prefix_exes(const gchar *dirs_csv, const gchar *prefix)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    gchar **dirs = g_strsplit(dirs_csv ? dirs_csv : "", ":", 0), **d;
    GList *names, *l;

    for (d = dirs; *d; d++) {
        GDir *dir;
        const gchar *n;

        g_strstrip(*d);
        if (!**d)
            continue;
        dir = g_dir_open(*d, 0, NULL);
        if (!dir)
            continue;
        while ((n = g_dir_read_name(dir))) {
            gchar *p;

            if (!g_str_has_prefix(n, prefix) || g_hash_table_contains(seen, n))
                continue;
            p = g_build_filename(*d, n, NULL);
            /* directories can carry the executable bit too */
            if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE) &&
                !g_file_test(p, G_FILE_TEST_IS_DIR))
                g_hash_table_insert(seen, g_strdup(n), GINT_TO_POINTER(1));
            g_free(p);
        }
        g_dir_close(dir);
    }
    g_strfreev(dirs);

    names = g_hash_table_get_keys(seen);
    for (l = names; l; l = l->next)
        g_ptr_array_add(out, g_strdup(l->data));
    g_list_free(names);
    g_ptr_array_sort(out, cmp_str);
    g_hash_table_destroy(seen);
    return out;
}

gchar *pm_effective_io_strategy(const gchar *elevator, const gchar *global_iox,
                                gboolean *no_loader_out)
{
    if (no_loader_out)
        *no_loader_out = FALSE;
    if (!elevator || !*elevator)
        return g_strdup("");
    if (g_strcmp0(elevator, "scx_io") != 0)
        return g_strdup(elevator);
    if (global_iox && *global_iox)
        return g_strdup(global_iox);
    if (no_loader_out)
        *no_loader_out = TRUE;
    return g_strdup("scx_io");
}

gboolean pm_cache_cgroup_valid(const gchar *cgroup)
{
    gboolean ok = FALSE;
    gchar *base;

    if (!cgroup || !g_str_has_prefix(cgroup, "/"))
        return FALSE;
    base = g_path_get_basename(cgroup);
    if (g_strcmp0(base, "/") != 0 && g_strcmp0(base, ".") != 0 &&
        g_strcmp0(base, "..") != 0 && g_str_has_prefix(base, "cache_ext"))
        ok = TRUE;
    g_free(base);
    return ok;
}

gboolean pm_kernel_has_scx_io(const gchar *sys_root)
{
    GPtrArray *devs = pm_enumerate_block_devices(sys_root, "");
    gboolean found = FALSE;
    guint i;

    for (i = 0; !found && i < devs->len; i++) {
        gchar *path = pm_path_sched(sys_root, g_ptr_array_index(devs, i));
        gchar **names = NULL;

        if (pm_read_scheduler_file(path, &names, NULL, NULL)) {
            gint j;
            for (j = 0; names && names[j]; j++)
                if (g_strcmp0(names[j], "scx_io") == 0) {
                    found = TRUE;
                    break;
                }
        }
        g_strfreev(names);
        g_free(path);
    }
    g_ptr_array_free(devs, TRUE);
    return found;
}

/* ---------------- misc ---------------- */

gchar *pm_kernel_release(void)
{
    struct utsname u;

    if (uname(&u) == 0)
        return g_strdup(u.release);
    return g_strdup("unknown");
}

gboolean pm_dev_name_valid(const gchar *dev)
{
    const gchar *p;

    if (!dev || !*dev || strlen(dev) > 63)
        return FALSE;
    for (p = dev; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    }
    return TRUE;
}
