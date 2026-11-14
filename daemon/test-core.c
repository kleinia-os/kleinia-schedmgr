// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * test-core.c — g_test unit tests for the pure helpers in schedmgr-core.c.
 * Everything runs against temporary
 * fake directories; the real /sys is never touched.  The config/drift
 * test re-parses the shipped ../packaging/schedmgr.conf so a key renamed
 * on one side cannot silently invalidate the other.
 */
#define _GNU_SOURCE
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "schedmgr-core.h"

/* ---------------- scheduler list parsing ---------------- */

static void test_sched_parse_basic(void)
{
    gchar *cur = NULL;
    gchar **names = pm_parse_scheduler_list(
        "none mq-deadline [kyber] bfq scx_io", &cur);
    gchar *empty = NULL;
    gchar **none_names;

    g_assert_cmpuint(g_strv_length(names), ==, 5);
    g_assert_cmpstr(names[0], ==, "none");
    g_assert_cmpstr(names[1], ==, "mq-deadline");
    g_assert_cmpstr(names[2], ==, "kyber");
    g_assert_cmpstr(names[3], ==, "bfq");
    g_assert_cmpstr(names[4], ==, "scx_io");
    g_assert_cmpstr(cur, ==, "kyber");
    g_strfreev(names);
    g_free(cur);

    none_names = pm_parse_scheduler_list("[none] mq-deadline", &empty);
    g_assert_cmpstr(empty, ==, "none");
    g_assert_cmpuint(g_strv_length(none_names), ==, 2);
    g_strfreev(none_names);
    g_free(empty);
}

static void test_sched_parse_edge_cases(void)
{
    gchar *cur = NULL;
    gchar **names;

    /* no brackets -> no current */
    names = pm_parse_scheduler_list("mq-deadline kyber", &cur);
    g_assert_cmpuint(g_strv_length(names), ==, 2);
    g_assert_null(cur);
    g_strfreev(names);
    g_free(cur);
    cur = NULL;

    /* empty / whitespace */
    names = pm_parse_scheduler_list("   \n\t ", &cur);
    g_assert_cmpuint(g_strv_length(names), ==, 0);
    g_assert_null(cur);
    g_strfreev(names);
    g_free(cur);
    cur = NULL;

    names = pm_parse_scheduler_list("", &cur);
    g_assert_cmpuint(g_strv_length(names), ==, 0);
    g_assert_null(cur);
    g_strfreev(names);
    g_free(cur);
    cur = NULL;

    /* NULL input */
    names = pm_parse_scheduler_list(NULL, &cur);
    g_assert_cmpuint(g_strv_length(names), ==, 0);
    g_assert_null(cur);
    g_strfreev(names);
    g_free(cur);
}

