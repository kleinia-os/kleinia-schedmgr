#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""window.py — io.kleinia.SchedMgr 主窗口（Adw.PreferencesWindow，2026-10 改版）。

要点：
  * 三页：CPU 调度器 / 页缓存 / 磁盘 IO，页面用 PreferencesWindow.add()
    —— add() 是 PreferencesWindow 在 Adw 1.4~1.10 全程唯一且未删除的加页 API
    （add_paged 是 1.5+ AdwPreferencesDialog 的 API，PreferencesWindow 并无此符号）。
  * 顶部两条横幅：常驻 sim 模式横幅（Backend=="sim"，不可关闭）+ 错误横幅
    （可关闭）；PreferencesWindow 无 banner 槽位，这里用
    AdwWindow.get_content()/set_content() 把内部内容保守地移入
    Gtk.Box{sim_banner, error_banner, Stack{原内容, StatusPage}}；失败则回退仅 Toast。
  * daemon 不在总线：Stack 切到 AdwStatusPage “服务未运行”。
  * 所有刷新由 SchedMgrClient 的 GObject 信号驱动（禁止轮询），含别处变更跟随。
  * 操作成功 → AdwToast（客户端新增 "action-done" 信号，向后兼容）；
    失败 → Banner + Toast（polkit 拒绝文案在客户端做友好映射）。
  * busy 防连点：方法调用期间相关面板按钮（与组合框）insensitive，
    完成/出错即恢复。

2026-10-28 i18n + 深色 + 图标改造：
  * 文案英文源串 + gettext（i18n._）；zh_CN 翻译在 locale/ 下编译为 .mo。
  * 深色模式：不调用 StyleManager.set_color_scheme（保持默认跟随系统
    color-scheme）；sim 横幅原先的自定义黄色 CSS（CssProvider 提供底色）已
    移除——Adw.Banner 没有官方 warning/error 样式类，任何自绘颜色都无法
    保证随深浅色正确，改用默认横幅样式（全部配色交给 libadwaita）。
  * 窗口图标：set_icon_name 前经 pages.window_icon_name() 做 has_icon 校验，
    缺失则不设置并 log（根治顶部红色 image-missing 占位）；同时把
    <app>/icons 加入图标主题搜索路径（源码运行的零安装兜底）。
