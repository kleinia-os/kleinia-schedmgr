#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
"""gen-translations.py — io.kleinia.SchedMgr 的翻译生成与编译（无 msgfmt 也能跑）。

本机 Ubuntu 26.10 未安装 gettext msgfmt/xgettext（`command -v msgfmt` 为空），
本脚本自实现三件事（对齐 gettext 工具链行为）：

  1. **抽取**：用 ast 解析 SOURCES 里的 ``_("...")`` 调用（首个参数必须是
     字符串常量），得到完整 msgid 清单与源码引用（等价 xgettext -k_）；
  2. **写 POT / 合并 PO**：生成 locale/io.kleinia.SchedMgr.pot（msgstr 为空），
     并把 zh_CN.po 重写为与最新 msgid 清单同步（保留既有译文，新串置空待译，
     源串消失的译文剔除）；
  3. **编译 MO**：极简 MO 编译器（GNU MO 格式：magic 0x950412de + 原文/译文
     偏移表 + NUL 结尾串），写出 locale/zh_CN/LC_MESSAGES/io.kleinia.SchedMgr.mo。

用法：
  python3 locale/gen-translations.py            # 全流程；有缺译/多余时 rc=1
  python3 locale/gen-translations.py --check    # 只检查，不改任何文件
"""

import ast
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.dirname(HERE)
DOMAIN = "io.kleinia.SchedMgr"
SOURCES = ["main.py", "window.py", "pages.py", "schedmgr-client.py"]
PO_PATH = os.path.join(HERE, "zh_CN.po")
POT_PATH = os.path.join(HERE, "%s.pot" % DOMAIN)
MO_DIR = os.path.join(HERE, "zh_CN", "LC_MESSAGES")
MO_PATH = os.path.join(MO_DIR, "%s.mo" % DOMAIN)

PO_HEADER = (
    "Project-Id-Version: io.kleinia.SchedMgr 1.0\n"
    "Report-Msgid-Bugs-To: kleinia@kleinia.io\n"
    "POT-Creation-Date: 2026-10-28 23:59+0800\n"
    "Language: zh_CN\n"
    "MIME-Version: 1.0\n"
    "Content-Type: text/plain; charset=UTF-8\n"
    "Content-Transfer-Encoding: 8bit\n"
    "Plural-Forms: nplurals=1; plural=0;\n"
    "X-Generator: locale/gen-translations.py\n"
)


# ---------------------------------------------------------------------------
# 1. 抽取（ast，等价 xgettext --keyword=_）
# ---------------------------------------------------------------------------
def extract():
    entries = {}   # msgid -> [(file, line), ...]
    for src in SOURCES:
        path = os.path.join(APP_DIR, src)
        if not os.path.isfile(path):
            continue
        tree = ast.parse(open(path, encoding="utf-8").read(), filename=src)
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if not (isinstance(func, ast.Name) and func.id == "_"):
                continue
            if not node.args or not isinstance(node.args[0], ast.Constant) \
                    or not isinstance(node.args[0].value, str):
                raise SystemExit("gen-translations: %s:%d — _() 首参必须是"
                                 "字符串常量" % (src, node.lineno))
            msgid = node.args[0].value
            entries.setdefault(msgid, []).append((src, node.lineno))
    return entries


# ---------------------------------------------------------------------------
# 2. PO 读写（支持多行字符串与标准转义）
# ---------------------------------------------------------------------------
def _escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"') \
            .replace("\n", "\\n").replace("\t", "\\t")


def _unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(
                nxt, "\\" + nxt))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _inner(quoted):
    """取 PO 引号串的内部内容（已去掉首尾引号，转义序列保留待统一 unescape）。"""
    s = quoted.strip()
    if len(s) >= 2 and s.startswith('"') and s.endswith('"'):
        return s[1:-1]
    return s


def parse_po(path):
    """解析 PO → (header_msgstr, {msgid: msgstr})。多行字符串已拼接。"""
    if not os.path.isfile(path):
        return "", {}
    lines = open(path, encoding="utf-8").read().splitlines()

    header = ""
    table = {}
    state = None        # None | "id" | "str"
    cur_id = []
    cur_str = []

    def flush():
        nonlocal state, cur_id, cur_str, header
        if state == "str":
            msgid = _unescape("".join(cur_id))
            msgstr = _unescape("".join(cur_str))
            if msgid == "":
                header = msgstr
            else:
                table[msgid] = msgstr
        state, cur_id, cur_str = None, [], []

    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("msgid "):
            flush()
            state = "id"
            cur_id = [_inner(stripped[6:])] if stripped[6:].strip() else []
            continue
        if stripped.startswith("msgstr "):
            state = "str"
            cur_str = [_inner(stripped[7:])] if stripped[7:].strip() else []
            continue
        if stripped.startswith('"'):
            (cur_id if state == "id" else cur_str).append(_inner(stripped))
    flush()
    return header, table


