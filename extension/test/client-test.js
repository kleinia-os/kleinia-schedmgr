#!/usr/bin/env gjs -m
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Kleinia Authors
// client-test.js — SchedMgr 扩展 DBus 客户端层端到端测试
//
// 在 dbus-run-session 私有总线里跑：
//   dbus-run-session -- bash -c 'python3 mock-schedmgrd.py & sleep 0.5; gjs -m client-test.js'
//
// 复用扩展同源的接口定义（../schedmgr@kleinia.io/dbus.js），覆盖：
// ListStrategies / ApplyStrategyRemote / StrategyChanged / StrategyFailed /
// DevicesChanged / 属性缓存同步读 / 错误映射。全绿 exit 0，任一失败 exit 1。

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import System from 'system';

import {
    BUS_NAME, ROOT_PATH,
    ManagerProxy, PolicyProxy, IoListProxy,
} from '../schedmgr@kleinia.io/dbus.js';

// TEST-ONLY 控制接口（mock-schedmgrd 独有，见 mock-schedmgrd.py）
const MockProxy = Gio.DBusProxy.makeProxyWrapper(`
<node>
  <interface name="io.kleinia.Mock1">
    <method name="Hotplug">
      <arg type="s" direction="in" name="command"/>
    </method>
    <method name="FailNext">
      <arg type="s" direction="in" name="reason"/>
    </method>
  </interface>
</node>`);

let failures = 0;
let passes = 0;

function check(cond, label, extra = '') {
    if (cond) {
        passes++;
        console.log(`PASS ${label}`);
    } else {
        failures++;
        console.log(`FAIL ${label}${extra ? `  [${extra}]` : ''}`);
    }
}

function errText(err) {
    return String(err?.message ?? err);
}

function errMatches(err, dbusName) {
    return errText(err).includes(dbusName);
}

/** 单出参方法的返回形状兼容（直接值 / [单值] 两种 gjs 形态） */
function unwrapSingle(result) {
    if (!Array.isArray(result))
        return result;
    if (result.length === 0)
        return undefined;
    if (Array.isArray(result[0]) && typeof result[0]?.[0] !== 'string')
        return result[0];
    return result;
}

/** 等待总线名出现，返回连接 */
function waitName(timeoutMs = 10000) {
    return new Promise((resolve, reject) => {
        const watchId = Gio.bus_watch_name(
            Gio.BusType.SESSION, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            (conn, name, owner) => {
                cleanup();
                resolve({conn, owner});
            },
            () => {});
        const timerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeoutMs, () => {
            cleanup();
            reject(new Error(`timeout waiting for ${BUS_NAME} on the session bus`));
            return GLib.SOURCE_REMOVE;
        });
        function cleanup() {
            Gio.bus_unwatch_name(watchId);
            GLib.Source.remove(timerId);
        }
    });
}

function mkProxy(ProxyClass, conn, path, timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
        const timerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeoutMs, () => {
            reject(new Error(`proxy init timeout: ${path}`));
            return GLib.SOURCE_REMOVE;
        });
        new ProxyClass(conn, BUS_NAME, path, (proxy, error) => {
            GLib.Source.remove(timerId);
            if (error)
                reject(error);
            else
                resolve(proxy);
        });
    });
}

/** 异步调用代理方法 */
function remote(proxy, method, ...args) {
    return new Promise((resolve, reject) => {
        try {
            proxy[`${method}Remote`](...args, (result, error) => {
                if (error)
                    reject(error);
                else
                    resolve(result);
            });
        } catch (e) {
            reject(e);
        }
    });
}

/** 订阅一个 DBus 信号一次，resolve 信号参数数组 */
function onceSignal(proxy, name, timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
        const id = proxy.connectSignal(name, (p, owner, args) => {
            cleanup();
            resolve(args);
        });
        const timerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeoutMs, () => {
            cleanup();
            reject(new Error(`timeout waiting for signal ${name}`));
            return GLib.SOURCE_REMOVE;
        });
        function cleanup() {
            proxy.disconnectSignal(id);
            GLib.Source.remove(timerId);
        }
    });
}

