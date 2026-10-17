#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""内存 Dock 页面布局静态门禁（层 0，秒级，不需要 Qt、不需要构建）。

背景：
    MemoryDock 的 QTabWidget 会把所有页签（含隐藏的旧页）的最小高度取最大值，DDMA 页单页就把整个
    Dock 的最小高度顶到约 1200px；Dock 被 ADS 的外层 QScrollArea 包着时，被滚走的是整个 Dock。
    修法由三件事组成，三件事必须同时成立，缺一件就会比修之前更糟：
        1. 每个页签都自带内部滚动壳 ks::ui::EnablePageInnerScroll（工作台容器页除外）；
        2. 页签控件用 ks::ui::IsolateMinimumSize 加固，工具栏最小宽度被压低；
        3. MainWindow 把 "memory" 加入"去掉 ADS 外层滚动区"的名单，且两处挂载共用同一个判据函数。
    只做第 3 件而页面没包壳，超高页的底部会被静默裁掉。本脚本把这三件事的静态形状钉死。

用法：
    python tools/test_memory_dock_layout_gate.py              检查全部规则，违规时退出码 1
    python tools/test_memory_dock_layout_gate.py --list       只打印页面清单
    python tools/test_memory_dock_layout_gate.py --self-test  变异自检：对源码注入退化，门禁必须逐个抓到
    （纯标准库，Python 3.9 以上即可；--skip-registration 可跳过 vcxproj 登记检查）

退出码：0 全部通过；1 有违规或自检未抓到退化；2 脚本自身出错（找不到文件等）。
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ROOT：仓库根目录（本脚本位于 tools/ 下）。
ROOT = Path(__file__).resolve().parents[1]
# APP_DIR：主程序源码根目录，所有相对路径都以它为基准。
APP_DIR = ROOT / "Ksword5.1" / "Ksword5.1"

# HEADER_REL：header-only 滚动壳工具的相对路径。
HEADER_REL = "UI/AdaptivePageScroll.h"
# MAIN_WINDOW_REL：Dock 挂载判据所在文件。
MAIN_WINDOW_REL = "MainWindow.cpp"
# PROCESS_DETAIL_REL：内嵌 MemoryDock 的进程详情窗口。
PROCESS_DETAIL_REL = "ProcessDock/ProcessDetailWindow.BaseAndUi.cpp"
# UI_BUILD_REL：MemoryDock 根结构（工具栏、页签控件）所在文件。
UI_BUILD_REL = "MemoryDock/MemoryDock.UiBuild.cpp"

# DOCK_TAB_RECEIVER：MemoryDock 自己的 QTabWidget 成员名；只有挂在它上面的页才是"Dock 页签"。
DOCK_TAB_RECEIVER = "m_tabWidget"

# WORKBENCH_ALLOWLIST：不包壳的页面白名单及理由。工作台容器页自己填满页签栈，
# MemoryWorkbenchView::minimumSizeHint 恒为很小的值且根布局是 SetNoConstraint，贡献的最小高度约为 0，
# 包壳反而会让视图拿不到完整高度。新增白名单项必须写明理由。
WORKBENCH_ALLOWLIST: Dict[str, str] = {
    "m_tabWorkbench": "工作台容器页：视图自己填满、最小提示恒小，不需要也不应该包壳",
}

# REQUIRED_CLASS_PAGES：必须在自己的 .cpp 里用 EnablePageInnerScroll(this) 包壳的类页面。
# 前三个是 Dock 的页签，后两个嵌在系统内存审计页里（约 520 / 450px 高），包壳后审计页最小高度才降得下来。
REQUIRED_CLASS_PAGES: Tuple[str, ...] = (
    "SystemMemoryAuditPage",
    "DdmaPage",
    "TamperDetectionPage",
    "PhysicalPageAttributionPage",
    "HyperVMemoryPage",
)

# SUPPRESS_KEYS：判据函数里必须出现的 Dock 键。
SUPPRESS_KEYS: Tuple[str, ...] = ("network", "hardware", "kernel", "hvm", "memory")
# SUPPRESS_FUNCTION：MainWindow 里唯一的判据函数名。
SUPPRESS_FUNCTION = "DockSuppressesOuterScrollArea"
# MIN_MOUNT_SITES：使用该判据的挂载点个数下限（惰性占位页首次挂载 + 真实内容挂载）。
MIN_MOUNT_SITES = 2

# MAX_ROOT_MIN_EXTENT：页根部（页面本身、Dock 根、页签控件）允许的最小高度/最小尺寸上限。
MAX_ROOT_MIN_EXTENT = 100
# MAX_PROCESS_COMBO_MIN_WIDTH：进程下拉框最小宽度上限（改前 280，是工具栏最小宽度的最大贡献者）。
MAX_PROCESS_COMBO_MIN_WIDTH = 200

# Sources：相对 APP_DIR 的路径（正斜杠）到源码文本的映射。检查函数只读它，变异自检才能在内存里改。
Sources = Dict[str, str]


# ============================================================
# C++ 文本处理小工具
# ============================================================

