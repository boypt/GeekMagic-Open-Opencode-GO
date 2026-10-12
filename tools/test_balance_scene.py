#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""额度场景人工逐帧验收推送（POST /api/v1/balance，验额度页三行）。

这是干什么的
------------
往设备推**固定测试值**（不联网、不取上游用量），让人能一帧一帧地肉眼验收额度场景：
屏顶三行额度（标签 / 百分比进度条 / 重置日期）+ 状态行。

三行里的进度条是**剩余百分比**（琥珀金 ≥50 / 橙 ≥20 / 红 <20），股票场景右下角的
三重圆环**亮弧画的也是同一个剩余百分比**（暗轨道才是已消耗掉的那段）——两处读的是同
一个量，所以本脚本每帧的「预期观感」都会点明这层对应关系，方便和
``test_stock_scene.py`` 左右对照。股票右下角小环与本页居中大环的像素算法现由共享的
``display/QuotaRingRenderer`` 提供（同一套语义，仅几何 / 调色板不同）：圆环弧长按
``remaining*36`` 原始计算、**只做上侧夹取**（防 99% 缺口被端帽补满）；低值弧短于端帽
直径时不画端帽、按原始角度呈现，低百分比不再被下界抬高；0% 只画暗轨道（无亮点），
只有**正值且端点弦长 <1px** 时才退化为 12 点一个实心圆点 marker。

与 tools/push_auto.py 的关系
----------------------------
``tools/push_auto.py`` 是**生产调度脚本**：按开市/休市节拍自动联网取上游数据、
定时推送、夜间休眠。本脚本**只是人工逐帧验收工具**：不联网取数、不遵守任何时间
策略、推的是写死的测试序列，推完就退出（或 ``--loop`` 循环），给用户留出每帧
默认 12 秒的观察时间。它**复用 push_auto.py 的 HTTP 层**（``http_request`` /
``normalize_device_base``）以保证 Authorization 头与错误语义和生产一致。

用法示例::

    # 离线自检：只打印将要发送的 URL 与 payload，绝不发请求（真机验证前先跑这个）
    python3 tools/test_balance_scene.py --device http://192.168.1.50 \\
        --device-token <token> --dry-run

    # 真机逐帧推（每帧默认停 12 秒）
    python3 tools/test_balance_scene.py --device <host> --device-token <token>

    # 只推一帧、指定进度（空项 = null）
    python3 tools/test_balance_scene.py --device <host> --device-token <token> \\
        --progress 50,,90 --once

    # 循环推预设序列
    python3 tools/test_balance_scene.py --device <host> --device-token <token> --loop

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
BALANCE_PATH = "/api/v1/balance"
SCENE_PATH = "/api/v1/scene"

# 北京时间（UTC+8）偏移，用于帧头时间戳；不依赖本机时区。
BEIJING_TZ_OFFSET_SEC = 8 * 3600

# 三行固定标签 / 重置日期（设备端要求 labels/progress/resets 出现时彼此等长，
# 故按本帧 progress 的项数切片，保证长度一致）。
BALANCE_LABELS = ("5H", "WK.", "MO.")
BALANCE_RESETS = ("5H: 2h13m", "1W: 6d17h", "1M: 24d18h")
BALANCE_STATUS = "TEST"

# 预设序列（与 test_stock_scene.py 共用同一套，便于左右对照）。每项是一个元组，
# 元素可为 None（对应设备端 null = 该行无数据）。语义是**剩余百分比**。
PRESETS = (
    (83, 89, 89),
    (100, 100, 100),
    (99, 99, 99),
    (50, 30, 10),
    (0, 0, 0),
)

# 预设帧对应的「预期观感」文案（额度场景：三行进度条 = 剩余百分比）。
PRESET_TEXT = {
    (83, 89, 89): "你最初报的那组：三行剩余 83/89/89%，进度条都接近满；"
                  "股票页右下角圆环亮弧同样占大半圈、缺口 17/11/11%。",
    (100, 100, 100): "满额度：三行剩余 100%，进度条画满（股票页圆环也是整圈、无帽）。",
    (99, 99, 99): "刚消耗 1%：三行剩余 99%，进度条几乎满、只差一丝"
                  "（股票页圆环是满圈带一道约 1px 背景窄缝）。",
    (50, 30, 10): "三行剩余 50/30/10%：进度条长度依次变短，档位色阶"
                  "琥珀金(≥50) / 橙(≥20) / 红(<20) 依次出现。",
    (0, 0, 0): "用尽：三行剩余 0%，进度条空槽"
              "（股票页圆环三环全是暗轨道、完全没有亮点，与无值 -1 表现一致）。",
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
    """本帧在额度场景下的预期观感（中文）。"""
    key = tuple(frame)
    if key in PRESET_TEXT:
        return PRESET_TEXT[key]
    return ("自定义帧：三行剩余 %s 的百分比条（股票页圆环亮弧画的是同一个剩余百分比）。"
            % format_progress(frame))


def parse_progress(value):
    """解析 ``--progress a,b,c``：1..3 项，每项为空（=null）或 0..100 整数。"""
    items = value.split(",")
    if len(items) < 1 or len(items) > 3:
        raise ValueError("--progress 需要 1..3 项，逗号分隔（如 50,,90）")
    frame = []
    for raw in items:
        token = raw.strip()
        if token == "":
            frame.append(None)  # 空项 = null（该行无数据）
            continue
        try:
            number = int(token, 10)
        except ValueError:
            raise ValueError("--progress 项必须为空或 0..100 整数，收到 %r" % raw)
        if number < 0 or number > 100:
            raise ValueError("--progress 项越界（须 0..100），收到 %d" % number)
        frame.append(number)
    return tuple(frame)


def build_payload(frame):
    """构造 POST /api/v1/balance 的 body。

    设备端要求 labels/progress/resets 出现时**彼此等长**（1..3），所以按本帧
    progress 的项数切片，短帧也不会因为长度不一致被 400 掉。
    """
    progress = [None if v is None else int(v) for v in frame]
    count = len(progress)
    return {
        "labels": list(BALANCE_LABELS[:count]),
        "progress": progress,
        "resets": list(BALANCE_RESETS[:count]),
        "status": BALANCE_STATUS,
    }


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
        print("当前场景：%s%s" % (current, "（接管成功）" if current == "balance" else ""))
    except Exception as ex:
        print("警告：确认场景失败：%s" % ex)


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="额度场景人工逐帧验收推送（POST /api/v1/balance，验额度页三行）")
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

    endpoint = device_base + BALANCE_PATH
    print("额度场景逐帧验收：%d 帧，每帧保持 %.1fs%s"
          % (len(frames), args.hold, "（dry-run，不等待）" if args.dry_run else ""))
    print("提示：三行进度条与股票页右下角圆环的亮弧画的都是剩余百分比（暗轨道=已消耗）——"
          "可配合 test_stock_scene.py 左右对照。")
    print("提示：若 d270 上 sd2-push.service 正在运行，其 5min（休市）/ 15s（开市）"
          "节拍会覆盖本测试画面；验收前请先 `sudo systemctl stop sd2-push.service`。")

    while True:
        for index, frame in enumerate(frames):
            is_last = index == len(frames) - 1
            print("[%s] 推送 progress=%s" % (now_hms(), format_progress(frame)))
            print("  本帧预期观感：%s" % expected_text(frame))

            payload = build_payload(frame)
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
