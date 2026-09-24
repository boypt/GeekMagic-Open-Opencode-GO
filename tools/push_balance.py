#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""上位机额度推送脚本：读上游 OpenCode Go 用量，格式化后推给 GeekMagic 小屏。

``--demo`` 用本地随机数据代替上游，无需上游凭据即可测试推送与屏幕渲染。

设备端不再自己 HTTPS 拉取，改为本脚本拉取上游、格式化成 3 行文本、
POST 到设备的 ``/api/v1/balance`` 接口显示。

契约（设备端固件实现，勿改名）::

    POST http://<device>/api/v1/balance
    Authorization: Bearer <device 的 api_token>
    {"labels":   ["5H", "WK.", "MO."],        # 可选：左上标签（null/缺省 → 固件默认 5H/WK./MO.）
     "progress": [58, null, 91],               # 可选：进度条填充 + 右上百分比（剩余%，0..100，null=空槽）
     "resets":   ["R2d4h", null, "R14d3h"],    # 可选：右下距重置相对时长（null/缺省 → "--"）
     "status": "可选状态行"}  -> 200 {"ok":true}
    GET  http://<device>/api/v1/balance
      -> {"labels":[...],"progress":[...],"resets":[...],"status":"...","ts":...,"age_s":...}

三段字段与行等长（1..3）；设备零语义照单渲染。整行 ``lines`` 通道已退役。

上游协议（移植自 include/opencodego/OpenCodeGoClient.h，只读参考）::

    GET https://<host><path>
    Authorization: Bearer <上游 key>
    x-opencode-session: push-balance-<hostname>
    -> {"usage": {"rolling": {"status","percent","resetsAt"},
                  "weekly":  {...}, "monthly": {...}}}
    rolling=5 小时窗口，weekly=周，monthly=月；percent=已用百分比，
    remaining = max(100 - percent, 0)（与 cc-switch 提取器一致）；
    status=="invalid" 视为无效窗口；resetsAt 为 ISO8601 UTC 时间。

三段渲染（设备零语义，分字段推送）：
``labels`` = 左上标签（``5H`` / ``WK.`` / ``MO.``）、``progress`` = 进度条
填充百分比（剩余额度 0..100，无数据为 null）、``resets`` = 右下距重置相对
时长（``R2d4h`` / ``R9h20m`` / ``R14d3h``，到期 ``Rnow``，无数据为 null）。
status 行沿用旧 ``UPDATE HH:MM`` 语义（推送成功时刻，本地时间）。

中文用法示例::

    # 单次推送（参数显式）
    python3 tools/push_balance.py --device http://192.168.1.10 \\
        --device-token DEV_TOKEN --upstream-host bwe.example.com \\
        --upstream-path /api/usage --upstream-key UPSTREAM_KEY

    # 循环推送（每 300 秒一轮，上游轮询节奏与旧固件一致默认 5 分钟）
    python3 tools/push_balance.py --device http://192.168.1.10 \\
        --device-token DEV_TOKEN --upstream-host bwe.example.com \\
        --upstream-path /api/usage --upstream-key UPSTREAM_KEY \\
        --loop --interval 300

    # 只打印 payload 不推送（格式化路径验证，可用假参数跑通）
    python3 tools/push_balance.py --upstream-host fake --upstream-path /x \\
        --upstream-key fake --device http://127.0.0.1 --device-token fake \\
        --dry-run

    # 查看设备当前 balance 状态（调试/自检，不拉上游）
    python3 tools/push_balance.py --device http://192.168.1.10 \\
        --device-token DEV_TOKEN --check

    # 本地随机测试数据直推（不拉上游、无需上游参数；加 --loop 可反复刷新）
    python3 tools/push_balance.py --device http://192.168.1.10 \\
        --device-token DEV_TOKEN --demo

