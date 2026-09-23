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

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <ArduinoJson.h>
#include "config/SecureStorage.h"
#include <string>
#include <cstdint>
#include <SPI.h>

// LCD configuration defaults for hellocubic lite
static constexpr int16_t LCD_W = 240;
static constexpr int16_t LCD_H = 240;
static constexpr uint8_t LCD_ROTATION = 4;
static constexpr int8_t LCD_MOSI_GPIO = 13;
static constexpr int8_t LCD_SCK_GPIO = 14;
static constexpr int8_t LCD_DC_GPIO = 0;
static constexpr int8_t LCD_RST_GPIO = 2;
static constexpr uint8_t LCD_SPI_MODE = SPI_MODE3;
static constexpr uint32_t LCD_SPI_HZ = 40000000;
static constexpr int8_t LCD_BACKLIGHT_GPIO = 5;
static constexpr bool LCD_BACKLIGHT_ACTIVE_LOW = true;

class ConfigManager {
   public:
    ConfigManager(const char* filename = "/config.json");
    bool load();
    bool save();
    void setWiFi(const char* newSsid, const char* newPassword);
    const char* getSSID() const;
    const char* getPassword() const;
    const char* getApiToken() const;
    void setApiToken(const char* newApiToken);
    uint8_t getLCDRotation() const;
    void setLCDRotation(uint8_t newRotation);
    uint32_t getLCDSpiHz() const;

    // ---- LCD 镜像翻转（MADCTL MX/MY 运行时可配）----
    bool getLCDMirrorX() const { return lcd_mirror_x; }
    bool getLCDMirrorY() const { return lcd_mirror_y; }
    void setLCDMirrorX(bool enable) { lcd_mirror_x = enable; }
    void setLCDMirrorY(bool enable) { lcd_mirror_y = enable; }

    // ---- OpenCode Go 用量配置 ----
    const char* getOpenCodeGoHost() const { return opencodego_host.c_str(); }
    const char* getOpenCodeGoPath() const { return opencodego_path.c_str(); }
    const char* getOpenCodeGoApiKey() const { return opencodego_api_key.c_str(); }
    bool getVerifyTlsCert() const { return verify_tls_cert != 0; }
    void setOpenCodeGoHost(const char* host) {
        if (host != nullptr) opencodego_host = host;
    }
    void setOpenCodeGoPath(const char* path) {
        if (path != nullptr) opencodego_path = path;
    }
    void setOpenCodeGoApiKey(const char* key) {
        if (key != nullptr) opencodego_api_key = key;
    }
    void setVerifyTlsCert(bool enable) { verify_tls_cert = enable ? 1 : 0; }

   public:
    uint8_t getLCDRotationSafe() const { return lcd_rotation; }
    std::string ssid;
    std::string password;
    std::string api_token;
    std::string filename;
    SecureStorage secure;
    uint8_t lcd_rotation = 4;

    // LCD 镜像翻转（MADCTL MX/MY 位，默认关闭）
    bool lcd_mirror_x = false;
    bool lcd_mirror_y = false;
    std::string ntp_server;

    // OpenCode Go 用量配置（api_key 走 SecureStorage，host/path/tls 开关走 config.json）
    std::string opencodego_host;
    std::string opencodego_path;
    std::string opencodego_api_key;
    uint8_t verify_tls_cert = 1;

    const char* getNtpServer() const { return ntp_server.c_str(); }
    void setNtpServer(const char* s) {
        if (s) ntp_server = s;
    }
};

#endif  // CONFIG_MANAGER_H
