// SPDX-License-Identifier: GPL-3.0-or-later
#include "opencodego/UsageManager.h"

#include <Logger.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <time.h>

#include "display/DisplayManager.h"
#include "opencodego/OpenCodeLogo.h"
#include "wireless/WiFiManager.h"

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

// ---- 上位机推送缓冲（静态定长，零堆常驻；时间戳见 pushBalance）----
static char s_lines[UsageManager::kBalanceLines][UsageManager::kLineCap] = {{0}};
static char s_status[UsageManager::kStatusCap] = {0};
static bool s_hasPush = false;
static bool s_hasStatus = false;
static time_t s_pushEpoch = 0;      // 推送时刻 UTC epoch；推送时 NTP 未同步则为 0
static uint32_t s_pushMillis = 0;   // 推送时刻 millis（age_s 基准，未同步时也有效）

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

// ---- 字体（U8g2_for_Adafruit_GFX，替代旧 TFT_eSPI FreeSans/内置字体）----
// - 时钟大数字不用字体：自绘七段数码管（见下方 drawSegDigit），恢复旧工程
//   TFT_eSPI Font 7（七段数码管 48px）观感，并省出约 10KB flash；
// - 百分比     u8g2_font_helvB12_tf：Helvetica Bold 12pt，接近旧 FreeSans12pt7b；
// - 标签       u8g2_font_helvR10_tf：Helvetica Regular 10pt，接近旧 FreeSans9pt7b；
// - 小字       u8g2_font_6x10_tf：6px 字宽同旧内置 Font 1（6x8），略高 2px。
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

// ---- 运行状态 ----
static bool ever_started = false;

// 上次绘制的本地分钟（hour*60+min）/ 秒；-1 = 尚未绘制
static int lastClockMinute = -1;
static int lastClockSecond = -1;
// 已绘制的 HH/MM 串与年积日（tickUi 差量更新基准）
static String sClockHH;
static String sClockMM;
static int sClockYday = -1;

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

// ---------- 定长缓冲绘制工具（零堆：直接走 const char*，不经过 String）----------
// 调用前须 u8g2().setFont(...)；测宽与绘制用同一字体。
// 参数 y 为字形顶部（同旧 TFT_eSPI TL_DATUM），内部按当前字体 ascent 折算基线
static int textWidthC(const char* s) {
    return static_cast<int>(u8g2().getUTF8Width(s));
}

static void drawTextC(int x, int y, const char* s, uint16_t color) {
    u8g2().setForegroundColor(color);
    u8g2().drawUTF8(static_cast<int16_t>(x),
                    static_cast<int16_t>(y + u8g2().getFontAscent()), s);
}