@lru_cache(maxsize=128)
def scan_cpp(text: str, keep_strings: bool) -> str:
    """把 C++ 注释替换成空格；keep_strings 为假时再把字符串/字符字面量内部也替换成空格。

    长度与换行位置保持不变，所以在"遮蔽后的文本"上找到的下标可以直接用来切原文。
    结果按（文本, keep_strings）缓存：同一份源码会被几十条规则反复遮蔽，不缓存会慢到不可用。
    """
    # out：逐字符累积的结果片段。
    out: List[str] = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        following = text[index + 1] if index + 1 < length else ""

        # 行注释：一直遮蔽到行尾（保留换行本身）。
        if char == "/" and following == "/":
            end = text.find("\n", index)
            end = length if end < 0 else end
            out.append(" " * (end - index))
            index = end
            continue

        # 块注释：遮蔽内容，保留其中的换行。
        if char == "/" and following == "*":
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[index:end]))
            index = end
            continue

        # 原始字符串 R"delim( ... )delim"：整体当作字符串字面量处理。
        previous = text[index - 1] if index > 0 else ""
        if char == "R" and following == '"' and not (previous.isalnum() or previous == "_"):
            paren = text.find("(", index + 2)
            if paren > 0:
                terminator = ")" + text[index + 2:paren] + '"'
                end = text.find(terminator, paren)
                if end > 0:
                    end += len(terminator)
                    segment = text[index:end]
                    out.append(segment if keep_strings else re.sub(r"[^\n]", " ", segment))
                    index = end
                    continue

        # 普通字符串/字符字面量：处理转义，遇到换行视为未闭合并停下。
        if char in ('"', "'"):
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == char:
                    end += 1
                    break
                if text[end] == "\n":
                    break
                end += 1
            segment = text[index:end]
            if keep_strings or len(segment) < 2 or segment[-1] != char:
                out.append(segment)
            else:
                out.append(char + re.sub(r"[^\n]", " ", segment[1:-1]) + char)
            index = end
            continue

        out.append(char)
        index += 1
    return "".join(out)


def find_matching(masked: str, open_index: int, open_char: str, close_char: str) -> int:
    """在遮蔽文本里找与 open_index 处括号配对的右括号下标，找不到返回 -1。"""
    depth = 0
    for position in range(open_index, len(masked)):
        char = masked[position]
        if char == open_char:
            depth += 1
        elif char == close_char:
            depth -= 1
            if depth == 0:
                return position
    return -1


def split_top_level_args(masked: str, kept: str, open_paren: int) -> List[str]:
    """把 open_paren 处的调用实参按顶层逗号切开；结构用 masked 判定，内容从 kept 取。"""
    close = find_matching(masked, open_paren, "(", ")")
    if close < 0:
        return []
    arguments: List[str] = []
    depth = 0
    start = open_paren + 1
    for position in range(open_paren + 1, close):
        char = masked[position]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            arguments.append(kept[start:position].strip())
            start = position + 1
    tail = kept[start:close].strip()
    if tail or arguments:
        arguments.append(tail)
    return arguments


def extract_function_body(text: str, name_pattern: str) -> Optional[str]:
    """按名字找函数定义并返回花括号内部的原文（保留字符串，去掉注释）；找不到返回 None。"""
    masked = scan_cpp(text, False)
    kept = scan_cpp(text, True)
    for match in re.finditer(name_pattern + r"\s*\(", masked):
        open_paren = match.end() - 1
        close_paren = find_matching(masked, open_paren, "(", ")")
        if close_paren < 0:
            continue
        # 参数表之后允许 const / noexcept / override 等，直到第一个 '{'（遇到 ';' 说明是声明，换下一个）。
        brace = -1
        for position in range(close_paren + 1, len(masked)):
            if masked[position] == "{":
                brace = position
                break
            if masked[position] == ";":
                break
        if brace < 0:
            continue
        end = find_matching(masked, brace, "{", "}")
        if end > brace:
            return kept[brace + 1:end]
    return None


def normalize(text: str) -> str:
    """把连续空白折叠成单个空格，便于做"忽略排版"的子串比较。"""
    return re.sub(r"\s+", " ", text).strip()


# ============================================================
# 数据装载
# ============================================================

def load_sources(app_dir: Path) -> Sources:
    """读取门禁需要的全部源文件；缺少关键文件时抛 FileNotFoundError。"""
    sources: Sources = {}
    wanted: List[Path] = [
        app_dir / HEADER_REL,
        app_dir / MAIN_WINDOW_REL,
        app_dir / PROCESS_DETAIL_REL,
    ]
    wanted.extend(sorted((app_dir / "MemoryDock").glob("*.cpp")))
    for path in wanted:
        if not path.exists():
            raise FileNotFoundError(str(path))
        relative = path.relative_to(app_dir).as_posix()
        sources[relative] = path.read_text(encoding="utf-8-sig", errors="replace")
    return sources


def memory_dock_files(sources: Sources) -> List[str]:
    """返回 MemoryDock 目录下参与检查的 .cpp 相对路径。"""
    return sorted(path for path in sources if path.startswith("MemoryDock/") and path.endswith(".cpp"))


# ============================================================
# 检查 1：header-only 工具本身
# ============================================================

