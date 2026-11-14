// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * dbus-server.c — the io.kleinia.SchedMgr object tree.
 *
 * Object layout:
 *   /io/kleinia/SchedMgr            Manager1 (+ Sim1 in sim mode)
 *   /io/kleinia/SchedMgr/Cpu        Sched1
 *   /io/kleinia/SchedMgr/Cache      Sched1
 *   /io/kleinia/SchedMgr/Io         Sched1 + Io1 (Devices list)
 *   /io/kleinia/SchedMgr/Io/<dev>   Sched1 (per block device)
 *
 * Every object also implements the standard Properties / Introspectable /
 * Peer interfaces by hand (GDBus passes them to the method_call vtable
 * when they are registered here).  Properties are read-only; a Set call
 * is answered with org.freedesktop.DBus.Error.PropertyReadOnly.
 *
 * The state-consistency rule is enforced in exactly one
 * place, pm_apply_done(): properties and StrategyChanged are updated
 * only after the backend confirmed the change, and every failed apply
 * emits StrategyFailed instead.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "schedmgr-core.h"

/* ---------------- introspection fragments ---------------- */

#define XML_POLICY \
"<interface name='" PM_IFACE_POLICY "'>\n" \
"    <method name='ListStrategies'>\n" \
"      <arg name='strategies' type='a(ss)' direction='out'/>\n" \
"    </method>\n" \
"    <method name='ApplyStrategy'>\n" \
"      <arg name='strategy' type='s' direction='in'/>\n" \
"    </method>\n" \
"    <method name='ApplyStrategyWithParams'>\n" \
"      <arg name='strategy' type='s' direction='in'/>\n" \
"      <arg name='params' type='as' direction='in'/>\n" \
"    </method>\n" \
"    <method name='StopStrategy'/>\n" \
"    <method name='RestartStrategy'/>\n" \
"    <method name='RestoreDefault'/>\n" \
"    <property name='CurrentStrategy' type='s' access='read'/>\n" \
"    <property name='CurrentParams' type='as' access='read'/>\n" \
"    <property name='DefaultStrategy' type='s' access='read'/>\n" \
"    <property name='Description' type='s' access='read'/>\n" \
"    <property name='Available' type='b' access='read'/>\n" \
"    <property name='StatusMessage' type='s' access='read'/>\n" \
"    <signal name='StrategyChanged'>\n" \
"      <arg name='strategy' type='s'/>\n" \
"      <arg name='params' type='as'/>\n" \
"    </signal>\n" \
"    <signal name='StrategyFailed'>\n" \
"      <arg name='strategy' type='s'/>\n" \
"      <arg name='reason' type='s'/>\n" \
"    </signal>\n" \
"  </interface>"

#define XML_MANAGER \
"<interface name='" PM_IFACE_MANAGER "'>\n" \
"    <property name='Version' type='u' access='read'/>\n" \
"    <property name='Backend' type='s' access='read'/>\n" \
"    <property name='KernelRelease' type='s' access='read'/>\n" \
"  </interface>"

#define XML_IO \
"<interface name='" PM_IFACE_IO "'>\n" \
"    <property name='Devices' type='ao' access='read'/>\n" \
"    <signal name='DevicesChanged'/>\n" \
"  </interface>"

#define XML_SIM \
"<interface name='" PM_IFACE_SIM "'>\n" \
"    <method name='Hotplug'>\n" \
"      <arg name='action' type='s' direction='in'/>\n" \
"      <arg name='device' type='s' direction='in'/>\n" \
"    </method>\n" \
"    <method name='SetAvailable'>\n" \
"      <arg name='object' type='s' direction='in'/>\n" \
"      <arg name='available' type='b' direction='in'/>\n" \
"    </method>\n" \
"  </interface>"

#define XML_PROPS \
"<interface name='org.freedesktop.DBus.Properties'>\n" \
"    <method name='Get'>\n" \
"      <arg name='interface_name' type='s' direction='in'/>\n" \
"      <arg name='property_name' type='s' direction='in'/>\n" \
"      <arg name='value' type='v' direction='out'/>\n" \
"    </method>\n" \
"    <method name='GetAll'>\n" \
"      <arg name='interface_name' type='s' direction='in'/>\n" \
"      <arg name='props' type='a{sv}' direction='out'/>\n" \
"    </method>\n" \
"    <method name='Set'>\n" \
"      <arg name='interface_name' type='s' direction='in'/>\n" \
"      <arg name='property_name' type='s' direction='in'/>\n" \
"      <arg name='value' type='v' direction='in'/>\n" \
"    </method>\n" \
"    <signal name='PropertiesChanged'>\n" \
"      <arg name='interface_name' type='s'/>\n" \
"      <arg name='changed_properties' type='a{sv}'/>\n" \
"      <arg name='invalidated_properties' type='as'/>\n" \
"    </signal>\n" \
"  </interface>"

#define XML_INTROSPECT \
"<interface name='org.freedesktop.DBus.Introspectable'>\n" \
"    <method name='Introspect'>\n" \
"      <arg name='xml_data' type='s' direction='out'/>\n" \
"    </method>\n" \
"  </interface>"

#define XML_PEER \
"<interface name='org.freedesktop.DBus.Peer'>\n" \
"    <method name='Ping'/>\n" \
"    <method name='GetMachineId'>\n" \
"      <arg name='machine_uuid' type='s' direction='out'/>\n" \
"    </method>\n" \
"  </interface>"

static GDBusInterfaceInfo *info_policy, *info_manager, *info_io, *info_sim,
                          *info_props, *info_introspect, *info_peer;

static void ensure_infos(void)
{
    struct { const gchar *xml; const gchar *name; GDBusInterfaceInfo **out; } tabs[] = {
        { XML_POLICY,     PM_IFACE_POLICY,    &info_policy },
        { XML_MANAGER,    PM_IFACE_MANAGER,   &info_manager },
        { XML_IO,         PM_IFACE_IO,        &info_io },
        { XML_SIM,        PM_IFACE_SIM,       &info_sim },
        { XML_PROPS,      PM_IFACE_PROPS,     &info_props },
        { XML_INTROSPECT, PM_IFACE_INTROSPECT,&info_introspect },
        { XML_PEER,       PM_IFACE_PEER,      &info_peer },
    };
    gsize i;

    if (info_policy)
        return;
    for (i = 0; i < G_N_ELEMENTS(tabs); i++) {
        gchar *node_xml = g_strdup_printf("<node>%s</node>", tabs[i].xml);
        GDBusNodeInfo *n = g_dbus_node_info_new_for_xml(node_xml, NULL);

        g_free(node_xml);
        if (!n) {
            g_error("schedmgrd: cannot parse built-in introspection for %s",
                    tabs[i].name);
            continue;
        }
        *tabs[i].out = g_dbus_interface_info_ref(
            g_dbus_node_info_lookup_interface(n, tabs[i].name));
        g_dbus_node_info_unref(n);
    }
}