// UTF-8 安全截断：按显示宽度把 buf 就地截到 maxPx 内（不得溢出侵入相邻元素）。
// 从尾部逐字符删除（跳过 10xxxxxx 后续字节），调用方保证 buf 以 NUL 结尾。
static void truncateToFit(char* buf, int maxPx) {
    while (buf[0] != '\0' && textWidthC(buf) > maxPx) {
        size_t len = strlen(buf);
        if (len == 0) break;
        size_t cut = len - 1;
        // 跳过 UTF-8 后续字节，找到字符起始字节
        while (cut > 0 && (static_cast<uint8_t>(buf[cut]) & 0xC0) == 0x80) {
            cut--;
        }
        buf[cut] = '\0';
    }
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

// logo 位图恢复 16bpp 原始位图（23 色保真），按 TFT_eSPI pushImage 字节序
// 预交换，此处还原为标准 RGB565；0x0000 为透明色，只绘制字标像素。
// 位置/尺寸/逐点绘制路径不变；位图存 flash(.irom.text.progmem，~11.4KB、
// 0 RAM)，读取走 pgm_read_word（32-bit 访问，见 IromAccess.h）。
// 【字节序证据】TFT_eSPI 中：
//  - Processors/TFT_eSPI_ESP8266.c: pushPixels(const void*) 直接
//    spi.writePattern(data,...) 原样发字节、不做交换；而 tft_Write_16(C)
//    定义为 (C)<<8|(C)>>8 内部交换（TFT_eSPI_ESP8266.h:161）；
//  - TFT_eSPI.cpp pushImage(PROGMEM data, transp)：buffer[] = pgm_read_word
//    原值不经交换交给 pushPixels → 表内字节序即 SPI 发送序（高字节次序由
//    表内容决定）→ oc_logo 表为「预交换（大端）」字节序，非真实 RGB565；
//  - 本工程 Arduino_GFX 期望真实 RGB565，故绘制前须 (c<<8)|(c>>8) 还原，
//    与旧固件颜色一致【交换必须在判透明之前还原？见下】——透明判定：
//    旧代码 `if (!_swapBytes) transp = transp>>8|transp<<8;` 后比较的是
//    表内原始值 vs 交换后的 transp（0x0000 交换不变）→ 即按表内原始值判 0；
//    新代码 (c==0) 先判再交换，语义一致。旧工程未调用 setSwapBytes（全仓
//    grep 无），_swapBytes 默认 false（TFT_eSPI.cpp:463）。
static void drawLogo(int x, int y) {
    auto* gfx = DisplayManager::getGfx();
    for (int j = 0; j < OC_LOGO_H; j++) {
        const uint16_t* row = &OC_LOGO_BITS[j * OC_LOGO_W];
        for (int k = 0; k < OC_LOGO_W; k++) {
            uint16_t c = static_cast<uint16_t>(pgm_read_word(&row[k]));
            if (c == OC_LOGO_NO_COLOR) {
                continue;
            }
            c = static_cast<uint16_t>((c << 8) | (c >> 8));
            gfx->drawPixel(static_cast<int16_t>(x + k), static_cast<int16_t>(y + j),
                           c);
        }
    }
}

// ---------- 七段数码管大字（像素级复刻旧工程 TFT_eSPI Font 7）----------
// 字模见 include/opencodego/SegFont7.h：由 TFT_eSPI Font7srle.c 的 8-bit RLE
// 原始字模按 drawChar() 同款算法逐像素解码的 1bpp 行位图（32x48/字符，
// MSB 左），渲染逐像素与旧固件一致。数字格 32x48，'-' 占满 32px 宽
// （同旧固件字宽表 widtbl_f7s），'-' 前备用的 ':' 实际 12px 宽。
// 渲染：每行把连续置位段合并为 drawFastHLine（等价旧固件 textsize=1 的
// fillRect 行内 run 绘制），只画字不填底；颜色 C_WHITE。
// 表存 flash，读取走 pgm_read_dword（IROM 32-bit 访问约束，见 IromAccess.h）。

#include "opencodego/SegFont7.h"

// 单个数字字符（'0'-'9' / '-' / ':'，':' 宽 12 其余 32），左上角 (x, y)
static void drawSegDigit(int x, int y, char ch, uint16_t color) {
    int idx;
    if (ch >= '0' && ch <= '9') {
        idx = ch - '0';
    } else if (ch == '-') {
        idx = SEG7_DASH;
    } else if (ch == ':') {
        idx = SEG7_COLON;  // ':' 仅 12px 宽（备用；时钟项目中缝线已表达分隔）
    } else {
        return;
    }
    auto* gfx = DisplayManager::getGfx();
    for (int row = 0; row < SEG7_H; row++) {
        uint32_t bits = pgm_read_dword(&kSeg7Font[idx][row]);
        if (bits == 0) continue;
        int k = 0;
        while (k < SEG7_W) {
            if (!(bits & (1u << (31 - k)))) {
                k++;
                continue;
            }
            int run = 0;
            while (k + run < SEG7_W && (bits & (1u << (31 - (k + run))))) run++;
            gfx->drawFastHLine(static_cast<int16_t>(x + k),
                               static_cast<int16_t>(y + row),
                               static_cast<int16_t>(run), color);
            k += run;
        }
    }
}

// 七段大字：每字符宽 32（':' 宽 12），按真实字宽以 cx 居中
//（无冒号串与旧版逐像素一致；含冒号串修正旧版 total=len*32 的偏心）
static void drawSegText(int cx, int topY, const String& s, uint16_t color) {
    int total = 0;
    for (unsigned i = 0; i < s.length(); i++) {
        total += (s[i] == ':') ? 12 : 32;
    }
    int x = cx - total / 2;
    for (unsigned i = 0; i < s.length(); i++) {
        drawSegDigit(x, topY, s[i], color);
        x += (s[i] == ':') ? 12 : 32;
    }
}

// ---------- 右栏额度行（推送纯文本，自适应字号）----------
// 行内容 = 上位机推送整行文本（自带标签，设备零语义只排版），在
// QUOTA_X0..QUOTA_X1 内右对齐（保持旧百分比的视觉锚点感）：
//   FONT_PCT 能放下 → 大字；否则 FONT_MINI，仍超宽则 truncateToFit 截断。
// 空槽（首次推送前）显示红色 "--" 占位；有内容为白色。
// 版式：单行文本在 56px 行内光学居中（大字 y+18 / 小字 y+21，字形视觉
// 中心落在行中线附近，略偏上）。旧进度条空槽已移除——推送模式无百分比
// 可填，实心空槽只是视觉死重；空出的留白让行内呼吸，与左栏 HH/分隔线/MM
// 的节奏对齐（行 1/2/3 文本分别落在 HH 带 / 分隔线 / MM 带高度）。
// 行内除文本无任何装饰，页面气质由顶部分隔线与中缝竖线维持。
void UsageManager::drawQuotaRow(int y, const char* text) {
    auto* gfx = DisplayManager::getGfx();
    gfx->fillRect(122, y, 112, ROW_H, C_BG);

    const bool empty = (text == nullptr || text[0] == '\0');
    const char* shown = empty ? "--" : text;
    const uint16_t color = empty ? C_RED : C_WHITE;

    char fit[UsageManager::kLineCap];
    strlcpy(fit, shown, sizeof(fit));

    u8g2().setFont(FONT_PCT);
    if (textWidthC(fit) <= QUOTA_X1 - QUOTA_X0) {
        drawTextC(QUOTA_X1 - textWidthC(fit), y + 18, fit, color);
        return;
    }

    u8g2().setFont(FONT_MINI);
    truncateToFit(fit, QUOTA_X1 - QUOTA_X0);  // 当前字体即 MINI，测宽一致
    drawTextC(QUOTA_X1 - textWidthC(fit), y + 21, fit, color);
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

// 前置声明：实现见下方纯时钟页段
static void drawSegRuns(int x, int y, uint32_t bits, uint16_t color);
static void redrawSegGlyphDiff(int x, int y, char oldCh, char newCh, uint16_t color);

// 串级差量更新：按 32/12 走宽逐字形比对，仅翻转差异字形的差异段
static void redrawSegTextDiff(int cx, int topY, const String& oldS, const String& newS, uint16_t color) {
    int total = 0;
    for (unsigned i = 0; i < newS.length(); i++) {
        total += (newS[i] == ':') ? 12 : 32;
    }

    int x = cx - total / 2;

    for (unsigned i = 0; i < newS.length(); i++) {
        const char oldCh = (i < oldS.length()) ? oldS[i] : '\0';
        redrawSegGlyphDiff(x, topY, oldCh, newS[i], color);
        x += (newS[i] == ':') ? 12 : 32;
    }
}

// 时间串生成（drawClock / tickUi 共用）
static void clockStrings(String& hh, String& mm) {
    hh = "--";
    mm = "--";

    if (currentLocalMinute() >= 0) {
        const struct tm tmv = localTmNow();
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", tmv.tm_hour);
        hh = buf;
        snprintf(buf, sizeof(buf), "%02d", tmv.tm_min);
        mm = buf;
    }
}

// 左栏时钟：日期在顶、HH 大字在中上、MM 大字在下，中间一条摆动分隔线
void UsageManager::drawClock() {
    drawDateLine();  // 日期只在零点变化，由分钟/整盒重绘天然覆盖

    String hh;
    String mm;
    clockStrings(hh, mm);

    drawSegText(CLOCK_CX, HH_Y, hh, C_WHITE);

    drawSeparator();  // 秒摆动分隔线（未同步时居中）

    drawSegText(CLOCK_CX, MM_Y, mm, C_WHITE);

    // 记录已绘制状态，供 tickUi 差量更新（未同步时不记录，保持 --/-- 可继续尝试）
    sClockHH = hh;
    sClockMM = mm;

    const int minuteOfDay = currentLocalMinute();
    if (minuteOfDay >= 0) {
        lastClockMinute = minuteOfDay;
        lastClockSecond = currentLocalSecond();
    }
}

// 常驻 tick（update() 每轮进一次，仅变化时碰屏；全部走像素差量，无清底闪烁）：
//  - 分钟变化：仅差量更新 HH/MM 中变化的字形（换日补画日期行）
//  - 秒变化：仅翻转摆动分隔线的新旧尾段
//  未同步时分隔线保持静态居中
void UsageManager::tickUi() {
    const int m = currentLocalMinute();
    if (m < 0) {
        return;
    }

    auto* gfx = DisplayManager::getGfx();

    if (m != lastClockMinute) {
        String hh;
        String mm;
        clockStrings(hh, mm);

        const struct tm tmv = localTmNow();

        gfx->startWrite();
        if (tmv.tm_yday != sClockYday) {
            sClockYday = tmv.tm_yday;
            drawDateLine();
        }
        if (hh != sClockHH) {
            redrawSegTextDiff(CLOCK_CX, HH_Y, sClockHH, hh, C_WHITE);
        }
        if (mm != sClockMM) {
            redrawSegTextDiff(CLOCK_CX, MM_Y, sClockMM, mm, C_WHITE);
        }
        gfx->endWrite();

        sClockHH = hh;
        sClockMM = mm;
        lastClockMinute = m;
        lastClockSecond = currentLocalSecond();
        return;
    }

    int s = currentLocalSecond();  // 秒变化 -> 只差量更新分隔线
    if (s == lastClockSecond) {
        return;
    }

    const int oldX = CLOCK_CX - SEP_W / 2 + sepOffsetForSecond(lastClockSecond);
    const int newX = CLOCK_CX - SEP_W / 2 + sepOffsetForSecond(s);
    lastClockSecond = s;

    if (newX == oldX) {
        return;
    }

    // 两线段同行同宽，仅翻转互不重叠的尾段（重叠区不动 → 零闪烁）
    gfx->startWrite();
    if (newX > oldX) {
        gfx->drawFastHLine(oldX, SEP_Y, newX - oldX, C_BG);
        gfx->drawFastHLine(oldX + SEP_W, SEP_Y, newX - oldX, C_ACCENT);
    } else {
        gfx->drawFastHLine(newX + SEP_W, SEP_Y, oldX - newX, C_BG);
        gfx->drawFastHLine(newX, SEP_Y, oldX - newX, C_ACCENT);
    }
    gfx->endWrite();
}

// ---------- 右栏状态行 ----------
// 右栏顶部 slim 状态行（推送模式，三态）：
//   从未推送      -> "WAITING"
//   有推送+状态行  -> 状态行文本（截断至 mini 字体 112px 内）
//   有推送无状态行 -> "UPDATE HH:MM"（推送时刻本地时间；NTP 未同步时 "UPD --:--"）
void UsageManager::drawUpdateRow() {
    auto* gfx = DisplayManager::getGfx();
    gfx->fillRect(122, STATUS_Y, 112, STATUS_H, C_BG);

    u8g2().setFont(FONT_MINI);
    char upd[UsageManager::kStatusCap];
    if (!s_hasPush) {
        strlcpy(upd, "WAITING", sizeof(upd));
    } else if (s_hasStatus && s_status[0] != '\0') {
        strlcpy(upd, s_status, sizeof(upd));
    } else if (s_pushEpoch > 0) {
        String t = "UPD " + formatLocalTime(s_pushEpoch + TZ_OFFSET_SEC, "%H:%M");
        strlcpy(upd, t.c_str(), sizeof(upd));
    } else {
        strlcpy(upd, "UPD --:--", sizeof(upd));
    }
    truncateToFit(upd, 112);
    drawTextC(QUOTA_X1 - textWidthC(upd), STATUS_Y + 4, upd, C_SUB);
}

// ---------- 页面组装 ----------
// 正文区域（状态行 + 额度栏 + 时钟栏 + 中缝竖线）；调用前须保证该区域已清底
void UsageManager::drawBody() {
    drawUpdateRow();
    drawQuotaRow(ROW_TOP, s_lines[0]);
    drawQuotaRow(ROW_TOP + ROW_H, s_lines[1]);
    drawQuotaRow(ROW_TOP + ROW_H * 2, s_lines[2]);
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

// ---------- 纯时钟页（clock 场景）----------
// 顶部小字日期星期 + 七段大字 HH:MM:SS（复用 SegFont7 字模/主题色/时间辅助）
static constexpr int CLOCK_PAGE_CX = 120;
static constexpr int CLOCK_PAGE_DATE_Y = 34;
static constexpr int CLOCK_PAGE_TIME_Y = 96;
static constexpr int CLOCK_PAGE_TIME_BAND_X = 8;
static constexpr int CLOCK_PAGE_TIME_BAND_W = 224;

static int clockPageLastSec = -1;
static int clockPageLastDay = -1;
static char clockPageShown[16] = {0};  // 上次绘制的时间串，逐字形差量更新

static String clockPageTimeString() {
    if (!timeSynced()) {
        return "--:--:--";
    }

    const struct tm tmv = localTmNow();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    return String(buf);
}

// 按 32bit 字模行位图把置位段合并成 drawFastHLine
static void drawSegRuns(int x, int y, uint32_t bits, uint16_t color) {
    auto* gfx = DisplayManager::getGfx();
    int k = 0;
    while (k < SEG7_W) {
        if (!(bits & (1u << (31 - k)))) {
            k++;
            continue;
        }
        int run = 0;
        while (k + run < SEG7_W && (bits & (1u << (31 - (k + run))))) {
            run++;
        }
        gfx->drawFastHLine(static_cast<int16_t>(x + k), static_cast<int16_t>(y), static_cast<int16_t>(run), color);
        k += run;
    }
}

// 像素差量重绘单个字形：仅翻转新旧字形的差异段（无清底 → 消除秒跳闪烁）
static void redrawSegGlyphDiff(int x, int y, char oldCh, char newCh, uint16_t color) {
    const auto idxOf = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch == '-') {
            return SEG7_DASH;
        }
        if (ch == ':') {
            return SEG7_COLON;
        }
        return -1;
    };

    const int iOld = idxOf(oldCh);
    const int iNew = idxOf(newCh);

    if (iOld == iNew) {
        return;
    }

    auto* gfx = DisplayManager::getGfx();
    gfx->startWrite();

    for (int row = 0; row < SEG7_H; row++) {
        const uint32_t o = (iOld >= 0) ? pgm_read_dword(&kSeg7Font[iOld][row]) : 0U;
        const uint32_t n = (iNew >= 0) ? pgm_read_dword(&kSeg7Font[iNew][row]) : 0U;

        drawSegRuns(x, y + row, o & ~n, C_BG);   // 旧有新无 → 擦除
        drawSegRuns(x, y + row, n & ~o, color);  // 新有旧无 → 点亮
    }

    gfx->endWrite();
}

// 时间行逐字形更新：force=全量绘制（入场），否则只动变化的字形
static void drawClockPageTime(bool force) {
    static const int kGlyphW[8] = {32, 32, 12, 32, 32, 12, 32, 32};  // HH:MM:SS
    const String s = clockPageTimeString();
    const int startX = CLOCK_PAGE_CX - 216 / 2;

    int x = startX;

    for (int i = 0; i < 8; i++) {
        if (force) {
            drawSegDigit(x, CLOCK_PAGE_TIME_Y, s[i], C_WHITE);
        } else {
            redrawSegGlyphDiff(x, CLOCK_PAGE_TIME_Y, clockPageShown[i], s[i], C_WHITE);
        }

        clockPageShown[i] = s[i];
        x += kGlyphW[i];
    }
}

static void drawClockPageDate() {
    auto* gfx = DisplayManager::getGfx();
    gfx->fillRect(0, CLOCK_PAGE_DATE_Y - 2, 240, 18, C_BG);

    String s;

    if (timeSynced()) {
        static const char* const kWeekday[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        const struct tm tmv = localTmNow();
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s", 1900 + tmv.tm_year, 1 + tmv.tm_mon, tmv.tm_mday,
                 kWeekday[tmv.tm_wday % 7]);
        s = buf;
    } else {
        s = "--";
    }

    u8g2().setFont(FONT_LABEL);
    drawText(CLOCK_PAGE_CX - textWidth(s) / 2, CLOCK_PAGE_DATE_Y, s, C_SUB);
}

// 纯时钟页入场：从零绘制所有元素（场景切换契约）
void UsageManager::drawClockPage() {
    u8g2();
    auto* gfx = DisplayManager::getGfx();
    gfx->startWrite();
    gfx->fillScreen(C_BG);
    drawClockPageDate();
    gfx->endWrite();

    memset(clockPageShown, 0, sizeof(clockPageShown));
    drawClockPageTime(true);  // 全量绘制并记录字形状态

    clockPageLastSec = timeSynced() ? currentLocalSecond() : -1;
    clockPageLastDay = timeSynced() ? localTmNow().tm_yday : -1;
}

// 秒级 tick：字形像素差量更新（无清底闪烁）；换日只重绘日期行
void UsageManager::tickClockPage() {
    if (!timeSynced()) {
        return;
    }

    const struct tm tmv = localTmNow();

    if (tmv.tm_yday != clockPageLastDay) {
        clockPageLastDay = tmv.tm_yday;
        drawClockPageDate();
    }

    if (tmv.tm_sec == clockPageLastSec) {
        return;
    }
    clockPageLastSec = tmv.tm_sec;

    drawClockPageTime(false);
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

// ---------- 推送接收（无轮询：数据源为 POST /api/v1/balance）----------
void UsageManager::begin() {
    ever_started = true;
    Logger::info("UsageManager initialized (push mode)", "Balance");
}

void UsageManager::pushBalance(const char* const* lines, uint8_t nLines, const char* status,
                               bool hasStatus) {
    for (uint8_t i = 0; i < kBalanceLines; i++) {
        if (i < nLines && lines != nullptr && lines[i] != nullptr) {
            strlcpy(s_lines[i], lines[i], kLineCap);
        } else {
            s_lines[i][0] = '\0';
        }
    }
    if (hasStatus && status != nullptr && status[0] != '\0') {
        strlcpy(s_status, status, kStatusCap);
        s_hasStatus = true;
    } else {
        s_status[0] = '\0';
        s_hasStatus = false;
    }
    s_hasPush = true;
    s_pushMillis = millis();
    s_pushEpoch = timeSynced() ? time(nullptr) : 0;
    // 立即刷新显示：经整屏重绘由 update() 重画主页面（严禁绘制启动屏，见坑 #4）
    DisplayManager::requestFullRedraw();
    Logger::info("Balance push stored", "Balance");
}

// 场景入场：必须从零绘制所有元素（场景切换契约）
void UsageManager::enterScene() {
    mainPageDrawn = false;
    lastClockMinute = -1;
    lastClockSecond = -1;

    if (WiFiManager::isConnected()) {
        drawMainPage();
        mainPageDrawn = true;
    } else {
        drawBootPage(true);
    }
}

// 场景退场：复位局部更新状态，防止下次 tick 按旧基准补笔留残影
void UsageManager::exitScene() {
    mainPageDrawn = false;
    lastClockMinute = -1;
    lastClockSecond = -1;
}

void UsageManager::update() {
    if (!ever_started) {
        return;
    }

    const bool wifiReady = (wifiManager != nullptr) && !wifiManager->isApMode() &&
                           WiFiManager::isConnected();

    // 显示设置（rotation / 面板 profile）变更或上位机推送后，应用层整屏重绘主页面
    //（用推送缓冲/占位符绘制；不看 wifiReady；严禁绘制启动屏，见坑 #4）
    if (DisplayManager::consumeFullRedrawRequest()) {
        drawMainPage();
        mainPageDrawn = true;
        Logger::info("UsageManager: main page redrawn", "Balance");
    }

    // 首次联网就绪时画主页面（同旧工程 onConnected hook）
    if (wifiReady && !mainPageDrawn) {
        drawMainPage();
        mainPageDrawn = true;
    }

    // 常驻时钟 tick：与推送无关，放在最后，WiFi 掉线时也照样走时；
    // 内部自行跳过未同步；boot 页/未画主页面期间不碰屏
    if (mainPageDrawn) {
        tickUi();
    }
}

// ---- UI 层 / API 层访问器（返回静态缓冲指针，调用方立即拷贝）----
bool UsageManager::hasPush() { return s_hasPush; }

const char* UsageManager::lineAt(uint8_t i) {
    if (i >= kBalanceLines) return "";
    return s_lines[i];
}

bool UsageManager::hasStatus() { return s_hasStatus; }

const char* UsageManager::statusText() { return s_status; }

time_t UsageManager::pushEpoch() { return s_pushEpoch; }

uint32_t UsageManager::pushAgeSec() {
    if (!s_hasPush) return 0;
    return (millis() - s_pushMillis) / 1000UL;
}
