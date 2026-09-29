#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""额度/股票自动推送与时间策略调度（同步生成器架构）。

调度层不使用 asyncio、线程、事件循环或 to_thread；网络调用保持同步，阻塞
urllib 调用只发生在 handler yield 的 Fetch*/Post* 任务中。

状态图（开市窗口可配置，下文用「收盘」「开盘」代指）::

    AWAKE_OPEN --行情过期/收盘--> AWAKE_CLOSED --开盘探针通过--> AWAKE_OPEN
         |                              |                                     |
         +--进入休眠--> SLEEPING <-------+------唤醒(08:00)-------------------+

每状态闹钟表（开盘/收盘 = 开市窗口的起止，默认 09:20/15:30）::

    AWAKE_OPEN     Every(stock,15s), Every(quota_due,300s),
                   DailyAt(market_check,收盘), DailyAt(sleep_at,20:00)
    AWAKE_CLOSED   Every(balance,300s), DailyAt(market_check,开盘),
                   DailyAt(sleep_at,20:00)
    SLEEPING       Every(keepalive,600s), DailyAt(wake_at,08:00)

开市窗口默认 09:20~15:30，可用 --market-open/--market-close（或环境变量
MARKET_OPEN / MARKET_CLOSE）覆盖。两点不变式：

1. 窗口是**半开区间** ``start <= 秒 < end``。这条不是洁癖 —— 收盘闹钟 DailyAt
   恰好落在 15:30:00.000 那一拍，闭区间会让 is_market_open 仍返回 True，收盘闹钟
   自己判自己「还开市」而空转，状态卡在 AWAKE_OPEN 每 15s 推行情直到 20:00 入睡。
2. **行情节拍每拍自愈**。Every(open_interval) 自身不重新判时间，所以任何一次收盘
   检查丢失（机器挂起、handler 异常、状态被外部改动）都必须由下一拍行情兜底：
   handle_stock 开头发现已过窗口就自己转休市（≤15s 内纠正）。

休眠区间 20:00~08:00 **跨午夜**（is_sleep_time 已支持 from > to 的情形）：收盘
到 20:00 之间照常按休市节拍推额度，20:00 闹钟入睡，次日 08:00 唤醒。
起止时间从命令行 flag 读，也可用环境变量 SLEEP_FROM / SLEEP_TO 覆盖 —— 部署机上
就是 systemd 的 EnvironmentFile（/etc/sd2.conf），改那里不用动 unit 的 ExecStart。

开市期间屏幕只保留股票场景：额度**不再**单独 POST /api/v1/balance（设备会立刻
切到 balance 页），改成按 --balance-every 单独推一条只含额度数据的股票推送；
休市/唤醒时仍单独推 /api/v1/balance，保证额度页在收盘后是新的。

行情与额度是两个独立节拍，各推各的（POST /api/v1/stock 两种 body）::

    行情 15s:  {"rows": [...]}
    额度 5min: {"balance": {"progress": [58, null, 91]}}

行名可以用 SYMBOL_NAMES 配的简称代替代码，按位置一一对应、项数必须相同（错位会把
简称贴到错的标的上，所以宁可启动失败）。建议用官方英文简称，例如::

    SYMBOLS=sh000300,sh000905,sz159531,sz399006,sh000688
    SYMBOL_NAMES=CSI300,CSI500,CSI2000,GEM,STAR50

留空项表示那只沿用代码。简称受设备端同名限制（1..9 字节可打印 ASCII），校验与
--symbols 一致；替换只发生在排版层，取数层拿到的永远是原代码。

取数层是 SinaQuotes（见下），用新浪的 s_ 简版接口：简版一行只有 6 个字段
``名称,点位,涨跌额,涨跌幅%,成交量,成交额``，**fields[3] 直接就是涨跌幅百分比**，
不用自己除，正好是设备唯一要的量；而完整版多一个 ``fields[30]`` 的行情日期，是
节假日/停市的兜底判定（日期≠今天）唯一的来源。两者可以混写在同一个 list= 里（实测
``s_sh000300,sh000300`` 两行都返回），所以每拍只发一次请求：每个标的既走简版、
也捎带完整版（多出来的完整版行只贡献日期、不再出行）。日期取所有完整版行里**最新
的一天**而不是第一个 —— 停牌标的会报陈旧日期，只信一个会把正常交易日误判成休市；
节假日时全员同日，max 不改变结论。写 ``--symbols`` 时可以带 ``s_`` 前缀，取数层
会剥掉。

海外指数是**第三种形态**（``int_`` 前缀，如 int_nikkei/int_dji/int_hangseng）::

    int_nikkei    4 字段  ['日经指数','44946.64','-408.35','-0.90']  ← 名称,现价,涨跌额,涨跌幅%
    s_int_nikkei  1 字段  ['']                                        ← 海外指数没有 s_ 简版

所以它既不套简版也不套 A 股完整版：(f[3]−f[2])/f[2] 在这里会把 -0.91% 算成
+0.02%（那里 fields[2] 是昨收，这里是涨跌额）。它**没有日期字段**，因此不参与
日期投票 —— 行情日期/节假日兜底只对 A 股代码有效，纯 ``int_`` 列表拿不到行情
日期。是否给某个代码加 ``s_`` 简版由 SIMPLE_PREFIXES 决定（实测只有 sh/sz 有简版，
**bj 没有** —— ``s_bj430047`` 恒为空串，详见 tools/sina-prefixes.md），列表里全是
``int_`` 时直接走纯完整版形态，不做「简版 → 回退」两轮 dance。``int_nikkei`` 有 10 字节，超过设备端 name 的 9 字节上限，所以 SYMBOLS
的语法上限放宽到 16 字节，但超 9 字节的代码**必须**在 SYMBOL_NAMES 里配简称
（没配简称才会原样上屏）。

外盘期货是**第四种形态**（``hf_`` 前缀，如 hf_NQ/hf_ES/hf_XAU）::

    hf_NQ   15 字段 [0]现价 [7]昨收 [12]日期 [13]中文名 [14]'0'
    hf_XAU  14 字段 ← 尾部少一个，所以**不能**按字段数判形态

它**没有现成的涨跌幅百分比**（不像 int_ 的 fields[3] 直给），必须自算
``(f[0]−f[7])/f[7]*100``（要求 f[7] > 0），再交给 round_change 归一到 2 位。实测
hf_NQ 30507.430 / 昨收 30566.250 → −0.19%。f[12] 的日期**故意不参与 A 股日期
投票** —— 外盘期货在 A 股节假日照常交易，混进投票会让节假日被误判成开市；所以
纯 ``hf_`` 列表的 data_date 同样是 None。名称取 f[13]（中文，只进日志）。实测样本
（日期投票之外都用它）：hf_NQ / hf_ES / hf_YM / hf_CL / hf_HSI / hf_CHA50CFD
都是 15 字段，hf_XAU 是 14。

港股是**第五种形态**（``hk`` 前缀，如 hkHSI/hk00700/hk09988/hkHSCEI）::

    hkHSI  19 字段 [0]英文名 [1]中文名 [2]今开 [3]昨收 [4]最高 [5]最低 [6]现价
                     [7]涨跌额 [8]涨跌幅% [9]买价 [10]卖价 [11]成交量 [12]成交额
                     [15]52周高 [16]52周低 [17]日期(斜杠) [18]时间

f[8] 是**现成的涨跌幅百分比，不要自算**（与 hf_ 相反）：算术自校验 hkHSI 的
(f[6]−f[3])/f[3] = −0.5487%，与 f[8]=−0.549 吻合。有效性只认 f[0] 非空 + f[8]
可解析，**不拿现价 f[6] 当门槛** —— 同「开盘前深市指数点位 0 是合法值」那条原则，
盘前/停牌时价格可能是 0 而涨跌幅字段仍是有效值。名称取 f[1] 中文名（只进日志，
不上屏；f[0] 是 ASCII 英文名，以后想让英文名直接上屏可用它），取不到也**不丢行**。
没有 ``s_`` 简版。f[17] 的日期是**斜杠格式** 2026/09/29，且**故意不参与日期投票**
（见下）。

海外市场指数是**第六种形态**（``b_`` 前缀，如 b_DAX/b_FTSE/b_CAC/b_NKY/b_SPX），
**至少延迟 15 分钟**（社区文档原文如此）::

    b_DAX  13 字段  [0]名称 [1]现价 [2]涨跌额 [3]涨跌幅% [4]? [5]? [6]日期
                      [7]时间 [8]? [9]昨收 [10]? [11]? [12]?
    b_SPX   6 字段  [0]名称 [1]现价 [2]涨跌额 [3]涨跌幅% [4]时间 [5]时间

**b_SPX 只有 6 字段** → 绝不能按字段数判形态（只能按 b_ 前缀）。f[3] 是**现成的
涨跌幅，直接用不自算**，依据是算术自校验：b_DAX 25374.42+34.22=25408.64=f[9]、
b_NKY 65219.72+657.90=65877.62=f[9] → f[9]=昨收；且 −34.22/25408.64=−0.13%
=f[3]、−657.90/65877.62=−1.00%=f[3]。f[4]/f[5] 实测时而是 '9/26/2025'+'2:12 AM'、
时而是 '2025-09-26'+'14:12:00'、时而全空 → **语义未定，不解析**；f[8]/f[10]/f[11]
疑似今开/最高/最低但**只是推测未确证 → 不填**。没有 s_ 简版（实测 s_b_DAX 空串）。

美股是**第七种形态**（``gb_`` 前缀，如 gb_bili/gb_ixic/gb_dji/gb_aapl）::

    gb_bili  36 字段  [0]名称 [1]最新价 [2]涨跌幅% [3]日期时间合一 [4]涨跌额
                      [5]今开 [6]最高 [7]最低 [8]52周高 [9]52周低 [10]成交量
                      [11]成交量(另一口径) [12]成交额 [24][25]EDT 时间串 [26]昨收 …
    gb_dji   30 字段  ← 同样不能按字段数判形态

f[2] 是**现成的涨跌幅，直接用不自算**（算术自校验：gb_bili 15.14−0.245=14.895
=f[26]、gb_aapl 338.40−(−2.67)=341.07=f[26] → f[26]=昨收；0.245/14.895=1.64%=f[2]、
−2.67/341.07=−0.78%=f[2]）。f[3] 是 '2026-09-29 08:02:08' 这种**日期时间合一**串，
格式与 A 股不同 + 美股交易日历不同 → **不参与日期投票**。没有 s_ 简版（实测
s_gb_bili 空串）。

外汇/汇率是**第八种形态**（**两种**子形态，**本轮只识别、绝不推算涨跌幅**）::

    3a 裸代码 11 字段  USDCNY / DINIW / CNYUSD（**没有前缀**）
    3b fx_ 前缀 18 字段  fx_susdcny / fx_susdjpy

3a **没有前缀**，不能靠前缀分流，只能靠代码形状识别：**整串都是 ASCII 大写字母、
不含任何数字、长度 5~6**。依据：A股代码必然含数字（sh000300 / bj430047），所以
这条判据**不会误伤 A 股**。**这条判据必须排在 A 股完整版分支之前** —— 否则 USDCNY
会掉进完整版分支被当成「昨收 f[2] / 现价 f[3]」算出 (6.7136−6.7055)/6.7055 =
**+0.12%** 这种**看着挺像样的垃圾**。

识别出来就**跳过**，中文原因写明「外汇/汇率形态的涨跌字段语义未验证，暂不推算」。
理由：15 秒差分实测里三个形态**只有 DINIW 的 f[1]/f[2]/f[8] 在变**，其余字段全静态
→ **无法可靠定位哪个是昨收**，fx_ 的 [10][11][12] 语义也定不下来。正因猜错算出来的
数看着合理，才最容易被误当成解析成功而静默上屏。两种形态的日期也不投票。

**日期投票只认 A 股完整版口径**（``fields[30]`` 的 YYYY-MM-DD），``int_`` / ``hf_``
/ ``hk*`` / ``b_`` / ``gb_`` 五种非 A 股形态一律把 date 填 ``""``（外汇根本不出行）。
理由是双重的：① 交易日历不同 —— 外盘期货在 A 股节假日照常交易、港股/美股/海外指数
各有各的假期，混进投票会把节假日/非交易日误判成开市（海外指数还是**延迟 15 分钟**
的数据，拿它判「今天开没开盘」本身就不成立）；② 格式不同 —— 港股是 2026/09/29、
美股 gb_ 的 f[3] 是「2026-09-29 08:02:08」日期时间合一串，和 A 股的 2026-09-29
混进同一个 ``max()`` 会按字符串序比错。所以纯非 A 股列表的 data_date 是 None。

**已知但未支持**（UNSUPPORTED_PREFIXES）：``rt_`` 延时行情 —— 实测 ``rt_hkHSI``
是 **25 字段**，位置 0..18 与 ``hk*`` 完全一致、尾部多 6 个空字段，但**语义**是
延时行情而不是实时。它会在形态分流的**最前面**被前缀名单拦下，给出「rt_ 延时行情
形态暂不支持」的跳过原因：按字段数判形态挡不住它（25 字段照样能读出 f[8]），放行
就会静默拿延时价当实时价上屏。

**代码大小写是非对称的，别「顺手统一成一种」** —— 实测::

    SH000300 → EMPTY      hf_NQ  → 15 字段正常（纳斯达克指数期货）
    sh000300 → 34 字段正常  hf_nq  → EMPTY
    Sz399006 → EMPTY      int_nikkei → 4 字段正常（本来就全小写）

所以 **A 股必须小写、外盘期货必须保大小写**。normalize_symbol 只把匹配
``^(sh|sz|bj)\\d{6}$`` 的代码转小写（用户写 ``SH000300`` 会被修正成 ``sh000300``），
其余形态原样送出；判定与字典归并用小写副本，**请求 URL 用保大小写的代码**。

**布局按 key 带不带 s_ 前缀分流，不按字段数。** 完整版的指数和个股是同一套布局
（沪市 34 字段 / 深市 33 字段，尾部多一个空字段）：``fields[2]``=昨收、
``fields[3]``=最新价/最新点位，涨跌幅 = ``(fields[3]−fields[2])/fields[2]``，
**这个公式对个股和指数都成立**；``fields[30]``=行情日期、``fields[31]``=行情时间，
sh/sz 下位置一致。所以「字段少的是指数」只在 s_ 简版下成立 —— 拿它去判完整版会把
指数误判成个股，只是因为公式对两者都成立才碰巧没出错。

坑：**开盘前深市指数（创业板指 sz399006、国证2000 sz399303、深证成指 sz399001）
的简版点位是 0.00**，同时沪市 sh000300 却有真实点位。旧代码拿「点位 > 0」当有效性
门槛，开盘前会把这些行整行丢掉。所以简版只认 fields[3] 那个百分比，点位为 0 是合法
值；完整版才保留昨收/现价 > 0 的守卫（否则开盘前会算出 −100% 的假跌）。

额度那条**不带 rows**：设备只更新屏顶三条额度条，不动行情行、不接管屏幕，
所以 5 分钟一刷的额度不会打断 15s 一刷的行情。progress 顺序固定为滚动/周/月
三段，元素是 0..100 的剩余百分比或 null（无窗口）；上游取数失败时这一拍什么
都不推，设备保留上次的条。--no-quota-bars 可让开市期间完全不发额度。

日志分两层：普通日志只有**启动摘要**、**状态转移**和**全部失败/异常**；例行节拍
的成功行（15s 行情、5min 额度、休眠 keepalive）走 Verbose 任务，默认丢弃，
`--verbose` 才逐拍输出。否则 journal 每分钟 4 行正常噪音，真异常反而被淹没。

