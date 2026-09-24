#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""上位机股票行情推送脚本：读取新浪行情并推送到 GeekMagic 小屏。

设备端不主动访问行情源；本脚本在 PC 上请求 ``hq.sinajs.cn``，按设备契约
整理后 POST 到 ``/api/v1/stock``。新浪响应声明为 GB18030（GBK 的超集），
按 GB18030 解码；设备显示的是新浪代码（如 ``sh600519``），不推送中文名称。

设备契约（设备端固件实现，勿改）::

    POST http://<device>/api/v1/stock
    Authorization: Bearer <device 的 api_token>
    {"rows":[{"name":"sh600519","change":1.23}, ...]}  -> 200 {"ok":true}
    GET  http://<device>/api/v1/stock
      -> {"rows":[...],"ts":...,"age_s":...}

``--demo`` 用本地随机但合理的 5 组数据代替新浪行情，无需行情源即可测试
设备推送与屏幕渲染。仅依赖 Python 标准库。

``--loop`` 按北京时间判断 A 股交易时段：周一至周五 09:15–11:30、
13:00–15:00 视为开市。09:15 起包含集合竞价，因为该阶段新浪现价也会变化，
所以将它纳入开市窗口。节假日无法只靠星期推导，取数时读取个股行情日期
作为兜底：若行情日期不是北京时间当天，即使处在时间窗内也按休市处理；
指数没有日期字段，拿不到日期时只按时间窗判断。开市期间默认每 15 秒取数，
约等于每分钟 4 次，必要时可通过 ``--open-interval`` 调大；休市期间仍会
取数并推送，只是使用较长的 ``--interval``，避免屏幕数据长期不变。
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
BEIJING_TZ_OFFSET_SEC = 8 * 3600
# A 股交易时段；09:15 起包含集合竞价，指数与行情刷新仍可能发生变化。
MARKET_WINDOWS = ((9 * 3600 + 15 * 60, 11 * 3600 + 30 * 60),
                  (13 * 3600, 15 * 3600))
WEEKDAY_NAMES = ("周一", "周二", "周三", "周四", "周五", "周六", "周日")


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
                   help="休市期间循环推送间隔秒数（默认 300）")
    p.add_argument("--open-interval", type=int, default=15,
                   help="开市期间循环推送间隔秒数（默认 15，约每分钟 4 次）")
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


def beijing_time_now():
    """返回当前北京时间对应的 UTC 结构体，不依赖宿主机时区。"""
    return time.gmtime(time.time() + BEIJING_TZ_OFFSET_SEC)


def beijing_date(beijing_time_struct):
    return "%04d-%02d-%02d" % (beijing_time_struct.tm_year,
                               beijing_time_struct.tm_mon,
                               beijing_time_struct.tm_mday)


def is_market_open(beijing_time_struct, data_date=None, today=None):
    """按北京时间结构体判断 A 股是否开市。

    星期和交易时段是主判定；``data_date`` 是新浪个股行情日期，缺失时
    忽略它，有值且不是北京时间当天时视为节假日或旧行情并判定休市。
    该函数不读取系统时间，方便用固定结构体做纯函数测试。
    """
    if beijing_time_struct.tm_wday > 4:
        return False
    seconds = beijing_time_struct.tm_hour * 3600 + \
        beijing_time_struct.tm_min * 60 + beijing_time_struct.tm_sec
    in_window = any(start <= seconds <= end
                    for start, end in MARKET_WINDOWS)
    if not in_window:
        return False
    if not data_date:
        return True
    if today is None:
        today = beijing_date(beijing_time_struct)
    return data_date == today


def market_state_text(beijing_time_struct, market_open, data_date=None):
    clock = "%02d:%02d:%02d" % (
        beijing_time_struct.tm_hour, beijing_time_struct.tm_min,
        beijing_time_struct.tm_sec)
    weekday = WEEKDAY_NAMES[beijing_time_struct.tm_wday]
    if market_open:
        return "开市中（%s %s，北京）" % (beijing_date(beijing_time_struct),
                                           clock)
    if data_date and data_date != beijing_date(beijing_time_struct):
        return "休市（%s %s，北京，行情日期 %s）" % (weekday, clock, data_date)
    return "休市（%s %s，北京）" % (weekday, clock)


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