/* ---------------- signal emission ---------------- */

static void emit_signal_all(PmApp *app, const gchar *path, const gchar *iface,
                             const gchar *member, GVariant *body)
{
    GVariant *b = body ? g_variant_ref_sink(body) : NULL;
    guint i;

    for (i = 0; i < app->conns->len; i++) {
        GError *e = NULL;
        if (!g_dbus_connection_emit_signal(
                g_ptr_array_index(app->conns, i), NULL, path, iface, member,
                b ? g_variant_ref(b) : NULL, &e)) {
            g_warning("schedmgrd: cannot emit %s.%s: %s", iface, member,
                      e ? e->message : "?");
            g_clear_error(&e);
        }
    }
    if (b)
        g_variant_unref(b);
}

static void emit_property_changed(PmApp *app, const gchar *path,
                                  const gchar *iface, const gchar *prop,
                                  GVariant *value /* floating */)
{
    GVariantBuilder b;

    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", prop, value);
    emit_signal_all(app, path, PM_IFACE_PROPS, "PropertiesChanged",
                    g_variant_new("(sa{sv}as)", iface, &b, NULL));
}

static GVariant *devices_variant(PmApp *app)
{
    GVariantBuilder b;
    guint i;

    g_variant_builder_init(&b, G_VARIANT_TYPE("ao"));
    for (i = 0; i < app->devices->len; i++) {
        PmObj *d = g_ptr_array_index(app->devices, i);
        g_variant_builder_add(&b, "o", d->path);
    }
    return g_variant_builder_end(&b);
}

static gchar *empty_strv[] = { NULL };

/* ---------------- object helpers ---------------- */

void pm_obj_set_current(PmObj *o, const gchar *strategy, gchar **params)
{
    PmApp *app = o->app;

    g_free(o->current);
    o->current = g_strdup(strategy ? strategy : "");
    g_strfreev(o->current_params);
    o->current_params = params;   /* takes ownership */

    emit_signal_all(app, o->path, PM_IFACE_POLICY, "StrategyChanged",
                    g_variant_new("(s^as)", o->current,
                                  o->current_params ?
                                      o->current_params :
                                      (gchar **)empty_strv));
    emit_property_changed(app, o->path, PM_IFACE_POLICY,
                          "CurrentStrategy", g_variant_new_string(o->current));
    emit_property_changed(app, o->path, PM_IFACE_POLICY,
                          "CurrentParams",
                          g_variant_new_strv((const gchar *const *)
                              (o->current_params ?
                                   o->current_params : empty_strv), -1));
    /* global IO policy changes propagate to every device sitting on the
     * scx_io elevator: their effective
     * CurrentStrategy/StatusMessage follow the running loader */
    if (o->kind == PM_KIND_IO)
        pm_app_refresh_io_devices(app);
}

void pm_app_refresh_io_devices(PmApp *app)
{
    const gchar *global;
    guint i;
    gboolean changed = FALSE;

    if (!app->io)
        return;
    global = app->io->current ? app->io->current : "";
    for (i = 0; i < app->devices->len; i++) {
        PmObj *d = g_ptr_array_index(app->devices, i);
        gboolean no_loader = FALSE;
        const gchar *status = "";
        gchar *eff = pm_effective_io_strategy(d->raw, global, &no_loader);

        if (no_loader)
            status = "scx_io elevator selected but no iox policy is running";
        if (g_strcmp0(eff, d->current) != 0) {
            pm_obj_set_current(d, eff, NULL);   /* takes a copy */
            changed = TRUE;
        }
        g_free(eff);
        if (g_strcmp0(status, d->status_message ? d->status_message : "") != 0) {
            g_free(d->status_message);
            d->status_message = g_strdup(status);
            emit_property_changed(app, d->path, PM_IFACE_POLICY,
                                  "StatusMessage",
                                  g_variant_new_string(status));
            changed = TRUE;
        }
    }
    if (changed)
        pm_app_persist_state(app);
}

void pm_obj_set_available(PmObj *o, gboolean available, const gchar *status)
{
    o->available = available;
    g_free(o->status_message);
    o->status_message = g_strdup(status ? status : "");
    emit_property_changed(o->app, o->path, PM_IFACE_POLICY, "Available",
                          g_variant_new_boolean(available));
    emit_property_changed(o->app, o->path, PM_IFACE_POLICY, "StatusMessage",
                          g_variant_new_string(o->status_message));
}

void pm_obj_report_failure(PmObj *o, const gchar *strategy, const gchar *reason)
{
    emit_signal_all(o->app, o->path, PM_IFACE_POLICY, "StrategyFailed",
                    g_variant_new("(ss)", strategy ? strategy : "",
                                  reason ? reason : ""));
    pm_obj_set_current(o, "", NULL);
    pm_app_persist_state(o->app);
}

void pm_app_persist_state(PmApp *app)
{
    GHashTable *io;
    GError *e = NULL;
    guint i;
    const gchar *io_global = NULL;

    if (app->backend_is_sim || !app->cpu)
        return;
    io = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (i = 0; i < app->devices->len; i++) {
        PmObj *d = g_ptr_array_index(app->devices, i);
        g_hash_table_insert(io, g_strdup(d->name), g_strdup(d->current));
    }
    if (app->io && app->io->current && *app->io->current)
        io_global = app->io->current;
    if (!pm_state_save(app->cfg.state_dir,
                       app->cpu->current, app->cpu->current_params,
                       app->cache ? app->cache->current : NULL,
                       app->cache ? app->cache->current_params : NULL,
                       io_global, io, &e)) {
        g_warning("schedmgrd: cannot save state to %s: %s",
                  app->cfg.state_dir, e ? e->message : "?");
        g_clear_error(&e);
    }
    g_hash_table_destroy(io);
}