handler 可 yield 的任务与调度器回喂结果::

    FetchStock / FetchBalance  同步执行阻塞网络调用，回 Result(ok,data,error)
    PostDevice(path,body)      同步 POST 设备，回 Result
    PostSleep(on)              同步设置休眠，回 Result
    Log(msg)                   打印一行状态，回 None
    Verbose(msg)               仅 --verbose 时打印（例行节拍的成功行），回 None
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
# 设备端行名 name 的上限是 9 字节（1..9 可打印 ASCII，见 POST /api/v1/stock）。
DEVICE_NAME_MAX = 9
# 标的代码的语法上限：比设备端 name 宽，因为取数层把代码当 name 推，只有没配简称
# 时才原样上屏。16 字节覆盖实测的海外指数代码（int_hangseng 12 / int_nikkei 10）。
MAX_SYMBOL_BYTES = 16
# A 股/北交所代码前缀（注意代码是 sh000300 这种「sh+数字」，没有下划线）。**只用于
# 「这类代码要转小写」这一条判断**（新浪大小写敏感，见 normalize_symbol 的实测对照）。
# 别拿它当「支持简版」的名单用 —— bj 没有 s_ 简版。
A_SHARE_PREFIXES = ("sh", "sz", "bj")
# 支持 s_ 简版的前缀。依据是实测：sh/sz 的代码请求 s_<code> 都返回 6 字段简版
# （sh600519 个股、sh510300 ETF 都试过），**bj 不行** —— s_bj430047 恒为空串。
# int_nikkei 的 s_int_nikkei 恒为空串（海外指数没有简版形态），hk*/hf_* 同理。
# 不按「看起来像不像 A 股」外推，只有这一条是实测过的。判定一律走 code.lower()。
SIMPLE_PREFIXES = ("sh", "sz")
# 无 s_ 简版的形态：int_ 海外指数是 4 字段，hf_ 外盘期货是 15/14 字段，hk* 港股是
# 19 字段，b_ 海外市场指数是 13/6 字段，gb_ 美股是 36/30 字段，fx_ 外汇是 18 字段；
# 实测 s_int_nikkei / s_hkHSI / s_hf_NQ / s_b_DAX / s_gb_bili 全部是空串。
# 这些形态**都不参与日期投票**（只有 A 股完整版的 fields[30] 投票）。
NOSIMPLE_PREFIXES = ("int_", "hf_", "hk", "b_", "gb_", "fx_")
# 已知存在但本轮**不实现**的形态，命中即给中文原因跳过，绝不放行到别的分支里被误算。
# rt_* 是新浪的**延时行情**：实测 rt_hkHSI 有 25 字段，位置 0..18 与 hk* 完全一致、
# 尾部多 6 个空字段，**语义**却不是同一回事（延时）。本轮不做：25 字段能按 hk 的
# 下标读出 f[8]，所以绝不能靠「字段数」把它挡在门外，只能靠这里的**前缀名单**在形态
# 分流最前面拦下 —— 放行就会静默地拿延时价当实时价上屏。
UNSUPPORTED_PREFIXES = ("rt",)
UNSUPPORTED_REASONS = {
    "rt": "rt_ 延时行情形态暂不支持（实测 25 字段，位置同 hk* 但语义是延时行情）",
}
# 外汇/汇率形态的代码判据：**整串都是 ASCII 大写字母、不含任何数字、长度 5~6**
# （实测 USDCNY 6 / DINIW 5 / CNYUSD 6，以及 fx_susdcny / fx_susdjpy）。
# 依据：A 股代码必然含数字（sh000300 / bj430047），所以「无数字 + 全大写字母」这条
# 判据**不会误伤 A 股**；反过来外汇代码正是「裸大写字母」或 `fx_` 前缀，两者都不会
# 匹配 ^(sh|sz|bj)\d{6}$。这条判据必须**排在 A 股完整版分支之前** —— 否则 USDCNY
# 会掉进完整版分支，被当成「昨收 6.7055 / 现价 6.7136」算出 +0.12% 这种**看着挺
# 像样的垃圾**（见 _parse_quote 里 FX 分支的注释）。
FX_PREFIX = "fx_"
FX_BARE_MIN_LEN = 5
FX_BARE_MAX_LEN = 6
# 识别出外汇形态就跳过（**不推算涨跌幅**）的中文原因。硬性决策，理由见
# _parse_quote：15 秒差分实测里三个形态只有 DINIW 的 f[1]/f[2]/f[8] 在动，其余全
# 静态，**无法可靠定位哪个是昨收**，[10][11][12] 的语义也定不下来。
FX_SKIP_REASON_BARE = ("外汇/汇率形态（裸代码 11 字段）的涨跌字段语义未验证，"
                       "暂不推算")
FX_SKIP_REASON_FX = ("外汇/汇率形态（fx_ 前缀 18 字段）的涨跌字段语义未验证，"
                     "暂不推算")
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


def is_market_open(value, windows=MARKET_WINDOWS, data_date=None, today=None):
    """星期 + 开市窗口（半开区间 start <= 秒 < end）+ 个股行情日期的外部开市判定。

    窗口默认是模块常量 MARKET_WINDOWS（09:20~15:30），实盘由 Scheduler 从
    --market-open/--market-close 解析后传进来。半开而非闭区间是刻意的：收盘闹钟
    DailyAt("15:30") 落在 15:30:00.000 那一拍，闭区间会让这一刻仍判为开市、收盘闹钟
    自己把自己判成「还开市」而空转。
    """
    if value.tm_wday > 4:
        return False
    seconds = local_seconds(value)
    if not any(start <= seconds < end for start, end in windows):
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


def parse_market_windows(open_value, close_value):
    """把 --market-open/--market-close 解析成 ((start_sec, end_sec),)。"""
    start = parse_clock(open_value, "--market-open/MARKET_OPEN")
    end = parse_clock(close_value, "--market-close/MARKET_CLOSE")
    if start == end:
        raise ValueError("--market-open/--market-close 不能相同")
    if start > end:
        raise ValueError("--market-open 必须早于 --market-close")
    return ((start, end),)


