// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
// extension.js — SchedMgr GNOME Shell 扩展
//
// Quick Settings 面板入口：一个 SystemIndicator（面板象征图标常显）+
// 三个 QuickMenuToggle（CPU Sched / Page Cache / Disk I/O），全部状态由
// io.kleinia.SchedMgr DBus 信号驱动（禁止轮询）。

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {
    BUS_NAME, ROOT_PATH,
    ManagerProxy, PolicyProxy, IoListProxy,
    cpuPath, cachePath, ioPath,
} from './dbus.js';

const OFFLINE_TEXT = 'service offline';
const FAIL_FADE_SEC = 4; // StrategyFailed 副标题回落时长（一次性定时器，非轮询）

/** CurrentStrategy "" 的展示形式（"" 表示未运行/none） */
function currentLabel(cur) {
    return (!cur || cur === '') ? 'none' : cur;
}

/** 列表里的 "none" 菜单项文案：CPU 域特化为 "EEVDF · kernel default"（§8.1） */
function strategyItemLabel(name, noneLabel) {
    return name === 'none' ? noneLabel : name;
}

/**
 * gjs 的 *Remote 回调对单出参方法返回 [value] 包装（与独立测试端一致），
 * 这里统一展平为 a(ss) 的 [[name, description], ...] 行数组。
 */
function normalizeStrategies(result) {
    const rows = Array.isArray(result) && Array.isArray(result[0]) &&
        typeof result[0]?.[0] !== 'string'
        ? result[0] : result;
    return rows ?? [];
}

/** 异步 DBus 调用的回调里统一吞错，只记日志，绝不抛回 shell 主循环 */
function warn(context, error) {
    console.warn(`schedmgr: ${context}: ${error?.message ?? error}`);
}

/** 安全注销：connectSignal 与 connect 两类 id 分别配对注销 */
function disconnectAll(proxy, {dbusIds = [], propIds = []}) {
    for (const id of dbusIds) {
        try {
            proxy.disconnectSignal(id);
        } catch (e) {
            warn('disconnectSignal', e);
        }
    }
    for (const id of propIds) {
        try {
            proxy.disconnect(id);
        } catch (e) {
            warn('disconnect', e);
        }
    }
}

/**
 * SchedMgrService — 总线监听与代理生命周期管理。
 *
 * - 按 GSettings bus-type 用 bus_watch_name 监听 io.kleinia.SchedMgr；
 * - 名称出现时异步创建 5 个固定代理（Manager、Cpu、Cache、Io 的 Sched1
 *   与 Io1），全部 init 完成后回调 onReady(bundle)；名称消失（含 init 失败）
 *   回调 onOffline()；
 * - 运行中切换 bus-type 即整体重建（unwatch → onOffline → 重新 watch）；
 * - makeDeviceProxy() 供 Disk I/O 控制器按 Devices 属性增删设备代理。
 */
class SchedMgrService {
    constructor(settings, onReady, onOffline) {
        this._settings = settings;
        this._onReady = onReady;
        this._onOffline = onOffline;

        this._watchId = 0;
        this._connection = null;
        this._bundle = null;
        this._pendingInits = 0;
        this._onlineNotified = false;

        this._busTypeId = settings.connect('changed::bus-type', () => this.restart());
    }

    get connection() {
        return this._connection;
    }

    start() {
        this._watch();
    }

    stop() {
        this._settings.disconnect(this._busTypeId);
        this._teardown();
    }

    restart() {
        this._teardown();
        this._watch();
    }

    _busType() {
        return this._settings.get_string('bus-type') === 'session'
            ? Gio.BusType.SESSION
            : Gio.BusType.SYSTEM;
    }

    _watch() {
        this._watchId = Gio.bus_watch_name(
            this._busType(), BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            this._appeared.bind(this), this._vanished.bind(this));
    }

    _teardown() {
        if (this._watchId) {
            Gio.bus_unwatch_name(this._watchId);
            this._watchId = 0;
        }
        this._connection = null;
        this._bundle = null;
        if (this._onlineNotified) {
            this._onlineNotified = false;
            this._onOffline();
        }
    }

