#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""i18n.py — io.kleinia.SchedMgr 的 gettext 初始化（GNOME 标准方案）。

约定（2026-10-28 i18n 改造）：
  * 源串一律英文，用 ``_()`` 包裹；翻译放 ``locale/<lang>/LC_MESSAGES/``。
  * 域名 = 应用 id ``io.kleinia.SchedMgr``。
  * localedir 解析顺序：
      1. 环境变量 KLEINIA_SCHEDMGR_LOCALEDIR（测试/定制用）；
      2. /usr/share/locale（仅当该目录下存在本域的 .mo，即安装版）；
      3. 源码目录 app/locale（开发运行；packaging 会把 .mo 装进 /usr/share/locale）。
  * 一律 ``gettext.translation(..., fallback=True)`` 语义：找不到目录/语言时
    回退 NullTranslations（直接显示英文源串），任何情况下不抛异常。
  * 语言选择完全由环境（LANGUAGE / LC_ALL / LC_MESSAGES / LANG）驱动——
    gettext 只做字符串解析，不依赖系统是否生成了对应 locale（无 zh_CN
    locale 的机器上 LANG=zh_CN.UTF-8 依然切中文）。
  * ``_ = 翻译对象.gettext``（等价 gettext.gettext 绑定域后）；同时对 C 运行时
    ``bindtextdomain/textdomain`` 做同等绑定（当前无 C 控件本地化需求，仅为
    GNOME 惯例与未来扩展）。

注意：daemon 经 DBus 返回的 Description/StatusMessage 属于数据面，不在本
i18n 范围内（mock-schedmgrd.py 的字符串保持原样）。桌面文件的 Name/Comment
用 Name[zh_CN]/Comment[zh_CN] 静态双语，不经 gettext。
"""

import gettext
import os

DOMAIN = "io.kleinia.SchedMgr"

# 源码目录（本文件所在目录）下的 locale/
_SOURCE_LOCALEDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "locale")

# 系统安装位置（packaging/install-destdir.sh 装入 <prefix>/share/locale）
_SYSTEM_LOCALEDIR = "/usr/share/locale"


def _candidate_localedirs():
    dirs = []
    env_dir = os.environ.get("KLEINIA_SCHEDMGR_LOCALEDIR", "").strip()
    if env_dir:
        dirs.append(env_dir)
    try:
        if gettext.find(DOMAIN, _SYSTEM_LOCALEDIR) is not None:
            dirs.append(_SYSTEM_LOCALEDIR)
    except Exception:
        pass
    dirs.append(_SOURCE_LOCALEDIR)
    return dirs


def _init_translation():
    """返回 (translation, localedir)。找不到任何目录时回退 NullTranslations。"""
    for localedir in _candidate_localedirs():
        try:
            found = gettext.find(DOMAIN, localedir)
        except Exception:
            found = None
        if found:
            try:
                return gettext.translation(DOMAIN, localedir), localedir
            except Exception:
                continue  # 损坏的 mo 等：尝试下一个候选目录
    return gettext.NullTranslations(), None


TRANSLATION, LOCALEDIR = _init_translation()

# C 运行时绑定（失败不影响 Python 侧 _）
try:
    if LOCALEDIR is not None:
        gettext.bindtextdomain(DOMAIN, LOCALEDIR)
    gettext.textdomain(DOMAIN)
except Exception:
    pass

_ = TRANSLATION.gettext


def translation_active():
    """当前语言是否有可用翻译（False = 英文源串直显）。测试用。

    注意 GNUTranslations 是 NullTranslations 的子类，须按精确类型判断。
    """
    return type(TRANSLATION) is not gettext.NullTranslations