退出码约定：0=成功 / 1=上游失败 / 2=设备失败 / 3=参数配置错误。
仅标准库（argparse/urllib/ssl/json/time/os/sys），零 pip 依赖。
"""

import argparse
import json
import os
import random
import socket
import ssl
import sys
import time
import urllib.error
import urllib.request

SESSION_PREFIX = "push-balance"
MAX_STATUS_LEN = 24
# 上游 resetsAt 是 UTC ISO8601；旧固件按 UTC+8 显示，这里同样 +8h。
TZ_OFFSET_SEC = 8 * 3600

# dry-run 时上游不可达则用此示例数据演示格式（仅演示，不推送）
SAMPLE_USAGE = {
    "rolling": {"status": "active", "percent": 24,
                "resetsAt": "2026-08-28T15:42:25.791Z"},
    "weekly": {"status": "active", "percent": 45,
               "resetsAt": "2026-09-01T00:00:00.000Z"},
    "monthly": {"status": "active", "percent": 9,
                "resetsAt": "2026-09-01T00:00:00.000Z"},
}


def build_demo_usage():
    """本地随机生成三窗口额度数据（--demo 用，不拉上游）。

    percent 取 0~100；重置时长按窗口量级取随机值（5H 10min~5h / 周 1h~7d /
    月 1d~30d），使三行长度在大字/小字档间浮动，便于肉眼检查自适应字号渲染。
    """
    from datetime import datetime, timedelta, timezone
    now = datetime.now(timezone.utc)

    def window(span_sec):
        return {
            "status": "active",
            "percent": random.randint(0, 100),
            "resetsAt": (now + timedelta(seconds=span_sec)).strftime(
                "%Y-%m-%dT%H:%M:%S.000Z"),
        }

    return {
        "rolling": window(random.randint(10 * 60, 5 * 3600)),
        "weekly": window(random.randint(3600, 7 * 86400)),
        "monthly": window(random.randint(86400, 30 * 86400)),
    }


def eprint(*args):
    print(*args, file=sys.stderr)


def build_arg_parser():
    p = argparse.ArgumentParser(
        description="拉取上游 OpenCode Go 用量并推送三行文本到 GeekMagic 小屏"
    )
    p.add_argument("--device", default=os.environ.get("DEVICE", ""),
                   help="设备地址：IP 或完整 URL（默认 http:// 前缀）。"
                        "也可经环境变量 DEVICE 传入")
    p.add_argument("--device-token", default=os.environ.get("DEVICE_TOKEN", ""),
                   help="设备 api_token（Bearer）。也可经 DEVICE_TOKEN 传入")
    p.add_argument("--upstream-host", default=os.environ.get("UPSTREAM_HOST", ""),
                   help="上游 host（不带 scheme，如 bwe.example.com）。"
                        "也可经 UPSTREAM_HOST 传入")
    p.add_argument("--upstream-path", default=os.environ.get("UPSTREAM_PATH", ""),
                   help="上游 path（以 / 开头，如 /api/usage）。"
                        "也可经 UPSTREAM_PATH 传入")
    p.add_argument("--upstream-key", default=os.environ.get("UPSTREAM_KEY", ""),
                   help="上游 API Key（Anthropic 兼容 Key）。"
                        "也可经 UPSTREAM_KEY 传入")
    p.add_argument("--loop", action="store_true",
                   help="循环推送（默认单次推送后退出）")
    p.add_argument("--interval", type=int, default=300,
                   help="循环推送间隔秒数（默认 300，与旧固件 5min 轮询一致）")
    p.add_argument("--insecure", action="store_true",
                   help="上游 TLS 不校验证书（默认用系统证书库校验）")
    p.add_argument("--ca-file", default="",
                   help="上游 TLS 自定义 CA 证书文件（PEM）")
    p.add_argument("--check", action="store_true",
                   help="GET 设备当前 balance 状态并打印，不拉上游、不推送")
    p.add_argument("--dry-run", action="store_true",
                   help="只打印 payload 不推送（仍会拉上游；上游不可达时用示例数据"
                        "演示格式，返回 0）")
    p.add_argument("--demo", action="store_true",
                   help="本地随机生成额度数据并推送（不拉上游、无需上游参数）；"
                        "可与 --loop/--dry-run 组合")
    p.add_argument("--timeout", type=float, default=15.0,
                   help="单次 HTTP 超时秒数（默认 15，与旧固件 FETCH_TIMEOUT_MS 一致）")
    return p


def normalize_device_base(device):
    """IP 或 URL 归一化为 http(s)://host[:port]（去掉尾部 /）。"""
    d = (device or "").strip()
    if not d:
        return ""
    if "://" not in d:
        d = "http://" + d
    return d.rstrip("/")


