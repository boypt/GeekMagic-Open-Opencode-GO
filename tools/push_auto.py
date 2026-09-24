#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""额度/股票自动推送与时间策略调度（同步生成器架构）。

调度层不使用 asyncio、线程、事件循环或 to_thread；网络调用保持同步，阻塞
urllib 调用只发生在 handler yield 的 Fetch*/Post* 任务中。

状态图::

    AWAKE_OPEN --额度到期--> BALANCE_WINDOW --窗口结束--> AWAKE_OPEN
         |                         |                         |
         +--行情过期/收盘-----------+--------------------------+--> AWAKE_CLOSED
         +--进入休眠--> SLEEPING <--唤醒---------------------+

每状态闹钟表::

    AWAKE_OPEN     Every(stock,300s), Every(balance_due,900s),
                   DailyAt(market_check,15:30), DailyAt(sleep_at,00:00)
    BALANCE_WINDOW Every(balance_refresh,300s), After(window_end,300s),
                   DailyAt(sleep_at,00:00)
    AWAKE_CLOSED   Every(balance,300s), DailyAt(market_check,09:20),
                   DailyAt(sleep_at,00:00)
    SLEEPING       Every(keepalive,600s), DailyAt(wake_at,08:00)

handler 可 yield 的任务与调度器回喂结果::

    FetchStock / FetchBalance  同步执行阻塞网络调用，回 Result(ok,data,error)
    PostDevice(path,body)      同步 POST 设备，回 Result
    PostSleep(on)              同步设置休眠，回 Result
    Log(msg)                   打印一行状态，回 None
    Wait(seconds)              挂起生成器并立即放掉调度器
    Register(alarm)            注册闹钟，回 None
    Unregister(name)           注销闹钟，回 None
    EnterState(event)          走显式转移表并按新状态换闹钟，回 None

如何新增一条时间策略：注册一个带 Every/DailyAt/After 规格和可选 guard 的
Alarm；如果它改变模式，再在 TRANSITIONS 增加明确的 state/event 转移并在
新状态 on_enter 注册对应闹钟；只改 handler 和注册表，不改主循环。