    _appeared(connection, name, _owner) {
        this._connection = connection;
        this._bundle = {};
        this._onlineNotified = true;

        const defs = [
            ['manager', ManagerProxy, ROOT_PATH],
            ['cpu', PolicyProxy, cpuPath()],
            ['cache', PolicyProxy, cachePath()],
            ['ioPolicy', PolicyProxy, ioPath()],
            ['ioList', IoListProxy, ioPath()],
        ];
        this._pendingInits = defs.length;

        for (const [key, ProxyClass, path] of defs) {
            try {
                // makeProxyWrapper 的类必须 new（GNOME 51 破坏点），init 全异步
                new ProxyClass(connection, name, path,
                    (proxy, error) => this._initDone(key, path, proxy, error));
            } catch (e) {
                warn(`construct proxy ${path}`, e);
                this._initDone(key, path, null, e);
            }
        }
    }

    _initDone(key, path, proxy, error) {
        this._pendingInits--;
        if (error) {
            warn(`proxy init ${path}`, error);
        } else {
            this._bundle[key] = proxy;
        }
        if (this._pendingInits === 0 && this._bundle !== null)
            this._onReady(this._bundle);
    }

    _vanished(_connection, _name) {
        this._teardown();
    }

    /** 为某个设备对象路径创建 Sched1 代理（Disk I/O 控制器用） */
    makeDeviceProxy(path, onInit) {
        if (!this._connection)
            return false;
        try {
            new PolicyProxy(this._connection, BUS_NAME, path,
                (proxy, error) => onInit(proxy, error));
            return true;
        } catch (e) {
            warn(`construct device proxy ${path}`, e);
            return false;
        }
    }
}

/** CPU / Page Cache 共用：单域策略列表菜单控制器 */
class DomainController {
    /**
     * @param {QuickSettings.QuickMenuToggle} toggle 所属开关
     * @param {object} opts: {noneLabel, iconName}
     */
    constructor(toggle, {noneLabel, iconName}) {
        this._toggle = toggle;
        this._noneLabel = noneLabel;
        this._iconName = iconName;

        this._proxy = null;
        this._dbusIds = [];
        this._propIds = [];
        this._items = []; // [{name, item}]
        this._section = new PopupMenu.PopupMenuSection();
        this._failTimer = 0;
        this._failureText = '';

        toggle.menu.setHeader(iconName, toggle.title, OFFLINE_TEXT);
        toggle.menu.addMenuItem(this._section);
        toggle.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        toggle.menu.addSettingsAction('Open SchedMgr', 'io.kleinia.SchedMgr.desktop');
        this._showOfflineMenu();
    }

    /** 服务就绪：接管代理，做一次属性缓存同步 + 拉策略列表 */
    attach(proxy) {
        this.detach();
        if (!proxy)
            return; // 该域代理 init 失败 → 维持 offline 展示

        this._proxy = proxy;
        this._dbusIds = [
            proxy.connectSignal('StrategyChanged', (p, _owner, args) => {
                const [strategy] = args;
                this._clearFailure();
                this._applyState(strategy ?? '');
            }),
            proxy.connectSignal('StrategyFailed', (p, _owner, args) => {
                const [, reason] = args;
                this._showFailure(reason ?? 'strategy failed');
            }),
        ];
        this._propIds = [
            proxy.connect('g-properties-changed', (p, changed) => {
                const props = changed ? changed.deepUnpack() : {};
                if ('Available' in props || 'StatusMessage' in props)
                    this._refreshStrategies();
                if ('CurrentStrategy' in props)
                    this._syncFromCache();
            }),
        ];
        this._syncFromCache();
        this._refreshStrategies();
    }

