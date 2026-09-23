// SPDX-License-Identifier: GPL-3.0-or-later
#include "opencodego/UsageManager.h"

#include <Logger.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <time.h>

#include "config/ConfigManager.h"
#include "display/DisplayManager.h"
#include "opencodego/OpenCodeLogo.h"
#include "wireless/WiFiManager.h"

extern ConfigManager configManager;
extern WiFiManager* wifiManager;

// Arduino_GFX 不继承 Adafruit_GFX，而 U8g2_for_Adafruit_GFX 解码器对显示对象
// 只调用 drawFastHLine/drawFastVLine（含 drawPixel 纯虚），用一个最小 shim
// 适配到 DisplayManager::getGfx() 的 Arduino_GFX。
class U8g2GfxShim : public Adafruit_GFX {
   public:
    explicit U8g2GfxShim(Arduino_GFX* target) : Adafruit_GFX(240, 240), _target(target) {}
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        _target->drawPixel(x, y, color);
    }
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        _target->drawFastHLine(x, y, w, color);
    }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        _target->drawFastVLine(x, y, h, color);
    }

   private:
    Arduino_GFX* _target;
};

static U8g2GfxShim& u8g2Shim() {
    static U8g2GfxShim shim(DisplayManager::getGfx());
    return shim;
}

static U8G2_FOR_ADAFRUIT_GFX& u8g2() {
    static U8G2_FOR_ADAFRUIT_GFX font = [] {
        U8G2_FOR_ADAFRUIT_GFX u;
        u.begin(u8g2Shim());
        u.setFontMode(1);  // 透明字形，清底由局部重绘 fillRect 负责
        return u;
    }();
    return font;
}

// ---- 轮询节奏 ----
static constexpr uint32_t POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 默认 5 分钟
static constexpr uint32_t FAST_RETRY_MS = 30UL * 1000UL;           // 无数据时 30s 快速重试
static constexpr uint32_t FETCH_TIMEOUT_MS = 15000;
static constexpr uint32_t FETCH_RETRY_INTERVAL_MS = 10000UL;
static constexpr int FETCH_MAX_ATTEMPTS = 3;

// ---- 时区（同旧工程 src/config.h：北京时间 UTC+8）----
// GeekMagic NTPClient 用 configTime(0, 0, ...) 同步，time(nullptr) 为 UTC epoch；
// 本地时间 = UTC epoch + TZ_OFFSET_SEC，再用 gmtime 解释。
static constexpr uint32_t TZ_OFFSET_SEC = 8UL * 3600UL;
static constexpr time_t TIME_MIN_VALID = 1600000000;  // 同 NTPClient REASONABLE_EPOCH

// ---- 主题色（旧工程 Sd2Theme.h C_* 暗色主题，标准 RGB565 数值，Arduino_GFX 直接可用）----
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>((static_cast<uint16_t>(r & 0xF8) << 8) |
                                 (static_cast<uint16_t>(g & 0xFC) << 3) | (b >> 3));
}
static constexpr uint16_t C_BG = rgb565(0x00, 0x00, 0x00);
static constexpr uint16_t C_BORDER = rgb565(0x28, 0x32, 0x49);
static constexpr uint16_t C_SUB = rgb565(0x8A, 0x94, 0xB8);
static constexpr uint16_t C_LABEL = rgb565(0x9A, 0xA5, 0xC8);
static constexpr uint16_t C_ACCENT = rgb565(0x4D, 0x6B, 0xFE);
static constexpr uint16_t C_WHITE = rgb565(0xFF, 0xFF, 0xFF);
static constexpr uint16_t C_GREEN = rgb565(0x34, 0xD3, 0x99);
static constexpr uint16_t C_RED = rgb565(0xFF, 0x6B, 0x6B);
static constexpr uint16_t C_YELLOW = rgb565(0xF6, 0xC3, 0x43);

