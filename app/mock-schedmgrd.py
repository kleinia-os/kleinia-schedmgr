#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""mock-schedmgrd.py — io.kleinia.SchedMgr 的 Python 模拟 daemon（sim 后端）。

按 DBus API、v2 域模型与 sim 语义在
【会话总线】上实现完整接口，供 schedmgr GUI / GNOME Shell 扩展 / CI 在没有
C daemon 时联调：

  io.kleinia.Manager1  @ /io/kleinia/SchedMgr        Version/Backend/KernelRelease
  io.kleinia.Sched1   @ /io/kleinia/SchedMgr/Cpu    ListStrategies / ApplyStrategy(W ithParams)
                            /Cache   （v2：cax_* 域）        StopStrategy / RestartStrategy / RestoreDefault
                            /Io     （v2：全局 iox 域）      属性 + StrategyChanged / StrategyFailed
                            /Io/<dev>（v2：native ∪ iox 组合列表，有效名语义）
  io.kleinia.Io1       @ /io/kleinia/SchedMgr/Io     Devices:ao + DevicesChanged()

v2 语义要点：
  * CPU "none" = EEVDF（内核默认调度类，Linux 6.6 起无 CFS）；
  * Cache = cax_* 加载器域（"none" = 不加载 cache_ext 策略）；
  * 全局 /Io = iox_* 域：ApplyStrategy(iox_X) 启动全局加载器，none/Stop 停止
    （设备的 scx_io elevator 不自动回退）；
  * 设备 /Io/<dev>：ListStrategies = native elevator ∪ iox 项（iox 描述注明
    “全局 eBPF 策略”）；ApplyStrategy(iox_X) = ①确保全局 iox_X 运行 ②设备写
    scx_io；CurrentStrategy = 有效名（elevator≠scx_io → elevator 名；=scx_io →
    运行中的 iox 名，无加载器时为 "scx_io" 并置 StatusMessage 提示）。多设备
    选不同 iox 时后选生效，其余 scx_io 设备的有效名经 PropertiesChanged 联动。

调试/测试注入接口 io.kleinia.Mock1（根对象）：
  PokeEvent(s type, s arg):
      type=hotplug-add    arg=<dev>          模拟设备插入（自动 DevicesChanged）
      type=hotplug-remove arg=<dev>          模拟设备拔出
      type=fail-next      arg=<strategy|*>   下一次 Cpu 域 ApplyStrategy(该策略) 注入失败
                                             （daemon 回滚状态并发出 StrategyFailed）
      type=availability   arg="<cpu|cache|io> <on|off>"   注入域不可用（UI 禁用态测试）
  DumpState() -> s（全部内部状态 JSON，测试断言用）

