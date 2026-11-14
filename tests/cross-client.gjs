#!/usr/bin/env gjs -m
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
// 交叉验证：扩展端 dbus.js 的代理定义（XML/wrapper）+ 原始 DBus 调用
// → 对接真实 C schedmgrd v2（--backend=sim，会话总线），v2 域模型语义。
// 前置：schedmgrd --session --backend=sim 已在同一会话总线上运行。
// 注：gjs 的 *Remote 方法必须传回调（无回调返回 undefined，不是 Promise），
//     回调 result 为 out 参数数组（单出参即 [value]）——探针实测。
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import {
    BUS_NAME, ROOT_PATH, cpuPath, cachePath, ioPath, ioDevicePath,
    ManagerProxy, PolicyProxy, IoListProxy,
} from '../extension/schedmgr@kleinia.io/dbus.js';

const loop = GLib.MainLoop.new(null, false);
let pass = 0, fail = 0;
const ok = (cond, msg) => {
    if (cond) { pass++; print(`ok:   ${msg}`); }
    else { fail++; print(`FAIL: ${msg}`); }
};
const sleep = ms => new Promise(r =>
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => { r(); return GLib.SOURCE_REMOVE; }));
const bus = Gio.DBus.session;

/** 包装 *Remote 回调风格为 Promise；resolve 值为 out 参数数组 */
const call = (obj, method, ...args) => new Promise((res, rej) => {
    obj[`${method}Remote`](...args, (result, error) => (error ? rej(error) : res(result)));
});

const initProxy = (Ctor, path) => new Promise((res, rej) => {
    new Ctor(bus, BUS_NAME, path, (proxy, err) => (err ? rej(err) : res(proxy)));
});

const waitSignal = (iface, name, timeoutMs = 3000) =>
    new Promise(res => {
        const id = bus.signal_subscribe(BUS_NAME, iface, name, null, null,
            Gio.DBusSignalFlags.NONE, (conn, sender, path, ifaceName, signalName, params) => {
                bus.signal_unsubscribe(id);
                res(params.deepUnpack());
            });
        GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeoutMs, () => { res(null); return GLib.SOURCE_REMOVE; });
    });

const errName = e => {
    try {
        const rn = Gio.DBusError.get_remote_error(e);
        return rn ?? String(e);
    } catch { return String(e); }
};

const hotplug = (action, dev) => bus.call_sync(BUS_NAME, ROOT_PATH, 'io.kleinia.Sim1',
    'Hotplug', GLib.Variant.new('(ss)', [action, dev]), null,
    Gio.DBusCallFlags.NONE, -1, null);