// ---- 字体（U8g2_for_Adafruit_GFX，替代旧 TFT_eSPI Font7/FreeSans/内置字体）----
// - 时钟大数字 u8g2_font_logisoso46_tn：u8g2 内最大数字字体（字高约 46px，仅含
//   空格 0-9 : + - .），最接近旧工程 Font 7 七段数码管 48px 观感；
// - 百分比     u8g2_font_helvB12_tf：Helvetica Bold 12pt，接近旧 FreeSans12pt7b；
// - 标签       u8g2_font_helvR10_tf：Helvetica Regular 10pt，接近旧 FreeSans9pt7b；
// - 小字       u8g2_font_6x10_tf：6px 字宽同旧内置 Font 1（6x8），略高 2px。
static const uint8_t* const FONT_BIG = u8g2_font_logisoso46_tn;
static const uint8_t* const FONT_PCT = u8g2_font_helvB12_tf;
static const uint8_t* const FONT_LABEL = u8g2_font_helvR10_tf;
static const uint8_t* const FONT_MINI = u8g2_font_6x10_tf;

// ---- 布局常量（与旧工程 main.cpp 完全一致）----
// 新布局（240x240）：下半部分左右分栏，左时钟（日期 + HH/MM）、右额度
//   ┌─ logo(居中) ──────────────────────┐ y=1
//   ├─ 横线 y=40 ───────────────────────┤
//   │ 日期 MM-DD WKD │ 状态行 UPDATE 22:00 │ y=44
//   │  HH  (46px)    │ 5H    行 y=58     │
//   │  ───────       │ WEEK  行 y=114    │
//   │  MM  (46px)    │ MONTH 行 y=170    │
//   └─ 错误条 y=226..240 ────────────────┘
static constexpr int QUOTA_X0 = 124;  // 内容左边界
static constexpr int QUOTA_X1 = 232;  // 右对齐基准
static constexpr int ROW_H = 56;      // 三行额度行高
static constexpr int ROW_TOP = 58;    // 第一行额度顶部 y（其上是状态行）
static constexpr int STATUS_Y = 44;   // 右栏顶部状态行顶部 y
static constexpr int STATUS_H = 14;   // 状态行高度（slim）

static constexpr int CLOCK_X = 0;
static constexpr int CLOCK_Y = 44;   // 时钟盒 44..203
static constexpr int CLOCK_W = 120;
static constexpr int CLOCK_H = 160;

static constexpr int SEP_Y = 134;
static constexpr int SEP_W = 56;
static constexpr int SEP_SWING = 10;
static constexpr int SEP_STRIP_X = 18;
static constexpr int SEP_STRIP_Y = 131;
static constexpr int SEP_STRIP_W = 84;
static constexpr int SEP_STRIP_H = 6;

static constexpr int CLOCK_CX = 60;  // 左栏水平中心
static constexpr int HH_Y = 70;      // HH 大字顶部（占 y 70..116）
static constexpr int MM_Y = 150;     // MM 大字顶部（占 y 150..196）
static constexpr int DATE_X = 4;     // 日期行紧贴左沿
static constexpr int DATE_Y = 50;

// ---- 缓存状态（供 UI 层读取）----
static OpenCodeGoUsage usageData;
static String lastErrorText;
static time_t lastSuccessTime = 0;  // 本地时刻（已 +TZ_OFFSET_SEC）；0 = 从未成功
static uint32_t last_poll_ms = 0;
static bool has_data = false;
static bool ever_started = false;

// 上次绘制的本地分钟（hour*60+min）/ 秒；-1 = 尚未绘制
static int lastClockMinute = -1;
static int lastClockSecond = -1;

// 主页面是否已绘制：只有绘制过后才允许时钟 tick / 局部重绘（boot 页除外）
static bool mainPageDrawn = false;

// ---------- 本地时间工具 ----------
static bool timeSynced() { return time(nullptr) > TIME_MIN_VALID; }

// 当前本地时间字段（UTC epoch + 时区偏移后按 UTC 解释）
static struct tm localTmNow() {
    time_t t = time(nullptr) + TZ_OFFSET_SEC;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    return tmv;
}

// t 为"本地时刻"（UTC epoch + 时区偏移）
static String formatLocalTime(time_t t, const char* fmt) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), fmt, &tmv);
    return String(buf);
}

// OpenCode 接口返回 UTC 时间（如 "2026-08-28T15:42:25.791Z"）。
// 同旧工程：mktime 按系统时区（GeekMagic 为 UTC）解释，再补回时区偏移。
static bool parseIsoTime(const String& iso, time_t& out) {
    if (iso.length() < 19) return false;

    struct tm tmv = {};
    tmv.tm_year = iso.substring(0, 4).toInt() - 1900;
    tmv.tm_mon = iso.substring(5, 7).toInt() - 1;
    tmv.tm_mday = iso.substring(8, 10).toInt();
    tmv.tm_hour = iso.substring(11, 13).toInt();
    tmv.tm_min = iso.substring(14, 16).toInt();
    tmv.tm_sec = iso.substring(17, 19).toInt();

    time_t t = mktime(&tmv) + TZ_OFFSET_SEC;
    if (t <= 0) return false;
    out = t;
    return true;
}

