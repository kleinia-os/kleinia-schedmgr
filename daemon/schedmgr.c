// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * schedmgr — command line client for io.kleinia.SchedMgr.
 *
 * Talks to the daemon through plain GDBus method calls and property
 * reads.  Defaults to the system bus; use --session (or --bus-address)
 * against a daemon started with schedmgrd --session / --backend=sim.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "schedmgr-core.h"

static GDBusConnection *conn;
static gboolean opt_session;
static gchar *opt_address;

/* ---------------- small helpers ---------------- */

static void die(const gchar *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "schedmgr: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* print a GLib remote error as "name: message" */
static void die_gerror(GError *e)
{
    const gchar *msg = e->message;

    if (g_str_has_prefix(msg, "GDBus.Error:"))
        msg += strlen("GDBus.Error:");
    die("%s", msg);
}

static void die_usage(void);

static GVariant *dbus_call(const gchar *path, const gchar *iface,
                           const gchar *method, GVariant *params,
                           const GVariantType *reply)
{
    GError *e = NULL;
    GVariant *r;

    r = g_dbus_connection_call_sync(conn, PM_BUS_NAME, path, iface, method,
                                    params, reply, G_DBUS_CALL_FLAGS_NONE,
                                    120000, NULL, &e);
    if (!r)
        die_gerror(e);
    return r;
}

static GVariant *get_prop_v(const gchar *path, const gchar *iface,
                            const gchar *name)
{
    GVariant *r, *child, *v;

    r = dbus_call(path, PM_IFACE_PROPS, "Get",
                  g_variant_new("(ss)", iface, name),
                  G_VARIANT_TYPE("(v)"));
    child = g_variant_get_child_value(r, 0);
    v = g_variant_get_variant(child);
    g_variant_unref(child);
    g_variant_unref(r);
    return v;
}

static gchar *get_prop_s(const gchar *path, const gchar *iface,
                         const gchar *name)
{
    GVariant *v = get_prop_v(path, iface, name);
    gchar *s = g_strdup(g_variant_get_string(v, NULL));
    g_variant_unref(v);
    return s;
}

static gboolean get_prop_b(const gchar *path, const gchar *iface,
                           const gchar *name)
{
    GVariant *v = get_prop_v(path, iface, name);
    gboolean b = g_variant_get_boolean(v);
    g_variant_unref(v);
    return b;
}

static guint get_prop_u(const gchar *path, const gchar *iface,
                        const gchar *name)
{
    GVariant *v = get_prop_v(path, iface, name);
    guint u = g_variant_get_uint32(v);
    g_variant_unref(v);
    return u;
}

static gchar **get_prop_as(const gchar *path, const gchar *iface,
                           const gchar *name)
{
    GVariant *v = get_prop_v(path, iface, name);
    const gchar *const *sv = g_variant_get_strv(v, NULL);
    gchar **s = g_strdupv((gchar **)sv);

    g_variant_unref(v);
    return s;
}

/* ---------------- domain helpers ---------------- */

static const struct {
    const gchar *cmd;
    const gchar *name;     /* object path segment */
    const gchar *display;
} domains[] = {
    { "cpu",   "Cpu",   "Cpu" },
    { "cache", "Cache", "Cache" },
    { "io",    "Io",    "Io" },
    { NULL, NULL, NULL }
};

static gchar *domain_obj_path(const gchar *cmd)
{
    gint i;
    for (i = 0; domains[i].cmd; i++)
        if (strcmp(domains[i].cmd, cmd) == 0)
            return g_strdup_printf(PM_ROOT_PATH "/%s", domains[i].name);
    return NULL;
}

static gchar *device_obj_path(const gchar *dev)
{
    return g_strdup_printf(PM_ROOT_PATH "/Io/%s", dev);
}

/* ---------------- commands ---------------- */

static void cmd_status(void)
{
    guint version = get_prop_u(PM_ROOT_PATH, PM_IFACE_MANAGER, "Version");
    gchar *backend = get_prop_s(PM_ROOT_PATH, PM_IFACE_MANAGER, "Backend");
    gchar *kernel = get_prop_s(PM_ROOT_PATH, PM_IFACE_MANAGER, "KernelRelease");
    gint i;

    printf("schedmgrd: %s (version %u)\n", PM_BUS_NAME, version);
    printf("  Backend:       %s\n", backend);
    printf("  Kernel:        %s\n", kernel);

    for (i = 0; domains[i].cmd; i++) {
        gchar *path = domain_obj_path(domains[i].cmd);
        gchar *desc = get_prop_s(path, PM_IFACE_POLICY, "Description");
        gchar *cur = get_prop_s(path, PM_IFACE_POLICY, "CurrentStrategy");
        gchar **params = get_prop_as(path, PM_IFACE_POLICY, "CurrentParams");
        gchar *def = get_prop_s(path, PM_IFACE_POLICY, "DefaultStrategy");
        gboolean avail = get_prop_b(path, PM_IFACE_POLICY, "Available");
        gchar *status = get_prop_s(path, PM_IFACE_POLICY, "StatusMessage");

        printf("\nDomain: %s (%s)\n", domains[i].display, path);
        printf("  Description:   %s\n", desc);
        if (cur && *cur) {
            if (params && params[0]) {
                gchar *j = g_strjoinv(" ", params);
                printf("  Current:       %s (params: %s)\n", cur, j);
                g_free(j);
            } else {
                printf("  Current:       %s\n", cur);
            }
        } else {
            printf("  Current:       (none)\n");
        }
        printf("  Default:       %s\n", (def && *def) ? def : "(none)");
        printf("  Available:     %s\n", avail ? "yes" : "no");
        printf("  Status:        %s\n", (status && *status) ? status : "(ok)");
        g_free(path);
        g_free(desc);
        g_free(cur);
        g_strfreev(params);
        g_free(def);
        g_free(status);
    }

    /* device table */
    {
        GVariant *devs = get_prop_v(PM_ROOT_PATH "/Io", PM_IFACE_IO, "Devices");
        gsize n = g_variant_n_children(devs), k;

        printf("\nBlock devices (%lu):\n", (unsigned long)n);
        for (k = 0; k < n; k++) {
            const gchar *opath;
            gchar *cur, *desc;
            const gchar *name;

            g_variant_get_child(devs, k, "&o", &opath);
            cur = get_prop_s(opath, PM_IFACE_POLICY, "CurrentStrategy");
            desc = get_prop_s(opath, PM_IFACE_POLICY, "Description");
            name = strrchr(opath, '/');
            name = name ? name + 1 : opath;
            printf("  %-14s %-11s %s\n", name, (cur && *cur) ? cur : "(none)",
                   desc ? desc : "");
            g_free(cur);
            g_free(desc);
        }
        g_variant_unref(devs);
    }
    g_free(backend);
    g_free(kernel);
}

static void cmd_list_path(const gchar *path, const gchar *label)
{
    GVariant *r = dbus_call(path, PM_IFACE_POLICY, "ListStrategies", NULL,
                            G_VARIANT_TYPE("(a(ss))"));
    GVariant *arr = g_variant_get_child_value(r, 0);
    gsize n = g_variant_n_children(arr), i;

    printf("%s strategies (%lu):\n", label, (unsigned long)n);
    for (i = 0; i < n; i++) {
        const gchar *name, *desc;

        g_variant_get_child(arr, i, "(&s&s)", &name, &desc);
        printf("  %-16s %s\n", name, desc);
    }
    if (n == 0)
        printf("  (none)\n");
    g_variant_unref(arr);
    g_variant_unref(r);
}

static void cmd_list(const gchar *dom)
{
    gchar *path = domain_obj_path(dom);

    cmd_list_path(path, dom);
    g_free(path);
}

static void cmd_apply(const gchar *dom, const gchar *name, gchar **params)
{
    gchar *path = domain_obj_path(dom);
    const gchar *label = dom;

    if (params && params[0]) {
        dbus_call(path, PM_IFACE_POLICY, "ApplyStrategyWithParams",
                  g_variant_new("(s^as)", name, params), G_VARIANT_TYPE("()"));
        {
            gchar *j = g_strjoinv(" ", params);
            printf("%s: applied '%s' (params: %s)\n", label, name, j);
            g_free(j);
        }
    } else {
        dbus_call(path, PM_IFACE_POLICY, "ApplyStrategy",
                  g_variant_new("(s)", name), G_VARIANT_TYPE("()"));
        printf("%s: applied '%s'\n", label, name);
    }
    g_free(path);
}

static void cmd_device_apply(const gchar *dev, const gchar *sched)
{
    gchar *path = device_obj_path(dev);

    dbus_call(path, PM_IFACE_POLICY, "ApplyStrategy",
              g_variant_new("(s)", sched), G_VARIANT_TYPE("()"));
    printf("io/%s: applied '%s'\n", dev, sched);
    g_free(path);
}

static void cmd_domain_simple(const gchar *dom, const gchar *method,
                              const gchar *action)
{
    gchar *path = domain_obj_path(dom);

    dbus_call(path, PM_IFACE_POLICY, method, NULL, G_VARIANT_TYPE("()"));
    printf("%s: %s\n", dom, action);
    g_free(path);
}

static void cmd_stop(const gchar *dom, const gchar *dev)
{
    if (dev) {
        gchar *path = device_obj_path(dev);

        dbus_call(path, PM_IFACE_POLICY, "StopStrategy", NULL,
                  G_VARIANT_TYPE("()"));
        printf("io/%s: stopped (default scheduler restored)\n", dev);
        g_free(path);
    } else {
        cmd_domain_simple(dom, "StopStrategy", "stopped");
    }
}

static void cmd_restart(const gchar *dom, const gchar *dev)
{
    if (dev) {
        gchar *path = device_obj_path(dev);

        dbus_call(path, PM_IFACE_POLICY, "RestartStrategy", NULL,
                  G_VARIANT_TYPE("()"));
        printf("io/%s: restarted\n", dev);
        g_free(path);
    } else {
        cmd_domain_simple(dom, "RestartStrategy", "restarted");
    }
}

static void cmd_default(const gchar *dom, const gchar *dev)
{
    gchar *path = dev ? device_obj_path(dev) : domain_obj_path(dom);
    gchar *label = dev ? g_strdup_printf("io/%s", dev) : g_strdup(dom);
    gchar *def = get_prop_s(path, PM_IFACE_POLICY, "DefaultStrategy");

    dbus_call(path, PM_IFACE_POLICY, "RestoreDefault", NULL,
              G_VARIANT_TYPE("()"));
    printf("%s: applied default '%s'\n", label, (def && *def) ? def : "");
    g_free(path);
    g_free(label);
    g_free(def);
}

static void cmd_io_devices(void)
{
    GVariant *devs = get_prop_v(PM_ROOT_PATH "/Io", PM_IFACE_IO, "Devices");
    gsize n = g_variant_n_children(devs), k;

    printf("Block devices (%lu):\n", (unsigned long)n);
    for (k = 0; k < n; k++) {
        const gchar *opath;
        gchar *cur, *desc;
        const gchar *name;

        g_variant_get_child(devs, k, "&o", &opath);
        cur = get_prop_s(opath, PM_IFACE_POLICY, "CurrentStrategy");
        desc = get_prop_s(opath, PM_IFACE_POLICY, "Description");
        name = strrchr(opath, '/');
        name = name ? name + 1 : opath;
        printf("  %-14s %-11s %s\n", name, (cur && *cur) ? cur : "(none)",
               desc ? desc : "");
        g_free(cur);
        g_free(desc);
    }
    g_variant_unref(devs);
}

static void cmd_sim_hotplug(const gchar *action, const gchar *dev)
{
    gchar *backend = get_prop_s(PM_ROOT_PATH, PM_IFACE_MANAGER, "Backend");

    if (g_strcmp0(backend, "sim") != 0) {
        g_free(backend);
        die("sim-hotplug requires a daemon running with --backend=sim");
    }
    g_free(backend);
    if (strcmp(action, "add") != 0 && strcmp(action, "remove") != 0)
        die("sim-hotplug action must be 'add' or 'remove'");
    dbus_call(PM_ROOT_PATH, PM_IFACE_SIM, "Hotplug",
              g_variant_new("(ss)", action, dev), G_VARIANT_TYPE("()"));
    printf("sim: device %s '%s'\n",
           strcmp(action, "add") == 0 ? "added" : "removed", dev);
}

/* sim-only knob: flip Available of a domain or device to rehearse the
 * NotAvailable error path */
static void cmd_sim_available(const gchar *what, const gchar *onoff)
{
    const gchar *obj;
    gboolean avail;

    if (g_strcmp0(what, "cpu") == 0)
        obj = "Cpu";
    else if (g_strcmp0(what, "cache") == 0)
        obj = "Cache";
    else if (g_strcmp0(what, "io") == 0)
        obj = "Io";
    else if (pm_dev_name_valid(what))
        obj = what;
    else
        die("sim-available: expected cpu|cache|io|<device>, not '%s'", what);

    if (g_strcmp0(onoff, "on") == 0 || g_strcmp0(onoff, "true") == 0)
        avail = TRUE;
    else if (g_strcmp0(onoff, "off") == 0 || g_strcmp0(onoff, "false") == 0)
        avail = FALSE;
    else
        die("sim-available: state must be 'on' or 'off'");

    dbus_call(PM_ROOT_PATH, PM_IFACE_SIM, "SetAvailable",
              g_variant_new("(sb)", obj, avail), G_VARIANT_TYPE("()"));
    printf("sim: %s available=%s\n", what, avail ? "yes" : "no");
}

/* ---------------- monitor ---------------- */

static void on_policy_signal(GDBusConnection *c, const gchar *sender,
                             const gchar *path, const gchar *iface,
                             const gchar *member, GVariant *params,
                             gpointer ud)
{
    (void)c;
    (void)sender;
    (void)iface;
    (void)ud;
    if (g_strcmp0(member, "StrategyChanged") == 0) {
        const gchar *s;
        gchar **ps;
        gchar *j;

        g_variant_get(params, "(&s^as)", &s, &ps);
        j = g_strjoinv(" ", ps);
        printf("%s: StrategyChanged strategy='%s' params=[%s]\n", path, s, j);
        fflush(stdout);
        g_free(j);
        g_strfreev(ps);
    } else if (g_strcmp0(member, "StrategyFailed") == 0) {
        const gchar *s, *reason;

        g_variant_get(params, "(&s&s)", &s, &reason);
        printf("%s: StrategyFailed strategy='%s' reason='%s'\n", path, s,
               reason);
        fflush(stdout);
    }
}

static void on_props_signal(GDBusConnection *c, const gchar *sender,
                            const gchar *path, const gchar *iface,
                            const gchar *member, GVariant *params,
                            gpointer ud)
{
    const gchar *prop_iface;
    GVariant *dict;
    GVariantIter iter;
    gchar *key;
    GVariant *val;
    GString *keys;

    (void)c;
    (void)sender;
    (void)iface;
    (void)member;
    (void)ud;
    g_variant_get(params, "(&s@a{sv}as)", &prop_iface, &dict, NULL);
    keys = g_string_new("");
    g_variant_iter_init(&iter, dict);
    while (g_variant_iter_loop(&iter, "{&sv}", &key, &val)) {
        if (keys->len)
            g_string_append(keys, " ");
        g_string_append(keys, key);
    }
    printf("%s: PropertiesChanged iface=%s %s\n", path, prop_iface,
           keys->str);
    fflush(stdout);
    g_string_free(keys, TRUE);
    g_variant_unref(dict);
}

static void on_devices_signal(GDBusConnection *c, const gchar *sender,
                              const gchar *path, const gchar *iface,
                              const gchar *member, GVariant *params,
                              gpointer ud)
{
    (void)c;
    (void)sender;
    (void)iface;
    (void)member;
    (void)params;
    (void)ud;
    printf("%s: DevicesChanged\n", path);
    fflush(stdout);
}

static gboolean monitor_quit_cb(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void cmd_monitor(int seconds)
{
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    const gchar *sender = opt_address ? NULL : PM_BUS_NAME;

    g_dbus_connection_signal_subscribe(conn, sender, PM_IFACE_POLICY, NULL,
                                       NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_policy_signal, NULL, NULL);
    g_dbus_connection_signal_subscribe(conn, sender, PM_IFACE_PROPS,
                                       "PropertiesChanged", NULL, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_props_signal, NULL, NULL);
    g_dbus_connection_signal_subscribe(conn, sender, PM_IFACE_IO,
                                       "DevicesChanged", NULL, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_devices_signal, NULL, NULL);
    printf("monitoring %s for %ds\n", PM_BUS_NAME, seconds);
    fflush(stdout);
    g_timeout_add_seconds(seconds, monitor_quit_cb, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

/* ---------------- main / usage ---------------- */

static void usage(FILE *out)
{
    fprintf(out,
"Usage: schedmgr [--session|--system|--bus-address=ADDRESS] COMMAND [ARGS...]\n"
"\n"
"Commands:\n"
"  status                          overall status of every domain + devices\n"
"  cpu|cache|io list               list available strategies\n"
"  cpu|cache apply NAME [PARAM...] apply a strategy (params: scx_*/cax_* argv;\n"
"                                  cache params may override the loader -w)\n"
"  cpu|cache|io stop               stop / unload the current policy\n"
"  cpu|cache|io restart            restart the current strategy\n"
"  cpu|cache|io default            apply the configured default\n"
"  io list [DEV]                   global iox policies, or DEV's combined\n"
"                                  native+iox list\n"
"  io policy list                  global IO policy list (= io list)\n"
"  io policy apply NAME [PARAM...] apply a global iox_* policy (single)\n"
"  io policy stop                  stop the global iox_* policy\n"
"  io stop|restart|default [DEV]   global policy, or DEV's (device default)\n"
"  io devices                      list block devices and their scheduler\n"
"  io apply DEV SCHED              native elevator or iox_* policy per device\n"
"  sim-hotplug add|remove DEV      simulate device hotplug (sim backend)\n"
"  sim-available cpu|cache|io|DEV on|off   flip Available (sim backend)\n"
"  monitor [SECONDS]               print signals as they arrive (default 10)\n"
"\n"
"Bus selection defaults to the system bus; --session selects the session\n"
"bus and --bus-address connects to a daemon started with a direct address.\n");
}

static void die_usage(void)
{
    usage(stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    gint first = 1;
    GError *e = NULL;

    while (first < argc && argv[first][0] == '-') {
        if (strcmp(argv[first], "--session") == 0) {
            opt_session = TRUE;
        } else if (strcmp(argv[first], "--system") == 0) {
            opt_session = FALSE;
        } else if (strncmp(argv[first], "--bus-address=", 14) == 0) {
            g_free(opt_address);
            opt_address = g_strdup(argv[first] + 14);
        } else if (strcmp(argv[first], "--help") == 0 ||
                   strcmp(argv[first], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "schedmgr: unknown option: %s\n\n", argv[first]);
            die_usage();
        }
        first++;
    }
    if (first >= argc)
        die_usage();

    if (opt_address)
        conn = g_dbus_connection_new_for_address_sync(
            opt_address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL,
            NULL, &e);
    else
        conn = g_bus_get_sync(opt_session ? G_BUS_TYPE_SESSION
                                          : G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!conn) {
        fprintf(stderr, "schedmgr: cannot connect: %s\n", e->message);
        return 1;
    }

    if (strcmp(argv[first], "status") == 0 && argc - first == 1) {
        cmd_status();
    } else if (strcmp(argv[first], "monitor") == 0 && argc - first <= 2) {
        int secs = (argc - first == 2) ? atoi(argv[first + 1]) : 10;

        if (secs <= 0)
            die("monitor SECONDS must be a positive number");
        cmd_monitor(secs);
    } else if (strcmp(argv[first], "sim-hotplug") == 0 &&
               argc - first == 3) {
        cmd_sim_hotplug(argv[first + 1], argv[first + 2]);
    } else if (strcmp(argv[first], "sim-available") == 0 &&
               argc - first == 3) {
        cmd_sim_available(argv[first + 1], argv[first + 2]);
    } else if (strcmp(argv[first], "io") == 0 && argc - first >= 2) {
        const gchar *sub = argv[first + 1];

        if (strcmp(sub, "devices") == 0 && argc - first == 2) {
            cmd_io_devices();
        } else if (strcmp(sub, "policy") == 0 && argc - first >= 3) {
            /* global iox policy namespace */
            const gchar *psub = argv[first + 2];

            if (strcmp(psub, "list") == 0 && argc - first == 3) {
                cmd_list("io");
            } else if (strcmp(psub, "apply") == 0 && argc - first >= 4) {
                cmd_apply("io", argv[first + 3], argv + first + 4);
            } else if (strcmp(psub, "stop") == 0 && argc - first == 3) {
                cmd_domain_simple("io", "StopStrategy", "stopped");
            } else {
                die_usage();
            }
        } else if (strcmp(sub, "apply") == 0 && argc - first == 4) {
            if (!pm_dev_name_valid(argv[first + 2]))
                die("invalid device name: %s", argv[first + 2]);
            cmd_device_apply(argv[first + 2], argv[first + 3]);
        } else if ((strcmp(sub, "stop") == 0 || strcmp(sub, "restart") == 0 ||
                    strcmp(sub, "default") == 0) &&
                   argc - first <= 3) {
            const gchar *dev = (argc - first == 3) ? argv[first + 2] : NULL;

            if (dev && !pm_dev_name_valid(dev))
                die("invalid device name: %s", dev);
            if (strcmp(sub, "stop") == 0)
                cmd_stop("io", dev);
            else if (strcmp(sub, "restart") == 0)
                cmd_restart("io", dev);
            else
                cmd_default("io", dev);
        } else if (strcmp(sub, "list") == 0 && argc - first <= 3) {
            if (argc - first == 3) {
                gchar *path;

                if (!pm_dev_name_valid(argv[first + 2]))
                    die("invalid device name: %s", argv[first + 2]);
                path = device_obj_path(argv[first + 2]);
                cmd_list_path(path, argv[first + 2]);
                g_free(path);
            } else {
                cmd_list("io");
            }
        } else {
            die_usage();
        }
    } else if ((strcmp(argv[first], "cpu") == 0 ||
                strcmp(argv[first], "cache") == 0) &&
               argc - first >= 2) {
        const gchar *dom = argv[first];
        const gchar *sub = argv[first + 1];

        if (strcmp(sub, "list") == 0 && argc - first == 2) {
            cmd_list(dom);
        } else if (strcmp(sub, "apply") == 0 && argc - first >= 3) {
            cmd_apply(dom, argv[first + 2], argv + first + 3);
        } else if (strcmp(sub, "stop") == 0 && argc - first == 2) {
            cmd_domain_simple(dom, "StopStrategy", "stopped");
        } else if (strcmp(sub, "restart") == 0 && argc - first == 2) {
            cmd_domain_simple(dom, "RestartStrategy", "restarted");
        } else if (strcmp(sub, "default") == 0 && argc - first == 2) {
            cmd_default(dom, NULL);
        } else {
            die_usage();
        }
    } else {
        die_usage();
    }

    g_object_unref(conn);
    g_free(opt_address);
    return 0;
}