    /** 服务离线：清信号，菜单显示单条禁用项 */
    detach() {
        if (this._proxy) {
            disconnectAll(this._proxy,
                {dbusIds: this._dbusIds, propIds: this._propIds});
            this._dbusIds = [];
            this._propIds = [];
            this._proxy = null;
        }
        this._clearFailure();
        this._showOfflineMenu();
    }

    destroy() {
        this.detach();
        this._section.destroy();
    }

    _showOfflineMenu() {
        this._items = [];
        this._section.removeAll();
        this._section.addMenuItem(
            new PopupMenu.PopupMenuItem(OFFLINE_TEXT, {reactive: false}));
        this._setSubtitle(OFFLINE_TEXT);
    }

    /** 属性缓存同步读（同步、无总线调用）：副标题 + 当前项标记 */
    _syncFromCache() {
        if (!this._proxy)
            return;
        this._applyState(this._proxy.CurrentStrategy ?? '');
    }

    _applyState(current) {
        if (!this._proxy)
            return;
        // 副标题：失败信息优先，其次 CurrentStrategy（"" → "none"）
        if (!this._failureText)
            this._setSubtitle(currentLabel(current));
        this._updateOrnaments(current);
    }

    _setSubtitle(text) {
        this._toggle.subtitle = text;
        this._toggle.menu.setHeader(this._iconName, this._toggle.title, text);
    }

    _showFailure(reason) {
        this._failureText = `⚠ ${reason}`;
        this._setSubtitle(this._failureText);
        // 一次性回落定时器；下次 StrategyChanged 也会立即恢复
        if (this._failTimer)
            GLib.Source.remove(this._failTimer);
        this._failTimer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT,
            FAIL_FADE_SEC, () => {
                this._failTimer = 0;
                this._failureText = '';
                this._syncFromCache();
                return GLib.SOURCE_REMOVE;
            });
    }

    _clearFailure() {
        if (this._failTimer) {
            GLib.Source.remove(this._failTimer);
            this._failTimer = 0;
        }
        this._failureText = '';
    }

    /** 异步拉取策略列表并重建菜单项（初次加载一次，之后全靠信号） */
    _refreshStrategies() {
        const proxy = this._proxy;
        if (!proxy)
            return;
        try {
            proxy.ListStrategiesRemote((strategies, error) => {
                if (error) {
                    warn('ListStrategies', error);
                    return;
                }
                if (this._proxy !== proxy)
                    return;
                this._rebuildMenu(normalizeStrategies(strategies));
            });
        } catch (e) {
            warn('ListStrategies call', e);
        }
    }

    _rebuildMenu(strategies) {
        const proxy = this._proxy;
        this._section.removeAll();
        this._items = [];

        // Available=false：只显示一条禁用项显示 StatusMessage（规格 3）
        if (!proxy || proxy.Available === false) {
            this._section.addMenuItem(new PopupMenu.PopupMenuItem(
                proxy?.StatusMessage || 'not available', {reactive: false}));
            return;
        }

        for (const entry of strategies) {
            const [name, description] = Array.isArray(entry) ? entry : [entry, ''];
            const item = new PopupMenu.PopupMenuItem(
                strategyItemLabel(name, this._noneLabel));
            item.label.get_accessible?.()?.set_name?.(description || name);
            item.connect('activate', () => this._apply(name));
            this._section.addMenuItem(item);
            this._items.push({name, item});
        }
        this._syncFromCache();
    }

    _updateOrnaments(current) {
        const cur = current === '' || current === null ? 'none' : current;
        for (const {name, item} of this._items)
            item.setOrnament(name === cur
                ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
    }

    _apply(name) {
        const proxy = this._proxy;
        if (!proxy)
            return;
        try {
            // 异步；成功结果一律等 StrategyChanged/属性变更信号，不本地改态
            proxy.ApplyStrategyRemote(name, (_res, error) => {
                if (error)
                    this._showFailure(
                        error.message.replace(/^GDBus\.Error:[^:]*:\s*/, ''));
            });
        } catch (e) {
            warn('ApplyStrategy call', e);
        }
    }
}