def format_reset(iso):
    """resetsAt ISO8601 UTC -> 距重置相对时长 'R2d4h'/'R9h20m'/'R12m'，到期回 'Rnow'，失败回 ''。"""
    if not iso:
        return ""
    try:
        s = iso.strip()
        # 容忍 "2026-08-28T15:42:25.791Z" / "+00:00" / 无后缀三种写法
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        from datetime import datetime, timezone
        dt = datetime.fromisoformat(s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        secs = int(dt.timestamp() - time.time())
        if secs <= 0:
            return "Rnow"
        d, rem = divmod(secs, 86400)
        h, rem = divmod(rem, 3600)
        m = rem // 60
        if d > 0:
            return "R%dd%dh" % (d, h)
        if h > 0:
            return "R%dh%02dm" % (h, m)
        return "R%dm" % max(m, 1)
    except Exception:
        return ""


def window_reset(win):
    """单个窗口 -> 距重置相对时长（如 'R2d4h'）；无有效数据返回 None（屏上显示 "--"）。"""
    if not isinstance(win, dict) or not win:
        return None
    if str(win.get("status", "")) == "invalid":
        return None
    return format_reset(str(win.get("resetsAt", "") or "")) or None


def window_progress(win):
    """单个窗口 -> 进度条填充值（0..100 剩余百分比）；无有效数据返回 None（空槽）。"""
    if not isinstance(win, dict) or not win:
        return None
    if str(win.get("status", "")) == "invalid":
        return None
    try:
        percent = int(win.get("percent", 0))
    except (TypeError, ValueError):
        return None
    return max(100 - percent, 0)


def build_payload(usage):
    """上游 usage dict -> (labels[3], progress[3], resets[3], status)。

    三段分别推送：左上标签 / 进度条填充百分比（剩余额度）/ 右下重置相对时长。
    """
    rolling = (usage or {}).get("rolling", {})
    weekly = (usage or {}).get("weekly", {})
    monthly = (usage or {}).get("monthly", {})
    labels = ["5H", "WK.", "MO."]
    progress = [
        window_progress(rolling),
        window_progress(weekly),
        window_progress(monthly),
    ]
    resets = [
        window_reset(rolling),
        window_reset(weekly),
        window_reset(monthly),
    ]
    lt = time.gmtime(time.time() + TZ_OFFSET_SEC)
    status = "UPDATE %02d:%02d" % (lt.tm_hour, lt.tm_min)
    return labels, progress, resets, status[:MAX_STATUS_LEN]


def http_request(url, method="GET", headers=None, body=None, timeout=15.0,
                 ssl_context=None):
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Accept", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout,
                                     context=ssl_context) as resp:
            raw = resp.read().decode("utf-8", "replace")
            return resp.status, raw
    except urllib.error.HTTPError as ex:
        try:
            raw = ex.read().decode("utf-8", "replace")
        except Exception:
            raw = ""
        return ex.code, raw
    except (urllib.error.URLError, socket.timeout, TimeoutError,
            ConnectionError, OSError) as ex:
        raise ex


def make_upstream_context(insecure, ca_file):
    if insecure:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx
    if ca_file:
        return ssl.create_default_context(cafile=ca_file)
    return None  # 系统默认证书库


