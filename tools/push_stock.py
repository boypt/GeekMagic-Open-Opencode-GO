#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""上位机股票行情推送脚本：读取新浪行情并推送到 GeekMagic 小屏。

设备端不主动访问行情源；本脚本在 PC 上请求 ``hq.sinajs.cn``，按设备契约
整理后 POST 到 ``/api/v1/stock``。行情响应是 GBK 编码，设备显示的标的是
新浪代码（如 ``sh600519``），不推送中文名称。

设备契约（设备端固件实现，勿改）::

    POST http://<device>/api/v1/stock
    Authorization: Bearer <device 的 api_token>
    {"rows":[{"name":"sh600519","change":1.23}, ...]}  -> 200 {"ok":true}
    GET  http://<device>/api/v1/stock
      -> {"rows":[...],"ts":...,"age_s":...}

``--demo`` 用本地随机但合理的 5 组数据代替新浪行情，无需行情源即可测试
设备推送与屏幕渲染。仅依赖 Python 标准库。
"""

import argparse
import json
import math
import os
import random
import socket
import ssl
import sys
import time
import urllib.error
import urllib.request
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP

DEFAULT_SINA_URL = "https://hq.sinajs.cn/list="
DEFAULT_SYMBOLS = "sh600519,sz000001,sh000001,sz399001,sh000300"
MAX_ROWS = 5
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
SINA_REFERER = "https://finance.sina.com.cn"


def eprint(*args):
    print(*args, file=sys.stderr)


def build_arg_parser():
    p = argparse.ArgumentParser(
        description="拉取新浪股票行情并推送最多 5 组到 GeekMagic 小屏")
    p.add_argument("--device", default=os.environ.get("DEVICE", ""),
                   help="设备地址：IP 或完整 URL（默认 http:// 前缀）。"
                        "也可经环境变量 DEVICE 传入")
    p.add_argument("--device-token", default=os.environ.get("DEVICE_TOKEN", ""),
                   help="设备 api_token（Bearer）。也可经 DEVICE_TOKEN 传入")
    p.add_argument("--sina-url",
                   default=os.environ.get("SINA_URL", DEFAULT_SINA_URL),
                   help="新浪行情 URL 前缀（默认 %s）。也可经环境变量 SINA_URL 传入"
                        % DEFAULT_SINA_URL)
    p.add_argument("--symbols", default=os.environ.get("SYMBOLS", DEFAULT_SYMBOLS),
                   help="逗号分隔的新浪标的，最多 5 个（默认 %s）。"
                        "也可经环境变量 SYMBOLS 传入" % DEFAULT_SYMBOLS)
    p.add_argument("--loop", action="store_true",
                   help="循环推送（默认单次推送后退出）")
    p.add_argument("--interval", type=int, default=300,
                   help="循环推送间隔秒数（默认 300）")
    p.add_argument("--insecure", action="store_true",
                   help="新浪 TLS 不校验证书（默认按标准系统证书库校验）")
    p.add_argument("--check", action="store_true",
                   help="GET 设备当前 stock 状态并打印，不拉行情、不推送")
    p.add_argument("--dry-run", action="store_true",
                   help="只打印 payload 不推送（仍会拉行情）")
    p.add_argument("--demo", action="store_true",
                   help="本地随机生成行情并推送（不拉新浪、无需行情源凭据）；"
                        "可与 --loop/--dry-run 组合")
    p.add_argument("--timeout", type=float, default=15.0,
                   help="单次 HTTP 超时秒数（默认 15）")
    return p


def normalize_device_base(device):
    """IP 或 URL 归一化为 http(s)://host[:port]（去掉尾部 /）。"""
    d = (device or "").strip()
    if not d:
        return ""
    if "://" not in d:
        d = "http://" + d
    return d.rstrip("/")


def parse_symbols(value):
    """解析标的列表并做设备 name 约束校验。"""
    symbols = []
    for item in str(value or "").split(","):
        symbol = item.strip().lower()
        if not symbol:
            raise ValueError("--symbols 含空的标的项")
        if len(symbol.encode("ascii", "ignore")) != len(symbol):
            raise ValueError("标的 %r 含非 ASCII 字符；设备仅支持可打印 ASCII"
                             % symbol)
        if not (1 <= len(symbol.encode("ascii")) <= 9):
            raise ValueError("标的 %r 长度必须为 1..9 字节" % symbol)
        if any(ord(ch) < 32 or ord(ch) > 126 for ch in symbol):
            raise ValueError("标的 %r 含不可打印 ASCII 字符" % symbol)
        symbols.append(symbol)
    if not symbols:
        raise ValueError("--symbols 不能为空")
    if len(symbols) > MAX_ROWS:
        raise ValueError("--symbols 收到 %d 个标的，设备最多 5 组；请减少标的"
                         % len(symbols))
    return symbols


