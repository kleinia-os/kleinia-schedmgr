// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
// dbus.js — SchedMgr DBus 接口定义与代理类
//
// 手写的 DBus 接口 XML；本模块只依赖 gi://，
// 因此可同时被 GNOME Shell 扩展（extension.js）和独立 gjs 测试
// （test/client-test.js）导入，保证两端的接口定义完全一致。

import Gio from 'gi://Gio';

export const BUS_NAME = 'io.kleinia.SchedMgr';
export const ROOT_PATH = '/io/kleinia/SchedMgr';

export const cpuPath = () => `${ROOT_PATH}/Cpu`;
export const cachePath = () => `${ROOT_PATH}/Cache`;
export const ioPath = () => `${ROOT_PATH}/Io`;
export const ioDevicePath = dev => `${ROOT_PATH}/Io/${dev}`;

/** 根对象 — io.kleinia.Manager1 */
export const MANAGER_XML = `
<node>
  <interface name="io.kleinia.Manager1">
    <property name="Version" type="u" access="read"/>
    <property name="Backend" type="s" access="read"/>
    <property name="KernelRelease" type="s" access="read"/>
  </interface>
</node>`;

/** 域对象 — io.kleinia.Sched1 */
export const POLICY_XML = `
<node>
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
</node>`;

/** IO 设备发现 — io.kleinia.Io1（挂在 /io/kleinia/SchedMgr/Io 上） */
export const IO_XML = `
<node>
  <interface name="io.kleinia.Io1">
    <property name="Devices" type="ao" access="read"/>
    <signal name="DevicesChanged"/>
  </interface>
</node>`;

// makeProxyWrapper 返回的是构造函数，必须 new（GNOME 51 起不再允许当函数调用）。
export const ManagerProxy = Gio.DBusProxy.makeProxyWrapper(MANAGER_XML);
export const PolicyProxy = Gio.DBusProxy.makeProxyWrapper(POLICY_XML);
export const IoListProxy = Gio.DBusProxy.makeProxyWrapper(IO_XML);