def fetch_upstream(host, path, key, timeout, ssl_context, retries=2):
    """GET https://<host><path>，带简单重试。成功返回 usage dict；失败抛异常。"""
    if not path.startswith("/"):
        path = "/" + path
    url = "https://%s%s" % (host, path)
    try:
        hostname = socket.gethostname()
    except Exception:
        hostname = "host"
    headers = {
        "Authorization": "Bearer " + key,
        "User-Agent": "cc-switch/1.0",
        "x-opencode-session": "%s-%s" % (SESSION_PREFIX, hostname),
    }
    last_err = None
    for attempt in range(1, retries + 2):
        try:
            code, raw = http_request(url, headers=headers, timeout=timeout,
                                     ssl_context=ssl_context)
        except Exception as ex:
            last_err = "Network error: %s" % ex
            eprint("上游请求失败 (attempt %d): %s" % (attempt, last_err))
            time.sleep(1)
            continue
        if code == 200:
            try:
                doc = json.loads(raw or "{}")
            except Exception as ex:
                last_err = "Response error: %s" % ex
                eprint("上游 JSON 解析失败: %s" % last_err)
                time.sleep(1)
                continue
            usage = doc.get("usage") if isinstance(doc, dict) else None
            if not isinstance(usage, dict) or not any(
                    k in usage for k in ("rolling", "weekly", "monthly")):
                last_err = "Response error: missing usage.rolling/weekly/monthly"
                eprint("上游响应缺 usage 三窗口: %s" % raw[:200])
                time.sleep(1)
                continue
            return usage
        # 非 200：解析服务端 error.message（同固件 doc["error"]["message"]）
        msg = ""
        try:
            msg = (json.loads(raw or "{}").get("error") or {}).get("message", "")
        except Exception:
            pass
        if code == 401:
            last_err = "Bad API key%s" % (": " + msg if msg else "")
            break  # 重试无意义
        if code == 403:
            last_err = "No Go plan%s" % (": " + msg if msg else "")
            break  # 重试无意义
        last_err = "HTTP %d%s" % (code, (": " + msg) if msg else "")
        eprint("上游 HTTP %d (attempt %d): %s" % (code, attempt, msg or raw[:200]))
        time.sleep(1)
    raise RuntimeError(last_err or "上游请求失败")


def push_device(device_base, token, labels, progress, resets, status, timeout,
                retries=2):
    """POST /api/v1/balance。成功返回 True；失败抛异常。"""
    url = device_base + "/api/v1/balance"
    headers = {"Authorization": "Bearer " + token}
    payload = {"labels": labels, "progress": progress, "resets": resets,
               "status": status}
    last_err = None
    for attempt in range(1, retries + 2):
        try:
            code, raw = http_request(url, method="POST", headers=headers,
                                     body=payload, timeout=timeout)
        except Exception as ex:
            last_err = "Network error: %s" % ex
            eprint("设备推送失败 (attempt %d): %s" % (attempt, last_err))
            time.sleep(1)
            continue
        if code == 200:
            return True
        if code in (401, 403):
            last_err = "设备鉴权失败 HTTP %d: 检查 --device-token" % code
            break
        last_err = "HTTP %d: %s" % (code, (raw or "")[:200])
        eprint("设备推送 HTTP %d (attempt %d)" % (code, attempt))
        time.sleep(1)
    raise RuntimeError(last_err or "设备推送失败")


def check_device(device_base, token, timeout):
    url = device_base + "/api/v1/balance"
    headers = {"Authorization": "Bearer " + token}
    try:
        code, raw = http_request(url, headers=headers, timeout=timeout)
    except Exception as ex:
        raise RuntimeError("Network error: %s" % ex)
    if code != 200:
        raise RuntimeError("HTTP %d: %s" % (code, (raw or "")[:200]))
    try:
        doc = json.loads(raw or "{}")
    except Exception:
        doc = {"raw": raw}
    print(json.dumps(doc, indent=2, ensure_ascii=False))
    return True


