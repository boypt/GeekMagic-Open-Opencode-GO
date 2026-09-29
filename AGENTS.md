# AGENTS.md — GeekMagic Open Opencode GO

ESP8266 (esp12e) + ST7789 240x240 桌面小屏固件，基于 [Times-Z/GeekMagic-Open-Firmware](https://github.com/Times-Z/GeekMagic-Open-Firmware)（GPL-3.0）移植。
主功能：显示上位机推送的额度文本（时钟 + 三行推送文本），并继承框架的 WiFi 配网 / NTP / Web 配置 / OTA / 救援模式。设备端零出站网络请求、无任何 TLS 组件，额度内容由上位机脚本 `tools/push_auto.py` 读上游后经 `POST /api/v1/balance` 推送，股票行情经 `POST /api/v1/stock` 推送，设备只排版渲染（零语义）。

## 构建 / 烧录 / 调试

```bash
pio run                                   # 编译验证（~8s [SUCCESS] 即过）
pio run -t upload --upload-port /dev/ttyUSB0     # 烧固件（必须显式 --upload-port，自动探测常错选 /dev/ttyS0）
pio run -t buildfs && pio run -t uploadfs --upload-port /dev/ttyUSB0   # 改了 data/（网页/config.json）必须刷文件系统
```

- pio 路径按本机安装（曾用 `~/.platformio/penv/bin/pio`）；波特率 115200（monitor_speed）。
- **`pio device monitor` 在非交互终端下无法运行**。抓串口日志用 pyserial：

```python
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)   # 复位沿
time.sleep(0.3); s.reset_input_buffer()
# 读 40-95s ...   （启动流程约 15s 出主页面）
```

- 设备通常在 STA 模式可从开发机直连（日志里 `WiFiManager: IP : x.x.x.x`），Bearer token 见 `data/config.json` 的 `api_token`。curl 调试很方便：

```bash
curl -H "Authorization: Bearer <token>" http://<ip>/api/v1/display/rotation
```

## 硬件与显示

- ST7789 240x240，SPI Mode3 40MHz，无 CS（引脚固化在 `include/config/ConfigManager.h`：MOSI=13 SCK=14 DC=0 RST=2，背光 GPIO5 低有效）。
- **WS2812 氛围灯**（本机加装，上游无）：数据脚 **GPIO12**，默认 1 颗（`include/led/AmbientLight.h` 的 `WS2812_LED_COUNT`），效果 tick 在 main loop 与场景无关；控制见 `/api/v1/light` 与首页控制块。呼吸 = 余弦包络 + gamma2.2 的 **Q12 定点查表**（`s_breathEnvelopeQ12`，129 项对称、258B flash、0B RAM，不用 `pow()`）、10s 周期、相位由 `millis()` 派生（主循环繁忙不漂移）、每通道最终 8bit 值兜底 `>= BREATH_MIN_CHANNEL`(10)、**相同颜色不重复发帧**（WS2812 会保持最后锁存值，冗余帧只增加误码风险，见已知坑 12）。
- 图形库是 **Arduino_GFX**（不是 TFT_eSPI）；面板初始化走 `src/display/DisplayManager.cpp::lcdRunVendorInit()`（厂商序列，含 gamma/电源/VCOM），另有 `lcdRunSd2Init()`（旧 sd2 精简序列）可切换。
- **本面板色序是 BGR**：旧 sd2 用 TFT_eSPI 的 ST7789_2 驱动（240x240 自动定义 CGRAM_OFFSET → MADCTL 带 BGR 位 0x08），所以 `lcd_bgr` 默认 **true**。改这个之前先确认观感（红蓝互换是最明显症状）。推入的 16bpp 位图（相册/实时帧）在 `writePixels` 前做 **R/B 字段交换**补偿（`Scenes.cpp::drawImage`、`Api.cpp` 推帧行），UI 主题色则由两个文件里的 `rgb565()` 助手统一做同一交换（`UsageManager.cpp`、`Scenes.cpp`，源码里一律写真 RGB）。三者必须同一口径，否则屏上红蓝颠倒（logo 位图自带历史补偿值、不经助手；WS2812 不走 RGB565 不受影响）。
- 亮度：GPIO5 反相 PWM（`analogWriteRange(1023)`，`analogWrite(pin, 1023 - duty)`），`lcd_brightness` 0-100。
- 屏幕方向：`lcd_rotation` 0-3 正常四向，4-7 是镜像变体；本机 0。另有 `lcd_mirror_x/y` 翻转 MADCTL 的 MX/MY 位。

## 架构地图

| 模块 | 职责 |
|---|---|
| `src/main.cpp` | 启动流程：DisplayManager → WiFiManager → NTP → Webserver → `UsageManager::begin()` + `StockData::begin()`；loop 委托各 manager，**休眠时跳过 `AmbientLight::update()` 与 `SceneManager::update()`**（web/NTP/watchdog 仍必须跑，否则脚本无法唤醒） |
| `src/opencodego/UsageManager.cpp` | 七段时钟、logo、三行额度、状态行渲染；数据源 = `POST /api/v1/balance` 的三段推送缓冲（`setRowLabel/setRowProgress/setRowReset` 写入，定长静态数组，零 String 抖动），推送到达走**局部差量刷新**：私有 `s_pushRefreshPending` 信号 → `redrawChangedQuota()` 比对屏上快照（134B 静态）只重画变化的行带/状态行，内容未变则**零绘制**（与显示设置变更的 `DisplayManager::requestFullRedraw()` 整屏路径分离，两者不互相降级）；额度行**三段式**：行上方左=标签（缺省回落 5H/WK./MO.）/右=百分比（中等字号，无值红 `--`）、中间=进度条（轨道左右 4px 等边距、填充绿≥50/黄≥20/红<20、无值空槽）、行下方右=重置日期（最小字号，缺省 `--`）。设备不解析语义 |
| `include/opencodego/StockData.h` + `src/opencodego/StockData.cpp` | 股票场景推送缓冲：`Row{name[10], float change}` 定长 5 行静态数组（零堆、零 String），`setRows()` 整体替换并记 `millis()`，`clear()` 清空，`getRow()` 越界返回 false；另带**可选额度缓存** `int8_t s_quota[3]`（-1=无值）由 `setQuota()/hasQuota()/quota()` 访问，只存百分比不要标签，`clear()` 一并清掉；`POST /api/v1/stock` 写入、stock 场景只读消费，涨跌方向由数值正负表达（设备零语义） |
| `tools/push_auto.py` | 上位机总控脚本（合并了原 `push_balance.py` + `push_stock.py`，仅标准库）：**额度** 读 OpenCode Go 用量（`--upstream-url` 默认 `https://opencode.ai/zen/go/v1/usage` + Bearer + `x-opencode-session`，按 `labels/progress/resets` 三段字段推 `POST /api/v1/balance`）＋**股票** 走 `SinaQuotes` 类读新浪 `hq.sinajs.cn/list=<代码,...>`（**必须带 `Referer: https://finance.sina.com.cn`** + `Accept: */*` + UA，按 gb18030 解码；**简版为主 + 完整版取日期 + 单请求混用**：`s_<code>` 简版 6 字段（`名称,点位,涨跌额,涨跌幅%,成交量,成交额`）的 `fields[3]` **直接就是涨跌幅百分比**（不用自己除），完整版的 `fields[30]` 是行情日期（节假日/停市兜底判定靠它）；两者可混写在同一 `list=` 里（实测 `s_sh000300,sh000300` 两行都返回），所以每拍只发一次请求：全部标的走简版 + 每个标的都捎带完整版，**日期取各标的上报日期里最新的一天**（单个标的停牌会报陈旧日期，只信一个会把交易日误判成休市；节假日时全员同日所以 max 不掩盖休市），**简版这轮全废时自动回退纯完整版**；`--symbols` 写 `s_` 前缀会被剥掉；**布局按 key 带不带 `s_` 分流，不按字段数**：完整版的指数和个股是同一套布局（**沪市 34 字段 / 深市 33 字段，尾部多一个空字段**），`fields[2]`=昨收、`fields[3]`=最新价/最新点位，`(fields[3]−fields[2])/fields[2]` **对个股与指数同一公式成立**，`fields[30]`=日期、`fields[31]`=时间（sh/sz 位置一致）——「字段少的是指数」只在 `s_` 简版下成立，拿它判完整版会把指数误判成个股，只因公式恰好通用才没出错；**第三种形态 = 海外指数 `int_` 前缀**（`int_nikkei`/`int_dji`/`int_hangseng` 等，实测 4 字段 `名称,现价,涨跌额,涨跌幅%`、`s_int_nikkei` 恒为空串即**没有简版形态**）：涨跌幅同为 `fields[3]` 直给（恒生 3 位小数，交给 `round_change` 归一到 2 位），**不得套 A 股完整版的 `(f[3]−f[2])/f[2]`**（那里 `fields[2]` 是昨收、这里是涨跌额，套上去会算出 `-0.91% → +0.02%` 这种看着像样的垃圾），**且它没有日期字段 → 不参与日期投票**（纯 `int_` 列表 `data_date` 为 `None`）；**第四种形态 = 外盘期货 `hf_` 前缀**（`hf_NQ`/`hf_ES`/`hf_CL`/`hf_XAU` 等，实测 **15 字段**、`hf_XAU` **14 字段**，所以不能按字段数判形态：`[0]`现价 `[7]`昨收 `[12]`日期 `[13]`中文名 `[14]`'0'，**没有现成涨跌幅**必须自算 `(f[0]−f[7])/f[7]`（要求 `f[7]>0`，实测 `hf_NQ` 30507.430/昨收 30566.250 → `−0.19%`），名称取 `f[13]`（中文只进日志），**`f[12]` 日期不参与日期投票**（外盘在 A 股节假日照常交易，混进投票会把节假日误判成开市）故纯 `hf_` 列表 `data_date` 为 `None`；**第五种形态 = 港股 `hk*`**（`hkHSI`/`hk00700`/`hk09988`/`hkHSCEI` 等，实测 **19 字段**：`[0]`英文名(ASCII) `[1]`中文名 `[2]`今开 `[3]`昨收 `[4]`最高 `[5]`最低 `[6]`现价 `[7]`涨跌额 `[8]`涨跌幅% `[9]`买价 `[10]`卖价 `[11]`成交量 `[12]`成交额 `[13]`/`[14]`? `[15]`52周高 `[16]`52周低 `[17]`日期 `[18]`时间）：涨跌幅是 **`f[8]` 直给**（3 位小数，交给 `round_change` 归一到 2 位），**不自算**——算术自校验 `hkHSI` `(f[6]−f[3])/f[3] = −0.5487%` 与 `f[8]=−0.549` 吻合，**与 `hf_` 相反**（`hf_` 没有现成涨跌幅才必须自算）；有效性只认 `f[0]` 非空 + `f[8]` 能解析成有限数，**不拿现价 `f[6] <= 0` 当门槛**（同「盘前深市指数点位 0 合法」原则，盘前/停牌时价格可能是 0 而涨跌幅字段有效），名称取 `f[1]` 中文名（只进日志/`--stock-dry-run`，不上屏；`f[0]` 是 ASCII 英文名，以后想直接上屏可用它），取不到名字**也不丢行**；读不到 `f[8]` → 跳过原因「`hk_ 形态字段不足（需要 英文名,涨跌幅% 两项）`」；**不按字段数判形态**（能读到 `f[8]` 就按 `hk` 解析）。**`rt_` 延时行情**（实测 `rt_hkHSI` **25 字段**，位置 0..18 与 `hk*` 一致、尾部多 6 个空字段，但语义是延时）**本轮显式不支持**（`UNSUPPORTED_PREFIXES`），命中给「`rt_ 延时行情形态暂不支持`」跳过原因——25 字段照样能读出 `f[8]`，只能靠**前缀名单**在形态分流最前面拦下，放行就会静默拿延时价当实时价上屏；**日期投票只认 A 股完整版的 `fields[30]`**，`int_`/`hf_`/`hk*` **三种非 A 股形态一律把 `date` 填 `""` 不参与投票**，两条理由：① 交易日历不同（外盘/港股各有各的假期，与 A 股混判会把节假日或非交易日误判成开市）；② 港股 `f[17]` 是**斜杠格式** `2026/09/29`，与 A 股的 `2026-09-29` 混进同一个 `max()` 会按字符串序比错 —— 故纯 `int_`/`hf_`/`hk*` 列表 `data_date` 全为 `None`；**代码大小写非对称、别「顺手统一」**：实测 `SH000300`/`hf_nq` 都是空串，只有 `sh000300`/`hf_NQ` 有数据 → **A 股必须小写（写大写会被修正）、外盘期货必须保大小写**，只有匹配 `^(sh|sz|bj)\d{6}$` 的代码才转小写，其余形态原样送出，形态判定/字典归并用小写副本但**请求 URL 用保大小写的代码**）；哪些前缀加 `s_` 简版由 `SIMPLE_PREFIXES` 决定（实测只有 `sh`/`sz`/`bj` 有），**全员不支持简版时直接走纯完整版形态，不做「简版 → 回退」两轮 dance**（否则每拍多一倍请求和重试噪音）；`SYMBOLS` 代码语法上限 16 字节（覆盖 `int_hangseng` 12 / `int_nikkei` 10），**超过设备端 name 的 9 字节必须配简称**否则启动失败（没配简称才原样上屏）；**坑：开盘前深市指数（`sz399006`/`sz399303`/`sz399001`）简版点位就是 0，简版只认 `fields[3]`、点位 0 是合法值不得当无效行丢掉（旧代码的 `point > 0` 守卫会把它们整行丢掉），完整版才保留昨收/现价 > 0 的守卫否则算出 −100%**；行序=请求序（被丢弃的行不能让后面的行上移）、最多 5 个标的；`name` 推规范化后的裸代码，中文名只进日志/`--stock-dry-run`，不上屏）推 `POST /api/v1/stock`。**行名简称**：可选 `SYMBOL_NAMES`（环境变量）/ `--symbol-names`（flag）按**位置**一一对应 `SYMBOLS` 的简称，项数必须相同否则报错（错位会把简称贴到错的标的上），留空项=该只沿用代码；受设备端同名限制（1..9 字节可打印 ASCII）；替换只做在排版层（推送前 `apply_aliases()`），取数层拿到的永远是原代码；启动摘要里的「简称」字段可确认配置生效。**调度层 = 生成器式可挂起 handler + 闹钟注册表 + 显式状态机，无 asyncio/线程/事件循环**：handler 可 `yield` `FetchStock/FetchBalance/PostDevice/PostSleep/Log/Verbose/Wait/Register/Unregister/EnterState` 十种任务，调度器推进并把结果喂回；闹钟规格 `Every/DailyAt/After`；**按状态注册** —— `AWAKE_OPEN`（股票 `--open-interval` **15s** + **额度只推条不推页** `--balance-every` 300s + 收盘闹钟（=开市窗口结束，默认 15:30）+ 20:00 入睡）、`AWAKE_CLOSED`（额度 300s + 开盘探针闹钟（默认 09:20）+ 20:00 入睡）、`SLEEPING`（**只有 keepalive 10min + 08:00 唤醒，不注册任何数据闹钟 → 夜间零出站是结构性保证**）。**没有额度窗口状态**：开市期间屏幕只停留在 stock 场景，绝不 POST `/api/v1/balance` 抢场景；行情与额度是**两个独立节拍各推各的** —— 行情 15s 发 `{"rows":[...]}`，额度 5min 发 `{"balance":{"progress":[..]}}`（**不带 rows**，设备只更新屏顶三条、不碰行情行也不接管屏幕，所以额度刷新不会打断行情；`--no-quota-bars` 可让开市完全不发额度）。**时间策略**：20:00~08:00 跨午夜休眠（`--sleep-from/--sleep-to`，也可用配置文件环境变量 **`SLEEP_FROM` / `SLEEP_TO`** 覆盖，命令行 flag 优先；部署机 d270 的配置文件就是 systemd `EnvironmentFile=/etc/sd2.conf`，与 `DEVICE/DEVICE_TOKEN/UPSTREAM_URL/UPSTREAM_KEY/SYMBOLS/SYMBOL_NAMES/SINA_URL/SLEEP_FROM/SLEEP_TO/MARKET_OPEN/MARKET_CLOSE` 同文件，改它不用动 unit 的 ExecStart），到点调 `POST /api/v1/display/sleep {"on":true}` 黑屏休眠（期间不取数不推送，每 `--sleep-keepalive` 重申，08:00 唤醒并立即推一次额度）；开市（周一至周五 **默认 09:20~15:30**（`--market-open` / `--market-close`，也可用配置文件环境变量 **`MARKET_OPEN` / `MARKET_CLOSE`** 覆盖，命令行 flag 优先；**窗口是半开区间 `start <= 秒 < end`** —— 闭区间会让落在 `15:30:00.000` 那一拍的收盘闹钟自己判成「还开市」而空转），节假日以新浪完整版 `fields[30]` 行情日期≠今天兜底）每 `--open-interval`(15s) 推行情，**行情节拍每拍自愈**（`handle_stock` 开头若已过窗口就自己转休市，任何一次收盘检查丢失最多 15s 纠正，**`--demo` 也照样遵守窗口**，只跳过 API 行情日期判定），**进入开市时先发一条纯额度推送**，让切到 stock 场景的第一帧就带着条；非开市每 `--closed-interval`(300s = 5min) 推额度。补漏：handler 跨多个周期时跳过积压节拍并记日志；非法状态转移抛错。`Clock` 抽象（`RealClock` 用 `threading.Event().wait` 可被 Ctrl-C 打断 / `VirtualClock` 供测试），`--self-test` 虚拟时钟瞬时跑完整套策略；**日志两层**：普通日志只有**启动摘要**（`启动：状态=… 设备=… 行情/额度/非开市/开市 HH:MM~HH:MM/休眠 …`，末尾提示例行日志已静默）、**状态转移**和**全部失败/异常**；例行节拍的成功行（15s 行情 / 5min 额度 / 10min 休眠 keepalive）走 `Verbose` 任务默认丢弃，`--verbose` 才逐拍输出；`--check` 输出设备 balance/stock/sleep 三份状态；`--stock-dry-run` **真实请求新浪**并打印 payload + 行情日期 + 开市判定（不需要 `--device`；注意 `--dry-run` 跟时间策略且用本地随机数据代替取数）；`--demo` 用本地随机数据不联网（**仍遵守开市时间窗口**，只跳过 API 行情日期判定）；`--no-sleep` 关休眠；退出码 0/1/2/3 |
| `include/opencodego/SegFont7.h` | TFT_eSPI Font7 原字模解码的 1bpp 行位图（0-9 : -，32x48），像素级还原旧七段观感 |
| `include/opencodego/IromAccess.h` | 经 `-include` 注入：`u8x8_pgm_read`→`pgm_read_byte`、字体独立节（配合 `NON32XFER_HANDLER`） |
| `src/display/DisplayManager.cpp` | 面板初始化（厂商/sd2 两套）、MADCTL（rotation/镜像/BGR）、背光 PWM、`requestFullRedraw()` 机制 |
| `include/display/Scene.h` + `src/display/SceneManager.cpp` | 场景接口 + 调度器：显示面唯一切换入口 `switchTo()`（退场重绘契约：旧场景 exit 禁画/释放，新场景 enter 全量绘制；失败回滚重绘上一场景） |
| `src/display/Scenes.cpp` | 内置场景：`sysinfo`（系统信息：IP / WiFi SSID+RSSI / NTP 服务器+同步状态 / 芯片 ID / 运行时长 / 剩余堆 / 显示配置 / 固件版本 + 底部 UTC+8 时钟；**开机落点**，取代原 startup IP 画面；值变化时只擦写对应单行）/ `balance`（额度+时钟）/ `album`（静态相册：param=文件名常驻单张、空=循环轮播 5s/张）/ `clock`（纯时钟页）/ `stock`（股票行情：**右下角是三重同心额度环**（擦底方框 41×41 占 x=193..233 / y=194..234，贴边留 5px 余量，圆心 (213,214)、外径 40px；外/中/内=5H/WK./MO.，轨道 `#283249`，进度弧沿用现有橙红 `#FF6B35` / 橙黄 `#F2A63C` / 橙棕 `#A9663A`；从 12 点顺时针，弧长按百分比占 360°，无文字；无额度数据时显示三圈空轨道）+ 标题 `STOCK` 居中（y=16，未下移）+ 5 个固定槽位（简称 textSize 2 左对齐、涨跌幅右对齐，**正红涨 `#FF0000` / 正绿跌 `#00FF00`** / 中性灰，0 行时空态 `NO DATA`+`PUSH VIA API`，行文本按可用宽度以 `~` 截断过长名称）+ 底部 UTC+8 `HH:MM:SS` 秒表**左对齐**（x=4，字号 3 共 144px 占 x=4..147，右侧空档让给圆环；y=204）+ 时钟与圆环的擦除带必须各守自己那块地：时钟整条擦除只到 `x=2..149`（**不得再 `fillRect(0,…,240,…)` 横扫**，否则抹掉右下角三层环），圆环 `fillRect` 只覆盖自己 41×41（不碰时钟像素），行情行带止于 y=180 与环的 y=194 也留 14px 纵向空档；数据源 = `StockData` 缓冲（`POST /api/v1/stock` 写入），时间戳或内容变化才重绘，**行带与额度环差量判定彼此独立**（只动其一则另一者零绘制），秒变化只擦秒字段）/ `live`（实时推图：API 流式直绘、不落盘、常驻最后一帧）。场景切换一律 `switchTo`；**唯三例外是推送自动接管**：`live` 推图 → live，额度推送 → balance，股票推送 → stock |
| `src/display/Scenes.cpp`（AlbumScene/LiveScene） | 相册图片 = `/album/<name>.rgb565`（240x240 RGB565(LE) 115200B，Web 端 canvas 转换上传，jpg/png 等任意源图）；`POST /album/live` 同格式流式推帧（480B 行缓冲逐行直绘、不保存），推送时若不在 live 场景则自动接管屏幕 |
| `include/display/NoticeScene.h` + `src/display/Scenes.cpp`（NoticeOverlayScene） | `notice` 临时通知覆盖层（第 7 个场景）：满屏近黑 `#06080E`、无 logo 无七段时钟，与常驻页结构上就不同。**首行 64×64 图标按 level 形状区分**（info 实心圆+`i` / warning 实心三角+`!` / critical 实心方块+`X`，纯图元手绘不引位图，色弱也能分辨）+ 字号 2 level 名 + 中性灰分隔线；**正文区 110..215px**，字号从 4 逐级降到 1 取第一个装得下的（按词换行、尊重 `\n`、空段保留、溢出末行补 `~`）；**右下角 `Ns` + 底部 216×4px 进度线**告知会自己消失，每秒只重画那 ~1.3k 像素（占全屏 2.4%），无任何闪烁动画。`image` 形态整屏即那一帧（同 live，不叠装饰）。内容由 API 校验后经 `prepareText()/prepareImage()` **预载**（`Scene::enter()` 只带 param、装不下正文）再 `switchTo("notice")`。生命周期：入场时读 `SceneManager::currentName()/currentParam()` 记返回目标（此刻 `s_current` 仍是即将退场的那个 —— `switchTo` 在 `enter()` 成功后才提交）→ `millis()` 回绕安全计时 → 耗尽后清活跃标志再 `switchTo` 回原场景（连 param 一起还原），失败兜底 `sysinfo`。`exit()` 刻意 no-op（清活跃标志会毁掉「通知中再来一条」与休眠唤醒重入）；重新入场时若计时已耗尽且无新内容则**一帧不画**、挂 `s_pendingReturn` 由下次 `update()` 兑现（`enter()` 里直接 `switchTo` 会在外层 switchTo 尚未提交 `s_current` 时被改回来）。非持久化，重启即失效 |
| `src/config/ConfigManager.cpp` | `config.json`（LittleFS）+ SecureStorage（EEPROM NVS）双层配置 |
| `src/boot/RescueMode.cpp` | boot-loop 保护（见"已知坑"） |
| `src/web/Api.cpp` + `data/web/` | REST API + Web 配置页（pico.css + Alpine.js，无构建步骤） |

## 配置系统（语义容易记反，注意）

- `config.json`（LittleFS 根）：`api_token` 有值且与 NVS **不同**时**覆盖** NVS（便于刷机生效）；随后 `save()` 会把它从 JSON 删除（敏感信息只留 NVS）。
- WiFi 凭据、API token 在 SecureStorage（EEPROM，XOR 混淆）；亮度/显示/NTP 等参数在 `config.json`。上游 host/path/key 已移出设备，归上位机脚本的参数/环境变量。
- `SecureStorage::put()` 对**与 EEPROM 已提交值相同**的键直接返回（`_dirty` 标志区分「内存已改但 commit 未成功」，提交失败后同值仍会重试，绝不误判为已持久化），因此改亮度/颜色/显示等非敏感字段不再触发 NVS 序列化 + EEPROM sector 擦写 —— 既省 flash 磨损，也避免阻塞主循环几十 ms 漏掉 WS2812 的 20ms tick。
- `data/config.json` 被上游 `.gitignore` 忽略（含 token，勿提交）。它被打进 littlefs 映像，所以**刷文件系统会覆盖设备上的 config.json**——新增持久化字段时记得同步加进去，否则刷完丢配置。
- TLS：设备端**已无任何出站 TLS**（BearSSL/CA 文件/`verify_tls_cert`/MFLN 全部移除）；上游 HTTPS 由上位机脚本按**标准系统证书库**校验（`--insecure` 可关闭校验，无自定义 CA 选项）。

## API 端点（全部 `/api/v1/...`，Bearer token 保护，rescue 模式除外）

- WiFi：`GET /wifi/scan|status`，`POST /wifi/connect`
- NTP：`GET /ntp/status|config`，`POST /ntp/sync|config`
- 显示：`GET/POST /display/rotation`（含 lcd_bgr/lcd_init_sd2/镜像）、`GET/POST /display/mirror`、`GET/POST /display/brightness`；`GET/POST /display/sleep`（`{"on":true}` 休眠：清屏置黑 + 反相背光 PWM 全灭 + 氛围灯**非持久化**挂起 `AmbientLight::suspend()`，且 `loop()` 跳过场景与灯效刷新、但保留 web/NTP/watchdog；`{"on":false}` 唤醒恢复背光与灯效并 `SceneManager::redrawCurrent()` 全量重绘。状态**不落配置**、重启即醒，由 `tools/push_auto.py` 按 00:00~08:00 时段断言，重复调用幂等）
- 相册：`GET/POST/DELETE /album`（图片 = 240x240 RGB565 原始位图 `.rgb565`，Web 端转换上传；上传时校验整幅尺寸 115200B）；`POST /album/live`（multipart 流式推一帧实时显示、**不保存**——脚本/HA 推画面用；不在 live 场景时自动接管）
- 灯光：`GET/POST /light`（WS2812 氛围灯：`on/mode(solid|breathe|rainbow)/r,g,b/brightness`，部分更新，持久化 config.json `led_*`）
- 场景：`GET /scene`（当前场景+参数+列表）、`POST /scene`（`{"scene":"album","param":"red.rgb565"}`，退场重绘契约，失败自动回滚重绘上一场景）。**无自动跳转，全手工**；推送自动接管是例外：live 推图→live、额度推送→balance、股票推送→stock
- 通知（临时覆盖层，**完整 curl 示例见 readme.md「API — Notice notifications」**）：`POST /api/v1/notice`（body `{"level":"info|warning|critical","text":"ASCII 正文","seconds":N}`；`level` 大小写不敏感、缺省 `info`；`seconds` **整数 1..30**、缺省 5（0/越界/负数/字符串/小数 → 400）；`text` 必填 1..199 字节、逐字节限可打印 ASCII `0x20..0x7E` 与 `\n`（`\r\n` 自动归一，≥0x80 的汉字/UTF-8 与 `\t` 一律 400，固件无 CJK 字模）；body<1KB；**进入前记录当前场景名+param，计满秒数自动切回**；`seconds` 缺省 5）、`POST /api/v1/notice/image?seconds=N`（multipart 推一帧，同 `/album/live` 格式与逐行直绘，秒数走 query，非法值在 done 回调回 400 且一帧不画）、`GET /api/v1/notice`（`{active,mode:text|image,level,text,total,remaining}`，remaining 向上取整）。**通知期间其他场景请求一律忽略**：`SceneManager::switchTo()` 里判 `NoticeScene::isActive() && 目标 != "notice"` 就直接 return false（不碰屏、不退场），一处覆盖 `POST /scene`（回 **409** + `notice active (Ns remaining)`；未知场景仍 400）、live 推图、额度/股票推送自动接管；推送数据照收不误只是不显示。`notice` 激活时再来一条通知 = 换内容并重置计时、**返回目标不变**。闸门必须判 `isActive()` 而非"当前场景名是不是 notice"，否则通知自己的返回会被拦成死锁（`triggerReturn()` 先清 `s_active` 再 `switchTo`） |
- 额度推送：`POST /balance`（body `{"labels":["5H","WK.","MO."],"progress":[58,null,91],"resets":["R2d4h",null,"R14d3h"],"status":"可选状态行"}`；三段字段均可选、出现时须**彼此等长**（1..3）、元素可 `null`；`progress` 元素为 0..100 剩余百分比（越界/类型错/长度不一致 → 400），缺省 `labels` 回落固件默认 `5H/WK./MO.`、缺省 `resets` 显示 `--`、缺省 `progress` 为空槽；body<1KB；成功 200 `{"ok":true}` 并立即重绘；**若当前不在 balance 场景则立即 `switchTo("balance")` 接管**）、`GET /balance`（`{labels,progress,resets,status,ts,age_s}`）。整行 `lines` 通道**已退役**——标签 / 百分比 / 重置三段分开推送，设备零语义照单渲染
- 股票推送：`POST /stock`（body `{"rows":[{"name":"600519","change":1.23},...]}` 或 `{"balance":{"progress":[58,null,91]}}` 或两者都有，**1..5 行**；`name` 须 1..9 字节**可打印 ASCII**（固件无中文字模，汉字/控制字符一律 400）、`change` 须有限数且 `|change|≤100000`（字符串/NaN/Inf/越界 → 400）；`{"rows":[]}` 清空（**连额度一起清**）；body<1280B；成功 200 `{"ok":true}` 并立即重绘；**带 rows 时若当前不在 stock 场景则立即 `switchTo("stock")` 接管**）。**`rows` 与 `balance` 都是可选键且彼此独立**（开市行情 15s 一推、额度 5min 一推，各推各的）：**只带 `balance` → 只写额度槽位，绝不碰行情行、绝不接管屏幕**（这是额度能与 15s 行情共存的关键；`setQuota()` 会更新时间戳唤醒 stock 场景重画条带），**两个键都缺 → 400**。`balance.progress` 必为 0..3 项数组，每项 `null`（无窗口）或不越界的整数 0..100（`[]` / 全 null = 清空并隐藏条）；任一非法 → 400 且**一行数据都不写**（先全量校验再落地））、`GET /stock`（`{rows:[{name,change}],balance?:{progress:[...]},ts,age_s}`，无额度数据时省略 `balance` 键；`ts` 是**行情或额度任一**的最近写入时刻）
- 系统：`POST /reboot`、`GET /logs`、`POST /ota/fw|fs|cancel`、`GET /ota/status`、`GET/POST /token/check|save`
- rescue 模式（AP `GeekMagic` @192.168.4.1，无鉴权）：`GET /rescue/status`，`POST /rescue/reset|reboot|token|ota`

## 已知坑（都踩过，别再踩）

1. **ESP8266 IROM 只支持 32-bit 对齐访问**：直接 `*(const uint8_t*)` 读 flash 常量会 `Exception (3)` LoadStoreError（excvaddr=符号地址）。必须走 `pgm_read_byte/word`，并保持 `-DNON32XFER_HANDLER` 兜底。u8g2 的裸指针解引用靠 `IromAccess.h` 重定义宏修正。
2. **rescue 误触发**：每次刷机/手动复位都算一次启动，旧阈值 3 太敏感。现已：阈值 10 + **只统计崩溃类复位**（Exception/Fatal/Watchdog），`External System`/`Power On`/`Software/System restart` 直接清零计数。若再进 rescue：连 AP 后 `curl -X POST http://192.168.4.1/api/v1/rescue/reset` 再 `/rescue/reboot`。
3. **显示设置后屏幕残留开机画面**：`DisplayManager::setRotation()/applyPanelProfile()` 不得画开机画面（会覆盖主页面且 `mainPageDrawn` 不回退）；改完必须 `requestFullRedraw()`，由 `UsageManager::update()` 重画。原生启动画面（`drawStartup`：红绿蓝闪烁 + 色块 + IP）已随 `sysinfo` 场景删除。
4. **Web 静态资源缓存 24h**（`max-age=86400`）：改了网页/JS 后浏览器要硬刷新（Ctrl+Shift+R）才生效。图像处理遵循「CDN 引库」策略（零设备空间）：`jpeg-js`（JS 解码 JPEG，浏览器内建解码对 CMYK/YCCK JPG 反色）+ `cropperjs`（画布裁剪缩放控件），均为 jsDelivr/esm.sh 动态或标签引用，离线/内网需自建镜像。
5. **刷文件系统覆盖设备 config.json**（见"配置系统"）。同理也会**清空设备上的 `/album` 相册库**（相册图只存在设备上，不在 `data/` 里）——`uploadfs` 前提醒用户重传图片，或先用 `GET /api/v1/album` + 逐个下载备份（无下载接口，重要图请留原图）。
6. 上传时报 `Invalid head of packet`：重试即可；报 PermissionError：先关掉占用串口的 monitor。
7. **堆碎片：空闲总量够 ≠ 能分配**。实测 `free 22KB / max block 13KB`，任何 >13KB 的整体 `new` 必失败；且 park 释放的大洞会被小块分配切碎、再也拼不回去。**结论（已付过学费）**：80KB RAM 塞不下 GIF 解码器（对象+LZW 字典 ≥20KB），相关方案（vendored AnimatedGIF/字典池交接）已整体删除，相册改为静态 RGB565 图（零解码、~3.8KB 行缓冲）。新功能若需 >4KB 连续块，先想清楚碎片化。
8. **Alpine `x-if` 里的元素在条件为真前不在 DOM**：`$refs.xxx` 取到 `undefined`（症状 `TypeError: Cannot set properties of undefined (setting 'src')`）。必须先置条件 + `await this.$nextTick()` 再取 ref（相册单图裁剪上传曾因此全挂）。要 ref 恒在用 `x-show`。
9. **GIF→相册重构的路径残留**：删除接口曾写死 `/gif/` 前缀，文件实际在 `/album/`，删除必 404 `file not found`。改存储目录/前缀时全仓 grep 旧前缀对齐（上传/列表/删除/场景四处）。
10. **勿在设备端重新引入出站 TLS**：BearSSL/`WiFiClientSecure`/CA 校验/MFLN 等组件已整体移除（Flash 从 ~59% 降到 ~46%），历史坑（CA 解析 OOM、低堆 abort 重启、MFLN 512 档被拒、握手 15s）都随移除消失；上游 HTTPS 归上位机脚本。
11. **LCD 上屏文案必须 ASCII**：Arduino_GFX 内建 6px 字体只覆盖 ASCII，固件里没有任何 CJK 字模（现有界面文案全是英文即因此）。给 LCD 文本写中文会取到字表越界字形（乱码，甚至读越界）——中文只出现在 Web 页（浏览器自带字体）。
12. **WS2812 低亮度偶发「突然亮一下」：根因是传输误码，不是亮度曲线**。0 位被灯珠读成 1 → 字节变大 → 锁存更亮的一帧（持续一个刷新周期）。`Adafruit_NeoPixel 1.15.5` 在 ESP8266 的 0 位高脉冲目标 0.4µs 超出 WS2812B-**V5/V6** 规格（220~380ns），而老版 WS2812B 反而宽裕；且 ESP 3.3V 直驱 5V 灯珠时高电平余量很薄（V5 规格 VIH≈0.63×VDD≈3.15V，只剩 ~150mV），WiFi 发射电流尖峰与 bit-bang 中断窗口会放大误码概率。**本机 LED 是成品、硬件不可改**（加不了串联电阻/电平转换/去耦电容），所以代码侧只做缓解：每通道下限 `>= 10`（V5 实测 1~2 不亮、3~7 慢启动，10 是合理暗端下限）+ **相同颜色不重复发帧**（30% 亮度下全周期传输次数降到约 1/4，暗段/平台段冗余率约 98%）。低频偶发闪亮无法根除，根治需换 V6 灯珠或加 330Ω 串阻 + 74AHCT125 + 100nF/470~1000µF 去耦。**别再去调呼吸曲线**——曲线已做 gamma 感知校正且用户验收平滑。
13. **会被 16/32 位视图访问的静态缓冲必须显式 `alignas(4)`**。ESP8266 对奇地址的 16 位访问直接 `Exception (9) LoadStoreError`（`excvaddr` 落在 `0x3fff4xxx` DRAM 区且为奇数，反汇编 `epc1` 能直接看到 `l16ui`），这与坑 #1 的 IROM 32 位对齐是**同一族**的坑，但发生在 DRAM。链接器只把 `uint8_t buf[480]` 放在**前面静态量结束处**、不保证 2/4 字节对齐 —— 相邻多一个单字节 `bool` 就足以把它挤到奇地址。真实案例：推帧行缓冲 `s_liveRow` 因前面多了 3 个 `bool` 被放到 `0x3fff484b`，`reinterpret_cast<uint16_t*>` 做 R/B 交换时必崩（`/album/live` 推任意大小的帧都重启设备），而**同样是 480B 的 `s_noticeRow` 只是碰巧落在 `0x3fff4660`** 所以一直没暴露。已给两者加 `alignas(4)`。**改完静态量后用符号表自查**：`xtensa-lx106-elf-nm -S firmware.elf` 找出所有「奇地址且 size≥2」的自有符号，纯 `char[]`（仅字节访问，如 `s_text`/`s_returnName`/`s_rowLabels`）可放过，任何被 `uint16_t*`/`uint32_t*` 看过一眼的都必须对齐。

## 验证工作流

改动后标准闭环：`pio run` → `upload`（+ 必要时 `uploadfs`）→ pyserial 抓 40-95s 启动日志，确认
`Clean boot (...)` 或 `Boot stable`、无 `Exception`、`UsageManager initialized`、`Free heap` 稳定无泄漏趋势。
调度策略自测：`python3 tools/push_auto.py --self-test`（虚拟时钟瞬时跑完 17 条：开市 15s 行情 tick / 开市 300s 额度只刷条不切屏 / **额度与行情分别推送互不夹带** / 额度取不到就不开推送（设备保留上次的条）/ `--no-quota-bars` / 额度上游失败 / 入市预取先于首帧行情 / 休市 300s / 休眠进入·keepalive·静默零出站·唤醒（休市与开市中两种）/ 行情日期过期 / 跨午夜区间 / 非法转移抛错 / 漏节拍跳过）。
联调：`--demo --loop --dry-run` 看将要发生的调用 → 加 `--device/--device-token` 真跑（缺 `--upstream-key` 时跳过额度、只推股票，属正常）→ `--check` 核对设备端 balance/stock/sleep 三份状态。额度页局部刷新可用 `curl -X POST .../api/v1/balance` 原样重推验证（内容相同 → 零重绘；只改一个值 → 只重画该行带）。
无测试/CI，`pio run` + 真机日志即为验证。RAM ~62% / Flash ~46%（移除出站 TLS 后 Flash 大降），加库注意余量。

## 提交规范

信息风格：`feat:` / `fix:` / `docs:` / `refactor:` + 中文简述。勿 `push`（除非明确要求）。

## 待办 / 备忘

- 远端 `origin` = `git@github.com:boypt/GeekMagic-Open-Opencode-GO.git`，已推送（外层工程以 submodule 引用本目录）。
- `x-opencode-session` 头仍是 `sd2-opencode-go-balance-<chipId>`（`OpenCodeGoClient.h`），可改项目名。
- `data/config.example.json` 模板缺失（新克隆者需在 Web 里配 host/path 等）。
- 上游同步：`upstream` 指向 Times-Z/GeekMagic-Open-Firmware，本仓库在其 develop 之上叠加了 OpenCode 业务与若干修复（BGR 默认、rescue 阈值、IROM 访问、TLS CA 可配）。
