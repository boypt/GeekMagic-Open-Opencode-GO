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

#include <ArduinoJson.h>
#include <LittleFS.h>

#include <Logger.h>
#include "config/ConfigManager.h"
#include "config/SecureStorage.h"

ConfigManager::ConfigManager(const char* filename) : filename(filename), secure() {}

/**
 * @brief Loads the configuration from a file stored in SPIFFS
 *
 * @return true if the configuration was successfully loaded and parsed false otherwise
 */
auto ConfigManager::load() -> bool {
    if (!LittleFS.begin()) {
        Logger::error("Failed to mount LittleFS", "ConfigManager");
        return false;
    }

    File file = LittleFS.open(filename.c_str(), "r");
    if (!file) {
        Logger::error("Failed to open config file", "ConfigManager");
        return false;
    }

    size_t size = file.size();
    if (size == 0) {
        Logger::warn("Config file is empty", "ConfigManager");
        file.close();
        return false;
    }

    std::unique_ptr<char[]> buf(new char[size + 1]);
    file.readBytes(buf.get(), size);
    buf[size] = '\0';
    file.close();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, buf.get());
    if (error) {
        Logger::error(("Failed to parse config file : " + String(error.c_str())).c_str(), "ConfigManager");
        return false;
    }

    String ssid = doc["wifi_ssid"] | "";
    String password = doc["wifi_password"] | "";
    String api_token = doc["api_token"] | "";
    String ntp_server_cfg = doc["ntp_server"] | "";
    String ocg_host = doc["opencodego_host"] | "";
    String ocg_path = doc["opencodego_path"] | "";
    String ocg_api_key = doc["opencodego_api_key"] | "";
    uint8_t ocg_verify_tls = doc["verify_tls_cert"] | verify_tls_cert;

    this->lcd_rotation = doc["lcd_rotation"] | lcd_rotation;
    this->lcd_mirror_x = doc["lcd_mirror_x"] | lcd_mirror_x;
    this->lcd_mirror_y = doc["lcd_mirror_y"] | lcd_mirror_y;
    this->lcd_bgr = doc["lcd_bgr"] | lcd_bgr;
    this->lcd_init_sd2 = doc["lcd_init_sd2"] | lcd_init_sd2;
    // 用 setter 统一钳制到 1..100；缺失时保持默认值 78
    const int lcd_brightness_cfg = doc["lcd_brightness"] | (int)lcd_brightness;
    this->setLCDBrightness(lcd_brightness_cfg);

    String nvs_ssid = secure.get("wifi_ssid", "");
    String nvs_password = secure.get("wifi_password", "");
    String nvs_api_token = secure.get("api_token", "");
    String nvs_ocg_api_key = secure.get("opencodego_api_key", "");

    if ((ssid.length() != 0 && nvs_ssid.length() == 0) || (password.length() != 0 && nvs_password.length() == 0)) {
        secure.put("wifi_ssid", ssid.c_str());
        secure.put("wifi_password", password.c_str());

        this->ssid = secure.get("wifi_ssid").c_str();
        this->password = secure.get("wifi_password").c_str();

        if (ntp_server_cfg.length() != 0) {
            this->ntp_server = ntp_server_cfg.c_str();
        }

        // Ensure we delete the wifi credentials from the json config after migrating
        ConfigManager::save();

        Logger::info("WiFi credentials migrated to SecureStorage", "ConfigManager");
    } else {
        this->ssid = secure.get("wifi_ssid").c_str();
        this->password = secure.get("wifi_password").c_str();
    }

    if (api_token.length() != 0 && api_token != nvs_api_token) {
        // config.json 里显式提供了 token：以它为准覆盖 SecureStorage，
        // 这样"改 config.json + 重刷文件系统"即可生效，无需先清空 NVS。
        secure.put("api_token", api_token.c_str());
        this->api_token = secure.get("api_token").c_str();

        // Ensure we delete the api token from the json config after migrating
        ConfigManager::save();

        Logger::info(nvs_api_token.length() == 0 ? "API token migrated to SecureStorage"
                                                : "API token updated from config.json",
                     "ConfigManager");
    } else {
        this->api_token = secure.get("api_token").c_str();
    }

    // OpenCode Go 配置：api_key 仿 api_token 走 SecureStorage（config.json 有值则迁移/覆盖）
    if (ocg_api_key.length() != 0 && ocg_api_key != nvs_ocg_api_key) {
        secure.put("opencodego_api_key", ocg_api_key.c_str());
        this->opencodego_api_key = secure.get("opencodego_api_key").c_str();

        // Ensure we delete the api key from the json config after migrating
        ConfigManager::save();

        Logger::info(nvs_ocg_api_key.length() == 0 ? "OpenCodeGo API key migrated to SecureStorage"
                                                   : "OpenCodeGo API key updated from config.json",
                     "ConfigManager");
    } else {
        this->opencodego_api_key = secure.get("opencodego_api_key").c_str();
    }

    if (ocg_host.length() != 0) {
        this->opencodego_host = ocg_host.c_str();
    }
    if (ocg_path.length() != 0) {
        this->opencodego_path = ocg_path.c_str();
    }
    this->verify_tls_cert = (ocg_verify_tls != 0) ? 1 : 0;

    return true;
}