def round_change(value):
    """按通常四舍五入规则把涨跌幅保留两位小数。"""
    if not math.isfinite(value):
        raise ValueError("涨跌幅不是有限数")
    rounded = Decimal(str(value)).quantize(Decimal("0.01"),
                                            rounding=ROUND_HALF_UP)
    result = float(rounded)
    return 0.0 if result == 0 else result


def parse_quote_data(code, data_str):
    """解析一条新浪数据，返回 (中文名称, 涨跌幅) 或 None。

    布局按字段数判断，不按 sh/sz 前缀判断：个股至少有 10 个字段，指数
    的第 4 个字段（索引 3）已经是百分比。脏数据在 fetch_quotes 中告警。
    """
    fields = (data_str or "").split(",")
    if not fields or not fields[0].strip():
        return None
    display_name = fields[0].strip()
    try:
        if len(fields) >= 10:
            # 个股：data[2]=昨收，data[3]=当前价。
            previous_close = float(fields[2].strip())
            current_price = float(fields[3].strip())
            if (not math.isfinite(previous_close) or
                    not math.isfinite(current_price) or
                    previous_close <= 0 or current_price <= 0):
                return None
            change = (current_price - previous_close) / previous_close * 100.0
        else:
            # 指数：data[1]=点位，data[2]=涨跌点数，data[3]=涨跌幅百分比。
            point = float(fields[1].strip())
            change = float(fields[3].strip())
            if not math.isfinite(point) or point <= 0:
                return None
        if not math.isfinite(change) or abs(change) > 100000:
            return None
        return display_name, round_change(change)
    except (IndexError, TypeError, ValueError, InvalidOperation, OverflowError):
        return None


def parse_quote_line(line):
    """解析 ``var hq_str_sh600519="...";``，返回 (code, 中文名, change)。"""
    line = (line or "").strip()
    prefix = "var hq_str_"
    if not line.startswith(prefix) or '"' not in line:
        return None
    body = line[len(prefix):]
    if "=" not in body or not body.endswith(";"):
        return None
    code, quoted = body.split("=", 1)
    if len(quoted) < 2 or not quoted.startswith('"') or not quoted.endswith('";'):
        return None
    data_str = quoted[1:-2]
    code = code.strip().lower()
    if not code:
        return None
    parsed = parse_quote_data(code, data_str)
    if parsed is None:
        return code, "", None
    return code, parsed[0], parsed[1]


def http_request(url, method="GET", headers=None, body=None, timeout=15.0,
                 insecure=False, encoding="utf-8", accept="application/json"):
    """统一 HTTP helper；新浪行情请求用 ``accept="*/*"`` + gb18030 解码。

    设备 API 是 JSON，强制 ``Accept: application/json`` 正确；新浪的
    ``hq.sinajs.cn`` 是 JS 文本端点，不要对它声明 JSON。
    """
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method)
    if accept:
        req.add_header("Accept", accept)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    ctx = None
    if insecure:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            raw = resp.read().decode(encoding, "replace")
            return resp.status, raw
    except urllib.error.HTTPError as ex:
        try:
            raw = ex.read().decode(encoding, "replace")
        except Exception:
            raw = ""
        return ex.code, raw
    except (urllib.error.URLError, socket.timeout, TimeoutError,
            ConnectionError, OSError) as ex:
        raise ex


def build_demo_quotes(symbols):
    """本地生成 1..MAX_ROWS 组合理的随机行情。"""
    rows = []
    for symbol in symbols:
        rows.append({"name": symbol,
                     "change": round_change(random.uniform(-5.0, 5.0))})
    return rows


def make_sina_url(base_url, symbols):
    """把 URL 前缀与标的拼成请求地址。

    ``--sina-url`` 语义是**前缀**（默认 ``https://hq.sinajs.cn/list=``）。若用户
    误粘了带 ``list=`` 的完整地址，则替换其中已有的标的 —— 早先的写法把「含
    list=」当作完整地址直接返回，导致默认前缀不带任何标的、请求空列表，新浪
    返回 HTTP 200 空 body（看起来像被风控，实则是脚本 bug）。
    """
    base_url = (base_url or "").strip()
    if not base_url:
        raise RuntimeError("新浪行情 URL 为空")
    head, marker, _tail = base_url.rpartition("list=")
    if marker:
        return head + marker + ",".join(symbols)
    return base_url + ",".join(symbols)