static String formatReset(const String& iso) {
    if (iso.length() == 0) return "";
    time_t t;
    if (!parseIsoTime(iso, t)) return "R --";
    return "R " + formatLocalTime(t, "%m-%d %H:%M");
}

// ---------- 界面工具 ----------
// 调用前须 u8g2().setFont(...)；测宽与绘制用同一字体。
// 参数 y 为字形顶部（同旧 TFT_eSPI TL_DATUM），内部按当前字体 ascent 折算基线
static void drawText(int x, int y, const String& s, uint16_t color) {
    u8g2().setForegroundColor(color);
    u8g2().drawUTF8(static_cast<int16_t>(x),
                    static_cast<int16_t>(y + u8g2().getFontAscent()), s.c_str());
}

static int textWidth(const String& s) {
    return static_cast<int>(u8g2().getUTF8Width(s.c_str()));
}

// logo 位图按 TFT_eSPI pushImage 字节序预交换，此处还原为标准 RGB565；
// 0x0000 (调色板 nibble 0) 为透明色，只绘制字标像素。
// 【字节序】调色板沿用原表预交换形态，还原逻辑仍为 c=(c<<8)|(c>>8)，行为保持。
// 位图/调色板存 flash(.irom.text.progmem)，读取一律走 pgm_read_byte/pgm_read_word。
static void drawLogo(int x, int y) {
    auto* gfx = DisplayManager::getGfx();
    for (int j = 0; j < OC_LOGO_H; j++) {
        const uint8_t* row = &OC_LOGO_BITS[j * (OC_LOGO_W / 2)];
        for (int k = 0; k < OC_LOGO_W; k++) {
            const uint8_t b = pgm_read_byte(&row[k >> 1]);
            const uint8_t nib = (k & 1) ? static_cast<uint8_t>(b & 0x0F)
                                         : static_cast<uint8_t>(b >> 4);
            if (nib == OC_LOGO_NO_COLOR) {
                continue;
            }
            uint16_t c = pgm_read_word(&OC_LOGO_PALETTE[nib]);
            c = static_cast<uint16_t>((c << 8) | (c >> 8));
            gfx->drawPixel(static_cast<int16_t>(x + k), static_cast<int16_t>(y + j), c);
        }
    }
}

// 颜色阈值 ≥50 绿 / ≥20 黄 / <20 红（同旧工程）
static uint16_t barColorFor(int remaining) {
    if (remaining >= 50) return C_GREEN;
    if (remaining >= 20) return C_YELLOW;
    return C_RED;
}

// ---------- 右栏额度行 ----------
// 一行额度：标题/百分比在上，进度条居中，重置时间用小字放底部（同旧 drawQuotaRow）
void UsageManager::drawQuotaRow(int y, const char* label, const OpenCodeGoWindow& w) {
    auto* gfx = DisplayManager::getGfx();
    gfx->fillRect(122, y, 112, ROW_H, C_BG);

    u8g2().setFont(FONT_LABEL);
    drawText(QUOTA_X0, y + 3, label, C_LABEL);

    String pct = "--";
    uint16_t pctColor = C_RED;
    int remaining = 0;
    if (w.present && w.valid) {
        remaining = w.remaining();
        pct = String(remaining) + "%";
        pctColor = C_WHITE;
    }
    u8g2().setFont(FONT_PCT);
    int pw = textWidth(pct);
    drawText(QUOTA_X1 - pw, y + 2, pct, pctColor);

    gfx->fillRect(QUOTA_X0, y + 28, QUOTA_X1 - QUOTA_X0, 12, C_BORDER);
    if (w.present && w.valid && remaining > 0) {
        gfx->fillRect(QUOTA_X0, y + 28,
                      static_cast<int32_t>(QUOTA_X1 - QUOTA_X0) * remaining / 100, 12,
                      barColorFor(remaining));
    }

    u8g2().setFont(FONT_MINI);
    if (w.present && !w.valid) {
        String inv = "INVALID";
        drawText(QUOTA_X1 - textWidth(inv), y + 44, inv, C_RED);
    } else {
        String reset = formatReset(w.resetsAt);
        if (reset.length() > 0) {
            drawText(QUOTA_X1 - textWidth(reset), y + 44, reset, C_SUB);
        }
    }
}

