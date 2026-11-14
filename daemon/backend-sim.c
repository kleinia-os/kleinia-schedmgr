// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * backend-sim.c — in-memory fake backend for development, demos and CI.
 *
 * Never touches sysfs, never spawns processes, skips polkit.  Implements
 * the v2 domain model: the global Io object is
 * an iox_* policy domain (single-select — device applies follow it),
 * device objects expose native ∪ iox combined strategy lists with
 * effective CurrentStrategy names, and the Cache domain runs the cax_*
 * fake policy set.  Sim1.Hotplug drives DevicesChanged and
 * Sim1.SetAvailable flips any object's Available flag for rehearsing
 * the unavailable path.
 */
#include <string.h>

#include "schedmgr-core.h"

static const struct { const gchar *name, *desc; } sim_cpu[] = {
    { "none",       "EEVDF (kernel default scheduling class)" },
    { "scx_agent",  "agent-aware sched_ext scheduler (acb)" },
    { "scx_simple", "simple sched_ext demo scheduler" },
    { "scx_central","central-pool sched_ext scheduler" },
    { NULL, NULL }
};

/* fake cax_* policies */
static const gchar *const sim_cax[] = {
    "cax_mru", "cax_lhd", "cax_s3fifo", NULL
};

/* fake global iox_* policies; "none" = stop */
static const gchar *const sim_iox[] = {
    "none", "iox_fifo", "iox_wedf", "iox_merge", NULL
};

static const gchar *const sim_io_scheds[] = {
    "none", "mq-deadline", "kyber", "bfq", "scx_io", NULL
};

static const struct { const gchar *dev, *desc, *current; } sim_devices[] = {
    { "sda",     "SATA SSD 512G", "kyber" },
    { "nvme0n1", "NVMe SSD 1T",   "kyber" },
    { "nvme1n1", "NVMe SSD 2T",   "kyber" },
    { NULL, NULL, NULL }
};

static gboolean sim_iox_known(const gchar *name)
{
    gint i;
    for (i = 0; sim_iox[i]; i++)
        if (g_strcmp0(sim_iox[i], name) == 0)
            return TRUE;
    return FALSE;
}

static gboolean sim_cax_known(const gchar *name)
{
    gint i;
    for (i = 0; sim_cax[i]; i++)
        if (g_strcmp0(sim_cax[i], name) == 0)
            return TRUE;
    return FALSE;
}

/* combined per-device list: native elevators ∪ global iox policies */
static gboolean sim_device_strategy_known(const gchar *name)
{
    gint i;

    for (i = 0; sim_io_scheds[i]; i++)
        if (g_strcmp0(sim_io_scheds[i], name) == 0)
            return TRUE;
    return g_str_has_prefix(name, "iox_") && sim_iox_known(name);
}

static void sim_list(PmApp *app, PmObj *o, GPtrArray *out)
{
    gint i;

    switch (o->kind) {
    case PM_KIND_CPU:
        for (i = 0; sim_cpu[i].name; i++) {
            PmStrategy *s = g_new0(PmStrategy, 1);
            s->name = g_strdup(sim_cpu[i].name);
            s->description = g_strdup(sim_cpu[i].desc);
            g_ptr_array_add(out, s);
        }
        break;
    case PM_KIND_CACHE:
        for (i = 0; sim_cax[i]; i++) {
            PmStrategy *s = g_new0(PmStrategy, 1);
            s->name = g_strdup(sim_cax[i]);
            s->description = g_strdup(pm_cax_strategy_description(sim_cax[i]));
            g_ptr_array_add(out, s);
        }
        break;
    case PM_KIND_IO:
        for (i = 0; sim_iox[i]; i++) {
            PmStrategy *s = g_new0(PmStrategy, 1);
            s->name = g_strdup(sim_iox[i]);
            s->description = g_strdup(pm_iox_strategy_description(sim_iox[i]));
            g_ptr_array_add(out, s);
        }
        break;
    case PM_KIND_DEVICE:
        for (i = 0; sim_io_scheds[i]; i++) {
            PmStrategy *s = g_new0(PmStrategy, 1);
            s->name = g_strdup(sim_io_scheds[i]);
            s->description = g_strdup(pm_io_strategy_description(sim_io_scheds[i]));
            g_ptr_array_add(out, s);
        }
        for (i = 0; sim_iox[i]; i++) {
            PmStrategy *s;
            const gchar *d;

            if (g_strcmp0(sim_iox[i], "none") == 0)
                continue;       /* native "none" already listed */
            s = g_new0(PmStrategy, 1);
            s->name = g_strdup(sim_iox[i]);
            d = pm_iox_strategy_description(sim_iox[i]);
            s->description = g_strdup_printf("%s (global eBPF policy)",
                                             d ? d : sim_iox[i]);
            g_ptr_array_add(out, s);
        }
        break;
    default:
        break;
    }
    (void)app;
}