def check_header(sources: Sources) -> List[str]:
    """钉死 AdaptivePageScroll.h 的关键实现形状；任何一条退化都会让页面重新撑高 Dock。"""
    violations: List[str] = []
    text = sources.get(HEADER_REL)
    if text is None:
        return [f"{HEADER_REL}: 文件不存在"]
    code = scan_cpp(text, True)

    isolate_body = extract_function_body(text, r"\bIsolateMinimumSize")
    enable_body = extract_function_body(text, r"\bEnablePageInnerScroll")
    if isolate_body is None:
        violations.append(f"{HEADER_REL}: 找不到 IsolateMinimumSize 的定义")
    else:
        flat = normalize(isolate_body)
        if "setMinimumSize(0, 0)" not in flat:
            violations.append(f"{HEADER_REL}: IsolateMinimumSize 必须 setMinimumSize(0, 0)")
        if "QSizePolicy::Ignored, QSizePolicy::Ignored" not in flat:
            violations.append(
                f"{HEADER_REL}: IsolateMinimumSize 必须把两个方向都设成 QSizePolicy::Ignored"
                "（qSmartMinSize 只在 Ignored 时才把下限置 0）")

    if enable_body is None:
        violations.append(f"{HEADER_REL}: 找不到 EnablePageInnerScroll 的定义")
        return violations

    flat = normalize(enable_body)
    required_fragments = {
        "setWidgetResizable(true)": "壳必须 setWidgetResizable(true)，否则内容不会随视口铺满",
        "setFrameShape(QFrame::NoFrame)": "壳必须无边框",
        "IsolateMinimumSize(scrollArea)": "壳必须经 IsolateMinimumSize 隔离最小尺寸（Ignored）",
        "setFocusPolicy(Qt::NoFocus)": "壳不应抢键盘焦点",
    }
    for fragment, reason in required_fragments.items():
        if fragment not in flat:
            violations.append(f"{HEADER_REL}: EnablePageInnerScroll 缺少 {fragment}：{reason}")

    # 背景必须在 setWidget 之后关回去：QScrollArea::setWidget 会把内容容器的 autoFillBackground 置真。
    set_widget = flat.find("setWidget(")
    content_fill = flat.find("contentWidget->setAutoFillBackground(false)")
    viewport_fill = flat.find("viewport()->setAutoFillBackground(false)")
    if content_fill < 0:
        violations.append(f"{HEADER_REL}: 内容容器必须 setAutoFillBackground(false)（壁纸/毛玻璃模式会盖出实色块）")
    elif set_widget >= 0 and content_fill < set_widget:
        violations.append(f"{HEADER_REL}: 内容容器的 setAutoFillBackground(false) 必须在 setWidget 之后，否则被改回真")
    if viewport_fill < 0:
        violations.append(f"{HEADER_REL}: 壳的视口必须 setAutoFillBackground(false)")

    # 壳与内容容器不得带本地样式表：跟随全局样式与调色板。
    if "setStyleSheet(" in code:
        violations.append(f"{HEADER_REL}: 不得出现 setStyleSheet（壳与内容容器不设本地样式表）")

    # objectName 必须是蛇形小写常量，且字面量里不能有汉字（否则 i18n 审计要求补词条）。
    for constant, value in (
        ("kAdaptivePageScrollObjectName", "ks_adaptive_page_scroll"),
        ("kAdaptivePageContentObjectName", "ks_adaptive_page_content"),
    ):
        if not re.search(rf"{constant}\[\]\s*=\s*\"{value}\"", code):
            violations.append(f"{HEADER_REL}: 常量 {constant} 应为 \"{value}\"")
    for literal in re.findall(r'"((?:\\.|[^"\\])*)"', code):
        if re.search(r"[㐀-鿿]", literal):
            violations.append(f"{HEADER_REL}: 字符串字面量含汉字（会被 i18n 审计当成待翻译串）：{literal[:30]}")
    return violations


# ============================================================
# 检查 2：Dock 页签是否都包了壳
# ============================================================

@dataclass
class PageRecord:
    """一个被发现的页签及其判定结果，用于打印页面清单。"""

    file: str       # 出现 addTab/insertTab 的文件。
    receiver: str   # 接收页签的 QTabWidget 变量名。
    page: str       # 页面表达式。
    kind: str       # 构造形态：QWidget / 类名 / allowlist / unresolved。
    status: str     # OK / ALLOWLIST / VIOLATION / NESTED。
    detail: str     # 一句话说明。


# ADD_TAB_RE：匹配 xxx->addTab( / xxx.insertTab( 调用的起点。
ADD_TAB_RE = re.compile(r"\b(?P<receiver>[A-Za-z_]\w*)\s*(?:->|\.)\s*(?P<fn>addTab|insertTab)\s*\(")


