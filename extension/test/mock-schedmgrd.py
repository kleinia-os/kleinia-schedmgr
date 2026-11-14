#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
# mock-schedmgrd.py — schedmgrd 的 sim 后端 mock（测试专用）
#
# 按 v2 域模型完整实现：
#   - io.kleinia.Manager1（根对象，只读属性）
#   - io.kleinia.Sched1（Cpu / Cache(cax_*) / Io 全局(iox_*) / Io 每设备
#     = native elevator ∪ iox 组合列表，含 §8.2 的 iox 全局唯一/有效名跟随）
#   - io.kleinia.Io1（Devices 属性 + DevicesChanged 信号）
# 附加一个明确标注 TEST-ONLY 的 io.kleinia.Mock1（Hotplug / FailNext），
# 供 client-test.js 驱动 DevicesChanged / StrategyFailed 信号；真实 daemon 无此接口。
#
# 用法：mock-schedmgrd.py [--session|--system]（默认 --session，配合 dbus-run-session）

import argparse
import platform
import signal as os_signal
import sys

import gi

gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib  # noqa: E402

BUS_NAME = 'io.kleinia.SchedMgr'
ROOT = '/io/kleinia/SchedMgr'

INTERFACE_XML = """
<node>
  <interface name="io.kleinia.Manager1">
    <property name="Version" type="u" access="read"/>
    <property name="Backend" type="s" access="read"/>
    <property name="KernelRelease" type="s" access="read"/>
  </interface>
  <interface name="io.kleinia.Sched1">
    <method name="ListStrategies">
      <arg type="a(ss)" direction="out"/>
    </method>
    <method name="ApplyStrategy">
      <arg type="s" direction="in" name="strategy"/>
    </method>
    <method name="ApplyStrategyWithParams">
      <arg type="s" direction="in" name="strategy"/>
      <arg type="as" direction="in" name="args"/>
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
  <!-- TEST-ONLY：驱动测试信号，真实 daemon 不实现 -->
  <interface name="io.kleinia.Mock1">
    <method name="Hotplug">
      <arg type="s" direction="in" name="command"/>
    </method>
    <method name="FailNext">
      <arg type="s" direction="in" name="reason"/>
    </method>
  </interface>
</node>
"""

NODE_INFO = Gio.DBusNodeInfo.new_for_xml(INTERFACE_XML)
IFACE_MANAGER = NODE_INFO.lookup_interface('io.kleinia.Manager1')
IFACE_POLICY = NODE_INFO.lookup_interface('io.kleinia.Sched1')
IFACE_IO = NODE_INFO.lookup_interface('io.kleinia.Io1')
IFACE_MOCK = NODE_INFO.lookup_interface('io.kleinia.Mock1')


class SchedMgrError(Exception):
    def __init__(self, name, message):
        super().__init__(message)
        self.name = name  # 如 'NoSuchStrategy' → io.kleinia.Sched1.NoSuchStrategy