// ---------- 左栏时钟区 ----------
static int currentLocalMinute() {
    if (!timeSynced()) return -1;
    struct tm tmv = localTmNow();
    return tmv.tm_hour * 60 + tmv.tm_min;
}

static int currentLocalSecond() {
    if (!timeSynced()) return -1;
    return localTmNow().tm_sec;
}

// 秒摆动：按当前秒做 4 步循环 左-中-右-中（sec&3），偏移 ±SEP_SWING
static int sepOffsetForSecond(int sec) {
    static const int kOffsets[4] = {-SEP_SWING, 0, SEP_SWING, 0};
    return kOffsets[sec & 3];
}

// 短分隔线：已同步时按当前秒左右摆动，未同步时静态居中
void UsageManager::drawSeparator() {
    auto* gfx = DisplayManager::getGfx();
    int x = CLOCK_CX - SEP_W / 2;
    if (timeSynced()) x += sepOffsetForSecond(currentLocalSecond());
    gfx->drawFastHLine(static_cast<int16_t>(x), SEP_Y, SEP_W, C_ACCENT);
}

// 左栏顶部日期星期：MM-DD WKD（未同步占位 "-- --"）
void UsageManager::drawDateLine() {
    String s;
    if (timeSynced()) {
        static const char* const kWeekday[7] = {"SUN", "MON", "TUE", "WED",
                                                "THU", "FRI", "SAT"};
        struct tm tmv = localTmNow();
        s = formatLocalTime(time(nullptr) + TZ_OFFSET_SEC, "%m-%d");
        s += ' ';
        s += kWeekday[tmv.tm_wday];  // tm_wday: 0=Sun .. 6=Sat
    } else {
        s = "-- --";
    }
    u8g2().setFont(FONT_MINI);
    drawText(DATE_X, DATE_Y, s, C_SUB);
}

// 左栏时钟：日期在顶、HH 大字在中上、MM 大字在下，中间一条摆动分隔线
void UsageManager::drawClock() {
    drawDateLine();  // 日期只在零点变化，由分钟/整盒重绘天然覆盖

    int minuteOfDay = currentLocalMinute();
    String hh = "--", mm = "--";
    if (minuteOfDay >= 0) {
        struct tm tmv = localTmNow();
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", tmv.tm_hour);
        hh = buf;
        snprintf(buf, sizeof(buf), "%02d", tmv.tm_min);
        mm = buf;
    }

    u8g2().setFont(FONT_BIG);
    int hw = textWidth(hh);
    drawText(CLOCK_CX - hw / 2, HH_Y, hh, C_WHITE);

    drawSeparator();  // 秒摆动分隔线（未同步时居中）

    int mw = textWidth(mm);
    drawText(CLOCK_CX - mw / 2, MM_Y, mm, C_WHITE);

    // 记录本次绘制的分钟/秒，供 tick 去重（未同步时不记录，保持 --/-- 可继续尝试）
    if (minuteOfDay >= 0) {
        lastClockMinute = minuteOfDay;
        lastClockSecond = currentLocalSecond();
    }
}

// 常驻 tick（update() 每轮进一次，仅变化时碰屏）：
//  - 分钟变化：整块时钟盒重绘（数字 + 分隔线）
//  - 秒变化：只清/重画分隔线小条带，不碰 HH/MM 数字
//  未同步时分隔线保持静态居中
void UsageManager::tickUi() {
    int m = currentLocalMinute();
    if (m < 0) return;

    auto* gfx = DisplayManager::getGfx();
    if (m != lastClockMinute) {  // 分钟变化 -> 全时钟盒重绘
        gfx->startWrite();
        gfx->fillRect(CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H, C_BG);
        drawClock();  // 一并更新 lastClockMinute / lastClockSecond
        gfx->endWrite();
        return;
    }

    int s = currentLocalSecond();  // 秒变化 -> 只重画分隔线条带
    if (s == lastClockSecond) return;
    lastClockSecond = s;

    gfx->startWrite();
    gfx->fillRect(SEP_STRIP_X, SEP_STRIP_Y, SEP_STRIP_W, SEP_STRIP_H, C_BG);
    drawSeparator();
    gfx->endWrite();
}