def class_wrap_violations(class_name: str, sources: Sources) -> List[str]:
    """检查类页面的 .cpp 是否在构造路径里用 EnablePageInnerScroll(this) 包壳，且没有直接给 this 建布局。"""
    relative = f"MemoryDock/{class_name}.cpp"
    text = sources.get(relative)
    if text is None:
        return [f"{relative}: 找不到类页面 {class_name} 的实现文件"]
    masked = scan_cpp(text, False)
    problems: List[str] = []
    if not re.search(r"EnablePageInnerScroll\s*\(\s*this\s*\)", masked):
        problems.append(f"{relative}: 类页面 {class_name} 没有 EnablePageInnerScroll(this)，内容会把 Dock 撑高")
    if re.search(r"new\s+Q\w*Layout\s*\(\s*this\s*\)", masked):
        problems.append(
            f"{relative}: 类页面 {class_name} 仍有 new Q*Layout(this)：包壳后页面自己已有外壳布局，"
            "根布局必须建在 EnablePageInnerScroll 返回的内容容器上")
    return problems


def check_dock_pages(sources: Sources) -> Tuple[List[str], List[PageRecord]]:
    """遍历 MemoryDock 目录里所有 addTab/insertTab，要求 Dock 自己的页签都包了壳（白名单除外）。"""
    violations: List[str] = []
    records: List[PageRecord] = []
    seen_allowlist: set = set()
    files = memory_dock_files(sources)

    for relative in files:
        text = sources[relative]
        masked = scan_cpp(text, False)
        kept = scan_cpp(text, True)
        for match in ADD_TAB_RE.finditer(masked):
            receiver = match.group("receiver")
            arguments = split_top_level_args(masked, kept, match.end() - 1)
            page_index = 1 if match.group("fn") == "insertTab" else 0
            page = arguments[page_index] if len(arguments) > page_index else ""

            # 嵌套页签（类页面里的 m_detailTabs / m_tabs）不属于 Dock 页签，只登记，由类页面规则覆盖。
            if receiver != DOCK_TAB_RECEIVER:
                records.append(PageRecord(relative, receiver, page, "-", "NESTED", "嵌套页签，由类页面规则覆盖"))
                continue

            if not re.fullmatch(r"[A-Za-z_]\w*", page):
                message = f"{relative}: {receiver}->{match.group('fn')} 的页面实参 '{page}' 不是简单标识符，门禁无法解析，请先改成成员变量"
                violations.append(message)
                records.append(PageRecord(relative, receiver, page, "unresolved", "VIOLATION", "无法解析页面表达式"))
                continue

            # 白名单页面：登记并跳过。
            if page in WORKBENCH_ALLOWLIST:
                seen_allowlist.add(page)
                records.append(PageRecord(relative, receiver, page, "allowlist", "ALLOWLIST", WORKBENCH_ALLOWLIST[page]))
                continue

            # 找页面的构造语句：先在本文件，再在全部文件里找。
            construct = None
            for candidate in [relative] + [name for name in files if name != relative]:
                construct = re.search(
                    rf"\b{re.escape(page)}\s*=\s*new\s+(?P<cls>[A-Za-z_][\w:]*)\s*\(",
                    scan_cpp(sources[candidate], False))
                if construct is not None:
                    construct_file = candidate
                    break
            if construct is None:
                violations.append(f"{relative}: 找不到页面 {page} 的构造语句（page = new Xxx(...)），门禁无法判定是否包壳")
                records.append(PageRecord(relative, receiver, page, "unresolved", "VIOLATION", "找不到构造语句"))
                continue

            class_name = construct.group("cls").split("::")[-1]
            if class_name == "QWidget":
                # 普通容器页：必须 EnablePageInnerScroll(page)，且不能再给页面本身建布局。
                wrapped = re.search(
                    rf"EnablePageInnerScroll\s*\(\s*{re.escape(page)}\s*\)", scan_cpp(sources[construct_file], False))
                direct_layout = re.search(
                    rf"new\s+Q\w*Layout\s*\(\s*{re.escape(page)}\s*\)", scan_cpp(sources[construct_file], False))
                if wrapped is None:
                    violations.append(
                        f"{construct_file}: Dock 页面 {page} 没有 EnablePageInnerScroll({page})，"
                        "内容会把 Dock 撑高；确属自适应页面请加入 WORKBENCH_ALLOWLIST 并写明理由")
                    records.append(PageRecord(relative, receiver, page, "QWidget", "VIOLATION", "未包壳"))
                elif direct_layout is not None:
                    violations.append(
                        f"{construct_file}: Dock 页面 {page} 已包壳却仍有 new Q*Layout({page})，"
                        "根布局必须建在 EnablePageInnerScroll 返回的内容容器上")
                    records.append(PageRecord(relative, receiver, page, "QWidget", "VIOLATION", "布局建在页面上"))
                else:
                    records.append(PageRecord(relative, receiver, page, "QWidget", "OK", "已包壳"))
            else:
                # 类页面：包壳必须发生在类自己的 .cpp 里。
                problems = class_wrap_violations(class_name, sources)
                violations.extend(problems)
                records.append(PageRecord(
                    relative, receiver, page, class_name, "VIOLATION" if problems else "OK",
                    "类页面未包壳" if problems else "类页面构造里已包壳"))

    # 白名单里的项必须真的存在，防止白名单悄悄变成死代码、失去约束力。
    for name in WORKBENCH_ALLOWLIST:
        if name not in seen_allowlist:
            violations.append(f"白名单项 {name} 在 MemoryDock 源码里已找不到对应的 addTab/insertTab，请同步清理 WORKBENCH_ALLOWLIST")
    return violations, records