def _format_msgstr_block(keyword, value):
    """把 keyword + 转义串写成 PO 行；长串（如 header）按 \\n 拆多行。"""
    escaped = _escape(value)
    if keyword == "msgstr" and "\\n" in escaped:
        parts = escaped.split("\\n")
        out = ['msgstr ""']
        for i, part in enumerate(parts):
            chunk = part + ("\\n" if i < len(parts) - 1 else "")
            if chunk:
                out.append('"%s"' % chunk)
        return out
    return ['%s "%s"' % (keyword, escaped)]


def write_catalog(path, entries, header, translated, title_lines):
    """entries: {msgid: [(file,line),...]}；按 msgid 排序写 PO/POT。"""
    out = list(title_lines)
    out.extend(_format_msgstr_block("msgstr", header))
    for msgid in sorted(entries):
        out.append("")
        out.append("#: " + " ".join("%s:%d" % r for r in entries[msgid]))
        out.append('msgid "%s"' % _escape(msgid))
        out.append('msgstr "%s"' % _escape(translated.get(msgid, "")))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")


# ---------------------------------------------------------------------------
# 3. 极简 MO 编译器（GNU MO：0x950412de，小端偏移表）
# ---------------------------------------------------------------------------
def compile_mo(catalog):
    """catalog: {msgid: msgstr}（须含 msgid "" 的 header 条目）→ MO 字节。

    header 条目不能省：Python gettext / glibc dgettext 都靠它声明的
    Content-Type charset 解码译文（缺省按 ASCII，UTF-8 中文会解码失败）。
    """
    pairs = sorted((k, v) for k, v in catalog.items() if v)
    n = len(pairs)
    ids = b""
    strs = b""
    offsets = []
    for msgid, msgstr in pairs:
        ib = msgid.encode("utf-8")
        sb = msgstr.encode("utf-8")
        offsets.append((len(ids), len(ib), len(strs), len(sb)))
        ids += ib + b"\0"
        strs += sb + b"\0"
    keystart = 7 * 4 + 16 * n
    valuestart = keystart + len(ids)
    koffsets = b""
    voffsets = b""
    for o1, l1, o2, l2 in offsets:
        koffsets += struct.pack("<II", l1, o1 + keystart)
        voffsets += struct.pack("<II", l2, o2 + valuestart)
    output = struct.pack("<Iiiiiii",
                         0x950412de, 0, n, 7 * 4, 7 * 4 + n * 8, 0, 0)
    output += koffsets + voffsets + ids + strs
    return output


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    check_only = "--check" in sys.argv[1:]
    entries = extract()
    header, translated = parse_po(PO_PATH)

    missing = sorted(m for m in entries if not translated.get(m))
    extra = sorted(m for m in translated if m not in entries)
    print("gen-translations: %d msgid(s) from _() calls; zh_CN.po: %d "
          "translated, %d missing, %d obsolete"
          % (len(entries), len(entries) - len(missing), len(missing),
             len(extra)))
    for m in missing:
        print("  MISSING: %r" % m[:72])
    for m in extra:
        print("  OBSOLETE: %r" % m[:72])

    if check_only:
        return 0 if not missing and not extra else 1

    pot_lines = ["# %s — translation template (POT)." % DOMAIN,
                 "# Generated by locale/gen-translations.py; edit zh_CN.po,",
                 "# never this file.", ""]
    po_lines = ["# %s — Simplified Chinese translations." % DOMAIN,
                "# 中文译文；由 locale/gen-translations.py 与源码同步合并，",
                "# 译文请直接编辑本文件。", ""]
    write_catalog(POT_PATH, entries, PO_HEADER, {}, pot_lines)
    write_catalog(PO_PATH, entries, header or PO_HEADER, translated, po_lines)
    os.makedirs(MO_DIR, exist_ok=True)
    full = dict(translated)
    full[""] = header or PO_HEADER      # header 条目进 MO（charset 声明）
    with open(MO_PATH, "wb") as fh:
        fh.write(compile_mo(full))
    print("gen-translations: wrote %s, zh_CN.po, %s"
          % (os.path.basename(POT_PATH),
             os.path.relpath(MO_PATH, HERE)))
    return 0 if not missing and not extra else 1


if __name__ == "__main__":
    sys.exit(main())
