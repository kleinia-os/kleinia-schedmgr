#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""main.py — io.kleinia.SchedMgr 应用入口（Adw.Application）。

用法：
  kleinia-schedmgr [--session] [--selftest N]

  --session               连接会话总线（默认 system 总线）
  KLEINIA_SCHEDMGR_BUS=session   环境变量等效于 --session
  --selftest N            隐藏自检：启动、建窗、realize、主循环跑 N 秒后自动退出 0
                          （期间 0.6s 时经客户端链路真实调用一次 ApplyStrategy，
                          供 e2e 验证 app→daemon 全链路；无头验证友好）
"""

import os
import sys

import gi

try:
    gi.require_version("Adw", "1.0")
except ValueError:  # 新 typelib 工具链（如本机 libadwaita 1.10）版本号注册为 "1"
    gi.require_version("Adw", "1")
gi.require_version("Gtk", "4.0")
from gi.repository import Adw, Gio, GLib  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import i18n  # noqa: E402,F401  （gettext 初始化：尽早于 window/pages 导入）
from window import SchedMgrWindow  # noqa: E402

APP_ID = "io.kleinia.SchedMgr"


class SchedMgrApplication(Adw.Application):
    def __init__(self, bus_type=Gio.BusType.SYSTEM, selftest=0):
        # NON_UNIQUE：自检/测试可能并行起多个实例，无需单例激活
        super().__init__(application_id=APP_ID,
                         flags=Gio.ApplicationFlags.NON_UNIQUE)
        self._bus_type = bus_type
        self._selftest = selftest

    def do_activate(self):
        win = self.props.active_window
        if win is None:
            win = SchedMgrWindow(self, self._bus_type)
            self.add_window(win)
        try:
            win.realize()  # 自检要求：建窗后 realize
        except Exception:
            pass
        win.present()
        if self._selftest > 0:
            GLib.timeout_add(600, self._selftest_apply)
            GLib.timeout_add_seconds(self._selftest, self._selftest_exit)

    def _selftest_apply(self):
        win = self.props.active_window
        if win is not None:
            win.selftest_apply()
        return GLib.SOURCE_REMOVE

    def _selftest_exit(self):
        print("selftest: window realized, main loop ran %ds — OK" % self._selftest,
              flush=True)
        self.quit()
        return GLib.SOURCE_REMOVE


def _parse_args(argv):
    bus = "system"
    selftest = 0
    rest = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--session":
            bus = "session"
        elif arg == "--selftest":
            i += 1
            selftest = int(argv[i]) if i < len(argv) else 0
        elif arg.startswith("--selftest="):
            selftest = int(arg.split("=", 1)[1])
        elif arg in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        else:
            rest.append(arg)
        i += 1
    return bus, selftest, rest


def main():
    bus, selftest, rest = _parse_args(sys.argv[1:])
    if os.environ.get("KLEINIA_SCHEDMGR_BUS", "").strip().lower() == "session":
        bus = "session"
    bus_type = Gio.BusType.SESSION if bus == "session" else Gio.BusType.SYSTEM
    app = SchedMgrApplication(bus_type=bus_type, selftest=selftest)
    sys.exit(app.run(rest))


if __name__ == "__main__":
    main()