/** 磁盘 I/O：顶部全局状态行 + 每设备 PopupSubMenuMenuItem */
class IoController {
    constructor(toggle) {
        this._toggle = toggle;
        this._service = null;

        this._globalProxy = null;
        this._listProxy = null;
        this._globalDbusIds = [];
        this._globalPropIds = [];
        this._listDbusIds = [];
        this._listPropIds = [];
        this._devices = new Map(); // path → {name, proxy, sub, items[], dbusIds[], propIds[]}

        this._statusItem = new PopupMenu.PopupMenuItem(OFFLINE_TEXT, {reactive: false});
        this._section = new PopupMenu.PopupMenuSection();

        toggle.menu.setHeader('drive-harddisk-symbolic', toggle.title, OFFLINE_TEXT);
        toggle.menu.addMenuItem(this._statusItem);
        toggle.menu.addMenuItem(this._section);
        toggle.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        toggle.menu.addSettingsAction('Open SchedMgr', 'io.kleinia.SchedMgr.desktop');
        this._setSubtitle(OFFLINE_TEXT);
    }

    /** 服务就绪：接管全局 Io 的 Sched1 与 Io1 两个代理 */
    attach(service, globalProxy, listProxy) {
        this.detach();
        if (!globalProxy || !listProxy)
            return; // 维持 offline 展示

        this._service = service;
        this._globalProxy = globalProxy;
        this._listProxy = listProxy;

        this._globalDbusIds = [
            globalProxy.connectSignal('StrategyChanged',
                () => this._updateGlobalStatus()),
        ];
        this._globalPropIds = [
            globalProxy.connect('g-properties-changed', (p, changed) => {
                const props = changed ? changed.deepUnpack() : {};
                if ('CurrentStrategy' in props || 'StatusMessage' in props ||
                    'Available' in props)
                    this._updateGlobalStatus();
            }),
        ];
        this._listDbusIds = [
            listProxy.connectSignal('DevicesChanged', () => this._rebuildDevices()),
        ];
        this._listPropIds = [
            listProxy.connect('g-properties-changed', (p, changed) => {
                const props = changed ? changed.deepUnpack() : {};
                if ('Devices' in props)
                    this._rebuildDevices();
            }),
        ];

        this._updateGlobalStatus();
        this._rebuildDevices();
    }

    detach() {
        for (const path of [...this._devices.keys()])
            this._removeDevice(path);
        if (this._globalProxy) {
            disconnectAll(this._globalProxy,
                {dbusIds: this._globalDbusIds, propIds: this._globalPropIds});
            this._globalDbusIds = [];
            this._globalPropIds = [];
            this._globalProxy = null;
        }
        if (this._listProxy) {
            disconnectAll(this._listProxy,
                {dbusIds: this._listDbusIds, propIds: this._listPropIds});
            this._listDbusIds = [];
            this._listPropIds = [];
            this._listProxy = null;
        }
        this._service = null;
        this._statusItem.label.text = OFFLINE_TEXT;
        this._setSubtitle(OFFLINE_TEXT);
    }

    destroy() {
        this.detach();
        this._section.destroy();
        this._statusItem.destroy();
    }

    _setSubtitle(text) {
        this._toggle.subtitle = text;
        this._toggle.menu.setHeader('drive-harddisk-symbolic', this._toggle.title, text);
    }

    /** 顶部一行全局状态：Io 对象 CurrentStrategy 或 StatusMessage */
    _updateGlobalStatus() {
        if (!this._globalProxy)
            return;
        const cur = this._globalProxy.CurrentStrategy ?? '';
        const statusMsg = this._globalProxy.StatusMessage ?? '';
        if (cur !== '')
            this._statusItem.label.text = `Global IO: ${cur}`;
        else if (statusMsg !== '')
            this._statusItem.label.text = statusMsg;
        else
            this._statusItem.label.text = 'Global IO: none';

        // 副标题：优先全局策略名；v1 无全局策略时给出设备计数
        const n = this._devices.size;
        this._setSubtitle(cur !== '' ? cur : (n > 0 ? `${n} devices` : 'none'));
    }

