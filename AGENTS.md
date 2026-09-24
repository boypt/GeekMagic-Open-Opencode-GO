# AGENTS.md — GeekMagic Open Opencode GO

ESP8266 (esp12e) + ST7789 240x240 桌面小屏固件，基于 [Times-Z/GeekMagic-Open-Firmware](https://github.com/Times-Z/GeekMagic-Open-Firmware)（GPL-3.0）移植。
主功能：显示上位机推送的额度文本（时钟 + 三行推送文本），并继承框架的 WiFi 配网 / NTP / Web 配置 / OTA / 救援模式。设备端零出站网络请求、无任何 TLS 组件，额度内容由上位机脚本 `tools/push_balance.py` 读上游后经 `POST /api/v1/balance` 推送，设备只排版渲染（零语义）。

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
- **WS2812 氛围灯**（本机加装，上游无）：数据脚 **GPIO12**，默认 1 颗（`include/led/AmbientLight.h` 的 `WS2812_LED_COUNT`），效果 tick 在 main loop 与场景无关；控制见 `/api/v1/light` 与首页控制块。
- 图形库是 **Arduino_GFX**（不是 TFT_eSPI）；面板初始化走 `src/display/DisplayManager.cpp::lcdRunVendorInit()`（厂商序列，含 gamma/电源/VCOM），另有 `lcdRunSd2Init()`（旧 sd2 精简序列）可切换。
- **本面板色序是 BGR**：旧 sd2 用 TFT_eSPI 的 ST7789_2 驱动（240x240 自动定义 CGRAM_OFFSET → MADCTL 带 BGR 位 0x08），所以 `lcd_bgr` 默认 **true**。改这个之前先确认观感（红蓝互换是最明显症状）。推入的 16bpp 位图（相册/实时帧）在 `writePixels` 前做 **R/B 字段交换**补偿（`Scenes.cpp::drawImage`、`Api.cpp` 推帧行），UI 主题色则由两个文件里的 `rgb565()` 助手统一做同一交换（`UsageManager.cpp`、`Scenes.cpp`，源码里一律写真 RGB）。三者必须同一口径，否则屏上红蓝颠倒（logo 位图自带历史补偿值、不经助手；WS2812 不走 RGB565 不受影响）。
- 亮度：GPIO5 反相 PWM（`analogWriteRange(1023)`，`analogWrite(pin, 1023 - duty)`），`lcd_brightness` 0-100。
- 屏幕方向：`lcd_rotation` 0-3 正常四向，4-7 是镜像变体；本机 0。另有 `lcd_mirror_x/y` 翻转 MADCTL 的 MX/MY 位。

## 架构地图

| 模块 | 职责 |
|---|---|
| `src/main.cpp` | 启动流程：DisplayManager → WiFiManager → NTP → Webserver → `UsageManager::begin()`；loop 委托各 manager |
| `src/opencodego/UsageManager.cpp` | 七段时钟、logo、三行额度、状态行渲染；数据源 = `POST /api/v1/balance` 的三段推送缓冲（`setRowLabel/setRowProgress/setRowReset` 写入，定长静态数组，零 String 抖动），推送到达即 `requestFullRedraw()`；额度行**三段式**：行上方左=标签（缺省回落 5H/WK./MO.）/右=百分比（中等字号，无值红 `--`）、中间=进度条（轨道左右 4px 等边距、填充绿≥50/黄≥20/红<20、无值空槽）、行下方右=重置日期（最小字号，缺省 `--`）。设备不解析语义 |
| `tools/push_balance.py` | 上位机脚本（在 PC 上运行，仅 Python 标准库）：HTTPS 读上游 OpenCode Go 用量（完整 URL `--upstream-url` + Bearer + `x-opencode-session`），按**三段字段**推送（左上标签 `labels` / 进度条与右上百分比 `progress`=剩余% / 右下重置相对时长 `resets`）+ 可选状态行，POST 到设备 `/api/v1/balance`；支持 `--loop/--dry-run/--check/--demo`（`--demo` 用本地随机数据测试、无需上游凭据），退出码 0/1/2/3 |
| `include/opencodego/SegFont7.h` | TFT_eSPI Font7 原字模解码的 1bpp 行位图（0-9 : -，32x48），像素级还原旧七段观感 |
| `include/opencodego/IromAccess.h` | 经 `-include` 注入：`u8x8_pgm_read`→`pgm_read_byte`、字体独立节（配合 `NON32XFER_HANDLER`） |
| `src/display/DisplayManager.cpp` | 面板初始化（厂商/sd2 两套）、MADCTL（rotation/镜像/BGR）、背光 PWM、`requestFullRedraw()` 机制 |
| `include/display/Scene.h` + `src/display/SceneManager.cpp` | 场景接口 + 调度器：显示面唯一切换入口 `switchTo()`（退场重绘契约：旧场景 exit 禁画/释放，新场景 enter 全量绘制；失败回滚重绘上一场景） |
| `src/display/Scenes.cpp` | 内置场景：`sysinfo`（系统信息：IP / WiFi SSID+RSSI / NTP 服务器+同步状态 / 芯片 ID / 运行时长 / 剩余堆 / 显示配置 / 固件版本 + 底部 UTC+8 时钟；**开机落点**，取代原 startup IP 画面；值变化时只擦写对应单行）/ `balance`（额度+时钟）/ `album`（静态相册：param=文件名常驻单张、空=循环轮播 5s/张）/ `clock`（纯时钟页）/ `live`（实时推图：API 流式直绘、不落盘、常驻最后一帧）。场景切换一律 `switchTo`；**唯二例外是推送自动接管**：`live` 推图 → live，额度推送 → balance |
| `src/display/Scenes.cpp`（AlbumScene/LiveScene） | 相册图片 = `/album/<name>.rgb565`（240x240 RGB565(LE) 115200B，Web 端 canvas 转换上传，jpg/png 等任意源图）；`POST /album/live` 同格式流式推帧（480B 行缓冲逐行直绘、不保存），推送时若不在 live 场景则自动接管屏幕 |
| `src/config/ConfigManager.cpp` | `config.json`（LittleFS）+ SecureStorage（EEPROM NVS）双层配置 |
| `src/boot/RescueMode.cpp` | boot-loop 保护（见"已知坑"） |
| `src/web/Api.cpp` + `data/web/` | REST API + Web 配置页（pico.css + Alpine.js，无构建步骤） |

## 配置系统（语义容易记反，注意）

- `config.json`（LittleFS 根）：`api_token` 有值且与 NVS **不同**时**覆盖** NVS（便于刷机生效）；随后 `save()` 会把它从 JSON 删除（敏感信息只留 NVS）。
- WiFi 凭据、API token 在 SecureStorage（EEPROM，XOR 混淆）；亮度/显示/NTP 等参数在 `config.json`。上游 host/path/key 已移出设备，归上位机脚本的参数/环境变量。
- `data/config.json` 被上游 `.gitignore` 忽略（含 token，勿提交）。它被打进 littlefs 映像，所以**刷文件系统会覆盖设备上的 config.json**——新增持久化字段时记得同步加进去，否则刷完丢配置。
- TLS：设备端**已无任何出站 TLS**（BearSSL/CA 文件/`verify_tls_cert`/MFLN 全部移除）；上游 HTTPS 由上位机脚本按**标准系统证书库**校验（`--insecure` 可关闭校验，无自定义 CA 选项）。

## API 端点（全部 `/api/v1/...`，Bearer token 保护，rescue 模式除外）

- WiFi：`GET /wifi/scan|status`，`POST /wifi/connect`
- NTP：`GET /ntp/status|config`，`POST /ntp/sync|config`
- 显示：`GET/POST /display/rotation`（含 lcd_bgr/lcd_init_sd2/镜像）、`GET/POST /display/mirror`、`GET/POST /display/brightness`
- 相册：`GET/POST/DELETE /album`（图片 = 240x240 RGB565 原始位图 `.rgb565`，Web 端转换上传；上传时校验整幅尺寸 115200B）；`POST /album/live`（multipart 流式推一帧实时显示、**不保存**——脚本/HA 推画面用；不在 live 场景时自动接管）
- 灯光：`GET/POST /light`（WS2812 氛围灯：`on/mode(solid|breathe|rainbow)/r,g,b/brightness`，部分更新，持久化 config.json `led_*`）
- 场景：`GET /scene`（当前场景+参数+列表）、`POST /scene`（`{"scene":"album","param":"red.rgb565"}`，退场重绘契约，失败自动回滚重绘上一场景）。**无自动跳转，全手工**；推送自动接管是例外：live 推图→live、额度推送→balance |
- 额度推送：`POST /balance`（body `{"labels":["5H","WK.","MO."],"progress":[58,null,91],"resets":["R2d4h",null,"R14d3h"],"status":"可选状态行"}`；三段字段均可选、出现时须**彼此等长**（1..3）、元素可 `null`；`progress` 元素为 0..100 剩余百分比（越界/类型错/长度不一致 → 400），缺省 `labels` 回落固件默认 `5H/WK./MO.`、缺省 `resets` 显示 `--`、缺省 `progress` 为空槽；body<1KB；成功 200 `{"ok":true}` 并立即重绘；**若当前不在 balance 场景则立即 `switchTo("balance")` 接管**）、`GET /balance`（`{labels,progress,resets,status,ts,age_s}`）。整行 `lines` 通道**已退役**——标签 / 百分比 / 重置三段分开推送，设备零语义照单渲染
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

## 验证工作流

改动后标准闭环：`pio run` → `upload`（+ 必要时 `uploadfs`）→ pyserial 抓 40-95s 启动日志，确认
`Clean boot (...)` 或 `Boot stable`、无 `Exception`、`UsageManager initialized`、`Free heap` 稳定无泄漏趋势。
额度链路联调：`python3 tools/push_balance.py --dry-run` 看 payload → 带真参数推送 → 屏幕三行即时刷新、`GET /api/v1/balance` 可见 lines/ts。
无测试/CI，`pio run` + 真机日志即为验证。RAM ~62% / Flash ~46%（移除出站 TLS 后 Flash 大降），加库注意余量。

## 提交规范

信息风格：`feat:` / `fix:` / `docs:` / `refactor:` + 中文简述。勿 `push`（除非明确要求）。

## 待办 / 备忘

- 远端 `origin` = `git@github.com:boypt/GeekMagic-Open-Opencode-GO.git`，已推送（外层工程以 submodule 引用本目录）。
- `x-opencode-session` 头仍是 `sd2-opencode-go-balance-<chipId>`（`OpenCodeGoClient.h`），可改项目名。
- `data/config.example.json` 模板缺失（新克隆者需在 Web 里配 host/path 等）。
- 上游同步：`upstream` 指向 Times-Z/GeekMagic-Open-Firmware，本仓库在其 develop 之上叠加了 OpenCode 业务与若干修复（BGR 默认、rescue 阈值、IROM 访问、TLS CA 可配）。
