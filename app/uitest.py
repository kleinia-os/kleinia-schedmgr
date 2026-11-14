#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""uitest.py — io.kleinia.SchedMgr UI 结构与行为断言（v2 域模型改版后）。

在一条**私有测试总线**（socket 激活的 dbus-broker，等效 dbus-run-session）
上运行 mock-schedmgrd.py（v2 语义：cax_* Cache 域、全局 iox 域、
设备 native∪iox 组合列表 + 有效名），再经 KLEINIA_SCHEDMGR_ADDRESS 测试钩子
在进程内真实构建 SchedMgrWindow，按步骤断言：

  * 信息架构：状态组在每页首位、操作组（含按钮行）在其下、服务信息组在页尾；
  * sim 常驻横幅存在且文案正确；
  * 初始状态行（无 · EEVDF（内核默认调度类）/已停止）、默认策略行、按钮样式
    与灵敏度（**EEVDF 命名**，双语言断言）；
  * **Cache 页 v2**：策略表 none + cax_*、应用/停止/恢复默认全链路 + 状态行
    实时跟随；Available=false 时只显示警告行（占位文案）；
  * **全局 IO 组 v2**：iox 选择 + 应用/停止、副标题实时显示当前全局策略与
    “全局 eBPF 策略”说明、Available=false 占位（警告行）；
  * **iox 设备项**：设备组合列表含 iox 项（描述带“全局 eBPF 策略”标记），
    直接切换即“切 scx_io + 启动该 iox”（设备副标题显示有效名）；
  * **联动刷新**：多设备选不同 iox 后选生效、其余 scx_io 设备有效名经
    PropertiesChanged 自动跟随；别处改全局 iox 同样跟随且不弹 Toast；
  * 操作成功 Toast（已切换到…/已停止/已重启当前策略/已恢复默认策略/设备…）；
  * busy 防连点（点击后立即 insensitive，完成后恢复）；
  * 停止需确认（取消不生效 / 确认生效）；批量操作需确认且汇总行计数正确
    （“使用 scx_io”计数含运行 iox 策略的设备）；
  * Available=false → 操作组整体隐藏、状态组只显示警告行；
  * 设备热插拔实时增删、别处（非本窗口）变更跟随且不弹 Toast；
  * daemon 掉线 → StatusPage“服务未运行”；
  * **i18n**：期望文案全部经 i18n._() 取出——同一套断言在 LANG=zh_CN.UTF-8
    下验中文、LANG=en_US.UTF-8 / C.UTF-8 下验英文（双语言各跑一遍即窗口级
    i18n 断言），并断言 zh 环境译文非英文源串 / 非 zh 环境回退源串；
  * **深色样式**：建窗前 Adw.StyleManager 设 FORCE_DARK，断言 get_dark()==True
    且窗口/关键控件正常；再切 FORCE_LIGHT 对称断言；结束恢复 DEFAULT
    （= 跟随系统，libadwaita 无名为 FORCE_FOLLOW 的枚举值，DEFAULT 即其义）；
  * **图标**：遍历窗口实际使用的每个图标名（Gtk.Image / 页面 / StatusPage /
    窗口图标），断言 has_icon 全为真；窗口 get_icon_name() 非空且可解析。

用法（推荐在 Xvfb 下跑，避免打扰真实桌面）：
  Xvfb :99 & DISPLAY=:99 GDK_BACKEND=x11 python3 uitest.py