sim 语义：只改内存态 + 发信号，绝不触碰真实 sysfs/进程；polkit 跳过。
"""

import json
import platform
import sys

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = "io.kleinia.SchedMgr"
ROOT_PATH = "/io/kleinia/SchedMgr"
CPU_PATH = ROOT_PATH + "/Cpu"
CACHE_PATH = ROOT_PATH + "/Cache"
IO_PATH = ROOT_PATH + "/Io"
IO_DEV_PREFIX = IO_PATH + "/"

IFACE_POLICY = "io.kleinia.Sched1"
IFACE_MANAGER = "io.kleinia.Manager1"
IFACE_IO = "io.kleinia.Io1"
IFACE_MOCK = "io.kleinia.Mock1"
ERROR_NS = "io.kleinia.Sched1."

NODE_XML = """
<node>
  <interface name="io.kleinia.Manager1">
    <property name="Version" type="u" access="read"/>
    <property name="Backend" type="s" access="read"/>
    <property name="KernelRelease" type="s" access="read"/>
  </interface>
  <interface name="io.kleinia.Mock1">
    <method name="PokeEvent">
      <arg type="s" name="type" direction="in"/>
      <arg type="s" name="arg" direction="in"/>
    </method>
    <method name="DumpState">
      <arg type="s" name="state" direction="out"/>
    </method>
  </interface>
  <interface name="io.kleinia.Sched1">
    <method name="ListStrategies">
      <arg type="a(ss)" name="strategies" direction="out"/>
    </method>
    <method name="ApplyStrategy">
      <arg type="s" name="strategy" direction="in"/>
    </method>
    <method name="ApplyStrategyWithParams">
      <arg type="s" name="strategy" direction="in"/>
      <arg type="as" name="args" direction="in"/>
    </method>
    <method name="StopStrategy"/>
    <method name="RestartStrategy"/>
    <method name="RestoreDefault"/>
    <property name="CurrentStrategy" type="s" access="read"/>
    <property name="CurrentParams" type="as" access="read"/>
    <property name="DefaultStrategy" type="s" access="read"/>
    <property name="Description" type="s" access="read"/>
    <property name="Available" type="b" access="read"/>
    <property name="StatusMessage" type="s" access="read"/>
    <signal name="StrategyChanged">
      <arg type="s" name="strategy"/>
      <arg type="as" name="params"/>
    </signal>
    <signal name="StrategyFailed">
      <arg type="s" name="strategy"/>
      <arg type="s" name="reason"/>
    </signal>
  </interface>
  <interface name="io.kleinia.Io1">
    <property name="Devices" type="ao" access="read"/>
    <signal name="DevicesChanged"/>
  </interface>