def format_hhmm(seconds):
    return "%02d:%02d" % (seconds // 3600, (seconds % 3600) // 60)


def is_sleep_time(value, sleep_from, sleep_to):
    seconds = local_seconds(value)
    if sleep_from < sleep_to:
        return sleep_from <= seconds < sleep_to
    return seconds >= sleep_from or seconds < sleep_to


class State(str, Enum):
    AWAKE_OPEN = "AWAKE_OPEN"
    AWAKE_CLOSED = "AWAKE_CLOSED"
    SLEEPING = "SLEEPING"


class Event(str, Enum):
    STOCK_TICK = "stock_tick"
    BALANCE_TICK = "balance_tick"
    ENTER_SLEEP = "enter_sleep"
    SLEEP_KEEPALIVE = "sleep_keepalive"
    WAKE = "wake"
    MARKET_OPEN = "market_open"
    MARKET_CLOSED = "market_closed"
    STALE_DATE = "stale_date"
    UNKNOWN = "unknown"


STATE_ALARMS = {
    State.AWAKE_OPEN: ("stock", "quota_due", "market_check", "sleep_at"),
    State.AWAKE_CLOSED: ("balance", "market_check", "sleep_at"),
    State.SLEEPING: ("keepalive", "wake_at"),
}

TRANSITIONS = {
    (State.AWAKE_OPEN, Event.STOCK_TICK): State.AWAKE_OPEN,
    (State.AWAKE_OPEN, Event.MARKET_CLOSED): State.AWAKE_CLOSED,
    (State.AWAKE_OPEN, Event.STALE_DATE): State.AWAKE_CLOSED,
    (State.AWAKE_OPEN, Event.ENTER_SLEEP): State.SLEEPING,
    (State.AWAKE_CLOSED, Event.BALANCE_TICK): State.AWAKE_CLOSED,
    (State.AWAKE_CLOSED, Event.MARKET_OPEN): State.AWAKE_OPEN,
    (State.AWAKE_CLOSED, Event.ENTER_SLEEP): State.SLEEPING,
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
class Verbose:
    """例行节拍的成功行：默认丢弃，只在 --verbose 下输出。

    15s 行情、5min 额度、休眠 keepalive 这类「每拍都成功」的行属于正常噪音，
    混在一起会把真正的异常淹没（journal 每分钟 4 行）。状态转移与全部失败/异常
    走 Log，不受本开关影响。
    """
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

    def get(self, name):
        return self._alarms.get(name)

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
                 quiet=False, aliases=None, windows=None):
        self.args = args
        self.device_base = device_base
        self.symbols = symbols
        # {代码: 简称}，推送前替换行名；None/空 = 全用原代码
        self.aliases = aliases or {}
        # 开市窗口 ((start_sec, end_sec),)。main 已解析好就传进来，否则自己从 args
        # 解析 —— 绝不读可变全局（模块常量只是默认值）。
        self.windows = windows or parse_market_windows(args.market_open,
                                                       args.market_close)
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

    def emit_verbose(self, message):
        """例行节拍行：没开 --verbose 就当它不存在（也不进 log_lines）。"""
        if not self.args.verbose:
            return
        self.emit(message)

    def sleeping_now(self):
        if self.args.no_sleep:
            return False
        start = parse_clock(self.args.sleep_from, "--sleep-from")
        end = parse_clock(self.args.sleep_to, "--sleep-to")
        return is_sleep_time(self.bj(), start, end)

    def market_open_at(self):
        return format_hhmm(self.windows[0][0])

    def market_close_at(self):
        return format_hhmm(self.windows[0][1])

    def market_window_text(self):
        return "%s~%s" % (self.market_open_at(), self.market_close_at())

    def market_provisional_open(self):
        # --demo 只跳过「API 行情日期」判定（demo 数据没有真实日期），不跳过时间
        # 窗口：窗口是时间策略，不是取数问题，无条件短路会让 demo 永远开市。
        return is_market_open(self.bj(), windows=self.windows)

    def log_market_closed(self, reason):
        """转休市的日志只有这一处文案，收盘闹钟与行情节拍自愈共用。"""
        return Log("%s（%s），转休市节拍" % (reason, self.clock_text()))

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
                "AWAKE_OPEN:quota_due", Every(self.args.balance_every),
                self.handle_quota_due), now, max(now, due))
            self.registry.register(Alarm(
                "AWAKE_OPEN:market_check", DailyAt(self.market_close_at()),
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
                "AWAKE_CLOSED:market_check", DailyAt(self.market_open_at()),
                self.handle_market_check), now)
            if not self.args.no_sleep:
                self.registry.register(Alarm(
                    "AWAKE_CLOSED:sleep_at", DailyAt(self.args.sleep_from),
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

    def alias_text(self):
        """启动摘要里的简称一览，确认 SYMBOL_NAMES 真的生效了。"""
        if not self.aliases:
            return "未配置（沿用代码）"
        return ",".join(self.aliases.get(symbol, symbol) for symbol in self.symbols)

    def log_start(self):
        """启动摘要 —— 普通日志里唯一常驻的「一切正常」信息。

        例行节拍的成功行都进了 --verbose，所以没有这行的话，进程起来之后 journal
        会长时间一片空白（看不出是没跑还是在跑）。末尾顺带说明日志为什么是静的。
        """
        yield Log("启动：状态=%s 设备=%s 行情 %ds / 额度 %ds / 非开市 %ds / "
                  "开市 %s / 休眠 %s-%s / 简称 %s%s"
                  % (self.state.value, self.device_base or "(未配置)",
                     self.args.open_interval, self.args.balance_every,
                     self.args.closed_interval, self.market_window_text(),
                     self.args.sleep_from, self.args.sleep_to, self.alias_text(),
                     "" if self.args.verbose
                     else "（例行节拍日志已静默，加 --verbose 查看每拍明细）"))

    def start(self, force_state=None):
        if self.sleeping_now() and force_state is None:
            self.state = State.AWAKE_CLOSED
            self.register_state_alarms(self.state, initial=True)
            self.run_handler(self.log_start())
            self.run_handler(self.handle_sleep_enter())
            return
        self.state = force_state or (
            State.AWAKE_OPEN if self.market_open() else State.AWAKE_CLOSED)
        self.register_state_alarms(self.state, initial=True)
        self.run_handler(self.log_start())
        if self.state == State.AWAKE_OPEN:
            # 开市先补拉一次额度，首帧股票推送就带上进度条。
            self.run_handler(self.handle_quota_due())

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
        elif isinstance(task, Verbose):
            self.emit_verbose(task.message)
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

    def balance_and_post(self, next_seconds=None):
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
        if next_seconds is None:
            next_seconds = self.args.closed_interval
        yield Verbose("额度已刷新（%s），%ds 后刷新"
                      % (self.clock_text(), next_seconds))
        return posted

    def handle_balance_due(self):
        """休市额度节拍：整段推 /api/v1/balance。"""
        yield EnterState(Event.BALANCE_TICK)
        return (yield from self.balance_and_post())

    def handle_quota_due(self):
        """开市额度节拍：只推 balance 块，与行情节拍完全独立。

        额度不搭行情的车 —— 行情 15s 一推、额度 5min 一推，各推各的：单独发一条
        只含 balance 的 POST /api/v1/stock，设备只更新屏顶三条额度条，不碰行情行、
        也不接管屏幕。上游失败时什么都不推，设备保留上次的条。
        """
        if self.args.no_quota_bars:
            return None
        result = yield FetchBalance()
        self.last_balance_at = self.now()
        if not result.ok:
            if not self.key_warned:
                yield Log("额度未配置或上游失败（开市只更新进度条）：%s"
                          % result.error)
                self.key_warned = True
            return result
        posted = yield PostDevice("/api/v1/stock", {
            "balance": {"progress": list(result.data[1])}})
        if posted.ok:
            yield Verbose("额度进度已更新（%s），%ds 后刷新"
                          % (self.clock_text(), self.args.balance_every))
        else:
            yield Log("额度条推送失败：%s" % posted.error)
        return posted

    def display_rows(self, rows):
        """行名换成用户配置的简称（简称是排版层的事，取数层保持原代码）。"""
        return apply_aliases(rows, self.aliases)

    def stock_and_post(self, result):
        rows, date = result.data
        self.last_data_date = date
        if date and date != beijing_date(self.bj()):
            self.probe_day = beijing_date(self.bj())
            yield EnterState(Event.STALE_DATE)
            yield Log("行情日期 %s 不是今天，转休市节拍" % date)
            return None
        posted = yield PostDevice("/api/v1/stock",
                                  {"rows": self.display_rows(rows)})
        if not posted.ok:
            yield Log("股票设备推送失败：%s" % posted.error)
            return posted
        # 15s 一推的例行成功行进 verbose：普通日志只留启动摘要、状态转移和异常，
        # 否则 journal 每分钟 4 行，真正要看的异常反而被淹没。
        prefix = "开市中" if self.state == State.AWAKE_OPEN else "休市中"
        yield Verbose("%s（%s）股票已推送，%ds 后下一轮"
                      % (prefix, self.clock_text(), self.args.open_interval))
        return posted

    def handle_stock(self):
        if self.state == State.AWAKE_OPEN:
            # 每拍自愈：Every(open_interval) 自身不判时间，收盘检查丢了（机器挂起、
            # handler 异常、状态被外部改）就只能靠这一拍兜底，最多 15s 纠正。
            if not self.market_provisional_open():
                yield EnterState(Event.MARKET_CLOSED)
                yield self.log_market_closed(
                    "行情节拍自愈：已过开市窗口 %s" % self.market_window_text())
                return (yield from self.balance_and_post())
            yield EnterState(Event.STOCK_TICK)
        result = yield FetchStock(self.symbols)
        if not result.ok:
            yield Log("股票拉取失败：%s" % result.error)
            return result
        return (yield from self.stock_and_post(result))

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
            yield Verbose("休市中（%s）重申 sleep，%ds 后再次检查"
                          % (self.clock_text(), self.args.sleep_keepalive))
        return result

    def handle_wake(self):
        result = yield PostSleep(False)
        if not result.ok:
            yield Log("唤醒失败：%s" % result.error)
            return
        yield EnterState(Event.WAKE)
        if self.market_open():
            # --sleep-to 落在开市时段时也会走到这里：此时只补拉额度缓存，
            # 绝不推 /api/v1/balance，否则额度页会在整个上午一直抢着屏幕。
            yield EnterState(Event.MARKET_OPEN)
            return (yield from self.handle_quota_due())
        return (yield from self.balance_and_post())

    def handle_market_check(self):
        if self.state == State.AWAKE_OPEN:
            if not self.market_open():
                yield EnterState(Event.MARKET_CLOSED)
                yield self.log_market_closed(
                    "已过开市窗口 %s" % self.market_window_text())
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
        # 先发一条纯额度推送（不进场景），再推行情：这样切到 stock 场景的第一帧
        # 就带着屏顶三条，不会有「先空屏一帧再补条」的闪烁。
        yield from self.handle_quota_due()
        posted = yield PostDevice("/api/v1/stock",
                                  {"rows": self.display_rows(rows)})
        if posted.ok:
            prefix = "开市中" if self.state == State.AWAKE_OPEN else "休市中"
            yield Verbose("%s（%s）股票已推送，%ds 后下一轮"
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


def normalize_symbol(symbol):
    """剥掉文档里常见的 s_ 前缀 + 按形态归一化大小写，返回裸代码。

    **大小写规则是非对称的，别「顺手统一成一种」** —— 依据是实测：

        SH000300  → EMPTY   ← 大写 A 股代码取不到
        sh000300  → 34 字段正常
        Sz399006  → EMPTY
        s_Sh000300 → EMPTY
        hf_NQ     → 15 字段正常（新浪标名「纳斯达克指数期货」）
        hf_nq     → EMPTY   ← 小写外盘代码取不到
        int_nikkei → 4 字段正常（本来就是全小写）

    所以只有匹配 ``^(sh|sz|bj)\\d{6}$``（大小写不敏感判定）的 A 股代码转小写；其余
    （``hf_*`` / ``int_*`` / ``hk*`` 等）**原样保留大小写**。s_ 前缀大小写不敏感地剥。
    """
    code = (symbol or "").strip()
    if code[:2].lower() == "s_":
        code = code[2:]
    parts = (code[:2].lower(), code[2:])
    if parts[0] in A_SHARE_PREFIXES and len(parts[1]) == 6 and parts[1].isdigit():
        return code.lower()
    return code


def parse_symbols(value):
    """解析标的并验证 ASCII、长度和数量约束。

    大小写走 normalize_symbol 的**非对称规则**（A 股转小写、外盘保大小写），不是一刀切
    lower() —— 详见该函数 docstring 里的实测对照。

    长度上限是 MAX_SYMBOL_BYTES(16) 而不是设备端 name 的 9 字节：取数层把代码当
    name 推给设备，但**只有没配简称时才会原样上屏**（见 require_aliases_for_long_codes）。
    """
    symbols = []
    for item in str(value or "").split(","):
        symbol = normalize_symbol(item)
        if not symbol:
            raise ValueError("--symbols 含空的标的项")
        if len(symbol.encode("ascii", "ignore")) != len(symbol):
            raise ValueError("标的 %r 含非 ASCII 字符" % symbol)
        if not 1 <= len(symbol.encode("ascii")) <= MAX_SYMBOL_BYTES:
            raise ValueError("标的 %r 长度必须为 1..%d 字节"
                             % (symbol, MAX_SYMBOL_BYTES))
        if any(ord(ch) < 32 or ord(ch) > 126 for ch in symbol):
            raise ValueError("标的 %r 含不可打印 ASCII" % symbol)
        symbols.append(symbol)
    if not symbols:
        raise ValueError("--symbols 不能为空")
    if len(symbols) > MAX_ROWS:
        raise ValueError("--symbols 最多 5 个标的")
    return symbols


def parse_symbol_names(symbols, raw):
    """按位置解析简称表 → {代码: 简称}；空项表示这只沿用原代码。

    数量必须与 --symbols 一致：按位错配会把简称贴到错的标的上（比如只给前 3 个
    配简称却按顺序顶替 5 个），宁可启动失败也不让屏上出现错误的简称。留空项是
    表达「只有这几只想简称」的正规写法。
    """
    text = str(raw or "").strip()
    if not text:
        return {}
    items = [item.strip() for item in text.split(",")]
    if len(items) != len(symbols):
        raise ValueError("SYMBOL_NAMES 有 %d 项，与 SYMBOLS 的 %d 个标的不一致"
                         % (len(items), len(symbols)))
    aliases = {}
    owner = {}
    for symbol, alias in zip(symbols, items):
        if not alias:
            continue
        if len(alias.encode("ascii", "ignore")) != len(alias):
            raise ValueError("简称 %r 含非 ASCII 字符" % alias)
        if not 1 <= len(alias.encode("ascii")) <= 9:
            raise ValueError("简称 %r 长度必须为 1..9 字节（设备端同名限制）" % alias)
        if any(ord(ch) < 32 or ord(ch) > 126 for ch in alias):
            raise ValueError("简称 %r 含不可打印 ASCII" % alias)
        if alias in owner:
            eprint("警告：简称 %r 重复（%s 与 %s 用了同一个），屏上两行会显示一样"
                   % (alias, owner[alias], symbol))
        owner[alias] = symbol
        aliases[symbol] = alias
    return aliases


def require_aliases_for_long_codes(symbols, aliases):
    """超过设备端 name 上限的代码必须有简称，否则早失败。

    这不是语法问题（1..MAX_SYMBOL_BYTES 字节的语法校验在 parse_symbols 里），而是
    「上屏名从哪来」：没配简称时行名就是代码本身，int_nikkei(10) 会被设备 400 拒。
    """
    for symbol in symbols:
        if len(symbol.encode("ascii")) > DEVICE_NAME_MAX and symbol not in aliases:
            raise ValueError(
                "代码 %s 有 %d 字节，超过设备端 name 的 %d 字节上限"
                "（SYMBOLS 长度必须为 1..%d 字节；没有简称时才会原样上屏）"
                % (symbol, len(symbol.encode("ascii")), DEVICE_NAME_MAX,
                   MAX_SYMBOL_BYTES))
    return aliases


def apply_aliases(rows, aliases):
    """把行名换成用户配置的简称。取数层只认代码，简称属于排版，故在推送前替换。"""
    if not aliases:
        return rows
    return [dict(row, name=aliases.get(row["name"], row["name"])) for row in rows]


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


@dataclass(frozen=True)
class SinaQuote:
    """新浪一条行情。percent 是设备唯一要的量（涨跌幅 %）。

    下面 6 个字段是本轮「**只解析、暂时不用**」的补充项，全部带默认值、不参与任何
    计算，只在 --stock-dry-run 里打印出来（这是这一轮改动的收益：能看见）。

    **填值纪律**：只有**算术自校验过**的才算数（b_ 的 f[9]、gb_ 的 f[26]、hk* 的
    f[3]、hf_ 的 f[7]），拿不到或存疑的一律留 **""**，**绝不把推测当实测**填进去。
    判断依据是「现价 −(涨跌幅×昨收) ≈ 涨跌额」这类算术自校验能不能闭合。
    """
    code: str            # 规范化后的代码（不带 s_ 前缀）
    name: str            # 新浪返回的中文名，只进日志/--stock-dry-run，不上屏
    point: float         # 点位；简版模式下 0 是合法的（开盘前深市指数点位为 0）
    change_amount: float # 涨跌额
    percent: float       # 涨跌幅 %，设备推的就是它
    date: str = ""       # 行情日期，只有完整版行才有
    # ↓ 以下为「只解析、暂时不用」的补充字段，默认全空（拿不到就留空，不填推测值）
    open: str = ""       # 今开；gb_ 的 f[5] 语义明确，其余形态存疑/无此字段
    high: str = ""       # 最高
    low: str = ""        # 最低
    prev_close: str = "" # 昨收（**只填算术自校验过的**：b_ f[9] / gb_ f[26] / hk* f[3] / hf_ f[7]）
    volume: str = ""     # 成交量
    amount: str = ""     # 成交额
    quote_time: str = "" # 行情时间（只进 dry-run；b_ f[7] / gb_ f[3]）
    status: str = ""     # 形态自报的说明文字（如 fx_ f[13]「此行情由新浪财经计算得出」）


def _quote_field(fields, index):
    """取第 index 个字段的浮点值；越界或非法一律抛，由调用方统一转成跳过原因。"""
    if index >= len(fields):
        raise ValueError("字段不足")
    return float(fields[index].strip())


def _optional_fields(spec):
    """按 ``{字段名: (fields, 下标)}`` 取「只解析、暂时不用」的补充字段。

    **一律返回原始字符串**（不转 float），因为这些值本轮不参与任何计算，只在
    --stock-dry-run 里打印给人看。越界、空串、纯空白一律返回 **""** —— 这是刻意的：
    「没解析出来」和「解析出来是 0」在 dry-run 里必须看得出区别，所以宁可留空，
    也**不把推测值或占位 0 填进去**（见 SinaQuote 的填值纪律）。
    """
    result = {}
    for key, (fields, index) in spec.items():
        value = fields[index].strip() if index < len(fields) else ""
        result[key] = value if value else ""
    return result


def is_fx_symbol(code):
    """这个代码是不是外汇/汇率形态（3a 裸代码或 3b ``fx_`` 前缀）。

    **3a 裸代码没有前缀**（USDCNY / DINIW / CNYUSD），所以**不能靠前缀分流**，
    只能靠代码形状：整串都是 ASCII 大写字母、**不含任何数字**、长度 5~6。

    依据与「不会误伤」的理由：A 股代码**必然含数字**（``sh000300`` / ``sz399006``
    / ``bj430047``），所以「无数字 + 全大写字母」这条判据不可能命中任何 A 股代码；
    长度 5~6 也覆盖实测的 DINIW(5) / USDCNY(6) / CNYUSD(6)。而 ``fx_`` 前缀那
    一支（fx_susdcny / fx_susdjpy）用前缀判更直接。

    **这条判据必须在 A 股完整版分支之前生效**，否则 USDCNY 会被当成
    「昨收 f[2] / 现价 f[3]」算出 +0.12% 这种看着挺像样的垃圾。
    """
    code = code or ""
    if code.lower().startswith(FX_PREFIX):
        return True
    return (FX_BARE_MIN_LEN <= len(code) <= FX_BARE_MAX_LEN and
            code.isascii() and code.isalpha() and code.isupper())


def _fx_reason(code):
    """是外汇形态就返回中文跳过原因；不是返回 ""。

    **本轮硬性决策：识别出外汇形态就跳过，绝不猜一个涨跌幅。** 理由：15 秒差分实测
    三个形态里只有 DINIW 的 f[1]/f[2]/f[8] 在变，其余字段全静态 —— **无法可靠定位
    哪个是昨收**，fx_ 的 [10][11][12] 语义也定不下来。若误把 f[2] 当昨收、f[3] 当
    现价，USDCNY 会算出 (6.7136−6.7055)/6.7055 = **+0.12%** 这种**看着挺像样的
    垃圾** —— 正因它看着合理，最容易被误当成「解析成功」而静默上屏。
    """
    if not is_fx_symbol(code):
        return ""
    if code.lower().startswith(FX_PREFIX):
        return FX_SKIP_REASON_FX
    return FX_SKIP_REASON_BARE


class SinaQuotes:
    """新浪行情客户端：按 key 前缀分**五种形态**，一次请求混写。

    - 简版（``s_<code>``，仅 SIMPLE_PREFIXES 支持）6 字段，只要涨跌幅，正是设备要的
    - 完整版（``<code>``，A 股裸代码）多一个日期字段，节假日/停市兜底判定靠它 ——
      **五种形态里只有它参与日期投票**
    - **海外指数**（``int_`` 前缀，实测 ``s_int_nikkei`` 恒为空串，即无简版形态）
      只有 4 字段：名称,现价,涨跌额,涨跌幅% —— fields[3] 同样是直接给的百分比
    - **外盘期货**（``hf_`` 前缀，如 ``hf_NQ``/``hf_ES``/``hf_XAU``）15 字段
      （``hf_XAU`` 实测 14，尾部少一个）：[0]现价 [7]昨收 [12]日期 [13]中文名。
      **没有现成涨跌幅**，自己算 (f[0]−f[7])/f[7]*100；f[12] 的日期**不参与**
      日期投票（外盘在 A 股节假日照常交易，混进投票会把节假日误判成开市）
    - **港股**（``hk*`` 前缀，如 ``hkHSI``/``hk00700``/``hk09988``）19 字段：
      [0]英文名 [1]中文名 [6]现价 [7]涨跌额 **[8]涨跌幅% 直给** [17]日期(斜杠)
      [18]时间。f[8] 与 int_ 的 f[3] 一样是现成百分比，**不要自算**（hf_ 才要）。
      有效性只认 f[0] 非空 + f[8] 可解析，**不拿现价当门槛**（盘前价格 0 合法）。
      f[17] 的斜杠格式日期**不参与**日期投票（见 _parse_quote 的注释）
    - **海外市场指数**（``b_`` 前缀，b_DAX/b_FTSE/b_CAC/b_NKY/b_SPX，**至少延迟
      15 分钟**）：13 字段但 b_SPX 只有 6 字段 → 不按字段数判形态。f[3] 是现成
      涨跌幅直给，**不自算**（同 int_，与 hf_ 相反）
    - **美股**（``gb_`` 前缀，gb_bili/gb_ixic/gb_dji/gb_aapl）：gb_bili 36 字段而
      gb_dji 只有 30 字段 → 同样不按字段数判形态。f[2] 是现成涨跌幅直给，**不自算**
    - **外汇/汇率**（3a 裸代码 USDCNY/DINIW/CNYUSD 11 字段，3b ``fx_`` 前缀
      fx_susdcny/fx_susdjpy 18 字段）：**识别出来就跳过，绝不推算涨跌幅**（涨跌
      字段的语义未验证，猜错会得到看着挺像样的垃圾）。3a 没有前缀，靠「整串都是
      ASCII 大写字母、无数字、长度 5~6」识别（A 股代码必含数字，故不误伤）
    - **已知未支持**（UNSUPPORTED_PREFIXES）：``rt_`` 延时行情（实测 25 字段，
      位置同 hk* 但语义是延时），命中就给中文原因跳过，绝不放行到别的分支
    - **大小写非对称，别统一**（实测 ``SH000300``/``hf_nq``/``hsi`` 都是空串，
      只有 ``sh000300``/``hf_NQ``/``hkHSI`` 有数据）：A 股代码转小写，其余形态
      原样保大小写 —— 见 normalize_symbol。判定/归并用小写副本，**请求 URL 用
      保大小写的代码**。
    - 一个 URL 里混着写（``list=s_sh000300,sh000300`` 实测两行都返回），所以每拍
      只发一次请求：**每个标的都同时捎带完整版**，多出来的行只用来投票日期
    - 「哪些前缀支持简版」按 SIMPLE_PREFIXES 判断，依据是**逐个实测**（见常量处
      注释），不外推。列表里**没有任何**支持简版的代码时直接走纯完整版形态，
      不做「简版 → 回退」两轮 dance —— 那种情况下简版必然全废，白跑一倍请求、
      每拍刷一堆重试噪音。
    - 行情日期取**所有 A 股完整版行里最新的那一天**，不是第一个：停牌标的会报陈旧
      日期（实测某标的报 7 天前而其余都是今天），只信一个会把正常交易日误判
      成休市。节假日时所有标的都报上一交易日，max 仍是那天 ≠ 今天，判定不变。
    """

    @staticmethod
    def supports_simple(code):
        """该代码有没有 s_ 简版形态。实测只有 sh/sz 有（bj 没有：s_bj430047 空串）。

        判定一律对小写副本做（用户可能写 SH000300），但 **build_url 发出去的代码是
        保大小写的原样** —— 这是 normalize_symbol 那条非对称规则的另一半。
        """
        code = (code or "").lower()
        if any(code.startswith(prefix) for prefix in NOSIMPLE_PREFIXES):
            return False
        if any(code.startswith(prefix) for prefix in UNSUPPORTED_PREFIXES):
            return False
        return any(code.startswith(prefix) for prefix in SIMPLE_PREFIXES)

    def __init__(self, base_url=DEFAULT_SINA_URL, timeout=15.0, insecure=False,
                 retries=2):
        self.base_url = base_url
        self.timeout = timeout
        self.insecure = insecure
        self.retries = retries
        self.headers = {"User-Agent": USER_AGENT, "Referer": SINA_REFERER}
        self.urls = []          # 实际发过的 URL，--stock-dry-run 打印用

    @staticmethod
    def normalize(symbol):
        """剥 s_ 前缀 + 按形态归一化大小写，返回裸代码（委托 normalize_symbol）。"""
        return normalize_symbol(symbol)

    def build_url(self, codes, simple=True):
        """构造请求 URL。simple=True 是简版为主 + 每个标的捎带完整版，False 是纯完整版。

        只有 supports_simple 的代码才加 s_ 前缀：int_ 之类实测 s_ 恒为空串，加了
        纯浪费。两种模式都带全部代码的完整版，区别只在前面那一段 s_ 简版 —— 也就是
        「简版全废时回退纯完整版」这条退路必须保留：回退请求里绝不能再出现 s_。
        """
        simple_codes = [code for code in codes if self.supports_simple(code)]
        request = (["s_" + code for code in simple_codes] if simple else []) + list(codes)
        return make_sina_url(self.base_url, request)

    def fetch(self, symbols):
        """对外主入口 → (rows, data_date)。rows 是 [{"name": code, "change": percent}]。"""
        quotes, data_date = self.fetch_quotes(symbols)
        return ([{"name": quote.code, "change": quote.percent} for quote in quotes],
                data_date)

    def fetch_quotes(self, symbols):
        """同 fetch，但返回带中文名/点位/涨跌额的 SinaQuote，供排障工具打印。"""
        codes = [self.normalize(symbol) for symbol in symbols]
        # 全员不支持简版（如纯 int_ 列表）时直接走纯完整版形态：那一轮简版必然
        # 「无有效行情」，走回退只是白跑一倍请求 + 一屏重试噪音。
        if not any(self.supports_simple(code) for code in codes):
            return self._require_quotes(*self._fetch_once(codes, self.build_url(codes, False)))
        quotes, data_date = self._fetch_once(codes, self.build_url(codes, True))
        if not quotes:
            # 简版接口行为变了或混用被拒：退回今天这套纯完整版，别让整条链路挂掉。
            eprint("简版无有效行情，回退完整版")
            quotes, fallback_date = self._fetch_once(
                codes, self.build_url(codes, False))
            data_date = data_date or fallback_date
        return self._require_quotes(quotes, data_date)

    @staticmethod
    def _require_quotes(quotes, data_date):
        if not quotes:
            raise RuntimeError("Response error: 未解析到有效行情")
        return quotes, data_date

    def _fetch_once(self, codes, url):
        """一次 URL 带原有重试语义；返回 ([], None) 表示 200 但没解析出有效行。"""
        self.urls.append(url)
        last_error = None
        for attempt in range(1, self.retries + 2):
            try:
                code, raw = self._transport(url)
            except Exception as ex:
                last_error = "Network error: %s" % ex
                eprint("新浪请求失败 (attempt %d): %s" % (attempt, last_error))
                time.sleep(1)
                continue
            if code == 200:
                quotes, data_date = self._parse(raw, codes)
                if quotes:
                    return quotes, data_date
                last_error = "Response error: 未解析到有效行情"
                eprint("新浪响应没有可用行情 (attempt %d)" % attempt)
                time.sleep(1)
                continue
            last_error = "HTTP %d: %s" % (code, (raw or "")[:200])
            eprint("新浪 HTTP %d (attempt %d)" % (code, attempt))
            if code == 403:
                break
            time.sleep(1)
        if last_error and not last_error.startswith("Response error"):
            raise RuntimeError(last_error)
        return [], None

    def _transport(self, url):
        """必须带 Referer、*/* Accept 并按 gb18030 解码。"""
        return http_request(url, headers=self.headers, timeout=self.timeout,
                            insecure=self.insecure, encoding="gb18030",
                            accept="*/*")

    def _parse(self, raw, codes):
        """逐行解析 → (按请求序排好的 SinaQuote 列表, 行情日期)。

        行序必须等于请求序：被丢弃的行不能让后面的行上移，否则屏上顺序会跳。

        行情日期是**多标的投票，取最新的一天**：每个标的都独立上报自己的完整版
        日期，最新的那个才是「当前市场交易日」的最佳估计。三种情形都成立：

        - 单个标的停牌 → 它报陈旧日期（实测 7 天前），被更晚的日期盖掉，不会
          把正常交易日误判成休市；
        - 节假日 → 没有任何标的会报今天，max 仍是上一交易日，≠ 今天 → 正确休市；
        - 因此 max 不会掩盖真正的休市。
        """
        # 归并字典一律用**小写 key**（新浪 key 大小写敏感，但形态判定与行序只关心
        # 「是不是同一个代码」）；quote.code 保留请求时的原样大小写。
        wanted = {code.lower() for code in codes}
        simple, full, reasons, dates = {}, {}, {}, []
        for line in (raw or "").splitlines():
            parts = self._split_line(line)
            if parts is None:
                continue
            key, data_str = parts
            low = key.lower()
            bare = low[2:] if low.startswith("s_") else low
            quote, reason = self._parse_quote(key, data_str)
            if quote is not None:
                if low.startswith("s_"):
                    simple[quote.code.lower()] = quote
                else:
                    full[quote.code.lower()] = quote
                    if quote.date:
                        # **只有 A 股完整版口径的日期会走到这里**（_parse_quote 的
                        # else 分支）：int_ / hf_ / hk* / b_ / gb_ 五种非 A 股形态
                        # 一律把 date 填成 ""，所以永远不参与投票（外汇根本不出行）。
                        # 理由见各自分支的注释 —— 交易日历不同 + 日期格式不同，
                        # 混进 max() 会误判。
                        # YYYY-MM-DD 字典序即时间序，直接取 max。
                        dates.append(quote.date)
            elif low.startswith("s_") or bare in wanted:
                # 完整版行与简版行重复，失败原因归到同一个代码上。
                reasons[bare] = reason
        data_date = max(dates) if dates else None
        quotes = []
        for code in codes:
            # 同一代码两种行都在响应里时以简版为准（涨跌幅已是百分比），
            # 完整版只贡献 date —— 每个代码只能出一行。
            low = code.lower()
            quote = simple.get(low) or full.get(low)
            if quote is None:
                eprint("跳过 %s：%s" % (code, reasons.get(low, "空行情或价格无效")))
                continue
            quotes.append(quote)
            if len(quotes) >= MAX_ROWS:
                break
        return quotes, data_date

    @staticmethod
    def _split_line(line):
        """`var hq_str_<key>="<data>";` → (key, data)；不合法返回 None。

        key **保留原样大小写**（只清首尾空白）：`hf_NQ` 与 `hf_nq` 是两个不同的新浪
        key，后者返回空串，所以这里下大写会把「用户写错大小写」这个事实抹掉。形态
        判定与字典归并一律用 key.lower()，见 _parse。
        """
        line = (line or "").strip()
        prefix = "var hq_str_"
        if not line.startswith(prefix) or '"' not in line:
            return None
        body = line[len(prefix):]
        if "=" not in body or not body.endswith(";"):
            return None
        key, quoted = body.split("=", 1)
        if len(quoted) < 2 or not quoted.startswith('"') or not quoted.endswith('";'):
            return None
        key = key.strip()
        if not key:
            return None
        return key, quoted[1:-2]

    @staticmethod
    def _parse_quote(key, data_str):
        """解析一行 → (SinaQuote, 跳过原因)。

        形态判定**按 key 的小写副本带什么前缀分流，不按字段数** —— 完整版的指数和
        个股是同一套布局（沪市 34 字段 / 深市 33 字段，尾部多一个空字段），按字段数
        分「个股 vs 指数」会把指数误判成个股，只是下面那个公式对两者都成立，
        才碰巧没出错。key 本身保大小写（`hf_NQ` 与 `hf_nq` 是不同的新浪 key），
        只有分流与归并用 lower()：

        - 简版（key 带 s_）6 字段：名称,点位,涨跌额,涨跌幅%,成交量(手),成交额(万)。
          fields[3] **直接就是涨跌幅百分比**，不用自己除。开盘前深市指数
          （创业板指/国证2000/深证成指）点位就是 0.00，所以只能拿 fields[3]
          当有效性门槛，点位 0 是合法值。
        - 海外指数（key 带 int_）**4 字段**：名称,现价,涨跌额,涨跌幅%。实测
          ``int_nikkei`` 4 字段（44946.64,-408.35,-0.90）而 ``s_int_nikkei``
          返回空串 —— 这类标的没有 s_ 简版形态，涨跌幅同样是 fields[3] 直接给的
          （恒生是 3 位小数 -0.600，交给 round_change 归一到 2 位）。**不要**对它套
          A 股完整版的 (f[3]−f[2])/f[2] 公式：那里 fields[2] 是昨收，这里是涨跌额，
          套上去会算出 +0.02% 这种看着像样的垃圾。它没有 A 股那种行情日期
          —— 不参与日期投票，节假日兜底只对 A 股代码有效。
        - 外盘期货（key 带 hf_）**15 字段**（``hf_XAU`` 实测 14，尾部少一个，所以
          判字段数会漏掉它）：[0]现价 [1]空 [2]买价 [3]卖价 [4]最高 [5]最低
          [6]时间 [7]昨收 [8]开盘 [9]持仓量 [10]? [11]? [12]日期 [13]中文名
          [14]'0'。**它没有现成的涨跌幅百分比**（不像 int_ 的 fields[3] 直给），
          必须自己算 (f[0]−f[7])/f[7]*100，且要求 f[7] > 0。示例 hf_NQ
          30507.430 / 昨收 30566.250 → −0.19%。f[12] 的日期**故意填 "" 不参与
          A 股日期投票**：外盘期货在 A 股节假日照常交易，混进投票会让节假日被
          误判成开市。名称取 f[13]（中文，只进日志/--stock-dry-run）；取不到也
          **不丢行** —— 有效性只认 f[0] 现价与 f[7] 昨收，名字对设备毫无用处。
        - 海外市场指数（key 带 b_，如 b_DAX/b_FTSE/b_CAC/b_NKY/b_SPX）：**至少延迟
          15 分钟**（社区文档原文如此）。b_DAX 13 字段但 **b_SPX 只有 6 字段**，
          所以不按字段数判形态。f[3] 是**现成涨跌幅**，直接用（算术自校验：
          b_DAX 25374.42+34.22=25408.64=f[9] 即昨收，−34.22/25408.64=−0.13%=f[3]；
          b_NKY 65219.72+657.90=65877.62=f[9]，−657.90/65877.62=−1.00%=f[3]）。
          f[4]/f[5] 实测在同一请求里时而是 '9/26/2025'+'2:12 AM'、时而是
          '2025-09-26'+'14:12:00'、时而全空 → 语义未定，**不解析**；f[8]/f[10]/f[11]
          疑似今开/最高/最低但**只是推测，未确证 → 不填**。f[6] 有日期但**不投票**。
        - 美股（key 带 gb_，如 gb_bili/gb_ixic/gb_dji/gb_aapl）：**gb_bili 36 字段
          而 gb_dji 只有 30 字段**，同样不按字段数判形态。**f[2] 是现成涨跌幅**，
          直接用（算术自校验：gb_bili 15.14−0.245=14.895=f[26] 即昨收，
          0.245/14.895=1.64%=f[2]；gb_aapl 338.40−(−2.67)=341.07=f[26]，
          −2.67/341.07=−0.78%=f[2]）。f[3] 是**日期时间合一**串
          '2026-09-29 08:02:08'，格式与 A 股不同 + 美股交易日历不同 → **不投票**。
        - 外汇/汇率（**两种形态**）：3a 是**没有前缀的裸代码**（USDCNY/DINIW/CNYUSD
          11 字段），3b 是 ``fx_`` 前缀（fx_susdcny/fx_susdjpy 18 字段）。**两者都
          识别出来就跳过**，绝不推算涨跌幅（理由见 _fx_reason）。
        - 港股（key 带 hk，实测 19 字段，如 hkHSI/hk00700/hk09988/hkHSCEI）：
          [0]英文名(ASCII) [1]中文名 [2]今开 [3]昨收 [4]最高 [5]最低 [6]现价
          [7]涨跌额 **[8]涨跌幅%（直给，不需自算）** [9]买价 [10]卖价 [11]成交量
          [12]成交额 [13]? [14]? [15]52周最高 [16]52周最低 [17]日期 f[18]时间。
          算术自校验：(f[6]−f[3])/f[3] = (24507.299−24642.510)/24642.510 = −0.5487%
          与 f[8]=−0.549 吻合，所以 f[8] 就是现成百分比（**与 hf_ 相反**：hf_ 必须
          自算，hk* 绝不能自算）。有效性只认 f[0] 非空 + f[8] 可解析，**不拿现价
          f[6] 当门槛**（盘前/停牌价格可能是 0，同「开盘前深市指数点位 0 是合法值」）。
          名称取 f[1] 中文名（只进日志/--stock-dry-run；f[0] 是 ASCII 英文名，以想
          直接上屏可用它），取不到也**不丢行**。f[17] 的日期是**斜杠格式**
          '2026/09/29' 且**故意不参与日期投票**（交易日历不同 + 格式不同）。
          注意这里**不按字段数判形态**：只要能读到 f[8] 就按 hk 解析，`rt_` 延时
          行情（25 字段、位置同 hk*）由 UNSUPPORTED_PREFIXES 在更早一步拦下。
        - **已知未支持**：`rt_` 延时行情（UNSUPPORTED_PREFIXES，见常量处注释）。
        - 完整版（key 不带上述前缀，**唯一参与日期投票的形态**）：fields[2]=昨收、
          ⚠️ **外汇裸代码（USDCNY 等）本来也会落进这里**，所以外汇判别被放在本函数
          最开头（_fx_reason），先于所有形态分支 —— 详见 is_fx_symbol 的注释。
          fields[3]=最新价/最新点位，涨跌幅 = (fields[3]−fields[2])/fields[2]
          （个股与指数同一公式）；fields[30]=行情日期、fields[31]=行情时间，
          sh/sz 下位置一致。开盘前 fields[3] 为 0 会算出 −100%，所以这里保留
          昨收/现价 > 0 的守卫。
        """
        fields = (data_str or "").split(",")
        low = key.lower()
        is_simple = low.startswith("s_")
        code = key[2:] if is_simple else key
        # **外汇/汇率必须排在 A 股完整版分支之前**（见 _fx_reason 的调用位置与
        # is_fx_symbol 的判据注释）：USDCNY/DINIW 是**没有前缀的裸代码**，不是按
        # 前缀分流就能认出来的形态，不在这里拦下就会掉进 else 完整版分支被当成
        # 「昨收 f[2] / 现价 f[3]」算出 +0.12% 这种看着挺像样的垃圾。
        fx_reason = _fx_reason(code)
        if fx_reason:
            return None, fx_reason
        name = fields[0].strip() if fields else ""
        if not name:
            return None, "空行情"
        extra = {}                 # 「只解析、暂时不用」的补充字段（见 SinaQuote）
        try:
            if any(low.startswith(prefix) for prefix in UNSUPPORTED_PREFIXES):
                reason = UNSUPPORTED_REASONS.get(low[:2], "形态暂不支持")
                return None, reason
            elif is_simple:
                point = _quote_field(fields, 1)
                change_amount = _quote_field(fields, 2)
                percent = _quote_field(fields, 3)   # 简版涨跌幅已是百分比
                date = ""
            elif low.startswith("int_"):
                # 海外指数：4 字段，涨跌幅同为 fields[3] 直给。
                if len(fields) <= 3:
                    return None, ("无简版形态字段不足（需要 名称,现价,涨跌额,"
                                  "涨跌幅%% 四项）")
                point = _quote_field(fields, 1)
                change_amount = _quote_field(fields, 2)
                percent = _quote_field(fields, 3)
                date = ""                        # 该形态没有行情日期字段
            elif low.startswith("b_"):
                # 海外市场指数（b_DAX / b_FTSE / b_CAC / b_NKY / b_SPX），**至少
                # 延迟 15 分钟**（社区文档原文如此）。13 字段，但 b_SPX 只有 6 字段
                # → **绝不按字段数判形态**，只看 b_ 前缀。
                #   [0]名称 [1]现价/点位 [2]涨跌额 **[3]涨跌幅%（直给）** [4]? [5]?
                #   [6]日期 [7]时间 [8]? [9]昨收 [10]? [11]? [12]?
                # 算术自校验（f[9]=昨收 的依据）：b_DAX 25374.42+34.22=25408.64=f[9]；
                #   b_NKY 65219.72+657.90=65877.62=f[9]。且 -34.22/25408.64=-0.13%
                #   = f[3]、-657.90/65877.62=-1.00% = f[3] → **f[3] 是现成涨跌幅，
                #   直接用，绝不自算**（同 int_，与 hf_ 相反）。
                # f[4]/f[5] 实测在同一请求里时而是 '9/26/2025'+'2:12 AM'、时而是
                #   '2025-09-26'+'14:12:00'、时而全空 → 语义未定，**不解析**。
                # f[8]/f[10]/f[11]（疑似今开/最高/最低）只是推测，**不填**。
                if len(fields) <= 3:
                    return None, ("b_ 形态字段不足（需要 名称,现价,涨跌额,"
                                  "涨跌幅%% 四项）")
                point = _quote_field(fields, 1)
                change_amount = _quote_field(fields, 2)
                percent = _quote_field(fields, 3)
                # f[6] 确实带日期，但**故意不参与 A 股日期投票**（海外市场交易日历
                # 不同，且数据本身就是延迟 15 分钟的，混进投票会误判节假日）。
                date = ""
                extra = _optional_fields({
                    "prev_close": (fields, 9),    # 算术自校验过
                    "quote_time": (fields, 7),
                })
            elif low.startswith("gb_"):
                # 美股（gb_bili / gb_ixic / gb_dji / gb_aapl）：**gb_bili 36 字段
                # 而 gb_dji 只有 30 字段** → 同样不按字段数判形态。
                #   [0]名称 [1]最新价 **[2]涨跌幅%（直给）** [3]日期与时间**合一**
                #   [4]涨跌额 [5]今开 [6]最高 [7]最低 [8]52周高 [9]52周低
                #   [10]成交量 [11]成交量(另一口径) [12]成交额 [24][25]EDT 时间串
                #   [26]昨收 [28]'1' [29]'2026' [30..35]?
                # 算术自校验（f[26]=昨收 的依据）：gb_bili 15.14−0.245=14.895=f[26]；
                #   gb_aapl 338.40−(−2.67)=341.07=f[26]。且 0.245/14.895=1.64%
                #   = f[2]、−2.67/341.07=−0.78% = f[2] → **f[2] 是现成涨跌幅**。
                # 注意社区文档表格里那个 1.176 是笔误，样例值 1.76 才对，别照抄。
                if len(fields) <= 2:
                    return None, "gb_ 形态字段不足（需要 名称,最新价,涨跌幅% 三项）"
                point = _quote_field(fields, 1)
                percent = _quote_field(fields, 2)
                # f[3] 是 '2026-09-29 08:02:08' 这种**日期时间合一**串，格式与 A 股的
                # '2026-09-29' 不同；美股交易日历也不同 → 两条理由都不参与投票。
                date = ""
                extra = _optional_fields({
                    "prev_close": (fields, 26),   # 算术自校验过
                    "open": (fields, 5),
                    "high": (fields, 6),
                    "low": (fields, 7),
                    "quote_time": (fields, 3),    # 日期时间合一，原样带回只进 dry-run
                    "volume": (fields, 10),
                    "amount": (fields, 12),
                })
                # f[4] 涨跌额只用于 dry-run 显示，缺失不该让整行丢掉。
                try:
                    change_amount = _quote_field(fields, 4)
                except (IndexError, ValueError):
                    change_amount = 0.0
            elif low.startswith("hf_"):
                # 外盘期货 15/14 字段：涨跌幅自算，只认 f[0] 现价与 f[7] 昨收。
                if len(fields) <= 7:
                    return None, "hf_ 形态字段不足（需要 现价,昨收 两项）"
                price = _quote_field(fields, 0)
                previous_close = _quote_field(fields, 7)
                if (not math.isfinite(previous_close) or
                        not math.isfinite(price) or previous_close <= 0):
                    return None, "hf_ 形态现价或昨收无效"
                point = price
                change_amount = price - previous_close   # 期货不给现成涨跌额，自算
                percent = (price - previous_close) / previous_close * 100.0
                # f[12] 的日期不参与 A 股日期投票（外盘在 A 股节假日照常交易）。
                date = ""
                # f[13] 是中文名，取不到也**不因此丢行**（只进日志，不上屏）。
                if len(fields) > 13 and fields[13].strip():
                    name = fields[13].strip()
                extra = _optional_fields({
                    "prev_close": (fields, 7),    # 涨跌幅就是拿它自算的，语义已确证
                    "open": (fields, 8),
                    "high": (fields, 4),
                    "low": (fields, 5),
                    "quote_time": (fields, 6),
                })
            elif low.startswith("hk"):
                # 港股 19 字段：涨跌幅是 f[8] **直给的百分比**，不需自算（与 hf_ 相反）。
                # 算术自校验 hkHSI：(f[6]−f[3])/f[3] = −0.5487% 与 f[8]=−0.549 吻合。
                # 有效性只认 f[0] 英文名（身份标识）非空 + f[8] 能解析成有限数；**不要**
                # 拿现价 f[6] 当门槛 —— 与「开盘前深市指数点位 0 是合法值」同一条原则，
                # 盘前/停牌时价格可能是 0 而涨跌幅字段仍是有效值。
                if len(fields) <= 8:
                    return None, "hk_ 形态字段不足（需要 英文名,涨跌幅% 两项）"
                percent = _quote_field(fields, 8)
                # f[0] 已是英文名（进 name 变量，f[1] 中文名取到才覆盖，只进日志不上屏）。
                # 以后若想让英文名直接上屏，取 f[0] 即可 —— 它是 ASCII（设备端要求）。
                if len(fields) > 1 and fields[1].strip():
                    name = fields[1].strip()
                # f[6] 现价 / f[7] 涨跌额只做日志用（--stock-dry-run 打印），取不到记 0。
                try:
                    point = _quote_field(fields, 6)
                    change_amount = _quote_field(fields, 7)
                except (IndexError, ValueError):
                    point = 0.0
                    change_amount = 0.0
                # f[17] 的日期（'2026/09/29' 斜杠格式）**故意填 "" 不参与日期投票**，
                # 两条理由：① 港股与 A 股交易日历不同（各有各的假期），拿港股日期参与
                #    「今天是不是交易日」会误判；② 斜杠格式和 A 股的 '2026-09-29' 混进
                #    同一个 max() 会按字符串序比错。日期投票只认 A 股完整版口径。
                date = ""
                extra = _optional_fields({
                    "prev_close": (fields, 3),    # 算术自校验过
                    "open": (fields, 2),
                    "high": (fields, 4),
                    "low": (fields, 5),
                    "volume": (fields, 11),
                    "amount": (fields, 12),
                    "quote_time": (fields, 18),
                })
            else:
                if len(fields) <= 3:
                    return None, "完整版字段不足（读不到 fields[2]/fields[3]）"
                previous_close = _quote_field(fields, 2)
                price = _quote_field(fields, 3)
                if (not math.isfinite(previous_close) or
                        not math.isfinite(price) or previous_close <= 0 or
                        price <= 0):
                    return None, "昨收或现价无效"
                point = price
                change_amount = _quote_field(fields, 4)
                percent = (price - previous_close) / previous_close * 100.0
                date = SinaQuotes._extract_date(data_str)
                extra = _optional_fields({
                    "prev_close": (fields, 2),
                    "open": (fields, 1),
                    "high": (fields, 6),
                    "low": (fields, 7),
                    "volume": (fields, 8),
                    "amount": (fields, 9),
                    "quote_time": (fields, 31),
                })
            if not math.isfinite(percent) or abs(percent) > 100000:
                return None, "涨跌幅不是有限数或越界"
            # percent 是唯一进设备 payload 的量；下面这些 extra 只给 --stock-dry-run 看。
            return SinaQuote(code, name, point, change_amount,
                             round_change(percent), date, **extra), ""
        except (IndexError, TypeError, ValueError, InvalidOperation, OverflowError):
            return None, "字段无法解析"

    @staticmethod
    def _extract_date(data_str):
        """完整版 fields[30] 的行情日期（YYYY-MM-DD），sh/sz 下位置一致。

        fields[31] 是行情时间。字段不存在或不合法返回 ""。
        """
        fields = (data_str or "").split(",")
        if len(fields) <= 30:
            return ""
        value = fields[30].strip()
        if (len(value) != 10 or value[4] != "-" or value[7] != "-" or
                any(ch not in "0123456789" for ch in value[:4] + value[5:7] + value[8:])):
            return ""
        return value


def fetch_quotes(sina_url, symbols, timeout, insecure, retries=2):
    """薄封装：取数逻辑都在 SinaQuotes 里，这里只保调用点签名与返回形状不变。"""
    return SinaQuotes(sina_url, timeout, insecure, retries).fetch(symbols)


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
    stock.add_argument("--symbol-names",
                       default=os.environ.get("SYMBOL_NAMES", ""),
                       help="与 SYMBOLS 按位置一一对应的简称（也可用 SYMBOL_NAMES）"
                            "，项数必须相同，留空项表示这只沿用代码；"
                            "例如 CSI300,CSI500,CSI2000,GEM,STAR50")
    stock.add_argument("--sina-url", default=os.environ.get("SINA_URL", DEFAULT_SINA_URL),
                       help="新浪 URL 前缀（也可用 SINA_URL）")
    stock.add_argument("--stock-dry-run", action="store_true",
                       help="只真实拉取并打印股票行情 payload，不连接设备"
                            "（用于测试取数链路；加 --demo 则用本地随机数据；"
                            "屏顶额度进度条 payload 只由调度路径生成）")
    stock.add_argument("--no-quota-bars", action="store_true",
                       help="开市时股票推送不带额度数据（屏幕顶部不画进度条）")

    policy = parser.add_argument_group("时间策略")
    policy.add_argument("--open-interval", type=int, default=15,
                        help="开市股票间隔秒数，默认 15（行情 15s 一刷）")
    policy.add_argument("--closed-interval", type=int, default=300,
                        help="非开市额度间隔秒数，默认 300")
    policy.add_argument("--balance-every", type=int, default=300,
                        help="开市额度间隔秒数，默认 300（5 分钟；单独推，"
                             "不带行情、不切屏）")
    policy.add_argument("--market-open",
                        default=os.environ.get("MARKET_OPEN", "09:20"),
                        help="开市开始 HH:MM，默认 09:20（也可用 MARKET_OPEN）")
    policy.add_argument("--market-close",
                        default=os.environ.get("MARKET_CLOSE", "15:30"),
                        help="开市结束 HH:MM，默认 15:30（半开区间：含开盘不含收盘；"
                             "也可用 MARKET_CLOSE）")
    policy.add_argument("--sleep-from",
                        default=os.environ.get("SLEEP_FROM", "20:00"),
                        help="每日休眠开始 HH:MM，默认 20:00（跨午夜到 --sleep-to；"
                             "也可用 SLEEP_FROM）")
    policy.add_argument("--sleep-to",
                        default=os.environ.get("SLEEP_TO", "08:00"),
                        help="每日休眠结束 HH:MM，默认 08:00（也可用 SLEEP_TO）")
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
                        help="额度/股票均用本地随机数据，不联网；"
                             "仍遵守开市时间窗口（只跳过 API 行情日期判定）")
    common.add_argument("--verbose", action="store_true",
                        help="连 15s 行情的成功行也打（默认只打低频状态与全部异常）")
    return parser


def validate_args(args):
    """返回 (device_base, symbols, aliases, windows, error)。

    windows 是解析好的 ((start_sec, end_sec),)，成功路径下永远有值（默认窗口）；
    提前返回的失败路径给 MARKET_WINDOWS，调用方看到 error 就不会用它。
    """
    device_base = normalize_device_base(args.device)
    try:
        symbols = parse_symbols(args.symbols)
    except ValueError as ex:
        return device_base, [], {}, MARKET_WINDOWS, str(ex)
    try:
        aliases = parse_symbol_names(symbols, args.symbol_names)
    except ValueError as ex:
        return device_base, symbols, {}, MARKET_WINDOWS, str(ex)
    try:
        # 交叉校验必须排在上面两步之后：不知道有哪些代码、哪些简称就没法判。
        require_aliases_for_long_codes(symbols, aliases)
    except ValueError as ex:
        return device_base, symbols, {}, MARKET_WINDOWS, str(ex)
    try:
        windows = parse_market_windows(args.market_open, args.market_close)
    except ValueError as ex:
        return device_base, symbols, aliases, MARKET_WINDOWS, str(ex)
    try:
        sleep_from = parse_clock(args.sleep_from, "--sleep-from/SLEEP_FROM")
        sleep_to = parse_clock(args.sleep_to, "--sleep-to/SLEEP_TO")
    except ValueError as ex:
        return device_base, symbols, aliases, windows, str(ex)
    if sleep_from == sleep_to:
        return device_base, symbols, aliases, windows, "--sleep-from/--sleep-to 不能相同"
    for name in ("timeout", "open_interval", "closed_interval", "balance_every",
                 "sleep_keepalive"):
        if getattr(args, name) <= 0:
            return (device_base, symbols, aliases, windows,
                    "--%s 必须 > 0" % name.replace("_", "-"))
    if not (args.sina_url or "").strip():
        return device_base, symbols, aliases, windows, "缺少 --sina-url（或环境变量 SINA_URL）"
    # --stock-dry-run 只测取数，不需要设备地址与 token
    if args.check or not (args.dry_run or args.stock_dry_run):
        if not device_base:
            return device_base, symbols, aliases, windows, "缺少 --device（或环境变量 DEVICE）"
        if not args.device_token:
            return (device_base, symbols, aliases, windows,
                    "缺少 --device-token（或环境变量 DEVICE_TOKEN）")
    if not (args.upstream_url or "").strip() and (args.upstream_key or args.demo):
        return device_base, symbols, aliases, windows, "缺少 --upstream-url"
    return device_base, symbols, aliases, windows, ""


def show_dry_run(endpoint, payload):
    print("[dry-run] POST %s" % endpoint)
    print(json.dumps(payload, ensure_ascii=False))


def _dash_if_empty(value):
    """dry-run 里把「没解析出来」显式画成 --，别让它和 0 混为一谈。"""
    return value if value else "--"


def stock_dry_run(args, symbols, aliases=None, windows=None):
    """真实拉取股票行情并打印 payload，全程不连接设备。

    与 ``--dry-run`` 的区别：后者跟随时间策略、且用本地随机数据代替取数；
    这里是真的请求新浪，用来单独验证取数链路、字段解析与开市判定。
    """
    aliases = aliases or {}
    windows = windows or parse_market_windows(args.market_open, args.market_close)
    quotes = None
    if args.demo:
        rows = [{"name": symbol, "change": round_change(random.uniform(-5.0, 5.0))}
                for symbol in symbols]
        data_date = None
    else:
        client = SinaQuotes(args.sina_url, args.timeout, args.insecure)
        try:
            quotes, data_date = client.fetch_quotes(symbols)
        except Exception as ex:
            for url in client.urls:
                eprint("已请求 %s" % url)
            eprint("行情失败: %s" % ex)
            return 1
        for url in client.urls:
            print("请求 URL %s" % url)
        rows = [{"name": quote.code, "change": quote.percent} for quote in quotes]

    now_bj = time.gmtime(time.time() + 8 * 3600)
    weekday = "周一至周五" if now_bj.tm_wday <= 4 else "周末"
    open_now = is_market_open(now_bj, windows=windows, data_date=data_date)
    print("北京时间 %s（%s）" % (time.strftime("%Y-%m-%d %H:%M:%S", now_bj), weekday))
    print("开市窗口 %s~%s（含开盘不含收盘）"
          % (format_hhmm(windows[0][0]), format_hhmm(windows[0][1])))
    print("行情日期 %s → 开市判定：%s" % (data_date or "(取不到)",
                                          "开市" if open_now else "休市"))
    if aliases:
        print("简称映射 %s" % ", ".join(
            "%s→%s" % (symbol, aliases.get(symbol, "(沿用代码)"))
            for symbol in symbols))
    for index, row in enumerate(rows):
        if row["change"] > 0:
            tone = "涨 → 屏上正红"
        elif row["change"] < 0:
            tone = "跌 → 屏上正绿"
        else:
            tone = "平 → 屏上中性白"
        if quotes is not None:
            quote = quotes[index]
            print("  %-9s %-8s 点位=%10.4f 涨跌额=%+9.4f 涨跌幅=%+7.2f%%"
                  % (quote.code, quote.name, quote.point, quote.change_amount,
                     quote.percent))
            # 「只解析、暂时不用」的补充字段：推给设备的 payload 里没有它们，
            # 打出来是为了能看见解析到了什么、以及哪些位置**诚实地留了空**
            # （留空 = 没确证，绝不填推测值）。
            print("    补充 今开=%s 最高=%s 最低=%s 昨收=%s 成交量=%s 成交额=%s "
                  "行情时间=%s"
                  % (_dash_if_empty(quote.open), _dash_if_empty(quote.high),
                     _dash_if_empty(quote.low), _dash_if_empty(quote.prev_close),
                     _dash_if_empty(quote.volume), _dash_if_empty(quote.amount),
                     _dash_if_empty(quote.quote_time)))
        # 推送时真正上屏的行名：配了简称就是简称，否则沿用代码。
        print("    推送行名 %-9s %+7.2f%%  %s"
              % (aliases.get(row["name"], row["name"]), row["change"], tone))
    show_dry_run("/api/v1/stock", {"rows": apply_aliases(rows, aliases)})
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


def run_stock(args, device_base, symbols, aliases=None):
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
    payload = {"rows": apply_aliases(rows, aliases or {})}
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
                      closed_interval=300, balance_every=300,
                      sleep_from="20:00", sleep_to="08:00",
                      market_open="09:20", market_close="15:30",
                      sleep_keepalive=600, loop=False, dry_run=False,
                      check=False, demo=True, self_test=False,
                      no_quota_bars=False, verbose=False,
                      symbol_names="", stock_dry_run=False)
        values.update(changes)
        return argparse.Namespace(**values)
    
    class Backend:
        def __init__(self):
            self.calls = []
            self.posts = []
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
            self.posts.append((path, body))
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

    def _raised(fn):
        """跑 fn 并返回它的 ValueError 文本；没抛则返回 ""。"""
        try:
            fn()
        except ValueError as ex:
            return str(ex)
        return ""

    def _fetch_failure(client, symbols):
        """跑 client.fetch 并同时收回异常文本与 stderr（跳过原因只打在 stderr 上）。"""
        import contextlib
        import io
        buffer = io.StringIO()
        try:
            with contextlib.redirect_stderr(buffer):
                client.fetch(symbols)
        except Exception as ex:
            return str(ex), buffer.getvalue()
        return "", buffer.getvalue()
    
    def bj_epoch(hour, minute=0, second=0, day=1):
        """北京时间 → epoch（beijing_time_at 的逆运算，不依赖本机 TZ）。"""
        return ((day - 1) * 86400 + hour * 3600 + minute * 60 + second
                - BEIJING_TZ_OFFSET_SEC)

    def bj_stamp(hour, minute=0, second=0, day=1):
        """北京时间 → is_market_open 吃的结构（1970-01-01 是周四，day=3 是周六）。"""
        return beijing_time_at(bj_epoch(hour, minute, second, day))

    # 默认起跑点必须落在默认开市窗口内：行情节拍每拍自愈，窗口外起跑会在第一拍
    # 就转休市。epoch 7200 = 北京 1970-01-01 10:00（周四，窗口内）。
    def make(start=None, aliases=None, **kwargs):
        start = bj_epoch(10) if start is None else start
        clock = VirtualClock(start)
        backend = Backend()
        scheduler = Scheduler(args_for(**kwargs), "", DEFAULT_SYMBOLS.split(","),
                               clock=clock, backend=backend, quiet=True,
                               aliases=aliases)
        return clock, backend, scheduler
    
    def stock_bodies(backend):
        return [body for path, body in backend.posts if path == "/api/v1/stock"]
    
    def row_bodies(backend):
        """只带 rows 的行情推送（额度那条不带 rows，两者互不夹带）。"""
        return [body for body in stock_bodies(backend) if "rows" in body]
    
    def quota_bodies(backend):
        """只带 balance 的额度推送（body 里没有 rows）。"""
        return [body for body in stock_bodies(backend) if "balance" in body]
    
    # 1. 开市连续股票 tick，且永不推 /api/v1/balance 抢场景。
    clock, backend, s = make(no_sleep=True)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=3)
    ok("开市 15s 股票 tick 连续触发", len(row_bodies(backend)) == 3 and
       "post/api/v1/balance" not in backend.calls)
    
    # 2. quota_due 闹钟只发额度，不切状态也不推 /balance。
    clock, backend, s = make(no_sleep=True, open_interval=3600)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)          # 先消费开市首帧股票
    backend.calls[:] = []
    clock.advance(299)
    s.run(max_events=0)          # 还没到点
    clock.advance(1)
    s.run(max_events=1)
    ok("开市 300s 额度到期只刷新进度条", s.state == State.AWAKE_OPEN and
       "fetch_balance" in backend.calls and
       "post/api/v1/balance" not in backend.calls and
       # 入市预取 1 次 + 到期这次 1 次，且两次都不带 rows
       len(quota_bodies(backend)) == 2 and
       all("rows" not in b for b in quota_bodies(backend)))
    
    # 3. 行情与额度分别推送：额度那条不带 rows，行情那条不带 balance。
    clock, backend, s = make(no_sleep=True, open_interval=15)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("额度与行情分别推送互不夹带",
       quota_bodies(backend) == [{"balance": {"progress": [50, 40, 30]}}] and
       all("rows" not in b for b in quota_bodies(backend)) and
       bool(row_bodies(backend)) and
       all("balance" not in b for b in row_bodies(backend)) and
       not any("股票+额度已推送" in line for line in s.log_lines))
    
    # 4. 额度取不到时不发额度推送（设备保留上次的条）。
    clock, backend, s = make(no_sleep=True, open_interval=15)
    backend.balance_failures = 1
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("额度未取到时不开额度推送", not quota_bodies(backend) and
       bool(row_bodies(backend)) and
       any("额度未配置或上游失败" in line for line in s.log_lines))
    
    # 5. --no-quota-bars 时开市完全不发额度（连上游都不碰）。
    clock, backend, s = make(no_sleep=True, open_interval=15, no_quota_bars=True)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    clock.advance(300)
    s.run(max_events=1)
    ok("--no-quota-bars 时开市不发额度", not quota_bodies(backend) and
       "fetch_balance" not in backend.calls and bool(row_bodies(backend)))
    
    # 6. 开市额度上游失败：留在开市，不退回额度推送。
    clock, backend, s = make(no_sleep=True, open_interval=3600)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    backend.balance_failures = 5
    backend.calls[:] = []
    backend.posts[:] = []
    clock.advance(300)
    s.run(max_events=1)
    ok("开市额度上游失败留在开市且不推额度", s.state == State.AWAKE_OPEN and
       "post/api/v1/balance" not in backend.calls and
       not any("stock" in call for call in backend.calls) and
       not quota_bodies(backend))
    
    # 7. 进入开市先补拉额度（独立一条），首帧行情随后才推。
    clock, backend, s = make(no_sleep=True, open_interval=15)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("开市预取额度先于首帧行情",
       backend.calls[0] == "fetch_balance" and
       quota_bodies(backend) == [{"balance": {"progress": [50, 40, 30]}}] and
       len(row_bodies(backend)) == 1)
    
    # 8. 休市 300s。
    clock, backend, s = make(no_sleep=True)
    s.start(State.AWAKE_CLOSED)
    s.run(max_events=2)
    ok("非开市 300s 额度节拍", backend.calls.count("post/api/v1/balance") == 2)
    
    # 9. 启动时刻已落在默认休眠区间（20:00~08:00 跨午夜，北京 00:00 在区间内）。
    clock, backend, s = make(bj_epoch(0))
    s.start()
    ok("启动落在休眠区间直接休眠并发送 sleep true", s.state == State.SLEEPING and
       backend.calls.count("sleep:True") == 1)
    
    # 10. keepalive 前无数据调用。
    backend.calls[:] = []
    clock.advance(599)
    s.run(max_events=0)
    ok("休眠中未到 keepalive 静默且零数据请求",
       not any("balance" in call or "stock" in call for call in backend.calls))
    
    # 11. keepalive。
    clock.advance(1)
    s.run(max_events=1)
    ok("休眠中到 keepalive 重发 sleep true", backend.calls.count("sleep:True") == 1)
    
    # 12. 08:00 唤醒顺序（休市时段，窗口起点 09:20 之前）。
    clock, backend, s = make(bj_epoch(8) - 60, demo=False, upstream_key="k")
    s.start(State.SLEEPING)
    clock.advance(60)
    s.run(max_events=1)
    ok("08:00 唤醒先关 sleep 再推额度",
       backend.calls[0] == "sleep:False" and
       backend.calls[-1] == "post/api/v1/balance")
    
    # 13. 唤醒时刻正落在开市时段：只补额度缓存，不推 /api/v1/balance 抢屏。
    #     唤醒闹钟得对齐窗口起点：sleep_to=09:20 + 北京 09:19 起跑休眠，+60s 到
    #     09:20:00 恰是半开窗口的起点（默认 08:00 唤醒点在窗口外，测不到这条）。
    clock, backend, s = make(bj_epoch(9, 19), demo=True, sleep_to="09:20")
    s.start(State.SLEEPING)
    clock.advance(60)
    s.run(max_events=1)
    ok("开市中唤醒只推额度条不推额度页",
       backend.calls[0] == "sleep:False" and s.state == State.AWAKE_OPEN and
       "post/api/v1/balance" not in backend.calls and
       "fetch_balance" in backend.calls)

    # 14. 过期行情日期转休市。必须在窗口内起跑，否则会被行情节拍的时间窗口自愈
    #     拦掉，测不到 stale_date 分支。
    clock, backend, s = make(bj_epoch(10), no_sleep=True)
    backend.stock_date = "2000-01-01"
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("行情日期过期转休市节拍", s.state == State.AWAKE_CLOSED and
       not row_bodies(backend))
    
    # 15. 跨午夜区间（北京 23:00 起）。
    clock, backend, s = make(bj_epoch(23), sleep_from="23:00", sleep_to="07:00")
    s.start()
    is_sleep = s.state == State.SLEEPING
    clock.advance(8 * 3600)
    s.run(max_events=1)
    ok("自定义跨午夜休眠区间", is_sleep and "sleep:False" in backend.calls)
    
    # 16. 非法转移 fail loud。
    clock, backend, s = make(no_sleep=True)
    s.start(State.AWAKE_OPEN)
    try:
        s.transition(Event.UNKNOWN)
        raised = False
    except IllegalTransition:
        raised = True
    ok("非法状态转移抛错", raised)
    
    # 17. 处理跨过多个周期只执行一次，积压周期被跳过。
    clock, backend, s = make(no_sleep=True, open_interval=1)
    backend.slow_stock = True
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("漏 tick 单飞跳过积压节拍", len(row_bodies(backend)) == 1 and
       any("跳过" in line for line in s.log_lines))
    
    # 18. 例行节拍行默认全部静默（--verbose 才打），启动摘要照打。
    clock, backend, s = make(no_sleep=True, open_interval=15)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=2)
    ok("例行节拍行默认静默、启动摘要照打",
       not any("股票已推送" in line for line in s.log_lines) and
       not any("额度进度已更新" in line for line in s.log_lines) and
       any("启动：状态=" in line for line in s.log_lines) and
       len(row_bodies(backend)) == 2)
    
    clock, backend, s = make(no_sleep=True, open_interval=15, verbose=True)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=2)
    ok("--verbose 恢复例行节拍行",
       any("股票已推送" in line for line in s.log_lines) and
       any("额度进度已更新" in line for line in s.log_lines) and
       len(row_bodies(backend)) == 2)
    
    # 20. 默认休眠 20:00：19:00 还醒着，20:00 闹钟触发入睡。
    clock, backend, s = make(bj_epoch(19), open_interval=3600, balance_every=3600)
    s.start()                    # 北京 19:00：不在休眠区间
    clock.advance(3600)          # 到 20:00
    s.run(max_events=1)
    ok("默认 20:00 闹钟触发入睡", s.state == State.SLEEPING and
       backend.calls.count("sleep:True") == 1)
    
    # 21. 休眠时间可由配置文件覆盖：SLEEP_FROM / SLEEP_TO 环境变量。
    saved = {key: os.environ.get(key) for key in ("SLEEP_FROM", "SLEEP_TO")}
    os.environ["SLEEP_FROM"] = "21:30"
    os.environ["SLEEP_TO"] = "07:15"
    try:
        parsed = build_arg_parser().parse_args([])
        ok("休眠时间读 SLEEP_FROM/SLEEP_TO 配置",
           parsed.sleep_from == "21:30" and parsed.sleep_to == "07:15")
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
    
    # 22. 显式 flag 优先于配置文件。
    saved = {key: os.environ.get(key) for key in ("SLEEP_FROM", "SLEEP_TO")}
    os.environ["SLEEP_FROM"] = "21:30"
    os.environ["SLEEP_TO"] = "07:15"
    try:
        parsed = build_arg_parser().parse_args(
            ["--sleep-from", "23:00", "--sleep-to", "06:00"])
        ok("命令行 flag 优先于配置文件",
           parsed.sleep_from == "23:00" and parsed.sleep_to == "06:00")
    finally:
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
    
    # 23. 简称表解析：按位置对齐，数量不符 / 非 ASCII 必须挡住，空项=沿用代码。
    symbols = DEFAULT_SYMBOLS.split(",")
    full = parse_symbol_names(symbols, "MT,PAZ,ZZ500,ZZ2000,CYB")
    partial = parse_symbol_names(symbols, "MT,,,,KCB")
    mismatch = non_ascii = 0
    try:
        parse_symbol_names(symbols, "A,B")
    except ValueError:
        mismatch = 1
    try:
        parse_symbol_names(symbols, "沪深300,,,,")
    except ValueError:
        non_ascii = 1
    ok("简称表按位置解析且校验严格",
       full == {"sh600519": "MT", "sz000001": "PAZ", "sh000001": "ZZ500",
                "sz399001": "ZZ2000", "sh000300": "CYB"} and
       partial == {"sh600519": "MT", "sh000300": "KCB"} and
       parse_symbol_names(symbols, "") == {} and
       mismatch == 1 and non_ascii == 1)
    
    # 24. 配了简称就用简称上屏（且启动摘要是能确认配置生效的）。
    clock, backend, s = make(no_sleep=True, open_interval=15,
                             aliases={"sh600519": "HS300"})
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("推送行名用简称代替代码",
       row_bodies(backend) and
       row_bodies(backend)[-1]["rows"] == [{"name": "HS300", "change": 1.0}] and
       # 摘要按 SYMBOLS 顺序列出实际会显示的名字：只有 sh600519 配了简称
       any("简称 HS300,sz000001,sh000001,sz399001,sh000300" in line
           for line in s.log_lines))
    
    # 25. 没配简称时一字不改，仍推原代码。
    clock, backend, s = make(no_sleep=True, open_interval=15)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("未配简称时沿用代码",
       row_bodies(backend)[-1]["rows"] == [{"name": "sh600519", "change": 1.0}] and
       any("简称 未配置" in line for line in s.log_lines))
    
    # 26. 简称只影响行名，涨跌幅数值与条数不受影响。
    clock, backend, s = make(no_sleep=True, open_interval=15,
                             aliases={"sh600519": "X"})
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    rows = row_bodies(backend)[-1]["rows"]
    ok("简称不改变涨跌幅数值",
       len(rows) == 1 and rows[0]["change"] == 1.0)
    
    # 27. 开市窗口是半开区间：含开盘不含收盘，周末恒休市。
    #     走模块常量 MARKET_WINDOWS（不传 windows），所以不依赖命令行/环境变量。
    ok("开市窗口半开区间边界",
       is_market_open(bj_stamp(9, 20, 0)) and
       not is_market_open(bj_stamp(9, 19, 59)) and
       is_market_open(bj_stamp(15, 29, 59)) and
       not is_market_open(bj_stamp(15, 30, 0)) and
       not is_market_open(bj_stamp(15, 30, 1)) and
       not is_market_open(bj_stamp(10, 0, 0, day=3)))   # 1970-01-03 周六

    # 28. 收盘闹钟丢了，行情节拍自己兜底转休市（≤15s 自愈）。
    clock, backend, s = make(bj_epoch(15, 29), no_sleep=True, open_interval=15)
    s.start(State.AWAKE_OPEN)
    s.unregister("AWAKE_OPEN:market_check")   # 模拟检查丢失
    s.run(max_events=1)                       # 15:29:00 正常推一帧行情
    healthy = s.state == State.AWAKE_OPEN and len(row_bodies(backend)) == 1
    clock.advance(3600)                       # 跨过 15:30 到 16:29
    s.run(max_events=1)                       # 下一拍行情
    ok("收盘闹钟丢失时行情节拍自愈转休市",
       healthy and s.state == State.AWAKE_CLOSED and
       len(row_bodies(backend)) == 1 and
       [p for p, _ in backend.posts].count("/api/v1/balance") == 1 and
       any("行情节拍自愈" in line and "转休市节拍" in line
           for line in s.log_lines))

    # 29. 开市窗口可配：判定与闹钟都用配置值，不是硬编码 09:20/15:30。
    windows = parse_market_windows("09:30", "13:30")
    clock, backend, s = make(bj_epoch(13), no_sleep=True,
                             market_open="09:30", market_close="13:30")
    s.start(State.AWAKE_OPEN)
    close_alarm = s.registry.get("AWAKE_OPEN:market_check")
    s.transition(Event.MARKET_CLOSED)
    open_alarm = s.registry.get("AWAKE_CLOSED:market_check")
    close_at = beijing_time_at(close_alarm.next_at) if close_alarm else None
    open_at = beijing_time_at(open_alarm.next_at) if open_alarm else None
    ok("自定义开市窗口驱动判定与闹钟",
       is_market_open(bj_stamp(13, 29), windows=windows) and
       not is_market_open(bj_stamp(13, 30), windows=windows) and
       is_market_open(bj_stamp(9, 30), windows=windows) and
       not is_market_open(bj_stamp(9, 29, 59), windows=windows) and
       close_at is not None and (close_at.tm_hour, close_at.tm_min) == (13, 30) and
       open_at is not None and (open_at.tm_hour, open_at.tm_min) == (9, 30) and
       s.market_window_text() == "09:30~13:30")

    # 30. --demo 不再无条件开市：窗口外强制开市也立刻转休市，窗口内照常推行情。
    clock, backend, s = make(bj_epoch(16), no_sleep=True, demo=True)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    outside_ok = s.state == State.AWAKE_CLOSED and not row_bodies(backend)
    clock, backend, s = make(bj_epoch(10), no_sleep=True, demo=True)
    s.start(State.AWAKE_OPEN)
    s.run(max_events=1)
    ok("--demo 仍遵守开市窗口", outside_ok and s.state == State.AWAKE_OPEN and
       len(row_bodies(backend)) == 1)

    # 31. 简版行解析：涨跌幅已是百分比（-0.00 归一到 0.0），中文名与点位照取。
    client = SinaQuotes()
    quotes, _ = client._parse(
        'var hq_str_s_sh000300="沪深300,4340.5791,-0.1759,-0.00,0,0";',
        ["sh000300"])
    ok("简版 6 字段解析涨跌幅",
       len(quotes) == 1 and quotes[0].code == "sh000300" and
       quotes[0].name == "沪深300" and
       abs(quotes[0].point - 4340.5791) < 1e-6 and quotes[0].percent == 0.0)

    # 32. 回归守卫：开盘前深市指数点位为 0，简版下**不能**被当成无效行丢掉。
    quotes, _ = client._parse(
        'var hq_str_s_sz399006="创业板指,0.00,0.000,0.00,0,0";', ["sz399006"])
    ok("开盘前深市指数点位为 0 仍出行",
       len(quotes) == 1 and quotes[0].point == 0.0 and quotes[0].percent == 0.0)

    # 33. 无效代码返回空串：跳过、不产生行，也不抛。
    quotes, date = client._parse('var hq_str_s_sh999999="";', ["sh999999"])
    ok("无效代码空串被跳过", quotes == [] and date is None)

    # 完整版响应行（沪市 34 字段）按位置拼出来，免得手数逗号数错 fields[30]。
    def full_quote(name, previous_close, price, date_text="2026-09-29"):
        fields = [name, str(price), str(previous_close), str(price)]
        fields += ["4350", "4330", "4339", "4350", "570123", "5312"]   # 4..9
        fields += ["0"] * 20                                            # 10..29
        fields += [date_text, "15:00:00", "00", ""]                     # 30..33
        assert len(fields) == 34, len(fields)
        return fields

    def full_line(key, *args, **kwargs):
        return 'var hq_str_%s="%s";' % (key, ",".join(full_quote(*args, **kwargs)))

    def simple_line(key, data):
        return 'var hq_str_s_%s="%s";' % (key, data)

    # 34. 混用响应：3 简版 + 1 完整版同一段文本 → 3 行、按请求序、日期取自完整版。
    order = ["sz399006", "sh000300", "sz399303"]
    quotes, date = client._parse("\n".join([
        simple_line("sz399006", "创业板指,0.00,0.000,0.00,0,0"),
        simple_line("sh000300", "沪深300,4340.5791,-0.1759,-0.00,0,0"),
        simple_line("sz399303", "国证2000,0.00,0.000,0.00,0,0"),
        full_line("sh000300", "沪深300", 4341.42, 4340.5791),
    ]), order)
    ok("混用响应按请求序出行并取完整版日期",
       [q.code for q in quotes] == order and date == "2026-09-29")

    # 35. 同一代码简版与完整版共存 → 只出一行，且以简版的百分比为准。
    quotes, date = client._parse("\n".join([
        simple_line("sh000300", "沪深300,4340.5791,-0.1759,1.23,0,0"),
        full_line("sh000300", "沪深300", 4341.42, 4340.5791),
    ]), ["sh000300"])
    ok("简版与完整版同代码只出一行",
       len(quotes) == 1 and quotes[0].percent == 1.23 and date == "2026-09-29")

    # 36. 完整版仍按 (现价−昨收)/昨收 算；开盘前现价为 0 的完整版行必须丢弃
    #     （否则会算出 −100% 的假跌）。
    quotes, _ = client._parse("\n".join([
        full_line("sz399006", "创业板指", 2050.0, 0.0),   # 开盘前：现价 0
        full_line("sh000300", "沪深300", 4341.42, 4338.5),  # -0.0672 → -0.07
    ]), ["sz399006", "sh000300"])
    ok("完整版按昨收算涨跌幅且现价 0 丢弃",
       [q.code for q in quotes] == ["sh000300"] and
       quotes[0].percent == -0.07)

    # 37. URL 拼接：一次请求里每个代码都以 s_ 简版 + 裸完整版两种形式出现。
    client = SinaQuotes()
    url = client.build_url(["sh000300", "sz399006"], True)
    plain = client.build_url(["sh000300", "sz399006"], False)
    ok("混用 URL 每个代码都有简版与完整版",
       "list=s_sh000300,s_sz399006,sh000300,sz399006" in url and
       "s_" not in plain.split("list=")[1])

    # 38. 简版全废 → 自动回退纯完整版仍能出行（离线：只桩掉传输层）。
    class _Fallback(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            if "s_" in url:
                return 200, simple_line("sh000300", "")   # 简版这轮全废
            return 200, full_line("sh000300", "沪深300", 4341.42, 4338.5)

    fallback = _Fallback()
    rows, date = fallback.fetch(["sh000300"])
    ok("简版无有效行情回退完整版",
       rows == [{"name": "sh000300", "change": -0.07}] and
       date == "2026-09-29" and len(fallback.seen_urls) == 2 and
       "s_sh000300" in fallback.seen_urls[0] and
       "s_" not in fallback.seen_urls[1])

    # 39~41 日期投票：所有完整版行里取最新的一天。日期一律由 bj_stamp 造出，
    # 不写死「今天」—— 1970-01-01 是周四，day=5 是周一（工作日，窗口内）。
    today_text = beijing_date(bj_stamp(10, day=5))
    stale_text = beijing_date(bj_stamp(10, day=1))     # 停牌/陈旧标的报的那天
    stale_pair = beijing_date(bj_stamp(10, day=2))     # 全员共同上报的那天

    # 39. 陈旧日期被更新的日期盖掉：sh000300 报 4 天前（停牌），sh000905 报今天。
    _, date = client._parse("\n".join([
        full_line("sh000300", "沪深300", 4341.42, 4340.58, stale_text),
        full_line("sh000905", "中证500", 6120.0, 6115.0, today_text),
    ]), ["sh000300", "sh000905"])
    ok("陈旧行情日期被更新的日期盖掉", date == today_text and date != stale_text)

    # 40. 全员陈旧 = 节假日：都报同一天 → 就是那一天，且 is_market_open 判休市。
    _, date = client._parse("\n".join([
        full_line("sh000300", "沪深300", 4341.42, 4340.58, stale_pair),
        full_line("sh000905", "中证500", 6120.0, 6115.0, stale_pair),
    ]), ["sh000300", "sh000905"])
    ok("全员陈旧日期即上一交易日且判休市",
       date == stale_pair and
       is_market_open(bj_stamp(10, day=5)) and
       not is_market_open(bj_stamp(10, day=5), data_date=date) and
       is_market_open(bj_stamp(10, day=5), data_date=today_text))

    # 41. 混用 URL 的形状：5 个代码各自同时出现 s_<code> 与裸 <code>，无多余重复。
    five = ["sh000300", "sh000905", "sz399303", "sz399006", "sh000688"]
    request = client.build_url(five, True).split("list=")[1].split(",")
    ok("混用 URL 每个代码两种形式且无重复",
       len(request) == 2 * len(five) and
       set(request) == set(["s_" + code for code in five] + list(five)) and
       all(request.count(code) == 1 and request.count("s_" + code) == 1
           for code in five))

    # 42~46 海外指数（int_ 前缀）：4 字段、无简版、无日期。
    def int_line(key, data):
        return 'var hq_str_%s="%s";' % (key, data)

    # 42. int_ 4 字段：涨跌幅就是 fields[3]，3 位小数归一到 2 位（恒生 -0.600）。
    quotes, date = client._parse("\n".join([
        int_line("int_nikkei", "日经指数,44946.64,-408.35,-0.90"),
        int_line("int_hangseng", "恒生指数,24736.95,-148.42,-0.600"),
    ]), ["int_nikkei", "int_hangseng"])
    ok("int_ 四字段涨跌幅直给且 3 位小数归一",
       [q.code for q in quotes] == ["int_nikkei", "int_hangseng"] and
       quotes[0].percent == -0.9 and quotes[0].point == 44946.64 and
       quotes[0].change_amount == -408.35 and quotes[1].percent == -0.6 and
       date is None)

    # 43. int_ 不贡献日期：只有 int_ 行时 data_date 是 None（节假日兜底只对 A 股有效）。
    _, date = client._parse(
        int_line("int_nikkei", "日经指数,44946.64,-408.35,-0.90"), ["int_nikkei"])
    ok("int_ 行不贡献行情日期", date is None)

    # 44. 纯 int_ 列表：URL 里没有 s_int_，且**只请求一轮**（不回退）。
    class _IntOnly(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, "\n".join([
                int_line("int_nikkei", "日经指数,44946.64,-408.35,-0.90"),
                int_line("int_dji", "道琼斯,41200.00,-150.00,-0.36"),
            ])

    int_only = _IntOnly()
    rows, date = int_only.fetch(["int_nikkei", "int_dji"])
    ok("纯 int_ 列表不发简版且只请求一轮",
       len(int_only.seen_urls) == 1 and "s_int_" not in int_only.seen_urls[0] and
       int_only.seen_urls[0].split("list=")[1] == "int_nikkei,int_dji" and
       rows == [{"name": "int_nikkei", "change": -0.9},
                {"name": "int_dji", "change": -0.36}] and date is None)

    # 45. 混合列表（2 A 股 + 1 int_）：单请求混用、行序等于请求序、A 股取简版值。
    class _Mixed(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, "\n".join([
                simple_line("sh000300", "沪深300,4340.5791,-0.1759,1.23,0,0"),
                int_line("int_nikkei", "日经指数,44946.64,-408.35,-0.90"),
                simple_line("sz399006", "创业板指,0.00,0.000,0.00,0,0"),
                full_line("sh000300", "沪深300", 4341.42, 4340.5791),
            ])

    mixed = _Mixed()
    rows, date = mixed.fetch(["sh000300", "int_nikkei", "sz399006"])
    ok("A 股与 int_ 混用单请求且行序不乱",
       len(mixed.seen_urls) == 1 and
       mixed.seen_urls[0].split("list=")[1] ==
       "s_sh000300,s_sz399006,sh000300,int_nikkei,sz399006" and
       rows == [{"name": "sh000300", "change": 1.23},
                {"name": "int_nikkei", "change": -0.9},
                {"name": "sz399006", "change": 0.0}] and date == "2026-09-29")

    # 46. 代码长度：>9 字节必须有简称（交叉校验在 validate_args 层，不联网）。
    long_args = args_for(symbols="int_nikkei", symbol_names="NIKKEI",
                         device="x", device_token="y")
    _base, long_symbols, long_aliases, _w, long_error = validate_args(long_args)
    ok("10 字节代码配了简称即通过", long_error == "" and
       long_symbols == ["int_nikkei"] and long_aliases == {"int_nikkei": "NIKKEI"})
    _base, _s, _a, _w, missing_error = validate_args(
        args_for(symbols="int_nikkei", symbol_names="", device="x",
                 device_token="y"))
    ok("10 字节代码无简称报错并说明因果",
       "int_nikkei" in missing_error and "9 字节上限" in missing_error and
       "1..16 字节" in missing_error and "简称" in missing_error)
    # 17 字节仍然超出语法上限（长度校验本身没被放宽）。
    ok("超过 16 字节的代码仍被语法校验拦下",
       "1..16 字节" in str(_raised(lambda: parse_symbols("int_abcdefghijklmnopq"))))

    # 49~56 外盘期货 hf_（15/14 字段）+ 大小写非对称规则。全部离线，用真实字段布局。
    def hf_line(key, fields):
        return 'var hq_str_%s="%s";' % (key, ",".join(fields))

    def hf_quote(price, previous_close, name="纳斯达克指数期货",
                 date_text="2026-09-29", tail="0"):
        """hf_ 15 字段样本（tail 传 None 造出 hf_XAU 的 14 字段形态）。

        实测布局：[0]现价 [1]''（hf_XAU 有值）[2]买价 [3]卖价 [4]最高 [5]最低
        [6]时间 [7]昨收 [8]开盘 [9]持仓量 [10]? [11]? [12]日期 [13]中文名 [14]'0'
        """
        fields = [str(price), "", "30500.000", "30520.000", "30700.000",
                  "30400.000", "10:26:53", str(previous_close), "30566.250",
                  "18234", "1", "2", date_text, name]
        if tail is not None:
            fields.append(tail)
        return fields

    # 49. hf_NQ 15 字段：涨跌幅自算 (f[0]−f[7])/f[7] = -0.19，名称取 f[13]。
    quotes, date = client._parse(
        hf_line("hf_NQ", hf_quote(30507.430, 30566.250)),
        ["hf_NQ"])
    ok("hf_ 十五字段自算涨跌幅",
       len(quotes) == 1 and quotes[0].code == "hf_NQ" and
       quotes[0].point == 30507.43 and quotes[0].percent == -0.19 and
       quotes[0].name == "纳斯达克指数期货" and
       round(quotes[0].change_amount, 3) == -58.82 and date is None)

    # 50. hf_XAU 是 14 字段（尾部少一个）：按字段数硬判形态会漏掉它。
    quotes, date = client._parse(
        hf_line("hf_XAU", hf_quote(3512.40, 3498.10, name="纽约黄金", tail=None)),
        ["hf_XAU"])
    ok("hf_ 十四字段（hf_XAU）同样能解析",
       len(quotes) == 1 and quotes[0].code == "hf_XAU" and
       abs(quotes[0].percent - 0.41) < 0.005 and date is None)

    # 51. f[12] 的日期**不参与** A 股日期投票：外盘在 A 股节假日照常交易，混进
    #     投票会让节假日被误判成开市。
    _, only_hf = client._parse(
        hf_line("hf_NQ", hf_quote(30507.430, 30566.250, date_text=stale_text)),
        ["hf_NQ"])
    _, mixed_date = client._parse("\n".join([
        hf_line("hf_NQ", hf_quote(30507.430, 30566.250, date_text=stale_text)),
        full_line("sh000300", "沪深300", 4341.42, 4340.5791, today_text),
    ]), ["sh000300", "hf_NQ"])
    ok("hf_ 日期不参与 A 股日期投票",
       only_hf is None and mixed_date == today_text)

    # 52. 大小写非对称：A 股大写被修正成小写（且仍带 s_ 简版），外盘保大小写。
    normalized = [SinaQuotes.normalize(s)
                  for s in ("SH000300", " hf_NQ ", "S_Sh000300", "int_nikkei")]
    mixed_url = SinaQuotes().build_url(normalized, True).split("list=")[1].split(",")
    ok("大小写归一化：A 股转小写、外盘保大小写",
       normalized == ["sh000300", "hf_NQ", "sh000300", "int_nikkei"] and
       "s_sh000300" in mixed_url and "sh000300" in mixed_url and
       "hf_NQ" in mixed_url and
       # 外盘绝不能被套上 s_ 简版（s_hf_NQ 实测恒为空串）
       not any(code.lower().startswith("s_hf") for code in mixed_url) and
       # 归并用小写副本，URL 用保大小写的代码
       "s_SH000300" not in mixed_url)

    # 53. 纯 hf_NQ 列表：单请求、URL 里无 s_、出行、data_date is None。
    class _HfOnly(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, "\n".join([
                hf_line("hf_NQ", hf_quote(30507.430, 30566.250)),
                hf_line("hf_ES", hf_quote(5688.25, 5701.75, name="标普500期货")),
            ])

    hf_only = _HfOnly()
    rows, date = hf_only.fetch(["hf_NQ", "hf_ES"])
    ok("纯 hf_ 列表单请求出行且无 s_ 简版",
       len(hf_only.seen_urls) == 1 and
       hf_only.seen_urls[0].split("list=")[1] == "hf_NQ,hf_ES" and
       rows == [{"name": "hf_NQ", "change": -0.19},
                {"name": "hf_ES", "change": -0.24}] and date is None)

    # 54. rt_ 延时行情显式不支持：给中文原因、不产出行、**不能**被当 hk* 算出涨跌幅。
    #     真实 rt_hkHSI 是 25 字段（位置 0..18 同 hk*，尾部多 6 个空字段），能读出
    #     f[8]=−0.549，所以只能靠前缀名单拦，放行就是静默拿延时价当实时价上屏。
    rt_raw = ('var hq_str_rt_hkHSI="HSI,恒生指数,24648.640,24642.510,24648.640,'
              '24479.610,24507.299,-135.211,-0.549,0.00000,0.00000,53762078,'
              '3320140923,0.000,0.000,28056.100,22518.000,2026/09/29,10:23,'
              ',,,,,,";')
    quotes, date = client._parse(rt_raw, ["rt_hkHSI"])
    _, rt_reason = client._parse_quote("rt_hkHSI", rt_raw[16:-2])
    ok("rt_ 延时行情暂不支持且不误算",
       quotes == [] and date is None and
       "rt_ 延时行情形态暂不支持" in rt_reason and
       # 关键语义：既不产出行，也绝不会「算出一个数」出来
       not any(isinstance(item, float) for item in quotes))

    class _RtOnly(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, rt_raw

    rt_only = _RtOnly()
    rt_error, rt_log = _fetch_failure(rt_only, ["rt_hkHSI"])
    ok("rt_ 请求整拍失败并说明原因",
       len(rt_only.seen_urls) == 1 and "未解析到有效行情" in rt_error and
       "rt_hkHSI" in rt_log and "rt_ 延时行情形态暂不支持" in rt_log)

    # 55. 用户把外盘代码写全小写（hf_nq）：新浪返回空串 → 整拍失败，且日志里能看到
    #     是哪个代码。这正是「外盘必须保大小写」这条规则的代价。
    class _LowerHf(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, 'var hq_str_hf_nq="";'

    lower_hf = _LowerHf()
    lower_error, lower_log = _fetch_failure(lower_hf, ["hf_nq"])
    ok("hf_ 全小写取不到数据并报出该代码",
       "未解析到有效行情" in lower_error and "hf_nq" in lower_log and
       "list=hf_nq" in lower_hf.seen_urls[0])

    # 56. hf_ 的有效性守卫：昨收为 0 / 字段不足都要跳过，且原因说得清。
    zero_close = client._parse(hf_line("hf_NQ", hf_quote(30507.430, 0.0)),
                               ["hf_NQ"])[0]
    short_fields = client._parse(
        'var hq_str_hf_NQ="30507.430,,";', ["hf_NQ"])[0]
    ok("hf_ 昨收无效与字段不足都跳过",
       zero_close == [] and short_fields == [] and
       SinaQuotes._parse_quote("hf_NQ", ",".join(hf_quote(1.0, 0.0)))[1] ==
       "hf_ 形态现价或昨收无效" and
       SinaQuotes._parse_quote("hf_NQ", "1,2,3")[1] ==
       "hf_ 形态字段不足（需要 现价,昨收 两项）")

    # 57. A 股大写代码在真实请求串里被修正成小写（parse_symbols → normalize 一致）。
    ok("parse_symbols 与 normalize 大小写口径一致",
       parse_symbols("SH000300, hf_NQ ,int_nikkei") ==
       ["sh000300", "hf_NQ", "int_nikkei"] and
       validate_args(args_for(symbols="SH000300", symbol_names="CSI300",
                              device="x", device_token="y"))[1] == ["sh000300"])

    # 58~64 港股 hk*（第五种形态，19 字段）。全部离线，用实测的字段布局。
    def hk_line(key, fields):
        return 'var hq_str_%s="%s";' % (key, ",".join(fields))

    def hk_quote(percent, open_="24648.640", previous_close="24642.510",
                 high="24648.640", low="24479.610", price="24507.299",
                 change="-135.211", date_text="2026/09/29", time_text="10:23",
                 name="恒生指数", en="HSI"):
        """hk* 19 字段样本。实测布局见 _parse_quote 的 docstring。

        刻意把 price/previous_close 做成可改参数：59 号断言要靠「改了现价与昨收、
        f[8] 不变」锁死「涨跌幅取 f[8] 而不是自算」这条。
        """
        return [en, name, open_, previous_close, high, low, price, change,
                percent, "0.00000", "0.00000", "53762078", "3320140923",
                "0.000", "0.000", "28056.100", "22518.000", date_text, time_text]

    # 58. hk* 19 字段：涨跌幅就是 f[8] 直给（-0.549 归一到 -0.55），名称取 f[1]。
    quotes, date = client._parse(
        hk_line("hkHSI", hk_quote("-0.549")), ["hkHSI"])
    ok("hk_ 十九字段涨跌幅取 f[8] 直给并归一",
       len(quotes) == 1 and quotes[0].code == "hkHSI" and
       quotes[0].percent == -0.55 and quotes[0].name == "恒生指数" and
       quotes[0].point == 24507.299 and quotes[0].change_amount == -135.211 and
       date is None)

    # 59. **绝不自算**：把 f[6] 现价与 f[3] 昨收改成会算出 +99% 的组合，f[8] 不动。
    #     若代码哪天改成 (f[6]−f[3])/f[3]，这里立刻从 -0.55 变成 99.0。
    quotes = client._parse(
        hk_line("hkHSI", hk_quote("-0.549", previous_close="200.000",
                                 price="398.000", change="198.000")),
        ["hkHSI"])[0]
    ok("hk_ 涨跌幅只认 f[8] 不自算（改现价昨收结果不变）",
       len(quotes) == 1 and quotes[0].percent == -0.55)

    # 60. hk* 的 f[17] 不参与日期投票：只有 hk* 行时 data_date 是 None。
    _, date = client._parse(
        hk_line("hkHSI", hk_quote("-0.549", date_text="2026/09/29")), ["hkHSI"])
    ok("hk_ 行不贡献行情日期（斜杠格式不投票）", date is None)

    # 61. 混合投票隔离：A 股完整版（有日期）+ int_ + hf_ + hk*（后三者都带各自格式的
    #     日期字段）→ data_date **只**等于 A 股那行的日期。
    _, date = client._parse("\n".join([
        full_line("sh000300", "沪深300", 4341.42, 4340.5791, today_text),
        int_line("int_nikkei", "日经指数,44946.64,-408.35,-0.90"),
        hf_line("hf_NQ", hf_quote(30507.430, 30566.250, date_text=stale_text)),
        hk_line("hkHSI", hk_quote("-0.549", date_text="2026/09/29")),
    ]), ["sh000300", "int_nikkei", "hf_NQ", "hkHSI"])
    ok("日期投票只取 A 股行（int_/hf_/hk_ 都被隔离）",
       date == today_text and date != stale_text)

    # 62. 纯 hk* 列表：单请求、URL 里无 s_（实测 s_hkHSI 恒为空串）、出行。
    class _HkRows(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, "\n".join([
                hk_line("hkHSI", hk_quote("-0.549")),
                hk_line("hk00700", hk_quote("1.250", en="TENCENT", name="腾讯控股",
                                            price="401.500",
                                            previous_close="400.000",
                                            change="12345678")),
            ])

    hk_rows = _HkRows()
    rows, date = hk_rows.fetch(["hkHSI", "hk00700"])
    ok("纯 hk_ 列表单请求出行且无 s_ 简版",
       len(hk_rows.seen_urls) == 1 and
       hk_rows.seen_urls[0].split("list=")[1] == "hkHSI,hk00700" and
       "s_" not in hk_rows.seen_urls[0] and
       rows == [{"name": "hkHSI", "change": -0.55},
                {"name": "hk00700", "change": 1.25}] and date is None)

    # 63. 盘前/停牌价格 0 仍出行：**不拿现价 f[6] 当有效性门槛**（同「开盘前深市指数
    #     点位 0 是合法值」），只认 f[0] 非空 + f[8] 可解析。
    pre_market = client._parse(
        hk_line("hk00700", hk_quote("0.000", en="TENCENT", name="腾讯控股",
                                    price="0.000", previous_close="400.000",
                                    change="0.000")),
        ["hk00700"])[0]
    ok("hk_ 盘前现价 0 仍出行",
       len(pre_market) == 1 and pre_market[0].percent == 0.0)

    # 64. 大小写：hkHSI 归一后**保大小写**（实测 hsi / hk_hsi 都是空串），且不加
    #     s_ 简版前缀 —— 这正是 normalize_symbol 那条非对称规则的另一半。
    ok("hk_ 保大小写且不加 s_ 简版",
       normalize_symbol("hkHSI") == "hkHSI" and
       parse_symbols("hkHSI,hk00700") == ["hkHSI", "hk00700"] and
       not SinaQuotes.supports_simple("hkHSI") and
       "list=hkHSI" in SinaQuotes().build_url(["hkHSI"], True) and
       "s_hkHSI" not in SinaQuotes().build_url(["hkHSI"], True))

    # 67~75 新形态：b_ 海外指数 / gb_ 美股 / 外汇汇率。全部离线，用实测字段布局。
    def b_line(key, fields):
        return 'var hq_str_%s="%s";' % (key, ",".join(fields))

    def b_quote(name="德国DAX指数", point="25374.4200", change="-34.22",
                percent="-0.13", previous_close="25408.6400",
                date_text="2026-09-29", time_text="11:38:25", tail=True):
        """b_ 13 字段样本（tail=False 造出 b_SPX 的 6 字段形态）。

        实测布局见 _parse_quote 的 docstring。刻意把 point/previous_close 做成可改
        参数：69 号断言要靠「改了现价与昨收、f[3] 不变」锁死「涨跌幅取 f[3] 而不自
        算」这条（b_ 的 f[9] 是算术自校验过的昨收：25374.42+34.22=25408.64）。
        """
        fields = [name, point, change, percent, "9/26/2025", "2025-09-26",
                  date_text, time_text, "25443.9800", previous_close,
                  "25575.6600", "25360.3200", "45936176"]
        return fields[:6] if not tail else fields

    def gb_quote(name="哔哩哔哩", point="15.1400", percent="1.64",
                 change="0.2450", previous_close="14.8950",
                 stamp="2026-09-29 08:02:08", tail=6):
        """gb_ 36 字段样本（tail=0 造出 gb_dji 的 30 字段形态）。

        实测布局见 _parse_quote 的 docstring。point/previous_close 可改：71 号断言
        靠「改了现价与昨收、f[2] 不变」锁死「涨跌幅取 f[2] 而不自算」
        （f[26]=14.8950 是算术自校验过的昨收：15.14−0.245=14.895）。
        """
        fields = [name, point, percent, stamp, change, "15.2950", "15.3600",
                  "15.1250", "36.4000", "14.3900", "5479267", "2774314",
                  "6313380000", "0.48", "31.540000", "0.00", "0.31", "0.00",
                  "0.00", "417000000", "0", "15.1800", "0.26", "0.04",
                  "Sep 28 07:54PM EDT", "Sep 28 04:00PM EDT", previous_close,
                  "1170558", "1", "2026", "82849244.0000", "15.2000",
                  "15.1100", "17789585.7046", "15.1400", "14.8950"]
        return fields[:30 + tail]

    # 67. b_ 13 字段：涨跌幅就是 f[3] 直给，f[9] 解析成 prev_close，日期不投票。
    quotes, date = client._parse(
        b_line("b_DAX", b_quote()), ["b_DAX"])
    ok("b_ 十三字段涨跌幅取 f[3] 直给且补昨收",
       len(quotes) == 1 and quotes[0].code == "b_DAX" and
       quotes[0].percent == -0.13 and quotes[0].point == 25374.42 and
       quotes[0].change_amount == -34.22 and
       quotes[0].prev_close == "25408.6400" and
       quotes[0].quote_time == "11:38:25" and date is None)

    # 68. b_ 的 f[8]/f[10]/f[11]（疑似今开/最高/最低）**只是推测 → 必须留空**，
    #     绝不能把推测值当实测填进 dry-run。
    ok("b_ 存疑字段诚实地留空（不填推测值）",
       quotes[0].open == "" and quotes[0].high == "" and
       quotes[0].low == "" and quotes[0].volume == "")

    # 69. b_ **绝不自算**：把 f[1] 现价与 f[9] 昨收改成会算出 +99% 的组合，f[3] 不动。
    #     若代码哪天改成 (f[1]−f[9])/f[9]，这里立刻从 -0.13 变成 99.0。
    b_selfcalc = client._parse(
        b_line("b_DAX", b_quote(point="30000.0000", previous_close="200.0000")),
        ["b_DAX"])[0]
    ok("b_ 涨跌幅只认 f[3] 不自算（改现价昨收结果不变）",
       len(b_selfcalc) == 1 and b_selfcalc[0].percent == -0.13)

    # 70. b_SPX 只有 6 字段（缺 f[6]~f[12]）：按字段数判形态会漏掉它，仍要出行。
    quotes, date = client._parse(
        b_line("b_SPX", b_quote(name="标准普尔500指数", point="7683.69",
                                change="-59.72", percent="-0.77", tail=False)),
        ["b_SPX"])
    ok("b_ 六字段（b_SPX）同样能出行（不按字段数判形态）",
       len(quotes) == 1 and quotes[0].code == "b_SPX" and
       quotes[0].percent == -0.77 and date is None)

    # 71. gb_ 36 字段：涨跌幅就是 f[2] 直给，f[26] 解析成 prev_close，日期不投票。
    quotes, date = client._parse(
        b_line("gb_bili", gb_quote()), ["gb_bili"])
    ok("gb_ 三十六字段涨跌幅取 f[2] 直给并补昨收今开最高最低",
       len(quotes) == 1 and quotes[0].code == "gb_bili" and
       quotes[0].percent == 1.64 and quotes[0].point == 15.14 and
       quotes[0].prev_close == "14.8950" and quotes[0].open == "15.2950" and
       quotes[0].high == "15.3600" and quotes[0].low == "15.1250" and
       quotes[0].quote_time == "2026-09-29 08:02:08" and date is None)

    # 72. gb_ **绝不自算**：改 f[1] 现价与 f[26] 昨收、f[2] 不动。
    gb_selfcalc = client._parse(
        b_line("gb_bili", gb_quote(point="398.0000", previous_close="200.0000")),
        ["gb_bili"])[0]
    ok("gb_ 涨跌幅只认 f[2] 不自算（改现价昨收结果不变）",
       len(gb_selfcalc) == 1 and gb_selfcalc[0].percent == 1.64)

    # 73. gb_dji 只有 30 字段：同样按字段数判形态会漏，仍要出行。
    quotes, date = client._parse(
        b_line("gb_dji", gb_quote(name="道琼斯", point="51481.5117",
                                  percent="-0.67", change="-347.1100",
                                  previous_close="51828.6211", tail=0)),
        ["gb_dji"])
    ok("gb_ 三十字段（gb_dji）同样能出行（不按字段数判形态）",
       len(quotes) == 1 and quotes[0].code == "gb_dji" and
       quotes[0].percent == -0.67 and date is None)

    # 74. 外汇/汇率**只识别、绝不推算**：3a 裸代码 11 字段（USDCNY）不出行。
    #     关键：若漏掉这条判据，USDCNY 会掉进 A 股完整版分支，被当成
    #     「昨收 f[2] / 现价 f[3]」算出 +0.12% 这种**看着挺像样的垃圾**。
    fx_bare = ('var hq_str_USDCNY="11:38:01,6.7054,6.7055,6.7136,45,6.7087,'
               '6.7132,6.7087,6.7094,美元人民币,2026-09-29";')
    quotes, date = client._parse(fx_bare, ["USDCNY"])
    _, bare_reason = client._parse_quote("USDCNY", fx_bare[16:-2])
    ok("外汇裸代码被识别并跳过且不误算成涨跌幅",
       quotes == [] and date is None and
       is_fx_symbol("USDCNY") and is_fx_symbol("DINIW") and
       is_fx_symbol("CNYUSD") and
       "外汇" in bare_reason and "未验证" in bare_reason and
       # 绝不能「算出一个数」出来 —— 这正是本轮要防的错
       not any(isinstance(item, float) for item in quotes))

    # 75. 3b ``fx_`` 前缀 18 字段同样跳过，f[17] 的日期也不投票。
    fx_prefixed = ('var hq_str_fx_susdjpy="11:41:58,157.310000,157.340000,'
                   '157.350000,4100,157.400000,157.580000,157.170000,'
                   '157.310000,美元兑日元即期汇率,-0.030000,-0.040000,'
                   '0.002605,,163.980000,152.100000,,2026-09-29";')
    quotes, date = client._parse(fx_prefixed, ["fx_susdjpy"])
    _, fx_reason_text = client._parse_quote("fx_susdjpy", fx_prefixed[16:-2])
    ok("外汇 fx_ 前缀形态被识别并跳过且日期不投票",
       quotes == [] and date is None and is_fx_symbol("fx_susdjpy") and
       "fx_" in fx_reason_text and "未验证" in fx_reason_text)

    # 76. 外汇判据**不误伤 A 股**：A 股代码必含数字，全大写字母 + 5~6 长度的判据
    #     对它们一律为假（这条要是破了，sh000300 就会被当外汇丢掉）。
    ok("外汇代码判据不误伤 A 股代码",
       not any(is_fx_symbol(code) for code in
               ("sh000300", "sz399006", "bj430047", "sh600519", "hkHSI",
                "hf_NQ", "int_nikkei", "b_DAX", "gb_ixic")) and
       not is_fx_symbol("sh000300") and
       # 太短/太长/含数字/小写 都不是外汇
       not is_fx_symbol("USDC") and not is_fx_symbol("USDCNYX") and
       not is_fx_symbol("usdcny") and
       # 另一支的 fx_ 前缀无论后面是什么都算外汇形态
       is_fx_symbol("fx_") and is_fx_symbol("fx_anything"))

    # 77. 混配一次请求：A 股 + b_ + gb_ + 外汇（3a + 3b）→ A 股照常出行、外汇被
    #     跳过、**行情日期仍只来自 A 股行**。
    _, mixed_new_date = client._parse("\n".join([
        full_line("sh000300", "沪深300", 4341.42, 4340.5791, today_text),
        b_line("b_DAX", b_quote(date_text=stale_text)),
        b_line("gb_ixic", gb_quote(name="纳斯达克", point="26820.3809",
                                   percent="-0.92", change="-248.3356",
                                   previous_close="27068.7165",
                                   stamp="2026-09-29 05:30:00", tail=6)),
        fx_bare,
        fx_prefixed,
    ]), ["sh000300", "b_DAX", "gb_ixic", "USDCNY", "fx_susdjpy"])
    ok("A 股与 b_/gb_/外汇混配：外汇跳过且日期只来自 A 股",
       mixed_new_date == today_text and mixed_new_date != stale_text)

    class _NewMixed(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, "\n".join([
                simple_line("sh000300", "沪深300,4340.5791,-0.1759,1.23,0,0"),
                b_line("b_DAX", b_quote()),
                b_line("gb_ixic", gb_quote(name="纳斯达克", point="26820.3809",
                                           percent="-0.92", change="-248.3356",
                                           previous_close="27068.7165")),
                fx_bare,
                fx_prefixed,
                full_line("sh000300", "沪深300", 4341.42, 4340.5791, today_text),
            ])

    new_mixed = _NewMixed()
    rows, new_date = new_mixed.fetch(
        ["sh000300", "b_DAX", "gb_ixic", "USDCNY", "fx_susdjpy"])
    ok("混配单请求出行 3 行且外汇两行被跳过",
       len(new_mixed.seen_urls) == 1 and
       new_mixed.seen_urls[0].split("list=")[1] ==
       "s_sh000300,sh000300,b_DAX,gb_ixic,USDCNY,fx_susdjpy" and
       # b_/gb_ 都没有 s_ 简版（实测 s_b_DAX / s_gb_bili 是空串），外汇同理
       "s_b_DAX" not in new_mixed.seen_urls[0] and
       "s_gb_ixic" not in new_mixed.seen_urls[0] and
       rows == [{"name": "sh000300", "change": 1.23},
                {"name": "b_DAX", "change": -0.13},
                {"name": "gb_ixic", "change": -0.92}] and
       new_date == today_text)

    # 78. 纯外汇列表：data_date is None（两种子形态的日期都不投票）。
    _, only_fx_date = client._parse("\n".join([fx_bare, fx_prefixed]),
                                    ["USDCNY", "fx_susdjpy"])
    ok("纯外汇列表行情日期为 None（两种子形态都不投票）",
       only_fx_date is None)

    # 79. 纯 b_ 列表：单请求、URL 里无 s_b_（实测 s_b_DAX 空串）、出行、无日期。
    class _BOnly(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, "\n".join([
                b_line("b_DAX", b_quote()),
                b_line("b_NKY", b_quote(name="日经225指数", point="65219.7200",
                                        change="-657.90", percent="-1.00",
                                        previous_close="65877.6200")),
            ])

    b_only = _BOnly()
    rows, b_only_date = b_only.fetch(["b_DAX", "b_NKY"])
    ok("纯 b_ 列表单请求出行且无 s_ 简版",
       len(b_only.seen_urls) == 1 and
       b_only.seen_urls[0].split("list=")[1] == "b_DAX,b_NKY" and
       "s_b_" not in b_only.seen_urls[0] and
       rows == [{"name": "b_DAX", "change": -0.13},
                {"name": "b_NKY", "change": -1.0}] and b_only_date is None)

    # 80. 纯 gb_ 列表：同理（实测 s_gb_bili 空串）。
    class _GbOnly(SinaQuotes):
        def __init__(self):
            super().__init__(retries=0)
            self.seen_urls = []

        def _transport(self, url):
            self.seen_urls.append(url)
            return 200, b_line("gb_ixic", gb_quote(name="纳斯达克",
                                                   point="26820.3809",
                                                   percent="-0.92",
                                                   change="-248.3356",
                                                   previous_close="27068.7165"))

    gb_only = _GbOnly()
    rows, gb_only_date = gb_only.fetch(["gb_ixic"])
    ok("纯 gb_ 列表单请求出行且无 s_ 简版",
       len(gb_only.seen_urls) == 1 and
       gb_only.seen_urls[0].split("list=")[1] == "gb_ixic" and
       "s_gb_" not in gb_only.seen_urls[0] and
       rows == [{"name": "gb_ixic", "change": -0.92}] and gb_only_date is None)

    # 81. fetch() 对外返回的形状**一个字节都不能变** —— 「只解析、暂时不用」的
    #     硬约束：新增字段只在 SinaQuote / dry-run 里，绝不流进设备 payload。
    ok("fetch() 返回形状不变（补充字段不外泄）",
       list(rows[0].keys()) == ["name", "change"] and
       set(rows[0].keys()) == {"name", "change"} and
       # SinaQuote 上的补充字段都是字符串且带默认值（""）
       client._parse(b_line("b_DAX", b_quote()), ["b_DAX"])[0][0].prev_close
       == "25408.6400" and
       all(isinstance(value, str) for value in
           (SinaQuote.__dataclass_fields__["open"].default,
            SinaQuote.__dataclass_fields__["prev_close"].default,
            SinaQuote.__dataclass_fields__["quote_time"].default)) and
       # 别名替换只认 name/change，补充字段不会影响它
       apply_aliases(rows, {"gb_ixic": "IXIC"}) ==
       [{"name": "IXIC", "change": -0.92}])

    # 66. A 股/北交所「要转小写」与「支持 s_ 简版」是**两件事**，别合成一个常量。
    #     实测 s_bj430047 恒为空串（北交所没有简版形态），但 BJ430047 仍必须被
    #     归一成 bj430047 —— 新浪大小写敏感，不小写就静默取不到数据。
    bj_url = SinaQuotes().build_url(["bj430047"], True)
    ok("bj 不加 s_ 简版但仍按 A 股规则转小写",
       "s_bj430047" not in bj_url and "list=bj430047" in bj_url and
       not SinaQuotes.supports_simple("bj430047") and
       normalize_symbol("BJ430047") == "bj430047" and
       normalize_symbol("Sh000300") == "sh000300" and
       # sh/sz 仍然有简版，别把这次修正过头
       SinaQuotes.supports_simple("sh000300") and
       "s_sh000300" in SinaQuotes().build_url(["sh000300"], True))

    print("SELF-TEST PASS: %d/%d" % (len(results), len(results)))
    

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
    device_base, symbols, aliases, windows, error = validate_args(args)
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
        return stock_dry_run(args, symbols, aliases, windows)
    scheduler = Scheduler(args, device_base, symbols, clock=RealClock(),
                          aliases=aliases, windows=windows)
    try:
        scheduler.start()
        scheduler.run(max_events=None if args.loop else 1)
    except KeyboardInterrupt:
        print("中断退出")
    return scheduler.rc


if __name__ == "__main__":
    sys.exit(main())