class PolicyDomain:
    """一个 io.kleinia.Sched1 域对象（纯内存状态）"""

    def __init__(self, conn, path, description, strategies,
                 current='', params=None, default='', available=True,
                 status_message='', accepts_params=False, on_apply=None):
        self.conn = conn
        self.path = path
        self.description = description
        self.strategies = strategies        # [(name, desc), ...]
        self.current = current              # '' = none
        self.params = params or []
        self.default = default
        self.available = available
        self.status_message = status_message
        self.accepts_params = accepts_params
        self.on_apply = on_apply            # 钩子：设备域组合策略（§8.2）
        self.last_applied = None            # (strategy, params) 供 RestartStrategy
        self.pending_fail_reason = None     # TEST-ONLY FailNext
        self.reg_id = conn.register_object(
            path, IFACE_POLICY, self._method_call, self._get_property, None)

    def unregister(self):
        if self.reg_id:
            self.conn.unregister_object(self.reg_id)
            self.reg_id = 0

    # ---- DBus -------------------------------------------------------------

    def _method_call(self, conn, sender, path, iface, method, params, invocation):
        try:
            if method == 'ListStrategies':
                invocation.return_value(
                    GLib.Variant('(a(ss))', (self.strategies,)))
                return
            # 改状态方法：Available=false 一律 NotAvailable
            if not self.available:
                raise SchedMgrError('NotAvailable', self.status_message or 'not available')

            if method == 'ApplyStrategy':
                (name,) = params
                self._apply(name, [])
            elif method == 'ApplyStrategyWithParams':
                name, args = params
                if args and not self.accepts_params:
                    raise SchedMgrError('NotSupported',
                                      'parameters only supported on the CPU domain')
                self._apply(name, list(args))
            elif method == 'StopStrategy':
                if self.current == '':
                    raise SchedMgrError('NotRunning', 'no strategy is running')
                self._set_state('', [])
            elif method == 'RestartStrategy':
                if self.last_applied is None and self.current == '':
                    raise SchedMgrError('NotRunning', 'no strategy to restart')
                name, args = self.last_applied or (self.current, self.params)
                self._apply(name, args)
            elif method == 'RestoreDefault':
                if self.default == '':
                    raise SchedMgrError('NoDefault', 'no default strategy configured')
                self._apply(self.default, [])
            else:
                invocation.return_dbus_error(
                    'org.freedesktop.DBus.Error.UnknownMethod', method)
                return
            invocation.return_value(None)
        except SchedMgrError as e:
            invocation.return_dbus_error(
                f'io.kleinia.Sched1.{e.name}', str(e))

    def _apply(self, name, args):
        names = [n for n, _ in self.strategies]
        if name not in names:
            raise SchedMgrError('NoSuchStrategy', f'no such strategy: {name}')
        # TEST-ONLY：模拟“确认失败 → 状态回滚 + StrategyFailed”（状态一致性铁律）
        if self.pending_fail_reason is not None:
            reason = self.pending_fail_reason
            self.pending_fail_reason = None
            self.conn.emit_signal(
                None, self.path, 'io.kleinia.Sched1', 'StrategyFailed',
                GLib.Variant('(ss)', (name, reason)))
            raise SchedMgrError('Failed', reason)
        self.last_applied = (name, args)
        if self.on_apply is not None:
            self.on_apply(name)      # 设备域组合策略语义（§8.2）
        elif name == 'none':
            self._set_state('', [])
        else:
            self._set_state(name, args)

    def _set_state(self, strategy, params):
        """状态只在“生效确认”后更新：属性 + StrategyChanged 一起发（铁律）"""
        self.current = strategy
        self.params = params
        changed = {
            'CurrentStrategy': GLib.Variant('s', self.current),
            'CurrentParams': GLib.Variant('as', self.params),
        }
        self.conn.emit_signal(
            None, self.path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
            GLib.Variant('(sa{sv}as)', ('io.kleinia.Sched1', changed, [])))
        self.conn.emit_signal(
            None, self.path, 'io.kleinia.Sched1', 'StrategyChanged',
            GLib.Variant('(sas)', (self.current, self.params)))

    def _get_property(self, conn, sender, path, iface, prop):
        values = {
            'CurrentStrategy': GLib.Variant('s', self.current),
            'CurrentParams': GLib.Variant('as', self.params),
            'DefaultStrategy': GLib.Variant('s', self.default),
            'Description': GLib.Variant('s', self.description),
            'Available': GLib.Variant('b', self.available),
            'StatusMessage': GLib.Variant('s', self.status_message),
        }
        v = values.get(prop)
        if v is None:
            print(f'mock-schedmgrd: unknown property {prop}', file=sys.stderr)
        return v