/** 由 g-properties-changed 驱动地等待属性缓存满足谓词（不轮询） */
function waitForProp(proxy, name, pred, timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
        if (pred(proxy[name])) {
            resolve();
            return;
        }
        const handlerId = proxy.connect('g-properties-changed', () => {
            if (pred(proxy[name])) {
                cleanup();
                resolve();
            }
        });
        const timerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, timeoutMs, () => {
            cleanup();
            reject(new Error(`timeout waiting for property ${name}`));
            return GLib.SOURCE_REMOVE;
        });
        function cleanup() {
            proxy.disconnect(handlerId);
            GLib.Source.remove(timerId);
        }
    });
}

async function main() {
    check(GLib.getenv('DBUS_SESSION_BUS_ADDRESS') !== null,
        'session bus address present');

    const {conn} = await waitName();
    check(true, `bus name ${BUS_NAME} appeared`);

    // ---- Manager1 -------------------------------------------------------
    const manager = await mkProxy(ManagerProxy, conn, ROOT_PATH);
    check(manager.Version === 1, 'Manager.Version == 1');
    check(manager.Backend === 'sim', 'Manager.Backend == sim',
        `got ${manager.Backend}`);
    check(typeof manager.KernelRelease === 'string' && manager.KernelRelease.length > 0,
        'Manager.KernelRelease non-empty');

    // ---- Cpu 域 ----------------------------------------------------------
    const cpu = await mkProxy(PolicyProxy, conn, `${ROOT_PATH}/Cpu`);
    check(cpu.CurrentStrategy === '', 'Cpu.CurrentStrategy cache == ""',
        `got ${JSON.stringify(cpu.CurrentStrategy)}`);
    check(cpu.Available === true, 'Cpu.Available == true');

    const cpuStrats = unwrapSingle(await remote(cpu, 'ListStrategies'));
    const cpuNames = cpuStrats.map(([n]) => n);
    check(['none', 'scx_agent', 'scx_simple', 'scx_central'].every(
        n => cpuNames.includes(n)), 'Cpu.ListStrategies has all sim strategies',
        `got ${cpuNames}`);
    const cpuNoneDesc = cpuStrats.find(([n]) => n === 'none')?.[1] ?? '';
    check(cpuNoneDesc.includes('EEVDF'),
        'Cpu none description says EEVDF (§8.1)', `got ${cpuNoneDesc}`);
    check(!cpuNoneDesc.includes('CFS'), 'Cpu none description drops CFS (§8.1)');

    // Apply：信号 + 属性缓存
    const evChanged = onceSignal(cpu, 'StrategyChanged');
    await remote(cpu, 'ApplyStrategy', 'scx_simple');
    const [sigStrategy, sigParams] = await evChanged;
    check(sigStrategy === 'scx_simple', 'StrategyChanged.strategy == scx_simple',
        `got ${sigStrategy}`);
    check(Array.isArray(sigParams) && sigParams.length === 0,
        'StrategyChanged.params == []');
    await waitForProp(cpu, 'CurrentStrategy', v => v === 'scx_simple');
    check(cpu.CurrentStrategy === 'scx_simple',
        'Cpu.CurrentStrategy cache == scx_simple');

    // 错误路径：NoSuchStrategy，状态不变
    try {
        await remote(cpu, 'ApplyStrategy', 'bogus');
        check(false, 'ApplyStrategy(bogus) raises NoSuchStrategy');
    } catch (e) {
        check(errMatches(e, 'NoSuchStrategy'),
            'ApplyStrategy(bogus) raises NoSuchStrategy', errText(e));
    }
    check(cpu.CurrentStrategy === 'scx_simple', 'state unchanged after error');

    // 错误路径：TEST-ONLY FailNext → StrategyFailed + 状态回滚（铁律）
    const mock = await mkProxy(MockProxy, conn, ROOT_PATH);
    await remote(mock, 'FailNext', 'simulated confirmation failure');
    const evFailed = onceSignal(cpu, 'StrategyFailed');
    try {
        await remote(cpu, 'ApplyStrategy', 'scx_central');
        check(false, 'failing ApplyStrategy raises error');
    } catch (e) {
        check(errMatches(e, 'Failed'), 'failing ApplyStrategy raises Failed',
            errText(e));
    }
    const [failStrategy, failReason] = await evFailed;
    check(failStrategy === 'scx_central', 'StrategyFailed.strategy == scx_central');
    check(failReason === 'simulated confirmation failure',
        'StrategyFailed.reason forwarded', `got ${failReason}`);
    check(cpu.CurrentStrategy === 'scx_simple',
        'CurrentStrategy unchanged after StrategyFailed');

    // Stop / Restart（依赖 last_applied）
    await remote(cpu, 'StopStrategy');
    await waitForProp(cpu, 'CurrentStrategy', v => v === '');
    check(cpu.CurrentStrategy === '', 'StopStrategy → CurrentStrategy == ""');
    try {
        await remote(cpu, 'StopStrategy');
        check(false, 'StopStrategy when idle raises NotRunning');
    } catch (e) {
        check(errMatches(e, 'NotRunning'),
            'StopStrategy when idle raises NotRunning', errText(e));
    }
    await remote(cpu, 'RestartStrategy');
    await waitForProp(cpu, 'CurrentStrategy', v => v === 'scx_simple');
    check(cpu.CurrentStrategy === 'scx_simple',
        'RestartStrategy restores scx_simple');

    // 带参数 apply（仅 Cpu 域有意义）
    await remote(cpu, 'ApplyStrategyWithParams', 'scx_agent', ['-v', '--test']);
    await waitForProp(cpu, 'CurrentStrategy', v => v === 'scx_agent');
    check(cpu.CurrentStrategy === 'scx_agent',
        'ApplyStrategyWithParams → CurrentStrategy == scx_agent');
    check(JSON.stringify(cpu.CurrentParams) === JSON.stringify(['-v', '--test']),
        'CurrentParams cache == ["-v","--test"]',
        `got ${JSON.stringify(cpu.CurrentParams)}`);

    // ---- Cache 域（§8.3 cax_* 全链路）-------------------------------------
    const cache = await mkProxy(PolicyProxy, conn, `${ROOT_PATH}/Cache`);
    check(cache.Available === true, 'Cache.Available == true (sim)');
    const cacheStrats = unwrapSingle(await remote(cache, 'ListStrategies'));
    const cacheNames = cacheStrats.map(([n]) => n);
    check(['cax_mru', 'cax_lhd', 'cax_s3fifo'].every(n => cacheNames.includes(n)),
        'Cache.ListStrategies == cax_mru/cax_lhd/cax_s3fifo', `got ${cacheNames}`);
    await remote(cache, 'ApplyStrategy', 'cax_lhd');
    await waitForProp(cache, 'CurrentStrategy', v => v === 'cax_lhd');
    check(cache.CurrentStrategy === 'cax_lhd', 'Cache apply cax_lhd OK');
    try {
        await remote(cache, 'ApplyStrategyWithParams', 'cax_mru', ['-x']);
        check(false, 'Cache ApplyStrategyWithParams(args) raises NotSupported');
    } catch (e) {
        check(errMatches(e, 'NotSupported'),
            'Cache ApplyStrategyWithParams(args) raises NotSupported', errText(e));
    }
    try {
        await remote(cache, 'RestoreDefault');
        check(false, 'RestoreDefault without default raises NoDefault');
    } catch (e) {
        check(errMatches(e, 'NoDefault'),
            'RestoreDefault without default raises NoDefault', errText(e));
    }

    // ---- Io 全局（§8.2 iox 策略域，与 Cpu 同构）---------------------------
    const ioGlobal = await mkProxy(PolicyProxy, conn, `${ROOT_PATH}/Io`);
    check(ioGlobal.Available === true, 'global Io.Available == true (iox domain)');
    const ioxStrats = unwrapSingle(await remote(ioGlobal, 'ListStrategies'));
    const ioxNames = ioxStrats.map(([n]) => n);
    check(['none', 'iox_fifo', 'iox_wedf', 'iox_merge'].every(
        n => ioxNames.includes(n)), 'global Io.ListStrategies has iox set + none',
        `got ${ioxNames}`);
    const evIox = onceSignal(ioGlobal, 'StrategyChanged');
    await remote(ioGlobal, 'ApplyStrategy', 'iox_wedf');
    const [ioxApplied] = await evIox;
    check(ioxApplied === 'iox_wedf', 'global Io StrategyChanged == iox_wedf');
    await waitForProp(ioGlobal, 'CurrentStrategy', v => v === 'iox_wedf');
    check(ioGlobal.CurrentStrategy === 'iox_wedf', 'global Io cache == iox_wedf');
    await remote(ioGlobal, 'StopStrategy');
    await waitForProp(ioGlobal, 'CurrentStrategy', v => v === '');
    check(ioGlobal.CurrentStrategy === '', 'global Io StopStrategy → ""');
    try {
        await remote(ioGlobal, 'ApplyStrategy', 'iox_bogus');
        check(false, 'global Io ApplyStrategy(bogus) raises NoSuchStrategy');
    } catch (e) {
        check(errMatches(e, 'NoSuchStrategy'),
            'global Io ApplyStrategy(bogus) raises NoSuchStrategy', errText(e));
    }

    // ---- Io1 设备发现 ------------------------------------------------------
    const ioList = await mkProxy(IoListProxy, conn, `${ROOT_PATH}/Io`);
    check(Array.isArray(ioList.Devices) && ioList.Devices.length === 3,
        'Io1.Devices == 3', `got ${ioList.Devices?.length}`);
    check(ioList.Devices.includes(`${ROOT_PATH}/Io/sda`), 'sda listed');

    const sda = await mkProxy(PolicyProxy, conn, `${ROOT_PATH}/Io/sda`);
    check(sda.CurrentStrategy === 'kyber', 'sda.CurrentStrategy == kyber',
        `got ${sda.CurrentStrategy}`);
    check(sda.Description.includes('SATA'), 'sda.Description has model info',
        `got ${sda.Description}`);
    const devStrats = unwrapSingle(await remote(sda, 'ListStrategies')).map(
        ([n]) => n);
    // §8.2：native ∪ iox 组合列表，iox 项在后
    check(JSON.stringify(devStrats) === JSON.stringify(
        ['none', 'mq-deadline', 'kyber', 'bfq', 'scx_io',
         'iox_fifo', 'iox_wedf', 'iox_merge']),
        'sda strategy list exact (native ∪ iox)', `got ${devStrats}`);
    const ioxDesc = unwrapSingle(await remote(sda, 'ListStrategies'))
        .find(([n]) => n === 'iox_fifo')?.[1] ?? '';
    check(ioxDesc.includes('global eBPF'),
        'sda iox entry flagged as global eBPF policy', `got ${ioxDesc}`);

    // 设备 native apply（v1 语义不变）
    const evSda = onceSignal(sda, 'StrategyChanged');
    await remote(sda, 'ApplyStrategy', 'bfq');
    const [sdaNew] = await evSda;
    check(sdaNew === 'bfq', 'sda native apply → bfq');
    await waitForProp(sda, 'CurrentStrategy', v => v === 'bfq');
    check(sda.CurrentStrategy === 'bfq', 'sda cache == bfq');

    // §8.2 设备 iox apply：①全局 iox_X 运行 ②设备有效名 = iox_X
    const evSdaIox = onceSignal(sda, 'StrategyChanged');
    await remote(sda, 'ApplyStrategy', 'iox_fifo');
    await evSdaIox;
    await waitForProp(sda, 'CurrentStrategy', v => v === 'iox_fifo');
    check(sda.CurrentStrategy === 'iox_fifo', 'sda iox apply → effective iox_fifo');
    await waitForProp(ioGlobal, 'CurrentStrategy', v => v === 'iox_fifo');
    check(ioGlobal.CurrentStrategy === 'iox_fifo',
        'device iox apply ensured global iox_fifo (§8.2)');

    // 热插拔：DevicesChanged + Devices 属性缓存
    const evDevices = onceSignal(ioList, 'DevicesChanged');
    await remote(mock, 'Hotplug', 'add sdb');
    await evDevices;
    check(true, 'DevicesChanged received (add sdb)');
    await waitForProp(ioList, 'Devices',
        d => d.includes(`${ROOT_PATH}/Io/sdb`));
    check(ioList.Devices.includes(`${ROOT_PATH}/Io/sdb`),
        'Devices cache includes sdb');

    const sdb = await mkProxy(PolicyProxy, conn, `${ROOT_PATH}/Io/sdb`);
    check(sdb.CurrentStrategy === 'mq-deadline',
        'sdb initial CurrentStrategy == mq-deadline', `got ${sdb.CurrentStrategy}`);
    const evSdb = onceSignal(sdb, 'StrategyChanged');
    await remote(sdb, 'ApplyStrategy', 'bfq');
    const [sdbNew] = await evSdb;
    check(sdbNew === 'bfq', 'sdb StrategyChanged.strategy == bfq');
    await waitForProp(sdb, 'CurrentStrategy', v => v === 'bfq');
    check(sdb.CurrentStrategy === 'bfq', 'sdb cache == bfq');

    // §8.2 全局唯一：sdb 后选 iox_merge → sda 有效名自动跟随
    const evSdbMerge = onceSignal(sdb, 'StrategyChanged');
    await remote(sdb, 'ApplyStrategy', 'iox_merge');
    await evSdbMerge;
    await waitForProp(sdb, 'CurrentStrategy', v => v === 'iox_merge');
    check(sdb.CurrentStrategy === 'iox_merge', 'sdb iox_merge effective');
    await waitForProp(sda, 'CurrentStrategy', v => v === 'iox_merge');
    check(sda.CurrentStrategy === 'iox_merge',
        'sda effective name follows global iox_merge (§8.2)');

    const evRemoved = onceSignal(ioList, 'DevicesChanged');
    await remote(mock, 'Hotplug', 'remove sda');
    await evRemoved;
    await waitForProp(ioList, 'Devices',
        d => !d.includes(`${ROOT_PATH}/Io/sda`));
    check(!ioList.Devices.includes(`${ROOT_PATH}/Io/sda`),
        'sda removed from Devices cache');

    // ---- 汇总 --------------------------------------------------------------
    console.log(`\nclient-test: ${passes} passed, ${failures} failed`);
}

const loop = new GLib.MainLoop(null, false);
let exitCode = 1;

// 兜底看门狗：只退出主循环；System.exit 统一在 loop.run() 返回后执行一次
const watchdog = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 120, () => {
    console.error('client-test: WATCHDOG timeout');
    exitCode = 2;
    loop.quit();
    return GLib.SOURCE_REMOVE;
});

main()
    .then(() => {
        exitCode = failures > 0 ? 1 : 0;
    })
    .catch(e => {
        exitCode = 1;
        console.error(`client-test: FATAL ${errText(e)}`);
    })
    .finally(() => {
        GLib.Source.remove(watchdog);
        loop.quit();
    });

loop.run();
System.exit(exitCode);