    /** 对比 Devices 属性缓存与现有设备，增删设备代理与子菜单项 */
    _rebuildDevices() {
        if (!this._listProxy)
            return;
        const paths = this._listProxy.Devices ?? [];

        for (const path of paths) {
            if (!this._devices.has(path))
                this._addDevice(path);
        }
        for (const path of [...this._devices.keys()]) {
            if (!paths.includes(path))
                this._removeDevice(path);
        }
        this._updateGlobalStatus();
    }

    _addDevice(path) {
        const name = path.split('/').pop();
        // label 初始为设备名，代理就绪后更新为 "sda — kyber"
        const sub = new PopupMenu.PopupSubMenuMenuItem(name);
        const entry = {
            name, proxy: null, sub, items: [], dbusIds: [], propIds: [],
        };
        this._devices.set(path, entry);
        this._section.addMenuItem(sub);

        const ok = this._service.makeDeviceProxy(path, (proxy, error) => {
            if (error || !proxy) {
                warn(`device proxy ${path}`, error);
                return;
            }
            if (this._devices.get(path) !== entry)
                return; // 等待期间已被移除
            entry.proxy = proxy;
            entry.dbusIds = [
                proxy.connectSignal('StrategyChanged', (p, _o, args) => {
                    const [strategy] = args;
                    this._updateDevice(entry, strategy ?? '');
                }),
            ];
            entry.propIds = [
                proxy.connect('g-properties-changed', (p, changed) => {
                    const props = changed ? changed.deepUnpack() : {};
                    if ('CurrentStrategy' in props)
                        this._updateDevice(entry, proxy.CurrentStrategy ?? '');
                }),
            ];
            this._updateDevice(entry, proxy.CurrentStrategy ?? '');

            try {
                proxy.ListStrategiesRemote((strategies, err) => {
                    if (err) {
                        warn(`device ListStrategies ${path}`, err);
                        return;
                    }
                    if (this._devices.get(path) !== entry)
                        return;
                    sub.menu.removeAll();
                    entry.items = [];
                    for (const row of normalizeStrategies(strategies)) {
                        const [sname] = Array.isArray(row) ? row : [row, ''];
                        const item = new PopupMenu.PopupMenuItem(
                            strategyItemLabel(sname, 'None'));
                        item.connect('activate', () => {
                            try {
                                proxy.ApplyStrategyRemote(sname, (_r, e) => {
                                    if (e)
                                        warn(`apply ${path}/${sname}`, e);
                                });
                            } catch (e) {
                                warn(`apply call ${path}/${sname}`, e);
                            }
                        });
                        sub.menu.addMenuItem(item);
                        entry.items.push({name: sname, item});
                    }
                    this._updateDevice(entry, proxy.CurrentStrategy ?? '');
                });
            } catch (e) {
                warn(`device ListStrategies call ${path}`, e);
            }
        });
        if (!ok)
            this._removeDevice(path);
    }