void pm_obj_free(PmObj *o)
{
    guint i;

    if (!o)
        return;
    for (i = 0; i < o->regs->len; i++) {
        PmReg *r = g_ptr_array_index(o->regs, i);
        g_dbus_connection_unregister_object(r->conn, r->id);
    }
    g_ptr_array_free(o->regs, TRUE);
    g_free(o->path);
    g_free(o->name);
    g_free(o->description);
    g_free(o->status_message);
    g_free(o->current);
    g_strfreev(o->current_params);
    g_free(o->last_ok);
    g_strfreev(o->last_ok_params);
    g_free(o->default_strategy);
    g_free(o->raw);
    g_free(o);
}

PmObj *pm_obj_new(PmApp *app, PmObjKind kind, const gchar *name)
{
    PmObj *o = g_new0(PmObj, 1);

    o->app = app;
    o->kind = kind;
    o->regs = g_ptr_array_new();
    o->current = g_strdup("");
    o->status_message = g_strdup("");
    o->default_strategy = g_strdup("");
    o->name = g_strdup(name);

    switch (kind) {
    case PM_KIND_ROOT:
        o->path = g_strdup(PM_ROOT_PATH);
        o->description = g_strdup("schedmgr policy manager");
        break;
    case PM_KIND_CPU:
        o->path = g_strdup(PM_ROOT_PATH "/Cpu");
        o->description = g_strdup("CPU 调度器");
        g_free(o->default_strategy);
        o->default_strategy = g_strdup(app->cfg.cpu_default);
        break;
    case PM_KIND_CACHE:
        o->path = g_strdup(PM_ROOT_PATH "/Cache");
        o->description = g_strdup("页缓存策略");
        g_free(o->default_strategy);
        o->default_strategy = g_strdup(app->cfg.cache_default);
        break;
    case PM_KIND_IO:
        o->path = g_strdup(PM_ROOT_PATH "/Io");
        o->description = g_strdup("IO 策略");
        /* [Io] DefaultStrategy is a per-device *elevator* default; the
         * global iox policy domain has no configured default, so
         * RestoreDefault here reports NoDefault */
        break;
    case PM_KIND_DEVICE:
        o->path = g_strdup_printf(PM_ROOT_PATH "/Io/%s", name);
        o->description = g_strdup(name);
        {
            const gchar *def = g_hash_table_lookup(app->cfg.io_dev_defaults,
                                                   name);
            if (!def || !*def)
                def = app->cfg.io_default;
            g_free(o->default_strategy);
            o->default_strategy = g_strdup(def ? def : "");
        }
        break;
    }
    return o;
}

/* ---------------- apply / stop plumbing ---------------- */

typedef struct {
    PmObj *o;
    GDBusMethodInvocation *inv;   /* NULL for daemon-internal operations */
    gchar *attempted;
    PmNotifyFn notify;            /* optional internal completion hook */
    gpointer notify_ud;
} PmPending;

static void return_pm_error(GDBusMethodInvocation *inv, GError *e)
{
    g_dbus_method_invocation_return_dbus_error(inv, pm_err_name(e->code),
                                               e->message);
    g_error_free(e);
}

static void pm_apply_done(PmApp *app, PmObj *o, const gchar *confirmed,
                          gchar **params, GError *error, gpointer ud)
{
    PmPending *p = ud;
    gboolean ok = error == NULL;

    if (o->dead) {
        if (p->inv)
            g_dbus_method_invocation_return_dbus_error(
                p->inv, pm_err_name(PM_ERR_FAILED),
                "object was removed during the operation");
        g_clear_error(&error);
        g_strfreev(params);
        if (p->notify)
            p->notify(app, o, FALSE, p->notify_ud);
        g_free(p->attempted);
        g_free(p);
        return;
    }

    if (error) {
        emit_signal_all(app, o->path, PM_IFACE_POLICY, "StrategyFailed",
                        g_variant_new("(ss)", p->attempted, error->message));
        g_warning("schedmgrd: strategy '%s' failed on %s: %s", p->attempted,
                  o->name, error->message);
        if (p->inv)
            return_pm_error(p->inv, error);
        else
            g_error_free(error);
        g_strfreev(params);
        /* NOTE: `confirmed` is BORROWED per PmDoneFn contract in
         * schedmgr-core.h — must NOT be freed here. Freeing it aborted
         * the daemon (g_free on a static "" from cpu_fail) on every
         * failed apply. */
    } else {
        if (confirmed && *confirmed) {
            g_free(o->last_ok);
            o->last_ok = g_strdup(confirmed);
            g_strfreev(o->last_ok_params);
            o->last_ok_params = g_strdupv(params);
        }
        /* the single place where state + signals move forward */
        pm_obj_set_current(o, confirmed, params);
        pm_app_persist_state(app);
        if (p->inv)
            g_dbus_method_invocation_return_value(p->inv, NULL);
    }
    if (p->notify)
        p->notify(app, o, ok, p->notify_ud);
    g_free(p->attempted);
    g_free(p);
}

static void begin_confirmed_apply(PmObj *o, gchar *strategy /*owned*/,
                                  gchar **params /*owned*/,
                                  GDBusMethodInvocation *inv)
{
    PmPending *p = g_new0(PmPending, 1);

    p->o = o;
    p->inv = inv;
    p->attempted = g_strdup(strategy);
    o->app->backend->apply(o->app, o, strategy, params, pm_apply_done, p);
}

static void begin_apply_notify(PmObj *o, gchar *strategy /*owned*/,
                               gchar **params /*owned*/,
                               PmNotifyFn notify, gpointer notify_ud)
{
    PmPending *p = g_new0(PmPending, 1);

    p->o = o;
    p->attempted = g_strdup(strategy);
    p->notify = notify;
    p->notify_ud = notify_ud;
    o->app->backend->apply(o->app, o, strategy, params, pm_apply_done, p);
}

static void begin_confirmed_stop(PmObj *o, GDBusMethodInvocation *inv)
{
    PmPending *p = g_new0(PmPending, 1);

    p->o = o;
    p->inv = inv;
    p->attempted = g_strdup("");
    o->app->backend->stop(o->app, o, pm_apply_done, p);
}

void pm_backend_apply_async(PmObj *o, const gchar *strategy, gchar **params,
                            gpointer inv)
{
    begin_confirmed_apply(o, g_strdup(strategy), params,
                          (GDBusMethodInvocation *)inv);
}

