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
#include "project_version.h"
#include <time.h>

extern WiFiManager* wifiManager;
extern ConfigManager configManager;

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
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
        char dateText[24];
        const time_t now = time(nullptr) + 8 * 3600;
        m_lastDay = static_cast<int>(now / 86400);
        struct tm localTime;
        gmtime_r(&now, &localTime);
        strftime(dateText, sizeof(dateText), "%Y-%m-%d  %a", &localTime);
        auto* gfx = DisplayManager::getGfx();
        gfx->setTextSize(1);
        gfx->setTextColor(C_ACCENT);
        gfx->setCursor(120 - textWidthPx(dateText, 1) / 2, 164);
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
                Logger::warn("AlbumScene: album is empty", "Scene");
                return false;
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

    char m_items[ALBUM_MAX][40] = {{0}};
    int m_count = 0;
    int m_index = 0;
    uint32_t m_lastMs = 0;

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
    SceneManager::addScene(&s_clockScene);
    SceneManager::addScene(&s_liveScene);
}
