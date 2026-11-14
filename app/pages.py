#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""pages.py — schedmgr 三页 UI（2026-10 改版：状态优先）。

信息架构（响应用户反馈“状态和启停应该放在比较上面的地方”）：
  * 域页面（CPU / 页缓存），自上而下：
      1. 状态组：大状态行（前缀状态图标 + 当前策略显示名 + 状态短语/参数）、
         “默认策略”行；Available=false 时整组只显示警告行。
      2. 操作组：切换策略 ComboRow（弹出项 = 策略名 + 描述两行）、
         [仅 CPU] 高级参数 ExpanderRow（折叠，内含参数 EntryRow）、
         按钮行：应用 / 重启 / 停止 / 恢复默认。
      3. 服务信息组（页底低调小字）：后端 / 内核版本 / 接口版本。
  * 磁盘 IO 页（v2），自上而下：状态组（设备汇总行）、全局 IO
    策略组（iox 策略选择 + 应用/停止，副标题实时显示当前全局策略与“全局 eBPF
    策略”说明）、批量操作组（全部切换到 scx_io / 全部恢复默认）、每设备一组
    （ComboRow 策略列表 = native elevator ∪ iox 项（描述带“全局 eBPF 策略”
    标记），组副标题显示有效名，随 DevicesChanged 增删）、服务信息组（页尾）。

仍然只用 Adw 1.4 时代稳定 API；所有刷新由客户端信号驱动（零轮询）。

2026-10-28 i18n + 图标改造：
  * 文案改为英文源串 + gettext（``i18n._``；zh_CN 翻译在 locale/ 下）。
    daemon 返回的 Description/StatusMessage 是数据面，不经 _()。
  * 图标三原则：
      1. 启动时 ``Gtk.IconTheme.add_search_path(<app>/icons)``（GTK4 API），
         使源码目录运行也能解析 io.kleinia.SchedMgr（零安装兜底）；
      2. 每个图标名都经 ``has_icon`` 校验 + 备用名清单（缺失回退
         preferences-system-symbolic 等，回退真实发生而非注释）；
      3. 窗口图标 ``io.kleinia.SchedMgr`` 仅在可解析时设置（避免 GTK 对缺失
         图标名渲染红色 image-missing 占位）。
