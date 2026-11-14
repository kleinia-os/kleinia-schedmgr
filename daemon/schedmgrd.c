// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * schedmgrd — kleinia policy manager daemon.
 *
 * Exposes io.kleinia.SchedMgr on the system bus (or, for testing,
 * the session bus / a direct --bus-address) and manages the three hot-
 * switchable kernel policy domains: CPU scheduling (sched_ext), page
 * cache eviction (cache_ext, v1 placeholder) and per-block-device IO
 * schedulers.  Run with --backend=sim for an in-memory simulation.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schedmgr-core.h"

static volatile sig_atomic_t stop_flag;

static void on_unix_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
"Usage: schedmgrd [OPTION...]\n"
"\n"
"kleinia policy manager daemon.\n"
"\n"
"Options:\n"
"  --backend=system|sim   backend to use (default: system)\n"
"  --session              attach to the session bus (testing)\n"
"  --system               attach to the system bus (default)\n"
"  --bus-address=ADDRESS  listen on ADDRESS (e.g. unix:path=/tmp/pm.sock)\n"
"                         instead of taking a bus name (testing)\n"
"  --config=PATH          config file (default: /etc/kleinia/schedmgr.conf)\n"
"  --state-dir=PATH       state directory (default: /var/lib/kleinia/schedmgr)\n"
"  --version              print version and exit\n"
"  -h, --help             show this help\n");
}

static gboolean stop_check_cb(gpointer ud)
{
    PmApp *app = ud;

    if (stop_flag) {
        g_print("schedmgrd: termination requested\n");
        g_main_loop_quit(app->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    PmApp app;
    const gchar *config = NULL, *state_dir = NULL, *backend = "system";
    const gchar *bus_address = NULL;
    gboolean session = FALSE;
    int i;
    guint j;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("schedmgrd 1\n");
            return 0;
        } else if (strncmp(argv[i], "--backend=", 10) == 0) {
            backend = argv[i] + 10;
        } else if (strncmp(argv[i], "--config=", 9) == 0) {
            config = argv[i] + 9;
        } else if (strncmp(argv[i], "--state-dir=", 12) == 0) {
            state_dir = argv[i] + 12;
        } else if (strncmp(argv[i], "--bus-address=", 14) == 0) {
            bus_address = argv[i] + 14;
        } else if (strcmp(argv[i], "--session") == 0) {
            session = TRUE;
        } else if (strcmp(argv[i], "--system") == 0) {
            session = FALSE;
        } else {
            fprintf(stderr, "schedmgrd: unknown option: %s\n\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    if (strcmp(backend, "sim") != 0 && strcmp(backend, "system") != 0) {
        fprintf(stderr, "schedmgrd: unknown backend '%s' (use system or sim)\n",
                backend);
        return 2;
    }

    memset(&app, 0, sizeof(app));
    app.loop = g_main_loop_new(NULL, FALSE);
    app.session_bus = session;
    app.backend_is_sim = strcmp(backend, "sim") == 0;
    app.backend = app.backend_is_sim ? &pm_sim_backend : &pm_system_backend;
    app.direct_mode = bus_address != NULL;
    app.bus_address = g_strdup(bus_address ? bus_address : "");
    app.kernel_release = pm_kernel_release();

    pm_config_defaults(&app.cfg);
    /* --config not given: fall back to the documented default path.
     * pm_config_load(NULL) would silently keep compiled-in defaults
     * and /etc/kleinia/schedmgr.conf would never be read. */
    pm_config_load(&app.cfg, config ? config : "/etc/kleinia/schedmgr.conf");
    if (state_dir && *state_dir) {
        g_free(app.cfg.state_dir);
        app.cfg.state_dir = g_strdup(state_dir);
    }

    app.conns = g_ptr_array_new();
    app.devices = g_ptr_array_new();    /* PmObj pointers, not owned here */
    app.graveyard = g_ptr_array_new();  /* removed PmObj objects */
    app.root = pm_obj_new(&app, PM_KIND_ROOT, "SchedMgr");

    app.backend->startup(&app);
    pm_dbus_setup(&app);

    g_print("schedmgrd: starting (backend=%s, bus=%s)\n", backend,
            app.direct_mode ? app.bus_address :
            (app.session_bus ? "session" : "system"));

    signal(SIGINT, on_unix_signal);
    signal(SIGTERM, on_unix_signal);
    g_timeout_add(200, stop_check_cb, &app);

    app.running = TRUE;
    g_main_loop_run(app.loop);
    app.running = FALSE;

    g_print("schedmgrd: shutting down\n");
    if (app.backend->shutdown)
        app.backend->shutdown(&app);
    pm_dbus_teardown(&app);

    pm_obj_free(app.root);
    pm_obj_free(app.cpu);
    pm_obj_free(app.cache);
    pm_obj_free(app.io);
    for (j = 0; j < app.devices->len; j++)
        pm_obj_free(g_ptr_array_index(app.devices, j));
    for (j = 0; j < app.graveyard->len; j++)
        pm_obj_free(g_ptr_array_index(app.graveyard, j));
    g_ptr_array_free(app.devices, TRUE);
    g_ptr_array_free(app.graveyard, TRUE);
    g_ptr_array_free(app.conns, TRUE);
    pm_config_free(&app.cfg);
    g_free(app.kernel_release);
    g_free(app.bus_address);
    g_main_loop_unref(app.loop);
    return 0;
}