// ---------- 右栏状态行 ----------
// 错误串压缩为 ASCII 短串（同旧 shortUpdateError）
static String shortUpdateError() {
    if (lastErrorText.startsWith("Network")) return "Network";
    if (lastErrorText.startsWith("WiFi")) return "WiFi";
    if (lastErrorText.startsWith("HTTP")) return lastErrorText;  // "HTTP 429" 等已短
    if (lastErrorText.startsWith("Server")) return "Server";
    if (lastErrorText.startsWith("Response") || lastErrorText.startsWith("Empty") ||
        lastErrorText.startsWith("Resp")) {
        return "Resp";
    }
    if (lastErrorText.startsWith("Bad")) return "BadKey";
    if (lastErrorText.startsWith("No Go") || lastErrorText.startsWith("NoPlan")) return "NoPlan";
    if (lastErrorText.startsWith("No Key") || lastErrorText.startsWith("NoKey")) return "NoKey";
    if (lastErrorText.length() <= 8) return lastErrorText;
    return lastErrorText.substring(0, 8);
}

// 右栏顶部 slim 状态行，四态（同旧 drawUpdateRow）：
//   无成功无错误 -> "NO UPDATE"
//   无成功有错误 -> "ERR <lastError>"（截断至 mini 字体 112px 内，红色）
//   有成功无错误 -> "UPDATE HH:MM"
//   有成功有错误 -> "UPDATE HH:MM E:<short>"（截断保 112px 内，红色）
void UsageManager::drawUpdateRow() {
    auto* gfx = DisplayManager::getGfx();
    gfx->fillRect(122, STATUS_Y, 112, STATUS_H, C_BG);

    u8g2().setFont(FONT_MINI);
    String upd;
    uint16_t color = C_SUB;
    bool hasErr = lastErrorText.length() > 0;
    if (lastSuccessTime == 0 && !hasErr) {
        upd = "NO UPDATE";
    } else if (lastSuccessTime == 0 && hasErr) {
        upd = "ERR " + lastErrorText;
        while (upd.length() > 4 && textWidth(upd) > 112) {
            upd.remove(upd.length() - 1);
        }
        color = C_RED;
    } else if (!hasErr) {
        upd = "UPDATE " + formatLocalTime(lastSuccessTime, "%H:%M");
    } else {
        String base = "UPDATE " + formatLocalTime(lastSuccessTime, "%H:%M") + " E:";
        String sh = shortUpdateError();
        upd = base + sh;
        while (sh.length() > 0 && textWidth(upd) > 112) {
            sh.remove(sh.length() - 1);
            upd = base + sh;
        }
        color = C_RED;
    }
    drawText(QUOTA_X1 - textWidth(upd), STATUS_Y + 4, upd, color);
}

// ---------- 页面组装 ----------
// 正文区域（状态行 + 额度栏 + 时钟栏 + 中缝竖线）；调用前须保证该区域已清底
void UsageManager::drawBody() {
    drawUpdateRow();
    drawQuotaRow(ROW_TOP, "5H", usageData.rolling);
    drawQuotaRow(ROW_TOP + ROW_H, "WK.", usageData.weekly);
    drawQuotaRow(ROW_TOP + ROW_H * 2, "MO.", usageData.monthly);
    drawClock();
    DisplayManager::getGfx()->drawFastVLine(120, STATUS_Y, STATUS_H + ROW_H * 3, C_BORDER);
}

void UsageManager::drawMainPage() {
    u8g2();  // 惰性初始化 U8g2 字形引擎
    auto* gfx = DisplayManager::getGfx();
    gfx->startWrite();
    gfx->fillScreen(C_BG);
    drawLogo((240 - OC_LOGO_W) / 2, 1);
    gfx->drawFastHLine(16, 40, 208, C_BORDER);
    drawBody();
    gfx->endWrite();
}

void UsageManager::drawBootPage(bool fail) {
    u8g2();  // 惰性初始化 U8g2 字形引擎
    auto* gfx = DisplayManager::getGfx();
    gfx->fillScreen(C_BG);
    drawLogo((240 - OC_LOGO_W) / 2, 90);
    const char* hint = fail ? "WiFi failed, retrying..." : "Connecting WiFi...";
    u8g2().setFont(FONT_LABEL);
    int hw = textWidth(hint);
    drawText((240 - hw) / 2, 140, hint, fail ? C_RED : C_LABEL);
}