class MockSchedMgrd:
    """sim 后端假数据（v2 域模型）：绝不触碰真实 sysfs/进程"""

    CPU_STRATS = [
        ('none', 'Kernel default EEVDF scheduler'),
        ('scx_agent', 'ACB sched_ext scheduler'),
        ('scx_simple', 'Simple sched_ext scheduler'),
        ('scx_central', 'Central server sched_ext scheduler'),
    ]
    # §8.3：cax_* 策略加载器（cache_ext spawn 模型）
    CACHE_STRATS = [
        ('cax_mru', 'MRU cache_ext policy'),
        ('cax_lhd', 'LHD cache_ext policy'),
        ('cax_s3fifo', 'S3-FIFO cache_ext policy'),
    ]
    # §8.2：全局 iox 策略域（与 Cpu 同构；none = 停加载器）
    IOX_STRATS = [
        ('none', 'Stop the iox loader'),
        ('iox_fifo', 'FIFO global eBPF IO policy'),
        ('iox_wedf', 'Weighted EDF global eBPF IO policy'),
        ('iox_merge', 'Merge global eBPF IO policy'),
    ]
    # 设备 = native elevator ∪ iox 组合列表（§8.2）
    DEV_NATIVE = [
        ('none', 'No scheduler'),
        ('mq-deadline', 'Multi-queue deadline'),
        ('kyber', 'Kyber'),
        ('bfq', 'Budget fair queueing'),
        ('scx_io', 'sched_ext IO scheduler'),
    ]
    DEV_STRATS = DEV_NATIVE + [
        (name, f'{desc} (global eBPF policy)')
        for name, desc in IOX_STRATS if name != 'none'
    ]
    INITIAL_DEVICES = {
        'sda': 'SATA SSD 512G',
        'nvme0n1': 'NVMe SSD 1T',
        'nvme1n1': 'NVMe SSD',
    }

    def __init__(self, conn):
        self.conn = conn
        self.devices = {}   # dev name → PolicyDomain
        self.regs = []

        # 根对象：Manager1 + Mock1（TEST-ONLY）
        self.regs.append(conn.register_object(
            ROOT, IFACE_MANAGER, None,
            self._manager_get_property, None))
        self.regs.append(conn.register_object(
            ROOT, IFACE_MOCK, self._mock_method_call, None, None))

        # Cpu / Cache
        self.cpu = PolicyDomain(
            conn, f'{ROOT}/Cpu', 'CPU 调度器', list(self.CPU_STRATS),
            accepts_params=True)
        self.cache = PolicyDomain(
            conn, f'{ROOT}/Cache', '页缓存策略', list(self.CACHE_STRATS))

        # Io 全局 = iox 策略域（§8.2，与 Cpu 同构）+ Io1 设备发现
        self.io_global = PolicyDomain(
            conn, f'{ROOT}/Io', 'IO 策略', list(self.IOX_STRATS))
        self.regs.append(conn.register_object(
            f'{ROOT}/Io', IFACE_IO, None, self._io_get_property, None))

        for dev, model in self.INITIAL_DEVICES.items():
            self._add_device(dev, model)

    # ---- Manager1 ----------------------------------------------------------

    def _manager_get_property(self, conn, sender, path, iface, prop):
        values = {
            'Version': GLib.Variant('u', 1),
            'Backend': GLib.Variant('s', 'sim'),
            'KernelRelease': GLib.Variant('s', platform.release()),
        }
        return values.get(prop)

    # ---- Io1 ---------------------------------------------------------------

    def _io_get_property(self, conn, sender, path, iface, prop):
        if prop == 'Devices':
            paths = [f'{ROOT}/Io/{d}' for d in sorted(self.devices)]
            return GLib.Variant('ao', paths)
        return None

    def _emit_devices_changed(self):
        paths = [f'{ROOT}/Io/{d}' for d in sorted(self.devices)]
        self.conn.emit_signal(
            None, f'{ROOT}/Io', 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
            GLib.Variant('(sa{sv}as)',
                         ('io.kleinia.Io1', {'Devices': GLib.Variant('ao', paths)}, [])))
        self.conn.emit_signal(
            None, f'{ROOT}/Io', 'io.kleinia.Io1', 'DevicesChanged', None)

    def _dev_apply(self, dev, name):
        """设备域组合策略（§8.2）：
        - native → 写 elevator（CurrentStrategy = elevator 名）
        - iox_X → ①确保全局 iox_X 运行 ②设备 elevator=scx_io
          ③CurrentStrategy = 运行中的 iox 名（全局唯一，后选生效，
          其余 scx_io 态设备的有效名自动跟随，各发各的信号）。"""
        if name.startswith('iox_') and self.io_global.current != name:
            self.io_global._set_state(name, [])
        strategy = 'scx_io' if name.startswith('iox_') else name
        domain = self.devices[dev]
        if strategy == 'scx_io':
            if self.io_global.current:
                domain._set_state(self.io_global.current, [])
            else:
                # §8.2：无加载器运行时显示 scx_io 并置 StatusMessage 提示
                domain.status_message = 'no iox policy is running'
                domain._set_state('scx_io', [])
                domain.status_message = ''
        else:
            domain._set_state(strategy, [])
        for other, d in self.devices.items():
            if other != dev and (d.current.startswith('iox_') or
                                 d.current == 'scx_io'):
                eff = self.io_global.current or 'scx_io'
                if d.current != eff:
                    d._set_state(eff, [])

    def _add_device(self, dev, model, current='kyber'):
        if dev in self.devices:
            raise SchedMgrError('Failed', f'device already present: {dev}')
        domain = PolicyDomain(
            self.conn, f'{ROOT}/Io/{dev}', model, list(self.DEV_STRATS),
            current=current, on_apply=lambda name, d=dev: self._dev_apply(d, name))
        self.devices[dev] = domain

    def _remove_device(self, dev):
        if dev not in self.devices:
            raise SchedMgrError('Failed', f'no such device: {dev}')
        self.devices.pop(dev).unregister()

    # ---- Mock1（TEST-ONLY）--------------------------------------------------

    def _mock_method_call(self, conn, sender, path, iface, method, params, invocation):
        try:
            if method == 'Hotplug':
                (command,) = params
                op, _, dev = command.partition(' ')
                if op == 'add':
                    self._add_device(dev, f'Hotplug Disk ({dev})',
                                     current='mq-deadline')
                elif op == 'remove':
                    self._remove_device(dev)
                else:
                    raise SchedMgrError('Failed', f'usage: add|remove <dev>')
                self._emit_devices_changed()
            elif method == 'FailNext':
                (reason,) = params
                self.cpu.pending_fail_reason = reason
            else:
                invocation.return_dbus_error(
                    'org.freedesktop.DBus.Error.UnknownMethod', method)
                return
            invocation.return_value(None)
        except SchedMgrError as e:
            invocation.return_dbus_error(
                f'io.kleinia.Sched1.{e.name}', str(e))

    def shutdown(self):
        self.cpu.unregister()
        self.cache.unregister()
        self.io_global.unregister()
        for d in self.devices.values():
            d.unregister()
        self.devices.clear()
        for reg in self.regs:
            self.conn.unregister_object(reg)
        self.regs.clear()