def check_class_pages(sources: Sources) -> List[str]:
    """五个类页面（含嵌在审计页里的两个）必须各自包壳，与它们是否被 addTab 发现无关。"""
    violations: List[str] = []
    for class_name in REQUIRED_CLASS_PAGES:
        violations.extend(class_wrap_violations(class_name, sources))
    return violations


# ============================================================
# 检查 3：MainWindow 的挂载判据
# ============================================================

def check_main_window(sources: Sources) -> List[str]:
    """判据函数唯一、含 memory，且两处挂载都调用它；memory 去外层滚动的前提是页面都已包壳。"""
    violations: List[str] = []
    text = sources.get(MAIN_WINDOW_REL)
    if text is None:
        return [f"{MAIN_WINDOW_REL}: 文件不存在"]
    masked = scan_cpp(text, False)

    definitions = re.findall(
        rf"\bbool\s+{SUPPRESS_FUNCTION}\s*\(\s*const\s+QString\s*&\s*\w+\s*\)\s*\{{", masked)
    if len(definitions) != 1:
        violations.append(f"{MAIN_WINDOW_REL}: {SUPPRESS_FUNCTION} 应恰有一处定义，实际 {len(definitions)} 处")

    body = extract_function_body(text, rf"\bbool\s+{SUPPRESS_FUNCTION}")
    keys: set = set()
    if body is None:
        violations.append(f"{MAIN_WINDOW_REL}: 找不到 {SUPPRESS_FUNCTION} 的函数体")
    else:
        keys = set(re.findall(r'QStringLiteral\("([^"]+)"\)', body))
        for required in SUPPRESS_KEYS:
            if required not in keys:
                violations.append(f"{MAIN_WINDOW_REL}: {SUPPRESS_FUNCTION} 缺少 \"{required}\"，该 Dock 会重新被 ADS 套上外层滚动区")

    # 每一处给 shouldSuppressOuterScrollArea 赋值的语句，右值必须恰好是对判据函数的调用，不得再写一份布尔表达式。
    assignments = re.findall(r"\bshouldSuppressOuterScrollArea\s*=\s*([^;]*);", masked)
    if len(assignments) < MIN_MOUNT_SITES:
        violations.append(
            f"{MAIN_WINDOW_REL}: shouldSuppressOuterScrollArea 的赋值只有 {len(assignments)} 处，"
            f"应至少 {MIN_MOUNT_SITES} 处（惰性占位页挂载 + 真实内容挂载）")
    for right_value in assignments:
        if normalize(right_value) != f"{SUPPRESS_FUNCTION}(dockKey)":
            violations.append(
                f"{MAIN_WINDOW_REL}: shouldSuppressOuterScrollArea 的右值必须是 {SUPPRESS_FUNCTION}(dockKey)，"
                f"实际 '{normalize(right_value)[:60]}'——判据复制了一份，下次漏改其中一份就会让两处挂载不一致")

    # ForceNoScrollArea 的三目条件必须是上面那个变量，不能在挂载处另写条件。
    ternaries = re.findall(
        r"(\w+)\s*\?\s*ads::CDockWidget::ForceNoScrollArea\s*:\s*ads::CDockWidget::AutoScrollArea", masked)
    if len(ternaries) < MIN_MOUNT_SITES:
        violations.append(f"{MAIN_WINDOW_REL}: ForceNoScrollArea/AutoScrollArea 三目只有 {len(ternaries)} 处，应至少 {MIN_MOUNT_SITES} 处")
    for condition in ternaries:
        if condition != "shouldSuppressOuterScrollArea":
            violations.append(f"{MAIN_WINDOW_REL}: 挂载三目的条件应是 shouldSuppressOuterScrollArea，实际 {condition}")

    # 顺序规则：先包壳、后去外层滚动。memory 在名单里时，页面不包壳就是静默裁掉底部。
    if "memory" in keys:
        page_violations, _ = check_dock_pages(sources)
        class_violations = check_class_pages(sources)
        if page_violations or class_violations:
            violations.append(
                f"{MAIN_WINDOW_REL}: \"memory\" 已加入去外层滚动名单，但仍有 {len(page_violations) + len(class_violations)} 个页面未包壳"
                "——超高页的底部会被静默裁掉。必须先包壳、后去外层滚动")
    return violations


# ============================================================
# 检查 4：Dock 外壳（页签控件加固、工具栏宽度、页根部最小高度）
# ============================================================