"""

import os
import sys

import gi

try:
    gi.require_version("Adw", "1.0")
except ValueError:  # 新 typelib 工具链（如本机 libadwaita 1.10）版本号注册为 "1"
    gi.require_version("Adw", "1")
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gtk, Adw, Gdk  # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from i18n import _  # noqa: E402

# ---------------------------------------------------------------------------
# 图标（标准 symbolic 名；缺失时按候选清单回退，最终回退 preferences-system）
# ---------------------------------------------------------------------------
_ICON_FALLBACK = "preferences-system-symbolic"
_ICON_DIR = os.path.join(_HERE, "icons")
_icon_cache = {}
_search_path_installed = False


def _has_icon(name):
    if name not in _icon_cache:
        try:
            display = Gdk.Display.get_default()
            _icon_cache[name] = bool(
                display is not None
                and Gtk.IconTheme.get_for_display(display).has_icon(name))
        except Exception:
            _icon_cache[name] = True  # 无显示环境时按标准名存在处理
    return _icon_cache[name]


def _icon(*candidates):
    for name in candidates:
        if _has_icon(name):
            return name
    return _ICON_FALLBACK


def install_icon_search_path():
    """把 <app>/icons 加入图标主题搜索路径（GTK4：add_search_path）。

    源码目录运行时窗口图标 io.kleinia.SchedMgr 只在 app/icons/ 下，不在系统
    hicolor 里；不加此路径 GTK 会渲染红色 image-missing 占位。幂等；失败
    （无显示等）返回 False 且不影响启动。
    """
    global _search_path_installed
    if _search_path_installed:
        return True
    try:
        display = Gdk.Display.get_default()
        if display is None:
            return False
        if not os.path.isdir(_ICON_DIR):
            return False
        # GTK4 API；root 布局（svg 直接放在搜索路径根）与 hicolor/... 布局均可
        Gtk.IconTheme.get_for_display(display).add_search_path(_ICON_DIR)
        _search_path_installed = True
        return True
    except Exception:
        return False


def window_icon_name(app_id="io.kleinia.SchedMgr"):
    """窗口图标名：仅在图标主题能解析时返回 app_id，否则 None（不设置）。

    调用方：win.set_icon_name 前必须 has_icon 校验——缺失时 GTK4 对不存在的
    图标名渲染红色 “image-missing” 占位（用户报告的顶部红图标即此因）。
    """
    install_icon_search_path()
    try:
        if _has_icon(app_id):
            return app_id
    except Exception:
        pass
    print("schedmgr: window icon %r not found in icon theme; leaving unset "
          "(search path: %s)" % (app_id, _ICON_DIR), file=sys.stderr)
    return None


ICON_CPU = ("speedometer-symbolic", "computer-symbolic")
ICON_CACHE = ("drive-multidisk-symbolic", "drive-harddisk-symbolic")
ICON_IO = ("drive-harddisk-symbolic",)
ICON_STATE_OK = "emblem-ok-symbolic"
ICON_STATE_STOP = "process-stop-symbolic"
ICON_STATE_WARN = "dialog-warning-symbolic"
ICON_SELECT = ("object-select-symbolic", "emblem-ok-symbolic")
# 离线 StatusPage 图标（window.py 用）
ICON_SERVICE_DOWN = ("computer-fail-symbolic", "computer-symbolic")

# 图标审计清单：UI 用到的每一个图标名（主名 + 备用名），供
# uitest 断言（见 icon_audit()）。
_AUDIT_SOURCE = [
    ("window icon", ("io.kleinia.SchedMgr",)),
    ("CPU page", ICON_CPU),
    ("cache page", ICON_CACHE),
    ("IO page", ICON_IO),
    ("state ok", (ICON_STATE_OK,) + ICON_SELECT),
    ("state stopped", (ICON_STATE_STOP, "media-playback-stop-symbolic")),
    ("state warning", (ICON_STATE_WARN, "dialog-information-symbolic")),
    ("combo check mark", ICON_SELECT),
    ("status page (service down)", ICON_SERVICE_DOWN),
    ("final fallback", (_ICON_FALLBACK,)),
]


def icon_audit():
    """返回 [(用途, 名字, has_icon)]；在真实显示下逐名校验。

    实测（Ubuntu 26.10 开发机）：wayland 会话（Yaru 主题）下全部名字可用；
    无设置守护进程的显示（如 Xvfb，主题回落 Adwaita）缺 speedometer-symbolic
    与 emblem-ok-symbolic —— 这正是备用名清单存在的意义（见 icon_audit_groups）。
    """
    install_icon_search_path()
    rows = []
    for purpose, names in _AUDIT_SOURCE:
        for name in names:
            rows.append((purpose, name, _has_icon(name)))
    return rows


def icon_audit_groups():
    """返回 [(用途, [(名字, has_icon), ...])]；供测试断言“每个用途至少
    一个名字可解析”（缺失的主名走备用名，回退真实发生）。"""
    install_icon_search_path()
    return [(purpose, [(n, _has_icon(n)) for n in names])
            for purpose, names in _AUDIT_SOURCE]


def strategy_display(kind, name):
    """策略显示名。kind: "cpu"|"cache"|"io"|"device"；name 为 "" 表示未运行。

    v2：CPU 的 "none" = 内核默认调度类 EEVDF（自 Linux 6.6 起
    CFS 已不存在），全部界面统一 EEVDF 命名；全局 IO（iox 域）的 "none" =
    未运行全局 eBPF 策略；设备的 "none" 是真实的 native elevator 名，原样显示。
    """
    if name in ("", None):
        name = "none"
    if name == "none":
        if kind == "cpu":
            return _("None (EEVDF · kernel default)")
        if kind == "io":
            return _("None (no global eBPF policy)")
        if kind == "cache":
            return _("None (kernel default)")
    return name


# ---------------------------------------------------------------------------
# ComboRow：模型为 StringList（名称），弹出列表自定义 factory 渲染
# 「策略名(title) + 描述(subtitle)」两行项，关闭态经 expression 只显示名称。
# factory 创建失败时自动回退为默认单行项（保守）。
# ---------------------------------------------------------------------------
class StrategyCombo(object):
    def __init__(self, title, subtitle):
        self.row = Adw.ComboRow(title=title, subtitle=subtitle)
        self.row.set_model(Gtk.StringList())
        self.row.set_expression(
            Gtk.PropertyExpression.new(Gtk.StringObject, None, "string"))
        self._items = []          # [(name, description)]
        try:
            factory = Gtk.SignalListItemFactory()
            factory.connect("setup", self._on_item_setup)
            factory.connect("bind", self._on_item_bind)
            self.row.set_list_factory(factory)
        except Exception:
            pass                  # 回退：默认单行项

    # -- 弹出列表项：两行（title=策略名，subtitle=描述）+ 选中勾。
    #    注意：弹出列表是 GtkListView（GtkListItemWidget 包裹子控件），
    #    AdwActionRow 只能放进 GtkListBox，故用普通 Box+Label 组合。
    #
    def _on_item_setup(self, factory, item):
        check = Gtk.Image(icon_name=_icon(*ICON_SELECT))
        check.set_valign(Gtk.Align.CENTER)
        title = Gtk.Label(xalign=0.0, hexpand=True)
        subtitle = Gtk.Label(xalign=0.0, hexpand=True, wrap=True)
        subtitle.add_css_class("caption")
        subtitle.add_css_class("dim-label")
        text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        text.append(title)
        text.append(subtitle)
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8,
                      margin_top=6, margin_bottom=6,
                      margin_start=8, margin_end=8)
        row.append(check)
        row.append(text)
        item.set_child(row)

    def _on_item_bind(self, factory, item):
        try:
            row = item.get_child()
            check = row.get_first_child()
            text = check.get_next_sibling()
            title = text.get_first_child()
            subtitle = title.get_next_sibling()
            pos = item.get_position()
            if 0 <= pos < len(self._items):
                name, desc = self._items[pos]
                title.set_text(name)
                subtitle.set_text(desc)
                subtitle.set_visible(bool(desc))
            else:
                obj = item.get_item()
                title.set_text(obj.get_string() if obj is not None else "")
                subtitle.set_visible(False)
            check.set_visible(pos == self.row.get_selected())
        except Exception:
            pass

    # -- 模型与选中态（由窗口在客户端信号回调里驱动）--
    def rebuild(self, strategies):
        """strategies=[(name, desc)]；仅在名称序列变化时重建模型。"""
        items = [(str(n), str(d or "")) for n, d in strategies]
        if [n for n, _ in items] != [n for n, _ in self._items]:
            self._items = items
            self.row.set_model(Gtk.StringList(strings=[n for n, _ in items]))
        else:
            self._items = items

    def sync_selection(self, current):
        names = [n for n, _ in self._items]
        want = current if current in names else \
            ("none" if current == "" and "none" in names else None)
        if want is not None:
            self.row.set_selected(names.index(want))
        elif names:
            self.row.set_selected(0)

    def selected_name(self):
        index = self.row.get_selected()
        if 0 <= index < len(self._items):
            return self._items[index][0]
        return None

    def count(self):
        return len(self._items)

    def set_sensitive(self, sensitive):
        self.row.set_sensitive(sensitive)


# ---------------------------------------------------------------------------
# 域页面 · 状态组（第一组）
# ---------------------------------------------------------------------------
class StatusSection(object):
    def __init__(self, kind):
        self.kind = kind          # "cpu" | "cache"
        self.group = Adw.PreferencesGroup(title=_("Status"))

        # Available=false 时的警告行（此时本组只显示这一行）
        self.alert_row = Adw.ActionRow(title=_("Unavailable"))
        self.alert_row.add_prefix(Gtk.Image(icon_name=_icon(ICON_STATE_WARN)))
        self.alert_row.set_visible(False)

        # 首行大状态：前缀图标 + 当前策略显示名 + 状态短语（+参数）
        self.state_row = Adw.ActionRow(title="…", subtitle="…")
        self.state_icon = Gtk.Image(icon_name=_icon(ICON_STATE_WARN))
        self.state_row.add_prefix(self.state_icon)

        self.default_row = Adw.ActionRow(title=_("Default Policy"),
                                         subtitle="—")

        for row in (self.alert_row, self.state_row, self.default_row):
            self.group.add(row)

    def update(self, props):
        """props=None 表示离线（daemon 不在总线）。"""
        if props is None:
            self.alert_row.set_visible(False)
            self.state_row.set_visible(True)
            self.default_row.set_visible(True)
            self.state_icon.set_from_icon_name(_icon(ICON_STATE_WARN))
            self.state_row.set_title("—")
            self.state_row.set_subtitle(_("Service not connected"))
            self.default_row.set_subtitle("—")
            return
        if not props.get("available"):
            self.alert_row.set_visible(True)
            self.state_row.set_visible(False)
            self.default_row.set_visible(False)
            self.alert_row.set_subtitle(
                props.get("status_message") or _("Not supported on this system"))
            return
        self.alert_row.set_visible(False)
        self.state_row.set_visible(True)
        self.default_row.set_visible(True)
        current = props.get("strategy") or ""
        params = props.get("params") or []
        if current:
            self.state_icon.set_from_icon_name(_icon(ICON_STATE_OK))
            self.state_row.set_title(current)
            subtitle = _("Running")
        else:
            self.state_icon.set_from_icon_name(_icon(ICON_STATE_STOP))
            self.state_row.set_title(strategy_display(self.kind, ""))
            subtitle = _("Stopped")
        if params:
            subtitle += "\n" + _("Params: %s") % " ".join(params)
        self.state_row.set_subtitle(subtitle)
        default = props.get("default") or ""
        self.default_row.set_subtitle(
            strategy_display(self.kind, default) if default else "—")


# ---------------------------------------------------------------------------
# 域页面 · 操作组（第二组，Available=false 时整体隐藏）
# ---------------------------------------------------------------------------
class OpsSection(object):
    def __init__(self, win, panel, show_params):
        self.win = win
        self.panel = panel        # 所属 DomainPanel（按钮回调经窗口动作）
        self.group = Adw.PreferencesGroup(title=_("Operations"))

        self.combo = StrategyCombo(_("Switch Strategy"),
                                   _("Select a policy, then click Apply"))
        self.group.add(self.combo.row)

        # 高级参数（仅 CPU）：折叠 ExpanderRow + 参数 EntryRow + 提示副标题
        self.params_expander = None
        self.params_entry = None
        if show_params:
            self.params_expander = Adw.ExpanderRow(
                title=_("Advanced Parameters"),
                subtitle=_("Space-separated command line arguments, "
                           "e.g. -v -b 10"))
            self.params_expander.set_expanded(False)
            self.params_entry = Adw.EntryRow(title=_("Parameters"))
            self.params_expander.add_row(self.params_entry)
            self.group.add(self.params_expander)

        # 按钮行：应用（醒目）/ 重启（flat）/ 停止（destructive）/ 恢复默认（flat）
        self.apply_button = Gtk.Button(label=_("Apply"),
                                       valign=Gtk.Align.CENTER)
        self.apply_button.add_css_class("suggested-action")
        self.apply_button.connect("clicked", lambda *a: win.panel_apply(panel))
        self.restart_button = Gtk.Button(label=_("Restart"),
                                         valign=Gtk.Align.CENTER)
        self.restart_button.add_css_class("flat")
        self.restart_button.connect("clicked",
                                    lambda *a: win.panel_restart(panel))
        self.stop_button = Gtk.Button(label=_("Stop"),
                                      valign=Gtk.Align.CENTER)
        self.stop_button.add_css_class("destructive-action")
        self.stop_button.connect("clicked", lambda *a: win.panel_stop(panel))
        self.restore_button = Gtk.Button(label=_("Restore Defaults"),
                                         valign=Gtk.Align.CENTER)
        self.restore_button.add_css_class("flat")
        self.restore_button.connect("clicked",
                                    lambda *a: win.panel_restore(panel))
        self.buttons = {"apply": self.apply_button, "restart": self.restart_button,
                        "stop": self.stop_button, "restore": self.restore_button}

        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        for button in (self.apply_button, self.restart_button,
                       self.stop_button, self.restore_button):
            box.append(button)
        row = Adw.ActionRow(
            title=_("Run"),
            subtitle=_("Apply the selected policy, or restart / stop / "
                       "restore the default"))
        row.add_suffix(box)
        self.group.add(row)

        self._enabled = False
        self._busy = False
        self._running = False
        self._has_default = False

    def params_text(self):
        return self.params_entry.get_text().strip() \
            if self.params_entry is not None else ""

    def update(self, props, strategies):
        if props is not None:
            if strategies is not None:
                self.combo.rebuild(strategies)
            self.combo.sync_selection(props.get("strategy") or "")
            self._enabled = bool(props.get("available"))
            self._running = bool(props.get("strategy"))
            self._has_default = bool(props.get("default"))
        else:
            self._enabled = False
        self._apply_sensitivity()

    def set_busy(self, busy):
        self._busy = busy
        self._apply_sensitivity()

    def _apply_sensitivity(self):
        active = self._enabled and not self._busy
        has_names = self.combo.count() > 0
        self.combo.set_sensitive(active and has_names)
        self.apply_button.set_sensitive(active and has_names)
        self.restart_button.set_sensitive(active and self._running)
        self.stop_button.set_sensitive(active and self._running)
        self.restore_button.set_sensitive(active and self._has_default)


# ---------------------------------------------------------------------------
# 域面板 = 状态组 + 操作组
# ---------------------------------------------------------------------------
class DomainPanel(object):
    """一个策略域（Cpu / Cache）的状态组 + 操作组与刷新逻辑。"""

    def __init__(self, win, client, domain, show_params=False):
        self.win = win
        self.client = client
        self.domain = domain        # "cpu" | "cache"
        self.kind = domain
        self.target = domain
        self.status = StatusSection(domain)
        self.ops = OpsSection(win, self, show_params)

    @property
    def status_group(self):
        return self.status.group

    @property
    def ops_group(self):
        return self.ops.group

    def update(self, props, strategies):
        self.status.update(props)
        # Available=false（或离线）时整体隐藏操作组（规格第 1 条）
        show_ops = props is not None and bool(props.get("available"))
        self.ops.group.set_visible(show_ops)
        self.ops.update(props, strategies)

    def params_text(self):
        return self.ops.params_text()

    def set_busy(self, busy):
        self.ops.set_busy(busy)


# ---------------------------------------------------------------------------
# 设备面板：组标题=设备名，组描述=型号+容量+当前策略，内含紧凑一行
# （ComboRow + 内联「应用」小按钮）；设备切换直接执行，不确认。
# ---------------------------------------------------------------------------
class DevicePanel(object):
    def __init__(self, win, client, path):
        self.win = win
        self.client = client
        self.path = path
        self.kind = "device"
        self.target = path
        dev = os.path.basename(path)
        self.group = Adw.PreferencesGroup(title=dev, description="…")

        self.combo = StrategyCombo(_("Scheduler"), _("Default: —"))
        self.apply_button = Gtk.Button(label=_("Apply"),
                                       valign=Gtk.Align.CENTER)
        self.apply_button.add_css_class("suggested-action")
        self.apply_button.connect("clicked", lambda *a: win.device_apply(self))
        self.combo.row.add_suffix(self.apply_button)
        self.group.add(self.combo.row)

        self._enabled = False
        self._busy = False

    def update(self, props, strategies):
        if props is None:
            self.group.set_description("…")
            self._enabled = False
            self._apply_sensitivity()
            return
        current = props.get("strategy") or ""
        desc = props.get("description") or ""
        shown = current if current else _("None")
        # v2：CurrentStrategy 已是“有效名”（elevator≠scx_io →
        # elevator 名；=scx_io → 运行中的 iox 名，无加载器时为 "scx_io"），前端
        # 直接展示；StatusMessage（如“scx_io 生效但未运行 iox 策略”提示）追加
        # 在组描述尾，随设备对象 PropertiesChanged 实时联动。
        text = (_("%s · Current: %s") % (desc, shown) if desc
                else _("Current: %s") % shown)
        hint = props.get("status_message") or ""
        if hint:
            text += " · " + hint
        self.group.set_description(text)
        default = props.get("default") or ""
        self.combo.row.set_subtitle(
            _("Default: %s") % default if default
            else (props.get("status_message") or _("No default configured")))
        if strategies is not None:
            self.combo.rebuild(strategies)
        self.combo.sync_selection(current)
        self._enabled = bool(props.get("available")) and self.combo.count() > 0
        self._apply_sensitivity()

    def selected_name(self):
        return self.combo.selected_name()

    def set_busy(self, busy):
        self._busy = busy
        self._apply_sensitivity()

    def _apply_sensitivity(self):
        sensitive = self._enabled and not self._busy
        self.combo.set_sensitive(sensitive)
        self.apply_button.set_sensitive(sensitive)


# ---------------------------------------------------------------------------
# 全局 IO 面板（v2）：/Io = iox 策略域（全局 eBPF 策略）。
# 组内一行 ComboRow（策略选择）+ 内联「应用」「停止」；副标题实时显示当前
# 全局策略与“全局 eBPF 策略”说明；Available=false 时整组只显示警告行。
# ---------------------------------------------------------------------------
class GlobalIoPanel(object):
    def __init__(self, win, client):
        self.win = win
        self.client = client
        self.domain = "io"
        self.kind = "io"
        self.target = "io"
        self.group = Adw.PreferencesGroup(title=_("Global IO Policy"))

        # Available=false 时的警告行（此时本组只显示这一行）
        self.alert_row = Adw.ActionRow(title=_("Unavailable"))
        self.alert_row.add_prefix(Gtk.Image(icon_name=_icon(ICON_STATE_WARN)))
        self.alert_row.set_visible(False)
        self.group.add(self.alert_row)

        self.combo = StrategyCombo(
            _("Switch Strategy"),
            _("Current: %s · global eBPF policy") % "none")
        self.apply_button = Gtk.Button(label=_("Apply"),
                                       valign=Gtk.Align.CENTER)
        self.apply_button.add_css_class("suggested-action")
        self.apply_button.connect("clicked", lambda *a: win.io_apply(self))
        self.stop_button = Gtk.Button(label=_("Stop"),
                                      valign=Gtk.Align.CENTER)
        self.stop_button.add_css_class("destructive-action")
        self.stop_button.connect("clicked", lambda *a: win.io_stop(self))
        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        box.append(self.apply_button)
        box.append(self.stop_button)
        self.combo.row.add_suffix(box)
        self.group.add(self.combo.row)

        self._enabled = False
        self._busy = False
        self._running = False

    def update(self, props, strategies):
        """props=None 表示离线；Available=false 只显示警告行（占位文案）。"""
        if props is None:
            self.alert_row.set_visible(False)
            self.combo.row.set_visible(True)
            self.combo.row.set_subtitle("—")
            self._enabled = False
            self._running = False
        elif not props.get("available"):
            self.alert_row.set_visible(True)
            self.combo.row.set_visible(False)
            self.alert_row.set_subtitle(
                props.get("status_message")
                or _("Not supported on this system"))
            self._enabled = False
            self._running = False
        else:
            self.alert_row.set_visible(False)
            self.combo.row.set_visible(True)
            if strategies is not None:
                self.combo.rebuild(strategies)
            current = props.get("strategy") or ""
            self.combo.sync_selection(current)
            # 副标题：当前全局策略（实时）+ “全局 eBPF 策略”说明
            self.combo.row.set_subtitle(
                _("Current: %s · global eBPF policy") % (current or "none"))
            self._enabled = self.combo.count() > 0
            self._running = bool(current)
        self._apply_sensitivity()

    def set_busy(self, busy):
        self._busy = busy
        self._apply_sensitivity()

    def _apply_sensitivity(self):
        active = self._enabled and not self._busy
        has_names = self.combo.count() > 0
        self.combo.set_sensitive(active and has_names)
        self.apply_button.set_sensitive(active and has_names)
        self.stop_button.set_sensitive(active and self._running)


# ---------------------------------------------------------------------------
# 页面构建
# ---------------------------------------------------------------------------
def make_service_group(win):
    """页底“服务信息”组（低调小字）；每页一份，窗口统一刷新。"""
    group = Adw.PreferencesGroup(title=_("Service Info"))
    backend_row = Adw.ActionRow(title=_("Backend"), subtitle="—")
    kernel_row = Adw.ActionRow(title=_("Kernel Version"), subtitle="—")
    version_row = Adw.ActionRow(title=_("Interface Version"), subtitle="—")
    for row in (backend_row, kernel_row, version_row):
        group.add(row)
    if not hasattr(win, "service_group_sets"):
        win.service_group_sets = []
    win.service_group_sets.append((backend_row, kernel_row, version_row))
    return group


def make_cpu_page(win, client):
    page = Adw.PreferencesPage(title=_("CPU Scheduler"),
                               icon_name=_icon(*ICON_CPU))
    win.cpu_page = page
    win.cpu_panel = DomainPanel(win, client, "cpu", show_params=True)
    page.add(win.cpu_panel.status_group)   # 1. 状态（首位）
    page.add(win.cpu_panel.ops_group)      # 2. 操作
    page.add(make_service_group(win))      # 3. 服务信息（页底）
    return page


def make_cache_page(win, client):
    page = Adw.PreferencesPage(title=_("Page Cache"),
                               icon_name=_icon(*ICON_CACHE))
    win.cache_page = page
    win.cache_panel = DomainPanel(win, client, "cache", show_params=False)
    page.add(win.cache_panel.status_group)
    page.add(win.cache_panel.ops_group)
    page.add(make_service_group(win))
    return page


def make_io_page(win, client):
    page = Adw.PreferencesPage(title=_("Disk IO"),
                               icon_name=_icon(*ICON_IO))
    win.io_page = page

    # 1. 状态组：设备汇总行（当前全局策略显示在全局组副标题里）
    status = Adw.PreferencesGroup(title=_("Status"))
    win.io_summary_row = Adw.ActionRow(title=_("Block Devices"), subtitle="…")
    win.io_summary_row.add_prefix(Gtk.Image(icon_name=_icon(*ICON_IO)))
    status.add(win.io_summary_row)
    page.add(status)

    # 2. 全局 IO 策略组（v2：iox 域，选择+应用/停止；副标题=当前全局策略）
    win.io_global_panel = GlobalIoPanel(win, client)
    page.add(win.io_global_panel.group)

    # 3. 批量操作组（批量操作一律确认；窗口层实现）
    batch = Adw.PreferencesGroup(title=_("Batch Operations"))
    row = Adw.ActionRow(title=_("All Devices"),
                        subtitle=_("Apply one operation to all block devices"))
    win.btn_all_scx_io = Gtk.Button(label=_("Switch All to scx_io"),
                                    valign=Gtk.Align.CENTER)
    win.btn_all_scx_io.add_css_class("suggested-action")
    win.btn_all_scx_io.connect("clicked", lambda *a: win.batch_all_scx_io())
    win.btn_all_default = Gtk.Button(label=_("Restore All Defaults"),
                                     valign=Gtk.Align.CENTER)
    win.btn_all_default.add_css_class("flat")
    win.btn_all_default.connect("clicked", lambda *a: win.batch_all_default())
    box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
    box.append(win.btn_all_scx_io)
    box.append(win.btn_all_default)
    row.add_suffix(box)
    batch.add(row)
    page.add(batch)

    # 4. 设备组由窗口按 DevicesChanged 动态插入（始终保持在服务信息组之前）
    win.io_service_group = make_service_group(win)
    page.add(win.io_service_group)         # 5. 服务信息（页底，动态设备组会移到它前面）
    return page