退出码：0=成功，1=上游失败，2=设备失败/鉴权失败，3=参数错误。
"""

import argparse
import json
import math
import os
import random
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
from enum import Enum

SESSION_PREFIX = "push-balance"
MAX_STATUS_LEN = 24
DEFAULT_UPSTREAM_URL = "https://opencode.ai/zen/go/v1/usage"
TZ_OFFSET_SEC = 8 * 3600
BEIJING_TZ_OFFSET_SEC = 8 * 3600
DEFAULT_SINA_URL = "https://hq.sinajs.cn/list="
DEFAULT_SYMBOLS = "sh600519,sz000001,sh000001,sz399001,sh000300"
MAX_ROWS = 5
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
SINA_REFERER = "https://finance.sina.com.cn"
MARKET_WINDOWS = ((9 * 3600 + 20 * 60, 15 * 3600 + 30 * 60),)
WEEKDAY_NAMES = ("周一", "周二", "周三", "周四", "周五", "周六", "周日")
SAMPLE_USAGE = {
    "rolling": {"status": "active", "percent": 24,
                "resetsAt": "2026-08-28T15:42:25.791Z"},
    "weekly": {"status": "active", "percent": 45,
               "resetsAt": "2026-09-01T00:00:00.000Z"},
    "monthly": {"status": "active", "percent": 9,
                "resetsAt": "2026-09-01T00:00:00.000Z"},
}


def eprint(*args):
    print(*args, file=sys.stderr)


def beijing_time_now():
    return time.gmtime(time.time() + BEIJING_TZ_OFFSET_SEC)


def beijing_time_at(epoch):
    return time.gmtime(epoch + BEIJING_TZ_OFFSET_SEC)


def beijing_date(value):
    return "%04d-%02d-%02d" % (value.tm_year, value.tm_mon, value.tm_mday)


def local_seconds(value):
    return value.tm_hour * 3600 + value.tm_min * 60 + value.tm_sec


def is_market_open(value, data_date=None, today=None):
    """星期 + 09:20~15:30 + 个股行情日期的外部开市判定。"""
    if value.tm_wday > 4:
        return False
    seconds = local_seconds(value)
    if not any(start <= seconds <= end for start, end in MARKET_WINDOWS):
        return False
    if not data_date:
        return True
    if today is None:
        today = beijing_date(value)
    return data_date == today


def parse_clock(value, option_name):
    try:
        hour, minute = [int(x) for x in str(value).split(":", 1)]
        if not (0 <= hour <= 23 and 0 <= minute <= 59):
            raise ValueError
        return hour * 3600 + minute * 60
    except (TypeError, ValueError):
        raise ValueError("%s 必须是 HH:MM 格式" % option_name)


def is_sleep_time(value, sleep_from, sleep_to):
    seconds = local_seconds(value)
    if sleep_from < sleep_to:
        return sleep_from <= seconds < sleep_to
    return seconds >= sleep_from or seconds < sleep_to


class State(str, Enum):
    AWAKE_OPEN = "AWAKE_OPEN"
    BALANCE_WINDOW = "BALANCE_WINDOW"
    AWAKE_CLOSED = "AWAKE_CLOSED"
    SLEEPING = "SLEEPING"


class Event(str, Enum):
    STOCK_TICK = "stock_tick"
    BALANCE_TICK = "balance_tick"
    BALANCE_DUE = "balance_due"
    WINDOW_END = "window_end"
    ENTER_SLEEP = "enter_sleep"
    SLEEP_KEEPALIVE = "sleep_keepalive"
    WAKE = "wake"
    MARKET_OPEN = "market_open"
    MARKET_CLOSED = "market_closed"
    STALE_DATE = "stale_date"
    UNKNOWN = "unknown"


STATE_ALARMS = {
    State.AWAKE_OPEN: ("stock", "balance_due", "market_check", "sleep_at"),
    State.BALANCE_WINDOW: ("balance_refresh", "window_end", "sleep_at"),
    State.AWAKE_CLOSED: ("balance", "market_check", "sleep_at"),
    State.SLEEPING: ("keepalive", "wake_at"),
}

TRANSITIONS = {
    (State.AWAKE_OPEN, Event.STOCK_TICK): State.AWAKE_OPEN,
    (State.AWAKE_OPEN, Event.BALANCE_DUE): State.BALANCE_WINDOW,
    (State.AWAKE_OPEN, Event.MARKET_CLOSED): State.AWAKE_CLOSED,
    (State.AWAKE_OPEN, Event.STALE_DATE): State.AWAKE_CLOSED,
    (State.AWAKE_OPEN, Event.ENTER_SLEEP): State.SLEEPING,
    (State.AWAKE_CLOSED, Event.BALANCE_TICK): State.AWAKE_CLOSED,
    (State.AWAKE_CLOSED, Event.MARKET_OPEN): State.AWAKE_OPEN,
    (State.AWAKE_CLOSED, Event.ENTER_SLEEP): State.SLEEPING,
    (State.BALANCE_WINDOW, Event.BALANCE_TICK): State.BALANCE_WINDOW,
    (State.BALANCE_WINDOW, Event.WINDOW_END): State.AWAKE_OPEN,
    (State.BALANCE_WINDOW, Event.ENTER_SLEEP): State.SLEEPING,
    (State.SLEEPING, Event.SLEEP_KEEPALIVE): State.SLEEPING,
    (State.SLEEPING, Event.WAKE): State.AWAKE_CLOSED,
}


class IllegalTransition(RuntimeError):
    pass


@dataclass(frozen=True)
class Every:
    seconds: int


@dataclass(frozen=True)
class DailyAt:
    at: str


@dataclass(frozen=True)
class After:
    seconds: int


@dataclass
class Alarm:
    name: str
    spec: object
    handler: object
    guard: object = None
    next_at: float = 0.0


@dataclass(frozen=True)
class Wait:
    seconds: float


@dataclass(frozen=True)
class Register:
    alarm: Alarm


@dataclass(frozen=True)
class Unregister:
    name: str


@dataclass(frozen=True)
class EnterState:
    event: Event


@dataclass(frozen=True)
class Log:
    message: str


@dataclass(frozen=True)
class FetchStock:
    symbols: object


@dataclass(frozen=True)
class FetchBalance:
    pass


@dataclass(frozen=True)
class PostDevice:
    path: str
    body: object


@dataclass(frozen=True)
class PostSleep:
    on: bool


@dataclass
class Result:
    ok: bool
    data: object = None
    error: str = ""
    code: int = 0


SUSPEND = object()


class Clock:
    def now(self):
        raise NotImplementedError

    def sleep_until(self, target):
        raise NotImplementedError


class RealClock(Clock):
    def __init__(self):
        self.wake = threading.Event()

    def now(self):
        return time.time()

    def sleep_until(self, target):
        self.wake.wait(max(0.0, target - self.now()))

    def stop(self):
        self.wake.set()


class VirtualClock(Clock):
    def __init__(self, start=0.0):
        self._now = float(start)
        self.sleep_requests = []

    def now(self):
        return self._now

    def sleep_until(self, target):
        seconds = max(0.0, float(target) - self._now)
        self.sleep_requests.append(seconds)
        self._now += seconds

    def advance(self, seconds):
        self._now += float(seconds)
        return self._now


def next_daily_epoch(now, at):
    hour, minute = [int(x) for x in at.split(":", 1)]
    value = beijing_time_at(now)
    target = now - local_seconds(value) + hour * 3600 + minute * 60
    if target <= now:
        target += 86400
    return target


class AlarmRegistry:
    def __init__(self):
        self._alarms = {}

    def register(self, alarm, now, first_at=None):
        if alarm.name in self._alarms:
            raise ValueError("闹钟已注册: %s" % alarm.name)
        if first_at is None:
            first_at = self._initial_time(alarm.spec, now)
        alarm.next_at = float(first_at)
        self._alarms[alarm.name] = alarm

    def unregister(self, name):
        self._alarms.pop(name, None)

    def unregister_state(self, state):
        prefix = state.value + ":"
        for name in list(self._alarms):
            if name.startswith(prefix):
                self.unregister(name)

    def clear(self):
        self._alarms.clear()

    @staticmethod
    def _initial_time(spec, now):
        if isinstance(spec, Every):
            return now + spec.seconds
        if isinstance(spec, After):
            return now + spec.seconds
        if isinstance(spec, DailyAt):
            return next_daily_epoch(now, spec.at)
        raise TypeError("未知闹钟规格: %r" % spec)

    def due(self, now, skip_log=None, context=None):
        result = []
        for name, alarm in list(self._alarms.items()):
            if alarm.next_at > now:
                continue
            if alarm.guard is not None and not alarm.guard(context):
                self._advance(alarm, now)
                continue
            missed = 0
            if isinstance(alarm.spec, Every):
                missed = max(0, int((now - alarm.next_at) // alarm.spec.seconds))
            if missed and skip_log is not None:
                skip_log("闹钟 %s 跳过 %d 个过期节拍" % (alarm.name, missed))
            self._advance(alarm, now)
            result.append(alarm)
        return result

    def skip_overdue(self, name, now, skip_log):
        alarm = self._alarms.get(name)
        if alarm is None or alarm.next_at > now or not isinstance(alarm.spec, Every):
            return
        missed = max(1, int((now - alarm.next_at) // alarm.spec.seconds) + 1)
        skip_log("闹钟 %s 跳过 %d 个过期节拍" % (name, missed))
        self._advance(alarm, now)

    def _advance(self, alarm, now):
        if isinstance(alarm.spec, Every):
            alarm.next_at += alarm.spec.seconds
            while alarm.next_at <= now:
                alarm.next_at += alarm.spec.seconds
        elif isinstance(alarm.spec, DailyAt):
            alarm.next_at = next_daily_epoch(now, alarm.spec.at)
        elif isinstance(alarm.spec, After):
            self.unregister(alarm.name)
        else:
            raise TypeError("未知闹钟规格: %r" % alarm.spec)

    def next_due(self, now=None):
        if not self._alarms:
            return None
        return min(alarm.next_at for alarm in self._alarms.values())

    def names(self):
        return sorted(self._alarms)


class Scheduler:
    def __init__(self, args, device_base, symbols, clock=None, backend=None,
                 quiet=False):
        self.args = args
        self.device_base = device_base
        self.symbols = symbols
        self.clock = clock or RealClock()
        self.backend = backend
        self.quiet = quiet
        self.registry = AlarmRegistry()
        self.suspended = {}
        self.state = None
        self.last_balance_at = None
        self.last_data_date = None
        self.probe_day = None
        self.key_warned = False
        self.rc = 0
        self.log_lines = []

    def now(self):
        return self.clock.now()

    def bj(self):
        return beijing_time_at(self.now())

    def clock_text(self):
        value = self.bj()
        return "%s %02d:%02d:%02d，北京" % (
            beijing_date(value), value.tm_hour, value.tm_min, value.tm_sec)

    def emit(self, message):
        self.log_lines.append(message)
        if not self.quiet:
            print(message)

    def sleeping_now(self):
        if self.args.no_sleep:
            return False
        start = parse_clock(self.args.sleep_from, "--sleep-from")
        end = parse_clock(self.args.sleep_to, "--sleep-to")
        return is_sleep_time(self.bj(), start, end)

    def market_provisional_open(self):
        return True if self.args.demo else is_market_open(self.bj())

    def market_open(self):
        if not self.market_provisional_open():
            return False
        today = beijing_date(self.bj())
        return (not self.last_data_date or self.last_data_date == today or
                self.probe_day != today)

    def register_state_alarms(self, state, initial=False):
        self.registry.unregister_state(state)
        now = self.now()
        if state == State.AWAKE_OPEN:
            self.registry.register(Alarm(
                "AWAKE_OPEN:stock", Every(self.args.open_interval),
                self.handle_stock), now, now if initial else now + self.args.open_interval)
            due = now + self.args.balance_every
            if self.last_balance_at is not None:
                due = self.last_balance_at + self.args.balance_every
            self.registry.register(Alarm(
                "AWAKE_OPEN:balance_due", Every(self.args.balance_every),
                self.handle_balance_due), now, max(now, due))
            self.registry.register(Alarm(
                "AWAKE_OPEN:market_check", DailyAt("15:30"),
                self.handle_market_check), now)
            if not self.args.no_sleep:
                self.registry.register(Alarm(
                    "AWAKE_OPEN:sleep_at", DailyAt(self.args.sleep_from),
                    self.handle_sleep_enter, guard=lambda ctx: self.sleeping_now()), now)
        elif state == State.AWAKE_CLOSED:
            first = now if initial or self.last_balance_at is None else \
                self.last_balance_at + self.args.closed_interval
            self.registry.register(Alarm(
                "AWAKE_CLOSED:balance", Every(self.args.closed_interval),
                self.handle_balance_due), now, max(now, first))
            self.registry.register(Alarm(
                "AWAKE_CLOSED:market_check", DailyAt("09:20"),
                self.handle_market_check), now)
            if not self.args.no_sleep:
                self.registry.register(Alarm(
                    "AWAKE_CLOSED:sleep_at", DailyAt(self.args.sleep_from),
                    self.handle_sleep_enter, guard=lambda ctx: self.sleeping_now()), now)
        elif state == State.BALANCE_WINDOW:
            self.registry.register(Alarm(
                "BALANCE_WINDOW:balance_refresh", Every(self.args.balance_refresh),
                self.handle_balance_refresh), now, now)
            self.registry.register(Alarm(
                "BALANCE_WINDOW:window_end", After(self.args.balance_window),
                self.handle_window_end), now)
            if not self.args.no_sleep:
                self.registry.register(Alarm(
                    "BALANCE_WINDOW:sleep_at", DailyAt(self.args.sleep_from),
                    self.handle_sleep_enter, guard=lambda ctx: self.sleeping_now()), now)
        elif state == State.SLEEPING:
            self.registry.register(Alarm(
                "SLEEPING:keepalive", Every(self.args.sleep_keepalive),
                self.handle_sleep_keepalive), now,
                now + self.args.sleep_keepalive)
            self.registry.register(Alarm(
                "SLEEPING:wake_at", DailyAt(self.args.sleep_to),
                self.handle_wake, guard=lambda ctx: not self.sleeping_now()), now)

    def transition(self, event):
        event = Event(event)
        try:
            target = TRANSITIONS[(self.state, event)]
        except KeyError as ex:
            raise IllegalTransition("非法状态转移: %s + %s" %
                                     (self.state.value, event.value)) from ex
        if target == self.state:
            return
        self.registry.unregister_state(self.state)
        self.state = target
        self.register_state_alarms(target)

    def start(self, force_state=None):
        if self.sleeping_now() and force_state is None:
            self.state = State.AWAKE_CLOSED
            self.register_state_alarms(self.state, initial=True)
            self.run_handler(self.handle_sleep_enter())
            return
        self.state = force_state or (
            State.AWAKE_OPEN if self.market_open() else State.AWAKE_CLOSED)
        self.register_state_alarms(self.state, initial=True)

    def register(self, alarm):
        self.registry.register(alarm, self.now())

    def unregister(self, name):
        self.registry.unregister(name)

    def suspend(self, gen, seconds):
        self.suspended[id(gen)] = (gen, self.now() + float(seconds))

    def run_handler(self, gen, result=None):
        try:
            while True:
                try:
                    task = gen.send(result)
                except StopIteration as stop:
                    self.suspended.pop(id(gen), None)
                    value = stop.value
                    if isinstance(value, Result):
                        self.rc = 0 if value.ok else value.code
                    elif isinstance(value, int):
                        self.rc = value
                    return value
                result = self.execute(task, gen)
                if result is SUSPEND:
                    return
        except IllegalTransition:
            raise
        except Exception as ex:
            self.emit("处理函数失败：%s" % ex)

    def execute(self, task, gen):
        if isinstance(task, Wait):
            self.suspend(gen, task.seconds)
            return SUSPEND
        if isinstance(task, Log):
            self.emit(task.message)
        elif isinstance(task, Register):
            self.register(task.alarm)
        elif isinstance(task, Unregister):
            self.unregister(task.name)
        elif isinstance(task, EnterState):
            self.transition(task.event)
        elif isinstance(task, FetchStock):
            return self.fetch_stock()
        elif isinstance(task, FetchBalance):
            return self.fetch_balance()
        elif isinstance(task, PostDevice):
            return self.post_device(task.path, task.body)
        elif isinstance(task, PostSleep):
            return self.post_sleep(task.on)
        else:
            raise TypeError("未知 handler 任务: %r" % (task,))
        return None

    def fetch_stock(self):
        if self.backend is not None:
            return self.backend.fetch_stock(self)
        try:
            if self.args.demo or self.args.dry_run:
                rows = [{"name": symbol,
                         "change": round_change(random.uniform(-5.0, 5.0))}
                        for symbol in self.symbols]
                return Result(True, (rows, None))
            rows, date = fetch_quotes(self.args.sina_url, self.symbols,
                                      self.args.timeout, self.args.insecure)
            return Result(True, (rows, date))
        except Exception as ex:
            return Result(False, error=str(ex), code=1)

    def fetch_balance(self):
        if self.backend is not None:
            return self.backend.fetch_balance(self)
        try:
            if not self.args.demo and not self.args.upstream_key:
                return Result(False, error="缺少 --upstream-key", code=1)
            usage = (build_demo_usage() if self.args.demo else
                     fetch_upstream(self.args.upstream_url, self.args.upstream_key,
                                    self.args.timeout, self.args.insecure))
            return Result(True, build_payload(usage))
        except Exception as ex:
            return Result(False, error=str(ex), code=1)

    def post_device(self, path, body):
        if self.backend is not None:
            return self.backend.post_device(self, path, body)
        try:
            if self.args.dry_run:
                show_dry_run(self.device_base + path, body)
            else:
                push_device(self.device_base + path, self.args.device_token,
                            body, self.args.timeout)
            return Result(True)
        except Exception as ex:
            return Result(False, error=str(ex), code=2)

    def post_sleep(self, on):
        if self.backend is not None:
            return self.backend.post_sleep(self, on)
        try:
            endpoint = self.device_base + "/api/v1/display/sleep"
            if self.args.dry_run:
                show_dry_run(endpoint, {"on": bool(on)})
            else:
                push_device(endpoint, self.args.device_token,
                            {"on": bool(on)}, self.args.timeout)
            return Result(True)
        except Exception as ex:
            return Result(False, error=str(ex), code=2)

    def balance_and_post(self):
        result = yield FetchBalance()
        self.last_balance_at = self.now()
        if not result.ok:
            if not self.key_warned:
                yield Log("额度未配置或上游失败：%s" % result.error)
                self.key_warned = True
            return result
        posted = yield PostDevice("/api/v1/balance", {
            "labels": result.data[0], "progress": result.data[1],
            "resets": result.data[2], "status": result.data[3]})
        yield Log("额度已刷新（%s），%ds 后刷新"
                  % (self.clock_text(), self.args.balance_refresh))
        return posted

    def handle_balance_due(self):
        if self.state == State.AWAKE_OPEN:
            yield EnterState(Event.BALANCE_DUE)
            return None
        yield EnterState(Event.BALANCE_TICK)
        return (yield from self.balance_and_post())

    def handle_balance_refresh(self):
        yield EnterState(Event.BALANCE_TICK)
        return (yield from self.balance_and_post())

    def stock_and_post(self, result):
        rows, date = result.data
        self.last_data_date = date
        if date and date != beijing_date(self.bj()):
            self.probe_day = beijing_date(self.bj())
            yield EnterState(Event.STALE_DATE)
            yield Log("行情日期 %s 不是今天，转休市节拍" % date)
            return None
        posted = yield PostDevice("/api/v1/stock", {"rows": rows})
        if posted.ok:
            prefix = "开市中" if self.state == State.AWAKE_OPEN else "休市中"
            yield Log("%s（%s）股票已推送，%ds 后下一轮"
                      % (prefix, self.clock_text(), self.args.open_interval))
        else:
            yield Log("股票设备推送失败：%s" % posted.error)
        return posted

    def handle_stock(self):
        if self.state == State.AWAKE_OPEN:
            yield EnterState(Event.STOCK_TICK)
        result = yield FetchStock(self.symbols)
        if not result.ok:
            yield Log("股票拉取失败：%s" % result.error)
            return result
        return (yield from self.stock_and_post(result))

    def handle_window_end(self):
        yield EnterState(Event.WINDOW_END)
        if not self.market_open():
            yield EnterState(Event.MARKET_CLOSED)
            return (yield from self.balance_and_post())
        result = yield FetchStock(self.symbols)
        if result.ok:
            return (yield from self.stock_and_post(result))
        yield Log("股票拉取失败：%s" % result.error)
        return result

    def handle_sleep_enter(self):
        if not self.sleeping_now():
            return
        result = yield PostSleep(True)
        if result.ok:
            yield EnterState(Event.ENTER_SLEEP)
            yield Log("休市中（%s），仅保留 keepalive" % self.clock_text())
        else:
            yield Log("休眠状态设置失败：%s" % result.error)
            return result
        return result

    def handle_sleep_keepalive(self):
        result = yield PostSleep(True)
        if result.ok:
            yield EnterState(Event.SLEEP_KEEPALIVE)
            yield Log("休市中（%s）重申 sleep，%ds 后再次检查"
                      % (self.clock_text(), self.args.sleep_keepalive))
        return result

    def handle_wake(self):
        result = yield PostSleep(False)
        if not result.ok:
            yield Log("唤醒失败：%s" % result.error)
            return
        yield EnterState(Event.WAKE)
        if self.market_open():
            yield EnterState(Event.MARKET_OPEN)
        return (yield from self.balance_and_post())

    def handle_market_check(self):
        if self.state == State.AWAKE_OPEN:
            if not self.market_open():
                yield EnterState(Event.MARKET_CLOSED)
                yield from self.balance_and_post()
            return
        if self.state != State.AWAKE_CLOSED or not self.market_provisional_open():
            return
        result = yield FetchStock(self.symbols)
        if not result.ok:
            yield Log("行情探针失败：%s" % result.error)
            return
        rows, date = result.data
        self.last_data_date = date
        if date and date != beijing_date(self.bj()):
            self.probe_day = beijing_date(self.bj())
            yield Log("行情日期 %s 不是今天，继续休市节拍" % date)
            return
        yield EnterState(Event.MARKET_OPEN)
        posted = yield PostDevice("/api/v1/stock", {"rows": rows})
        if posted.ok:
            prefix = "开市中" if self.state == State.AWAKE_OPEN else "休市中"
            yield Log("%s（%s）股票已推送，%ds 后下一轮"
                      % (prefix, self.clock_text(), self.args.open_interval))

    def _dispatch_due(self):
        count = 0
        for alarm in self.registry.due(self.now(), self.emit, self):
            count += 1
            self.run_handler(alarm.handler())
            self.registry.skip_overdue(alarm.name, self.now(), self.emit)
        return count

    def _resume_due(self):
        now = self.now()
        for key, (gen, target) in list(self.suspended.items()):
            if target <= now:
                del self.suspended[key]
                self.run_handler(gen)

    def run(self, max_events=None):
        dispatched = 0
        if max_events == 0:
            return self.rc
        while True:
            dispatched += self._dispatch_due()
            self._resume_due()
            if max_events is not None and dispatched >= max_events:
                break
            now = self.now()
            targets = [self.registry.next_due(now)]
            targets.extend(value[1] for value in self.suspended.values())
            targets = [value for value in targets if value is not None]
            if not targets:
                return self.rc
            target = min(targets)
            if target > now:
                self.clock.sleep_until(target)

        # 单轮模式在休市且没有额度 key 时，仍允许真实股票链路完成一次恢复；
        # 循环模式不会这样兜底，避免改变正常休市策略。
        if (max_events == 1 and not self.args.loop and not self.args.demo and
                not self.args.upstream_key and self.state == State.AWAKE_CLOSED and
                self.rc == 1):
            self.run_handler(self.handle_stock())
        return self.rc

def build_demo_usage():
    """本地随机生成额度窗口，便于不访问上游测试。"""
    from datetime import datetime, timedelta, timezone
    now = datetime.now(timezone.utc)

    def window(span_sec):
        return {
            "status": "active", "percent": random.randint(0, 100),
            "resetsAt": (now + timedelta(seconds=span_sec)).strftime(
                "%Y-%m-%dT%H:%M:%S.000Z"),
        }

    return {
        "rolling": window(random.randint(10 * 60, 5 * 3600)),
        "weekly": window(random.randint(3600, 7 * 86400)),
        "monthly": window(random.randint(86400, 30 * 86400)),
    }


def build_payload(usage):
    """usage dict -> 设备三段字段和状态行。"""
    rolling = (usage or {}).get("rolling", {})
    weekly = (usage or {}).get("weekly", {})
    monthly = (usage or {}).get("monthly", {})

    def progress(win):
        if not isinstance(win, dict) or str(win.get("status", "")) == "invalid":
            return None
        try:
            return max(100 - int(win.get("percent", 0)), 0)
        except (TypeError, ValueError):
            return None

    def reset(win):
        if not isinstance(win, dict) or str(win.get("status", "")) == "invalid":
            return None
        try:
            value = str(win.get("resetsAt", "") or "").strip()
            if value.endswith("Z"):
                value = value[:-1] + "+00:00"
            from datetime import datetime, timezone
            dt = datetime.fromisoformat(value)
            if dt.tzinfo is None:
                dt = dt.replace(tzinfo=timezone.utc)
            seconds = int(dt.timestamp() - time.time())
            if seconds <= 0:
                return "Rnow"
            days, rest = divmod(seconds, 86400)
            hours, rest = divmod(rest, 3600)
            minutes = rest // 60
            if days:
                return "R%dd%dh" % (days, hours)
            if hours:
                return "R%dh%02dm" % (hours, minutes)
            return "R%dm" % max(minutes, 1)
        except Exception:
            return None

    now = beijing_time_now()
    status = "UPDATE %02d:%02d" % (now.tm_hour, now.tm_min)
    return (["5H", "WK.", "MO."], [progress(rolling), progress(weekly),
                                  progress(monthly)],
            [reset(rolling), reset(weekly), reset(monthly)],
            status[:MAX_STATUS_LEN])


def http_request(url, method="GET", headers=None, body=None, timeout=15.0,
                 insecure=False, encoding="utf-8", accept="application/json"):
    """统一 HTTP helper；新浪请求必须使用 */* 和 gb18030。"""
    data = None
    if body is not None:
        data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method)
    if accept:
        req.add_header("Accept", accept)
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    context = None
    if insecure:
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=context) as resp:
            return resp.status, resp.read().decode(encoding, "replace")
    except urllib.error.HTTPError as ex:
        try:
            return ex.code, ex.read().decode(encoding, "replace")
        except Exception:
            return ex.code, ""
    except (urllib.error.URLError, socket.timeout, TimeoutError,
            ConnectionError, OSError):
        raise


def fetch_upstream(url, key, timeout, insecure, retries=2):
    """读取 OpenCode Go usage，失败抛出 RuntimeError。"""
    url = (url or "").strip()
    if not url:
        raise RuntimeError("上游 URL 为空")
    if "://" not in url:
        raise RuntimeError("上游 URL 需带 scheme，如 https://host/path")
    if url.endswith("/"):
        url = url[:-1]
    try:
        hostname = socket.gethostname()
    except Exception:
        hostname = "host"
    headers = {
        "Authorization": "Bearer " + key,
        "User-Agent": "cc-switch/1.0",
        "x-opencode-session": "%s-%s" % (SESSION_PREFIX, hostname),
    }
    last_error = None
    for attempt in range(1, retries + 2):
        try:
            code, raw = http_request(url, headers=headers, timeout=timeout,
                                     insecure=insecure)
        except Exception as ex:
            last_error = "Network error: %s" % ex
            eprint("上游请求失败 (attempt %d): %s" % (attempt, last_error))
            time.sleep(1)
            continue
        if code == 200:
            try:
                doc = json.loads(raw or "{}")
                usage = doc.get("usage") if isinstance(doc, dict) else None
                if not isinstance(usage, dict) or not any(
                        key_name in usage for key_name in
                        ("rolling", "weekly", "monthly")):
                    raise ValueError("missing usage.rolling/weekly/monthly")
                return usage
            except (ValueError, TypeError) as ex:
                last_error = "Response error: %s" % ex
                eprint("上游响应错误: %s" % last_error)
                time.sleep(1)
                continue
        last_error = "HTTP %d: %s" % (code, (raw or "")[:200])
        eprint("上游 HTTP %d (attempt %d)" % (code, attempt))
        time.sleep(1)
    raise RuntimeError(last_error or "上游请求失败")


def parse_symbols(value):
    """解析标的并验证设备端 ASCII、长度和数量约束。"""
    symbols = []
    for item in str(value or "").split(","):
        symbol = item.strip().lower()
        if not symbol:
            raise ValueError("--symbols 含空的标的项")
        if len(symbol.encode("ascii", "ignore")) != len(symbol):
            raise ValueError("标的 %r 含非 ASCII 字符" % symbol)
        if not 1 <= len(symbol.encode("ascii")) <= 9:
            raise ValueError("标的 %r 长度必须为 1..9 字节" % symbol)
        if any(ord(ch) < 32 or ord(ch) > 126 for ch in symbol):
            raise ValueError("标的 %r 含不可打印 ASCII" % symbol)
        symbols.append(symbol)
    if not symbols:
        raise ValueError("--symbols 不能为空")
    if len(symbols) > MAX_ROWS:
        raise ValueError("--symbols 最多 5 个标的")
    return symbols


def make_sina_url(base_url, symbols):
    """用 rpartition 替换已有 list= 后的标的，避免请求成空列表。"""
    base_url = (base_url or "").strip()
    if not base_url:
        raise RuntimeError("新浪行情 URL 为空")
    head, marker, _tail = base_url.rpartition("list=")
    if marker:
        return head + marker + ",".join(symbols)
    return base_url + ",".join(symbols)


def round_change(value):
    if not math.isfinite(value):
        raise ValueError("涨跌幅不是有限数")
    result = float(Decimal(str(value)).quantize(
        Decimal("0.01"), rounding=ROUND_HALF_UP))
    return 0.0 if result == 0 else result


def parse_quote_data(code, data_str):
    """按字段数解析个股/指数；不按 sh/sz 前缀猜测。"""
    fields = (data_str or "").split(",")
    if not fields or not fields[0].strip():
        return None
    display_name = fields[0].strip()
    try:
        if len(fields) >= 10:
            previous_close = float(fields[2].strip())  # 个股 data[2]
            current_price = float(fields[3].strip())    # 个股 data[3]
            if (not math.isfinite(previous_close) or
                    not math.isfinite(current_price) or previous_close <= 0 or
                    current_price <= 0):
                return None
            change = (current_price - previous_close) / previous_close * 100.0
        else:
            point = float(fields[1].strip())
            change = float(fields[3].strip())           # 指数 data[3] 已是百分比
            if not math.isfinite(point) or point <= 0:
                return None
        if not math.isfinite(change) or abs(change) > 100000:
            return None
        return display_name, round_change(change)
    except (IndexError, TypeError, ValueError, InvalidOperation, OverflowError):
        return None


def extract_data_date(data_str):
    fields = (data_str or "").split(",")
    if len(fields) <= 30:
        return None
    value = fields[30].strip()
    if (len(value) != 10 or value[4] != "-" or value[7] != "-" or
            any(ch not in "0123456789" for ch in value[:4] + value[5:7] + value[8:])):
        return None
    return value


def parse_quote_line_parts(line):
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
    return code, parsed[0] if parsed else "", parsed[1] if parsed else None, \
        extract_data_date(data_str)


def fetch_quotes(sina_url, symbols, timeout, insecure, retries=2):
    """请求新浪；必须使用 Referer、*/* Accept 和 gb18030 解码。"""
    url = make_sina_url(sina_url, symbols)
    headers = {"User-Agent": USER_AGENT, "Referer": SINA_REFERER}
    wanted = set(symbols)
    last_error = None
    for attempt in range(1, retries + 2):
        try:
            code, raw = http_request(url, headers=headers, timeout=timeout,
                                     insecure=insecure, encoding="gb18030",
                                     accept="*/*")
        except Exception as ex:
            last_error = "Network error: %s" % ex
            eprint("新浪请求失败 (attempt %d): %s" % (attempt, last_error))
            time.sleep(1)
            continue
        if code == 200:
            rows, data_date, seen = [], None, set()
            for line in (raw or "").splitlines():
                parsed = parse_quote_line_parts(line)
                if parsed is None:
                    continue
                code_name, display_name, change, quote_date = parsed
                if code_name not in wanted or code_name in seen:
                    continue
                seen.add(code_name)
                if data_date is None and quote_date:
                    data_date = quote_date
                if not display_name or change is None:
                    eprint("跳过 %s：空行情或价格无效" % code_name)
                    continue
                rows.append({"name": code_name, "change": change})
            if rows:
                return rows, data_date
            last_error = "Response error: 未解析到有效行情"
            eprint("新浪响应没有可用行情 (attempt %d)" % attempt)
            time.sleep(1)
            continue
        last_error = "HTTP %d: %s" % (code, (raw or "")[:200])
        eprint("新浪 HTTP %d (attempt %d)" % (code, attempt))
        if code == 403:
            break
        time.sleep(1)
    raise RuntimeError(last_error or "新浪行情请求失败")


def push_device(endpoint, token, payload, timeout, retries=2):
    """向设备 JSON 端点 POST，失败抛出 RuntimeError。"""
    headers = {"Authorization": "Bearer " + token}
    last_error = None
    for attempt in range(1, retries + 2):
        try:
            code, raw = http_request(endpoint, method="POST", headers=headers,
                                     body=payload, timeout=timeout)
        except Exception as ex:
            last_error = "Network error: %s" % ex
            eprint("设备推送失败 (attempt %d): %s" % (attempt, last_error))
            time.sleep(1)
            continue
        if code == 200:
            return
        if code in (401, 403):
            raise RuntimeError("设备鉴权失败 HTTP %d: 检查 --device-token" % code)
        last_error = "HTTP %d: %s" % (code, (raw or "")[:200])
        eprint("设备推送 HTTP %d (attempt %d)" % (code, attempt))
        time.sleep(1)
    raise RuntimeError(last_error or "设备推送失败")


def get_device_json(endpoint, token, timeout):
    code, raw = http_request(endpoint, headers={"Authorization": "Bearer " + token},
                             timeout=timeout)
    if code != 200:
        raise RuntimeError("HTTP %d: %s" % (code, (raw or "")[:200]))
    try:
        return json.loads(raw or "{}")
    except Exception:
        return {"raw": raw}


def normalize_device_base(device):
    d = (device or "").strip()
    if not d:
        return ""
    if "://" not in d:
        d = "http://" + d
    return d.rstrip("/")


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="额度/股票自动推送与时间策略调度")
    device = parser.add_argument_group("设备")
    device.add_argument("--device", default=os.environ.get("DEVICE", ""),
                        help="设备地址（默认补 http://；也可用 DEVICE）")
    device.add_argument("--device-token", default=os.environ.get("DEVICE_TOKEN", ""),
                        help="设备 Bearer token（也可用 DEVICE_TOKEN）")
    device.add_argument("--timeout", type=float, default=15.0,
                        help="单次 HTTP 超时秒数，默认 15")
    device.add_argument("--no-sleep", action="store_true",
                        help="关闭休眠时段策略和 sleep API，便于离线测试")

    balance = parser.add_argument_group("额度上游")
    balance.add_argument("--upstream-url",
                         default=os.environ.get("UPSTREAM_URL", DEFAULT_UPSTREAM_URL),
                         help="OpenCode Go 完整 URL（也可用 UPSTREAM_URL）")
    balance.add_argument("--upstream-key", default=os.environ.get("UPSTREAM_KEY", ""),
                         help="上游 API key（也可用 UPSTREAM_KEY）；缺失时跳过额度")
    balance.add_argument("--insecure", action="store_true",
                         help="上游 TLS 不校验证书")

    stock = parser.add_argument_group("股票行情")
    stock.add_argument("--symbols", default=os.environ.get("SYMBOLS", DEFAULT_SYMBOLS),
                       help="逗号分隔新浪代码，最多 5 个（也可用 SYMBOLS）")
    stock.add_argument("--sina-url", default=os.environ.get("SINA_URL", DEFAULT_SINA_URL),
                       help="新浪 URL 前缀（也可用 SINA_URL）")
    stock.add_argument("--stock-dry-run", action="store_true",
                       help="只真实拉取并打印股票行情 payload，不连接设备"
                            "（用于测试取数链路；加 --demo 则用本地随机数据）")

    policy = parser.add_argument_group("时间策略")
    policy.add_argument("--open-interval", type=int, default=300,
                        help="开市股票间隔秒数，默认 300（5 分钟）")
    policy.add_argument("--closed-interval", type=int, default=300,
                        help="非开市额度间隔秒数，默认 300")
    policy.add_argument("--balance-every", type=int, default=900,
                        help="开市额度窗口周期秒数，默认 900")
    policy.add_argument("--balance-window", type=int, default=300,
                        help="开市额度窗口持续秒数，默认 300")
    policy.add_argument("--balance-refresh", type=int, default=300,
                        help="额度窗口内刷新间隔秒数，默认 300（5 分钟）")
    policy.add_argument("--sleep-from", default="00:00",
                        help="每日休眠开始 HH:MM，默认 00:00")
    policy.add_argument("--sleep-to", default="08:00",
                        help="每日休眠结束 HH:MM，默认 08:00")
    policy.add_argument("--sleep-keepalive", type=int, default=600,
                        help="休眠期间重申 sleep 的间隔秒数，默认 600")

    common = parser.add_argument_group("通用")
    common.add_argument("--loop", action="store_true", help="持续运行；默认只跑一轮")
    common.add_argument("--dry-run", action="store_true",
                        help="只打印端点和 payload，不发请求（含 sleep）")
    common.add_argument("--check", action="store_true",
                        help="GET 并打印 balance、stock、sleep 三份 JSON")
    common.add_argument("--self-test", dest="self_test", action="store_true",
                        help="用 VirtualClock 和假后端运行内置策略验收")
    common.add_argument("--demo", action="store_true",
                        help="额度/股票均用本地随机数据；不联网且跳过开市判定")
    return parser


def validate_args(args):
    device_base = normalize_device_base(args.device)
    try:
        symbols = parse_symbols(args.symbols)
    except ValueError as ex:
        return device_base, [], str(ex)
    try:
        sleep_from = parse_clock(args.sleep_from, "--sleep-from")
        sleep_to = parse_clock(args.sleep_to, "--sleep-to")
    except ValueError as ex:
        return device_base, symbols, str(ex)
    if sleep_from == sleep_to:
        return device_base, symbols, "--sleep-from 与 --sleep-to 不能相同"
    for name in ("timeout", "open_interval", "closed_interval", "balance_every",
                 "balance_window", "balance_refresh", "sleep_keepalive"):
        if getattr(args, name) <= 0:
            return device_base, symbols, "--%s 必须 > 0" % name.replace("_", "-")
    if not (args.sina_url or "").strip():
        return device_base, symbols, "缺少 --sina-url（或环境变量 SINA_URL）"
    # --stock-dry-run 只测取数，不需要设备地址与 token
    if args.check or not (args.dry_run or args.stock_dry_run):
        if not device_base:
            return device_base, symbols, "缺少 --device（或环境变量 DEVICE）"
        if not args.device_token:
            return device_base, symbols, "缺少 --device-token（或环境变量 DEVICE_TOKEN）"
    if not (args.upstream_url or "").strip() and (args.upstream_key or args.demo):
        return device_base, symbols, "缺少 --upstream-url"
    return device_base, symbols, ""


def show_dry_run(endpoint, payload):
    print("[dry-run] POST %s" % endpoint)
    print(json.dumps(payload, ensure_ascii=False))


def stock_dry_run(args, symbols):
    """真实拉取股票行情并打印 payload，全程不连接设备。

    与 ``--dry-run`` 的区别：后者跟随时间策略、且用本地随机数据代替取数；
    这里是真的请求新浪，用来单独验证取数链路、字段解析与开市判定。
    """
    if args.demo:
        rows = [{"name": symbol, "change": round_change(random.uniform(-5.0, 5.0))}
                for symbol in symbols]
        data_date = None
    else:
        try:
            rows, data_date = fetch_quotes(args.sina_url, symbols,
                                           args.timeout, args.insecure)
        except Exception as ex:
            eprint("行情失败: %s" % ex)
            return 1

    now_bj = time.gmtime(time.time() + 8 * 3600)
    weekday = "周一至周五" if now_bj.tm_wday <= 4 else "周末"
    open_now = is_market_open(now_bj, data_date=data_date)
    print("北京时间 %s（%s）" % (time.strftime("%Y-%m-%d %H:%M:%S", now_bj), weekday))
    print("行情日期 %s → 开市判定：%s" % (data_date or "(取不到)",
                                          "开市" if open_now else "休市"))
    for row in rows:
        if row["change"] > 0:
            tone = "涨 → 屏上正红"
        elif row["change"] < 0:
            tone = "跌 → 屏上正绿"
        else:
            tone = "平 → 屏上中性白"
        print("  %-10s %+7.2f%%  %s" % (row["name"], row["change"], tone))
    show_dry_run("/api/v1/stock", {"rows": rows})
    return 0


def run_balance(args, device_base):
    if not args.demo and not args.upstream_key:
        return 1, False
    if args.demo:
        labels, progress, resets, status = build_payload(build_demo_usage())
        status = "DEMO " + status.replace("UPDATE ", "")
        usage_ok = True
    elif args.dry_run:
        # dry-run 的定义是完全不联网，用固定样例走格式化路径。
        labels, progress, resets, status = build_payload(SAMPLE_USAGE)
        usage_ok = True
    else:
        try:
            usage = fetch_upstream(args.upstream_url, args.upstream_key,
                                   args.timeout, args.insecure)
        except RuntimeError as ex:
            eprint("额度失败: %s" % ex)
            return 1, True
        labels, progress, resets, status = build_payload(usage)
        usage_ok = True
    payload = {"labels": labels, "progress": progress, "resets": resets,
               "status": status}
    endpoint = device_base + "/api/v1/balance"
    if args.dry_run:
        show_dry_run(endpoint, payload)
        return 0, usage_ok
    try:
        push_device(endpoint, args.device_token, payload, args.timeout)
    except RuntimeError as ex:
        eprint("设备失败: %s" % ex)
        return 2, usage_ok
    return 0, usage_ok


def run_stock(args, device_base, symbols):
    if args.demo or args.dry_run:
        # dry-run 也不能访问新浪；使用本地样例保证离线可重复。
        rows = [{"name": symbol,
                 "change": round_change(random.uniform(-5.0, 5.0))}
                for symbol in symbols]
        data_date = None
    else:
        try:
            rows, data_date = fetch_quotes(args.sina_url, symbols, args.timeout,
                                           args.insecure)
        except RuntimeError as ex:
            eprint("行情失败: %s" % ex)
            return 1, None
        # 接口日期是节假日的权威兜底；日期不是今天时只作探针，不把旧行情
        # 推上屏，下一轮会转入休市/额度节拍。
        if data_date and data_date != beijing_date(beijing_time_now()):
            eprint("新浪行情日期 %s 不是今天，按休市处理" % data_date)
            return 0, data_date
    endpoint = device_base + "/api/v1/stock"
    payload = {"rows": rows}
    if args.dry_run:
        show_dry_run(endpoint, payload)
        return 0, data_date
    try:
        push_device(endpoint, args.device_token, payload, args.timeout)
    except RuntimeError as ex:
        eprint("设备失败: %s" % ex)
        return 2, data_date
    return 0, data_date


def run_sleep(args, device_base, on):
    payload = {"on": bool(on)}
    endpoint = device_base + "/api/v1/display/sleep"
    if args.dry_run:
        show_dry_run(endpoint, payload)
        return 0
    try:
        push_device(endpoint, args.device_token, payload, args.timeout)
    except RuntimeError as ex:
        eprint("设备失败: %s" % ex)
        return 2
    return 0


def self_test():
    def args_for(**changes):
        values = dict(device="", device_token="", timeout=1.0, no_sleep=False,
                      upstream_url=DEFAULT_UPSTREAM_URL, upstream_key="",
                      insecure=False, symbols=DEFAULT_SYMBOLS,
                      sina_url=DEFAULT_SINA_URL, open_interval=15,
                      closed_interval=300, balance_every=900,
                      balance_window=300, balance_refresh=60,
                      sleep_from="00:00", sleep_to="08:00",
                      sleep_keepalive=600, loop=False, dry_run=False,
                      check=False, demo=True, self_test=False)
        values.update(changes)
        return argparse.Namespace(**values)
    
    class Backend:
        def __init__(self):
            self.calls = []
            self.balance_failures = 0
            self.stock_date = None
            self.slow_stock = False
    
        def fetch_stock(self, ctx):
            self.calls.append("fetch_stock")
            if self.slow_stock:
                ctx.clock.advance(3)
            return Result(True, ([{"name": "sh600519", "change": 1.0}],
                                 self.stock_date))
    
        def fetch_balance(self, ctx):
            self.calls.append("fetch_balance")
            if self.balance_failures:
                self.balance_failures -= 1
                return Result(False, error="fake upstream")
            return Result(True, (["5H", "WK.", "MO."], [50, 40, 30],
                                 ["R1h", "R2d", "R3d"], "TEST"))
    
        def post_device(self, ctx, path, body):
            self.calls.append("post" + path)
            return Result(True)
    
        def post_sleep(self, ctx, on):
            self.calls.append("sleep:%s" % bool(on))
            return Result(True)
    
    results = []
    
    def ok(name, condition):
        if not condition:
            raise AssertionError(name)
        results.append(name)
        print("[PASS] %02d %s" % (len(results), name))
    
    def make(start=0.0, **kwargs):
        clock = VirtualClock(start)
        backend = Backend()
        scheduler = Scheduler(args_for(**kwargs), "", DEFAULT_SYMBOLS.split(","),
                               clock=clock, backend=backend, quiet=True)
        return clock, backend, scheduler
    
    # 1. 开市连续股票 tick。
    clock, backend, s = make(0, no_sleep=True)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=3)
    ok("开市 15s 股票 tick 连续触发", backend.calls.count("post/api/v1/stock") == 3)
    
    # 2. 到期进入额度窗口。
    clock, backend, s = make(0, no_sleep=True)
    s.start(State.AWAKE_OPEN)
    s.last_balance_at = -900
    s.run_handler(s.handle_balance_due())
    ok("开市距上次额度 900s 进入额度窗口", s.state == State.BALANCE_WINDOW)
    
    # 3. 窗口刷新。
    clock, backend, s = make(0, no_sleep=True)
    s.start(State.BALANCE_WINDOW)
    s.run(max_events=1)
    ok("额度窗口内 60s 刷新", backend.calls.count("post/api/v1/balance") == 1)
    
    # 4. 窗口结束回股票并立即推股票。
    clock, backend, s = make(0, no_sleep=True, balance_refresh=1000)
    s.start(State.BALANCE_WINDOW)
    s.run(max_events=2)
    ok("额度窗口满 5 分钟回股票并推送", s.state == State.AWAKE_OPEN and
       "post/api/v1/stock" in backend.calls)
    
    # 5. 休市 300s。
    clock, backend, s = make(0, no_sleep=True)
    s.start(State.AWAKE_CLOSED)
    s.run(max_events=2)
    ok("非开市 300s 额度节拍", backend.calls.count("post/api/v1/balance") == 2)
    
    # 6. 00:00 进入休眠。
    midnight = -8 * 3600
    clock, backend, s = make(midnight)
    s.start()
    ok("00:00 进入休眠并发送 sleep true", s.state == State.SLEEPING and
       backend.calls.count("sleep:True") == 1)
    
    # 7. keepalive 前无数据调用。
    backend.calls[:] = []
    clock.advance(599)
    s.run(max_events=0)
    ok("休眠中未到 keepalive 静默且零数据请求",
       not any("balance" in call or "stock" in call for call in backend.calls))
    
    # 8. keepalive。
    clock.advance(1)
    s.run(max_events=1)
    ok("休眠中到 keepalive 重发 sleep true", backend.calls.count("sleep:True") == 1)
    
    # 9. 08:00 唤醒顺序。
    clock, backend, s = make(-60)
    s.start(State.SLEEPING)
    clock.advance(60)
    s.run(max_events=1)
    ok("08:00 唤醒先关 sleep 再推额度",
       backend.calls[0] == "sleep:False" and
       backend.calls[-1] == "post/api/v1/balance")
    
    # 10. 窗口失败留在窗口。
    clock, backend, s = make(0, no_sleep=True)
    backend.balance_failures = 2
    s.start(State.BALANCE_WINDOW)
    s.run(max_events=1)
    clock.advance(60)
    s.run(max_events=1)
    ok("窗口上游失败按刷新重试且不回股票", s.state == State.BALANCE_WINDOW and
       not any("stock" in call for call in backend.calls))
    
    # 11. 过期行情日期转休市。
    clock, backend, s = make(10 * 3600, no_sleep=True)
    backend.stock_date = "2000-01-01"
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("行情日期过期转休市节拍", s.state == State.AWAKE_CLOSED and
       "post/api/v1/stock" not in backend.calls)
    
    # 12. 跨午夜区间。
    clock, backend, s = make(15 * 3600, sleep_from="23:00", sleep_to="07:00")
    s.start()
    is_sleep = s.state == State.SLEEPING
    clock.advance(8 * 3600)
    s.run(max_events=1)
    ok("自定义跨午夜休眠区间", is_sleep and "sleep:False" in backend.calls)
    
    # 13. 非法转移 fail loud。
    clock, backend, s = make(0, no_sleep=True)
    s.start(State.AWAKE_OPEN)
    try:
        s.transition(Event.UNKNOWN)
        raised = False
    except IllegalTransition:
        raised = True
    ok("非法状态转移抛错", raised)
    
    # 14. 处理跨过多个周期只执行一次，积压周期被跳过。
    clock, backend, s = make(0, no_sleep=True, open_interval=1)
    backend.slow_stock = True
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("漏 tick 单飞跳过积压节拍", backend.calls.count("post/api/v1/stock") == 1 and
       any("跳过" in line for line in s.log_lines))
    
    print("SELF-TEST PASS: %d/14" % len(results))
    

def check_device_sync(args, device_base):
    for label, path in (("balance", "/api/v1/balance"),
                        ("stock", "/api/v1/stock"),
                        ("sleep", "/api/v1/display/sleep")):
        value = get_device_json(device_base + path, args.device_token, args.timeout)
        print("%s: %s" % (label, json.dumps(value, ensure_ascii=False)))


def main(argv=None):
    args = build_arg_parser().parse_args(argv)
    if args.self_test:
        return self_test()
    device_base, symbols, error = validate_args(args)
    if error:
        eprint("参数错误: %s" % error)
        return 3
    if args.check:
        try:
            check_device_sync(args, device_base)
            return 0
        except (RuntimeError, OSError) as ex:
            eprint("设备失败: %s" % ex)
            return 2
    if args.stock_dry_run:
        return stock_dry_run(args, symbols)
    scheduler = Scheduler(args, device_base, symbols, clock=RealClock())
    try:
        scheduler.start()
        scheduler.run(max_events=None if args.loop else 1)
    except KeyboardInterrupt:
        print("中断退出")
    return scheduler.rc


if __name__ == "__main__":
    sys.exit(main())
