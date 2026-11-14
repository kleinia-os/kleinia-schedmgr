// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
/*
 * schedmgr-core.h — shared types and pure helpers for schedmgrd.
 *
 * Everything in schedmgr-core.c is side-effect-free with respect to the
 * real system: sysfs accessors take an explicit root path so the unit
 * tests can run them against throw-away directories.
 */
#ifndef SCHEDMGR_CORE_H
#define SCHEDMGR_CORE_H

#include <glib.h>
#include <gio/gio.h>
#include <sys/types.h>

/* ---- names ---- */
#define PM_BUS_NAME          "io.kleinia.SchedMgr"
#define PM_ROOT_PATH         "/io/kleinia/SchedMgr"
#define PM_IFACE_POLICY      "io.kleinia.Sched1"
#define PM_IFACE_MANAGER     "io.kleinia.Manager1"
#define PM_IFACE_IO          "io.kleinia.Io1"
#define PM_IFACE_SIM         "io.kleinia.Sim1"
#define PM_IFACE_PROPS       "org.freedesktop.DBus.Properties"
#define PM_IFACE_INTROSPECT  "org.freedesktop.DBus.Introspectable"
#define PM_IFACE_PEER        "org.freedesktop.DBus.Peer"
#define PM_ERR_PREFIX        "io.kleinia.Sched1."
#define PM_BUS_NAME_SIM      "schedmgrd"

#define PM_POLKIT_ACTION     "io.kleinia.schedmgr.apply"
#define PM_POLKIT_BUS        "org.freedesktop.PolicyKit1"
#define PM_POLKIT_PATH       "/org/freedesktop/PolicyKit1/Authority"
#define PM_POLKIT_IFACE      "org.freedesktop.PolicyKit1.Authority"

/* PM_SYSFS_ROOT can be overridden at compile time (-DPM_SYSFS_ROOT=...)
 * to point the system backend at a synthetic sysfs tree for testing;
 * production builds always use /sys. */
#ifndef PM_SYSFS_ROOT
#define PM_SYSFS_ROOT        "/sys"
#endif
#define PM_SCHED_EXT_STATE   "/sys/kernel/sched_ext/state"

/* ---- error domain ---- */
typedef enum {
    PM_ERR_NO_SUCH_STRATEGY,
    PM_ERR_NOT_SUPPORTED,
    PM_ERR_NOT_RUNNING,
    PM_ERR_NO_DEFAULT,
    PM_ERR_NOT_AVAILABLE,
    PM_ERR_FAILED,
} PmErrCode;

GQuark pm_error_quark(void);
#define PM_ERROR pm_error_quark()
const gchar *pm_err_name(PmErrCode code);   /* full DBus error name */

/* ---- configuration (GKeyFile) ---- */
typedef struct {
    gchar *sched_dirs;        /* colon-separated, default /usr/bin */
    gchar *cache_policy_dir;  /* legacy v1 key, still parsed; default
                               * /usr/lib/kleinia/schedmgr/cache-policies */
    gchar *state_dir;         /* default /var/lib/kleinia/schedmgr */
    gint   io_poll_sec;       /* default 5 */
    gchar *io_skip;           /* comma-separated prefixes, default "zram,loop" */
    gchar *cpu_default;       /* default "none"; empty = no default */
    gchar *cache_default;     /* default "" */
    gchar *io_default;        /* per-device elevator default; default "" */
    gchar *iox_dirs;          /* [Io] IoxDirs, default
                               * /usr/bin:/usr/local/bin:/var/usrlocal/bin */
    gchar *cax_dirs;          /* [Cache] CaxDirs, default same as IoxDirs */
    gchar *cache_watch_dir;   /* [Cache] WatchDir, default /var/tmp */
    gchar *cache_cgroup;      /* [Cache] Cgroup, default
                               * /sys/fs/cgroup/cache_ext_schedmgr */
    GHashTable *io_dev_defaults; /* dev -> strategy, may be empty */
} PmConfig;

void pm_config_defaults(PmConfig *cfg);
void pm_config_load(PmConfig *cfg, const gchar *path); /* path may be NULL */
void pm_config_free(PmConfig *cfg);