async function main() {
    // 1) Manager1（扩展代理路径）
    const mgr = await initProxy(ManagerProxy, ROOT_PATH);
    ok(mgr.Version === 1, `Manager1.Version == 1 (got ${mgr.Version})`);
    ok(mgr.Backend === 'sim', `Manager1.Backend == sim (got ${mgr.Backend})`);

    // 2) CPU 域策略发现（v2 命名：EEVDF + scx_agent）
    const cpu = await initProxy(PolicyProxy, cpuPath());
    const strategies = (await call(cpu, 'ListStrategies'))[0];
    const names = strategies.map(sd => sd[0]);
    ok(names.includes('none'), `cpu strategies include none (${JSON.stringify(names)})`);
    ok(names.includes('scx_agent') && names.includes('scx_central'), 'cpu strategies include scx_agent/scx_central');
    const noneDesc = strategies.find(sd => sd[0] === 'none')[1];
    ok(noneDesc.includes('EEVDF') && !noneDesc.includes('CFS'), `none described as EEVDF ("${noneDesc}")`);

    // 3) 带参应用 + StrategyChanged + 属性缓存
    const sigP = waitSignal('io.kleinia.Sched1', 'StrategyChanged');
    await call(cpu, 'ApplyStrategyWithParams', 'scx_simple', ['-v']);
    const sig = await sigP;
    ok(sig !== null && sig[0] === 'scx_simple', `StrategyChanged == scx_simple (got ${JSON.stringify(sig)})`);
    await sleep(300);
    ok(cpu.CurrentStrategy === 'scx_simple', `cached CurrentStrategy == scx_simple (got '${cpu.CurrentStrategy}')`);

    // 4) 错误映射
    try {
        await call(cpu, 'ApplyStrategy', 'definitely-not-a-strategy');
        ok(false, 'bogus apply should raise');
    } catch (e) {
        ok(errName(e).includes('NoSuchStrategy'), `bogus apply -> NoSuchStrategy (got ${errName(e)})`);
    }
    await call(cpu, 'StopStrategy');
    await sleep(300);
    ok(cpu.CurrentStrategy === '', `after Stop CurrentStrategy == "" (got '${cpu.CurrentStrategy}')`);

    // 5) Cache 域（v2：cax 实装）
    const cache = await initProxy(PolicyProxy, cachePath());
    ok(cache.Available === true, `cache Available in sim v2 (got ${cache.Available})`);
    const cacheStrategies = (await call(cache, 'ListStrategies'))[0].map(sd => sd[0]);
    ok(cacheStrategies.includes('cax_mru') && cacheStrategies.includes('cax_s3fifo'),
        `cache strategies include cax_mru/cax_s3fifo (${JSON.stringify(cacheStrategies)})`);
    const cacheSigP = waitSignal('io.kleinia.Sched1', 'StrategyChanged');
    await call(cache, 'ApplyStrategy', 'cax_mru');
    const cacheSig = await cacheSigP;
    ok(cacheSig !== null && cacheSig[0] === 'cax_mru', `cache StrategyChanged == cax_mru`);
    await sleep(300);
    ok(cache.CurrentStrategy === 'cax_mru', `cache CurrentStrategy == cax_mru (got '${cache.CurrentStrategy}')`);

    // 6) IO 全局域（v2：iox spawn 域）
    const ioGlobal = await initProxy(PolicyProxy, ioPath());
    ok(ioGlobal.Available === true, `global Io Available in sim v2 (got ${ioGlobal.Available})`);
    const ioStrategies = (await call(ioGlobal, 'ListStrategies'))[0].map(sd => sd[0]);
    ok(ioStrategies.includes('none') && ioStrategies.includes('iox_fifo'),
        `global Io strategies include none/iox_fifo (${JSON.stringify(ioStrategies)})`);
    await call(ioGlobal, 'ApplyStrategy', 'iox_wedf');
    await sleep(300);
    ok(ioGlobal.CurrentStrategy === 'iox_wedf', `global Io CurrentStrategy == iox_wedf (got '${ioGlobal.CurrentStrategy}')`);

    // 7) IO 设备：组合列表 + iox 有效名 + native 还原
    const ioList = await initProxy(IoListProxy, ioPath());
    const devices = ioList.Devices || [];
    ok(devices.length >= 3, `Io1.Devices >= 3 (got ${devices.length})`);
    const sdaPath = ioDevicePath('sda');
    ok(devices.includes(sdaPath), `Devices includes ${sdaPath}`);
    const sda = await initProxy(PolicyProxy, sdaPath);
    const origCurrent = sda.CurrentStrategy;
    const devStrategies = (await call(sda, 'ListStrategies'))[0].map(sd => sd[0]);
    ok(devStrategies.includes('scx_io') && devStrategies.includes('iox_fifo') && devStrategies.includes('mq-deadline'),
        `sda combined list has native + iox (${JSON.stringify(devStrategies)})`);
    // 全局已在 iox_wedf：设备选 iox_fifo → 全局切换 + 设备有效名跟随
    await call(sda, 'ApplyStrategy', 'iox_fifo');
    await sleep(300);
    ok(sda.CurrentStrategy === 'iox_fifo', `sda effective name == iox_fifo (got '${sda.CurrentStrategy}')`);
    ok(ioGlobal.CurrentStrategy === 'iox_fifo', `global policy followed to iox_fifo (got '${ioGlobal.CurrentStrategy}')`);
    // 还原 native
    await call(sda, 'ApplyStrategy', origCurrent);
    await sleep(300);
    ok(sda.CurrentStrategy === origCurrent, `sda restored to ${origCurrent} (got '${sda.CurrentStrategy}')`);

    // 8) Sim1.Hotplug → DevicesChanged → Devices 更新
    const devChangedP = waitSignal('io.kleinia.Io1', 'DevicesChanged');
    hotplug('remove', 'sda');
    const dc = await devChangedP;
    ok(dc !== null, 'received DevicesChanged signal');
    await sleep(300);
    ok(!(ioList.Devices || []).includes(sdaPath), 'sda removed from Devices');
    hotplug('add', 'sda');
    await sleep(300);
    ok((ioList.Devices || []).includes(sdaPath), 'sda re-added to Devices');

    print(`cross-client: ${pass} passed, ${fail} failed`);
    loop.quit();
}

main().catch(e => {
    print(`cross-client: FATAL ${e}`);
    fail++;
    loop.quit();
});
GLib.timeout_add(GLib.PRIORITY_DEFAULT, 30000, () => {
    print('cross-client: FATAL global timeout');
    fail++;
    loop.quit();
    return GLib.SOURCE_REMOVE;
});
loop.run();
imports.system.exit(fail > 0 ? 1 : 0);