def extract_data_date(data_str):
    """从个股 data[30] 提取 YYYY-MM-DD；指数字段不足 31 个时返回 None。"""
    fields = (data_str or "").split(",")
    if len(fields) <= 30:
        return None
    value = fields[30].strip()
    if (len(value) != 10 or value[4] != "-" or value[7] != "-" or
            any(ch not in "0123456789" for ch in value[:4] + value[5:7] + value[8:])):
        return None
    return value


def parse_quote_line_parts(line):
    """解析行情行，返回 (code, 中文名, change, 个股行情日期)。"""
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
    data_date = extract_data_date(data_str)
    if parsed is None:
        return code, "", None, data_date
    return code, parsed[0], parsed[1], data_date


def parse_quote_line(line):
    """解析 ``var hq_str_sh600519="...";``，返回 (code, 中文名, change)。"""
    parsed = parse_quote_line_parts(line)
    if parsed is None:
        return None
    return parsed[:3]


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
    """请求新浪行情并返回 (设备 rows, 第一条可提供日期的个股行情日期)。"""
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
            data_date = None
            seen = set()
            for line in (raw or "").splitlines():
                parsed = parse_quote_line_parts(line)
                if parsed is None:
                    if line.strip():
                        eprint("跳过无法识别的新浪响应行: %s" % line[:120])
                    continue
                code_name, display_name, change, quote_date = parsed
                if code_name not in wanted or code_name in seen:
                    continue
                seen.add(code_name)
                # 只要个股提供合法日期就记录；即使该行价格因停牌被跳过，
                # 日期仍可用于判断当前是否处于节假日。
                if data_date is None and quote_date:
                    data_date = quote_date
                if not display_name or change is None:
                    eprint("跳过 %s：空行情或昨收/当前价无效" % code_name)
                    continue
                eprint("%s %s: %+.2f%%" % (code_name, display_name, change))
                rows.append({"name": code_name, "change": change})
            if rows:
                return rows, data_date
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
    if args.open_interval <= 0:
        return "", symbols, "--open-interval 必须 > 0"
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
    """完成一轮，返回 (退出码, 第一条可提供日期的个股行情日期)。"""
    if args.demo:
        return deliver_payload(args, build_demo_quotes(symbols)), None
    try:
        rows, data_date = fetch_quotes(args.sina_url, symbols, args.timeout,
                                       args.insecure)
    except RuntimeError as ex:
        eprint("行情失败: %s" % ex)
        return 1, None
    return deliver_payload(args, rows), data_date


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
        return run_once(args, symbols)[0]

    if args.demo:
        print("启动：demo 模式（跳过开市判定）→ 固定使用 --interval=%ds；"
              "开市间隔=%ds，休市间隔=%ds"
              % (args.interval, args.open_interval, args.interval))
    else:
        startup_time = beijing_time_now()
        startup_open = is_market_open(startup_time)
        print("启动：%s；开市间隔=%ds，休市间隔=%ds"
              % (market_state_text(startup_time, startup_open),
                 args.open_interval, args.interval))

    # 每轮重新取北京时间并结合本轮行情日期决定下一轮 sleep，不能在循环外
    # 固定间隔，否则跨过 09:15/11:30/13:00 后仍会沿用旧节奏。
    rc = 0
    while True:
        rc, data_date = run_once(args, symbols)
        if rc != 0:
            eprint("本轮失败 (rc=%d)，准备下一轮" % rc)
        if args.demo:
            wait = args.interval
            print("demo 模式（跳过开市判定）→ %ds 后下一轮" % wait)
        else:
            now = beijing_time_now()
            market_open = is_market_open(now, data_date=data_date)
            wait = args.open_interval if market_open else args.interval
            print("%s → %ds 后下一轮"
                  % (market_state_text(now, market_open, data_date), wait))
        try:
            time.sleep(wait)
        except KeyboardInterrupt:
            print("中断退出")
            return rc


if __name__ == "__main__":
    sys.exit(main())