"""

import os
import shlex
import importlib.util
import sys

import gi

try:
    gi.require_version("Adw", "1.0")
except ValueError:  # 新 typelib 工具链（如本机 libadwaita 1.10）版本号注册为 "1"
    gi.require_version("Adw", "1")
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Adw  # noqa: E402

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from i18n import _  # noqa: E402

import pages  # noqa: E402


def _load_schedmgr_client():
    """按规格文件名为 schedmgr-client.py（连字符无法直接 import），用 importlib 装载。"""
    if "schedmgr_client" in sys.modules:
        return sys.modules["schedmgr_client"]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "schedmgr-client.py")
    spec = importlib.util.spec_from_file_location("schedmgr_client", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    sys.modules["schedmgr_client"] = module
    return module


_schedmgr_client = _load_schedmgr_client()
SchedMgrClient = _schedmgr_client.SchedMgrClient
BUS_NAME = _schedmgr_client.BUS_NAME

# 域的人读名称（确认对话框 / Toast 文案）
DOMAIN_LABELS = {"cpu": _("CPU Scheduler"),
                 "cache": _("Page Cache Policy"),
                 "io": _("Global IO Policy")}

STOP_CONFIRM_BODY = {
    # v2：内核默认调度类为 EEVDF（Linux 6.6 起，无 CFS）
    "cpu": _("This will stop the running sched_ext policy on the CPU "
             "scheduler and fall back to EEVDF, the kernel's default "
             "scheduling class. Stop it?"),
    "cache": _("This will stop the page cache policy and return to the "
               "kernel's default behavior. Stop it?"),
    # v2：停全局 iox 加载器；设备 scx_io elevator 不自动回退
    "io": _("This will stop the global eBPF IO policy loader. Devices "
            "already switched to the scx_io elevator stay on it. Stop it?"),
}

SIM_BANNER_TEXT = _("Simulation Mode — all operations only affect demo "
                    "data; the real system is not modified")


def _kind_of(target):
    return target if target in ("cpu", "cache", "io") else "device"


class SchedMgrWindow(Adw.PreferencesWindow):
    def __init__(self, app, bus_type):
        super().__init__(application=app)
        self._app = app
        self._bus_type = bus_type
        self.set_title(_("Kleinia Sched Manager"))
        self.set_default_size(720, 640)

        # ---- 窗口图标：先把 <app>/icons 加入图标主题搜索路径（零安装兜底），
        #      再 has_icon 校验——缺失时不设置（避免红色 image-missing 占位）。
        icon_name = pages.window_icon_name()
        if icon_name is not None:
            self.set_icon_name(icon_name)

        self._device_groups = {}   # object path -> pages.DevicePanel
        self._banner = None        # 错误横幅（可关闭）
        self._sim_banner = None    # sim 常驻横幅
        self._stack = None
        self._status_page = None
        self._pending = {}         # target -> 未完成调用计数（busy 防连点）
        self._batch = None         # 批量操作进度 {total, done, failed, msg}
        self.toast_log = []        # 测试钩子：弹过的 Toast 文案
        self._last_dialog = None   # 测试钩子：最近一个确认对话框

        # ---- 客户端（先建，UI 信号稍后连接）----
        self.client = SchedMgrClient(bus_type)

        # ---- 页面（状态组在首位，见 pages.py）----
        self.add(pages.make_cpu_page(self, self.client))
        self.add(pages.make_cache_page(self, self.client))
        self.add(pages.make_io_page(self, self.client))

        # ---- 顶部横幅 / 离线 StatusPage 的保守挂载 ----
        self._try_install_overlay()

        # ---- 客户端信号 -> UI ----
        client = self.client
        client.connect("ready", self._on_ready)
        client.connect("vanished", self._on_vanished)
        client.connect("manager-changed", self._update_service_rows)
        client.connect("domain-changed", self._on_domain_changed)
        client.connect("device-changed", self._on_device_changed)
        client.connect("devices-changed", self._sync_device_groups)
        client.connect("strategy-failed", self._on_strategy_failed)
        client.connect("action-error", self._on_action_error)
        client.connect("action-done", self._on_action_done)

        self._set_offline()

    # ------------------------------------------------------------------
    # 覆盖层：sim 常驻横幅 + 错误横幅 + daemon 不在时的 StatusPage
    # ------------------------------------------------------------------
    def _try_install_overlay(self):
        """把 PreferencesWindow 的内部内容包进 Box{sim_banner, err_banner, Stack}。

        仅使用 AdwWindow.get_content/set_content（1.0 起）+ 普通容器 API；
        任何异常都回退到“无横幅/无状态页，仅 Toast + 禁用”模式。
        横幅一律用默认 Adw.Banner 样式（无自定义颜色，深浅色自动正确）。
        """
        try:
            content = self.get_content()
            if content is None:
                raise RuntimeError("PreferencesWindow has no content widget")

            sim_banner = Adw.Banner()
            sim_banner.set_title(SIM_BANNER_TEXT)
            sim_banner.set_revealed(False)

            banner = Adw.Banner()
            banner.set_button_label(_("Dismiss"))
            banner.connect("button-clicked", self._on_banner_dismissed)
            banner.set_revealed(False)

            status_page = Adw.StatusPage(
                icon_name=pages._icon(*pages.ICON_SERVICE_DOWN),
                title=_("Service Not Running"),
                description=(
                    _("%s is not on the %s bus.\n\n"
                      "Start kleinia-schedmgrd.service (system bus), or run "
                      "mock-schedmgrd.py on the session bus for testing.")
                    % (BUS_NAME, self.client.bus_label)))

            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            box.append(sim_banner)
            box.append(banner)
            # 先换掉窗口内容（content 解挂，引用仍在本函数局部变量）
            self.set_content(box)
            # 再把解挂后的原内容与状态页装入 Stack（顺序保证 content 无父）
            stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE)
            stack.add_named(content, "prefs")
            stack.add_named(status_page, "status")
            stack.set_visible_child_name("prefs")
            box.append(stack)

            self._banner = banner
            self._sim_banner = sim_banner
            self._stack = stack
            self._status_page = status_page
        except Exception:
            self._banner = None
            self._sim_banner = None
            self._stack = None
            self._status_page = None

    def _on_banner_dismissed(self, banner):
        banner.set_revealed(False)

    # ------------------------------------------------------------------
    # Toast / Banner / 确认对话框
    # ------------------------------------------------------------------
    def _toast(self, text):
        self.toast_log.append(text)  # 测试钩子
        toast = Adw.Toast(title=text)
        self.add_toast(toast)

    def _show_error(self, text):
        """DBus 错误 -> 顶部错误 AdwBanner + AdwToast（带 daemon 的 reason）。"""
        self._toast(text)
        if self._banner is not None:
            self._banner.set_title(text)
            self._banner.set_revealed(True)

    def _confirm(self, heading, body, accept_label, appearance, callback):
        """变更确认对话框（Adw.MessageDialog，Adw 1.2 起可用，保守 API）。"""
        dialog = Adw.MessageDialog(transient_for=self, heading=heading, body=body)
        dialog.add_response("cancel", _("Cancel"))
        dialog.add_response("ok", accept_label)
        if appearance is not None:
            dialog.set_response_appearance("ok", appearance)
        dialog.set_default_response("cancel")
        dialog.set_close_response("cancel")

        def on_response(dlg, response):
            self._last_dialog = None
            dlg.destroy()
            if response == "ok":
                callback()

        dialog.connect("response", on_response)
        self._last_dialog = dialog
        dialog.present()

    # ------------------------------------------------------------------
    # busy 防连点：调用期间按钮（与组合框）insensitive
    # ------------------------------------------------------------------
    def _busy_add(self, target):
        self._pending[target] = self._pending.get(target, 0) + 1
        self._apply_busy(target)

    def _busy_release_target(self, target):
        count = self._pending.get(target, 0) - 1
        if count > 0:
            self._pending[target] = count
        else:
            self._pending.pop(target, None)
        self._apply_busy(target)

    @staticmethod
    def _target_of_context(context):
        """context 形如 apply:<target>:<name> / stop:<target> / devapply:<path>:<name>
        / batch-io:<path>；返回 busyness 归属的 target（域键或对象路径）。"""
        if not context:
            return None
        parts = context.split(":")
        op = parts[0]
        if op in ("apply", "stop", "restart", "restore",
                  "devapply", "batch-io") and len(parts) >= 2:
            return parts[1]
        return None

    def _apply_busy(self, target):
        if target is None:
            return
        busy = self._pending.get(target, 0) > 0
        if target in ("cpu", "cache"):
            panel = {"cpu": getattr(self, "cpu_panel", None),
                     "cache": getattr(self, "cache_panel", None)}[target]
            if panel is not None:
                panel.set_busy(busy)
        elif target == "io":
            panel = getattr(self, "io_global_panel", None)
            if panel is not None:
                panel.set_busy(busy)
        elif target == "batch":
            self._update_batch_sensitivity()
        else:  # 设备对象路径
            panel = self._device_groups.get(target)
            if panel is not None:
                panel.set_busy(busy)

    # ------------------------------------------------------------------
    # 面板动作（应用 / 重启 / 停止 / 恢复默认 / 设备应用 / 批量）
    # 应用、重启、恢复默认 = 直接执行 + Toast；仅停止需确认；批量都确认。
    # ------------------------------------------------------------------
    def panel_apply(self, panel):
        name = panel.ops.combo.selected_name()
        if name is None:
            return
        params = None
        if panel.domain == "cpu":
            text = panel.params_text()
            if text:
                try:
                    params = shlex.split(text)
                except ValueError as err:
                    self._show_error(_("Invalid parameters: %s") % err)
                    return
        target = panel.domain
        self._busy_add(target)
        self.client.apply(target, name, params,
                          context="apply:%s:%s" % (target, name))

    def panel_stop(self, panel):
        domain = panel.domain
        self._confirm(
            _("Stop Policy"),
            STOP_CONFIRM_BODY.get(
                domain, _("This will stop the policy running on %s and "
                          "return to the default state. Stop it?")
                % DOMAIN_LABELS.get(domain, domain)),
            _("Stop"), Adw.ResponseAppearance.DESTRUCTIVE,
            lambda: (self._busy_add(domain),
                     self.client.stop(domain, context="stop:%s" % domain)))

    def panel_restart(self, panel):
        domain = panel.domain
        self._busy_add(domain)
        self.client.restart(domain, context="restart:%s" % domain)

    def panel_restore(self, panel):
        domain = panel.domain
        self._busy_add(domain)
        self.client.restore_default(domain, context="restore:%s" % domain)

    def device_apply(self, panel):
        name = panel.selected_name()
        if name is None:
            return
        self._busy_add(panel.path)
        self.client.apply(panel.path, name, None,
                          context="devapply:%s:%s" % (panel.path, name))

    # ------------------------------------------------------------------
    # 全局 IO 策略（v2：/Io = iox 域）。应用直接执行；停止需确认（加载器停
    # 止后设备的 scx_io elevator 不自动回退，见 STOP_CONFIRM_BODY["io"]）。
    # ------------------------------------------------------------------
    def io_apply(self, panel):
        name = panel.combo.selected_name()
        if name is None:
            return
        self._busy_add("io")
        self.client.apply("io", name, None,
                          context="apply:io:%s" % name)

    def io_stop(self, panel):
        self._confirm(
            _("Stop Policy"),
            STOP_CONFIRM_BODY["io"],
            _("Stop"), Adw.ResponseAppearance.DESTRUCTIVE,
            lambda: (self._busy_add("io"),
                     self.client.stop("io", context="stop:io")))

    def batch_all_scx_io(self):
        paths = self.client.device_paths()
        if not paths:
            return
        n = len(paths)

        def run():
            self._run_batch(
                paths,
                lambda p: self.client.apply(p, "scx_io", None,
                                            context="batch-io:%s" % p),
                _("Switched all %d devices to scx_io") % n)

        self._confirm(
            _("Switch All to scx_io"),
            _("All %d block devices will be switched to the scx_io IO "
              "scheduler; brief IO hiccups may occur during the switch. "
              "Continue?") % n,
            _("Switch All"), Adw.ResponseAppearance.SUGGESTED, run)

    def batch_all_default(self):
        paths = self.client.device_paths()
        if not paths:
            return
        n = len(paths)

        def run():
            self._run_batch(
                paths,
                lambda p: self.client.restore_default(
                    p, context="batch-io:%s" % p),
                _("Restored all %d devices to their defaults") % n)

        self._confirm(
            _("Restore All Defaults"),
            _("All %d block devices will be restored to their respective "
              "default IO schedulers. Continue?") % n,
            _("Restore Defaults"), None, run)

    def _run_batch(self, paths, call_one, success_msg):
        self._batch = {"total": len(paths), "done": 0, "failed": 0,
                       "msg": success_msg}
        self._busy_add("batch")
        for path in paths:
            self._busy_add(path)
            call_one(path)

    def _batch_progress(self, failed):
        batch = self._batch
        if batch is None:
            return
        batch["done"] += 1
        if failed:
            batch["failed"] += 1
        if batch["done"] < batch["total"]:
            return
        if batch["failed"] == 0:
            self._toast(batch["msg"])
        else:
            self._toast(_("Batch operation partially failed (%d/%d devices)")
                        % (batch["failed"], batch["total"]))
        self._batch = None
        self._busy_release_target("batch")

    # ------------------------------------------------------------------
    # 操作成功 Toast（context -> 文案）
    # ------------------------------------------------------------------
    def _toast_for_context(self, context):
        if not context:
            return None
        parts = context.split(":")
        op = parts[0]
        target = parts[1] if len(parts) > 1 else ""
        if op == "apply" and len(parts) >= 3:
            return _("Switched to %s") % pages.strategy_display(
                _kind_of(target), parts[2])
        if op == "devapply" and len(parts) >= 3:
            return _("%s switched to %s") % (os.path.basename(target), parts[2])
        if op == "stop":
            return _("Stopped")
        if op == "restart":
            return _("Restarted the current policy")
        if op == "restore":
            return _("Restored the default policy")
        return None

    # ------------------------------------------------------------------
    # 客户端信号处理
    # ------------------------------------------------------------------
    def _on_ready(self, client):
        if self._stack is not None:
            self._stack.set_visible_child_name("prefs")
        self._update_service_rows(client)
        for domain, panel in (("cpu", self.cpu_panel),
                              ("cache", self.cache_panel),
                              ("io", self.io_global_panel)):
            panel.update(client.domain_props(domain),
                         client.strategies(domain))
        self._sync_device_groups(client)

    def _on_vanished(self, client):
        self._set_offline()

    def _set_offline(self):
        self._pending.clear()
        self._batch = None
        if self._stack is not None:
            self._stack.set_visible_child_name("status")
        if self._sim_banner is not None:
            self._sim_banner.set_revealed(False)
        if self._banner is not None:
            self._banner.set_revealed(False)
        for rows in getattr(self, "service_group_sets", []):
            for row in rows:
                row.set_subtitle("—")
        for panel in (self.cpu_panel, self.cache_panel, self.io_global_panel):
            panel.update(None, None)
        self.io_summary_row.set_subtitle("—")
        self.btn_all_scx_io.set_sensitive(False)
        self.btn_all_default.set_sensitive(False)
        self._clear_device_groups()

    def _update_service_rows(self, client):
        props = client.manager_props()
        if props is None:
            return
        for backend_row, kernel_row, version_row in \
                getattr(self, "service_group_sets", []):
            backend_row.set_subtitle("%s" % props["backend"])
            kernel_row.set_subtitle("%s" % props["kernel_release"])
            version_row.set_subtitle("%d" % props["version"])
        # sim 后端：窗口顶部常驻警告横幅（不可关闭）
        if self._sim_banner is not None:
            self._sim_banner.set_revealed(props["backend"] == "sim")

    def _on_domain_changed(self, client, domain):
        if domain in ("cpu", "cache"):
            panel = {"cpu": self.cpu_panel, "cache": self.cache_panel}[domain]
            panel.update(client.domain_props(domain), client.strategies(domain))
        elif domain == "io":
            # 全局 iox 域变化（含设备侧 ApplyStrategy(iox_X) 自动拉起的加载器）
            self.io_global_panel.update(client.domain_props("io"),
                                        client.strategies("io"))
            self._update_io_page()

    def _on_device_changed(self, client, path):
        panel = self._device_groups.get(path)
        if panel is not None:
            panel.update(client.device_props(path), client.strategies(path))
        self._update_io_page()   # 汇总行跟随（含别处变更）

    def _on_strategy_failed(self, client, target, strategy, reason):
        self._show_error(_("Failed to apply policy “%s”: %s")
                         % (strategy, reason))
        self._refresh_target(target)

    def _on_action_error(self, client, context, message):
        if context and context.startswith("batch-io:"):
            self._busy_release_target(self._target_of_context(context))
            self._batch_progress(failed=True)
            self._show_error(message)
            return
        self._show_error(message)
        self._busy_release_target(self._target_of_context(context))
        self._refresh_all()

    def _on_action_done(self, client, context):
        if context and context.startswith("batch-io:"):
            self._busy_release_target(self._target_of_context(context))
            self._batch_progress(failed=False)
            return
        self._busy_release_target(self._target_of_context(context))
        message = self._toast_for_context(context)
        if message is not None:
            self._toast(message)

    def _refresh_target(self, target):
        if target in ("cpu", "cache"):
            self._on_domain_changed(self.client, target)
        elif target == "io":
            self._on_domain_changed(self.client, "io")
        else:
            self._on_device_changed(self.client, target)

    def _refresh_all(self):
        """方法调用失败后按缓存属性恢复所有面板选中态（事件驱动，非轮询）。"""
        client = self.client
        if not client.is_ready:
            return
        self._update_service_rows(client)
        for domain, panel in (("cpu", self.cpu_panel),
                              ("cache", self.cache_panel),
                              ("io", self.io_global_panel)):
            panel.update(client.domain_props(domain), client.strategies(domain))
        for path, panel in self._device_groups.items():
            panel.update(client.device_props(path), client.strategies(path))
        self._update_io_page()

    # ------------------------------------------------------------------
    # 磁盘 IO 页：汇总行 / 批量按钮灵敏度（全部信号驱动；当前全局策略显示
    # 在全局组的副标题里，见 GlobalIoPanel.update）
    # ------------------------------------------------------------------
    def _update_io_page(self):
        client = self.client
        paths = client.device_paths()
        if paths:
            # v2：设备 CurrentStrategy 是“有效名”——elevator≠
            # scx_io → elevator 名；=scx_io → 运行中的 iox 名（无加载器时为
            # "scx_io"）。故“使用 scx_io 的设备” = 有效名为 "scx_io" 或任一
            # 全局 iox 策略名（iox 名集合来自全局域 ListStrategies）。
            iox_names = {n for n, _ in client.strategies("io") if n != "none"}
            using = 0
            for path in paths:
                props = client.device_props(path)
                if props is None:
                    continue
                strategy = props.get("strategy") or ""
                if strategy == "scx_io" or strategy in iox_names:
                    using += 1
            self.io_summary_row.set_subtitle(
                _("%d block devices · %d using scx_io") % (len(paths), using))
        else:
            self.io_summary_row.set_subtitle(
                _("No block devices found") if client.is_ready else "—")
        self._update_batch_sensitivity()

    def _update_batch_sensitivity(self):
        client = self.client
        paths = client.device_paths()
        ready = client.is_ready and bool(paths)
        all_have_scx_io = ready
        for path in paths:
            if "scx_io" not in [n for n, _ in client.strategies(path)]:
                all_have_scx_io = False
                break
        busy = self._pending.get("batch", 0) > 0
        self.btn_all_scx_io.set_sensitive(all_have_scx_io and not busy)
        self.btn_all_default.set_sensitive(ready and not busy)

    # ------------------------------------------------------------------
    # 设备组动态增删（DevicesChanged 信号驱动；服务信息组始终保持在页尾）
    # ------------------------------------------------------------------
    def _clear_device_groups(self):
        for path in list(self._device_groups):
            panel = self._device_groups.pop(path)
            self.io_page.remove(panel.group)
        self._keep_service_group_last()

    def _keep_service_group_last(self):
        try:
            self.io_page.remove(self.io_service_group)
            self.io_page.add(self.io_service_group)
        except Exception:
            pass

    def _sync_device_groups(self, client):
        paths = client.device_paths()
        for path in list(self._device_groups):
            if path not in paths:
                panel = self._device_groups.pop(path)
                self.io_page.remove(panel.group)
        for path in paths:
            if path not in self._device_groups:
                panel = pages.DevicePanel(self, client, path)
                self._device_groups[path] = panel
                self.io_page.add(panel.group)
            panel = self._device_groups[path]
            panel.update(client.device_props(path), client.strategies(path))
        self._keep_service_group_last()
        self._update_io_page()

    # ------------------------------------------------------------------
    # 隐藏自检（--selftest N）：经客户端链路真实调用一次 ApplyStrategy
    # ------------------------------------------------------------------
    def selftest_apply(self):
        self.client.apply("cpu", "scx_simple", context="selftest-apply")