def fetch_quotes(sina_url, symbols, timeout, insecure, retries=2):
    """请求新浪行情并返回设备 rows；无任何有效行时抛 RuntimeError。"""
    url = make_sina_url(sina_url, symbols)
    headers = {"User-Agent": USER_AGENT, "Referer": SINA_REFERER}
    wanted = set(symbols)
    last_err = None
    for attempt in range(1, retries + 2):
        try:
            # 新浪响应声明 charset=GB18030（GBK 的超集），按 gb18030 解码更稳；
            # 行情端点是 JS 文本，不要声明 Accept: application/json。
            code, raw = http_request(url, headers=headers, timeout=timeout,
                                     insecure=insecure, encoding="gb18030",
                                     accept="*/*")
        except Exception as ex:
            last_err = "Network error: %s" % ex
            eprint("新浪请求失败 (attempt %d): %s" % (attempt, last_err))
            time.sleep(1)
            continue
        if code == 200:
            rows = []
            seen = set()
            for line in (raw or "").splitlines():
                parsed = parse_quote_line(line)
                if parsed is None:
                    if line.strip():
                        eprint("跳过无法识别的新浪响应行: %s" % line[:120])
                    continue
                code_name, display_name, change = parsed
                if code_name not in wanted or code_name in seen:
                    continue
                seen.add(code_name)
                if not display_name or change is None:
                    eprint("跳过 %s：空行情或昨收/当前价无效" % code_name)
                    continue
                eprint("%s %s: %+.2f%%" % (code_name, display_name, change))
                rows.append({"name": code_name, "change": change})
            if rows:
                return rows
            last_err = "Response error: 未解析到有效行情"
            eprint("新浪响应没有可用行情 (attempt %d): %s"
                   % (attempt, (raw or "")[:200]))
            time.sleep(1)
            continue
        if code == 403:
            last_err = "HTTP 403: 新浪拒绝请求（检查 Referer）"
            break
        last_err = "HTTP %d: %s" % (code, (raw or "")[:200])
        eprint("新浪 HTTP %d (attempt %d): %s" % (code, attempt, raw[:200]))
        time.sleep(1)
    raise RuntimeError(last_err or "新浪行情请求失败")


def push_device(device_base, token, rows, timeout, retries=2):
    """POST /api/v1/stock。成功返回 True；失败抛异常。"""
    url = device_base + "/api/v1/stock"
    headers = {"Authorization": "Bearer " + token}
    payload = {"rows": rows}
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
    url = device_base + "/api/v1/stock"
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
    """返回 (device_base, symbols, errmsg)。"""
    try:
        symbols = parse_symbols(args.symbols)
    except ValueError as ex:
        return "", [], str(ex)
    device_base = normalize_device_base(args.device)
    if args.interval <= 0:
        return "", symbols, "--interval 必须 > 0"
    if args.timeout <= 0:
        return "", symbols, "--timeout 必须 > 0"
    if not args.sina_url.strip():
        return "", symbols, "缺少 --sina-url（或环境变量 SINA_URL）"
    if args.check or not args.dry_run:
        if not device_base:
            return "", symbols, "缺少 --device（或环境变量 DEVICE）"
        if not args.device_token:
            return "", symbols, "缺少 --device-token（或环境变量 DEVICE_TOKEN）"
    return device_base, symbols, ""


def deliver_payload(args, rows):
    """dry-run 只打印 payload；否则推送到设备。返回 0 / 2。"""
    payload = {"rows": rows}
    if args.dry_run:
        print(json.dumps(payload, indent=2, ensure_ascii=False))
        return 0
    try:
        push_device(normalize_device_base(args.device), args.device_token,
                    rows, args.timeout)
    except RuntimeError as ex:
        eprint("设备失败: %s" % ex)
        return 2
    print("已推送: %s" % json.dumps(rows, ensure_ascii=False))
    return 0


def run_once(args, symbols):
    if args.demo:
        return deliver_payload(args, build_demo_quotes(symbols))
    try:
        rows = fetch_quotes(args.sina_url, symbols, args.timeout, args.insecure)
    except RuntimeError as ex:
        eprint("行情失败: %s" % ex)
        return 1
    return deliver_payload(args, rows)


def main(argv=None):
    args = build_arg_parser().parse_args(argv)
    device_base, symbols, err = validate_args(args)
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
    if not args.loop:
        return run_once(args, symbols)

    # 循环模式：单轮失败只报错不退出，下一轮继续
    rc = 0
    while True:
        rc = run_once(args, symbols)
        if rc != 0:
            eprint("本轮失败 (rc=%d)，%ds 后重试" % (rc, args.interval))
        try:
            time.sleep(args.interval)
        except KeyboardInterrupt:
            print("中断退出")
            return rc


if __name__ == "__main__":
    sys.exit(main())
