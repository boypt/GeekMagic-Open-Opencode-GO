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

extern WiFiManager* wifiManager;

// ---------- startup：开机 IP 画面 ----------
// 纯手动场景：不再自动跳转（全手工切换策略），停留至用户切换
class StartupScene : public Scene {
   public:
    auto name() const -> const char* override { return "startup"; }

    auto enter(const char* param) -> bool override {
        (void)param;
        const String ip = (wifiManager != nullptr) ? wifiManager->getIP().toString() : String("--");
        DisplayManager::drawStartup(ip);

        return true;
    }

    auto update() -> void override {}

    auto exit() -> void override {}
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

static StartupScene s_startupScene;
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
    SceneManager::addScene(&s_startupScene);
    SceneManager::addScene(&s_balanceScene);
    SceneManager::addScene(&s_albumScene);
    SceneManager::addScene(&s_clockScene);
    SceneManager::addScene(&s_liveScene);
}