</node>
"""

# sim 静态策略表（描述中文，与真实 daemon 的中文
# Description 对齐；iox 项描述注明“全局 eBPF 策略”）
CPU_STRATEGIES = [
    ("none", "EEVDF（内核默认调度类，不加载 sched_ext 策略）"),
    ("scx_agent", "ACB 参考调度器"),
    ("scx_simple", "简单示例调度器"),
    ("scx_central", "中心队列调度器"),
]
CACHE_STRATEGIES = [
    ("none", "内核默认页缓存换出（不加载 cache_ext 策略）"),
    ("cax_mru", "MRU 淘汰策略（cache_ext）"),
    ("cax_lhd", "LHD 热数据感知淘汰（cache_ext）"),
    ("cax_s3fifo", "S3FIFO 淘汰（cache_ext）"),
]
# 全局 /Io = iox 域（§8.2）："none" = 停止全局加载器（设备 elevator 不回退）
IOX_STRATEGIES = [
    ("none", "无（停止全局 iox 策略加载器）"),
    ("iox_fifo", "FIFO 排序 · 全局 eBPF 策略"),
    ("iox_wedf", "加权 EDF · 全局 eBPF 策略"),
    ("iox_merge", "写合并优化 · 全局 eBPF 策略"),
]
# 设备：native elevator（sysfs 枚举，含 scx_io 开关本身）∪ iox 策略项
# （iox 项直接切换 = 切 scx_io elevator + 启动该全局 iox 策略）
NATIVE_DEV_STRATEGIES = [
    ("none", "无调度算法"),
    ("mq-deadline", "多队列截止时间"),
    ("kyber", "Kyber（低延迟导向）"),
    ("bfq", "带宽公平队列"),
    ("scx_io", "sched_ext elevator 开关（策略由全局 eBPF 策略提供）"),
]
IOX_DEV_ITEMS = [(n, d) for n, d in IOX_STRATEGIES if n != "none"]
DEV_STRATEGIES = NATIVE_DEV_STRATEGIES + IOX_DEV_ITEMS

# 设备 elevator=scx_io 且无全局加载器运行时的 StatusMessage（§8.2 有效名语义）
DEV_NO_IOX_STATUS = "scx_io elevator 生效，但未运行全局 iox 策略（选择任一 iox 项以启动）"


class MockError(Exception):
    """按 DBus API 语义抛出的 DBus 错误（名字在 io.kleinia.Sched1. 前缀下）。"""

    def __init__(self, short, message):
        super().__init__(message)
        self.name = ERROR_NS + short
        self.message = message


class SimDomain(object):
    """一个 Sched1 域对象（Cpu / Cache / 全局 Io / 每设备）的模拟状态。

    设备对象（v2 §8.2）额外跟踪 elevator（sysfs 状态）与 global_ref（指向全局
    iox 域）：CurrentStrategy = 有效名（elevator≠scx_io → elevator 名；=scx_io
    → 运行中的 iox 名，无加载器时为 "scx_io"）。emitted 缓存上次发出的
    (有效名, StatusMessage)，供联动去重。
    """

    def __init__(self, path, kind, description, strategies,
                 default="", current="", available=True, status="",
                 elevator=None):
        self.path = path
        self.kind = kind            # 'cpu' | 'cache' | 'io' | 'device'
        self.description = description
        self.strategies = list(strategies)
        self.default = default
        self.current = current      # "" = 未运行/none
        self.params = []
        self.last = current or default  # 最近一次成功应用（供 RestartStrategy）
        self.available = available
        self.status = status
        self.fail_next = None       # PokeEvent 注入的失败标记
        self.elevator = elevator if elevator is not None else \
            (current if kind == "device" else None)
        self.global_ref = None      # 设备对象 → 全局 iox 域（seed 时回填）
        self.emitted = None         # 设备：(有效名, StatusMessage) 上次已发值

    @property
    def names(self):
        return [name for name, _ in self.strategies]

    def effective(self):
        """CurrentStrategy 有效名（§8.2）；非设备域即 current。"""
        if self.kind != "device":
            return self.current
        if self.elevator != "scx_io":
            return self.elevator
        g = self.global_ref
        global_current = g.current if g is not None else ""
        return global_current if global_current else "scx_io"

    def status_now(self):
        """设备当前的 StatusMessage（scx_io 无加载器 → 提示；§8.2）。"""
        if self.kind == "device" and self.available \
                and self.elevator == "scx_io":
            g = self.global_ref
            if g is None or not g.current:
                return self.status or DEV_NO_IOX_STATUS
        return self.status

    def props(self):
        if self.kind == "device":
            return {
                "CurrentStrategy": self.effective(),
                "CurrentParams": [],
                "DefaultStrategy": self.default,
                "Description": self.description,
                "Available": self.available,
                "StatusMessage": self.status_now(),
            }
        return {
            "CurrentStrategy": self.current,
            "CurrentParams": list(self.params),
            "DefaultStrategy": self.default,
            "Description": self.description,
            "Available": self.available,
            "StatusMessage": self.status,
        }


class MockDaemon(object):
    def __init__(self, conn):
        self.conn = conn
        self.domains = {}          # path -> SimDomain
        self._dev_order = []       # 设备路径顺序
        self._reg_ids = []
        self._seed()
        self._register_all()

    # ------------------------------------------------------------------
    # sim 初始数据（v2 数据集）
    # ------------------------------------------------------------------
    def _seed(self):
        self.domains[CPU_PATH] = SimDomain(
            CPU_PATH, "cpu", "CPU 调度器", CPU_STRATEGIES,
            default="none", current="")
        self.domains[CACHE_PATH] = SimDomain(
            CACHE_PATH, "cache", "页缓存策略", CACHE_STRATEGIES,
            default="cax_mru", current="")
        # 全局 /Io = iox 域（v2 实装；sim 下可用，§8.6）
        self.domains[IO_PATH] = SimDomain(
            IO_PATH, "io", "全局 IO 策略（iox）", IOX_STRATEGIES,
            default="", current="")
        for dev, desc, default, elevator in (
                ("sda", "SATA SSD 512G", "mq-deadline", "kyber"),
                ("nvme0n1", "NVMe SSD 1T", "mq-deadline", "kyber"),
                ("nvme1n1", "NVMe SSD 2T", "mq-deadline", "kyber")):
            path = IO_DEV_PREFIX + dev
            domain = SimDomain(path, "device", desc, DEV_STRATEGIES,
                               default=default, current=elevator,
                               elevator=elevator)
            domain.global_ref = self.domains[IO_PATH]
            domain.emitted = (domain.effective(), domain.status_now())
            self.domains[path] = domain
            self._dev_order.append(path)

    # ------------------------------------------------------------------
    # 对象注册
    # ------------------------------------------------------------------
    def _register_all(self):
        node = Gio.DBusNodeInfo.new_for_xml(NODE_XML)
        manager_info = node.lookup_interface(IFACE_MANAGER)
        mock_info = node.lookup_interface(IFACE_MOCK)
        policy_info = node.lookup_interface(IFACE_POLICY)
        io_info = node.lookup_interface(IFACE_IO)

        self._reg_ids.append(self.conn.register_object(
            ROOT_PATH, manager_info,
            self._manager_method, self._get_property, None))
        self._reg_ids.append(self.conn.register_object(
            ROOT_PATH, mock_info,
            self._mock_method, None, None))
        self._reg_ids.append(self.conn.register_object(
            IO_PATH, io_info,
            self._io_method, self._get_property, None))
        for path in list(self.domains):
            if path == ROOT_PATH:
                continue
            self._reg_ids.append(self.conn.register_object(
                path, policy_info,
                self._policy_method, self._get_property, None))

    # ------------------------------------------------------------------
    # 属性读取（GDBus 自动处理 Properties.Get/GetAll 与 Introspect/Peer）
    # ------------------------------------------------------------------
    def _get_property(self, conn, sender, path, iface, prop):
        if iface == IFACE_MANAGER:
            values = {
                "Version": GLib.Variant("u", 1),
                "Backend": GLib.Variant("s", "sim"),
                "KernelRelease": GLib.Variant("s", platform.release()),
            }
            return values.get(prop)
        if iface == IFACE_POLICY:
            domain = self.domains.get(path)
            if domain is None or prop not in domain.props():
                return None
            value = domain.props()[prop]
            type_str = {"CurrentStrategy": "s", "CurrentParams": "as",
                        "DefaultStrategy": "s", "Description": "s",
                        "Available": "b", "StatusMessage": "s"}[prop]
            return GLib.Variant(type_str, value)
        if iface == IFACE_IO and path == IO_PATH and prop == "Devices":
            return GLib.Variant("ao", list(self._dev_order))
        return None

    def _manager_method(self, conn, sender, path, iface, method, params, invocation):
        # Manager1 无方法
        invocation.return_dbus_error(
            "org.freedesktop.DBus.Error.UnknownMethod",
            "io.kleinia.Manager1 defines no methods")

    def _io_method(self, conn, sender, path, iface, method, params, invocation):
        invocation.return_dbus_error(
            "org.freedesktop.DBus.Error.UnknownMethod",
            "io.kleinia.Io1 defines no methods (properties + DevicesChanged only)")

    # ------------------------------------------------------------------
    # Sched1 方法（sim 后端跳过 polkit）
    # ------------------------------------------------------------------
    def _policy_method(self, conn, sender, path, iface, method, params, invocation):
        domain = self.domains.get(path)
        if domain is None:
            invocation.return_dbus_error(ERROR_NS + "Failed", "unknown object %s" % path)
            return
        try:
            if method == "ListStrategies":
                invocation.return_value(GLib.Variant("(a(ss))", (domain.strategies,)))
            elif method == "ApplyStrategy":
                (strategy,) = params.unpack()
                self._apply(domain, strategy, [])
                invocation.return_value(None)
            elif method == "ApplyStrategyWithParams":
                strategy, args = params.unpack()
                if domain.kind != "cpu" and args:
                    # 仅 CPU 域有意义；其余传非空 args 报 NotSupported
                    raise MockError("NotSupported",
                                    "parameters are only supported on the CPU domain")
                self._apply(domain, strategy, list(args))
                invocation.return_value(None)
            elif method == "StopStrategy":
                self._stop(domain)
                invocation.return_value(None)
            elif method == "RestartStrategy":
                self._restart(domain)
                invocation.return_value(None)
            elif method == "RestoreDefault":
                self._restore_default(domain)
                invocation.return_value(None)
            else:
                invocation.return_dbus_error(
                    "org.freedesktop.DBus.Error.UnknownMethod",
                    "no such method %s" % method)
        except MockError as err:
            self._log("ERR  %s %s -> %s: %s" % (path.rsplit("/", 1)[-1], method, err.name, err.message))
            invocation.return_dbus_error(err.name, err.message)

    def _require_available(self, domain):
        if not domain.available:
            raise MockError("NotAvailable",
                            domain.status or "domain is not available")

    def _apply(self, domain, strategy, params):
        self._require_available(domain)
        if strategy not in domain.names:
            raise MockError("NoSuchStrategy", "no such strategy '%s'" % strategy)
        # PokeEvent fail-next 注入：确认失败 -> 回滚（状态不变）+ StrategyFailed
        if domain.fail_next and domain.fail_next in (strategy, "*"):
            domain.fail_next = None
            reason = "模拟应用 '%s' 失败（Mock1.PokeEvent fail-next）" % strategy
            self._emit_failed(domain, strategy, reason)
            raise MockError("Failed", reason)
        domain.last = strategy
        if domain.kind == "device":
            # v2（§8.2）：iox 项 = ①确保全局 iox_X 运行 ②设备写 scx_io；
            # native 项 = 写 sysfs（v1 语义不变）
            if strategy.startswith("iox_"):
                self._set_global_io(strategy)
                domain.elevator = "scx_io"
            else:
                domain.elevator = strategy
            domain.current = strategy
            self._log("APPLY %s <- %s" % (domain.path.rsplit("/", 1)[-1],
                                          strategy))
            self._sync_device_props()
            return
        if domain.kind == "io":
            # 全局 iox 域："none" = 停止全局加载器（设备 elevator 不回退）
            self._set_global_io(strategy)
            self._log("APPLY Io <- %s" % strategy)
            return
        # CPU / Cache 的 "none" = 卸载回内核默认（EEVDF / 默认换出），
        # CurrentStrategy 规约为 ""（cache 域 v2 同构）
        if strategy == "none" and domain.kind in ("cpu", "cache"):
            domain.current, domain.params = "", []
        else:
            domain.current, domain.params = strategy, list(params)
        self._log("APPLY %s <- %s %s" % (domain.path.rsplit("/", 1)[-1], strategy, params))
        self._emit_changed(domain)

    # ------------------------------------------------------------------
    # 全局 iox 加载器（§8.2）：name="none" 表示停止。全局策略是内核唯一事实
    # ——变化后所有 elevator=scx_io 设备的有效名经 PropertiesChanged 联动。
    # ------------------------------------------------------------------
    def _set_global_io(self, name):
        g = self.domains[IO_PATH]
        g.current, g.params = ("" if name == "none" else name), []
        if name != "none":
            g.last = name
        self._emit_changed(g)
        self._sync_device_props()

    def _sync_device_props(self):
        """对所有设备去重地发 PropertiesChanged/StrategyChanged（有效名联动）。"""
        for path in self._dev_order:
            domain = self.domains.get(path)
            if domain is None:
                continue
            state = (domain.effective(), domain.status_now())
            if domain.emitted == state:
                continue
            domain.emitted = state
            self._emit_props(path, IFACE_POLICY, {
                "CurrentStrategy": GLib.Variant("s", state[0]),
                "StatusMessage": GLib.Variant("s", state[1])})
            self.conn.emit_signal(None, path, IFACE_POLICY, "StrategyChanged",
                                  GLib.Variant("(sas)", (state[0], [])))

    def _stop(self, domain):
        self._require_available(domain)
        if domain.kind == "device":
            # 设备 Stop = 恢复该设备 DefaultStrategy
            if domain.default == "":
                raise MockError("NoDefault", "device %s has no default" % domain.path)
            self._apply(domain, domain.default, [])
            return
        if domain.kind == "io":
            # v2（§8.2）：停全局加载器；设备的 scx_io elevator 不自动回退
            if domain.current == "":
                raise MockError("NotRunning", "no strategy is running")
            domain.current, domain.params = "", []
            self._log("STOP  Io")
            self._emit_changed(domain)
            self._sync_device_props()
            return
        if domain.current == "":
            raise MockError("NotRunning", "no strategy is running")
        domain.current, domain.params = "", []
        self._log("STOP  %s" % domain.path.rsplit("/", 1)[-1])
        self._emit_changed(domain)

    def _restart(self, domain):
        self._require_available(domain)
        if domain.kind == "io":
            if domain.current != "":
                self._apply(domain, domain.current, [])
            elif domain.last not in ("", None, "none"):
                self._apply(domain, domain.last, [])
            else:
                raise MockError("NotRunning", "no strategy to restart")
            return
        if domain.current != "":
            self._apply(domain, domain.current, list(domain.params))
        elif domain.last not in ("", None):
            self._apply(domain, domain.last, [])
        else:
            raise MockError("NotRunning", "no strategy to restart")

    def _restore_default(self, domain):
        self._require_available(domain)
        if domain.default == "":
            raise MockError("NoDefault", "no default strategy configured")
        self._apply(domain, domain.default, [])

    # ------------------------------------------------------------------
    # Mock1 调试接口
    # ------------------------------------------------------------------
    def _mock_method(self, conn, sender, path, iface, method, params, invocation):
        try:
            if method == "PokeEvent":
                type_, arg = params.unpack()
                self._poke(type_.strip(), (arg or "").strip())
                invocation.return_value(None)
            elif method == "DumpState":
                invocation.return_value(GLib.Variant("(s)", (self.dump_state(),)))
            else:
                invocation.return_dbus_error(
                    "org.freedesktop.DBus.Error.UnknownMethod", "no such method %s" % method)
        except MockError as err:
            invocation.return_dbus_error(err.name, err.message)

    def _poke(self, type_, arg):
        if type_ == "hotplug-add":
            if not arg or "/" in arg:
                raise MockError("Failed", "invalid device name '%s'" % arg)
            path = IO_DEV_PREFIX + arg
            if path in self.domains:
                raise MockError("Failed", "device %s already exists" % arg)
            domain = SimDomain(path, "device", "模拟热插拔磁盘",
                               DEV_STRATEGIES, default="mq-deadline",
                               current="mq-deadline", elevator="mq-deadline")
            domain.global_ref = self.domains[IO_PATH]
            domain.emitted = (domain.effective(), domain.status_now())
            self.domains[path] = domain
            self._dev_order.append(path)
            self.conn.register_object(path, self._policy_info(), self._policy_method,
                                      self._get_property, None)
            self._set_devices()
            self._log("HOTPLUG add %s" % arg)
        elif type_ == "hotplug-remove":
            path = IO_DEV_PREFIX + arg
            if path not in self.domains:
                raise MockError("Failed", "no such device '%s'" % arg)
            del self.domains[path]
            self._dev_order.remove(path)
            self._set_devices()
            self._log("HOTPLUG remove %s" % arg)
        elif type_ == "fail-next":
            cpu = self.domains[CPU_PATH]
            cpu.fail_next = arg or "*"
            self._log("INJECT fail-next=%s (cpu)" % cpu.fail_next)
        elif type_ == "availability":
            parts = arg.split()
            if len(parts) != 2 or parts[0] not in ("cpu", "cache", "io") \
                    or parts[1] not in ("on", "off"):
                raise MockError("Failed",
                                "usage: availability <cpu|cache|io> <on|off>")
            domain = {"cpu": self.domains[CPU_PATH],
                      "cache": self.domains[CACHE_PATH],
                      "io": self.domains[IO_PATH]}[parts[0]]
            domain.available = (parts[1] == "on")
            domain.status = "" if domain.available else "模拟不可用（Mock1.PokeEvent）"
            self._emit_props(domain.path, IFACE_POLICY, {
                "Available": GLib.Variant("b", domain.available),
                "StatusMessage": GLib.Variant("s", domain.status)})
            self._log("INJECT availability %s" % arg)
        else:
            raise MockError("Failed",
                            "unknown poke type '%s' (hotplug-add|hotplug-remove|fail-next|availability)" % type_)

    def _policy_info(self):
        if not hasattr(self, "_policy_info_cache"):
            self._policy_info_cache = Gio.DBusNodeInfo.new_for_xml(NODE_XML).lookup_interface(IFACE_POLICY)
        return self._policy_info_cache

    def dump_state(self):
        data = {"_devices": list(self._dev_order)}
        for path, domain in self.domains.items():
            data[path] = domain.props()
        return json.dumps(data, indent=1, sort_keys=True)

    # ------------------------------------------------------------------
    # 信号发送
    # ------------------------------------------------------------------
    def _emit_props(self, path, iface, props):
        # 先发 PropertiesChanged（客户端代理缓存随之更新），再发业务信号
        self.conn.emit_signal(None, path, "org.freedesktop.DBus.Properties",
                              "PropertiesChanged",
                              GLib.Variant("(sa{sv}as)", (iface, props, [])))

    def _emit_changed(self, domain):
        self._emit_props(domain.path, IFACE_POLICY, {
            "CurrentStrategy": GLib.Variant("s", domain.current),
            "CurrentParams": GLib.Variant("as", domain.params)})
        self.conn.emit_signal(None, domain.path, IFACE_POLICY, "StrategyChanged",
                              GLib.Variant("(sas)", (domain.current, domain.params)))

    def _emit_failed(self, domain, strategy, reason):
        self.conn.emit_signal(None, domain.path, IFACE_POLICY, "StrategyFailed",
                              GLib.Variant("(ss)", (strategy, reason)))

    def _set_devices(self):
        self._emit_props(IO_PATH, IFACE_IO,
                         {"Devices": GLib.Variant("ao", list(self._dev_order))})
        self.conn.emit_signal(None, IO_PATH, IFACE_IO, "DevicesChanged",
                              GLib.Variant("()", ()))

    @staticmethod
    def _log(msg):
        print("mock-schedmgrd: %s" % msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# 总线占有与主循环
# ---------------------------------------------------------------------------
_DAEMON = None  # 防 GC


def main():
    owner_id = Gio.bus_own_name(
        Gio.BusType.SESSION, BUS_NAME, Gio.BusNameOwnerFlags.NONE,
        _on_bus_acquired, _on_name_acquired, _on_name_lost)
    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        Gio.bus_unown_name(owner_id)


def _on_bus_acquired(conn, name):
    global _DAEMON
    _DAEMON = MockDaemon(conn)
    print("mock-schedmgrd: objects registered", file=sys.stderr, flush=True)


def _on_name_acquired(conn, name):
    print("mock-schedmgrd: ready on session bus as %s (backend=sim)" % name,
          file=sys.stderr, flush=True)


def _on_name_lost(conn, name):
    print("mock-schedmgrd: lost/failed to acquire %s — another instance running?" % name,
          file=sys.stderr, flush=True)
    raise SystemExit(1)


if __name__ == "__main__":
    if "--help" in sys.argv or "-h" in sys.argv:
        print(__doc__)
        sys.exit(0)
    main()
