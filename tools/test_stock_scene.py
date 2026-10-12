#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""股票场景人工逐帧验收推送（POST /api/v1/stock，验右下角三重额度环）。

这是干什么的
------------
往设备推**固定测试值**（不联网、不取新浪行情），让人能一帧一帧地肉眼验收股票场景：

  - 5 行行情行（固定名称/涨跌幅，name 都在设备端 9 字节可打印 ASCII 限制内）；
  - 右下角三重同心额度环（5H / WK. / MO.）——本脚本的重点。

圆环的几何是「**弧长 = 剩余百分比**」（像素算法现由共享的
``display/QuotaRingRenderer`` 提供，``StockScene::drawQuota()`` /
``UsageManager::drawQuotaRings()`` 两个调用点共用，含现算的
``tangentArcTenths()``，**只用于上界**）：
**亮弧 = 剩余额度**（占大半圈），暗轨道 = 已消耗掉的那段；剩余 100%（满额度）
画满整圈且不画帽，是唯一无帽态；剩余 0% 不画弧、只在 12 点画一个身份色实心圆点；
剩余 1..99 的弧角按 ``remaining*36`` **原始**计算、**只做上侧夹取**
（``tenths <= 3600 - Θ_min``，防 99% 缺口被两端帽补满而与整圈混淆；低百分比不再被
下界抬高）。端帽仅在两帽心弦长 ≥ 两帽直径 (``2*capR``) 时绘制 —— 更短的弧不画端帽、
按原始角度/弧长呈现，不被两个大圆盘融合撑宽（帽半径不缩放）。低于约 1px 的弧长仍会被
像素栅格量化，这是显示固有限制，与帽盘无关。
Θ_min 实测（0.1°，stock 小环几何）= ``{194, 288, 604}`` = 19.4° / 28.8° / 60.4°（外 / 中 / 内环）。

与 tools/push_auto.py 的关系
----------------------------
``tools/push_auto.py`` 是**生产调度脚本**：按开市/休市节拍自动联网取上游数据、
定时推送、夜间休眠。本脚本**只是人工逐帧验收工具**：不联网取数、不遵守任何时间
策略、推的是写死的测试序列，推完就退出（或 ``--loop`` 循环），给用户留出每帧
默认 12 秒的观察时间。它**复用 push_auto.py 的 HTTP 层**（``http_request`` /
``normalize_device_base``）以保证 Authorization 头与错误语义和生产一致。

用法示例::

    # 离线自检：只打印将要发送的 URL 与 payload，绝不发请求（真机验证前先跑这个）
    python3 tools/test_stock_scene.py --device http://192.168.1.50 \\
        --device-token <token> --dry-run

    # 真机逐帧推（每帧默认停 12 秒）
    python3 tools/test_stock_scene.py --device <host> --device-token <token>

    # 只推一帧、指定进度
    python3 tools/test_stock_scene.py --device <host> --device-token <token> \\
        --progress 50,,90 --once

    # 循环推预设序列
    python3 tools/test_stock_scene.py --device <host> --device-token <token> --loop

设备地址与 token 也可用环境变量 ``DEVICE`` / ``DEVICE_TOKEN`` 提供；两者缺任一即
报错退出（退出码 3），**即使 ``--dry-run`` 也要求提供**（与真机运行同一套参数校验）。

退出码：0 成功 / 2 设备失败或鉴权失败 / 3 参数错误。
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

# 复用 push_auto.py 的 HTTP 层（同目录、纯标准库、import 时不会跑 main）。
sys.path.insert(0, str(Path(__file__).resolve().parent))
from push_auto import http_request, normalize_device_base  # noqa: E402

# 设备端端点：POST 推数据、GET 查当前场景。
STOCK_PATH = "/api/v1/stock"
SCENE_PATH = "/api/v1/scene"

# 北京时间（UTC+8）偏移，用于帧头时间戳；不依赖本机时区。
BEIJING_TZ_OFFSET_SEC = 8 * 3600

# 5 行固定测试行情。name 必须 1..9 字节可打印 ASCII（固件无 CJK 字模，越界会 400）；
# change 是固定浮点，不联网。
FIXED_ROWS = (
    {"name": "T300", "change": 1.23},
    {"name": "T500", "change": -0.45},
    {"name": "T1K", "change": 0.08},
    {"name": "NIK", "change": -1.20},
    {"name": "NQ", "change": 0.67},
)

# 预设序列（与 test_balance_scene.py 共用同一套，便于左右对照）。每项是一个元组，
# 元素可为 None（对应设备端 null = 该环无数据）。语义是**剩余百分比**。
PRESETS = (
    (83, 89, 89),
    (100, 100, 100),
    (99, 99, 99),
    (50, 30, 10),
    (0, 0, 0),
)