void pm_backend_apply_async_notify(PmObj *o, const gchar *strategy,
                                   gchar **params, PmNotifyFn notify,
                                   gpointer notify_ud)
{
    begin_apply_notify(o, g_strdup(strategy), params, notify, notify_ud);
}

void pm_backend_stop_async(PmObj *o, gpointer inv)
{
    begin_confirmed_stop(o, (GDBusMethodInvocation *)inv);
}

/* ---------------- polkit ---------------- */

typedef struct {
    PmApp *app;
    PmObj *o;
    GDBusMethodInvocation *inv;  /* NULL for internal ops (no polkit) */
    gchar *strategy;             /* owned; NULL means "stop" */
    gchar **params;              /* owned */
} PmAuthReq;

static void polkit_check(PmAuthReq *r);

static void polkit_finish(PmAuthReq *r, gboolean authorized, const gchar *why)
{
    if (!authorized) {
        if (r->inv) {
            gchar *msg = g_strdup_printf("not authorized%s%s",
                                         why && *why ? ": " : "",
                                         why ? why : "");
            g_dbus_method_invocation_return_dbus_error(
                r->inv, pm_err_name(PM_ERR_FAILED), msg);
            g_free(msg);
        } else {
            g_warning("schedmgrd: internal operation not authorized: %s",
                      why ? why : "");
        }
        g_free(r->strategy);
        g_strfreev(r->params);
        g_free(r);
        return;
    }
    if (r->strategy)
        begin_confirmed_apply(r->o, r->strategy, r->params, r->inv);
    else
        begin_confirmed_stop(r->o, r->inv);
    g_free(r);
}

static void authz_cb(GObject *source, GAsyncResult *res, gpointer ud)
{
    PmAuthReq *r = ud;
    GVariant *reply, *details;
    GError *e = NULL;
    gboolean authorized = FALSE, challenge = FALSE;

    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &e);
    if (!reply) {
        /* no polkitd on the bus — deny */
        gchar *why = g_strdup_printf("polkit authority unavailable (%s)",
                                     e->message);
        g_error_free(e);
        polkit_finish(r, FALSE, why);
        g_free(why);
        return;
    }
    /* polkitd 127 returns ((bba{ss})); unwrap one tuple level */
    if (g_variant_is_of_type(reply, G_VARIANT_TYPE("((bba{ss}))"))) {
        GVariant *inner = g_variant_get_child_value(reply, 0);
        g_variant_unref(reply);
        reply = inner;
    }
    if (!g_variant_is_of_type(reply, G_VARIANT_TYPE("(bba{ss})"))) {
        g_variant_unref(reply);
        polkit_finish(r, FALSE, "unexpected CheckAuthorization reply");
        return;
    }
    g_variant_get(reply, "(bb@a{ss})", &authorized, &challenge, &details);
    g_variant_unref(details);
    g_variant_unref(reply);
    if (!authorized)
        polkit_finish(r, FALSE, challenge ? "authorization dismissed" : NULL);
    else
        polkit_finish(r, TRUE, NULL);
}

static void creds_cb(GObject *source, GAsyncResult *res, gpointer ud)
{
    PmAuthReq *r = ud;
    GVariant *reply, *dict;
    GVariantIter iter;
    gchar *key;
    GVariant *val;
    GError *e = NULL;
    guint32 pid = 0, uid = 0;
    GVariantBuilder subj, det;

    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &e);
    if (!reply) {
        gchar *why = g_strdup_printf("cannot determine caller credentials (%s)",
                                     e->message);
        g_error_free(e);
        polkit_finish(r, FALSE, why);
        g_free(why);
        return;
    }
    if (g_variant_is_of_type(reply, G_VARIANT_TYPE("(a{sv})")))
        dict = g_variant_get_child_value(reply, 0);
    else if (g_variant_is_of_type(reply, G_VARIANT_TYPE("a{sv}")))
        dict = g_variant_ref(reply);
    else {
        g_variant_unref(reply);
        polkit_finish(r, FALSE, "unexpected GetConnectionCredentials reply");
        return;
    }
    g_variant_iter_init(&iter, dict);
    while (g_variant_iter_loop(&iter, "{&sv}", &key, &val)) {
        if (g_strcmp0(key, "ProcessID") == 0) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32))
                pid = g_variant_get_uint32(val);
            else if (g_variant_is_of_type(val, G_VARIANT_TYPE_UINT64))
                pid = (guint32)g_variant_get_uint64(val);
        } else if (g_strcmp0(key, "UnixUserID") == 0 &&
                   g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32)) {
            uid = g_variant_get_uint32(val);
        }
    }
    g_variant_unref(dict);
    g_variant_unref(reply);
    if (pid == 0) {
        polkit_finish(r, FALSE, "caller did not provide a ProcessID");
        return;
    }

    /* CheckAuthorization on the polkit authority */
    g_variant_builder_init(&subj, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&subj, "{sv}", "pid", g_variant_new_uint32(pid));
    g_variant_builder_add(&subj, "{sv}", "start-time",
                          g_variant_new_uint64(pm_proc_starttime((pid_t)pid)));
    g_variant_builder_add(&subj, "{sv}", "uid", g_variant_new_uint32(uid));
    g_variant_builder_init(&det, G_VARIANT_TYPE("a{ss}"));

    g_dbus_connection_call(
        G_DBUS_CONNECTION(source), PM_POLKIT_BUS, PM_POLKIT_PATH,
        PM_POLKIT_IFACE, "CheckAuthorization",
        g_variant_new("((sa{sv})sa{ss}us)", "unix-process", &subj,
                      PM_POLKIT_ACTION, &det,
                      (guint)1 /* AllowUserInteraction */, ""),
        /* reply type varies: spec says (bba{ss}); polkitd 127 (as shipped
         * on kleinia) wraps it once more: ((bba{ss})). Accept both. */
        NULL, G_DBUS_CALL_FLAGS_NONE, 300000, NULL,
        authz_cb, r);
}

static void polkit_check(PmAuthReq *r)
{
    const gchar *sender;
    GDBusConnection *conn;

    if (r->app->backend_is_sim || r->inv == NULL) {
        polkit_finish(r, TRUE, NULL);
        return;
    }

    conn = g_dbus_method_invocation_get_connection(r->inv);
    sender = g_dbus_method_invocation_get_sender(r->inv);
    if (!sender) {
        polkit_finish(r, FALSE,
                      "no bus sender credentials (direct connection?)");
        return;
    }
    /* reply_type NULL: dbus-daemon returns a bare a{sv} body while
     * dbus-broker wraps it in a tuple — handle both in creds_cb */
    g_dbus_connection_call(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "GetConnectionCredentials",
                           g_variant_new("(s)", sender), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, creds_cb, r);
}