static void sim_apply(PmApp *app, PmObj *o, gchar *strategy,
                      gchar **params, PmDoneFn done, gpointer ud)
{
    gboolean known = FALSE;

    switch (o->kind) {
    case PM_KIND_CPU: {
        gint i;
        for (i = 0; sim_cpu[i].name; i++)
            if (g_strcmp0(sim_cpu[i].name, strategy) == 0)
                known = TRUE;
        break;
    }
    case PM_KIND_CACHE:
        known = sim_cax_known(strategy);
        break;
    case PM_KIND_IO:
        known = sim_iox_known(strategy);
        break;
    case PM_KIND_DEVICE:
        known = sim_device_strategy_known(strategy);
        break;
    default:
        known = FALSE;
        break;
    }

    if (!known) {
        GError *e = g_error_new(PM_ERROR, PM_ERR_NO_SUCH_STRATEGY,
                                "no such strategy: %s", strategy);

        g_free(strategy);
        g_strfreev(params);
        done(app, o, "", NULL, e, ud);
        return;
    }

    switch (o->kind) {
    case PM_KIND_CPU:
    case PM_KIND_IO:
        /* "none" means "no policy loaded" -> CurrentStrategy == "" */
        if (g_strcmp0(strategy, "none") == 0) {
            g_free(strategy);
            g_strfreev(params);
            done(app, o, "", NULL, NULL, ud);
        } else {
            done(app, o, strategy, params, NULL, ud);  /* borrowed + owned */
            g_free(strategy);
        }
        return;
    case PM_KIND_DEVICE:
        if (g_str_has_prefix(strategy, "iox_")) {
            if (!app->io || !app->io->available) {
                /* kernel lacks the scx_io elevator */
                GError *e = g_error_new(PM_ERROR, PM_ERR_NOT_AVAILABLE,
                                        "kernel lacks iosched_ext "
                                        "(scx_io elevator)");

                g_free(strategy);
                g_strfreev(params);
                done(app, o, "", NULL, e, ud);
                return;
            }
            /* ①global single-select: the iox policy is unique system
             * wide, the last apply wins ②device sits on
             * the scx_io elevator ③CurrentStrategy = effective name */
            pm_obj_set_current(app->io, strategy, NULL);
            g_free(o->raw);
            o->raw = g_strdup("scx_io");
            done(app, o, strategy, NULL, NULL, ud);
            g_free(strategy);
            return;
        }
        g_free(o->raw);
        o->raw = g_strdup(strategy);
        done(app, o, strategy, params, NULL, ud);
        g_free(strategy);
        return;
    default:
        /* cache: plain memory state change, params (e.g. -w override)
         * are recorded in CurrentParams */
        done(app, o, strategy, params, NULL, ud);
        g_free(strategy);
        return;
    }
}

static void sim_stop(PmApp *app, PmObj *o, PmDoneFn done, gpointer ud)
{
    /* pure memory model: stopping always yields "no policy running" */
    done(app, o, "", NULL, NULL, ud);
}

static gboolean sim_hotplug(PmApp *app, const gchar *action, const gchar *dev,
                            GError **error)
{
    if (g_strcmp0(action, "add") == 0) {
        PmObj *o;

        if (pm_app_find_device(app, dev)) {
            g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                        "device already exists: %s", dev);
            return FALSE;
        }
        if (!pm_dev_name_valid(dev)) {
            g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                        "invalid device name: %s", dev);
            return FALSE;
        }
        o = pm_app_add_device(app, dev, "simulated block device (hotplug)",
                              TRUE, "", "none", "none");
        pm_app_refresh_io_devices(app);
        pm_app_devices_changed(app);
        (void)o;
        return TRUE;
    }
    if (g_strcmp0(action, "remove") == 0) {
        if (!pm_app_find_device(app, dev)) {
            g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                        "no such device: %s", dev);
            return FALSE;
        }
        pm_app_remove_device(app, dev);
        pm_app_devices_changed(app);
        return TRUE;
    }
    g_set_error(error, PM_ERROR, PM_ERR_NOT_SUPPORTED,
                "hotplug action must be 'add' or 'remove', not '%s'", action);
    return FALSE;
}

static void sim_startup(PmApp *app)
{
    gint i;

    app->cpu = pm_obj_new(app, PM_KIND_CPU, "Cpu");
    app->cpu->available = TRUE;

    app->cache = pm_obj_new(app, PM_KIND_CACHE, "Cache");
    app->cache->available = TRUE;

    app->io = pm_obj_new(app, PM_KIND_IO, "Io");
    app->io->available = TRUE;   /* sim pretends the scx_io elevator exists */

    for (i = 0; sim_devices[i].dev; i++) {
        pm_app_add_device(app, sim_devices[i].dev, sim_devices[i].desc,
                          TRUE, "", sim_devices[i].current,
                          sim_devices[i].current);
    }
}

const PmBackend pm_sim_backend = {
    .id = "sim",
    .list_strategies = sim_list,
    .apply = sim_apply,
    .stop = sim_stop,
    .startup = sim_startup,
    .shutdown = NULL,
    .hotplug = sim_hotplug,
};