static void test_sched_file_roundtrip(void)
{
    gchar *dir = g_dir_make_tmp("schedmgr-test-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "scheduler", NULL);
    gchar **names = NULL;
    gchar *cur = NULL;

    g_file_set_contents(path, "none mq-deadline [kyber] bfq scx_io\n", -1,
                        NULL);
    g_assert_true(pm_read_scheduler_file(path, &names, &cur, NULL));
    g_assert_cmpstr(cur, ==, "kyber");
    g_assert_cmpuint(g_strv_length(names), ==, 5);
    g_strfreev(names);
    g_free(cur);

    /* reading a missing file must fail cleanly */
    g_assert_false(pm_read_scheduler_file("/nonexistent/zzz", &names, &cur,
                                          NULL));
    g_assert_null(names);
    g_assert_null(cur);

    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

/* writing a plain tmp file cannot produce bracketed output, so the
 * confirmation must fail — exactly the mismatch path we want covered */
static void test_sched_write_confirm_mismatch(void)
{
    gchar *dir = g_dir_make_tmp("schedmgr-test-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "scheduler", NULL);
    gchar *confirmed = NULL;
    GError *e = NULL;

    g_file_set_contents(path, "[kyber] bfq\n", -1, NULL);
    g_assert_false(pm_write_scheduler_file(path, "bfq", &confirmed, &e));
    g_assert_nonnull(e);
    /* plain file cannot produce bracketed output -> confirmation fails */
    g_assert_nonnull(strstr(e->message, "no active scheduler reported"));
    g_assert_null(confirmed);
    g_clear_error(&e);

    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

/* ---------------- device filtering / enumeration ---------------- */

static void test_device_filter(void)
{
    g_assert_true(pm_device_should_skip("zram0", "zram,loop"));
    g_assert_true(pm_device_should_skip("loop3", "zram,loop"));
    g_assert_false(pm_device_should_skip("sda", "zram,loop"));
    g_assert_false(pm_device_should_skip("nvme0n1", "zram,loop"));
    /* empty config disables skipping entirely */
    g_assert_false(pm_device_should_skip("zram0", ""));
    g_assert_false(pm_device_should_skip("zram0", NULL));
    /* whitespace tolerance */
    g_assert_true(pm_device_should_skip("zram9", " zram , loop "));
}

static void make_fake_sysfs(const gchar *root)
{
    struct {
        const gchar *dev;
        gboolean with_sched;
    } devs[] = {
        { "sda",     TRUE },
        { "sdb",     TRUE },
        { "zram0",   TRUE },
        { "loop0",   TRUE },
        { "nvme0n1", TRUE },
        { "sdc",     FALSE },  /* no queue/scheduler file -> excluded */
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(devs); i++) {
        gchar *d;
        if (devs[i].with_sched) {
            d = g_build_filename(root, "block", devs[i].dev, "queue", NULL);
            g_mkdir_with_parents(d, 0755);
            g_free(d);
            d = g_build_filename(root, "block", devs[i].dev, "queue",
                                 "scheduler", NULL);
            g_file_set_contents(d, "none [mq-deadline] kyber\n", -1, NULL);
            g_free(d);
        } else {
            d = g_build_filename(root, "block", devs[i].dev, NULL);
            g_mkdir_with_parents(d, 0755);
            g_free(d);
        }
    }
}

static void rm_rf_fake_sysfs(const gchar *root)
{
    const gchar *names[] = { "sda", "sdb", "zram0", "loop0", "nvme0n1",
                             "sdc" };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        gchar *p = g_build_filename(root, "block", names[i], "queue",
                                    "scheduler", NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            g_unlink(p);
        g_free(p);
        p = g_build_filename(root, "block", names[i], "queue", NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            g_rmdir(p);
        g_free(p);
        p = g_build_filename(root, "block", names[i], NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            g_rmdir(p);
        g_free(p);
    }
    {
        gchar *p = g_build_filename(root, "block", NULL);
        g_rmdir(p);
        g_free(p);
    }
}

static void test_enumerate_devices(void)
{
    gchar *root = g_dir_make_tmp("schedmgr-test-XXXXXX", NULL);
    GPtrArray *devs;

    make_fake_sysfs(root);

    devs = pm_enumerate_block_devices(root, "zram,loop");
    /* sda, sdb, nvme0n1 survive: no zram/loop, sdc lacks scheduler file */
    g_assert_cmpuint(devs->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(devs, 0), ==, "nvme0n1");
    g_assert_cmpstr(g_ptr_array_index(devs, 1), ==, "sda");
    g_assert_cmpstr(g_ptr_array_index(devs, 2), ==, "sdb");
    g_ptr_array_free(devs, TRUE);

    /* empty skip list keeps zram/loop but still drops sdc */
    devs = pm_enumerate_block_devices(root, "");
    g_assert_cmpuint(devs->len, ==, 5);
    g_ptr_array_free(devs, TRUE);

    /* missing root -> empty, no crash */
    devs = pm_enumerate_block_devices("/nonexistent-root-xyz", "zram,loop");
    g_assert_cmpuint(devs->len, ==, 0);
    g_ptr_array_free(devs, TRUE);

    rm_rf_fake_sysfs(root);
    g_rmdir(root);
    g_free(root);
}

/* ---------------- sysfs path helpers ---------------- */

static void test_sysfs_paths(void)
{
    gchar *p;

    p = pm_path_block("/sys", "sda");
    g_assert_cmpstr(p, ==, "/sys/block/sda");
    g_free(p);

    p = pm_path_sched("/sys", "nvme0n1");
    g_assert_cmpstr(p, ==, "/sys/block/nvme0n1/queue/scheduler");
    g_free(p);

    p = pm_path_model("/sys", "sda");
    g_assert_cmpstr(p, ==, "/sys/block/sda/device/model");
    g_free(p);

    p = pm_path_size("/sys", "sda");
    g_assert_cmpstr(p, ==, "/sys/block/sda/size");
    g_free(p);
}

static void test_device_description(void)
{
    gchar *root = g_dir_make_tmp("schedmgr-test-XXXXXX", NULL);
    gchar *dir, *p, *desc;

    dir = g_build_filename(root, "block", "sda", "device", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    p = g_build_filename(root, "block", "sda", "size", NULL);
    g_file_set_contents(p, "1000215216", -1, NULL);  /* ~477 GiB */
    g_free(p);
    p = g_build_filename(root, "block", "sda", "device", "model", NULL);
    g_file_set_contents(p, "Fake SSD 9000\n", -1, NULL);
    g_free(p);

    desc = pm_device_description(root, "sda");
    g_assert_nonnull(strstr(desc, "Fake SSD 9000"));
    g_assert_nonnull(strstr(desc, "GiB"));
    g_free(desc);

    /* no model, no size -> falls back to the device name */
    desc = pm_device_description(root, "no-such-dev");
    g_assert_cmpstr(desc, ==, "no-such-dev");
    g_free(desc);

    dir = g_build_filename(root, "block", "sda", "device", NULL);
    g_rmdir(dir);
    g_free(dir);
    p = g_build_filename(root, "block", "sda", NULL);
    g_rmdir(p);
    g_free(p);
    p = g_build_filename(root, "block", NULL);
    g_rmdir(p);
    g_free(p);
    g_rmdir(root);
    g_free(root);
}

/* ---------------- state.ini ---------------- */

static void test_state_roundtrip(void)
{
    gchar *dir = g_dir_make_tmp("schedmgr-test-XXXXXX", NULL);
    PmState st;
    GHashTable *io = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
    gchar *params[] = { "--slice-ms", "20", NULL };
    gchar *cparams[] = { "-w", "/tmp/custom-watch", NULL };
    gchar *path;

    g_hash_table_insert(io, g_strdup("sda"), g_strdup("kyber"));
    g_hash_table_insert(io, g_strdup("nvme0n1"), g_strdup("none"));

    /* v2 keys: [cache] Strategy/Params + [io] Global */
    g_assert_true(pm_state_save(dir, "scx_agent", params,
                                "cax_mru", cparams, "iox_wedf", io, NULL));

    memset(&st, 0, sizeof(st));
    pm_state_load(&st, dir);
    g_assert_true(st.existed);
    g_assert_cmpstr(st.cpu_strategy, ==, "scx_agent");
    g_assert_cmpuint(g_strv_length(st.cpu_params), ==, 2);
    g_assert_cmpstr(st.cpu_params[0], ==, "--slice-ms");
    g_assert_cmpstr(st.cpu_params[1], ==, "20");
    g_assert_cmpstr(st.cache_strategy, ==, "cax_mru");
    g_assert_cmpuint(g_strv_length(st.cache_params), ==, 2);
    g_assert_cmpstr(st.cache_params[0], ==, "-w");
    g_assert_cmpstr(st.cache_params[1], ==, "/tmp/custom-watch");
    g_assert_cmpstr(st.io_global, ==, "iox_wedf");
    g_assert_cmpstr(g_hash_table_lookup(st.io, "sda"), ==, "kyber");
    g_assert_cmpstr(g_hash_table_lookup(st.io, "nvme0n1"), ==, "none");
    /* "Global" is hoisted out of the device table */
    g_assert_null(g_hash_table_lookup(st.io, "Global"));
    pm_state_clear(&st);

    /* stopped state persists Strategy="" without params / global */
    g_assert_true(pm_state_save(dir, "", NULL, "", NULL, "", io, NULL));
    pm_state_load(&st, dir);
    g_assert_true(st.existed);
    g_assert_cmpstr(st.cpu_strategy, ==, "");
    g_assert_null(st.cpu_params);
    g_assert_cmpstr(st.cache_strategy, ==, "");
    g_assert_cmpstr(st.io_global ? st.io_global : "", ==, "");
    pm_state_clear(&st);

    /* missing directory -> empty state */
    pm_state_load(&st, "/nonexistent-state-dir-xyz");
    g_assert_false(st.existed);
    g_assert_null(st.cpu_strategy);
    g_assert_null(st.cpu_params);
    g_assert_null(st.cache_strategy);
    g_assert_null(st.io_global);
    g_assert_null(st.io);
    pm_state_clear(&st);

    g_hash_table_destroy(io);
    path = g_build_filename(dir, "state.ini", NULL);
    g_unlink(path);
    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

/* graceful shutdown: the daemon writes what is *still
 * running* — cpu + cache + global iox loaders + device table — into
 * state.ini before exiting without killing the healthy children */
static void test_state_shutdown_write(void)
{
    gchar *dir = g_dir_make_tmp("schedmgr-test-XXXXXX", NULL);
    PmState st;
    GHashTable *io = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, g_free);
    gchar *path;

    /* composition mirroring pm_app_persist_state() at shutdown:
     * cpu=scx_agent, cache=cax_lhd, io Global=iox_fifo,
     * sda follows the global policy, nvme0n1 stays native */
    g_hash_table_insert(io, g_strdup("sda"), g_strdup("iox_fifo"));
    g_hash_table_insert(io, g_strdup("nvme0n1"), g_strdup("mq-deadline"));
    g_assert_true(pm_state_save(dir, "scx_agent", NULL, "cax_lhd", NULL,
                                "iox_fifo", io, NULL));
    pm_state_load(&st, dir);
    g_assert_true(st.existed);
    g_assert_cmpstr(st.cpu_strategy, ==, "scx_agent");
    g_assert_cmpstr(st.cache_strategy, ==, "cax_lhd");
    g_assert_cmpstr(st.io_global, ==, "iox_fifo");
    g_assert_cmpstr(g_hash_table_lookup(st.io, "sda"), ==, "iox_fifo");
    g_assert_cmpstr(g_hash_table_lookup(st.io, "nvme0n1"), ==,
                    "mq-deadline");
    pm_state_clear(&st);

    g_hash_table_destroy(io);
    path = g_build_filename(dir, "state.ini", NULL);
    g_unlink(path);
    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

/* ---------------- config ---------------- */

static void test_config_defaults(void)
{
    PmConfig cfg;

    pm_config_defaults(&cfg);
    g_assert_cmpstr(cfg.sched_dirs, ==, "/usr/bin");
    g_assert_cmpstr(cfg.cache_policy_dir, ==,
                    "/usr/lib/kleinia/schedmgr/cache-policies");
    g_assert_cmpstr(cfg.state_dir, ==, "/var/lib/kleinia/schedmgr");
    g_assert_cmpint(cfg.io_poll_sec, ==, 5);
    g_assert_cmpstr(cfg.io_skip, ==, "zram,loop");
    g_assert_cmpstr(cfg.cpu_default, ==, "none");
    g_assert_cmpstr(cfg.cache_default, ==, "");
    g_assert_cmpstr(cfg.io_default, ==, "");
    /* v2 keys */
    g_assert_cmpstr(cfg.iox_dirs, ==,
                    "/usr/bin:/usr/local/bin:/var/usrlocal/bin");
    g_assert_cmpstr(cfg.cax_dirs, ==,
                    "/usr/bin:/usr/local/bin:/var/usrlocal/bin");
    g_assert_cmpstr(cfg.cache_watch_dir, ==, "/var/tmp");
    g_assert_cmpstr(cfg.cache_cgroup, ==, "/sys/fs/cgroup/cache_ext_schedmgr");
    pm_config_free(&cfg);
}

static void test_config_load(void)
{
    gchar *dir = g_dir_make_tmp("schedmgr-test-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "schedmgr.conf", NULL);
    PmConfig cfg;
    const gchar *conf =
        "[Cpu]\n"
        "SchedDirs=/opt/scx:/usr/bin\n"
        "DefaultStrategy=scx_simple\n"
        "\n"
        "[Cache]\n"
        "PolicyDir=/tmp/policies\n"
        "CaxDirs=/opt/cax:/usr/bin\n"
        "WatchDir=/tmp/watch\n"
        "Cgroup=/sys/fs/cgroup/cache_ext_test\n"
        "\n"
        "[Io]\n"
        "PollIntervalSec=9\n"
        "SkipPrefixes=zd\n"
        "DefaultStrategy=bfq\n"
        "IoxDirs=/opt/iox:/usr/bin\n"
        "\n"
        "[Io.DeviceDefaults]\n"
        "sda=kyber\n";

    g_file_set_contents(path, conf, -1, NULL);
    pm_config_defaults(&cfg);
    pm_config_load(&cfg, path);
    g_assert_cmpstr(cfg.sched_dirs, ==, "/opt/scx:/usr/bin");
    g_assert_cmpstr(cfg.cpu_default, ==, "scx_simple");
    g_assert_cmpstr(cfg.cache_policy_dir, ==, "/tmp/policies");
    g_assert_cmpstr(cfg.cax_dirs, ==, "/opt/cax:/usr/bin");
    g_assert_cmpstr(cfg.cache_watch_dir, ==, "/tmp/watch");
    g_assert_cmpstr(cfg.cache_cgroup, ==, "/sys/fs/cgroup/cache_ext_test");
    g_assert_cmpint(cfg.io_poll_sec, ==, 9);
    g_assert_cmpstr(cfg.io_skip, ==, "zd");
    g_assert_cmpstr(cfg.io_default, ==, "bfq");
    g_assert_cmpstr(cfg.iox_dirs, ==, "/opt/iox:/usr/bin");
    g_assert_cmpstr(g_hash_table_lookup(cfg.io_dev_defaults, "sda"), ==,
                    "kyber");
    pm_config_free(&cfg);

    /* missing file keeps defaults */
    pm_config_defaults(&cfg);
    pm_config_load(&cfg, "/nonexistent/schedmgr.conf");
    g_assert_cmpstr(cfg.sched_dirs, ==, "/usr/bin");
    g_assert_cmpint(cfg.io_poll_sec, ==, 5);
    g_assert_cmpstr(cfg.iox_dirs, ==,
                    "/usr/bin:/usr/local/bin:/var/usrlocal/bin");
    pm_config_free(&cfg);

    g_unlink(path);
    g_free(path);
    g_rmdir(dir);
    g_free(dir);
}

/* locate the shipped packaging/schedmgr.conf relative to the test binary
 * (make check runs in daemon/); a hard failure keeps drift protection
 * enforced no matter where the binary is invoked from */
static gchar *find_shipped_conf(void)
{
    static const gchar *const cands[] = {
        "../packaging/schedmgr.conf",
        "./packaging/schedmgr.conf",
        NULL,
    };
    gint i;

    for (i = 0; cands[i]; i++) {
        if (g_file_test(cands[i], G_FILE_TEST_EXISTS))
            return g_strdup(cands[i]);
    }
    return NULL;
}

/* config drift: the shipped schedmgr.conf must parse cleanly, contain
 * only keys the parser knows, and carry *valid* values — a key renamed
 * on one side must fail here instead of silently disabling a feature */
static void test_config_drift(void)
{
    static const gchar *const known_cpu[] = { "SchedDirs",
                                              "DefaultStrategy", NULL };
    static const gchar *const known_cache[] = { "PolicyDir", "CaxDirs",
                                                "WatchDir", "Cgroup",
                                                "DefaultStrategy", NULL };
    static const gchar *const known_io[] = { "DefaultStrategy",
                                             "SkipPrefixes",
                                             "PollIntervalSec", "IoxDirs",
                                             NULL };
    gchar *conf = find_shipped_conf();
    GKeyFile *kf;
    gchar **keys = NULL;
    gsize len = 0, i, j;
    PmConfig cfg;

    if (!conf)
        g_error("cannot locate packaging/schedmgr.conf (run via make check)");

    kf = g_key_file_new();
    g_assert_true(g_key_file_load_from_file(kf, conf, G_KEY_FILE_NONE, NULL));

    /* every shipped key must be recognized by the parser */
    keys = g_key_file_get_keys(kf, "Cpu", &len, NULL);
    for (i = 0; keys && i < len; i++) {
        gboolean ok = FALSE;
        for (j = 0; known_cpu[j]; j++)
            if (g_strcmp0(keys[i], known_cpu[j]) == 0)
                ok = TRUE;
        if (!ok)
            g_error("unknown [Cpu] key in packaging/schedmgr.conf: %s",
                    keys[i]);
    }
    g_strfreev(keys);

    keys = g_key_file_get_keys(kf, "Cache", &len, NULL);
    for (i = 0; keys && i < len; i++) {
        gboolean ok = FALSE;
        for (j = 0; known_cache[j]; j++)
            if (g_strcmp0(keys[i], known_cache[j]) == 0)
                ok = TRUE;
        if (!ok)
            g_error("unknown [Cache] key in packaging/schedmgr.conf: %s",
                    keys[i]);
    }
    g_strfreev(keys);

    keys = g_key_file_get_keys(kf, "Io", &len, NULL);
    for (i = 0; keys && i < len; i++) {
        gboolean ok = FALSE;
        for (j = 0; known_io[j]; j++)
            if (g_strcmp0(keys[i], known_io[j]) == 0)
                ok = TRUE;
        if (!ok)
            g_error("unknown [Io] key in packaging/schedmgr.conf: %s",
                    keys[i]);
    }
    g_strfreev(keys);

    /* device defaults: keys must be valid device names (may be absent) */
    keys = g_key_file_get_keys(kf, "Io.DeviceDefaults", &len, NULL);
    for (i = 0; keys && i < len; i++) {
        if (!pm_dev_name_valid(keys[i]))
            g_error("invalid device name in [Io.DeviceDefaults]: %s",
                    keys[i]);
    }
    g_strfreev(keys);

    /* values must be valid */
    {
        gint poll = g_key_file_get_integer(kf, "Io", "PollIntervalSec", NULL);

        g_assert_cmpint(poll, >, 0);
    }
    {
        const gchar *dirkeys[] = { "Cpu::SchedDirs", "Io::IoxDirs",
                                   "Cache::CaxDirs", NULL };

        for (i = 0; dirkeys[i]; i++) {
            gchar *grp = g_strdup(dirkeys[i]);
            gchar *sep = strstr(grp, "::");
            gchar *val, **toks, **t;
            gboolean ok = TRUE;

            g_assert_nonnull(sep);
            *sep = '\0';
            val = g_key_file_get_string(kf, grp, sep + 2, NULL);
            g_assert_nonnull(val);
            toks = g_strsplit(val, ":", 0);
            for (t = toks; *t; t++) {
                g_strstrip(*t);
                if (**t && !g_str_has_prefix(*t, "/"))
                    ok = FALSE;
            }
            g_strfreev(toks);
            g_free(val);
            g_free(grp);
            g_assert_true(ok);
        }
    }
    {
        gchar *wd = g_key_file_get_string(kf, "Cache", "WatchDir", NULL);

        g_assert_nonnull(wd);
        g_assert_true(*wd == '\0' || g_str_has_prefix(wd, "/"));
        g_free(wd);
    }
    {
        gchar *cg = g_key_file_get_string(kf, "Cache", "Cgroup", NULL);

        g_assert_nonnull(cg);
        g_assert_true(pm_cache_cgroup_valid(cg));
        g_free(cg);
    }

    /* and the parser must accept everything the file ships */
    pm_config_defaults(&cfg);
    pm_config_load(&cfg, conf);
    g_assert_cmpstr(cfg.iox_dirs, ==,
                    "/usr/bin:/usr/local/bin:/var/usrlocal/bin");
    g_assert_cmpstr(cfg.cax_dirs, ==,
                    "/usr/bin:/usr/local/bin:/var/usrlocal/bin");
    g_assert_cmpstr(cfg.cache_watch_dir, ==, "/var/tmp");
    g_assert_cmpstr(cfg.cache_cgroup, ==,
                    "/sys/fs/cgroup/cache_ext_schedmgr");
    pm_config_free(&cfg);

    g_key_file_free(kf);
    g_free(conf);
}

/* ---------------- v2 domain helpers ---------------- */

/* iox/cax discovery: dedup across dirs, sorted, executable-only */
static void test_scan_prefix_exes(void)
{
    gchar *d1 = g_dir_make_tmp("schedmgr-scan1-XXXXXX", NULL);
    gchar *d2 = g_dir_make_tmp("schedmgr-scan2-XXXXXX", NULL);
    gchar *p, *csv;
    GPtrArray *arr;

    /* d1: iox_b (exec), iox_a (exec), iox_c (non-exec), scx_x (wrong prefix) */
    p = g_build_filename(d1, "iox_b", NULL);
    g_file_set_contents(p, "#!/bin/sh\n", -1, NULL);
    g_chmod(p, 0755);
    g_free(p);
    p = g_build_filename(d1, "iox_a", NULL);
    g_file_set_contents(p, "#!/bin/sh\n", -1, NULL);
    g_chmod(p, 0755);
    g_free(p);
    p = g_build_filename(d1, "iox_c", NULL);
    g_file_set_contents(p, "#!/bin/sh\n", -1, NULL);
    g_chmod(p, 0644);
    g_free(p);
    p = g_build_filename(d1, "scx_x", NULL);
    g_file_set_contents(p, "#!/bin/sh\n", -1, NULL);
    g_chmod(p, 0755);
    g_free(p);
    /* d2: iox_a again (dedup), cax_mru */
    p = g_build_filename(d2, "iox_a", NULL);
    g_file_set_contents(p, "#!/bin/sh\n", -1, NULL);
    g_chmod(p, 0755);
    g_free(p);
    p = g_build_filename(d2, "cax_mru", NULL);
    g_file_set_contents(p, "#!/bin/sh\n", -1, NULL);
    g_chmod(p, 0755);
    g_free(p);

    csv = g_strdup_printf("%s:%s", d1, d2);
    arr = pm_scan_prefix_exes(csv, "iox_");
    g_assert_cmpuint(arr->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(arr, 0), ==, "iox_a");  /* sorted */
    g_assert_cmpstr(g_ptr_array_index(arr, 1), ==, "iox_b");  /* dedup'd */
    g_ptr_array_free(arr, TRUE);

    arr = pm_scan_prefix_exes(csv, "cax_");
    g_assert_cmpuint(arr->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(arr, 0), ==, "cax_mru");
    g_ptr_array_free(arr, TRUE);
    g_free(csv);

    /* missing dirs -> empty, no crash */
    arr = pm_scan_prefix_exes("/nonexistent-dir-xyz", "iox_");
    g_assert_cmpuint(arr->len, ==, 0);
    g_ptr_array_free(arr, TRUE);

    p = g_build_filename(d1, "iox_a", NULL);
    g_unlink(p);
    g_free(p);
    p = g_build_filename(d1, "iox_b", NULL);
    g_unlink(p);
    g_free(p);
    p = g_build_filename(d1, "iox_c", NULL);
    g_unlink(p);
    g_free(p);
    p = g_build_filename(d1, "scx_x", NULL);
    g_unlink(p);
    g_free(p);
    p = g_build_filename(d2, "iox_a", NULL);
    g_unlink(p);
    g_free(p);
    p = g_build_filename(d2, "cax_mru", NULL);
    g_unlink(p);
    g_free(p);
    g_rmdir(d1);
    g_rmdir(d2);
    g_free(d1);
    g_free(d2);
}

/* effective per-device strategy, incl. the
 * "elevator = scx_io but no iox loader runs" branch */
static void test_effective_io_strategy(void)
{
    gboolean no_loader = TRUE;
    gchar *s;

    s = pm_effective_io_strategy("kyber", "iox_fifo", &no_loader);
    g_assert_cmpstr(s, ==, "kyber");
    g_assert_false(no_loader);
    g_free(s);

    s = pm_effective_io_strategy("scx_io", "iox_fifo", &no_loader);
    g_assert_cmpstr(s, ==, "iox_fifo");
    g_assert_false(no_loader);
    g_free(s);

    /* no-loader branch: show the literal elevator + hint flag */
    s = pm_effective_io_strategy("scx_io", "", &no_loader);
    g_assert_cmpstr(s, ==, "scx_io");
    g_assert_true(no_loader);
    g_free(s);

    s = pm_effective_io_strategy("scx_io", NULL, &no_loader);
    g_assert_cmpstr(s, ==, "scx_io");
    g_assert_true(no_loader);
    g_free(s);

    s = pm_effective_io_strategy("", "iox_fifo", &no_loader);
    g_assert_cmpstr(s, ==, "");
    g_free(s);

    s = pm_effective_io_strategy(NULL, "iox_fifo", NULL);
    g_assert_cmpstr(s, ==, "");
    g_free(s);
}

/* the kernel requires the cache cgroup name to start with "cache_ext" */
static void test_cache_cgroup_valid(void)
{
    g_assert_true(pm_cache_cgroup_valid("/sys/fs/cgroup/cache_ext_schedmgr"));
    g_assert_true(pm_cache_cgroup_valid("/sys/fs/cgroup/cache_ext"));
    g_assert_false(pm_cache_cgroup_valid("/sys/fs/cgroup/schedmgr"));
    g_assert_false(pm_cache_cgroup_valid("cache_ext_schedmgr")); /* relative */
    g_assert_false(pm_cache_cgroup_valid(""));
    g_assert_false(pm_cache_cgroup_valid(NULL));
    g_assert_false(pm_cache_cgroup_valid("/"));
}

/* kernel capability probe: any device's scheduler list containing
 * scx_io makes the global IO domain available */
static void test_kernel_has_scx_io(void)
{
    gchar *root = g_dir_make_tmp("schedmgr-cap-XXXXXX", NULL);
    gchar *p;

    /* sda without scx_io */
    p = g_build_filename(root, "block", "sda", "queue", NULL);
    g_mkdir_with_parents(p, 0755);
    g_free(p);
    p = g_build_filename(root, "block", "sda", "queue", "scheduler", NULL);
    g_file_set_contents(p, "none [mq-deadline] kyber\n", -1, NULL);
    g_free(p);
    g_assert_false(pm_kernel_has_scx_io(root));

    /* sdb gains scx_io -> capability detected */
    p = g_build_filename(root, "block", "sdb", "queue", NULL);
    g_mkdir_with_parents(p, 0755);
    g_free(p);
    p = g_build_filename(root, "block", "sdb", "queue", "scheduler", NULL);
    g_file_set_contents(p, "none mq-deadline [scx_io]\n", -1, NULL);
    g_free(p);
    g_assert_true(pm_kernel_has_scx_io(root));

    g_assert_false(pm_kernel_has_scx_io("/nonexistent-cap-root"));

    p = g_build_filename(root, "block", "sdb", "queue", "scheduler", NULL);
    g_unlink(p);
    g_free(p);
    p = g_build_filename(root, "block", "sdb", "queue", NULL);
    g_rmdir(p);
    g_free(p);
    p = g_build_filename(root, "block", "sdb", NULL);
    g_rmdir(p);
    g_free(p);
    {
        gchar *q = g_build_filename(root, "block", "sda", "queue",
                                    "scheduler", NULL);
        g_unlink(q);
        g_free(q);
        q = g_build_filename(root, "block", "sda", "queue", NULL);
        g_rmdir(q);
        g_free(q);
        q = g_build_filename(root, "block", "sda", NULL);
        g_rmdir(q);
        g_free(q);
    }
    p = g_build_filename(root, "block", NULL);
    g_rmdir(p);
    g_free(p);
    g_rmdir(root);
    g_free(root);
}

/* ---------------- description tables ---------------- */

static void test_descriptions(void)
{
    const gchar *d;

    d = pm_cpu_strategy_description("scx_agent");
    g_assert_nonnull(d);
    g_assert_cmpstr(d, ==, "agent-aware sched_ext scheduler (acb)");

    /* v2 (8.1): "none" is EEVDF, never CFS */
    d = pm_cpu_strategy_description("none");
    g_assert_cmpstr(d, ==, "EEVDF (kernel default scheduling class)");

    /* v2 (8.5): legacy build name keeps a mirror description */
    d = pm_cpu_strategy_description("scx_acb");
    g_assert_cmpstr(d, ==, "scx_agent (legacy scx_acb build)");

    /* unknown strategies fall back to the file name at the caller */
    g_assert_null(pm_cpu_strategy_description("scx_nosuchthing"));

    d = pm_io_strategy_description("kyber");
    g_assert_cmpstr(d, ==, "Kyber I/O scheduler");
    g_assert_null(pm_io_strategy_description("bogus"));

    /* v2: iox (by full name and by suffix) and cax tables */
    d = pm_iox_strategy_description("iox_wedf");
    g_assert_nonnull(d);
    g_assert_nonnull(strstr(d, "EDF"));
    g_assert_nonnull(pm_iox_strategy_description("iox_merge"));
    g_assert_nonnull(pm_iox_strategy_description("none"));
    g_assert_null(pm_iox_strategy_description("iox_totallyunknown"));

    d = pm_cax_strategy_description("cax_mru");
    g_assert_nonnull(d);
    g_assert_nonnull(strstr(d, "recently"));
    g_assert_nonnull(pm_cax_strategy_description("cax_s3fifo"));
    g_assert_null(pm_cax_strategy_description("cax_nosuch"));
}

static void test_dev_name_valid(void)
{
    g_assert_true(pm_dev_name_valid("sda"));
    g_assert_true(pm_dev_name_valid("nvme0n1"));
    g_assert_true(pm_dev_name_valid("mmcblk-1_2"));
    g_assert_false(pm_dev_name_valid(""));
    g_assert_false(pm_dev_name_valid("sd/a"));
    g_assert_false(pm_dev_name_valid("sd a"));
    g_assert_false(pm_dev_name_valid("sda:b"));
    g_assert_false(pm_dev_name_valid(NULL));
}

/* ---------------- misc ---------------- */

static void test_kernel_release(void)
{
    gchar *k = pm_kernel_release();

    g_assert_nonnull(k);
    g_assert_cmpuint(strlen(k), >, 0);
    g_free(k);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sched/parse-basic", test_sched_parse_basic);
    g_test_add_func("/sched/parse-edge", test_sched_parse_edge_cases);
    g_test_add_func("/sched/file-roundtrip", test_sched_file_roundtrip);
    g_test_add_func("/sched/write-confirm", test_sched_write_confirm_mismatch);
    g_test_add_func("/device/filter", test_device_filter);
    g_test_add_func("/device/enumerate", test_enumerate_devices);
    g_test_add_func("/sysfs/paths", test_sysfs_paths);
    g_test_add_func("/sysfs/description", test_device_description);
    g_test_add_func("/state/roundtrip", test_state_roundtrip);
    g_test_add_func("/state/shutdown-write", test_state_shutdown_write);
    g_test_add_func("/config/defaults", test_config_defaults);
    g_test_add_func("/config/load", test_config_load);
    g_test_add_func("/config/drift", test_config_drift);
    g_test_add_func("/scan/prefix-exes", test_scan_prefix_exes);
    g_test_add_func("/io/effective-name", test_effective_io_strategy);
    g_test_add_func("/cache/cgroup-name", test_cache_cgroup_valid);
    g_test_add_func("/io/kernel-scx-io", test_kernel_has_scx_io);
    g_test_add_func("/desc/table", test_descriptions);
    g_test_add_func("/misc/dev-name", test_dev_name_valid);
    g_test_add_func("/misc/kernel-release", test_kernel_release);
    return g_test_run();
}