/* request polkit (if applicable) and then perform strategy/stop */
static void polkit_then(PmObj *o, GDBusMethodInvocation *inv,
                        gchar *strategy /*owned or NULL*/, gchar **params /*owned*/)
{
    PmAuthReq *r = g_new0(PmAuthReq, 1);

    r->app = o->app;
    r->o = o;
    r->inv = inv;
    r->strategy = strategy;
    r->params = params;
    polkit_check(r);
}

/* ---------------- Sched1 methods ---------------- */

static gboolean strategy_in_list(PmApp *app, PmObj *o, const gchar *name)
{
    GPtrArray *l = g_ptr_array_new_with_free_func(pm_strategy_free);
    gboolean found = FALSE;
    guint i;

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

static void apply_requested(PmObj *o, const gchar *strategy, gchar **params,
                            GDBusMethodInvocation *inv)
{
    GError *e = NULL;

    if (!o->available) {
        e = g_error_new(PM_ERROR, PM_ERR_NOT_AVAILABLE, "not available: %s",
                        *o->status_message ? o->status_message : o->name);
        goto out;
    }
    if (o->kind == PM_KIND_DEVICE && params && params[0]) {
        e = g_error_new(PM_ERROR, PM_ERR_NOT_SUPPORTED,
                        "device objects do not accept strategy parameters");
        goto out;
    }
    /* CPU and global IO accept argv passthrough for their loader
     * binaries; Cache accepts params that may override the -w flag */
    if ((o->kind == PM_KIND_CPU || o->kind == PM_KIND_IO) &&
        g_strcmp0(strategy, "none") == 0 && params && params[0]) {
        e = g_error_new(PM_ERROR, PM_ERR_NOT_SUPPORTED,
                        "strategy 'none' takes no parameters");
        goto out;
    }
    if (!strategy_in_list(o->app, o, strategy)) {
        e = g_error_new(PM_ERROR, PM_ERR_NO_SUCH_STRATEGY,
                        "no such strategy: %s", strategy);
        goto out;
    }
    polkit_then(o, inv, g_strdup(strategy), params);
    return;
out:
    g_strfreev(params);
    return_pm_error(inv, e);
}

static void stop_requested(PmObj *o, GDBusMethodInvocation *inv)
{
    GError *e = NULL;

    if (!o->available) {
        e = g_error_new(PM_ERROR, PM_ERR_NOT_AVAILABLE, "not available: %s",
                        *o->status_message ? o->status_message : o->name);
        return_pm_error(inv, e);
        return;
    }
    if (o->kind == PM_KIND_DEVICE) {
        /* devices: stop == restore the device default */
        if (!o->default_strategy || !*o->default_strategy) {
            e = g_error_new(PM_ERROR, PM_ERR_NO_DEFAULT,
                            "no default strategy configured for device %s",
                            o->name);
            return_pm_error(inv, e);
            return;
        }
        polkit_then(o, inv, g_strdup(o->default_strategy), NULL);
        return;
    }
    polkit_then(o, inv, NULL, NULL);
}

static void restart_requested(PmObj *o, GDBusMethodInvocation *inv)
{
    const gchar *s;
    gchar **params;
    GError *e = NULL;

    if (!o->available) {
        e = g_error_new(PM_ERROR, PM_ERR_NOT_AVAILABLE, "not available: %s",
                        *o->status_message ? o->status_message : o->name);
        return_pm_error(inv, e);
        return;
    }
    s = (o->current && *o->current) ? o->current : o->last_ok;
    if (!s || !*s) {
        e = g_error_new(PM_ERROR, PM_ERR_NOT_RUNNING, "no strategy to restart");
        return_pm_error(inv, e);
        return;
    }
    params = (o->current && *o->current) ? g_strdupv(o->current_params)
                                         : g_strdupv(o->last_ok_params);
    polkit_then(o, inv, g_strdup(s), params);
}

static void default_requested(PmObj *o, GDBusMethodInvocation *inv)
{
    GError *e = NULL;

    if (!o->available) {
        e = g_error_new(PM_ERROR, PM_ERR_NOT_AVAILABLE, "not available: %s",
                        *o->status_message ? o->status_message : o->name);
        return_pm_error(inv, e);
        return;
    }
    if (!o->default_strategy || !*o->default_strategy) {
        e = g_error_new(PM_ERROR, PM_ERR_NO_DEFAULT,
                        "no default strategy configured");
        return_pm_error(inv, e);
        return;
    }
    polkit_then(o, inv, g_strdup(o->default_strategy), NULL);
}

static void policy_method_call(PmObj *o, const gchar *method, GVariant *params,
                               GDBusMethodInvocation *inv)
{
    if (g_strcmp0(method, "ListStrategies") == 0) {
        GPtrArray *l = g_ptr_array_new_with_free_func(pm_strategy_free);
        GVariantBuilder b;
        guint i;

        o->app->backend->list_strategies(o->app, o, l);
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ss)"));
        for (i = 0; i < l->len; i++) {
            PmStrategy *s = g_ptr_array_index(l, i);
            g_variant_builder_add(&b, "(ss)", s->name,
                                  s->description ? s->description : "");
        }
        g_ptr_array_free(l, TRUE);
        g_dbus_method_invocation_return_value(inv,
                                              g_variant_new("(a(ss))", &b));
    } else if (g_strcmp0(method, "ApplyStrategy") == 0) {
        const gchar *s;
        g_variant_get(params, "(&s)", &s);
        apply_requested(o, s, NULL, inv);
    } else if (g_strcmp0(method, "ApplyStrategyWithParams") == 0) {
        const gchar *s;
        gchar **ps;
        g_variant_get(params, "(&s^as)", &s, &ps);
        apply_requested(o, s, ps, inv);
    } else if (g_strcmp0(method, "StopStrategy") == 0) {
        stop_requested(o, inv);
    } else if (g_strcmp0(method, "RestartStrategy") == 0) {
        restart_requested(o, inv);
    } else if (g_strcmp0(method, "RestoreDefault") == 0) {
        default_requested(o, inv);
    } else {
        gchar *m = g_strdup_printf("Unknown method %s on " PM_IFACE_POLICY,
                                   method);
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod", m);
        g_free(m);
    }
}