def check_dock_chrome(sources: Sources) -> List[str]:
    """页签控件必须隔离最小尺寸；工具栏最小宽度被压低；页根部不得再硬写大的最小高度。"""
    violations: List[str] = []
    text = sources.get(UI_BUILD_REL)
    if text is None:
        return [f"{UI_BUILD_REL}: 文件不存在"]
    masked = scan_cpp(text, False)

    # 页签控件加固：创建 -> IsolateMinimumSize -> 加入结果组布局，结果组再挂到根布局，顺序不能乱。
    created = masked.find("m_tabWidget = new QTabWidget(")
    isolated = masked.find("IsolateMinimumSize(m_tabWidget)")
    added_match = re.search(r"\b[A-Za-z_]\w*->addWidget\(m_tabWidget(?:\s*,\s*1)?\s*\)", masked)
    root_group_match = re.search(r"\bm_rootLayout->addWidget\(resultGroup(?:\s*,\s*1)?\s*\)", masked)
    added = added_match.start() if added_match else -1
    root_group_added = root_group_match.start() if root_group_match else -1
    if created < 0 or added < 0 or root_group_added < 0:
        violations.append(f"{UI_BUILD_REL}: 找不到 m_tabWidget 的创建、结果组挂载或根布局语句，门禁需要同步更新")
    elif isolated < 0:
        violations.append(
            f"{UI_BUILD_REL}: 缺少 ks::ui::IsolateMinimumSize(m_tabWidget)："
            "以后新增页签忘了包壳，会把 Dock 的头部/工具栏/状态栏挤坏而不是页内裁剪")
    elif not (created < isolated < added < root_group_added):
        violations.append(
            f"{UI_BUILD_REL}: IsolateMinimumSize(m_tabWidget) 必须位于创建之后、加入结果组之前，"
            "结果组再挂入 m_rootLayout")

    # 工具栏：进程下拉框最小宽度是工具栏横向下限的最大贡献者。
    combo = re.search(r"m_processCombo\s*->\s*setMinimumWidth\s*\(\s*(\d+)\s*\)", masked)
    if combo is None:
        violations.append(f"{UI_BUILD_REL}: 找不到 m_processCombo->setMinimumWidth(N)，门禁需要同步更新")
    elif int(combo.group(1)) > MAX_PROCESS_COMBO_MIN_WIDTH:
        violations.append(
            f"{UI_BUILD_REL}: m_processCombo 最小宽度 {combo.group(1)} > {MAX_PROCESS_COMBO_MIN_WIDTH}，"
            "Dock 不再套外层滚动区后，窄 Dock 的工具栏右侧会被直接裁掉")

    # 页根部（页面本身、Dock 根、页签控件）不得再硬写大的最小高度/最小尺寸。
    root_receiver = r"(?:this|m_tabWidget|m_tab[A-Z]\w*)"
    pattern = re.compile(
        rf"\b(?P<receiver>{root_receiver})\s*->\s*(?P<fn>setMinimumHeight|setFixedHeight|setMinimumSize|setFixedSize)"
        r"\s*\((?P<args>[^;]*?)\)\s*;")
    for relative in memory_dock_files(sources):
        file_masked = scan_cpp(sources[relative], False)
        for match in pattern.finditer(file_masked):
            numbers = [int(value) for value in re.findall(r"\d+", match.group("args"))]
            # *Height 的数字是高度；*Size 的第二个数字是高度。
            height = numbers[-1] if numbers else 0
            if height > MAX_ROOT_MIN_EXTENT:
                violations.append(
                    f"{relative}: {match.group('receiver')}->{match.group('fn')}({match.group('args').strip()}) "
                    f"在页根部硬写了 > {MAX_ROOT_MIN_EXTENT} 的最小高度，会重新撑高整个 Dock")
    return violations


# ============================================================
# 检查 5：进程详情窗口内嵌 MemoryDock 的既有保护
# ============================================================

def check_process_detail_embed(sources: Sources) -> List[str]:
    """内嵌 MemoryDock 必须保持 setMinimumSize(0, 0) + Ignored，包壳不能替代它（工具栏最小宽度仍要靠它挡）。"""
    text = sources.get(PROCESS_DETAIL_REL)
    if text is None:
        return [f"{PROCESS_DETAIL_REL}: 文件不存在"]
    body = extract_function_body(text, r"\battachEmbeddedDockToTabLayout")
    if body is None:
        return [f"{PROCESS_DETAIL_REL}: 找不到 attachEmbeddedDockToTabLayout 的函数体"]
    flat = normalize(body)
    violations: List[str] = []
    if "embeddedDockWidget->setMinimumSize(0, 0)" not in flat:
        violations.append(f"{PROCESS_DETAIL_REL}: ProcessDetailWindow 内嵌 Dock 缺少 setMinimumSize(0, 0)")
    if "embeddedDockWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored)" not in flat:
        violations.append(f"{PROCESS_DETAIL_REL}: ProcessDetailWindow 内嵌 Dock 缺少 Ignored 尺寸策略")
    return violations


# ============================================================
# 检查 6：工程登记（读文件，不走 Sources）
# ============================================================

def check_registration(app_dir: Path) -> List[str]:
    """新增的头文件必须登记到 .vcxproj 和 .vcxproj.filters（项目规范）。"""
    violations: List[str] = []
    expected = "UI\\AdaptivePageScroll.h"
    for name in ("Ksword5.1.vcxproj", "Ksword5.1.vcxproj.filters"):
        path = app_dir / name
        if not path.exists():
            violations.append(f"{name}: 文件不存在")
            continue
        content = path.read_text(encoding="utf-8-sig", errors="replace")
        if f'Include="{expected}"' not in content:
            violations.append(f"{name}: 尚未登记 {expected}（ClInclude）")
    return violations


# ============================================================
# 汇总与变异自检
# ============================================================