def main():
    ap = argparse.ArgumentParser(description='schedmgrd sim mock for tests')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--session', action='store_true', default=True,
                   help='own the session bus (default)')
    g.add_argument('--system', dest='session', action='store_false',
                   help='own the system bus (requires root + policy)')
    args = ap.parse_args()

    bus_type = Gio.BusType.SESSION if args.session else Gio.BusType.SYSTEM
    loop = GLib.MainLoop()
    daemon = {}

    def on_bus_acquired(conn, name):
        daemon['obj'] = MockSchedMgrd(conn)
        print(f'mock-schedmgrd: ready on {name} '
              f'({"session" if args.session else "system"})', flush=True)

    def on_name_lost(conn, name):
        print(f'mock-schedmgrd: lost name {name}', file=sys.stderr, flush=True)
        loop.quit()

    owner_id = Gio.bus_own_name(
        bus_type, BUS_NAME, Gio.BusNameOwnerFlags.NONE,
        on_bus_acquired, lambda *_: None, on_name_lost)

    def on_term(*_):
        if loop.is_running():
            if daemon.get('obj'):
                daemon['obj'].shutdown()
            Gio.bus_unown_name(owner_id)
            loop.quit()

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, os_signal.SIGTERM, on_term)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, os_signal.SIGINT, on_term)

    try:
        loop.run()
    finally:
        on_term()
    return 0


if __name__ == '__main__':
    sys.exit(main())