# 预设帧对应的「预期观感」文案（股票场景：圆环亮弧 = 剩余百分比）。
PRESET_TEXT = {
    (83, 89, 89): "你最初报的那组：剩余 83/89/89% ⇒ 亮弧占大半圈"
                  "（外/中/内 = 299°/320°/320°），12 点处留暗缺口 61°/40°/40°，"
                  "外环缺口应比中/内环长出一截。",
    (100, 100, 100): "满额度：剩余 100% ⇒ 三个整圈、无圆头帽（唯一无帽态）。",
    (99, 99, 99): "刚消耗 1%：剩余 99%，缺口只有 1%（3.6°）比两个帽的间距还小 → "
                  "上侧夹到 3600−Θ_min，表现为满圈带一道约 1px 背景窄缝（帽边互不挨住），"
                  "不会与 100% 的整圈合并；0% 不是短弧而是 12 点单点，低值弧不再被下界抬高。",
    (50, 30, 10): "剩余 50/30/10% ⇒ 亮弧依次变短（180°/108°/36°），三环长度差别明显。",
    (0, 0, 0): "用尽：剩余 0% ⇒ **不画弧**，三个环只在 12 点各亮一个身份色实心圆点"
              "（直径 = 该环描边宽；外/中/内均为 4px 圆点），环带其余仍是暗轨道；"
              "不是弧、不是整圈、也没有第二个端帽。",
}


def eprint(*args):
    """错误/诊断信息走 stderr，与 push_auto.py 一致。"""
    print(*args, file=sys.stderr)


def now_hms():
    """北京时间 HH:MM:SS。"""
    return time.strftime("%H:%M:%S", time.gmtime(time.time() + BEIJING_TZ_OFFSET_SEC))


def format_progress(frame):
    """把帧格式化成 ``[83, 89, 89]`` / ``[50, null, 90]`` 这种打印形式。"""
    return "[" + ", ".join("null" if v is None else str(v) for v in frame) + "]"


def expected_text(frame):
    """本帧在股票场景下的预期观感（中文）。"""
    key = tuple(frame)
    if key in PRESET_TEXT:
        return PRESET_TEXT[key]
    parts = []
    for v in frame:
        if v is None:
            parts.append("无值")
        elif v == 0:
            parts.append("12点单点")
        elif v >= 100:
            parts.append("整圈")
        else:
            parts.append("亮弧 %d%%" % v)
    return ("自定义帧：剩余 %s ⇒ 圆环 %s（亮弧 = 剩余百分比；1..99 弧角原始计算、"
            "低值弧短于端帽直径时不画端帽、按原始角度呈现，0%% 是 12 点单点，"
            "100%% 整圈无帽）。"
            % (format_progress(frame), "、".join(parts)))


def parse_progress(value):
    """解析 ``--progress a,b,c``：1..3 项，每项为空（=null）或 0..100 整数。"""
    items = value.split(",")
    if len(items) < 1 or len(items) > 3:
        raise ValueError("--progress 需要 1..3 项，逗号分隔（如 50,,90）")
    frame = []
    for raw in items:
        token = raw.strip()
        if token == "":
            frame.append(None)  # 空项 = null（该环无数据）
            continue
        try:
            number = int(token, 10)
        except ValueError:
            raise ValueError("--progress 项必须为空或 0..100 整数，收到 %r" % raw)
        if number < 0 or number > 100:
            raise ValueError("--progress 项越界（须 0..100），收到 %d" % number)
        frame.append(number)
    return tuple(frame)


def build_payload(frame, no_rows):
    """构造 POST /api/v1/stock 的 body。

    带 ``rows`` 时设备会切到 stock 场景；``no_rows``（--no-rows）时只发 ``balance``，
    设备端语义是**只更新额度环、不碰行情行、也不接管屏幕**。
    """
    progress = [None if v is None else int(v) for v in frame]
    if no_rows:
        return {"balance": {"progress": progress}}
    rows = [{"name": row["name"], "change": row["change"]} for row in FIXED_ROWS]
    return {"rows": rows, "balance": {"progress": progress}}


def push_frame(endpoint, token, payload, timeout):
    """单帧推送：成功返回 None，失败抛出 RuntimeError（含退出码语义）。

    这里**不做重试**（与 push_auto.py 的 push_device 不同）：人工逐帧验收时快速失败
    比反复重试更合适，失败即中止。
    """
    try:
        code, raw = http_request(
            endpoint, method="POST",
            headers={"Authorization": "Bearer " + token},
            body=payload, timeout=timeout)
    except Exception as ex:
        raise RuntimeError("网络错误：%s" % ex)
    if code == 200:
        return
    if code in (401, 403):
        raise RuntimeError("设备鉴权失败 HTTP %d：检查 --device-token" % code)
    raise RuntimeError("设备返回 HTTP %d：%s" % (code, (raw or "")[:200]))