def run_source_checks(sources: Sources) -> List[str]:
    """只依赖 Sources 的全部检查，返回违规列表（变异自检复用它）。"""
    violations: List[str] = []
    violations.extend(check_header(sources))
    page_violations, _ = check_dock_pages(sources)
    violations.extend(page_violations)
    violations.extend(check_class_pages(sources))
    violations.extend(check_main_window(sources))
    violations.extend(check_dock_chrome(sources))
    violations.extend(check_process_detail_embed(sources))
    # 去重但保持顺序：同一个类页面会被"页签规则"和"类页面规则"各报一次。
    seen: set = set()
    unique: List[str] = []
    for violation in violations:
        if violation not in seen:
            seen.add(violation)
            unique.append(violation)
    return unique


@dataclass
class Mutation:
    """一次源码退化注入：在 path 里把第 occurrence 个 old 换成 new，门禁必须多报出含 expect 的违规。"""

    name: str          # 变异名，打印用。
    path: str          # 相对 APP_DIR 的文件。
    old: str           # 要替换的原文（必须真实存在，否则变异本身过期）。
    new: str           # 替换后的文本。
    expect: str        # 期望新增违规里包含的子串。
    occurrence: int = 0  # 替换第几个匹配（从 0 开始）。


# MUTATIONS：覆盖报告里"门禁必须抓住"的全部退化。anchor 都取源码里稳定的短片段。
MUTATIONS: List[Mutation] = [
    Mutation("DDMA 页去掉包壳", "MemoryDock/DdmaPage.cpp",
             "QVBoxLayout* rootLayout = new QVBoxLayout(content)",
             "QVBoxLayout* rootLayout = new QVBoxLayout(this)", "DdmaPage"),
    Mutation("系统内存审计页去掉包壳", "MemoryDock/SystemMemoryAuditPage.cpp",
             "new QVBoxLayout(ks::ui::EnablePageInnerScroll(this))", "new QVBoxLayout(this)", "SystemMemoryAuditPage"),
    Mutation("篡改检测页去掉包壳", "MemoryDock/TamperDetectionPage.cpp",
             "new QVBoxLayout(ks::ui::EnablePageInnerScroll(this))", "new QVBoxLayout(this)", "TamperDetectionPage"),
    Mutation("物理页归因页去掉包壳", "MemoryDock/PhysicalPageAttributionPage.cpp",
             "new QVBoxLayout(ks::ui::EnablePageInnerScroll(this))", "new QVBoxLayout(this)", "PhysicalPageAttributionPage"),
    Mutation("Hyper-V 页去掉包壳", "MemoryDock/HyperVMemoryPage.cpp",
             "new QVBoxLayout(ks::ui::EnablePageInnerScroll(this))", "new QVBoxLayout(this)", "HyperVMemoryPage"),
    Mutation("内存搜索页去掉包壳", UI_BUILD_REL,
             "ks::ui::EnablePageInnerScroll(m_tabSearch)", "m_tabSearch", "m_tabSearch"),
    Mutation("内核可执行页去掉包壳", "MemoryDock/MemoryDock.KernelExecutableMemory.cpp",
             "ks::ui::EnablePageInnerScroll(m_tabKernelExecutableMemory)", "m_tabKernelExecutableMemory",
             "m_tabKernelExecutableMemory"),
    Mutation("进程内存证据页去掉包壳", "MemoryDock/MemoryDock.ProcessMemoryEvidence.cpp",
             "ks::ui::EnablePageInnerScroll(m_tabProcessMemoryEvidence)", "m_tabProcessMemoryEvidence",
             "m_tabProcessMemoryEvidence"),
    Mutation("新增一个没包壳的页签", UI_BUILD_REL,
             'm_tabWidget->addTab(m_tabProcessModule, "进程与模块");',
             'm_tabWidget->addTab(m_tabProcessModule, "进程与模块");\n'
             '    m_tabExtra = new QWidget(m_tabWidget);\n'
             '    m_tabWidget->addTab(m_tabExtra, "extra");',
             "m_tabExtra"),
    Mutation("MainWindow 判据去掉 memory", MAIN_WINDOW_REL,
             '\n            || dockKey == QStringLiteral("memory");', ";", "memory"),
    Mutation("MainWindow 惰性占位页挂载改回内联判据", MAIN_WINDOW_REL,
             "const bool shouldSuppressOuterScrollArea = DockSuppressesOuterScrollArea(dockKey);",
             'const bool shouldSuppressOuterScrollArea = (dockKey == QStringLiteral("network"));',
             SUPPRESS_FUNCTION, 0),
    Mutation("MainWindow 真实内容挂载改回内联判据", MAIN_WINDOW_REL,
             "const bool shouldSuppressOuterScrollArea = DockSuppressesOuterScrollArea(dockKey);",
             'const bool shouldSuppressOuterScrollArea = (dockKey == QStringLiteral("network"));',
             SUPPRESS_FUNCTION, 1),
    Mutation("壳去掉 Ignored", HEADER_REL,
             "widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);",
             "widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);", "Ignored"),
    Mutation("壳改成 widgetResizable(false)", HEADER_REL,
             "scrollArea->setWidgetResizable(true);", "scrollArea->setWidgetResizable(false);", "setWidgetResizable"),
    Mutation("内容容器不关自填背景", HEADER_REL,
             "contentWidget->setAutoFillBackground(false);", "", "setAutoFillBackground"),
    Mutation("视口不关自填背景", HEADER_REL,
             "scrollArea->viewport()->setAutoFillBackground(false);", "", "视口"),
    Mutation("壳带本地样式表", HEADER_REL,
             "scrollArea->setFocusPolicy(Qt::NoFocus);",
             "scrollArea->setFocusPolicy(Qt::NoFocus);\n        scrollArea->setStyleSheet(QString());", "setStyleSheet"),
    Mutation("页签控件去掉 IsolateMinimumSize 加固", UI_BUILD_REL,
             "ks::ui::IsolateMinimumSize(m_tabWidget);", "", "IsolateMinimumSize(m_tabWidget)"),
    Mutation("进程下拉框最小宽度改回 280", UI_BUILD_REL,
             "m_processCombo->setMinimumWidth(160);", "m_processCombo->setMinimumWidth(280);", "m_processCombo"),
    Mutation("页根部硬写 500 的最小高度", UI_BUILD_REL,
             "m_tabWidget->setDocumentMode(true);",
             "m_tabWidget->setDocumentMode(true);\n    m_tabSearch->setMinimumHeight(500);", "setMinimumHeight"),
    Mutation("进程详情内嵌 Dock 去掉 Ignored", PROCESS_DETAIL_REL,
             "embeddedDockWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);", "", "ProcessDetailWindow"),
]