/* ---- persisted state (state.ini) ---- */
typedef struct {
    gboolean   existed;       /* state file was found */
    gchar     *cpu_strategy;  /* NULL when absent */
    gchar    **cpu_params;    /* NULL-terminated or NULL */
    gchar     *cache_strategy; /* [cache] Strategy, NULL when absent */
    gchar    **cache_params;
    gchar     *io_global;     /* [io] Global (running iox loader), NULL/""=none */
    GHashTable *io;           /* dev -> strategy, may be NULL; never
                               * contains the "Global" key */
} PmState;

void pm_state_load(PmState *st, const gchar *state_dir);
gboolean pm_state_save(const gchar *state_dir,
                       const gchar *cpu_strategy, gchar **cpu_params,
                       const gchar *cache_strategy, gchar **cache_params,
                       const gchar *io_global,
                       GHashTable *io, GError **error);
void pm_state_clear(PmState *st);

/* ---- scheduler list parsing ("none mq-deadline [kyber] bfq") ---- */
gchar **pm_parse_scheduler_list(const gchar *text, gchar **current_out);
gboolean pm_read_scheduler_file(const gchar *path, gchar ***names_out,
                                gchar **current_out, GError **error);
gboolean pm_write_scheduler_file(const gchar *path, const gchar *sched,
                                 gchar **confirmed_out, GError **error);

/* ---- device filtering / enumeration ---- */
gboolean pm_device_should_skip(const gchar *dev, const gchar *skip_csv);
GPtrArray *pm_enumerate_block_devices(const gchar *sys_root, const gchar *skip_csv);

/* ---- sysfs path helpers ---- */
gchar *pm_path_block(const gchar *sys_root, const gchar *dev);
gchar *pm_path_sched(const gchar *sys_root, const gchar *dev);
gchar *pm_path_model(const gchar *sys_root, const gchar *dev);
gchar *pm_path_size(const gchar *sys_root, const gchar *dev);
gchar *pm_device_description(const gchar *sys_root, const gchar *dev);
gchar *pm_read_sched_ext_state(const gchar *sys_root);  /* NULL if absent */

/* ---- v2 domain helpers ---- */
/* discover <prefix>* executables under a colon-separated dir list;
 * returns a sorted, de-duplicated GPtrArray of file names (g_free) */
GPtrArray *pm_scan_prefix_exes(const gchar *dirs_csv, const gchar *prefix);
/* effective per-device strategy: elevator != scx_io -> elevator;
 * scx_io -> running global iox name, or "scx_io" when no loader runs
 * (*no_loader_out set) */
gchar *pm_effective_io_strategy(const gchar *elevator, const gchar *global_iox,
                                gboolean *no_loader_out);
/* the kernel requires the cache cgroup name to start with "cache_ext" */
gboolean pm_cache_cgroup_valid(const gchar *cgroup);
/* TRUE when any block device's queue/scheduler file (under sys_root)
 * lists the scx_io elevator */
gboolean pm_kernel_has_scx_io(const gchar *sys_root);

/* ---- built-in strategy descriptions ---- */
const gchar *pm_cpu_strategy_description(const gchar *name); /* NULL if unknown */
const gchar *pm_io_strategy_description(const gchar *name);  /* NULL if unknown */
const gchar *pm_iox_strategy_description(const gchar *name); /* iox_* or NULL */
const gchar *pm_cax_strategy_description(const gchar *name); /* cax_* or NULL */
guint64 pm_proc_starttime(pid_t pid);   /* 0 on failure (for polkit) */

/* ---- misc ---- */
gchar *pm_kernel_release(void);
gboolean pm_dev_name_valid(const gchar *dev);

/* ---- objects and app (filled in by dbus-server.c / backends) ---- */
typedef struct _PmApp PmApp;
typedef struct _PmObj PmObj;

/* one entry of ListStrategies() output */
typedef struct {
    gchar *name;
    gchar *description;
} PmStrategy;
void pm_strategy_free(gpointer s);  /* GDestroyNotify compatible */

typedef enum {
    PM_KIND_ROOT,
    PM_KIND_CPU,
    PM_KIND_CACHE,
    PM_KIND_IO,
    PM_KIND_DEVICE,
} PmObjKind;

/* completion of an async backend operation.
 * confirmed == "" means "no strategy active" (stop succeeded);
 * confirmed is BORROWED (callee must not free it), params and error
 * are owned by the callee (g_strfreev / g_error_free). */