    _updateDevice(entry, current) {
        entry.sub.label.text = `${entry.name} — ${currentLabel(current)}`;
        const cur = current === '' || current === null ? 'none' : current;
        for (const {name, item} of entry.items)
            item.setOrnament(name === cur
                ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
        this._updateGlobalStatus();
    }

    _removeDevice(path) {
        const entry = this._devices.get(path);
        if (!entry)
            return;
        // 先从 Map 删除，再注销/销毁：中途任何异常都不会留下半删除状态
        this._devices.delete(path);
        if (entry.proxy) {
            disconnectAll(entry.proxy,
                {dbusIds: entry.dbusIds, propIds: entry.propIds});
        }
        entry.sub.destroy();
        this._updateGlobalStatus();
    }
}

const SchedMgrIndicator = GObject.registerClass(
class SchedMgrIndicator extends QuickSettings.SystemIndicator {
    _init(extension) {
        super._init();

        this._service = null;
        this._cpuController = null;
        this._cacheController = null;
        this._ioController = null;

        // 面板象征图标：常显（_addIndicator 初始 visible=false）
        this._icon = this._addIndicator();
        this._icon.gicon = Gio.icon_new_for_string(
            `${extension.path}/icons/kleinia-schedmgr-symbolic.svg`);
        this._icon.visible = true;

        const mkToggle = (title, iconName) => {
            const toggle = new QuickSettings.QuickMenuToggle({
                title,
                iconName,
                toggleMode: false, // 纯菜单按钮，非开关
            });
            this.quickSettingsItems.push(toggle);
            return toggle;
        };

        const cpuToggle = mkToggle('CPU Sched', 'speedometer-symbolic');
        const cacheToggle = mkToggle('Page Cache', 'view-paged-symbolic');
        const ioToggle = mkToggle('Disk I/O', 'drive-harddisk-symbolic');

        this._cpuController = new DomainController(cpuToggle,
            {noneLabel: 'EEVDF · kernel default', iconName: 'speedometer-symbolic'});
        this._cacheController = new DomainController(cacheToggle,
            {noneLabel: 'None', iconName: 'view-paged-symbolic'});
        this._ioController = new IoController(ioToggle);

        const onReady = bundle => {
            try {
                this._cpuController.attach(bundle.cpu);
                this._cacheController.attach(bundle.cache);
                this._ioController.attach(this._service,
                    bundle.ioPolicy, bundle.ioList);
            } catch (e) {
                warn('ready hook', e);
            }
        };
        const onOffline = () => {
            try {
                this._cpuController.detach();
                this._cacheController.detach();
                this._ioController.detach();
            } catch (e) {
                warn('offline hook', e);
            }
        };

        this._service = new SchedMgrService(extension.getSettings(), onReady, onOffline);
        this._service.start();
    }

    destroy() {
        this._service?.stop();
        this._service = null;
        this._cpuController?.destroy();
        this._cacheController?.destroy();
        this._ioController?.destroy();
        this._cpuController = this._cacheController = this._ioController = null;
        // QuickMenuToggle 不是本容器子项（在 QuickSettings grid 中），需单独销毁
        for (const item of this.quickSettingsItems)
            item.destroy();
        this.quickSettingsItems = [];
        super.destroy();
    }
});

export default class SchedMgrExtension extends Extension {
    enable() {
        this._indicator = new SchedMgrIndicator(this);
        this._addToQuickSettings(this._indicator);
        console.log(`schedmgr: enabled on ${this._indicator._service?._busType() === Gio.BusType.SESSION ? 'session' : 'system'} bus`);
    }

    _addToQuickSettings(indicator) {
        // GNOME 51：模块级 API（若提供）；45–51：quickSettings 菜单上的
        // 公开方法；更早的私有实现：_addItems + 手动挂面板图标容器。
        const quickSettings = Main.panel.statusArea?.quickSettings;
        if (typeof QuickSettings.addExternalIndicator === 'function') {
            QuickSettings.addExternalIndicator(indicator, 1);
        } else if (quickSettings &&
                   typeof quickSettings.addExternalIndicator === 'function') {
            quickSettings.addExternalIndicator(indicator, 1);
        } else if (quickSettings) {
            quickSettings._addItems(indicator.quickSettingsItems, 1);
            quickSettings._indicators.add_child(indicator);
        } else {
            warn('enable', new Error('QuickSettings not available'));
        }
    }

    _removeFromQuickSettings(indicator) {
        // addExternalIndicator 没有对应的移除 API；indicator 及其 grid 子项
        // 已在 SchedMgrIndicator.destroy() 里销毁，这里仅防御性清容器引用。
        const quickSettings = Main.panel.statusArea?.quickSettings;
        if (quickSettings?._indicators?.contains?.(indicator))
            quickSettings._indicators.remove_child(indicator);
    }

    disable() {
        this._removeFromQuickSettings(this._indicator);
        this._indicator?.destroy();
        this._indicator = null;
    }
}