无 Xvfb 时直接在任何可用显示（含 wayland）上运行亦可，窗口会短暂出现约 20 秒。
"""

import os
import socket
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gi  # noqa: E402

try:
    gi.require_version("Adw", "1.0")
except ValueError:
    gi.require_version("Adw", "1")
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Adw, Gtk, Gdk, Gio, GLib  # noqa: E402

import i18n  # noqa: E402  （gettext 初始化：顺序在 window 之前）
from i18n import _  # noqa: E402
import pages  # noqa: E402
import window as window_mod  # noqa: E402  （其内装载 schedmgr-client.py）

BUS_NAME = "io.kleinia.SchedMgr"
ROOT_PATH = "/io/kleinia/SchedMgr"
CPU_PATH = ROOT_PATH + "/Cpu"
IO_PATH = ROOT_PATH + "/Io"

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("ok:   %s" % name, flush=True)
    else:
        FAIL += 1
        suffix = " — %s" % (detail,) if detail else ""
        print("FAIL: %s%s" % (name, suffix), flush=True)


def dev_panel(win, dev):
    path = ROOT_PATH + "/Io/" + dev
    panel = win._device_groups.get(path)
    if panel is None:
        check("设备面板 %s 在 _device_groups 中（keys=%s, client.paths=%s）"
              % (dev, list(win._device_groups),
                 win.client.device_paths()), False)
    return panel


# ---------------------------------------------------------------------------
# 私有总线（socket 激活 dbus-broker-launch --scope user，等效 dbus-run-session；
# sd_listen_fds 约定：监听 fd 必须为 3，LISTEN_PID 经 bash `exec` 保持一致）
# ---------------------------------------------------------------------------
def start_private_bus(tmpdir):
    sock_path = os.path.join(tmpdir, "bus")
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    server.listen(64)
    if server.fileno() != 3:
        os.dup2(server.fileno(), 3)
    os.set_inheritable(3, True)
    env = dict(os.environ)
    env["LISTEN_FDS"] = "1"
    broker = subprocess.Popen(
        ["bash", "-c",
         'export LISTEN_PID=$$; exec dbus-broker-launch --scope user'],
        env=env, pass_fds=(3,), stderr=subprocess.DEVNULL)
    address = "unix:path=%s" % sock_path
    deadline = time.time() + 8
    while time.time() < deadline:
        if broker.poll() is not None:
            raise RuntimeError("dbus-broker-launch 提前退出 rc=%s" % broker.returncode)
        try:
            conn = Gio.DBusConnection.new_for_address_sync(
                address,
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
                | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
                None, None)
            return address, conn, server, broker
        except GLib.Error:
            time.sleep(0.05)
    raise RuntimeError("私有 dbus-broker 总线未能在 8s 内就绪")


def wait_name(conn, deadline_s=8.0):
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        try:
            res = conn.call_sync(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "NameHasOwner",
                GLib.Variant("(s)", (BUS_NAME,)), GLib.VariantType("(b)"),
                Gio.DBusCallFlags.NONE, -1, None)
            if res.unpack()[0]:
                return True
        except GLib.Error:
            pass
        time.sleep(0.05)
    return False


# ---------------------------------------------------------------------------
# 遍历辅助
# ---------------------------------------------------------------------------
def walk(widget):
    yield widget
    child = widget.get_first_child()
    while child is not None:
        yield from walk(child)
        child = child.get_next_sibling()


def group_titles(page):
    return [g.get_title() for g in walk(page)
            if isinstance(g, Adw.PreferencesGroup)]


def buttons_of(widget):
    return [b for b in walk(widget) if isinstance(b, Gtk.Button)]


def button_labels(widget):
    return [b.get_label() for b in buttons_of(widget)]


def combo_select(combo, name):
    model = combo.row.get_model()
    for i in range(model.get_n_items()):
        if model.get_item(i).get_string() == name:
            combo.row.set_selected(i)
            return True
    return False


def poke(conn, type_, arg):
    conn.call_sync(BUS_NAME, ROOT_PATH, "io.kleinia.Mock1", "PokeEvent",
                   GLib.Variant("(ss)", (type_, arg)), None,
                   Gio.DBusCallFlags.NONE, -1, None)


def dbus_apply(conn, path, strategy):
    conn.call_sync(BUS_NAME, path, "io.kleinia.Sched1", "ApplyStrategy",
                   GLib.Variant("(s)", (strategy,)), None,
                   Gio.DBusCallFlags.NONE, -1, None)


# ---------------------------------------------------------------------------
# 测试步骤（delay 毫秒后执行；断言同步完成）
# ---------------------------------------------------------------------------
STEPS = []


def step(delay):
    def deco(fn):
        STEPS.append((delay, fn))
        return fn
    return deco


def _run_next_step():
    if not STEPS:
        _finish()
        return GLib.SOURCE_REMOVE
    delay, fn = STEPS.pop(0)
    try:
        fn()
    except Exception:
        check("step %s 无异常" % fn.__name__, False, traceback.format_exc())
    GLib.timeout_add(delay, _run_next_step)
    return GLib.SOURCE_REMOVE


def _finish():
    print("RESULT: PASS=%d FAIL=%d" % (PASS, FAIL), flush=True)
    GLib.timeout_add(300, APP.quit)
    return GLib.SOURCE_REMOVE


def used_icon_names(win):
    """窗口实际使用的图标名集合（Gtk.Image / 页 / StatusPage / 窗口图标）。"""
    used = set()
    for widget in walk(win):
        if isinstance(widget, Gtk.Image):
            name = widget.get_icon_name()
            if name:
                used.add(name)
        elif isinstance(widget, (Adw.StatusPage, Adw.PreferencesPage)):
            name = widget.get_icon_name()
            if name:
                used.add(name)
    win_icon = win.get_icon_name()
    if win_icon:
        used.add(win_icon)
    return used


def build_steps():
    win = WIN

    # ---- 初始结构 ----
    @step(0)
    def structure():
        check("窗口标题", win.get_title() == _("Kleinia Sched Manager"),
              win.get_title())
        check("sim 常驻横幅存在且已显示",
              win._sim_banner is not None and win._sim_banner.get_revealed()
              and win._sim_banner.get_title()
              == _("Simulation Mode — all operations only affect demo "
                   "data; the real system is not modified"),
              win._sim_banner.get_title() if win._sim_banner else None)
        check("CPU 页组顺序", group_titles(win.cpu_page)
              == [_("Status"), _("Operations"), _("Service Info")],
              group_titles(win.cpu_page))
        st = win.cpu_panel.status
        check("CPU 初始状态行（无 · EEVDF（内核默认调度类）/已停止）",
              st.state_row.get_title() == _("None (EEVDF · kernel default)")
              and _("Stopped") in st.state_row.get_subtitle(),
              "%r / %r" % (st.state_row.get_title(), st.state_row.get_subtitle()))
        check("状态行前缀图标为停止图标（经 has_icon 兜底链解析）",
              st.state_icon.get_icon_name() == pages._icon(pages.ICON_STATE_STOP),
              st.state_icon.get_icon_name())
        check("默认策略行", st.default_row.get_title() == _("Default Policy")
              and st.default_row.get_subtitle() == _("None (EEVDF · kernel default)"),
              st.default_row.get_subtitle())
        ops = win.cpu_panel.ops
        check("操作组按钮顺序与文案",
              [ops.buttons[k].get_label()
               for k in ("apply", "restart", "stop", "restore")]
              == [_("Apply"), _("Restart"), _("Stop"), _("Restore Defaults")])
        check("按钮样式（应用 suggested / 停止 destructive / 其余 flat）",
              ops.apply_button.has_css_class("suggested-action")
              and ops.stop_button.has_css_class("destructive-action")
              and ops.restart_button.has_css_class("flat")
              and ops.restore_button.has_css_class("flat"))
        check("按钮在状态组之下（操作组可见且位于状态组之后）",
              ops.group.get_visible()
              and group_titles(win.cpu_page).index(_("Operations"))
              > group_titles(win.cpu_page).index(_("Status")))
        check("初始灵敏度：应用/恢复默认可用，重启/停止不可用（未运行）",
              ops.apply_button.get_sensitive()
              and ops.restore_button.get_sensitive()
              and not ops.restart_button.get_sensitive()
              and not ops.stop_button.get_sensitive())
        check("高级参数折叠且含参数 EntryRow",
              ops.params_expander is not None
              and not ops.params_expander.get_expanded()
              and ops.params_entry is not None
              and ops.params_expander.get_subtitle()
              == _("Space-separated command line arguments, e.g. -v -b 10"),
              ops.params_expander.get_subtitle() if ops.params_expander else None)
        check("页缓存页组顺序", group_titles(win.cache_page)
              == [_("Status"), _("Operations"), _("Service Info")],
              group_titles(win.cache_page))
        cst = win.cache_panel.status
        check("页缓存初始状态行（无（内核默认））",
              cst.state_row.get_title() == _("None (kernel default)")
              and _("Stopped") in cst.state_row.get_subtitle())
        cache_names = [n for n, _ in win.client.strategies("cache")]
        check("页缓存策略表（v2：none + cax_*）",
              cache_names[:1] == ["none"] and "cax_mru" in cache_names
              and "cax_lhd" in cache_names and "cax_s3fifo" in cache_names,
              cache_names)
        check("页缓存组合框初始选中 none",
              win.cache_panel.ops.combo.selected_name() == "none",
              win.cache_panel.ops.combo.selected_name())
        check("磁盘 IO 页组顺序（全局组介于状态与批量之间，设备组在批量后）",
              group_titles(win.io_page)
              == [_("Status"), _("Global IO Policy"), _("Batch Operations"),
                  "sda", "nvme0n1", "nvme1n1", _("Service Info")],
              group_titles(win.io_page))
        check("IO 汇总行计数", win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 0),
              win.io_summary_row.get_subtitle())
        gp = win.io_global_panel
        check("全局 IO 组含 ComboRow + 应用/停止按钮及样式",
              gp.combo.row.get_title() == _("Switch Strategy")
              and button_labels(gp.group) == [_("Apply"), _("Stop")]
              and gp.apply_button.has_css_class("suggested-action")
              and gp.stop_button.has_css_class("destructive-action"),
              button_labels(gp.group))
        check("全局 IO 组初始副标题（当前全局策略 + 全局 eBPF 策略说明）",
              gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "none",
              gp.combo.row.get_subtitle())
        check("全局 IO 组初始灵敏度（应用可用 / 停止不可用）",
              gp.apply_button.get_sensitive()
              and not gp.stop_button.get_sensitive())
        check("批量按钮初始可用",
              win.btn_all_scx_io.get_sensitive()
              and win.btn_all_default.get_sensitive())
        sda = dev_panel(win, "sda")
        check("sda 组描述=型号+容量+当前策略（有效名）",
              "SATA SSD 512G" in sda.group.get_description()
              and (_("Current: %s") % "kyber") in sda.group.get_description(),
              sda.group.get_description())
        sda_items = dict(sda.combo._items)
        check("sda 策略列表含 iox 项且描述带「全局 eBPF 策略」标记",
              "iox_fifo" in sda_items and "iox_wedf" in sda_items
              and "全局 eBPF 策略" in sda_items.get("iox_fifo", ""),
              sda_items.get("iox_fifo"))
        check("sda 策略列表 native 与 iox 并存（含 scx_io 开关）",
              all(n in sda_items for n in
                  ("none", "mq-deadline", "kyber", "bfq", "scx_io")),
              sorted(sda_items))
        check("sda 设备行含内联「应用」按钮",
              button_labels(sda.group) == [_("Apply")],
              button_labels(sda.group))
        rows = win.service_group_sets[0]
        check("服务信息行（后端=sim / 接口版本=1）",
              rows[0].get_subtitle() == "sim" and rows[2].get_subtitle() == "1",
              [r.get_subtitle() for r in rows])
        check("在线（prefs 可见）", win._stack.get_visible_child_name() == "prefs")

    # ---- i18n：窗口级断言（同一套断言在 zh/en 两种 LANG 下各跑一遍）----
    @step(0)
    def i18n_language():
        lang = os.environ.get("LANG", "")
        print("i18n: LANG=%s title=%r button=%r active=%s"
              % (lang, win.get_title(), win.btn_all_scx_io.get_label(),
                 i18n.translation_active()), flush=True)
        check("i18n：标题/文案跟随当前 LANG=%s" % (lang or "(unset)"),
              win.get_title() == _("Kleinia Sched Manager")
              and win.btn_all_scx_io.get_label() == _("Switch All to scx_io"))
        eevdf = _("None (EEVDF · kernel default)")
        check("i18n：EEVDF 文案（v2 命名，无 CFS 字样）",
              "EEVDF" in eevdf and "CFS" not in eevdf
              and win.cpu_panel.status.state_row.get_title() == eevdf,
              eevdf)
        if lang.startswith("zh"):
            check("i18n：zh 环境译文生效（非英文源串）",
                  i18n.translation_active()
                  and _("Kleinia Sched Manager") != "Kleinia Sched Manager",
                  i18n._("Kleinia Sched Manager"))
        else:
            check("i18n：非 zh 环境回退英文源串",
                  _("Kleinia Sched Manager") == "Kleinia Sched Manager")

    # ---- 图标：所用每个图标名 has_icon 为真；窗口图标非空且可解析 ----
    @step(0)
    def icons_audit():
        display = Gdk.Display.get_default()
        theme = Gtk.IconTheme.get_for_display(display)
        used = used_icon_names(win)
        win_icon = win.get_icon_name()
        check("窗口图标已设置且非空", bool(win_icon), win_icon)
        if win_icon:
            check("窗口图标可解析（has_icon）", theme.has_icon(win_icon),
                  win_icon)
        bad = [n for n in sorted(used) if n and not theme.has_icon(n)]
        check("窗口实际使用的 %d 个图标名全部 has_icon=true" % len(used),
              not bad, bad)
        # 审计清单：每个用途主名或备用名至少一个可解析（缺名走备用，真实回退）
        bad_groups = [p for p, pairs in pages.icon_audit_groups()
                      if not any(ok for _, ok in pairs)]
        check("图标审计：每个用途至少一个名字可解析（缺失主名回退备用名）",
              not bad_groups, bad_groups)

    # ---- 深色/浅色用例：建窗前已设 FORCE_DARK（见 on_activate）----
    @step(200)
    def dark_mode_on():
        sm = Adw.StyleManager.get_default()
        check("深色：StyleManager.get_dark()==True（FORCE_DARK 下建窗）",
              sm.get_dark() is True)
        check("深色下窗口与关键控件正常构建",
              win.get_visible() and win.cpu_panel.ops.group.get_visible()
              and win.btn_all_scx_io.get_sensitive()
              and win._sim_banner.get_revealed())
        sm.set_color_scheme(Adw.ColorScheme.FORCE_LIGHT)

    @step(300)
    def light_mode_on():
        sm = Adw.StyleManager.get_default()
        check("浅色：get_dark()==False（对称断言）", sm.get_dark() is False)
        check("浅色下窗口与关键控件正常",
              win.get_visible() and win.cpu_panel.ops.group.get_visible())
        sm.set_color_scheme(Adw.ColorScheme.DEFAULT)  # DEFAULT=跟随系统
        check("恢复跟随系统（DEFAULT，即 FORCE_FOLLOW 语义）",
              sm.get_color_scheme() == Adw.ColorScheme.DEFAULT)

    # ---- CPU 域操作流 ----
    @step(500)
    def cpu_apply():
        ops = win.cpu_panel.ops
        combo_select(ops.combo, "scx_agent")
        ops.apply_button.emit("clicked")
        check("点击「应用」后立即进入 busy（按钮+组合框 insensitive）",
              not ops.apply_button.get_sensitive()
              and not ops.combo.row.get_sensitive())

    @step(500)
    def cpu_applied():
        ops = win.cpu_panel.ops
        st = win.cpu_panel.status
        check("成功 Toast",
              win.toast_log[-1] == _("Switched to %s") % "scx_agent",
              win.toast_log[-1:])
        check("状态行跟随（scx_agent / 运行中 / ok 图标经兜底链解析）",
              st.state_row.get_title() == "scx_agent"
              and _("Running") in st.state_row.get_subtitle()
              and st.state_icon.get_icon_name() == pages._icon(pages.ICON_STATE_OK),
              st.state_row.get_title())
        check("busy 解除且重启/停止转为可用",
              ops.apply_button.get_sensitive()
              and ops.restart_button.get_sensitive()
              and ops.stop_button.get_sensitive())
        check("组合框选中回到当前策略",
              ops.combo.selected_name() == "scx_agent")

    @step(100)
    def cpu_apply_params():
        ops = win.cpu_panel.ops
        ops.params_expander.set_expanded(True)
        ops.params_entry.set_text("-v -b 10")
        ops.apply_button.emit("clicked")

    @step(500)
    def cpu_params_applied():
        st = win.cpu_panel.status
        check("状态行附参数",
              (_("Params: %s") % "-v -b 10") in st.state_row.get_subtitle(),
              st.state_row.get_subtitle())
        check("参数 Toast",
              win.toast_log[-1] == _("Switched to %s") % "scx_agent")

    @step(100)
    def cpu_bad_params():
        ops = win.cpu_panel.ops
        ops.params_entry.set_text('"未闭合')
        ops.apply_button.emit("clicked")
        check("非法参数 → Banner+Toast 且不发起调用",
              win._banner.get_revealed()
              and win._banner.get_title()
              == _("Invalid parameters: %s") % "No closing quotation",
              win._banner.get_title())
        ops.params_entry.set_text("")

    # ---- 停止需确认：取消不生效 / 确认生效 ----
    @step(300)
    def stop_cancel():
        ops = win.cpu_panel.ops
        ops.stop_button.emit("clicked")
        dlg = win._last_dialog
        check("停止弹确认对话框", dlg is not None
              and dlg.get_heading() == _("Stop Policy"),
              dlg.get_heading() if dlg else None)
        dlg.emit("response", "cancel")

    @step(400)
    def stop_cancelled():
        st = win.cpu_panel.status
        check("取消后仍在运行", st.state_row.get_title() == "scx_agent"
              and _("Running") in st.state_row.get_subtitle())
        check("取消不产生成功 Toast",
              not any(t == _("Stopped") for t in win.toast_log))

    @step(100)
    def stop_confirm():
        win.cpu_panel.ops.stop_button.emit("clicked")
        dlg = win._last_dialog
        dlg.emit("response", "ok")

    @step(500)
    def stopped():
        st = win.cpu_panel.status
        ops = win.cpu_panel.ops
        check("停止成功 Toast", win.toast_log[-1] == _("Stopped"),
              win.toast_log[-1:])
        check("状态行回到无 · EEVDF（内核默认调度类）/已停止",
              st.state_row.get_title() == _("None (EEVDF · kernel default)")
              and _("Stopped") in st.state_row.get_subtitle())
        check("停止后重启/停止按钮转不可用",
              not ops.restart_button.get_sensitive()
              and not ops.stop_button.get_sensitive())

    @step(100)
    def restart_click():
        win.cpu_panel.ops.restart_button.emit("clicked")

    @step(500)
    def restarted():
        st = win.cpu_panel.status
        check("重启 Toast + 状态恢复运行",
              win.toast_log[-1] == _("Restarted the current policy")
              and st.state_row.get_title() == "scx_agent",
              (win.toast_log[-1:], st.state_row.get_title()))

    @step(100)
    def restore_click():
        win.cpu_panel.ops.restore_button.emit("clicked")

    @step(500)
    def restored():
        st = win.cpu_panel.status
        check("恢复默认 Toast + 回到默认（none）",
              win.toast_log[-1] == _("Restored the default policy")
              and st.state_row.get_title() == _("None (EEVDF · kernel default)"),
              (win.toast_log[-1:], st.state_row.get_title()))

    # ---- 页缓存域（v2 实装：cax_* 全链路）----
    @step(100)
    def cache_apply():
        ops = win.cache_panel.ops
        combo_select(ops.combo, "cax_mru")
        ops.apply_button.emit("clicked")

    @step(500)
    def cache_applied():
        st = win.cache_panel.status
        ops = win.cache_panel.ops
        check("页缓存切换 Toast 与状态行实时跟随",
              win.toast_log[-1] == _("Switched to %s") % "cax_mru"
              and st.state_row.get_title() == "cax_mru"
              and _("Running") in st.state_row.get_subtitle(),
              (win.toast_log[-1:], st.state_row.get_title()))
        check("页缓存停止/重启转为可用",
              ops.stop_button.get_sensitive()
              and ops.restart_button.get_sensitive())

    @step(100)
    def cache_none_apply():
        ops = win.cache_panel.ops
        combo_select(ops.combo, "none")
        ops.apply_button.emit("clicked")

    @step(500)
    def cache_none_applied():
        st = win.cache_panel.status
        check("页缓存选 none → 无（内核默认）/已停止",
              st.state_row.get_title() == _("None (kernel default)")
              and _("Stopped") in st.state_row.get_subtitle()
              and win.toast_log[-1]
              == _("Switched to %s") % _("None (kernel default)"),
              (st.state_row.get_title(), win.toast_log[-1:]))

    @step(100)
    def cache_restore_click():
        win.cache_panel.ops.restore_button.emit("clicked")

    @step(500)
    def cache_restored():
        st = win.cache_panel.status
        check("页缓存恢复默认（cax_mru）",
              win.toast_log[-1] == _("Restored the default policy")
              and st.state_row.get_title() == "cax_mru",
              (win.toast_log[-1:], st.state_row.get_title()))

    @step(100)
    def cache_stop():
        win.cache_panel.ops.stop_button.emit("clicked")
        dlg = win._last_dialog
        check("页缓存停止弹确认", dlg is not None
              and dlg.get_heading() == _("Stop Policy"))
        dlg.emit("response", "ok")

    @step(500)
    def cache_stopped():
        st = win.cache_panel.status
        ops = win.cache_panel.ops
        check("页缓存停止成功（Toast + 无（内核默认））",
              win.toast_log[-1] == _("Stopped")
              and st.state_row.get_title() == _("None (kernel default)"),
              (win.toast_log[-1:], st.state_row.get_title()))
        check("页缓存停止后重启/停止转不可用",
              not ops.restart_button.get_sensitive()
              and not ops.stop_button.get_sensitive())

    # ---- Available=false：操作组隐藏、状态组只剩警告行 ----
    @step(200)
    def avail_off():
        poke(BUS, "availability", "cpu off")
        print("DIAG poke done", flush=True)

    @step(500)
    def avail_off_state():
        st = win.cpu_panel.status
        props = win.client.domain_props("cpu")
        check("不可用时操作组整体隐藏（client available=%s status=%r）"
              % (props.get("available"), props.get("status_message")),
              not win.cpu_panel.ops.group.get_visible())
        check("状态组只显示警告行",
              st.alert_row.get_visible()
              and not st.state_row.get_visible()
              and not st.default_row.get_visible()
              and "模拟不可用" in st.alert_row.get_subtitle(),
              st.alert_row.get_subtitle())

    @step(100)
    def avail_on():
        poke(BUS, "availability", "cpu on")

    @step(500)
    def avail_on_state():
        st = win.cpu_panel.status
        check("恢复可用后操作组与状态行回归",
              win.cpu_panel.ops.group.get_visible()
              and st.state_row.get_visible()
              and not st.alert_row.get_visible())

    # ---- 全局 IO 组 Available=false → 只显示警告行（占位文案）----
    @step(100)
    def io_avail_off():
        poke(BUS, "availability", "io off")

    @step(500)
    def io_avail_off_state():
        gp = win.io_global_panel
        check("全局组不可用时只显示警告行（组合框与按钮隐藏）",
              gp.alert_row.get_visible()
              and not gp.combo.row.get_visible()
              and "模拟不可用" in gp.alert_row.get_subtitle()
              and not gp.apply_button.get_sensitive(),
              gp.alert_row.get_subtitle())

    @step(100)
    def io_avail_on():
        poke(BUS, "availability", "io on")

    @step(500)
    def io_avail_on_state():
        gp = win.io_global_panel
        check("全局组恢复可用后组合框回归且副标题实时",
              gp.combo.row.get_visible()
              and not gp.alert_row.get_visible()
              and gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "none",
              gp.combo.row.get_subtitle())

    # ---- 全局 IO 策略组（v2：iox 域；应用直接执行，停止需确认）----
    @step(100)
    def io_global_apply():
        gp = win.io_global_panel
        combo_select(gp.combo, "iox_wedf")
        gp.apply_button.emit("clicked")
        check("全局应用立即 busy（组合框+按钮 insensitive）",
              not gp.apply_button.get_sensitive()
              and not gp.combo.row.get_sensitive())

    @step(500)
    def io_global_applied():
        gp = win.io_global_panel
        check("全局切换 Toast 与副标题实时跟随",
              win.toast_log[-1] == _("Switched to %s") % "iox_wedf"
              and gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "iox_wedf",
              (win.toast_log[-1:], gp.combo.row.get_subtitle()))
        check("全局 busy 解除且停止转为可用",
              gp.apply_button.get_sensitive()
              and gp.stop_button.get_sensitive())
        check("仅切全局不触设备（汇总仍 0 使用 scx_io）",
              win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 0),
              win.io_summary_row.get_subtitle())

    @step(100)
    def io_global_stop_cancel():
        global TOAST_MARK
        TOAST_MARK = len(win.toast_log)
        gp = win.io_global_panel
        gp.stop_button.emit("clicked")
        dlg = win._last_dialog
        check("全局停止弹确认（加载器停止/elevator 不回退文案）",
              dlg is not None and dlg.get_heading() == _("Stop Policy"),
              dlg.get_heading() if dlg else None)
        dlg.emit("response", "cancel")

    @step(400)
    def io_global_stop_cancelled():
        gp = win.io_global_panel
        check("全局停止取消后仍运行且无 Toast",
              gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "iox_wedf"
              and len(win.toast_log) == TOAST_MARK,
              (gp.combo.row.get_subtitle(), win.toast_log[TOAST_MARK:]))

    @step(100)
    def io_global_stop_ok():
        win.io_global_panel.stop_button.emit("clicked")
        dlg = win._last_dialog
        dlg.emit("response", "ok")

    @step(500)
    def io_global_stopped():
        gp = win.io_global_panel
        check("全局停止成功（Toast + 副标题回 none）",
              win.toast_log[-1] == _("Stopped")
              and gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "none"
              and not gp.stop_button.get_sensitive(),
              (win.toast_log[-1:], gp.combo.row.get_subtitle()))

    # ---- 设备 iox 项（直接切换即“切 scx_io + 启动该 iox”；不确认）----
    @step(100)
    def device_iox_apply():
        sda = dev_panel(win, "sda")
        combo_select(sda.combo, "iox_fifo")
        sda.apply_button.emit("clicked")
        check("设备应用立即 busy", not sda.apply_button.get_sensitive())

    @step(500)
    def device_iox_applied():
        sda = dev_panel(win, "sda")
        gp = win.io_global_panel
        check("设备切 iox Toast",
              win.toast_log[-1] == _("%s switched to %s") % ("sda", "iox_fifo"),
              win.toast_log[-1:])
        check("设备组描述显示有效名 iox_fifo（无未运行提示）",
              (_("Current: %s") % "iox_fifo") in sda.group.get_description()
              and "未运行全局 iox" not in sda.group.get_description(),
              sda.group.get_description())
        check("全局加载器被设备切换自动拉起（副标题联动）",
              gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "iox_fifo",
              gp.combo.row.get_subtitle())
        check("IO 汇总行实时更新（运行 iox 计入 scx_io）",
              win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 1),
              win.io_summary_row.get_subtitle())
        check("设备应用后按钮恢复", sda.apply_button.get_sensitive())

    # ---- 联动刷新：多设备选不同 iox，后选生效、其余 scx_io 设备自动跟随 ----
    @step(100)
    def iox_follow_apply():
        global TOAST_MARK
        TOAST_MARK = len(win.toast_log)
        nvme = dev_panel(win, "nvme0n1")
        combo_select(nvme.combo, "iox_merge")
        nvme.apply_button.emit("clicked")

    @step(600)
    def iox_followed():
        sda = dev_panel(win, "sda")
        nvme = dev_panel(win, "nvme0n1")
        gp = win.io_global_panel
        check("后选 iox 生效：nvme0n1 Toast + 全局副标题跟随",
              win.toast_log[-1]
              == _("%s switched to %s") % ("nvme0n1", "iox_merge")
              and gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "iox_merge",
              (win.toast_log[-1:], gp.combo.row.get_subtitle()))
        check("sda 有效名经 PropertiesChanged 自动跟随 iox_merge（不弹 Toast）",
              (_("Current: %s") % "iox_merge") in sda.group.get_description()
              and len(win.toast_log) == TOAST_MARK + 1,
              (sda.group.get_description(), win.toast_log[TOAST_MARK:]))
        check("nvme0n1 自身有效名 = iox_merge",
              (_("Current: %s") % "iox_merge") in nvme.group.get_description(),
              nvme.group.get_description())
        check("汇总计数含两台 scx_io 设备",
              win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 2),
              win.io_summary_row.get_subtitle())

    # ---- 别处改全局 iox：副标题与 scx_io 设备跟随，且不弹成功 Toast ----
    @step(200)
    def external_io_change():
        global TOAST_MARK
        TOAST_MARK = len(win.toast_log)
        dbus_apply(BUS, IO_PATH, "iox_fifo")

    @step(700)
    def external_io_changed():
        sda = dev_panel(win, "sda")
        nvme = dev_panel(win, "nvme0n1")
        gp = win.io_global_panel
        check("别处改全局 iox：全局副标题跟随",
              gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "iox_fifo",
              gp.combo.row.get_subtitle())
        check("别处改全局 iox：两台 scx_io 设备有效名跟随",
              (_("Current: %s") % "iox_fifo") in sda.group.get_description()
              and (_("Current: %s") % "iox_fifo") in nvme.group.get_description(),
              (sda.group.get_description(), nvme.group.get_description()))
        check("别处变更不弹成功 Toast",
              len(win.toast_log) == TOAST_MARK, win.toast_log[TOAST_MARK:])

    # ---- 全局停止：scx_io 设备不回退，有效名=scx_io+提示（§8.2）----
    @step(100)
    def io_global_stop2():
        win.io_global_panel.stop_button.emit("clicked")
        dlg = win._last_dialog
        dlg.emit("response", "ok")

    @step(500)
    def io_global_stop2_state():
        sda = dev_panel(win, "sda")
        nvme = dev_panel(win, "nvme0n1")
        gp = win.io_global_panel
        check("全局停止后副标题回 none",
              gp.combo.row.get_subtitle()
              == _("Current: %s · global eBPF policy") % "none",
              gp.combo.row.get_subtitle())
        check("scx_io 设备不回退：有效名=scx_io + 未运行提示（StatusMessage 联动）",
              (_("Current: %s") % "scx_io") in sda.group.get_description()
              and "未运行全局 iox" in sda.group.get_description()
              and (_("Current: %s") % "scx_io") in nvme.group.get_description(),
              (sda.group.get_description(), nvme.group.get_description()))
        check("汇总计数不变（elevator 仍为 scx_io）",
              win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 2),
              win.io_summary_row.get_subtitle())

    # ---- 热插拔：设备组实时增删，服务信息组保持在页尾 ----
    @step(200)
    def hotplug_add():
        poke(BUS, "hotplug-add", "sdb")

    @step(600)
    def hotplug_added():
        check("热插拔后 4 个设备组且页尾仍为服务信息",
              group_titles(win.io_page)
              == [_("Status"), _("Global IO Policy"), _("Batch Operations"),
                  "sda", "nvme0n1", "nvme1n1", "sdb", _("Service Info")],
              group_titles(win.io_page))
        check("汇总行计数跟随", win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (4, 2),
              win.io_summary_row.get_subtitle())

    @step(100)
    def hotplug_remove():
        poke(BUS, "hotplug-remove", "sdb")

    @step(600)
    def hotplug_removed():
        check("拔出后设备组移除",
              group_titles(win.io_page)
              == [_("Status"), _("Global IO Policy"), _("Batch Operations"),
                  "sda", "nvme0n1", "nvme1n1", _("Service Info")],
              group_titles(win.io_page))
        check("汇总行计数回落", win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 2),
              win.io_summary_row.get_subtitle())

    # ---- 批量操作（需确认）----
    @step(200)
    def batch_scx_io():
        win.btn_all_scx_io.emit("clicked")
        dlg = win._last_dialog
        check("批量切换弹确认", dlg is not None
              and dlg.get_heading() == _("Switch All to scx_io"),
              dlg.get_heading() if dlg else None)
        dlg.emit("response", "ok")

    @step(800)
    def batch_scx_io_done():
        check("批量切换 Toast",
              win.toast_log[-1] == _("Switched all %d devices to scx_io") % 3,
              win.toast_log[-1:])
        check("全部设备使用 scx_io 且汇总正确",
              win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 3),
              win.io_summary_row.get_subtitle())
        check("批量按钮恢复可用",
              win.btn_all_scx_io.get_sensitive()
              and win.btn_all_default.get_sensitive())
        sda = dev_panel(win, "sda")
        check("设备描述全部为 scx_io",
              (_("Current: %s") % "scx_io") in sda.group.get_description())

    @step(100)
    def batch_restore():
        win.btn_all_default.emit("clicked")
        dlg = win._last_dialog
        check("批量恢复默认弹确认", dlg is not None
              and dlg.get_heading() == _("Restore All Defaults"))
        dlg.emit("response", "ok")

    @step(800)
    def batch_restore_done():
        check("批量恢复 Toast + 汇总归零",
              win.toast_log[-1]
              == _("Restored all %d devices to their defaults") % 3
              and win.io_summary_row.get_subtitle()
              == _("%d block devices · %d using scx_io") % (3, 0),
              (win.toast_log[-1:], win.io_summary_row.get_subtitle()))
        sda = dev_panel(win, "sda")
        check("设备恢复各自默认",
              (_("Current: %s") % "mq-deadline") in sda.group.get_description(),
              sda.group.get_description())

    # ---- 失败路径：fail-next 注入（Banner+Toast 带 reason，按钮恢复）----
    @step(200)
    def fail_next():
        poke(BUS, "fail-next", "scx_central")
        ops = win.cpu_panel.ops
        combo_select(ops.combo, "scx_central")
        ops.apply_button.emit("clicked")

    @step(600)
    def fail_next_state():
        st = win.cpu_panel.status
        ops = win.cpu_panel.ops
        failure = _("The policy manager daemon reported a failure.")
        check("失败 → Banner+Toast 带 reason",
              win._banner.get_revealed()
              and (failure in win._banner.get_title()
                   or failure in win.toast_log[-1]),
              (win._banner.get_title(), win.toast_log[-1:]))
        check("失败后状态不变（仍为默认 none）",
              st.state_row.get_title() == _("None (EEVDF · kernel default)"))
        check("失败后按钮恢复可用", ops.apply_button.get_sensitive())

    # ---- 别处变更跟随（信号驱动，不弹成功 Toast）----
    @step(200)
    def external_change():
        global TOAST_MARK
        TOAST_MARK = len(win.toast_log)
        dbus_apply(BUS, CPU_PATH, "scx_simple")

    @step(700)
    def external_changed():
        st = win.cpu_panel.status
        check("别处变更：状态行跟随刷新",
              st.state_row.get_title() == "scx_simple",
              st.state_row.get_title())
        check("别处变更不弹成功 Toast",
              len(win.toast_log) == TOAST_MARK, win.toast_log[TOAST_MARK:])
        check("别处变更后组合框跟随选中",
              win.cpu_panel.ops.combo.selected_name() == "scx_simple")

    # ---- daemon 掉线 ----
    @step(200)
    def kill_mock():
        MOCK.terminate()

    @step(1200)
    def offline():
        gp = win.io_global_panel
        check("掉线 → StatusPage 服务未运行",
              win._stack.get_visible_child_name() == "status"
              and win._status_page.get_title() == _("Service Not Running"),
              win._status_page.get_title() if win._status_page else None)
        rows = win.service_group_sets[0]
        check("掉线后服务信息行为 —", rows[0].get_subtitle() == "—")
        check("掉线后设备组清空（全局组保留、副标题回 —）",
              group_titles(win.io_page)
              == [_("Status"), _("Global IO Policy"), _("Batch Operations"),
                  _("Service Info")]
              and gp.combo.row.get_subtitle() == "—",
              (group_titles(win.io_page), gp.combo.row.get_subtitle()))
        check("掉线后批量按钮禁用",
              not win.btn_all_scx_io.get_sensitive()
              and not win.btn_all_default.get_sensitive())
        check("掉线后全局组按钮禁用",
              not gp.apply_button.get_sensitive()
              and not gp.stop_button.get_sensitive())


# ---------------------------------------------------------------------------
# 进程入口
# ---------------------------------------------------------------------------
APP = None
WIN = None
BUS = None
MOCK = None
BROKER = None
SERVER = None
TOAST_MARK = 0


def main():
    global APP, WIN, BUS, MOCK, BROKER, SERVER
    tmpdir = tempfile.mkdtemp(prefix="schedmgr-uitest-")
    address, BUS, SERVER, BROKER = start_private_bus(tmpdir)
    os.environ["KLEINIA_SCHEDMGR_ADDRESS"] = address  # 测试钩子：直连私有总线

    mock_log = open(os.path.join(tmpdir, "mock.log"), "w+")
    MOCK = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "mock-schedmgrd.py")],
        env=dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address),
        stdout=subprocess.DEVNULL, stderr=mock_log)
    if not wait_name(BUS):
        print("FAIL: mock-schedmgrd 未在私有总线上就绪", flush=True)
        print("RESULT: PASS=0 FAIL=1", flush=True)
        return 1

    APP = Adw.Application(application_id="io.kleinia.SchedMgr.UITest",
                          flags=Gio.ApplicationFlags.NON_UNIQUE)

    def on_activate(app):
        global WIN
        # 深色用例：创建窗口前强制深色（对称的浅色断言与恢复见 dark/light 步骤）
        Adw.StyleManager.get_default().set_color_scheme(
            Adw.ColorScheme.FORCE_DARK)
        WIN = window_mod.SchedMgrWindow(APP, Gio.BusType.SESSION)
        app.add_window(WIN)
        WIN.present()
        build_steps()
        GLib.timeout_add(700, _run_next_step)

    APP.connect("activate", on_activate)
    try:
        APP.run([])
    finally:
        for proc in (MOCK, BROKER):
            if proc is not None and proc.poll() is None:
                proc.kill()
        try:
            mock_log.flush()
            mock_log.seek(0)
            print("--- mock-schedmgrd 日志尾部 ---", flush=True)
            print(mock_log.read()[-1200:], flush=True)
        except Exception:
            pass
        try:
            BUS.close()
        except Exception:
            pass
        SERVER.close()
        mock_log.close()
    rc = 0 if FAIL == 0 else 1
    print("uitest 退出码 rc=%d" % rc, flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