def validate_args(args):
    """返回 (device_base, errmsg)。check 模式只需设备参数；其余缺一不可。"""
    device_base = normalize_device_base(args.device)
    if not device_base:
        return "", "缺少 --device（或环境变量 DEVICE）"
    if not args.device_token:
        return "", "缺少 --device-token（或环境变量 DEVICE_TOKEN）"
    if args.check:
        return device_base, ""
    if args.interval <= 0:
        return "", "--interval 必须 > 0"
    if args.timeout <= 0:
        return "", "--timeout 必须 > 0"
    if args.demo:
        return device_base, ""   # 随机数据直推，不需要上游参数
    if not args.upstream_host:
        return "", "缺少 --upstream-host（或环境变量 UPSTREAM_HOST）"
    if not args.upstream_path:
        return "", "缺少 --upstream-path（或环境变量 UPSTREAM_PATH）"
    if not args.upstream_key:
        return "", "缺少 --upstream-key（或环境变量 UPSTREAM_KEY）"
    return device_base, ""


def deliver_payload(args, labels, progress, resets, status, tag=""):
    """dry-run 只打印 payload；否则推送到设备。返回 0 / 2。"""
    payload = {"labels": labels, "progress": progress, "resets": resets,
               "status": status}
    if args.dry_run:
        print(json.dumps(payload, indent=2, ensure_ascii=False))
        return 0
    try:
        push_device(normalize_device_base(args.device), args.device_token,
                    labels, progress, resets, status, args.timeout)
    except RuntimeError as ex:
        eprint("设备失败: %s" % ex)
        return 2
    print("%s已推送: %s | %s | %s | %s" % (tag, labels, progress, resets,
                                            status))
    return 0


def run_once(args, ssl_context):
    if args.demo:
        labels, progress, resets, status = build_payload(build_demo_usage())
        status = "DEMO " + status.replace("UPDATE ", "")   # 屏上可辨认是随机数据
        return deliver_payload(args, labels, progress, resets, status,
                               tag="[demo] ")
    try:
        usage = fetch_upstream(args.upstream_host, args.upstream_path,
                               args.upstream_key, args.timeout, ssl_context)
    except RuntimeError as ex:
        if args.dry_run:
            # 演示模式：上游不可达时用示例数据走完格式化路径（假参数可验证）
            eprint("上游失败 (%s)，用示例数据演示格式" % ex)
            return deliver_payload(args, *build_payload(SAMPLE_USAGE))
        eprint("上游失败: %s" % ex)
        return 1
    return deliver_payload(args, *build_payload(usage))


def main(argv=None):
    args = build_arg_parser().parse_args(argv)
    device_base, err = validate_args(args)
    if err:
        eprint("参数错误: %s" % err)
        return 3
    if args.check:
        try:
            check_device(device_base, args.device_token, args.timeout)
            return 0
        except RuntimeError as ex:
            eprint("设备失败: %s" % ex)
            return 2
    if args.ca_file and not os.path.isfile(args.ca_file):
        eprint("参数错误: --ca-file 不存在: %s" % args.ca_file)
        return 3
    try:
        ssl_context = make_upstream_context(args.insecure, args.ca_file)
    except Exception as ex:
        eprint("参数错误: TLS 上下文创建失败: %s" % ex)
        return 3

    if not args.loop:
        return run_once(args, ssl_context)

    # 循环模式：单轮失败只报错不退出，下一轮继续
    rc = 0
    while True:
        rc = run_once(args, ssl_context)
        if rc != 0:
            eprint("本轮失败 (rc=%d)，%ds 后重试" % (rc, args.interval))
        try:
            time.sleep(args.interval)
        except KeyboardInterrupt:
            print("中断退出")
            return rc


if __name__ == "__main__":
    sys.exit(main())
