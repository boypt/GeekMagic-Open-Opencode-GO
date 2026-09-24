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

        DisplayManager::clearScreen();
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
                    strlcpy(m_rows[i].name, m_saved[i].name, sizeof(m_rows[i].name));
                }
                drawRows();
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
    static constexpr uint16_t C_NEUTRAL = rgb565(0x8A, 0x94, 0xB8);
    static constexpr int16_t ROW_Y = 50;
    static constexpr uint8_t ROW_STEP = 27;
    static constexpr int16_t CLOCK_Y = 204;
    static constexpr int16_t NAME_X = 4;
    static constexpr int16_t VALUE_RIGHT = 236;
    static constexpr int8_t NAME_GAP = 4;

    StockData::Row m_rows[StockData::MAX_ROWS]{};
    StockData::Row m_saved[StockData::MAX_ROWS]{};
    uint8_t m_count = 0;
    uint32_t m_lastUpdated = 0;
    int m_lastMinute = -1;
    int m_lastSecond = -1;

    static auto formatChange(float change, char* out, size_t size) -> void {
        (void)size;
        // 先转百分位整数，避免把浮点 printf 及其格式化库带进 ESP8266 固件。
        int32_t cents = static_cast<int32_t>(change * 100.0F + (change >= 0.0F ? 0.5F : -0.5F));
        if (cents < 0) {
            out[0] = '-';
            cents = -cents;
        } else {
            out[0] = '+';
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
        size_t pos = 1;
        while (digitCount != 0U) {
            out[pos++] = digits[--digitCount];
        }
        out[pos++] = '.';
        out[pos++] = static_cast<char>('0' + fraction / 10U);
        out[pos++] = static_cast<char>('0' + fraction % 10U);
        out[pos++] = '%';
        out[pos] = '\0';
    }

    auto drawTitle() -> void {
        auto* gfx = DisplayManager::getGfx();
        gfx->setTextSize(2);
        gfx->setTextColor(C_WHITE);
        gfx->setCursor(120 - textWidthPx("STOCK", 2) / 2, 16);
        gfx->print("STOCK");
    }

    auto drawRows() -> void {
        auto* gfx = DisplayManager::getGfx();
        // 字号 2 的字高为 16px；27px 行距给每行保留 11px 的呼吸空间。
        // 擦除带在 47~179，时钟擦除带从 202 开始，中间保留 22px 安全间隔。
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
        for (uint8_t i = 0; i < StockData::MAX_ROWS; ++i) {
            const int16_t y = ROW_Y + i * ROW_STEP;
            gfx->setTextSize(2);
            if (i >= m_count) {
                gfx->setTextColor(C_SUB);
                gfx->setCursor(NAME_X, y);
                gfx->print("--");
                continue;
            }

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
        const int16_t clockX = 120 - textWidthPx("00:00:00", 3) / 2;
        const int16_t minuteWidth = textWidthPx("00:00:", 3);
        const int16_t secondX = clockX + minuteWidth;
        const int16_t secondWidth = textWidthPx("00", 3);
        if (fullRedraw) {
            gfx->fillRect(0, CLOCK_Y - 2, 240, 28, C_BG);
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

auto registerBuiltinScenes() -> void {
    SceneManager::addScene(&s_sysInfoScene);
    SceneManager::addScene(&s_balanceScene);
    SceneManager::addScene(&s_albumScene);
    SceneManager::addScene(&s_stockScene);
    SceneManager::addScene(&s_clockScene);
    SceneManager::addScene(&s_liveScene);
}