def apply_mutation(sources: Sources, mutation: Mutation) -> Sources:
    """返回注入退化后的 Sources 副本；锚点不存在时抛 ValueError（说明变异清单需要随源码更新）。"""
    original = sources.get(mutation.path)
    if original is None:
        raise ValueError(f"变异 '{mutation.name}'：{mutation.path} 不在已装载的源文件里")
    position = -1
    for _ in range(mutation.occurrence + 1):
        position = original.find(mutation.old, position + 1)
        if position < 0:
            raise ValueError(f"变异 '{mutation.name}'：在 {mutation.path} 里找不到第 {mutation.occurrence + 1} 处锚点：{mutation.old[:60]}")
    mutated = dict(sources)
    mutated[mutation.path] = original[:position] + mutation.new + original[position + len(mutation.old):]
    return mutated


def run_self_test(sources: Sources) -> int:
    """逐个注入退化，要求门禁新增含 expect 的违规；基线本身有违规时只对"新增"的违规计数。"""
    baseline = set(run_source_checks(sources))
    print(f"[self-test] 基线违规 {len(baseline)} 条（变异只看新增的违规）")
    failures = 0
    for mutation in MUTATIONS:
        try:
            mutated_sources = apply_mutation(sources, mutation)
        except ValueError as error:
            print(f"  ERROR  {error}")
            failures += 1
            continue
        added = [v for v in run_source_checks(mutated_sources) if v not in baseline]
        caught = [v for v in added if mutation.expect in v]
        if caught:
            print(f"  CAUGHT {mutation.name}")
        else:
            failures += 1
            print(f"  MISSED {mutation.name}（期望新增违规含 '{mutation.expect}'，实际新增 {len(added)} 条）")
    print(f"[self-test] {len(MUTATIONS) - failures}/{len(MUTATIONS)} 个退化被抓到")
    return 0 if failures == 0 else 1


def print_page_table(records: List[PageRecord]) -> None:
    """打印发现的 Dock 页签清单（嵌套页签只报个数），白名单外的新页一眼可见。"""
    print("发现的 Dock 页签：")
    nested = 0
    for record in records:
        if record.status == "NESTED":
            nested += 1
            continue
        print(f"  [{record.status:<9}] {record.receiver}->{record.page:<32} {record.kind:<28} {record.file}  {record.detail}")
    print(f"  （另有 {nested} 个嵌套页签在类页面内部，由类页面规则覆盖）")


def main() -> int:
    """入口：解析参数、装载源码、执行检查并打印结果。"""
    parser = argparse.ArgumentParser(description="内存 Dock 页面布局静态门禁")
    parser.add_argument("--list", action="store_true", help="只打印页面清单")
    parser.add_argument("--self-test", action="store_true", help="变异自检：注入退化，门禁必须逐个抓到")
    parser.add_argument("--skip-registration", action="store_true", help="跳过 vcxproj 登记检查")
    parser.add_argument("--app-dir", type=Path, default=APP_DIR, help="主程序源码根目录（默认仓库内路径）")
    arguments = parser.parse_args()

    try:
        sources = load_sources(arguments.app_dir)
    except FileNotFoundError as error:
        print(f"门禁无法读取源文件：{error}", file=sys.stderr)
        return 2

    if arguments.self_test:
        return run_self_test(sources)

    _, records = check_dock_pages(sources)
    print_page_table(records)
    if arguments.list:
        return 0

    violations = run_source_checks(sources)
    if not arguments.skip_registration:
        violations.extend(check_registration(arguments.app_dir))
    if violations:
        print(f"\nFAIL：{len(violations)} 条违规")
        for violation in violations:
            print(f"  - {violation}")
        return 1
    print("\nPASS：内存 Dock 布局门禁全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
