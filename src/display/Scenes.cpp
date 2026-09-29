// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "display/Scenes.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <Logger.h>
#include <string.h>

#include "display/DisplayManager.h"
#include "display/NoticeScene.h"
#include "display/SceneManager.h"
#include "opencodego/UsageManager.h"
#include "wireless/WiFiManager.h"
#include "config/ConfigManager.h"
#include "ntp/NTPClient.h"
#include "opencodego/StockData.h"
#include "project_version.h"
#include <time.h>

extern WiFiManager* wifiManager;
extern ConfigManager configManager;

// 源值写真 RGB；打包后做面板 BGR 色序的 R/B 字段交换（与 UsageManager::rgb565
// 及 logo/相册位图路径同一口径，否则本面板屏上红蓝颠倒）。
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t v = static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
    return static_cast<uint16_t>(((v & 0x001FU) << 11) | (v & 0x07E0U) | ((v & 0xF800U) >> 11));
}

// Arduino_GFX 的内建 6px 字体没有测量 API，按其实际字宽做像素级截断。
static auto textWidthPx(const char* text, uint8_t textSize) -> int {
    return text == nullptr ? 0 : static_cast<int>(strlen(text)) * 6 * textSize;
}

// ---------- sysinfo：系统信息 + UTC+8 时钟 ----------
// LCD 内建字体只含 ASCII（固件无 CJK 字模），故所有标签走英文；
// enter() 全量绘制；update() 只擦写「值发生变化」的单行小区域（IP/NTP/运行时长/剩余堆）
// 与分钟变化的时钟区域，绝不整屏重绘，避免闪屏。
class SystemInfoScene : public Scene {
   public:
    auto name() const -> const char* override { return "sysinfo"; }

    auto enter(const char* param) -> bool override {
        (void)param;
        m_lastMinute = -1;
        m_lastDay = -1;
        m_ntpSynced = ntpSyncedNow();
        readDeviceAddress(m_deviceAddress, sizeof(m_deviceAddress));
        readUptime(m_uptimeValue, sizeof(m_uptimeValue));
        readHeap(m_heapValue, sizeof(m_heapValue));
        char ntpStatus[64];
        ntpStatusText(ntpStatus, sizeof(ntpStatus));
        DisplayManager::clearScreen();

        drawLineChars("IP", m_deviceAddress, 8);
        drawLine("WIFI", wifiValue(), 25);
        drawLineChars("NTP", ntpStatus, 42);
        drawLine("CHIP", String(ESP.getChipId()), 59);
        drawLineChars("UPTIME", m_uptimeValue, 76);
        drawLineChars("HEAP", m_heapValue, 93);
        drawLine("DISPLAY", displayValue(), 110);
        drawLine("VERSION", PROJECT_VER_STR, 127);

        drawDate();
        drawClock(true);

        return true;
    }

    auto update() -> void override {
        char address[32];
        readDeviceAddress(address, sizeof(address));
        if (strcmp(address, m_deviceAddress) != 0) {
            strlcpy(m_deviceAddress, address, sizeof(m_deviceAddress));
            drawLineChars("IP", m_deviceAddress, 8);
        }

        const bool ntpSynced = ntpSyncedNow();
        if (ntpSynced != m_ntpSynced) {
            m_ntpSynced = ntpSynced;
            char status[64];
            ntpStatusText(status, sizeof(status));
            drawLineChars("NTP", status, 42);
        }

        char uptime[16];
        readUptime(uptime, sizeof(uptime));
        if (strcmp(uptime, m_uptimeValue) != 0) {
            strlcpy(m_uptimeValue, uptime, sizeof(m_uptimeValue));
            drawLineChars("UPTIME", m_uptimeValue, 76);
        }

        char heap[16];
        readHeap(heap, sizeof(heap));
        if (strcmp(heap, m_heapValue) != 0) {
            strlcpy(m_heapValue, heap, sizeof(m_heapValue));
            drawLineChars("HEAP", m_heapValue, 93);
        }

        const time_t now = time(nullptr) + 8 * 3600;
        // 日期行也要随校时刷新：开机入场时 NTP 可能未同步（会先画成 1970），同步后立即纠正
        if (static_cast<int>(now / 86400) != m_lastDay) {
            drawDate();
        }
        const int minute = static_cast<int>((now / 60) % 1440);
        if (minute != m_lastMinute) {
            drawClock(false);
        }
    }

    auto exit() -> void override {}

   private:
    static constexpr uint16_t C_BG = rgb565(0x00, 0x00, 0x00);
    static constexpr uint16_t C_SUB = rgb565(0x8A, 0x94, 0xB8);
    static constexpr uint16_t C_WHITE = rgb565(0xFF, 0xFF, 0xFF);
    static constexpr uint16_t C_ACCENT = rgb565(0x4D, 0x6B, 0xFE);
    static constexpr int16_t VALUE_X = 80;
    static constexpr int16_t VALUE_MAX_W = 151;
    static constexpr int16_t CLOCK_Y = 181;

    int m_lastMinute = -1;
    int m_lastDay = -1;
    bool m_ntpSynced = false;
    char m_deviceAddress[32] = {0};
    char m_uptimeValue[16] = {0};
    char m_heapValue[16] = {0};

    static auto ntpSyncedNow() -> bool { return time(nullptr) > 1600000000; }

    static auto readDeviceAddress(char* output, size_t outputSize) -> void {
        if (wifiManager == nullptr) {
            strlcpy(output, "--", outputSize);
            return;
        }

        const IPAddress ip = wifiManager->getIP();
        snprintf(output, outputSize, "%u.%u.%u.%u%s", ip[0], ip[1], ip[2], ip[3],
                 wifiManager->isApMode() ? " (AP)" : "");
    }

    static auto ntpStatusText(char* output, size_t outputSize) -> void {
        const char* server = NTPClient::effectiveServer();
        if (server == nullptr || server[0] == '\0') {
            server = "--";
        }
        snprintf(output, outputSize, "%s / %s", server, ntpSyncedNow() ? "SYNCED" : "NOSYNC");
    }

    auto wifiValue() -> String {
        String ssid = WiFi.SSID();
        if (ssid.length() == 0 && configManager.getSSID() != nullptr) {
            ssid = configManager.getSSID();
        }
        if (ssid.length() == 0) {
            return String("--");
        }
        return ssid + " / " + String(WiFi.RSSI()) + "dBm";
    }

    static auto readUptime(char* output, size_t outputSize) -> void {
        const unsigned long totalMinutes = millis() / 60000UL;
        if (totalMinutes >= 1440UL) {
            snprintf(output, outputSize, "%lud%02lu:%02lu", totalMinutes / 1440UL,
                     (totalMinutes / 60UL) % 24UL, totalMinutes % 60UL);
        } else {
            snprintf(output, outputSize, "%02lu:%02lu:%02lu", totalMinutes / 60UL,
                     totalMinutes % 60UL, (millis() / 1000UL) % 60UL);
        }
    }

    static auto readHeap(char* output, size_t outputSize) -> void {
        snprintf(output, outputSize, "%uKB", ESP.getFreeHeap() / 1024U);
    }

    auto displayValue() -> String {
        return String(configManager.getLCDBrightness()) + "% / ROT " + String(configManager.getLCDRotationSafe());
    }

    auto drawLine(const char* label, const String& value, int16_t y) -> void {
        drawLineChars(label, value.c_str(), y);
    }

    auto drawLineChars(const char* label, const char* value, int16_t y) -> void {
        auto* gfx = DisplayManager::getGfx();
        char fitted[64];
        const int maxChars = VALUE_MAX_W / 6;
        snprintf(fitted, sizeof(fitted), "%.*s", maxChars, value == nullptr ? "" : value);

        gfx->fillRect(0, y - 1, 240, 11, C_BG);
        gfx->setTextSize(1);
        gfx->setTextColor(C_SUB);
        gfx->setCursor(10, y);
        gfx->print(label);
        gfx->setTextColor(C_WHITE);
        gfx->setCursor(VALUE_X, y);
        gfx->print(fitted);
    }

    auto drawDate() -> void {
        char dateText[16];
        const time_t now = time(nullptr) + 8 * 3600;
        m_lastDay = static_cast<int>(now / 86400);
        struct tm localTime;
        gmtime_r(&now, &localTime);
        // 完整日期与星期共 15 个 ASCII 字符；小字号控制在 90px，
        // 仍保留 Arduino_GFX 的像素点阵观感。
        strftime(dateText, sizeof(dateText), "%Y-%m-%d  %a", &localTime);
        auto* gfx = DisplayManager::getGfx();
        // Clear the full date band before redrawing so a longer previous value
        // cannot leave glyph remnants. This is a fixed-area update, not a
        // full-screen redraw, and the clock band starts at y=174.
        // 下移日期并收紧独占带：日期字形 166~173，时钟字形从 181 开始，视觉间隔约 7px。
        // 仍不触及信息行末行和时钟从 y=174 开始的清屏边界。
        gfx->fillRect(0, 150, 240, 24, C_BG);
        gfx->setTextSize(1);
        gfx->setTextColor(C_ACCENT);
        gfx->setCursor(120 - textWidthPx(dateText, 1) / 2, 166);
        gfx->print(dateText);
    }