// ---------- 轮询 ----------
void UsageManager::begin() {
    ever_started = true;
    // 首次尽快轮询
    last_poll_ms = millis() - FAST_RETRY_MS;
    if (WiFiManager::isConnected()) {
        drawMainPage();
        mainPageDrawn = true;
    } else {
        drawBootPage(true);
    }
    Logger::info("UsageManager initialized", "OpenCodeGo");
}

void UsageManager::update() {
    if (!ever_started) {
        return;
    }

    const bool wifiReady = (wifiManager != nullptr) && !wifiManager->isApMode() &&
                           WiFiManager::isConnected();

    // 首次联网就绪时画主页面（同旧工程 onConnected hook）
    if (wifiReady && !mainPageDrawn) {
        drawMainPage();
        mainPageDrawn = true;
    }

    // 常驻时钟 tick：与网络/额度获取无关，放在最前，WiFi 掉线时也照样走时；
    // 内部自行跳过未同步；boot 页/未画主页面期间不碰屏
    if (mainPageDrawn) {
        tickUi();
    }

    if (!wifiReady) {
        return;
    }

    // 从未成功过数据时用 FAST_RETRY，有数据后等满轮询周期（失败不清 has_data，同旧工程）
    const uint32_t interval = has_data ? POLL_INTERVAL_MS : FAST_RETRY_MS;
    const uint32_t now = millis();
    if (now - last_poll_ms < interval) {
        return;
    }
    last_poll_ms = now;

    if (configManager.getOpenCodeGoApiKey()[0] == '\0' || configManager.getOpenCodeGoHost()[0] == '\0') {
        // 未填 Key 时提示（同旧工程），等满一个周期后再查
        lastErrorText = "No Key";
        drawUpdateRow();
        yield();
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    Serial.printf("Free heap before fetch: %u B\n", ESP.getFreeHeap());
    OpenCodeGoUsage fetched;
    bool ok = fetchOpenCodeGoUsage(fetched, configManager.getOpenCodeGoHost(),
                                   configManager.getOpenCodeGoPath(),
                                   configManager.getOpenCodeGoApiKey(),
                                   configManager.getVerifyTlsCert(),
                                   FETCH_TIMEOUT_MS, FETCH_RETRY_INTERVAL_MS,
                                   FETCH_MAX_ATTEMPTS);
    Serial.printf("Free heap after fetch: %u B\n", ESP.getFreeHeap());

    if (ok) {
        usageData = fetched;  // 只在成功时提交，失败保留上次数据供显示
        has_data = true;
        lastErrorText = "";
        lastSuccessTime = time(nullptr) + TZ_OFFSET_SEC;

        Serial.printf("Quota: 5h %d%% / week %d%% / month %d%%\n",
                      usageData.rolling.percent, usageData.weekly.percent,
                      usageData.monthly.percent);

        // 局部重绘：清空正文区（含底部错误条）+ 重画整块正文（含时钟），
        // 与 drawMainPage 的 drawBody 完全一致，避免残影、避免整屏闪动
        auto* gfx = DisplayManager::getGfx();
        gfx->startWrite();
        gfx->fillRect(0, 44, 240, 240 - 44, C_BG);
        drawBody();
        gfx->endWrite();
        yield();
    } else {
        Serial.printf("Fetch failed after retries: %s (HTTP %d)\n",
                      fetched.error.c_str(), fetched.http_code);
        // 屏幕保留已显示的额度数据，仅更新状态行 + 底部错误条；
        // 下次成功会自动重绘覆盖。startWrite/endWrite 严格配对
        String e = fetched.error;
        if (e.isEmpty()) e = "Fetch fail";
        if (e.length() > 18) e = e.substring(0, 18);
        lastErrorText = e;

        auto* gfx = DisplayManager::getGfx();
        gfx->startWrite();
        drawUpdateRow();
        gfx->fillRect(0, 226, 240, 14, C_BG);
        u8g2().setFont(FONT_MINI);
        String err = "ERR " + lastErrorText;
        drawText((240 - textWidth(err)) / 2, 229, err, C_RED);
        gfx->endWrite();
        yield();
    }
}

// ---- UI 层访问器 ----
bool UsageManager::hasData() { return has_data; }
const String& UsageManager::lastError() { return lastErrorText; }
const OpenCodeGoUsage& UsageManager::usage() { return usageData; }