typedef void (*PmDoneFn)(PmApp *app, PmObj *o, const gchar *confirmed,
                         gchar **params, GError *error, gpointer user_data);

/* notification that an internally-triggered apply finished (ok = no
 * error); used e.g. by the device->global iox chaining in the system
 * backend */
typedef void (*PmNotifyFn)(PmApp *app, PmObj *o, gboolean ok, gpointer user_data);

typedef struct {
    const gchar *id;          /* "system" / "sim" */
    void (*list_strategies)(PmApp *app, PmObj *o, GPtrArray *out);
    void (*apply)(PmApp *app, PmObj *o, gchar *strategy /*owned*/,
                  gchar **params /*owned*/, PmDoneFn done, gpointer ud);
    void (*stop)(PmApp *app, PmObj *o, PmDoneFn done, gpointer ud);
    void (*startup)(PmApp *app);
    void (*shutdown)(PmApp *app);
    gboolean (*hotplug)(PmApp *app, const gchar *action, const gchar *dev,
                        GError **error);
} PmBackend;

typedef struct {
    GDBusConnection *conn;
    guint id;
} PmReg;

struct _PmObj {
    PmApp      *app;
    PmObjKind   kind;
    gchar      *path;
    gchar      *name;            /* "Cpu", "Cache", "Io" or device name */
    gchar      *description;
    gboolean    available;
    gchar      *status_message;
    gchar      *current;         /* "" = none running */
    gchar     **current_params;  /* NULL allowed */
    gchar      *last_ok;         /* last confirmed non-empty strategy */
    gchar     **last_ok_params;
    gchar      *default_strategy;
    gchar      *raw;             /* devices: raw sysfs elevator (backend use) */
    GPtrArray  *regs;            /* PmReg */
    gboolean    dead;            /* removed from the tree */
    gpointer    priv;            /* backend private data */
};

struct _PmApp {
    GMainLoop   *loop;
    PmConfig     cfg;
    const PmBackend *backend;
    gboolean     backend_is_sim;

    /* connection */
    gboolean     direct_mode;    /* --bus-address: no bus name, GDBusServer */
    gchar       *bus_address;
    GPtrArray   *conns;          /* GDBusConnection* */
    guint        owner_id;
    GDBusServer *server;

    gboolean     session_bus;     /* --session */
    gchar       *kernel_release;

    PmObj       *root;
    PmObj       *cpu;
    PmObj       *cache;
    PmObj       *io;
    GPtrArray   *devices;        /* PmObj* */
    GPtrArray   *graveyard;      /* removed objects, freed at shutdown */
    guint        io_poll_id;
    gboolean     running;
};

/* provided by dbus-server.c, used by backends and main */
void pm_dbus_setup(PmApp *app);
void pm_dbus_teardown(PmApp *app);
PmObj *pm_obj_new(PmApp *app, PmObjKind kind, const gchar *name);
void pm_obj_register(PmObj *o);
void pm_obj_set_current(PmObj *o, const gchar *strategy, gchar **params /*owned*/);
void pm_obj_report_failure(PmObj *o, const gchar *strategy, const gchar *reason);
void pm_obj_set_available(PmObj *o, gboolean available, const gchar *status);
void pm_backend_apply_async(PmObj *o, const gchar *strategy,
                            gchar **params /*owned, NULL ok*/,
                            gpointer inv /* GDBusMethodInvocation*, NULL for internal */);
void pm_backend_apply_async_notify(PmObj *o, const gchar *strategy,
                                   gchar **params /*owned, NULL ok*/,
                                   PmNotifyFn notify, gpointer notify_ud);
void pm_backend_stop_async(PmObj *o, gpointer inv /* NULL for internal */);
PmObj *pm_app_add_device(PmApp *app, const gchar *dev, const gchar *description,
                         gboolean available, const gchar *status,
                         const gchar *current, const gchar *raw_elevator);
void pm_app_remove_device(PmApp *app, const gchar *dev);
void pm_app_devices_changed(PmApp *app);
PmObj *pm_app_find_device(PmApp *app, const gchar *dev);
void pm_app_persist_state(PmApp *app);
void pm_app_refresh_io_devices(PmApp *app);
void pm_obj_free(PmObj *o);

/* backends */
extern const PmBackend pm_sim_backend;
extern const PmBackend pm_system_backend;

#endif /* SCHEDMGR_CORE_H */