/* ---------------- standard interfaces ---------------- */

static GVariant *obj_prop_value(PmObj *o, const gchar *iface,
                                const gchar *name, GError **error)
{
    if (g_strcmp0(iface, PM_IFACE_POLICY) == 0) {
        if (g_strcmp0(name, "CurrentStrategy") == 0)
            return g_variant_new_string(o->current ? o->current : "");
        if (g_strcmp0(name, "CurrentParams") == 0)
            return g_variant_new_strv((const gchar *const *)
                (o->current_params ? o->current_params : empty_strv), -1);
        if (g_strcmp0(name, "DefaultStrategy") == 0)
            return g_variant_new_string(o->default_strategy ?
                                        o->default_strategy : "");
        if (g_strcmp0(name, "Description") == 0)
            return g_variant_new_string(o->description ? o->description : "");
        if (g_strcmp0(name, "Available") == 0)
            return g_variant_new_boolean(o->available);
        if (g_strcmp0(name, "StatusMessage") == 0)
            return g_variant_new_string(o->status_message ?
                                        o->status_message : "");
    } else if (g_strcmp0(iface, PM_IFACE_IO) == 0 &&
               o->kind == PM_KIND_IO) {
        if (g_strcmp0(name, "Devices") == 0)
            return devices_variant(o->app);
    } else if (g_strcmp0(iface, PM_IFACE_MANAGER) == 0 &&
               o->kind == PM_KIND_ROOT) {
        if (g_strcmp0(name, "Version") == 0)
            return g_variant_new_uint32(1);
        if (g_strcmp0(name, "Backend") == 0)
            return g_variant_new_string(o->app->backend->id);
        if (g_strcmp0(name, "KernelRelease") == 0)
            return g_variant_new_string(o->app->kernel_release ?
                                        o->app->kernel_release : "unknown");
    }
    g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                "no such property %s on %s", name, iface);
    return NULL;
}

static const gchar *const *iface_prop_names(const gchar *iface)
{
    static const gchar *const manager[] = { "Version", "Backend",
                                            "KernelRelease", NULL };
    static const gchar *const policy[] = { "CurrentStrategy", "CurrentParams",
                                           "DefaultStrategy", "Description",
                                           "Available", "StatusMessage", NULL };
    static const gchar *const io[] = { "Devices", NULL };

    if (g_strcmp0(iface, PM_IFACE_MANAGER) == 0)
        return manager;
    if (g_strcmp0(iface, PM_IFACE_POLICY) == 0)
        return policy;
    if (g_strcmp0(iface, PM_IFACE_IO) == 0)
        return io;
    return NULL;
}

static void props_method_call(PmObj *o, const gchar *method, GVariant *params,
                              GDBusMethodInvocation *inv)
{
    if (g_strcmp0(method, "Get") == 0) {
        const gchar *iface, *prop;
        GError *e = NULL;
        GVariant *v;

        g_variant_get(params, "(&s&s)", &iface, &prop);
        v = obj_prop_value(o, iface, prop, &e);
        if (!v) {
            g_dbus_method_invocation_return_dbus_error(
                inv, "org.freedesktop.DBus.Error.InvalidArgs", e->message);
            g_error_free(e);
            return;
        }
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
    } else if (g_strcmp0(method, "GetAll") == 0) {
        const gchar *iface;
        const gchar *const *names;
        GVariantBuilder b;
        gint i;

        g_variant_get(params, "(&s)", &iface);
        names = iface_prop_names(iface);
        if (!names) {
            gchar *m = g_strdup_printf("no such interface %s", iface);
            g_dbus_method_invocation_return_dbus_error(
                inv, "org.freedesktop.DBus.Error.InvalidArgs", m);
            g_free(m);
            return;
        }
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        for (i = 0; names[i]; i++) {
            GError *e = NULL;
            GVariant *v = obj_prop_value(o, iface, names[i], &e);
            if (v) {
                g_variant_builder_add(&b, "{sv}", names[i], v);
            } else {
                g_error_free(e);
            }
        }
        g_dbus_method_invocation_return_value(inv,
                                              g_variant_new("(a{sv})", &b));
    } else if (g_strcmp0(method, "Set") == 0) {
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.PropertyReadOnly",
            "all schedmgr properties are read-only");
    } else {
        gchar *m = g_strdup_printf("Unknown method %s on " PM_IFACE_PROPS,
                                   method);
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod", m);
        g_free(m);
    }
}

static gchar *read_machine_id(void)
{
    static const gchar *const paths[] = { "/etc/machine-id",
                                          "/var/lib/dbus/machine-id", NULL };
    gint i;

    for (i = 0; paths[i]; i++) {
        gchar *buf = NULL;
        if (g_file_get_contents(paths[i], &buf, NULL, NULL)) {
            gchar *t = g_strstrip(buf);
            gchar *r = *t ? g_strdup(t) : g_strdup("");
            g_free(buf);
            if (*r)
                return r;
            g_free(r);
        }
    }
    return g_strdup("");
}

static void peer_method_call(const gchar *method, GDBusMethodInvocation *inv)
{
    if (g_strcmp0(method, "Ping") == 0) {
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "GetMachineId") == 0) {
        gchar *id = read_machine_id();
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", id));
        g_free(id);
    } else {
        gchar *m = g_strdup_printf("Unknown method %s on " PM_IFACE_PEER,
                                   method);
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod", m);
        g_free(m);
    }
}

/* ---------------- introspection (dynamic children) ---------------- */

static gchar *build_introspection(PmObj *o)
{
    GString *s = g_string_new("<node>\n");

    switch (o->kind) {
    case PM_KIND_ROOT:
        g_string_append_printf(s, "  %s\n", XML_MANAGER);
        if (o->app->backend_is_sim)
            g_string_append_printf(s, "  %s\n", XML_SIM);
        g_string_append_printf(s, "  %s\n  %s\n  %s\n",
                               XML_PROPS, XML_INTROSPECT, XML_PEER);
        g_string_append_printf(s, "  <node name='Cpu'/>\n"
                                  "  <node name='Cache'/>\n"
                                  "  <node name='Io'/>\n");
        break;
    case PM_KIND_IO:
        g_string_append_printf(s, "  %s\n  %s\n  %s\n  %s\n  %s\n",
                               XML_POLICY, XML_IO, XML_PROPS,
                               XML_INTROSPECT, XML_PEER);
        {
            guint i;
            for (i = 0; i < o->app->devices->len; i++) {
                PmObj *d = g_ptr_array_index(o->app->devices, i);
                g_string_append_printf(s, "  <node name='%s'/>\n", d->name);
            }
        }
        break;
    default:
        g_string_append_printf(s, "  %s\n  %s\n  %s\n  %s\n",
                               XML_POLICY, XML_PROPS, XML_INTROSPECT,
                               XML_PEER);
        break;
    }
    g_string_append(s, "</node>\n");
    return g_string_free(s, FALSE);
}