def confirm_scene(device_base, token, timeout):
    """推送后 GET /api/v1/scene，打印当前场景确认接管成功（失败只警告）。"""
    try:
        code, raw = http_request(
            device_base + SCENE_PATH,
            headers={"Authorization": "Bearer " + token},
            timeout=timeout)
        if code != 200:
            print("警告：确认场景失败 HTTP %d：%s" % (code, (raw or "")[:120]))
            return
        doc = json.loads(raw or "{}")
        current = doc.get("current", "?")
        print("当前场景：%s%s" % (current, "（接管成功）" if current == "stock" else ""))
    except Exception as ex:
        print("警告：确认场景失败：%s" % ex)


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="股票场景人工逐帧验收推送（POST /api/v1/stock，验右下角三重额度环）")
    device = parser.add_argument_group("设备")
    device.add_argument("--device", default=os.environ.get("DEVICE", ""),
                        help="设备地址（默认补 http://；也可用环境变量 DEVICE）")
    device.add_argument("--device-token",
                        default=os.environ.get("DEVICE_TOKEN", ""),
                        help="设备 Bearer token（也可用环境变量 DEVICE_TOKEN）")
    device.add_argument("--timeout", type=float, default=15.0,
                        help="单次 HTTP 超时秒数，默认 15")

    frame = parser.add_argument_group("帧")
    frame.add_argument("--hold", type=float, default=12.0,
                       help="每帧保持秒数，默认 12（必须 >= 0；0 = 不等直接连推）")
    frame.add_argument("--progress", default=None,
                       help="自定义一帧进度：1..3 项逗号分隔的 0..100 整数，"
                            "空项表示 null（如 50,,90）")
    frame.add_argument("--once", action="store_true",
                       help="只推一帧（配合 --progress；不给则用预设第一帧）")
    frame.add_argument("--loop", action="store_true",
                       help="循环推预设序列（默认推一遍就退出）")
    frame.add_argument("--no-rows", action="store_true",
                       help="只发 balance、不发 rows：只更新额度环，不接管屏幕")
    frame.add_argument("--dry-run", action="store_true",
                       help="只打印将要发送的 URL 与 payload，绝不发请求")
    return parser


def main(argv=None):
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    # 参数校验（退出码 3）：设备地址/token 缺任一即报错，--dry-run 也同样要求，
    # 保证离线自检与真机运行用的是同一套参数。
    device_base = normalize_device_base(args.device)
    if not device_base:
        eprint("参数错误：缺少 --device（或环境变量 DEVICE）")
        return 3
    if not args.device_token:
        eprint("参数错误：缺少 --device-token（或环境变量 DEVICE_TOKEN）")
        return 3
    if args.timeout <= 0:
        eprint("参数错误：--timeout 必须 > 0")
        return 3
    if args.hold < 0:
        eprint("参数错误：--hold 必须 >= 0")
        return 3

    # 组装帧序列：--progress 给定则单帧即该值，否则用预设序列；--once 截取第一帧。
    if args.progress is not None:
        try:
            frames = [parse_progress(args.progress)]
        except ValueError as ex:
            eprint("参数错误：%s" % ex)
            return 3
    else:
        frames = [tuple(frame) for frame in PRESETS]
    if args.once:
        frames = frames[:1]

    endpoint = device_base + STOCK_PATH
    print("股票场景逐帧验收：%d 帧，每帧保持 %.1fs%s"
          % (len(frames), args.hold, "（dry-run，不等待）" if args.dry_run else ""))
    if args.no_rows:
        print("--no-rows：只发 balance，只更新额度环、不碰行情行、不切换屏幕。")
    print("提示：若 d270 上 sd2-push.service 正在运行，其 15s 节拍会覆盖本测试"
          "画面；验收前请先 `sudo systemctl stop sd2-push.service`。")

    while True:
        for index, frame in enumerate(frames):
            is_last = index == len(frames) - 1
            print("[%s] 推送 progress=%s" % (now_hms(), format_progress(frame)))
            print("  本帧预期观感：%s" % expected_text(frame))

            payload = build_payload(frame, args.no_rows)
            if args.dry_run:
                print("[dry-run] POST %s" % endpoint)
                print(json.dumps(payload, ensure_ascii=False))
            else:
                try:
                    push_frame(endpoint, args.device_token, payload, args.timeout)
                except RuntimeError as ex:
                    eprint("设备失败：%s" % ex)
                    return 2

            # 保持本帧。dry-run 下没有真的上屏，不等待；默认一笔推完最后一帧就退出，
            # 不必为最后一帧空等。
            if not args.dry_run and args.hold > 0 and not (is_last and not args.loop):
                time.sleep(args.hold)

        if not args.loop:
            break

    if not args.dry_run:
        confirm_scene(device_base, args.device_token, args.timeout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
