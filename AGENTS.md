# AGENTS.md — GeekMagic Open Opencode GO

ESP8266 (esp12e) + ST7789 240x240 桌面小屏固件，基于 [Times-Z/GeekMagic-Open-Firmware](https://github.com/Times-Z/GeekMagic-Open-Firmware)（GPL-3.0）移植。
主功能：OpenCode Go 额度显示（时钟 + 5H/WEEK/MONTH 三行），并继承框架的 WiFi 配网 / NTP / Web 配置 / OTA / 救援模式。

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
# 读 40-95s ...   （启动流程约 15s 出主页面；拉取在联网后立即发生）
```

- 设备通常在 STA 模式可从开发机直连（日志里 `WiFiManager: IP : x.x.x.x`），Bearer token 见 `data/config.json` 的 `api_token`。curl 调试很方便：

```bash
curl -H "Authorization: Bearer <token>" http://<ip>/api/v1/display/rotation
```

## 硬件与显示

- ST7789 240x240，SPI Mode3 40MHz，无 CS（引脚固化在 `include/config/ConfigManager.h`：MOSI=13 SCK=14 DC=0 RST=2，背光 GPIO5 低有效）。
- 图形库是 **Arduino_GFX**（不是 TFT_eSPI）；面板初始化走 `src/display/DisplayManager.cpp::lcdRunVendorInit()`（厂商序列，含 gamma/电源/VCOM），另有 `lcdRunSd2Init()`（旧 sd2 精简序列）可切换。
- **本面板色序是 BGR**：旧 sd2 用 TFT_eSPI 的 ST7789_2 驱动（240x240 自动定义 CGRAM_OFFSET → MADCTL 带 BGR 位 0x08），所以 `lcd_bgr` 默认 **true**。改这个之前先确认观感（红蓝互换是最明显症状）。
- 亮度：GPIO5 反相 PWM（`analogWriteRange(1023)`，`analogWrite(pin, 1023 - duty)`），`lcd_brightness` 0-100。
- 屏幕方向：`lcd_rotation` 0-3 正常四向，4-7 是镜像变体；本机 0。另有 `lcd_mirror_x/y` 翻转 MADCTL 的 MX/MY 位。

## 架构地图

| 模块 | 职责 |
|---|---|
| `src/main.cpp` | 启动流程：DisplayManager → WiFiManager → NTP → Webserver → `UsageManager::begin()`；loop 委托各 manager |
| `include/opencodego/OpenCodeGoClient.h` | HTTPS 拉取用量（BearSSL + 自定义 CA）、解析三窗口、3 次退避重试、`dechunkInPlace` 单缓冲响应解析 |
| `src/opencodego/UsageManager.cpp` | UI + 轮询调度：七段时钟、logo、三行额度、状态行；5min 轮询 / 无数据 30s fast retry；局部重绘 |
| `include/opencodego/SegFont7.h` | TFT_eSPI Font7 原字模解码的 1bpp 行位图（0-9 : -，32x48），像素级还原旧七段观感 |
| `include/opencodego/IromAccess.h` | 经 `-include` 注入：`u8x8_pgm_read`→`pgm_read_byte`、字体独立节（配合 `NON32XFER_HANDLER`） |
| `src/display/DisplayManager.cpp` | 面板初始化（厂商/sd2 两套）、MADCTL（rotation/镜像/BGR）、背光 PWM、`requestFullRedraw()` 机制 |
| `src/config/ConfigManager.cpp` | `config.json`（LittleFS）+ SecureStorage（EEPROM NVS）双层配置 |
| `src/boot/RescueMode.cpp` | boot-loop 保护（见"已知坑"） |
| `src/web/Api.cpp` + `data/web/` | REST API + Web 配置页（pico.css + Alpine.js，无构建步骤） |

## 配置系统（语义容易记反，注意）

- `config.json`（LittleFS 根）：`api_token` / `opencodego_api_key` 有值且与 NVS **不同**时**覆盖** NVS（便于刷机生效）；随后 `save()` 会把它们从 JSON 删除（敏感信息只留 NVS）。
- WiFi 凭据、API token、OpenCode Key 在 SecureStorage（EEPROM，XOR 混淆）；host/path/亮度/显示参数在 `config.json`。
- `data/config.json` 被上游 `.gitignore` 忽略（含 token，勿提交）。它被打进 littlefs 映像，所以**刷文件系统会覆盖设备上的 config.json**——新增持久化字段时记得同步加进去，否则刷完丢配置。
- TLS 信任：**源码无内置证书**。`/ca.pem`（LittleFS）存在且 `verify_tls_cert=1` 时用它做链校验，否则 `setInsecure()`（默认即不校验）。当前 host（`bwe.134133.xyz`，Let's Encrypt）的信任锚是 **ISRG Root X1**。

## API 端点（全部 `/api/v1/...`，Bearer token 保护，rescue 模式除外）

- WiFi：`GET /wifi/scan|status`，`POST /wifi/connect`
- NTP：`GET /ntp/status|config`，`POST /ntp/sync|config`
- 显示：`GET/POST /display/rotation`（含 lcd_bgr/lcd_init_sd2/镜像）、`GET/POST /display/mirror`、`GET/POST /display/brightness`
- OpenCode Go：`GET/POST /opencodego/config`（body 字段名是 `opencodego_host/opencodego_path/opencodego_api_key`，不是 host/path）、`GET/POST/DELETE /opencodego/ca`（PEM 全文 `{"pem":"..."}`）
- 系统：`POST /reboot`、`GET /logs`、`POST /ota/fw|fs|cancel`、`GET /ota/status`、`GET/POST /token/check|save`、GIF 若干
- rescue 模式（AP `GeekMagic` @192.168.4.1，无鉴权）：`GET /rescue/status`，`POST /rescue/reset|reboot|token|ota`

## 已知坑（都踩过，别再踩）

1. **ESP8266 IROM 只支持 32-bit 对齐访问**：直接 `*(const uint8_t*)` 读 flash 常量会 `Exception (3)` LoadStoreError（excvaddr=符号地址）。必须走 `pgm_read_byte/word`，并保持 `-DNON32XFER_HANDLER` 兜底。u8g2 的裸指针解引用靠 `IromAccess.h` 重定义宏修正。
2. **BearSSL OOM**：拉取时 free heap 仅 ~24KB。每次拉取重新 `String + X509List` 解析 CA 会挤到 `Unhandled C++ exception: OOM`；现已一次性解析常驻。**不要**在 webserver handler 里再开 TLS 连接（叠加请求缓冲必炸——`/ca/test` 因此被移除）。
3. **rescue 误触发**：每次刷机/手动复位都算一次启动，旧阈值 3 太敏感。现已：阈值 10 + **只统计崩溃类复位**（Exception/Fatal/Watchdog），`External System`/`Power On`/`Software/System restart` 直接清零计数。若再进 rescue：连 AP 后 `curl -X POST http://192.168.4.1/api/v1/rescue/reset` 再 `/rescue/reboot`。
4. **显示设置后屏幕残留启动屏**：`DisplayManager::setRotation()/applyPanelProfile()` 不得画启动屏（会覆盖主页面且 `mainPageDrawn` 不回退）；改完必须 `requestFullRedraw()`，由 `UsageManager::update()` 重画。
5. **Web 静态资源缓存 24h**（`max-age=86400`）：改了网页/JS 后浏览器要硬刷新（Ctrl+Shift+R）才生效。
6. **刷文件系统覆盖设备 config.json**（见"配置系统"）。
7. 上传时报 `Invalid head of packet`：重试即可；报 PermissionError：先关掉占用串口的 monitor。
8. `connect()` 无超时参数（本核心），TLS 握手最坏约 15s，长链（4 证书 + RSA-4096）验证较重，首次偶发失败由重试兜底（日志 `Fetch recovered on attempt 2` 属正常）。

## 验证工作流

改动后标准闭环：`pio run` → `upload`（+ 必要时 `uploadfs`）→ pyserial 抓 40-95s 启动日志，确认
`Clean boot (...)` 或 `Boot stable`、无 `Exception`、`UsageManager initialized`、拉取 `HTTP 200` + `Quota: ...`、`Free heap` 稳定无泄漏趋势。
无测试/CI，`pio run` + 真机日志即为验证。RAM ~56% / Flash ~59%，加库注意余量。

## 提交规范

信息风格：`feat:` / `fix:` / `docs:` / `refactor:` + 中文简述。勿 `push`（除非明确要求）。

## 待办 / 备忘

- 远端 `origin` = `git@github.com:boypt/GeekMagic-Open-Opencode-GO.git`，**尚未 push**（外层工程以 submodule 引用本目录，push 后别人才能 clone 到）。
- `x-opencode-session` 头仍是 `sd2-opencode-go-balance-<chipId>`（`OpenCodeGoClient.h`），可改项目名。
- `data/config.example.json` 模板缺失（新克隆者需在 Web 里配 host/path 等）。
- 上游同步：`upstream` 指向 Times-Z/GeekMagic-Open-Firmware，本仓库在其 develop 之上叠加了 OpenCode 业务与若干修复（BGR 默认、rescue 阈值、IROM 访问、TLS CA 可配）。