/* ---------------- vtables ---------------- */

static void obj_method_call(GDBusConnection *conn, const gchar *sender,
                            const gchar *path, const gchar *iface,
                            const gchar *method, GVariant *params,
                            GDBusMethodInvocation *inv, gpointer ud)
{
    PmObj *o = ud;

    (void)conn;
    (void)sender;
    (void)path;

    if (g_strcmp0(iface, PM_IFACE_POLICY) == 0)
        policy_method_call(o, method, params, inv);
    else if (g_strcmp0(iface, PM_IFACE_PROPS) == 0)
        props_method_call(o, method, params, inv);
    else if (g_strcmp0(iface, PM_IFACE_INTROSPECT) == 0) {
        gchar *xml = build_introspection(o);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", xml));
        g_free(xml);
    } else if (g_strcmp0(iface, PM_IFACE_PEER) == 0)
        peer_method_call(method, inv);
    else if (g_strcmp0(iface, PM_IFACE_IO) == 0) {
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod",
            PM_IFACE_IO " has no methods");
    } else {
        gchar *m = g_strdup_printf("Unknown interface %s", iface);
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod", m);
        g_free(m);
    }
}

static GVariant *obj_get_property(GDBusConnection *conn, const gchar *sender,
                                  const gchar *path, const gchar *iface,
                                  const gchar *name, GError **error,
                                  gpointer ud)
{
    (void)conn;
    (void)sender;
    (void)path;
    return obj_prop_value(ud, iface, name, error);
}

static gboolean obj_set_property(GDBusConnection *conn, const gchar *sender,
                                 const gchar *path, const gchar *iface,
                                 const gchar *name, GVariant *value,
                                 GError **error, gpointer ud)
{
    (void)conn;
    (void)sender;
    (void)path;
    (void)iface;
    (void)name;
    (void)value;
    (void)ud;
    g_set_error(error, PM_ERROR, PM_ERR_FAILED,
                "all schedmgr properties are read-only");
    return FALSE;
}

static void root_method_call(GDBusConnection *conn, const gchar *sender,
                             const gchar *path, const gchar *iface,
                             const gchar *method, GVariant *params,
                             GDBusMethodInvocation *inv, gpointer ud)
{
    PmObj *o = ud;

    (void)conn;
    (void)sender;
    (void)path;

    if (g_strcmp0(iface, PM_IFACE_MANAGER) == 0) {
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod",
            PM_IFACE_MANAGER " has no methods");
    } else if (g_strcmp0(iface, PM_IFACE_SIM) == 0 &&
               g_strcmp0(method, "Hotplug") == 0) {
        const gchar *action, *dev;
        GError *e = NULL;

        g_variant_get(params, "(&s&s)", &action, &dev);
        if (!o->app->backend->hotplug) {
            e = g_error_new(PM_ERROR, PM_ERR_NOT_SUPPORTED,
                            "hotplug simulation is only supported by the sim backend");
            return_pm_error(inv, e);
            return;
        }
        if (!o->app->backend->hotplug(o->app, action, dev, &e))
            return_pm_error(inv, e);
        else
            g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(iface, PM_IFACE_SIM) == 0 &&
               g_strcmp0(method, "SetAvailable") == 0) {
        /* sim-only knob: flip Available/StatusMessage of any domain or
         * device so front ends and e2e can rehearse the unavailable path */
        const gchar *name;
        gboolean avail;
        PmApp *app = o->app;
        PmObj *t = NULL;

        g_variant_get(params, "(&sb)", &name, &avail);
        if (g_strcmp0(name, "Cpu") == 0)
            t = app->cpu;
        else if (g_strcmp0(name, "Cache") == 0)
            t = app->cache;
        else if (g_strcmp0(name, "Io") == 0)
            t = app->io;
        else if (pm_dev_name_valid(name))
            t = pm_app_find_device(app, name);
        if (!t) {
            return_pm_error(inv, g_error_new(PM_ERROR, PM_ERR_NO_SUCH_STRATEGY,
                                             "no such object: %s", name));
            return;
        }
        pm_obj_set_available(t, avail,
                             avail ? "" : "simulated unavailability");
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(iface, PM_IFACE_PROPS) == 0) {
        props_method_call(o, method, params, inv);
    } else if (g_strcmp0(iface, PM_IFACE_INTROSPECT) == 0) {
        gchar *xml = build_introspection(o);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", xml));
        g_free(xml);
    } else if (g_strcmp0(iface, PM_IFACE_PEER) == 0) {
        peer_method_call(method, inv);
    } else {
        gchar *m = g_strdup_printf("Unknown method %s on %s", method, iface);
        g_dbus_method_invocation_return_dbus_error(
            inv, "org.freedesktop.DBus.Error.UnknownMethod", m);
        g_free(m);
    }
}

static const GDBusInterfaceVTable obj_vtable = {
    obj_method_call, obj_get_property, obj_set_property, { 0 }
};
static const GDBusInterfaceVTable root_vtable = {
    root_method_call, obj_get_property, obj_set_property, { 0 }
};

/* ---------------- registration ---------------- */

static void register_iface(PmObj *o, GDBusConnection *conn,
                           GDBusInterfaceInfo *info,
                           const GDBusInterfaceVTable *vt)
{
    GError *e = NULL;
    guint id;
    PmReg *r;

    id = g_dbus_connection_register_object(conn, o->path, info, vt, o, NULL,
                                           &e);
    if (!id) {
        g_warning("schedmgrd: cannot register %s on %s: %s", info->name,
                  o->path, e->message);
        g_error_free(e);
        return;
    }
    r = g_new(PmReg, 1);
    r->conn = conn;
    r->id = id;
    g_ptr_array_add(o->regs, r);
}