    auto drawClock(bool fullRedraw) -> void {
        const time_t now = time(nullptr) + 8 * 3600;
        struct tm localTime;
        gmtime_r(&now, &localTime);
        const int minute = static_cast<int>((now / 60) % 1440);
        m_lastMinute = minute;

        char clockText[6];
        strftime(clockText, sizeof(clockText), "%H:%M", &localTime);
        auto* gfx = DisplayManager::getGfx();
        if (fullRedraw) {
            // 起点 174：日期行占 164~171，清屏不得覆盖（否则日期缺一像素行）
            gfx->fillRect(0, 174, 240, 34, C_BG);
        } else {
            gfx->fillRect(0, CLOCK_Y - 2, 240, 28, C_BG);
        }
        gfx->setTextSize(3);
        gfx->setTextColor(C_WHITE);
        gfx->setCursor(120 - textWidthPx(clockText, 3) / 2, CLOCK_Y);
        gfx->print(clockText);
    }
};

// 1/255 定标的正弦表（0..90°，四分之一波）。画圆弧要取圆周坐标，为了不把 libm 的
// sinf/cosf（连带浮点库和一堆代码）带进固件，这里用 91 字节查表 + 象限映射，误差
// ≤ 0.5/255 ≈ 0.04px，240×240 屏上完全看不出来。91 字节常驻静态区（.rodata），
// 不参与堆分配。
static const uint8_t kSinUnitDeg[91] = {
      0,   4,   9,  13,  18,  22,  27,  31,  35,  40,  44,  49,  53,  57,  62,
     66,  70,  75,  79,  83,  87,  91,  96, 100, 104, 108, 112, 116, 120, 124,
    127, 131, 135, 139, 143, 146, 150, 153, 157, 160, 164, 167, 171, 174, 177,
    180, 183, 186, 190, 192, 195, 198, 201, 204, 206, 209, 211, 214, 216, 219,
    221, 223, 225, 227, 229, 231, 233, 235, 236, 238, 240, 241, 243, 244, 245,
    246, 247, 248, 249, 250, 251, 252, 253, 253, 254, 254, 254, 255, 255, 255,
    255,
};

/// sin(角度) 的 1/255 定标值（-255..255）。tenths = 角度×10，即 0.1° 分辨率，
/// 在 kSinUnitDeg 上线性插值。取 0.1° 而不是整度，是因为额度环的扇形边界正好是
/// 3.6·percent 度（如 70% → 252.0°、17% → 61.2°），整度会把边界量化掉 0.5°，
/// 在 r=20 处就是 0.17px 的错位；0.1° 只剩 0.02px。
static auto sinUnitTenths(uint16_t tenths) -> int16_t {
    tenths %= 3600U;
    uint16_t folded = 0U;
    bool negative = false;
    if (tenths < 900U) {
        folded = tenths;
    } else if (tenths < 1800U) {
        folded = 1800U - tenths;
    } else if (tenths < 2700U) {
        folded = tenths - 1800U;
        negative = true;
    } else {
        folded = 3600U - tenths;
        negative = true;
    }
    const uint16_t index = folded / 10U;   // 0..90
    const int16_t remainder = static_cast<int16_t>(folded % 10U);
    int16_t magnitude = 255;
    if (index < 90U) {
        const int16_t low = kSinUnitDeg[index];
        const int16_t high = kSinUnitDeg[index + 1U];
        magnitude = static_cast<int16_t>(low + (((high - low) * remainder + 5) / 10));
    }
    return negative ? static_cast<int16_t>(-magnitude) : magnitude;
}

/// cos(角度) = sin(角度 + 90°)，复用同一张表。
static auto cosUnitTenths(uint16_t tenths) -> int16_t {
    return sinUnitTenths(static_cast<uint16_t>(tenths + 900U));
}