/**
 * @brief Retrieves the current Wi-Fi SSID
 *
 * @return The SSID as a c style string
 */
auto ConfigManager::getSSID() const -> const char* { return ssid.c_str(); }

/**
 * @brief Retrieves the current Wi-Fi password
 *
 * @return The password as a c style string
 */
auto ConfigManager::getPassword() const -> const char* { return password.c_str(); }

/**
 * @brief Retrieves the current API token
 *
 * @return The API token as a c style string
 */
auto ConfigManager::getApiToken() const -> const char* { return api_token.c_str(); }

/**
 * @brief Retrieves the LCD rotation setting
 *
 * @return The rotation of the LCD
 */
auto ConfigManager::getLCDRotation() const -> uint8_t { return lcd_rotation; }

/**
 * @brief Set LCD rotation in memory
 *
 * @param newRotation Rotation value in range [0, 7]
 *
 * @return void
 */
auto ConfigManager::setLCDRotation(uint8_t newRotation) -> void { lcd_rotation = newRotation; }

/**
 * @brief Set WiFi credentials in memory
 * @param newSsid The SSID
 * @param newPassword The password
 *
 * @return void
 */
auto ConfigManager::setWiFi(const char* newSsid, const char* newPassword) -> void {
    if (newSsid != nullptr) {
        ssid = newSsid;
    }
    if (newPassword != nullptr) {
        password = newPassword;
    }
}
/**
 * @brief Set WiFi credentials in memory
 * @param newSsid The SSID
 * @param newPassword The password
 *
 * @return void
 */
auto ConfigManager::setApiToken(const char* newApiToken) -> void {
    if (newApiToken != nullptr) {
        api_token = newApiToken;
    }
}

/**
 * @brief Save the current configuration to the file
 *
 * @param clearWifiCreds If true wifi credentials will be cleared from json config
 *
 * @return true if the configuration was successfully saved false otherwise
 */
auto ConfigManager::save() -> bool {
    if (!LittleFS.begin()) {
        Logger::error("Failed to mount LittleFS", "ConfigManager");

        return false;
    }

    File file = LittleFS.open(filename.c_str(), "w");

    if (!file) {
        Logger::error("Failed to open config file for writing", "ConfigManager");

        return false;
    }

    JsonDocument doc;

    secure.put("wifi_ssid", this->getSSID());
    secure.put("wifi_password", this->getPassword());

    doc["lcd_rotation"] = lcd_rotation;
    doc["lcd_mirror_x"] = lcd_mirror_x;
    doc["lcd_mirror_y"] = lcd_mirror_y;
    doc["lcd_bgr"] = lcd_bgr;
    doc["lcd_init_sd2"] = lcd_init_sd2;
    doc["lcd_brightness"] = lcd_brightness;
    if (!this->ntp_server.empty()) {
        doc["ntp_server"] = this->ntp_server.c_str();
    }

    // OpenCode Go：host/path/tls 开关进 config.json，api_key 只进 SecureStorage
    secure.put("opencodego_api_key", this->getOpenCodeGoApiKey());
    if (!this->opencodego_host.empty()) {
        doc["opencodego_host"] = this->opencodego_host.c_str();
    }
    if (!this->opencodego_path.empty()) {
        doc["opencodego_path"] = this->opencodego_path.c_str();
    }
    doc["verify_tls_cert"] = this->verify_tls_cert;

    if (serializeJson(doc, file) == 0) {
        Logger::error("Failed to write config file", "ConfigManager");
        file.close();

        return false;
    }

    file.close();
    Logger::info("Configuration saved", "ConfigManager");

    return true;
}