static void register_obj_ifaces(PmApp *app, GDBusConnection *conn, PmObj *o)
{
    ensure_infos();
    switch (o->kind) {
    case PM_KIND_ROOT:
        register_iface(o, conn, info_manager, &root_vtable);
        if (app->backend_is_sim)
            register_iface(o, conn, info_sim, &root_vtable);
        register_iface(o, conn, info_props, &root_vtable);
        register_iface(o, conn, info_introspect, &root_vtable);
        register_iface(o, conn, info_peer, &root_vtable);
        break;
    case PM_KIND_IO:
        register_iface(o, conn, info_policy, &obj_vtable);
        register_iface(o, conn, info_io, &obj_vtable);
        register_iface(o, conn, info_props, &obj_vtable);
        register_iface(o, conn, info_introspect, &obj_vtable);
        register_iface(o, conn, info_peer, &obj_vtable);
        break;
    default:
        register_iface(o, conn, info_policy, &obj_vtable);
        register_iface(o, conn, info_props, &obj_vtable);
        register_iface(o, conn, info_introspect, &obj_vtable);
        register_iface(o, conn, info_peer, &obj_vtable);
        break;
    }
}

void pm_obj_register(PmObj *o)
{
    guint i;

    for (i = 0; i < o->app->conns->len; i++)
        register_obj_ifaces(o->app, g_ptr_array_index(o->app->conns, i), o);
}

static void register_all(PmApp *app, GDBusConnection *conn)
{
    guint i;

    register_obj_ifaces(app, conn, app->root);
    register_obj_ifaces(app, conn, app->cpu);
    register_obj_ifaces(app, conn, app->cache);
    register_obj_ifaces(app, conn, app->io);
    for (i = 0; i < app->devices->len; i++)
        register_obj_ifaces(app, conn, g_ptr_array_index(app->devices, i));
}

/* ---------------- device tree ---------------- */

PmObj *pm_app_find_device(PmApp *app, const gchar *dev)
{
    guint i;

    for (i = 0; i < app->devices->len; i++) {
        PmObj *d = g_ptr_array_index(app->devices, i);
        if (g_strcmp0(d->name, dev) == 0)
            return d;
    }
    return NULL;
}

PmObj *pm_app_add_device(PmApp *app, const gchar *dev, const gchar *description,
                         gboolean available, const gchar *status,
                         const gchar *current, const gchar *raw_elevator)
{
    PmObj *o = pm_obj_new(app, PM_KIND_DEVICE, dev);

    g_free(o->description);
    o->description = g_strdup(description ? description : dev);
    o->available = available;
    g_free(o->status_message);
    o->status_message = g_strdup(status ? status : "");
    g_free(o->current);
    o->current = g_strdup(current ? current : "");
    g_free(o->raw);
    o->raw = g_strdup(raw_elevator ? raw_elevator : "");
    g_ptr_array_add(app->devices, o);
    pm_obj_register(o);
    g_debug("schedmgrd: device added: %s", dev);
    return o;
}

void pm_app_remove_device(PmApp *app, const gchar *dev)
{
    PmObj *d = pm_app_find_device(app, dev);
    guint i;

    if (!d)
        return;
    for (i = 0; i < d->regs->len; i++) {
        PmReg *r = g_ptr_array_index(d->regs, i);
        g_dbus_connection_unregister_object(r->conn, r->id);
    }
    g_ptr_array_set_size(d->regs, 0);
    d->dead = TRUE;
    g_ptr_array_remove(app->devices, d);
    g_ptr_array_add(app->graveyard, d);   /* freed at shutdown */
    g_debug("schedmgrd: device removed: %s", dev);
}

void pm_app_devices_changed(PmApp *app)
{
    if (!app->io)
        return;
    emit_property_changed(app, app->io->path, PM_IFACE_IO, "Devices",
                          devices_variant(app));
    emit_signal_all(app, app->io->path, PM_IFACE_IO, "DevicesChanged", NULL);
}

/* ---------------- connection setup ---------------- */

static void on_bus_acquired(GDBusConnection *conn, const gchar *name,
                            gpointer ud)
{
    PmApp *app = ud;

    (void)name;
    g_ptr_array_add(app->conns, conn);
    register_all(app, conn);
    g_print("schedmgrd: connected to bus\n");
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name,
                             gpointer ud)
{
    (void)conn;
    (void)ud;
    g_print("schedmgrd: acquired %s\n", name);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name, gpointer ud)
{
    PmApp *app = ud;

    (void)conn;
    if (app->running) {
        g_printerr("schedmgrd: lost or failed to acquire %s "
                   "(another instance running?)\n", name);
        app->running = FALSE;
        g_main_loop_quit(app->loop);
    }
}

static gboolean on_new_connection(GDBusServer *server, GDBusConnection *conn,
                                  gpointer ud)
{
    PmApp *app = ud;

    (void)server;
    register_all(app, conn);
    g_ptr_array_add(app->conns, g_object_ref(conn));
    return TRUE;
}

void pm_dbus_setup(PmApp *app)
{
    if (app->direct_mode) {
        gchar *guid = g_dbus_generate_guid();
        GError *e = NULL;

        app->server = g_dbus_server_new_sync(app->bus_address,
                                             G_DBUS_SERVER_FLAGS_NONE, guid,
                                             NULL, NULL, &e);
        g_free(guid);
        if (!app->server) {
            g_printerr("schedmgrd: cannot listen on %s: %s\n",
                       app->bus_address, e->message);
            g_error_free(e);
            exit(1);
        }
        g_signal_connect(app->server, "new-connection",
                         G_CALLBACK(on_new_connection), app);
        g_dbus_server_start(app->server);
        g_print("schedmgrd: listening on %s (direct connection mode)\n",
                g_dbus_server_get_client_address(app->server));
    } else {
        app->owner_id = g_bus_own_name(
            app->session_bus ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
            PM_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
            on_bus_acquired, on_name_acquired, on_name_lost, app, NULL);
    }
}

void pm_dbus_teardown(PmApp *app)
{
    guint i;

    if (app->owner_id) {
        g_bus_unown_name(app->owner_id);
        app->owner_id = 0;
    }
    if (app->server) {
        g_dbus_server_stop(app->server);
        g_object_unref(app->server);
        app->server = NULL;
    }
    if (app->direct_mode) {
        for (i = 0; i < app->conns->len; i++)
            g_object_unref(g_ptr_array_index(app->conns, i));
    }
    g_ptr_array_set_size(app->conns, 0);
}