/// 32 位整数平方根（逐位恢复法）。算像素到圆心的距离要用到它 —— 引入 sqrtf() 连带
/// libm 与浮点库，正是本文件上面那张正弦表要绕开的东西，所以这里手写整数版。
static auto isqrt32(uint32_t value) -> uint16_t {
    uint32_t remainder = value;
    uint32_t root = 0U;
    uint32_t bit = 1UL << 30;
    while (bit > remainder) {
        bit >>= 2;
    }
    for (; bit != 0U; bit >>= 2) {
        if (remainder >= root + bit) {
            remainder -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    return static_cast<uint16_t>(root);
}

/// 两个**已交换色序**的 RGB565 之间按 ratio(0..255) 线性插值。
/// BGR 交换是固定通道置换，与逐通道 lerp 可交换，所以直接在本文件 rgb565() 打包
/// 出来的值上插即可，不必回到源 RGB 再转换。三段通道位宽不同（5/6/5），
/// 必须各自在自己的位宽里插完再拼回去，跨位宽插会偏色。
static auto lerp565(uint16_t from, uint16_t to, uint8_t ratio) -> uint16_t {
    const int32_t weight = ratio;
    const int32_t fromR = (from >> 11) & 0x1FU;
    const int32_t fromG = (from >> 5) & 0x3FU;
    const int32_t fromB = from & 0x1FU;
    const int32_t red = fromR + ((((to >> 11) & 0x1FU) - fromR) * weight + 127) / 255;
    const int32_t green = fromG + ((((to >> 5) & 0x3FU) - fromG) * weight + 127) / 255;
    const int32_t blue = fromB + (((static_cast<int32_t>(to & 0x1FU) - fromB) * weight + 127) / 255);
    return static_cast<uint16_t>((static_cast<uint32_t>(red) << 11) |
                                 (static_cast<uint32_t>(green) << 5) |
                                 static_cast<uint32_t>(blue));
}

// ---------- stock：推送的股票行情 ----------
// 固定五个槽位让 1~5 行的切换不跳动；数据变化只擦写行带，不触碰整屏。
class StockScene : public Scene {
   public:
    auto name() const -> const char* override { return "stock"; }

    auto enter(const char* param) -> bool override {
        (void)param;
        m_count = StockData::rowCount();
        for (uint8_t i = 0; i < StockData::MAX_ROWS; ++i) {
            if (i < m_count && StockData::getRow(i, &m_rows[i])) {
                m_saved[i] = m_rows[i];
            } else {
                m_rows[i].name[0] = '\0';
                m_saved[i].change = 0.0F;
            }
        }
        m_lastUpdated = StockData::updatedAtMs();
        m_count = m_count > StockData::MAX_ROWS ? StockData::MAX_ROWS : m_count;
        captureQuota();

        DisplayManager::clearScreen();
        drawQuota();
        drawTitle();
        drawRows();
        drawClock(true, true);
        return true;
    }

    auto update() -> void override {
        // 时间戳变了才读取定长缓冲；相同 payload 不重绘，避免无意义的 SPI 传输。
        const uint32_t updated = StockData::updatedAtMs();
        if (updated != m_lastUpdated) {
            m_lastUpdated = updated;
            const uint8_t count = StockData::rowCount();
            StockData::Row candidate[StockData::MAX_ROWS]{};
            bool changed = count != m_count;
            for (uint8_t i = 0; i < count && i < StockData::MAX_ROWS; ++i) {
                if (!StockData::getRow(i, &candidate[i])) {
                    changed = true;
                    continue;
                }
                if (i >= m_count || strcmp(candidate[i].name, m_saved[i].name) != 0 ||
                    candidate[i].change != m_saved[i].change) {
                    changed = true;
                }
            }
            if (changed) {
                m_count = count > StockData::MAX_ROWS ? StockData::MAX_ROWS : count;
                for (uint8_t i = 0; i < StockData::MAX_ROWS; ++i) {
                    m_saved[i] = i < m_count ? candidate[i] : StockData::Row{};
                    // 必须整行赋值：早先只 strlcpy 了 name，change 一直停在 enter()
                    // 时的旧值，于是推送后涨跌幅不变（真机发现「推送无变化」）。
                    m_rows[i] = m_saved[i];
                }
                drawRows();
            }

            // 额度环与行数据同属一次股票推送，但差量判定必须彼此独立：
            // 只动额度时行带保持零绘制，只动行时也不必重画圆环。
            int8_t quotaNow[StockData::QUOTA_MAX];
            const bool hasNow = StockData::hasQuota();
            bool quotaChanged = hasNow != m_hasQuota;
            for (uint8_t i = 0; i < StockData::QUOTA_MAX; ++i) {
                quotaNow[i] = StockData::quota(i);
                if (quotaNow[i] != m_savedQuota[i]) {
                    quotaChanged = true;
                }
            }
            if (quotaChanged) {
                m_hasQuota = hasNow;
                for (uint8_t i = 0; i < StockData::QUOTA_MAX; ++i) {
                    m_savedQuota[i] = quotaNow[i];
                }
                drawQuota();
            }
        }

        const time_t now = time(nullptr) + 8 * 3600;
        const int minute = static_cast<int>((now / 60) % 1440);
        const int second = static_cast<int>(now % 60);
        if (second != m_lastSecond) {
            // 平时只擦写 SS 两个字符（36px）；分钟跳变时补写 HH:MM。
            drawClock(minute != m_lastMinute, false);
        }
    }

    auto exit() -> void override {}

   private:
    static constexpr uint16_t C_BG = rgb565(0x00, 0x00, 0x00);
    static constexpr uint16_t C_SUB = rgb565(0x8A, 0x94, 0xB8);
    static constexpr uint16_t C_WHITE = rgb565(0xFF, 0xFF, 0xFF);
    static constexpr uint16_t C_UP = rgb565(0xFF, 0x00, 0x00);
    static constexpr uint16_t C_DOWN = rgb565(0x00, 0xFF, 0x00);
    /// 平盘 0.00%：中性白，比灰蓝醒目，也不与涨跌色混淆
    static constexpr uint16_t C_NEUTRAL = rgb565(0xFF, 0xFF, 0xFF);
    // 额度环：颜色**按行固定**，不随百分比变 —— 三个环各占一个身份色，数值完全
    // 由弧长表达，色相只负责分辨「哪一个环是 5H / WK. / MO.」。所以这里不能套
    // 额度页那套「越多越绿/越少越红」的严重度分档（会与 stock 的涨跌红绿打架，
    // 也会被误读成告警），而是取三个**平级**的身份色。
    //
    // 上一版三色（#FFC53D / #FF7A2F / #B85C2E）在小屏上糊成一团，根因是两处：
    //   ① 5H→WK 明度只差 2.7 点（62.0 vs 59.2），色相差 20° 却被同族暖色吃掉；
    //   ② WK→MO 色相只差 1.7°（21.6° vs 20.0°），几乎同色，只靠明度硬撑，
    //      而 2px 细弧 + 20% 背光又把这点明度差压掉了 —— 于是中圈与内圈糊成一片。
    // 现改为**色相与明度同向单调递减**的三级阶梯（奶金 → 琥珀 → 陶红）：
    //   5H #FFF2B4  H=51.7°  L=84.7   最亮的浅奶金
    //   WK #FCA654  H=29.3°  L=65.9   中间调琥珀
    //   MO #D4523C  H= 8.7°  L=53.3   最沉的陶红
    // 三个色相角各差 20° 以上（52→29→9），明度各降 12 点以上，20% 背光下相邻
    // 两环的实显亮度比仍有 1.80 / 2.37，肉眼一眼可分。
    // 引入浅奶金作为第四种「色相」是为了把外环抬到足够亮 —— 这是暖调内部的
    // 明度扩展，不引入任何冷色，整体仍是橙红黄棕。
    // MO 比上一版更亮（L 45.5→53.3、对轨道对比 1.58→1.64），不会退回「看着
    // 像空了半圈」的老问题；且刻意避开纯红（对涨红 #FF0000 的 ΔE=41，明显
    // 偏棕的陶土色而非正红），也不含绿，不会与行情涨跌红绿混淆。
    // 源值仍是真 RGB，由本文件的 rgb565() 做 BGR 交换（与全项目同一口径）。
    static constexpr uint16_t C_TRACK = rgb565(0x40, 0x28, 0x1A);  // 轨道：深棕（同额度页 C_BORDER）
    static constexpr uint16_t C_BAR_5H = rgb565(0xFF, 0xF2, 0xB4);  // 浅奶金（最亮）
    static constexpr uint16_t C_BAR_WK = rgb565(0xFC, 0xA6, 0x54);  // 琥珀（居中）
    static constexpr uint16_t C_BAR_MO = rgb565(0xD4, 0x52, 0x3C);  // 陶红（最沉但仍清晰）
    /// 行序 0/1/2 = 5H / WK. / MO.，与额度页三行同序
    static constexpr uint16_t C_BAR_ROW[StockData::QUOTA_MAX] = {
        C_BAR_5H, C_BAR_WK, C_BAR_MO};
    // 额度指示 = **右下角**三重同心圆环，整体外径 40px，擦底方框 41×41。
    // 贴右下角各留 5px 余量：方框占 x=193..233 / y=194..234（屏幕 240×240），
    // 圆心 (213,214)，最外半径 20，故实际像素恰在框内不越界。
    // 环区与其它元素在两个方向上都错开，互不覆盖：
    //   · 行情行擦除带止于 y=180（环从 y=194 起，纵向 14px 空档）
    //   · 时钟擦除带只到 x=149（CLOCK_X−2 .. CLOCK_X+CLOCK_W+1），
    //     环从 x=193 起，横向 44px 空档
    // 所以圆环只擦自己那 41×41 的方框，而时钟的整条擦除也绝不越过 x=149。
    static constexpr int16_t RING_BOX = 41;   // 擦底方框边长（含外缘各 1px 余量）
    static constexpr int16_t RING_X = 193;
    static constexpr int16_t RING_Y = 194;
    static constexpr int16_t RING_CX = RING_X + 20;
    static constexpr int16_t RING_CY = RING_Y + 20;
    static constexpr int16_t RING_R[StockData::QUOTA_MAX] = {20, 14, 8};
    static constexpr uint8_t RING_THICK = 2;   // 每环 2px 描边（向内取）
    static constexpr int16_t ROW_Y = 50;
    static constexpr uint8_t ROW_STEP = 27;
    // 时钟**左对齐**（不再水平居中）：字号 3 每字 18px，8 字符共 144px，
    // 占 x=4..147，右侧空出来的 148..192 恰好让给右下角圆环。
    // 擦除带上沿 CLOCK_Y−2=202，与行情行带（止于 180）也不相接。
    static constexpr int16_t CLOCK_Y = 204;
    static constexpr int16_t CLOCK_X = 4;
    static constexpr int16_t CLOCK_W = 144;   // == textWidthPx("00:00:00", 3)
    static constexpr int16_t NAME_X = 4;
    static constexpr int16_t VALUE_RIGHT = 236;
    static constexpr int8_t NAME_GAP = 4;

    StockData::Row m_rows[StockData::MAX_ROWS]{};
    StockData::Row m_saved[StockData::MAX_ROWS]{};
    // 屏上额度快照：只用于判断「是否需要重画条带」，值本身直接取自 StockData
    int8_t m_savedQuota[StockData::QUOTA_MAX] = {-1, -1, -1};
    bool m_hasQuota = false;
    uint8_t m_count = 0;
    uint32_t m_lastUpdated = 0;
    int m_lastMinute = -1;
    int m_lastSecond = -1;

    static auto formatChange(float change, char* out, size_t size) -> void {
        (void)size;
        // 先转百分位整数，避免把浮点 printf 及其格式化库带进 ESP8266 固件。
        int32_t cents = static_cast<int32_t>(change * 100.0F + (change >= 0.0F ? 0.5F : -0.5F));
        // 平盘不写正负号：0.00% 若带 '+' 会被读成「涨」。
        size_t pos = 0;
        if (cents < 0) {
            out[pos++] = '-';
            cents = -cents;
        } else if (cents > 0) {
            out[pos++] = '+';
        }
        uint32_t whole = static_cast<uint32_t>(cents) / 100U;
        const uint8_t fraction = static_cast<uint8_t>(static_cast<uint32_t>(cents) % 100U);
        // 上界留足：接口允许 |change| ≤ 100000，输出形如 "+100000.00%" 共 12 字符 + NUL。
        char digits[10];
        uint8_t digitCount = 0;
        do {
            digits[digitCount++] = static_cast<char>('0' + whole % 10U);
            whole /= 10U;
        } while (whole != 0U);
        while (digitCount != 0U) {
            out[pos++] = digits[--digitCount];
        }
        out[pos++] = '.';
        out[pos++] = static_cast<char>('0' + fraction / 10U);
        out[pos++] = static_cast<char>('0' + fraction % 10U);
        out[pos++] = '%';
        out[pos] = '\0';
    }

    auto captureQuota() -> void {
        m_hasQuota = StockData::hasQuota();
        for (uint8_t i = 0; i < StockData::QUOTA_MAX; ++i) {
            m_savedQuota[i] = StockData::quota(i);
        }
    }

    /// 右下角三重同心额度环：外/中/内 = 5H / WK. / MO.，无任何文字。
    /// 弧长 = 百分比（唯一的数据通道，50% 半圈、100% 整圈），环色 = 行身份，不随数值变。
    /// 从 12 点起顺时针扫：x = cx + r·sin(deg)，y = cy − r·cos(deg)。
    /// 轨道恒画满整圈（无值 -1 / 0% 只留轨道，没有任何数据时也是三圈空轨道）。
    /// 环区自清底（fillRect 只覆盖 RING_X/RING_Y 起的 41×41，落在 x≤233 / y≤234），
    /// 因此它本身就是自洽的局部更新单元（约屏面 0.7%），既不触碰左上标题、
    /// 中部行情行带，也碰不到左侧时钟（时钟擦除带止于 x=149）。
    ///
    /// 画法：**逐像素距离场 + 径向覆盖率抗锯齿**。旧画法是按 1° 步长沿角度逐点
    /// 落笔，根因有三，都会在 240px 小屏上显成「像素感重、弧不圆滑」：
    ///   ① 外环 1° 只走 0.35px 弧长，相邻角度**大量落在同一像素**（实测 2160 次
    ///      drawPixel 里 56% 是重复覆盖同一像素），偶尔又跳 2px —— 弧长忽疏忽密。
    ///   ② 坐标用 `>> 16`，对负数是算术右移（向 −∞ 取整）而非就近取整，圆心左右
    ///      两侧取整规律不一致，一侧顺一侧毛。
    ///   ③ 2px 描边按整数半径硬切，无法表达亚像素部分覆盖，内外缘必然是硬的。
    /// 现在反过来遍历包围盒里每个像素：整数开方算它到圆心的距离，落在环带内的按
    /// 覆盖率把颜色混出来 —— 径向完全对称（无累积取整误差）、每像素只画一次、
    /// 内外缘是真抗锯齿。与 16×16 超采样理想图（同样用 565 量化色）比对，平均
    /// 通道误差 44.7 → 4.9（三环同 70%）/ 44.6 → 4.7（100/50/0%）/ 26.3 → 3.0
    /// （3% 最小弧），三档都降 89%。
    /// 相邻两环不会互相污染：环带之间本就有 4px 空白，抗锯齿最多向外洇 0.5px，
    /// 实测同时被两条环带覆盖的像素为 0，环 0↔环 1 的着墨像素最小中心距 3.61px。
    ///
    /// 扇形边界（弧的端头）用**叉积**判，不比较角度也不比较余弦：
    ///   A = cross(vΘ, P) = sinΘ·u − cosΘ·dx   —— A ≥ 0 ⇔ α ≤ Θ（在 α ≥ Θ−180 支上）
    ///   B = dx ≥ 0                            —— ⇔ α ∈ [0,180]
    /// 凸扇形 Θ≤180 取 A ≥ 0 且 B；反射扇形 Θ>180 取 [0,Θ] = 整圈减 (Θ,360)，
    /// 后者正是 A ≤ 0 且 !B，故取补得 A > 0 或 B。判据对 P 线性，误差只有坐标的
    /// 1/255，角分辨率约 0.02°。这里刻意不用「cos α ≥ cos Θ/2」那种半角写法：
    /// cos 在 0 附近极平坦，1% 档（Θ=3.6°）的余弦差不足半个 LSB，会整圈判反。
    auto drawQuota() -> void {
        auto* gfx = DisplayManager::getGfx();
        // 只擦环区（x=193..233 / y=194..234），不碰标题/行带/时钟。
        gfx->fillRect(RING_X, RING_Y, RING_BOX, RING_BOX, C_BG);

        // 每个环的扇形边界方向预先算好（1/255 定标），像素循环里只做点积。
        // 形状 0 = 无弧（无值 -1 与 0% 都只剩轨道；完全没有额度数据时三段必全是
        // -1 —— setQuota 只在至少一段 >=0 时才置位 hasQuota，clear() 也复位，
        // 所以这里不提前返回，三圈轨道照画）、1 = 整圈、2 = 凸扇形、3 = 反射扇形。
        uint8_t shape[StockData::QUOTA_MAX] = {};
        int16_t sinT[StockData::QUOTA_MAX] = {};
        int16_t cosT[StockData::QUOTA_MAX] = {};
        for (uint8_t i = 0; i < StockData::QUOTA_MAX; ++i) {
            const int16_t percent = m_savedQuota[i];
            if (percent <= 0) {
                shape[i] = 0U;
            } else if (percent >= 100) {
                shape[i] = 1U;   // 整圈：上面两个判据在 Θ=360° 都退化，单独短路
            } else {
                // Θ = 3.6·percent 度 = percent·36 个 0.1°；交叉相乘比不出浮点。
                const uint16_t tenths = static_cast<uint16_t>(percent) * 36U;
                sinT[i] = sinUnitTenths(tenths);
                cosT[i] = cosUnitTenths(tenths);
                shape[i] = percent <= 50 ? 2U : 3U;
            }
        }

        // 环带的内外边界，1/16 px 定标。取 (R−THICK+0.5) 与 (R+0.5) 而不是整数
        // R−THICK / R，是为了让连续极限正好等于旧画法「取 r 与 r−1 两个整数半径」
        // 的实际着墨面积 —— 换句话说抗锯齿只改边缘质感，不改环的视觉粗细。
        int16_t inner16[StockData::QUOTA_MAX];
        int16_t outer16[StockData::QUOTA_MAX];
        for (uint8_t i = 0; i < StockData::QUOTA_MAX; ++i) {
            inner16[i] = (RING_R[i] - RING_THICK) * 16 + 8;
            outer16[i] = RING_R[i] * 16 + 8;
        }

        // 41×41 = 1681 次迭代，其中约 710 次真正落笔 —— 比旧画法 2160 次
        // drawPixel（且 56% 互相覆盖）还少，SPI 传输量不增反降。
        for (int16_t y = RING_Y; y < RING_Y + RING_BOX; ++y) {
            for (int16_t x = RING_X; x < RING_X + RING_BOX; ++x) {
                const int16_t dx = x - RING_CX;    // 右正
                const int16_t up = RING_CY - y;    // 上正
                // 到圆心的距离，1/16 px 定标：isqrt(n·2^8) 恰是 16·√n。
                const int16_t r16 = static_cast<int16_t>(
                    isqrt32(static_cast<uint32_t>(dx * dx + up * up) << 8));
                // 本像素在径向覆盖 [r−0.5, r+0.5]
                const int16_t low = r16 - 8;
                const int16_t high = r16 + 8;
                for (uint8_t i = 0; i < StockData::QUOTA_MAX; ++i) {
                    const int16_t from = low > inner16[i] ? low : inner16[i];
                    const int16_t to = high < outer16[i] ? high : outer16[i];
                    if (to <= from) {
                        continue;
                    }
                    // 覆盖率 0..255（像素自身径向宽 1px = 16 个 1/16 单位）
                    const uint8_t coverage =
                        static_cast<uint8_t>((to - from) * 255 / 16);
                    bool onArc = false;
                    if (shape[i] == 1U) {
                        onArc = true;
                    } else if (shape[i] != 0U) {
                        const int32_t cross =
                            static_cast<int32_t>(sinT[i]) * up -
                            static_cast<int32_t>(cosT[i]) * dx;
                        onArc = shape[i] == 2U ? (cross >= 0 && dx >= 0)
                                               : (cross > 0 || dx >= 0);
                    }
                    const uint16_t base = onArc ? C_BAR_ROW[i] : C_TRACK;
                    // 环带**内外两侧都是背景（纯黑）**，轨道只存在于环带之内 ——
                    // 所以径向抗锯齿要往 C_BG 混。往 C_TRACK 混会给每个环外缘糊上
                    // 一层深棕光晕，既虚胖又把环色洗淡。
                    gfx->drawPixel(x, y, coverage == 255U
                                                 ? base
                                                 : lerp565(C_BG, base, coverage));
                    break;   // 三条环带彼此隔 4px 空白，一个像素只可能命中一条
                }
            }
        }
    }

    auto drawTitle() -> void {
        auto* gfx = DisplayManager::getGfx();
        // y=16 起、水平居中（x≈90..150）；额度环已移到右下角 y≥194，
        // 顶部这一带只剩标题，与环区毫无交集。
        gfx->setTextSize(2);
        gfx->setTextColor(C_WHITE);
        gfx->setCursor(120 - textWidthPx("STOCK", 2) / 2, 16);
        gfx->print("STOCK");
    }

    auto drawRows() -> void {
        auto* gfx = DisplayManager::getGfx();
        // 字号 2 的字高为 16px；27px 行距给每行保留 11px 的呼吸空间。
        // 擦除带在 47~180，左下角时钟擦除带从 y=202 开始（且只占 x=2..149），
        // 中间保留 22px 纵向间隔；头部 16~31 的标题与右下角额度环
        // （y≥194）也都在这块之外，互不覆盖。
        gfx->fillRect(0, ROW_Y - 3, 240, 5 * ROW_STEP - 2, C_BG);
        if (m_count == 0) {
            gfx->setTextSize(2);
            gfx->setTextColor(C_SUB);
            gfx->setCursor(120 - textWidthPx("NO DATA", 2) / 2, 90);
            gfx->print("NO DATA");
            gfx->setTextSize(1);
            gfx->setTextColor(C_WHITE);
            gfx->setCursor(120 - textWidthPx("PUSH VIA API", 1) / 2, 116);
            gfx->print("PUSH VIA API");
            return;
        }

        char value[16];
        for (uint8_t i = 0; i < m_count; ++i) {
            const int16_t y = ROW_Y + i * ROW_STEP;
            gfx->setTextSize(2);

            formatChange(m_rows[i].change, value, sizeof(value));
            const int valueWidth = textWidthPx(value, 2);
            const int valueX = VALUE_RIGHT - valueWidth;
            const size_t nameLength = strlen(m_rows[i].name);
            // 极端的 9 字符名称 + "+100000.00%" 原本需要 252px，必须让名称
            // 退让而不是让两段文字重叠；用 ASCII ~ 标记被截断的末尾。
            const int maxNameChars = (valueX - NAME_GAP - NAME_X) / 12;
            char nameText[StockData::NAME_MAX];
            int shownNameChars = static_cast<int>(nameLength);
            if (shownNameChars > maxNameChars) {
                shownNameChars = maxNameChars > 0 ? maxNameChars - 1 : 0;
                for (int n = 0; n < shownNameChars; ++n) {
                    nameText[n] = m_rows[i].name[n];
                }
                if (shownNameChars > 0) {
                    nameText[shownNameChars++] = '~';
                }
                nameText[shownNameChars] = '\0';
            } else {
                strlcpy(nameText, m_rows[i].name, sizeof(nameText));
            }

            gfx->setTextColor(C_WHITE);
            gfx->setCursor(NAME_X, y);
            gfx->print(nameText);
            gfx->setTextColor(m_rows[i].change > 0.0F ? C_UP :
                               (m_rows[i].change < 0.0F ? C_DOWN : C_NEUTRAL));
            gfx->setCursor(valueX, y);
            gfx->print(value);
        }
    }

    auto drawClock(bool minuteChanged, bool fullRedraw) -> void {
        const time_t now = time(nullptr) + 8 * 3600;
        struct tm localTime;
        gmtime_r(&now, &localTime);
        m_lastMinute = static_cast<int>((now / 60) % 1440);
        m_lastSecond = static_cast<int>(now % 60);

        auto* gfx = DisplayManager::getGfx();
        gfx->setTextSize(3);
        gfx->setTextColor(C_WHITE);
        // 左对齐到 CLOCK_X，整条时钟恒为 CLOCK_W 宽（字形宽度固定，数字位数不变）。
        const int16_t clockX = CLOCK_X;
        const int16_t minuteWidth = textWidthPx("00:00:", 3);
        const int16_t secondX = clockX + minuteWidth;
        const int16_t secondWidth = textWidthPx("00", 3);
        if (fullRedraw) {
            // 关键：**只擦时钟自己那一条**（x=2..149），绝不能像原先那样
            // fillRect(0, …, 240, 28) 横扫整行 —— 圆环已移到右下角
            // x=193..233 / y=194..234，整行擦除会把三层同心环抹掉。
            // 左右各留 2px 余量，保证字号下沿与首尾字形都不残留。
            gfx->fillRect(CLOCK_X - 2, CLOCK_Y - 2, CLOCK_W + 4, 28, C_BG);
        }
        if (fullRedraw || minuteChanged) {
            // 分钟跳变时整条擦除不会执行，必须先擦掉本区域再画，否则新旧分钟
            // 数字直接叠在一起（14:37 → 14:38 会看到 7 与 8 重影）。
            if (!fullRedraw) {
                gfx->fillRect(clockX, CLOCK_Y - 2, minuteWidth, 28, C_BG);
            }
            gfx->setCursor(clockX, CLOCK_Y);
            char minuteText[8];
            strftime(minuteText, sizeof(minuteText), "%H:%M:", &localTime);
            gfx->print(minuteText);
        }
        // 仅擦写秒字段，避免每秒把整条时钟或屏幕重新推送到 SPI。
        // 高度与全量路径一致，避免字号下沿残留上一秒字形。
        gfx->fillRect(secondX, CLOCK_Y - 2, secondWidth, 28, C_BG);
        char secondText[3];
        strftime(secondText, sizeof(secondText), "%S", &localTime);
        gfx->setCursor(secondX, CLOCK_Y);
        gfx->print(secondText);
    }
};

// ---------- balance：额度 + 时钟主页面 ----------
class BalanceScene : public Scene {
   public:
    auto name() const -> const char* override { return "balance"; }

    auto enter(const char* param) -> bool override {
        (void)param;
        UsageManager::enterScene();

        return true;
    }

    auto update() -> void override { UsageManager::update(); }

    auto exit() -> void override { UsageManager::exitScene(); }
};

// ---------- album：静态相册 ----------
// 图片格式：/album/<name>.rgb565 = 240x240 RGB565(LE) 原始位图（115200B），
// 由 Web 端 canvas 转换后上传。固件零解码器：流式读文件直接送屏，
// RAM 仅用 ~4KB 分带行缓冲。
// 双模式：param=单张固定显示（单图 Play）；无 param=轮播现存全部（Play all /
// 外部场景按钮），播完自动从头开始。场景退出一律靠手动 switchTo
class AlbumScene : public Scene {
   public:
    auto name() const -> const char* override { return "album"; }

    auto enter(const char* param) -> bool override {
        m_count = 0;
        m_index = 0;

        if (param != nullptr && param[0] != '\0') {
            // 单张固定：只载入该图（update 里 m_count<=1 不轮播）
            String name = String(param);
            name.replace("\\", "/");
            name = name.substring(name.lastIndexOf('/') + 1);

            if (!LittleFS.exists(String("/album/") + name)) {
                Logger::warn("AlbumScene: image not found", "Scene");
                return false;
            }

            strlcpy(m_items[m_count++], name.c_str(), sizeof(m_items[0]));
        } else {
            // 轮播：收集现存全部图片
            collectDir();

            if (m_count == 0) {
                // 空相册是有效的停留状态；上传后重新切入场景即可再次收集图片。
                Logger::warn("AlbumScene: album is empty", "Scene");
                drawEmptyState();
                m_lastMs = millis();
                return true;
            }
        }

        drawImage(m_items[m_index]);
        m_lastMs = millis();
        Logger::info((String("AlbumScene: showing ") + String(m_items[m_index])).c_str(), "Scene");

        return true;
    }

    auto update() -> void override {
        // 纯轮播：按停留时长换下一张，最后一张放完自动回到第一张（不自动切场景）
        if (m_count <= 1) {
            return;
        }

        if (millis() - m_lastMs < SHOW_MS_PER_IMAGE) {
            return;
        }
        m_lastMs = millis();

        m_index = (m_index + 1) % m_count;
        drawImage(m_items[m_index]);
        Logger::info((String("AlbumScene: showing ") + String(m_items[m_index])).c_str(), "Scene");
    }

    auto exit() -> void override {
        m_count = 0;
        m_index = 0;
    }

   private:
    static constexpr int ALBUM_MAX = 16;
    static constexpr int IMG_W = 240;
    static constexpr int IMG_H = 240;
    static constexpr int BAND_ROWS = 8;  // 每带 8 行 = 3840B 缓冲
    static constexpr uint32_t SHOW_MS_PER_IMAGE = 5000U;
    static constexpr size_t IMG_BYTES = static_cast<size_t>(IMG_W) * IMG_H * 2;

    static constexpr uint16_t C_EMPTY_BG = rgb565(0x00, 0x00, 0x00);
    static constexpr uint16_t C_EMPTY_SUB = rgb565(0x8A, 0x94, 0xB8);
    static constexpr uint16_t C_EMPTY_ACCENT = rgb565(0x4D, 0x6B, 0xFE);

    char m_items[ALBUM_MAX][40] = {{0}};
    int m_count = 0;
    int m_index = 0;
    uint32_t m_lastMs = 0;

    auto drawEmptyState() -> void {
        auto* gfx = DisplayManager::getGfx();
        gfx->fillScreen(C_EMPTY_BG);

        // 文案保持 ASCII：内建字体没有 CJK 字模；两行居中，避免空态像故障画面。
        const char* title = "ALBUM EMPTY";
        const char* hint = "UPLOAD VIA WEB";
        gfx->setTextSize(2);
        gfx->setTextColor(C_EMPTY_SUB);
        gfx->setCursor(120 - textWidthPx(title, 2) / 2, 108);
        gfx->print(title);

        gfx->setTextSize(1);
        gfx->setTextColor(C_EMPTY_ACCENT);
        gfx->setCursor(120 - textWidthPx(hint, 1) / 2, 136);
        gfx->print(hint);
    }

    auto collectDir() -> void {
        Dir dir = LittleFS.openDir("/album");

        while (dir.next() && m_count < ALBUM_MAX) {
            const String name = dir.fileName();

            if (!name.endsWith(".rgb565") || dir.fileSize() != IMG_BYTES) {
                continue;
            }

            strlcpy(m_items[m_count++], name.c_str(), sizeof(m_items[0]));
        }
    }

    static auto drawImage(const char* name) -> void {
        File file = LittleFS.open(String("/album/") + name, "r");

        if (!file || file.size() != IMG_BYTES) {
            Logger::warn("AlbumScene: bad image file", "Scene");
            return;
        }

        static uint16_t band[IMG_W * BAND_ROWS];
        auto* tft = reinterpret_cast<Arduino_TFT*>(DisplayManager::getGfx());

        tft->startWrite();

        for (int y = 0; y < IMG_H; y += BAND_ROWS) {
            const int rows = (y + BAND_ROWS <= IMG_H) ? BAND_ROWS : (IMG_H - y);
            const size_t want = static_cast<size_t>(IMG_W) * 2 * rows;

            if (file.read(reinterpret_cast<uint8_t*>(band), want) != static_cast<int>(want)) {
                break;
            }

            // 本面板色序 BGR（MADCTL 带 BGR 位，见 DisplayManager::lcdApplyMirrorMADCTL）：
            // .rgb565 契约是标准 RGB565(LE)，直推会红蓝互换——与 UI 调色板同口径做 R/B 字段交换补偿
            const size_t px = static_cast<size_t>(IMG_W) * rows;
            for (size_t i = 0; i < px; i++) {
                const uint16_t v = band[i];
                band[i] = static_cast<uint16_t>(((v & 0x001FU) << 11) | (v & 0x07E0U) | ((v & 0xF800U) >> 11));
            }

            tft->writeAddrWindow(0, y, IMG_W, rows);
            tft->writePixels(band, static_cast<uint32_t>(IMG_W) * rows);
            yield();
        }

        tft->endWrite();
        file.close();
    }
};

// ---------- clock：纯时钟 ----------
// 七段大字 HH:MM:SS + 顶部小字日期星期（绘制复用 UsageManager 的字模/主题）
class ClockScene : public Scene {
   public:
    auto name() const -> const char* override { return "clock"; }

    auto enter(const char* param) -> bool override {
        (void)param;
        UsageManager::drawClockPage();

        return true;
    }

    auto update() -> void override { UsageManager::tickClockPage(); }

    auto exit() -> void override {}
};

static SystemInfoScene s_sysInfoScene;
static BalanceScene s_balanceScene;
static AlbumScene s_albumScene;
static StockScene s_stockScene;

// ---------- live：实时推图 ----------
// POST /api/v1/album/live 流式推送 240x240 RGB565 帧，边收边绘、不落盘；
// 帧内容即整屏（enter 只做清底），推送即实时更新。全手工退出：
// 常驻显示最后一帧，直到用户手动切换场景；再次推送自动回到本场景
class LiveScene : public Scene {
   public:
    auto name() const -> const char* override { return "live"; }

    auto enter(const char* param) -> bool override {
        (void)param;
        // 清底等首帧（推图流会立即覆盖整屏 = 全量重绘）
        DisplayManager::clearScreen();

        return true;
    }

    auto update() -> void override {}

    auto exit() -> void override {}
};

static LiveScene s_liveScene;
static ClockScene s_clockScene;

// ---------- notice：临时通知覆盖层 ----------
// 打断式告警页：首行一枚大图标（info 圆 / warning 三角 / critical 方 —— 三个
// 形状彼此可区分，不依赖颜色也能分辨），其余竖直空间全部留给 ASCII 正文。
// 与常驻页面（余额 / 股票 / 相册 / 时钟）在结构上就不同：满屏黑底、无 logo、
// 无七段时钟，唯一的常驻元素是右下角倒计时数字 + 底部细进度线（用户据此知道
// 它会自己消失）。每秒只重画那两块（~1.3k 像素），绝不整屏重绘；无任何闪烁 /
// 呼吸动画（闪烁刚踩过坑，且对光敏用户不友好）。
class NoticeOverlayScene : public Scene {
   public:
    auto name() const -> const char* override { return "notice"; }

    auto enter(const char* param) -> bool override {
        (void)param;
        // switchTo() 在 enter() 返回 true 之后才把 s_current 指向下一场景，
        // 所以此刻 SceneManager::currentName() 拿到的仍是即将退场的那个场景
        // —— 正是记录返回目标的机会（Api.cpp 的 handleLivePush 同理）。
        const bool fresh = s_hasContent;
        if (fresh) {
            s_hasContent = false;
            s_startedMs = millis();
        }

        if (!s_active) {
            recordReturnTarget();
            s_active = true;
            s_startedMs = millis();
            if (!fresh) {
                // 没有预载内容却被直接点名（POST /scene {"scene":"notice"}）：
                // 不编造正文，只显示图标 + 空正文，走默认时长。
                s_level = NoticeScene::LevelInfo;
                s_imageMode = false;
                s_text[0] = '\0';
                s_totalSeconds = NoticeScene::kDefaultSeconds;
            }
        }

        // 重入（休眠唤醒的 redrawCurrent / 同场景 switchTo）且计时已耗尽又没有
        // 新内容：一帧都不重画，避免唤醒瞬间闪一帧过期通知。返回动作只挂起，
        // 交给下一次 update() 兑现 —— 在 enter() 里直接 switchTo 会在外层
        // switchTo 尚未提交 s_current 时把它改掉。
        if (!fresh && expired()) {
            s_pendingReturn = true;
            return true;
        }

        s_lastSecond = kNoSecond;
        if (s_imageMode) {
            // 整屏即画面：推帧流随后覆盖整屏（与 live 一致），enter 只清底等首帧。
            // 此形态不叠任何装饰 —— 推入的 115200B 位图会盖掉一切叠加物。
            DisplayManager::clearScreen();
        } else {
            drawNotice();
        }

        return true;
    }

    auto update() -> void override {
        if (!s_active) {
            return;
        }

        if (s_hasContent) {
            // notice 已在屏上时又来一条：换内容 + 重置计时，返回目标保持不变
            // （不变是因为 !s_active 分支被跳过，返回目标根本没被重记）。
            s_hasContent = false;
            s_startedMs = millis();
            s_lastSecond = kNoSecond;
            if (s_imageMode) {
                DisplayManager::clearScreen();
            } else {
                drawNotice();
            }
            return;
        }

        if (s_pendingReturn || expired()) {
            triggerReturn();
            return;
        }

        if (s_imageMode) {
            return;  // 图像形态没有可每秒更新的装饰区
        }

        const uint16_t remaining = remainingCeil();
        if (remaining != s_lastSecond) {
            s_lastSecond = remaining;
            drawCountdown();
        }
    }

    auto exit() -> void override {
        // 刻意 no-op：不得清 s_active / 返回目标 —— 同场景重入
        // （redrawCurrent）与「通知中再来一条」都依赖它们保持不变。
        // 返回动作由 triggerReturn() 在 switchTo 之前自行清活跃标志。
    }

   private:
    // ---- 静态内容/状态（全部定长，零堆分配）----
    static constexpr size_t kReturnCap = 64;  // 与 SCENE_PARAM_MAX 对齐
    static char s_text[NoticeScene::kTextCap];
    static char s_returnName[kReturnCap];
    static char s_returnParam[kReturnCap];
    static NoticeScene::Level s_level;
    static uint16_t s_totalSeconds;
    static uint32_t s_startedMs;
    static bool s_active;        // 是否正挂在屏上
    static bool s_imageMode;
    static bool s_hasContent;    // 有待消费的新预载内容
    static bool s_pendingReturn; // enter() 挂起、待 update() 兑现的返回
    static uint16_t s_lastSecond;

    // 预载/查询接口要直接读写上面这组静态状态
    friend class NoticeScene;

    // ---- 版式常量 ----
    static constexpr uint16_t kNoSecond = 0xFFFFU;
    static constexpr int16_t SCREEN = 240;
    static constexpr int16_t MARGIN = 12;
    static constexpr int16_t TEXT_W = SCREEN - 2 * MARGIN;  // 216px 正文宽度
    static constexpr int16_t ICON_SIZE = 64;
    static constexpr int16_t ICON_X = (SCREEN - ICON_SIZE) / 2;  // 88
    static constexpr int16_t ICON_Y = 10;
    static constexpr int16_t LABEL_Y = 80;   // 字号 2 → 字高 16，80~96
    static constexpr int16_t RULE_Y = 100;   // 头/身分隔细线
    static constexpr int16_t RULE_H = 2;
    static constexpr int16_t BODY_TOP = 110;
    static constexpr int16_t BODY_BOTTOM = 216;  // 不含；正文可用 106px
    static constexpr int16_t CN_Y = 220;         // 倒计时数字（字号 1，8px 高）
    static constexpr int16_t BAR_Y = 232;        // 底部进度线
    static constexpr int16_t BAR_H = 4;
    static constexpr int16_t BAR_W = TEXT_W;
    static constexpr uint8_t kMaxLines = 12;  // 字号 1 最多 11 行，留 1 行余量

    // 正文换行后的行首表（渲染时按需拼 ~ 截断，无第二份正文缓冲）
    int16_t m_lineOff[kMaxLines] = {0};
    uint8_t m_lineLen[kMaxLines] = {0};
    uint8_t m_lineCount = 0;
    bool m_overflow = false;

    static constexpr uint16_t C_BG = rgb565(0x06, 0x08, 0x0E);     // 近黑，比常驻页的纯黑更有质感
    static constexpr uint16_t C_TEXT = rgb565(0xF2, 0xF5, 0xFF);   // 近似纯白的正文
    static constexpr uint16_t C_SUB = rgb565(0x8A, 0x94, 0xB8);    // 次要信息
    static constexpr uint16_t C_TRACK = rgb565(0x1E, 0x24, 0x36);  // 进度线底槽
    static constexpr uint16_t C_INFO = rgb565(0x4D, 0x6B, 0xFE);   // 主题蓝（沿用现有强调色）
    static constexpr uint16_t C_WARN = rgb565(0xFF, 0xB3, 0x00);   // 琥珀黄
    static constexpr uint16_t C_CRIT = rgb565(0xFF, 0x33, 0x3C);   // 警示红

    static auto levelColor(NoticeScene::Level level) -> uint16_t {
        switch (level) {
            case NoticeScene::LevelWarning:
                return C_WARN;
            case NoticeScene::LevelCritical:
                return C_CRIT;
            default:
                return C_INFO;
        }
    }

    static auto levelName(NoticeScene::Level level) -> const char* {
        switch (level) {
            case NoticeScene::LevelWarning:
                return "WARNING";
            case NoticeScene::LevelCritical:
                return "CRITICAL";
            default:
                return "INFO";
        }
    }

    static auto clampSeconds(uint16_t seconds) -> uint16_t {
        if (seconds == 0U) {
            return NoticeScene::kDefaultSeconds;
        }
        return seconds > NoticeScene::kMaxSeconds ? NoticeScene::kMaxSeconds : seconds;
    }

    // 回绕安全：millis() 每 ~49.7 天回绕一次，无符号差值依然正确
    static auto elapsedMs() -> uint32_t { return static_cast<uint32_t>(millis() - s_startedMs); }

    static auto totalMs() -> uint32_t { return static_cast<uint32_t>(s_totalSeconds) * 1000UL; }

    static auto expired() -> bool { return elapsedMs() >= totalMs(); }

    static auto remainingCeil() -> uint16_t {
        const uint32_t total = totalMs();
        const uint32_t used = elapsedMs();
        if (used >= total) {
            return 0U;
        }
        return static_cast<uint16_t>((total - used + 999UL) / 1000UL);
    }

    static auto recordReturnTarget() -> void {
        const char* current = SceneManager::currentName();
        const char* param = SceneManager::currentParam();
        // 空名字或恰好是自己（redrawCurrent 之外的异常兜底）→ 落回开机落点
        if (current == nullptr || current[0] == '\0' || strcmp(current, "notice") == 0) {
            strlcpy(s_returnName, "sysinfo", kReturnCap);
        } else {
            strlcpy(s_returnName, current, kReturnCap);
        }
        strlcpy(s_returnParam, param != nullptr ? param : "", kReturnCap);
    }

    // 返回动作：先清活跃标志（exit() 不清，所以由这里负责），再切回原场景。
    static auto triggerReturn() -> void {
        char name[kReturnCap];
        char param[kReturnCap];
        strlcpy(name, s_returnName, kReturnCap);
        strlcpy(param, s_returnParam, kReturnCap);

        s_active = false;
        s_pendingReturn = false;
        s_hasContent = false;
        s_returnName[0] = '\0';
        s_returnParam[0] = '\0';

        if (!SceneManager::switchTo(name, param)) {
            Logger::warn("NoticeScene: return target failed, fallback to sysinfo", "Scene");
            SceneManager::switchTo("sysinfo");
        }
    }

    // ---- 绘制 ----

    // 首行大图标：三种轮廓（圆 / 三角 / 方）+ 内嵌字形（i / ! / X），
    // 色弱用户也能一眼分辨。纯 Arduino_GFX 图元手绘，不引入位图，
    // 因此不存在裸解引用 flash 的风险（坑 #1）。
    auto drawIcon(Arduino_GFX* gfx, uint16_t color) -> void {
        const int16_t x = ICON_X;
        const int16_t y = ICON_Y;
        const int16_t s = ICON_SIZE;
        const char* glyph = "i";

        if (s_level == NoticeScene::LevelWarning) {
            gfx->fillTriangle(x + s / 2, y, x + s, y + s, x, y + s, color);
            glyph = "!";
        } else if (s_level == NoticeScene::LevelCritical) {
            gfx->fillRect(x + 4, y + 4, s - 8, s - 8, color);
            glyph = "X";
        } else {
            gfx->fillCircle(x + s / 2, y + s / 2, s / 2, color);
        }

        // 三角形的视觉重心低于几何中心，字形下移 4px 才显得居中
        const int16_t glyphShift = (s_level == NoticeScene::LevelWarning) ? 4 : 0;
        const int16_t glyphHeight = 8 * 3;  // 字号 3 的字高
        gfx->setTextSize(3);
        gfx->setTextColor(C_BG);  // 深色字压在饱和色块上，对比最强
        gfx->setCursor(x + s / 2 - textWidthPx(glyph, 3) / 2,
                       y + s / 2 - glyphHeight / 2 + glyphShift);
        gfx->print(glyph);
    }

    // 每秒只重画这两块：右下角 "Ns"（约 40x12）+ 底部 216x4 进度线
    auto drawCountdown() -> void {
        auto* gfx = DisplayManager::getGfx();
        const uint16_t color = levelColor(s_level);
        const uint32_t total = totalMs();
        const uint32_t used = elapsedMs();
        const uint32_t left = used >= total ? 0UL : total - used;

        char digits[8];
        snprintf(digits, sizeof(digits), "%us", static_cast<unsigned>(remainingCeil()));
        const int digitsWidth = textWidthPx(digits, 1);
        const int digitsX = SCREEN - MARGIN - digitsWidth;
        gfx->fillRect(digitsX - 2, CN_Y - 2, digitsWidth + 4, 12, C_BG);
        gfx->setTextSize(1);
        gfx->setTextColor(C_SUB);
        gfx->setCursor(digitsX, CN_Y);
        gfx->print(digits);

        gfx->fillRect(MARGIN, BAR_Y, BAR_W, BAR_H, C_TRACK);
        const int filled = total == 0UL ? 0 : static_cast<int>((left * BAR_W) / total);
        if (filled > 0) {
            gfx->fillRect(MARGIN, BAR_Y, filled, BAR_H, color);
        }
    }

    // 正文排版：从最大字号往下试，选第一个能装下全部换行后内容的字号。
    // 6px 内建字体 → 每行可容 TEXT_W / (6 * size) 个字符。
    static auto maxCharsFor(uint8_t size) -> int { return TEXT_W / (6 * size); }
    static auto lineStepFor(uint8_t size) -> int16_t { return static_cast<int16_t>(10 * size); }
    static auto glyphHeightFor(uint8_t size) -> int16_t { return static_cast<int16_t>(8 * size); }
    static auto maxLinesFor(uint8_t size) -> uint8_t {
        const int16_t bodyHeight = BODY_BOTTOM - BODY_TOP;  // 106px
        const int16_t used = bodyHeight - glyphHeightFor(size);
        return used <= 0 ? 1U : static_cast<uint8_t>(used / lineStepFor(size) + 1);
    }

    // 按词换行 + 尊重 '\n' 硬换行；超长单词（无空格可断）按字宽硬断。
    // 装不下的余量记进 m_overflow，由渲染时给最后一行补 '~'。
    auto layoutBody(uint8_t size, uint8_t maxLines) -> void {
        m_lineCount = 0;
        m_overflow = false;
        const int maxChars = maxCharsFor(size);
        const size_t length = strlen(s_text);
        size_t cursor = 0;

        while (cursor < length && m_lineCount < maxLines) {
            // 一段（到下一个 '\n' 或文本末尾）
            size_t paragraphEnd = cursor;
            while (paragraphEnd < length && s_text[paragraphEnd] != '\n') {
                paragraphEnd++;
            }

            if (paragraphEnd == cursor) {
                // 空段 = 发送方要的空行，必须保留（"\n\n" 不能被吞成 "\n"）
                pushLine(cursor, 0);
                cursor = paragraphEnd + 1;
                continue;
            }

            size_t pos = cursor;
            while (pos < paragraphEnd) {
                if (m_lineCount >= maxLines) {
                    m_overflow = true;
                    return;
                }
                const size_t avail = paragraphEnd - pos;
                if (avail <= static_cast<size_t>(maxChars)) {
                    pushLine(pos, static_cast<uint8_t>(avail));
                    pos = paragraphEnd;
                    break;
                }
                // 在 [pos+1, pos+maxChars] 里找最后一个可断点（空格之后）
                size_t cut = 0;
                for (size_t q = pos + maxChars; q > pos; q--) {
                    if (s_text[q - 1] == ' ') {
                        cut = q;
                        break;
                    }
                }
                if (cut == 0) {
                    cut = pos + maxChars;  // 无空格：按字宽硬断
                    pushLine(pos, static_cast<uint8_t>(cut - pos));
                    pos = cut;
                    continue;
                }
                const size_t segLen = cut - pos - 1;
                if (segLen == 0) {
                    pos = cut;  // 一串空格：不产出空行，直接跳过
                    continue;
                }
                pushLine(pos, static_cast<uint8_t>(segLen));
                pos = cut;
            }

            cursor = paragraphEnd + 1;  // 跳过 '\n'；文本末尾时自然越界结束
        }

        if (cursor < length) {
            m_overflow = true;  // 触及行数上限但仍有正文
        }
    }

    auto pushLine(size_t offset, uint8_t len) -> void {
        m_lineOff[m_lineCount] = static_cast<int16_t>(offset);
        m_lineLen[m_lineCount] = len;
        m_lineCount++;
    }

    auto drawBody(Arduino_GFX* gfx, uint8_t size) -> void {
        if (m_lineCount == 0) {
            return;
        }
        const int16_t step = lineStepFor(size);
        const int16_t blockHeight = (m_lineCount - 1) * step + glyphHeightFor(size);
        const int16_t bodyHeight = BODY_BOTTOM - BODY_TOP;
        const int16_t top = BODY_TOP + (bodyHeight - blockHeight) / 2;  // 竖直居中

        gfx->setTextSize(size);
        gfx->setTextColor(C_TEXT);
        for (uint8_t i = 0; i < m_lineCount; i++) {
            int len = m_lineLen[i];
            // 溢出时给最后一行补 '~'（与 stock 场景截断约定一致）。
            // 末行本身是空行时也补 '~'，否则截断标记会连同空行一起消失。
            const bool truncate = m_overflow && (i + 1 == m_lineCount);
            if (truncate && len > 0) {
                len -= 1;
            }
            char buffer[SCREEN / 6 + 1];  // 最多 maxChars 个字符 + NUL（栈上，41B）
            memcpy(buffer, s_text + m_lineOff[i], static_cast<size_t>(len));
            const int shown = truncate ? len + 1 : len;
            if (truncate) {
                buffer[len] = '~';
            }
            buffer[shown] = '\0';
            const int16_t x = MARGIN + ((TEXT_W - shown * 6 * size) / 2);
            gfx->setCursor(x > MARGIN ? x : MARGIN, top + i * step);
            gfx->print(buffer);
        }
    }

    auto drawNotice() -> void {
        auto* gfx = DisplayManager::getGfx();
        gfx->fillScreen(C_BG);

        const uint16_t color = levelColor(s_level);
        drawIcon(gfx, color);

        const char* name = levelName(s_level);
        gfx->setTextSize(2);
        gfx->setTextColor(color);
        gfx->setCursor(SCREEN / 2 - textWidthPx(name, 2) / 2, LABEL_Y);
        gfx->print(name);

        gfx->fillRect(MARGIN, RULE_Y, TEXT_W, RULE_H, C_SUB);

        // 字号选择：4 → 3 → 2 → 1，取第一个装得下的。短消息用大字号不显空，
        // 长消息自动降级；连字号 1 都装不下时按字号 1 排版并给末行补 '~'。
        // 循环体必然跑到 size == 1，m_lineCount / m_overflow 留下的就是最终排版。
        uint8_t chosen = 1;
        for (uint8_t size = 4; size >= 1; size--) {
            layoutBody(size, maxLinesFor(size));
            chosen = size;
            if (!m_overflow) {
                break;
            }
        }
        drawBody(gfx, chosen);

        drawCountdown();
        s_lastSecond = remainingCeil();
    }
};

// NoticeScene 的静态内容/状态（接口在头文件，实现只落在这里）
char NoticeOverlayScene::s_text[NoticeScene::kTextCap] = {0};
char NoticeOverlayScene::s_returnName[NoticeOverlayScene::kReturnCap] = {0};
char NoticeOverlayScene::s_returnParam[NoticeOverlayScene::kReturnCap] = {0};
NoticeScene::Level NoticeOverlayScene::s_level = NoticeScene::LevelInfo;
uint16_t NoticeOverlayScene::s_totalSeconds = NoticeScene::kDefaultSeconds;
uint32_t NoticeOverlayScene::s_startedMs = 0;
bool NoticeOverlayScene::s_active = false;
bool NoticeOverlayScene::s_imageMode = false;
bool NoticeOverlayScene::s_hasContent = false;
bool NoticeOverlayScene::s_pendingReturn = false;
uint16_t NoticeOverlayScene::s_lastSecond = 0;

// out-of-class 定义（C++11 下若被 ODR-use 则必须补，稳妥起见都补上）
constexpr uint16_t NoticeScene::kMaxSeconds;
constexpr uint16_t NoticeScene::kDefaultSeconds;
constexpr size_t NoticeScene::kTextCap;

auto NoticeScene::prepareText(Level level, const char* text, uint16_t seconds) -> void {
    NoticeOverlayScene::s_level = level;
    NoticeOverlayScene::s_imageMode = false;
    strlcpy(NoticeOverlayScene::s_text, text != nullptr ? text : "",
            sizeof(NoticeOverlayScene::s_text));
    NoticeOverlayScene::s_totalSeconds = NoticeOverlayScene::clampSeconds(seconds);
    NoticeOverlayScene::s_hasContent = true;
}

auto NoticeScene::prepareImage(uint16_t seconds) -> void {
    NoticeOverlayScene::s_imageMode = true;
    NoticeOverlayScene::s_totalSeconds = NoticeOverlayScene::clampSeconds(seconds);
    NoticeOverlayScene::s_hasContent = true;
}

auto NoticeScene::isActive() -> bool { return NoticeOverlayScene::s_active; }

auto NoticeScene::isImageMode() -> bool { return NoticeOverlayScene::s_imageMode; }

auto NoticeScene::level() -> Level { return NoticeOverlayScene::s_level; }

auto NoticeScene::text() -> const char* {
    return NoticeOverlayScene::s_imageMode ? "" : NoticeOverlayScene::s_text;
}

auto NoticeScene::totalSeconds() -> uint16_t { return NoticeOverlayScene::s_totalSeconds; }

auto NoticeScene::remainingSeconds() -> uint16_t {
    return NoticeOverlayScene::s_active ? NoticeOverlayScene::remainingCeil() : 0U;
}

static NoticeOverlayScene s_noticeScene;

auto registerBuiltinScenes() -> void {
    SceneManager::addScene(&s_sysInfoScene);
    SceneManager::addScene(&s_balanceScene);
    SceneManager::addScene(&s_albumScene);
    SceneManager::addScene(&s_stockScene);
    SceneManager::addScene(&s_clockScene);
    SceneManager::addScene(&s_liveScene);
    SceneManager::addScene(&s_noticeScene);
}
