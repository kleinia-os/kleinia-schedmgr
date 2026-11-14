#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""schedmgr-client.py — io.kleinia.SchedMgr 的 DBus 客户端封装。

实现前端契约：
  * 用 Gio.DBusProxy 连接各对象——代理构造时 GDBus 自动执行 GetAll
    加载缓存属性，并自动订阅 org.freedesktop.DBus.Properties.PropertiesChanged；
  * 通过 g-signal 订阅 StrategyChanged / StrategyFailed / DevicesChanged；
  * **禁止轮询**：所有 UI 刷新均由 GObject 信号驱动（本文件对外发信号，
    窗口层订阅后按需读取缓存属性）；
  * 方法调用全部异步（proxy.call），错误经 friendly_dbus_error 转成
    人读文案后以 "action-error" 信号抛给 UI（polkit 拒绝有专门文案）。

总线选择：默认 system；`--session` / KLEINIA_SCHEDMGR_BUS=session 时用会话总线。
"""

import os
import sys

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
gi.require_version("GObject", "2.0")
from gi.repository import Gio, GLib, GObject  # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from i18n import _  # noqa: E402

# ---------------------------------------------------------------------------
# 常量（DBus 命名与接口）
# ---------------------------------------------------------------------------
BUS_NAME = "io.kleinia.SchedMgr"
ROOT_PATH = "/io/kleinia/SchedMgr"
IFACE_POLICY = "io.kleinia.Sched1"
IFACE_MANAGER = "io.kleinia.Manager1"
IFACE_IO = "io.kleinia.Io1"

CPU_PATH = ROOT_PATH + "/Cpu"
CACHE_PATH = ROOT_PATH + "/Cache"
IO_PATH = ROOT_PATH + "/Io"

# 域键 -> 对象路径（Io 为全局对象；设备对象 /Io/<dev> 动态发现）
DOMAIN_PATHS = {
    "cpu": CPU_PATH,
    "cache": CACHE_PATH,
    "io": IO_PATH,
}

# 连接成功后必须就位的代理（根 + 三域 + Io 设备列表）
_EXPECTED_PROXIES = [
    (ROOT_PATH, IFACE_MANAGER),
    (CPU_PATH, IFACE_POLICY),
    (CACHE_PATH, IFACE_POLICY),
    (IO_PATH, IFACE_POLICY),
    (IO_PATH, IFACE_IO),
]

# io.kleinia.Sched1.* 与 polkit / DBus 错误的友好文案（英文源串 + gettext；
# zh_CN 译本在 locale/ 下——这些是 UI 横幅/Toast 文案，属 i18n 范围）
FRIENDLY_ERRORS = {
    "io.kleinia.Sched1.NoSuchStrategy":
        _("No such strategy on this domain."),
    "io.kleinia.Sched1.NotSupported":
        _("This operation is not supported on this domain "
          "(parameters only apply to the CPU scheduler)."),
    "io.kleinia.Sched1.NotRunning":
        _("No strategy is currently running."),
    "io.kleinia.Sched1.NoDefault":
        _("No default strategy is configured."),
    "io.kleinia.Sched1.NotAvailable":
        _("This feature is not available on this system."),
    "io.kleinia.Sched1.Failed":
        _("The policy manager daemon reported a failure."),
    "org.freedesktop.DBus.Error.AccessDenied":
        _("Access denied — polkit authorization failed "
          "(io.kleinia.schedmgr.apply)."),
    "org.freedesktop.PolicyKit1.Error.Cancelled":
        _("Authentication was cancelled."),
    "org.freedesktop.PolicyKit1.Error.NotAuthorized":
        _("Not authorized to modify system policies "
          "(io.kleinia.schedmgr.apply)."),
    "org.freedesktop.DBus.Error.UnknownMethod":
        _("No such method on the daemon — client/server version mismatch?"),
}


def friendly_dbus_error(err):
    """把 GLib.Error(GDBus) 转成人读文案（polkit 拒绝等给出友好提示）。"""
    msg = getattr(err, "message", None) or str(err)
    name = None
    detail = msg
    if msg.startswith("GDBus.Error:"):
        rest = msg[len("GDBus.Error:"):].strip()
        if ":" in rest:
            name, detail = rest.split(":", 1)
            name, detail = name.strip(), detail.strip()
        else:
            name = rest
    if name in FRIENDLY_ERRORS:
        base = FRIENDLY_ERRORS[name]
        # Failed / AccessDenied 之类保留 daemon 给出的细节
        if name.endswith((".Failed", "AccessDenied")) and detail:
            return "%s (%s)" % (base, detail)
        return base
    return detail or msg


class SchedMgrClient(GObject.Object):
    """策略管理器 DBus 客户端（缓存属性 + 信号驱动，无轮询）。

    对外信号：
      ready()                       总线名出现，全部代理与策略表就绪
      vanished()                    总线名消失（daemon 掉线）
      manager-changed()             Manager1 属性变化（Backend/Version/...）
      domain-changed(str domain)    cpu|cache|io 域状态变化（含 StrategyChanged）
      device-changed(str path)      某设备对象状态/策略表就绪或变化
      devices-changed()             设备列表增删（UI 应重建设备组）
      strategy-failed(str domain_or_path, str strategy, str reason)
      action-error(str context, str message)   方法调用失败（含 polkit 拒绝）
      action-done(str context)                 方法调用成功（v2 新增，向后兼容；
                                                UI 据此弹成功 Toast 并解除 busy）
    """

    __gsignals__ = {
        "ready": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "vanished": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "manager-changed": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "domain-changed": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
        "device-changed": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
        "devices-changed": (GObject.SignalFlags.RUN_FIRST, None, ()),
        "strategy-failed": (GObject.SignalFlags.RUN_FIRST, None, (str, str, str)),
        "action-error": (GObject.SignalFlags.RUN_FIRST, None, (str, str)),
        "action-done": (GObject.SignalFlags.RUN_FIRST, None, (str,)),
    }

    def __init__(self, bus_type=Gio.BusType.SYSTEM):
        GObject.Object.__init__(self)
        self._bus_type = bus_type
        self._conn = None
        self._proxies = {}        # (path, iface) -> Gio.DBusProxy
        self._strategies = {}     # path -> [(name, description), ...]
        self._device_paths = []   # [object_path, ...]
        self._ready = False
        # 测试钩子：KLEINIA_SCHEDMGR_ADDRESS=<dbus address> 直连指定地址的总线
        # （用于在私有测试总线上联调而不干扰 GTK 的 portal/会话总线）
        address = os.environ.get("KLEINIA_SCHEDMGR_ADDRESS", "").strip()
        if address:
            self._conn = Gio.DBusConnection.new_for_address_sync(
                address,
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
                | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
                None, None)
            self._watch_id = Gio.bus_watch_name_on_connection(
                self._conn, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
                self._on_name_appeared, self._on_name_vanished)
        else:
            self._watch_id = Gio.bus_watch_name(
                bus_type, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
                self._on_name_appeared, self._on_name_vanished)

    # ------------------------------------------------------------------
    # 名称监听与代理生命周期
    # ------------------------------------------------------------------
    def _on_name_appeared(self, conn, name, owner):
        self._conn = conn
        self._ready = False
        for path, iface in _EXPECTED_PROXIES:
            self._new_proxy(path, iface)

    def _on_name_vanished(self, conn, name):
        was_ready = self._ready
        self._ready = False
        self._proxies.clear()
        self._strategies.clear()
        self._device_paths = []
        # 无论之前是否就绪都通知 UI 进入离线态（连接对象保留备用）
        self.emit("vanished")

    def _new_proxy(self, path, iface):
        Gio.DBusProxy.new(
            self._conn, Gio.DBusProxyFlags.NONE, None,
            BUS_NAME, path, iface, None,
            self._proxy_created, (path, iface))

    def _proxy_created(self, source, result, user_data):
        path, iface = user_data
        try:
            proxy = Gio.DBusProxy.new_finish(result)
        except GLib.Error as err:
            self.emit("action-error", "connect",
                      _("Failed to create proxy for %s (%s): %s")
                      % (path, iface, err.message))
            return
        proxy.connect("g-properties-changed", self._on_props_changed, path, iface)
        proxy.connect("g-signal", self._on_g_signal, path, iface)
        self._proxies[(path, iface)] = proxy
        if iface == IFACE_POLICY:
            # 策略表不是属性，只能在对象就绪时一次性调用 ListStrategies（非轮询）
            self._fetch_strategies(path, proxy)
        if (path, iface) == (IO_PATH, IFACE_IO):
            self._sync_devices_from_proxy(proxy)
        self._check_ready()

    def _check_ready(self):
        if self._ready:
            return
        if all(key in self._proxies for key in _EXPECTED_PROXIES):
            self._ready = True
            self.emit("ready")

    # ------------------------------------------------------------------
    # 信号 / 属性回调
    # ------------------------------------------------------------------
    def _on_props_changed(self, proxy, changed, invalidated, path, iface):
        keys = list(changed.unpack().keys()) if changed is not None else []
        if iface == IFACE_MANAGER:
            self.emit("manager-changed")
        elif iface == IFACE_IO:
            if "Devices" in keys:
                self._sync_devices_from_proxy(proxy)
        elif iface == IFACE_POLICY:
            domain = self._domain_of(path)
            if domain is not None:
                self.emit("domain-changed", domain)
            else:
                self.emit("device-changed", path)

    def _on_g_signal(self, proxy, sender, signal, params, path, iface):
        if iface == IFACE_POLICY:
            if signal == "StrategyChanged":
                # 属性缓存已由随后的 PropertiesChanged/前序更新同步；这里只通知 UI
                domain = self._domain_of(path)
                if domain is not None:
                    self.emit("domain-changed", domain)
                else:
                    self.emit("device-changed", path)
            elif signal == "StrategyFailed":
                strategy, reason = params.unpack()
                target = self._domain_of(path) or path
                self.emit("strategy-failed", target, strategy, reason)
        elif iface == IFACE_IO and signal == "DevicesChanged":
            # 保险起见重读一次设备列表（信号驱动的一次性查询，不是轮询）
            self._sync_devices_from_proxy(proxy)

    @staticmethod
    def _domain_of(path):
        for domain, p in DOMAIN_PATHS.items():
            if p == path:
                return domain
        return None

    # ------------------------------------------------------------------
    # 设备发现
    # ------------------------------------------------------------------
    def _sync_devices_from_proxy(self, proxy):
        cached = proxy.get_cached_property("Devices")
        if cached is not None:
            self._sync_devices(cached.unpack())
        else:
            proxy.call(
                "org.freedesktop.DBus.Properties.Get",
                GLib.Variant("(ss)", (IFACE_IO, "Devices")),
                Gio.DBusCallFlags.NONE, -1, None,
                self._get_devices_done, None)

    def _get_devices_done(self, source, result, user_data):
        try:
            value = source.call_finish(result)[0]
        except GLib.Error:
            return
        self._sync_devices(value.unpack())

    def _sync_devices(self, paths):
        new_paths = [str(p) for p in paths]
        old = set(self._device_paths)
        cur = set(new_paths)
        # 先移除消失的设备
        for gone in old - cur:
            self._proxies.pop((gone, IFACE_POLICY), None)
            self._strategies.pop(gone, None)
        self._device_paths = new_paths
        # 为新设备创建代理（代理就绪后会 fetch 策略表并 emit device-changed）
        for added in cur - old:
            self._new_proxy(added, IFACE_POLICY)
        self.emit("devices-changed")

    # ------------------------------------------------------------------
    # 策略表
    # ------------------------------------------------------------------
    def _fetch_strategies(self, path, proxy):
        proxy.call("ListStrategies", None, Gio.DBusCallFlags.NONE, -1, None,
                   self._strategies_done, path)

    def _strategies_done(self, source, result, path):
        try:
            value = source.call_finish(result)
        except GLib.Error as err:
            self.emit("action-error", "list-strategies", friendly_dbus_error(err))
            return
        self._strategies[path] = [(str(n), str(d)) for (n, d) in value.unpack()[0]]
        domain = self._domain_of(path)
        if domain is not None:
            self.emit("domain-changed", domain)
        else:
            self.emit("device-changed", path)

    # ------------------------------------------------------------------
    # 只读取数（全部来自代理缓存属性）
    # ------------------------------------------------------------------
    def _prop(self, path, iface, name, default):
        proxy = self._proxies.get((path, iface))
        if proxy is None:
            return default
        value = proxy.get_cached_property(name)
        return value.unpack() if value is not None else default

    def manager_props(self):
        """返回 {backend, kernel_release, version}；未连接返回 None。"""
        if (ROOT_PATH, IFACE_MANAGER) not in self._proxies:
            return None
        return {
            "backend": self._prop(ROOT_PATH, IFACE_MANAGER, "Backend", "system"),
            "kernel_release": self._prop(ROOT_PATH, IFACE_MANAGER, "KernelRelease", "?"),
            "version": self._prop(ROOT_PATH, IFACE_MANAGER, "Version", 0),
        }

    def domain_props(self, domain):
        """返回某域的属性字典；未连接返回 None。"""
        path = DOMAIN_PATHS.get(domain)
        if path is None or (path, IFACE_POLICY) not in self._proxies:
            return None
        return {
            "strategy": self._prop(path, IFACE_POLICY, "CurrentStrategy", ""),
            "params": list(self._prop(path, IFACE_POLICY, "CurrentParams", [])),
            "default": self._prop(path, IFACE_POLICY, "DefaultStrategy", ""),
            "description": self._prop(path, IFACE_POLICY, "Description", ""),
            "available": self._prop(path, IFACE_POLICY, "Available", True),
            "status_message": self._prop(path, IFACE_POLICY, "StatusMessage", ""),
        }

    def device_props(self, path):
        """返回设备对象属性字典；代理未就绪返回 None。"""
        if (path, IFACE_POLICY) not in self._proxies:
            return None
        return {
            "strategy": self._prop(path, IFACE_POLICY, "CurrentStrategy", ""),
            "params": list(self._prop(path, IFACE_POLICY, "CurrentParams", [])),
            "default": self._prop(path, IFACE_POLICY, "DefaultStrategy", ""),
            "description": self._prop(path, IFACE_POLICY, "Description", ""),
            "available": self._prop(path, IFACE_POLICY, "Available", True),
            "status_message": self._prop(path, IFACE_POLICY, "StatusMessage", ""),
        }

    def strategies(self, target):
        """target 为域键（cpu/cache/io）或对象路径；返回 [(name, desc)]。"""
        path = DOMAIN_PATHS.get(target, target)
        return list(self._strategies.get(path, []))

    def device_paths(self):
        return list(self._device_paths)

    @property
    def is_ready(self):
        return self._ready

    @property
    def bus_label(self):
        return "session" if self._bus_type == Gio.BusType.SESSION else "system"

    # ------------------------------------------------------------------
    # 改状态方法（异步；错误走 action-error 信号）
    # ------------------------------------------------------------------
    @staticmethod
    def _path_of(target):
        return DOMAIN_PATHS.get(target, target)

    def _policy_call(self, path, method, params_v, context):
        proxy = self._proxies.get((path, IFACE_POLICY))
        if proxy is None:
            self.emit("action-error", context or method,
                      _("The policy manager service is not connected."))
            return
        proxy.call(method, params_v, Gio.DBusCallFlags.NONE, -1, None,
                   self._call_done, context or method)

    def _call_done(self, source, result, context):
        try:
            source.call_finish(result)
        except GLib.Error as err:
            self.emit("action-error", context, friendly_dbus_error(err))
        else:
            self.emit("action-done", context)

    def apply(self, target, strategy, params=None, context=None):
        """应用策略。仅 CPU 域支持 params（ApplyStrategyWithParams）。"""
        path = self._path_of(target)
        ctx = context or ("apply %s" % strategy)
        if params and path == CPU_PATH:
            self._policy_call(path, "ApplyStrategyWithParams",
                              GLib.Variant("(sas)", (strategy, list(params))), ctx)
        else:
            self._policy_call(path, "ApplyStrategy",
                              GLib.Variant("(s)", (strategy,)), ctx)

    def stop(self, target, context=None):
        self._policy_call(self._path_of(target), "StopStrategy",
                          None, context or "stop strategy")

    def restart(self, target, context=None):
        self._policy_call(self._path_of(target), "RestartStrategy",
                          None, context or "restart strategy")

    def restore_default(self, target, context=None):
        self._policy_call(self._path_of(target), "RestoreDefault",
                          None, context or "restore default")
