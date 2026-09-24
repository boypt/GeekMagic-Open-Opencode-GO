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

#include <Arduino.h>
#include <Logger.h>
#include <ArduinoJson.h>
#include <Updater.h>
#include <LittleFS.h>

#include "web/Webserver.h"
#include "web/Api.h"
#include "display/DisplayManager.h"
#include "display/SceneManager.h"
#include "led/AmbientLight.h"
#include <Arduino_GFX_Library.h>
#include <WiFiClient.h>

/// 相册整幅尺寸：240x240 RGB565(LE)
static constexpr size_t ALBUM_IMG_BYTES = 240UL * 240UL * 2UL;

#include "config/ConfigManager.h"
#include "wireless/WiFiManager.h"
#include "ntp/NTPClient.h"
#include "opencodego/OpenCodeGoClient.h"

extern ConfigManager configManager;
extern WiFiManager* wifiManager;
extern NTPClient* ntpClient;

static bool otaError = false;
static size_t otaSize = 0;
static String otaStatus;
static volatile bool otaInProgress = false;
static volatile bool otaCancelRequested = false;
static size_t otaTotal = 0;

static constexpr int OTA_TEXT_X_OFFSET = 50;
static constexpr int OTA_TEXT_Y_OFFSET = 80;
static constexpr int OTA_LOADING_Y_OFFSET = 110;

static void otaHandleStart(HTTPUpload& upload, int mode);
static String updaterErrorString() {
    StreamString out;
    Update.printError(out);
    out.trim();
    return out;
}
static void otaHandleWrite(HTTPUpload& upload);
static void otaHandleEnd(HTTPUpload& upload, int mode);
static void otaHandleAborted(HTTPUpload& upload);
void handleDisplayRotationGet(Webserver* webserver);
void handleDisplayRotationSet(Webserver* webserver);
void handleDisplayMirrorGet(Webserver* webserver);
void handleDisplayMirrorSet(Webserver* webserver);
void handleDisplayBrightnessGet(Webserver* webserver);
void handleDisplayBrightnessSet(Webserver* webserver);
void handleDeleteGif(Webserver* webserver);
static constexpr int WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr size_t NTP_CONFIG_DOC_SIZE = 512;
static constexpr int BEARER_LEN = 7;

/**
 * @brief Register API endpoints for the webserver
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void registerApiEndpoints(Webserver* webserver) {
    Logger::info("Registering API endpoints", "API");

    // @openapi {get} /wifi/scan version=v1 group=WiFi summary="Scan available WiFi networks" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/scan", HTTP_GET, [webserver]() { handleWifiScan(webserver); });

    // @openapi {post} /wifi/connect version=v1 group=WiFi summary="Connect to a WiFi network" requiresAuth=true
    // requestBody=application/json requestBodySchema=ssid:string,password:string
    // example={"ssid":"MyNetwork","password":"password123"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/connect", HTTP_POST, [webserver]() { handleWifiConnect(webserver); });

    // @openapi {get} /wifi/status version=v1 group=WiFi summary="Get WiFi connection status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/status", HTTP_GET, [webserver]() { handleWifiStatus(webserver); });

    // @openapi {post} /ntp/sync version=v1 group=NTP summary="Trigger NTP sync" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/sync", HTTP_POST, [webserver]() { handleNtpSync(webserver); });

    // @openapi {get} /ntp/status version=v1 group=NTP summary="Get NTP status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/status", HTTP_GET, [webserver]() { handleNtpStatus(webserver); });

    // @openapi {get} /ntp/config version=v1 group=NTP summary="Get NTP configuration" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_GET, [webserver]() { handleNtpConfigGet(webserver); });

    // @openapi {post} /ntp/config version=v1 group=NTP summary="Set NTP configuration" requiresAuth=true
    // requestBody=application/json requestBodySchema=ntp_server:string example={"ntp_server":"pool.ntp.org"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_POST, [webserver]() { handleNtpConfigSet(webserver); });

    // @openapi {get} /display/rotation version=v1 group=Display summary="Get display rotation, mirror, color order and init profile settings" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/display/rotation", HTTP_GET, [webserver]() { handleDisplayRotationGet(webserver); });

    // @openapi {post} /display/rotation version=v1 group=Display summary="Set display rotation (optionally with mirror flags, BGR color order and init profile)" requiresAuth=true
    // requestBody=application/json requestBodySchema=rotation:integer,lcd_mirror_x:boolean,lcd_mirror_y:boolean,lcd_bgr:boolean,lcd_init_sd2:boolean example={"rotation":4,"lcd_mirror_x":false,"lcd_mirror_y":false,"lcd_bgr":false,"lcd_init_sd2":false}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/display/rotation", HTTP_POST, [webserver]() { handleDisplayRotationSet(webserver); });

    // @openapi {get} /display/mirror version=v1 group=Display summary="Get display mirror (MADCTL MX/MY), color order and init profile settings" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/display/mirror", HTTP_GET, [webserver]() { handleDisplayMirrorGet(webserver); });

    // @openapi {post} /display/mirror version=v1 group=Display summary="Set display mirror (optionally with BGR color order and init profile) and apply immediately" requiresAuth=true
    // requestBody=application/json requestBodySchema=lcd_mirror_x:boolean,lcd_mirror_y:boolean,lcd_bgr:boolean,lcd_init_sd2:boolean example={"lcd_mirror_x":true,"lcd_mirror_y":false,"lcd_bgr":false,"lcd_init_sd2":false}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/display/mirror", HTTP_POST, [webserver]() { handleDisplayMirrorSet(webserver); });

    // @openapi {get} /display/brightness version=v1 group=Display summary="Get LCD backlight brightness (percentage)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/display/brightness", HTTP_GET, [webserver]() { handleDisplayBrightnessGet(webserver); });

    // @openapi {post} /display/brightness version=v1 group=Display summary="Set LCD backlight brightness (percentage 1..100) and apply immediately" requiresAuth=true
    // requestBody=application/json requestBodySchema=lcd_brightness:integer example={"lcd_brightness":78}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/display/brightness", HTTP_POST, [webserver]() { handleDisplayBrightnessSet(webserver); });

    // @openapi {post} /reboot version=v1 group=System summary="Reboot the device" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/reboot", HTTP_POST, [webserver]() { handleReboot(webserver); });

    // @openapi {post} /ota/fw version=v1 group=OTA summary="Upload firmware (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fw", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FLASH); });

    // @openapi {post} /ota/fs version=v1 group=OTA summary="Upload filesystem (OTA)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fs", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FS); });

    // @openapi {get} /ota/status version=v1 group=OTA summary="Get OTA status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/status", HTTP_GET, [webserver]() { handleOtaStatus(webserver); });

    // @openapi {post} /ota/cancel version=v1 group=OTA summary="Cancel OTA" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/cancel", HTTP_POST, [webserver]() { handleOtaCancel(webserver); });

    // @openapi {post} /album version=v1 group=Album summary="Upload an album image (240x240 RGB565 raw, converted by the web client)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/album", HTTP_POST, [webserver]() { handleAlbumUploadDone(webserver); },
        [webserver]() { handleGifUpload(webserver); });

    // @openapi {delete} /album version=v1 group=Album summary="Delete an album image by name" requiresAuth=true
    // requestBody=application/json requestBodySchema=name:string example={"name":"photo.rgb565"}
    // responses=200:application/json,400:application/json,401:application/json,404:application/json
    webserver->raw().on("/api/v1/album", HTTP_DELETE, [webserver]() { handleDeleteGif(webserver); });

    // @openapi {get} /album version=v1 group=Album summary="List album images" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/album", HTTP_GET, [webserver]() { handleListGifs(webserver); });

    // @openapi {post} /album/live version=v1 group=Album summary="Stream one live frame (240x240 RGB565, multipart) - drawn immediately, not saved" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/album/live", HTTP_POST, [webserver]() { handleLivePushDone(webserver); },
        [webserver]() { handleLivePush(webserver); });

    // @openapi {get} /scene version=v1 group=Scene summary="Get current scene and available scenes" requiresAuth=true
    webserver->raw().on("/api/v1/scene", HTTP_GET, [webserver]() { handleSceneGet(webserver); });

    // @openapi {post} /scene version=v1 group=Scene summary="Switch display scene (old scene exits, new scene fully redraws)" requiresAuth=true
    // requestBody=application/json requestBodySchema=scene:string,param:string example={"scene":"gif","param":"animation.gif"}
    webserver->raw().on("/api/v1/scene", HTTP_POST, [webserver]() { handleSceneSet(webserver); });

    // @openapi {get} /light version=v1 group=Light summary="Get WS2812 ambient light settings" requiresAuth=true
    webserver->raw().on("/api/v1/light", HTTP_GET, [webserver]() { handleLightGet(webserver); });

    // @openapi {post} /light version=v1 group=Light summary="Set WS2812 ambient light (partial update)" requiresAuth=true
    // requestBody=application/json requestBodySchema=on:boolean,mode:string,r:integer,g:integer,b:integer,brightness:integer example={"on":true,"mode":"breathe","r":255,"g":140,"b":40,"brightness":60}
    webserver->raw().on("/api/v1/light", HTTP_POST, [webserver]() { handleLightSet(webserver); });

    // @openapi {get} /token/check version=v1 group=Authentication summary="Check bearer token validity"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/token/check", HTTP_GET, [webserver]() { handleTokenCheck(webserver); });

    // @openapi {post} /token/save version=v1 group=Authentication summary="Save a new bearer token" requiresAuth=true
    // requestBody=application/json requestBodySchema=token:string example={"token":"your_secure_token_value"}
    // responses=200:application/json,401:application/json,400:application/json
    webserver->raw().on("/api/v1/token/save", HTTP_POST, [webserver]() { handleTokenSave(webserver); });

    // @openapi {get} /logs version=v1 group=System summary="Get recent logs" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/logs", HTTP_GET, [webserver]() { handleLogsGet(webserver); });

    // @openapi {get} /logs/download version=v1 group=System summary="Download logs as text file" requiresAuth=true
    // responses=200:text/plain,401:application/json
    webserver->raw().on("/api/v1/logs/download", HTTP_GET, [webserver]() { handleLogsDownload(webserver); });

    // @openapi {post} /logs/clear version=v1 group=System summary="Clear log buffer" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/logs/clear", HTTP_POST, [webserver]() { handleLogsClear(webserver); });

    // @openapi {get} /opencodego/config version=v1 group=OpenCodeGo summary="Get OpenCode Go usage config" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/opencodego/config", HTTP_GET, [webserver]() { handleOpenCodeGoConfigGet(webserver); });

    // @openapi {post} /opencodego/config version=v1 group=OpenCodeGo summary="Set OpenCode Go usage config" requiresAuth=true
    // requestBody=application/json requestBodySchema=opencodego_host:string,opencodego_path:string,opencodego_api_key:string,verify_tls_cert:integer
    // example={"opencodego_host":"opencode.ai","opencodego_path":"/zen/go/v1/usage","opencodego_api_key":"sk-...","verify_tls_cert":1}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/opencodego/config", HTTP_POST, [webserver]() { handleOpenCodeGoConfigSet(webserver); });

    // @openapi {get} /opencodego/ca version=v1 group=OpenCodeGo summary="Get custom TLS CA (PEM) config" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/opencodego/ca", HTTP_GET, [webserver]() { handleOpenCodeGoCaGet(webserver); });

    // @openapi {post} /opencodego/ca version=v1 group=OpenCodeGo summary="Save custom TLS root CA PEM (validated, stored to /ca.pem)" requiresAuth=true
    // requestBody=application/json requestBodySchema=pem:string
    // example={"pem":"-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/opencodego/ca", HTTP_POST, [webserver]() { handleOpenCodeGoCaSet(webserver); });

    // @openapi {delete} /opencodego/ca version=v1 group=OpenCodeGo summary="Delete custom TLS CA (verification disabled when absent)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/opencodego/ca", HTTP_DELETE, [webserver]() { handleOpenCodeGoCaDelete(webserver); });


    webserver->raw().onNotFound([webserver]() {
        if (webserver->raw().method() == HTTP_OPTIONS) {
            setCorsHeaders(webserver);
            webserver->raw().send(HTTP_CODE_OK);
        }
    });
}

/**
 * @brief Set CORS headers for API responses
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void setCorsHeaders(Webserver* webserver) {
    webserver->raw().sendHeader("Access-Control-Allow-Origin", "*");
    webserver->raw().sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    webserver->raw().sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    webserver->raw().sendHeader("Access-Control-Max-Age", "3600");
}

/**
 * @brief Validate bearer token from Authorization header
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto validateBearerToken(Webserver* webserver) -> bool {
    if (!webserver->raw().hasHeader("Authorization")) {
        return false;
    }

    String authHeader = webserver->raw().header("Authorization");

    if (!authHeader.startsWith("Bearer ")) {
        return false;
    }

    String providedToken = authHeader.substring(BEARER_LEN);
    String storedToken = configManager.getApiToken();

    if (storedToken.length() == 0) {
        return false;
    }

    return providedToken.equals(storedToken);
}

/**
 * @brief Enforce bearer token check and send 401 response if invalid
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto requireBearerToken(Webserver* webserver) -> bool {
    if (validateBearerToken(webserver)) {
        return true;
    }

    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = "Invalid or missing token";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

    Logger::warn(("Unauthorized request from " + webserver->raw().client().remoteIP().toString()).c_str(), "API");

    return false;
}

/**
 * @brief Check if bearer token is valid
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenCheck(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token is valid";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Save a new bearer token
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenSave(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save API token with invalid JSON", "API");

        return;
    }

    const char* newToken = ddoc["token"] | "";

    if (strlen(newToken) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "token field is required";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save empty API token", "API");
        return;
    }

    configManager.setApiToken(newToken);
    configManager.save();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token saved successfully";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("API token updated", "API");
}

/**
 * @brief OTA status endpoint
 */
void handleOtaStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["inProgress"] = otaInProgress;
    doc["bytesWritten"] = otaSize;
    doc["totalBytes"] = otaTotal;
    doc["error"] = otaError;
    doc["message"] = otaStatus;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief OTA cancel endpoint
 */
void handleOtaCancel(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    otaCancelRequested = true;
    otaStatus = "Cancel requested";

    JsonDocument doc;
    doc["status"] = "cancelling";
    doc["message"] = "Cancel request received";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief List album images and FS info
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleListGifs(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();

    size_t usedBytes = 0;
    size_t totalBytes = 0;

    if (LittleFS.begin()) {
        Dir dir = LittleFS.openDir("/album");

        while (dir.next()) {
            String name = dir.fileName();
            if (name.endsWith(".rgb565")) {
                JsonObject fileObj = files.add<JsonObject>();

                fileObj["name"] = name;            // NOLINT(readability-misplaced-array-index)
                fileObj["size"] = dir.fileSize();  // NOLINT(readability-misplaced-array-index)
                usedBytes += dir.fileSize();
            }
        }

        FSInfo fs_info;

        if (LittleFS.info(fs_info)) {
            totalBytes = fs_info.totalBytes;
            usedBytes = fs_info.usedBytes;
        }
    }

    doc["usedBytes"] = usedBytes;
    doc["totalBytes"] = totalBytes;
    doc["freeBytes"] = totalBytes > usedBytes ? totalBytes - usedBytes : 0;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload start
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadStart(const String& currentFilename, File& gifFile, bool& uploadError) {
    uploadError = false;
    Logger::info((String("UPLOAD_FILE_START for: ") + currentFilename).c_str(), "API::Album");

    if (!LittleFS.exists("/album")) {
        Logger::info("/album directory does not exist, creating...", "API::Album");
        if (!LittleFS.mkdir("/album")) {
            Logger::error("Failed to create /album directory!", "API::Album");
        }
    }

    gifFile = LittleFS.open(currentFilename, "w");
    if (!gifFile) {
        uploadError = true;
        Logger::error((String("Impossible to open file: ") + currentFilename).c_str(), "API::Album");
        Logger::error("GIF UPLOAD Failed to open file", "API::Album");
    } else {
        Logger::info("File opened successfully for writing.", "API::Album");
    }
}

/**
 * @brief Handle GIF upload write
 * @param upload Reference to the HTTPUpload object
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadWrite(HTTPUpload& upload, File& gifFile, bool& uploadError) {
    if (!uploadError && gifFile) {
        size_t total = 0;

        while (total < upload.currentSize) {
            size_t remaining = upload.currentSize - total;
            int toWrite = static_cast<int>(remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : remaining);
            size_t written = gifFile.write(upload.buf + total, toWrite);

            if (written == 0) {
                Logger::error("Write returned 0 bytes!", "API::Album");
                uploadError = true;
                break;
            }

            total += written;
        }
    } else {
        Logger::error("Cannot write, file not open or previous error", "API::Album");
    }
}

/**
 * @brief Handle GIF upload end
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 *
 * @return void
 */
void handleGifUploadEnd(const String& currentFilename, File& gifFile, bool& uploadError) {
    if (gifFile) {
        gifFile.close();
    }

    // 校验整幅尺寸：240x240 RGB565 = 115200B，残图/错格式直接拒绝
    if (!uploadError) {
        File check = LittleFS.open(currentFilename, "r");
        const size_t size = check ? check.size() : 0;

        if (check) {
            check.close();
        }

        if (size != ALBUM_IMG_BYTES) {
            LittleFS.remove(currentFilename);
            uploadError = true;
            Logger::error((String("Album upload size mismatch: ") + String(size)).c_str(), "API::Album");
        }
    }

    Logger::info((String("Album upload end: ") + currentFilename).c_str(), "API::Album");
}

/**
 * @brief Handle GIF upload aborted
 * @param currentFilename The current filename being uploaded
 * @param gifFile Reference to the File object for the GIF
 * @param uploadError Reference to the upload error flag
 *
 * @return void
 */
void handleGifUploadAborted(const String& currentFilename, File& gifFile, bool& uploadError) {
    Logger::warn("UPLOAD_FILE_ABORTED", "API::Album");

    if (gifFile) {
        gifFile.close();

        Logger::warn("File closed after abort", "API::Album");
    }

    if (!currentFilename.isEmpty()) {
        if (LittleFS.remove(currentFilename)) {
            Logger::warn((String("Removed incomplete file: ") + currentFilename).c_str(), "API::Album");
        } else {
            Logger::error((String("Failed to remove incomplete file: ") + currentFilename).c_str(), "API::Album");
        }
    }

    uploadError = true;
}

/**
 * @brief Send GIF upload result
 * @param webserver Pointer to the Webserver instance
 * @param currentFilename The current filename being uploaded
 * @param uploadError The upload error flag
 *
 * @return void
 */
void sendGifUploadResult(Webserver* webserver, const String& currentFilename, bool uploadError) {
    JsonDocument doc;

    if (uploadError) {
        doc["status"] = "error";
        doc["message"] = "Image upload failed (need 240x240 RGB565)";

        Logger::error("GIF UPLOAD Error during upload", "API::Album");
    } else {
        doc["status"] = "success";
        doc["message"] = "Image uploaded";
        doc["filename"] = currentFilename;

        Logger::info((String("Image upload success, filename: ") + currentFilename).c_str(), "API::Album");
    }

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GIF upload
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
// multipart 上传状态：upload 回调只收数据并记录结果，
// HTTP 应答由请求完成时的 handleAlbumUploadDone() 统一发送（在回调里 send 会竞态空包）
static File albumUploadFile;
static bool albumUploadError = false;
static bool albumUploadAuthed = true;
static String albumUploadedName;

void handleGifUpload(Webserver* webserver) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START) {
        albumUploadError = false;
        albumUploadAuthed = validateBearerToken(webserver);
        albumUploadedName = "";

        if (!albumUploadAuthed) {
            return;
        }
    }

    if (!albumUploadAuthed) {
        return;
    }

    String filename = upload.filename;
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    if (!filename.endsWith(".rgb565")) {
        filename += ".rgb565";
    }
    String currentFilename = "/album/" + filename;

    switch (upload.status) {
        case UPLOAD_FILE_START:
            handleGifUploadStart(currentFilename, albumUploadFile, albumUploadError);
            break;
        case UPLOAD_FILE_WRITE:
            handleGifUploadWrite(upload, albumUploadFile, albumUploadError);
            break;
        case UPLOAD_FILE_END:
            handleGifUploadEnd(currentFilename, albumUploadFile, albumUploadError);
            albumUploadedName = currentFilename;
            break;
        case UPLOAD_FILE_ABORTED:
            handleGifUploadAborted(currentFilename, albumUploadFile, albumUploadError);
            albumUploadedName = currentFilename;
            break;
        default:
            Logger::warn("Unknown upload status.", "API::Album");
            break;
    }
}

/**
 * @brief 上传请求完成回调：统一发送上传结果应答
 */
void handleAlbumUploadDone(Webserver* webserver) {
    if (!albumUploadAuthed) {
        JsonDocument doc;

        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    sendGifUploadResult(webserver, albumUploadedName, albumUploadError);
}

// ---- live 实时推图：multipart 分块流式直绘（不落盘、不整帧进 RAM）----
// 帧 = 240x240 RGB565(LE) 115200B；行缓冲 480B 组装整行后立即写屏
static bool s_liveAuthed = true;
static bool s_liveAborted = false;
static int s_liveRowY = 0;
static int s_liveRowFill = 0;
static uint8_t s_liveRow[480];

static void livePushBytes(const uint8_t* data, size_t len) {
    auto* tft = reinterpret_cast<Arduino_TFT*>(DisplayManager::getGfx());

    for (size_t i = 0; i < len; i++) {
        if (s_liveRowY >= 240) {
            return;  // 超出整幅的余数忽略
        }

        s_liveRow[s_liveRowFill++] = data[i];

        if (s_liveRowFill == 480) {
            tft->startWrite();
            tft->writeAddrWindow(0, s_liveRowY, 240, 1);
            // 同 AlbumScene：标准 RGB565(LE) 直推在本面板红蓝互换，做 R/B 字段交换补偿
        auto* px = reinterpret_cast<uint16_t*>(s_liveRow);
        for (int i = 0; i < 240; i++) {
            const uint16_t v = px[i];
            px[i] = static_cast<uint16_t>(((v & 0x001FU) << 11) | (v & 0x07E0U) | ((v & 0xF800U) >> 11));
        }
        tft->writePixels(reinterpret_cast<uint16_t*>(s_liveRow), 240);
            tft->endWrite();

            s_liveRowFill = 0;
            s_liveRowY++;
        }
    }
}

void handleLivePush(Webserver* webserver) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START) {
        s_liveAuthed = validateBearerToken(webserver);
        s_liveAborted = false;
        s_liveRowY = 0;
        s_liveRowFill = 0;

        if (!s_liveAuthed) {
            return;
        }

        // 首帧推送自动进入 live 场景（退场重绘契约）；后续帧仅覆盖画面
        if (strcmp(SceneManager::currentName(), "live") != 0) {
            SceneManager::switchTo("live");
        }

        return;
    }

    if (!s_liveAuthed) {
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        livePushBytes(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        s_liveAborted = true;
    }
}

/**
 * @brief live 推图请求完成回调：统一发送应答
 */
void handleLivePushDone(Webserver* webserver) {
    if (!s_liveAuthed) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "Invalid or missing token";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", jsonOut);

        return;
    }

    JsonDocument resp;

    resp["status"] = s_liveAborted ? "error" : "ok";
    resp["rows"] = s_liveRowY;

    if (s_liveAborted) {
        resp["message"] = "upload aborted";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Reboot endpoint
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleReboot(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 1000;

    doc["status"] = "rebooting";
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    delay(rebootDelayMs);
    ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
}

/**
 * @brief Manual NTP sync trigger endpoint
 */
void handleNtpSync(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    bool syncOk = ntpClient->syncNow();
    doc["status"] = syncOk ? "ok" : "error";
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Return NTP status
 */
void handleNtpStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);
        return;
    }

    doc["lastOk"] = ntpClient->lastSyncOk();
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get NTP configuration
 */
void handleNtpConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["ntp_server"] = configManager.getNtpServer();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set NTP configuration
 */
void handleNtpConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    const char* server = ddoc["ntp_server"] | "";

    if (strlen(server) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "ntp_server missing";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    configManager.setNtpServer(server);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;

        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    // optionally trigger a sync
    if (ntpClient != nullptr) {
        ntpClient->syncNow();
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["ntp_server"] = server;
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get display rotation configuration
 */
void handleDisplayRotationGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["rotation"] = configManager.getLCDRotationSafe();
    doc["lcd_mirror_x"] = configManager.getLCDMirrorX();
    doc["lcd_mirror_y"] = configManager.getLCDMirrorY();
    doc["lcd_bgr"] = configManager.getLCDBgr();
    doc["lcd_init_sd2"] = configManager.getLCDInitSd2();
    doc["lcd_brightness"] = configManager.getLCDBrightness();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set display rotation configuration
 */
void handleDisplayRotationSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err || !ddoc["rotation"].is<int>()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON or missing rotation";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    int rotation = ddoc["rotation"].as<int>();
    const int rotation_range_min = 0;
    const int rotation_range_max = 7;

    if (rotation < rotation_range_min || rotation > rotation_range_max) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] =
            "rotation must be between " + String(rotation_range_min) + " and " + String(rotation_range_max);

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    auto newRotation = static_cast<uint8_t>(rotation);

    // 可选镜像翻转/面板 profile 参数：POST 体中带 lcd_mirror_x / lcd_mirror_y /
    // lcd_bgr / lcd_init_sd2（bool）则一并持久化
    const bool oldInitSd2 = configManager.getLCDInitSd2();
    if (ddoc["lcd_mirror_x"].is<bool>()) {
        configManager.setLCDMirrorX(ddoc["lcd_mirror_x"].as<bool>());
    }
    if (ddoc["lcd_mirror_y"].is<bool>()) {
        configManager.setLCDMirrorY(ddoc["lcd_mirror_y"].as<bool>());
    }
    if (ddoc["lcd_bgr"].is<bool>()) {
        configManager.setLCDBgr(ddoc["lcd_bgr"].as<bool>());
    }
    if (ddoc["lcd_init_sd2"].is<bool>()) {
        configManager.setLCDInitSd2(ddoc["lcd_init_sd2"].as<bool>());
    }

    configManager.setLCDRotation(newRotation);
    String currentIP = "unknown";

    if (wifiManager != nullptr) {
        currentIP = wifiManager->getIP().toString();
    }

    if (configManager.getLCDInitSd2() || oldInitSd2 != configManager.getLCDInitSd2()) {
        // init profile 参与切换（或已处于 sd2 profile）时需完整重新初始化面板
        DisplayManager::applyPanelProfile();
    } else {
        DisplayManager::setRotation(newRotation, currentIP);
    }

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["rotation"] = newRotation;
    doc["lcd_mirror_x"] = configManager.getLCDMirrorX();
    doc["lcd_mirror_y"] = configManager.getLCDMirrorY();
    doc["lcd_bgr"] = configManager.getLCDBgr();
    doc["lcd_init_sd2"] = configManager.getLCDInitSd2();
    doc["lcd_brightness"] = configManager.getLCDBrightness();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info(("Display rotation updated to " + String(newRotation) +
                  " BGR=" + (configManager.getLCDBgr() ? "1" : "0") +
                  " init=" + (configManager.getLCDInitSd2() ? "sd2" : "vendor"))
                     .c_str(),
                 "API");
}

/**
 * @brief Get display mirror (MADCTL MX/MY) configuration
 */
void handleDisplayMirrorGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["lcd_mirror_x"] = configManager.getLCDMirrorX();
    doc["lcd_mirror_y"] = configManager.getLCDMirrorY();
    doc["lcd_bgr"] = configManager.getLCDBgr();
    doc["lcd_init_sd2"] = configManager.getLCDInitSd2();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set display mirror (MADCTL MX/MY) configuration and apply immediately
 */
void handleDisplayMirrorSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    // 允许部分更新：mirror 与 panel profile 字段任一存在即可，
    // 避免前端只提交 lcd_bgr/lcd_init_sd2 时被误判为非法请求。
    const bool hasMirrorField = ddoc["lcd_mirror_x"].is<bool>() || ddoc["lcd_mirror_y"].is<bool>();
    const bool hasProfileField = ddoc["lcd_bgr"].is<bool>() || ddoc["lcd_init_sd2"].is<bool>();

    if (err || (!hasMirrorField && !hasProfileField)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON or missing display settings (lcd_mirror_x/lcd_mirror_y/lcd_bgr/lcd_init_sd2)";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    if (ddoc["lcd_mirror_x"].is<bool>()) {
        configManager.setLCDMirrorX(ddoc["lcd_mirror_x"].as<bool>());
    }
    if (ddoc["lcd_mirror_y"].is<bool>()) {
        configManager.setLCDMirrorY(ddoc["lcd_mirror_y"].as<bool>());
    }

    // 可选面板 profile 参数：POST 体中带 lcd_bgr / lcd_init_sd2（bool）则一并处理
    const bool oldInitSd2 = configManager.getLCDInitSd2();
    if (ddoc["lcd_bgr"].is<bool>()) {
        configManager.setLCDBgr(ddoc["lcd_bgr"].as<bool>());
    }
    if (ddoc["lcd_init_sd2"].is<bool>()) {
        configManager.setLCDInitSd2(ddoc["lcd_init_sd2"].as<bool>());
    }

    // 立刻应用：init profile 变化需完整重新初始化，否则仅重发 MADCTL
    String currentIP = "unknown";
    if (wifiManager != nullptr) {
        currentIP = wifiManager->getIP().toString();
    }

    if (configManager.getLCDInitSd2() || oldInitSd2 != configManager.getLCDInitSd2()) {
        DisplayManager::applyPanelProfile();
    } else {
        DisplayManager::setRotation(configManager.getLCDRotationSafe(), currentIP);
    }

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["lcd_mirror_x"] = configManager.getLCDMirrorX();
    doc["lcd_mirror_y"] = configManager.getLCDMirrorY();
    doc["lcd_bgr"] = configManager.getLCDBgr();
    doc["lcd_init_sd2"] = configManager.getLCDInitSd2();
    doc["lcd_brightness"] = configManager.getLCDBrightness();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info(("Display mirror updated: x=" + String(configManager.getLCDMirrorX() ? "1" : "0") +
                  " y=" + String(configManager.getLCDMirrorY() ? "1" : "0") +
                  " BGR=" + (configManager.getLCDBgr() ? "1" : "0") +
                  " init=" + (configManager.getLCDInitSd2() ? "sd2" : "vendor"))
                     .c_str(),
                 "API");
}

/**
 * @brief Get LCD backlight brightness configuration
 */
void handleDisplayBrightnessGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["lcd_brightness"] = configManager.getLCDBrightness();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set LCD backlight brightness configuration and apply immediately
 */
void handleDisplayBrightnessSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err || !ddoc["lcd_brightness"].is<int>()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON or missing lcd_brightness";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    const int brightness = ddoc["lcd_brightness"].as<int>();
    if (brightness < 1 || brightness > 100) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "lcd_brightness must be between 1 and 100";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    configManager.setLCDBrightness(brightness);

    // 立即应用新亮度
    DisplayManager::setBacklight(configManager.getLCDBrightness());

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["lcd_brightness"] = configManager.getLCDBrightness();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info(("Display brightness updated to " + String(configManager.getLCDBrightness()) + "%").c_str(), "API");
}

/**
 * @brief Handle OTA upload
 * @param webserver Pointer to the Webserver instance
 * @param mode Update mode U_FLASH U_FS
 *
 * @return void
 */
void handleOtaUpload(Webserver* webserver, int mode) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        otaError = true;
        otaStatus = "Unauthorized";

        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    switch (upload.status) {
        case UPLOAD_FILE_START:
            otaHandleStart(upload, mode);
            break;
        case UPLOAD_FILE_WRITE:
            otaHandleWrite(upload);
            break;
        case UPLOAD_FILE_END:
            otaHandleEnd(upload, mode);
            break;
        case UPLOAD_FILE_ABORTED:
            otaHandleAborted(upload);
            break;
        default:
            break;
    }
}

/**
 * @brief Handle OTA finished
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleOtaFinished(Webserver* webserver) {
    if (!validateBearerToken(webserver)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 5000;

    doc["status"] = "Upload successful";
    doc["message"] = otaStatus;

    if (otaError) {
        doc["status"] = "Error";
    }

    otaInProgress = false;
    otaCancelRequested = false;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    if (!otaError) {
        delay(rebootDelayMs);
        ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
    }
}

/**
 * @brief Get current scene and available scenes
 */
void handleSceneGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["current"] = SceneManager::currentName();
    doc["param"] = SceneManager::currentParam();

    JsonArray scenes = doc["scenes"].to<JsonArray>();

    for (int i = 0; i < SceneManager::sceneCount(); i++) {
        scenes.add(SceneManager::sceneNameAt(i));
    }

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Switch display scene (old scene exits, new scene fully redraws)
 */
void handleSceneSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);
    const char* scene = ddoc["scene"];
    const char* param = ddoc["param"];

    if (err || scene == nullptr || strlen(scene) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "Invalid JSON or missing scene";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", jsonOut);

        return;
    }

    const bool ok = SceneManager::switchTo(scene, param != nullptr ? param : "");

    JsonDocument resp;
    resp["status"] = ok ? "ok" : "error";
    resp["current"] = SceneManager::currentName();

    if (!ok) {
        resp["message"] = "scene switch failed (unknown scene or scene refused)";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(ok ? HTTP_CODE_OK : HTTP_CODE_BAD_REQUEST, "application/json", jsonOut);

    Logger::info((String("Scene switch to ") + String(scene) + (ok ? " ok" : " failed")).c_str(), "API");
}

/**
 * @brief Get WS2812 ambient light settings
 */
void handleLightGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    static const char* const kModes[3] = {"solid", "breathe", "rainbow"};

    JsonDocument doc;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    configManager.getLedColor(r, g, b);

    doc["on"] = configManager.getLedOn();
    doc["mode"] = kModes[configManager.getLedMode() % 3];
    doc["r"] = r;
    doc["g"] = g;
    doc["b"] = b;
    doc["brightness"] = configManager.getLedBrightness();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set WS2812 ambient light（部分更新：on / mode / r,g,b / brightness）
 */
void handleLightSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", jsonOut);

        return;
    }

    if (ddoc["on"].is<bool>()) {
        AmbientLight::setOn(ddoc["on"].as<bool>());
    }

    if (ddoc["mode"].is<const char*>()) {
        const String mode = String(ddoc["mode"].as<const char*>());

        if (mode == "solid") {
            AmbientLight::setMode(0);
        } else if (mode == "breathe") {
            AmbientLight::setMode(1);
        } else if (mode == "rainbow") {
            AmbientLight::setMode(2);
        }
    }

    if (ddoc["r"].is<int>() || ddoc["g"].is<int>() || ddoc["b"].is<int>()) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        configManager.getLedColor(r, g, b);

        if (ddoc["r"].is<int>()) {
            r = static_cast<uint8_t>(ddoc["r"].as<int>() & 0xFF);
        }
        if (ddoc["g"].is<int>()) {
            g = static_cast<uint8_t>(ddoc["g"].as<int>() & 0xFF);
        }
        if (ddoc["b"].is<int>()) {
            b = static_cast<uint8_t>(ddoc["b"].as<int>() & 0xFF);
        }

        AmbientLight::setColor(r, g, b);
    }

    if (ddoc["brightness"].is<int>()) {
        AmbientLight::setBrightness(ddoc["brightness"].as<int>());
    }

    configManager.save();

    handleLightGet(webserver);
}

/**
 * @brief Delete a GIF file from storage
 */
void handleDeleteGif(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* name = doc["name"];
    if (name == nullptr || strlen(name) == 0) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "missing name";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    String filename(name);
    filename.replace("\\", "/");
    filename = filename.substring(filename.lastIndexOf('/') + 1);
    String path = String("/album/") + filename;

    if (!LittleFS.exists(path)) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "file not found";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_NOT_FOUND, "application/json", jsonOut);

        return;
    }

    if (LittleFS.remove(path)) {
        JsonDocument resp;
        resp["status"] = "success";
        resp["message"] = "file removed";
        resp["file"] = path;

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);

        Logger::info((String("Removed file: ") + path).c_str(), "API::Album");
    } else {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "failed to remove file";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        Logger::error((String("Failed to remove file: ") + path).c_str(), "API::Album");
    }
}

/**
 * @brief Handle WiFi scan
 */
void handleWifiScan(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (wifiManager != nullptr) {
        WiFiManager::scanNetworks(networks);
    }

    String out;
    serializeJson(doc["networks"], out);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", out);
}

/**
 * @brief Handle WiFi connect request
 */
void handleWifiConnect(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";

    if (strlen(ssid) == 0) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "missing ssid";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    bool connectOk = false;
    if (wifiManager != nullptr) {
        connectOk = wifiManager->connectToNetwork(ssid, password, WIFI_CONNECT_TIMEOUT_MS);
    }

    JsonDocument resp;

    resp["status"] = connectOk ? "connected" : "error";
    resp["ssid"] = ssid;

    if (connectOk) {
        resp["ip"] = wifiManager->getIP().toString();
        configManager.setWiFi(ssid, password);
        configManager.save();
    }

    if (!connectOk) {
        resp["message"] = "failed to connect";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief WiFi status
 */
void handleWifiStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    bool connected = (wifiManager != nullptr) && WiFiManager::isConnected();

    resp["connected"] = connected;
    resp["ssid"] = connected ? WiFiManager::getConnectedSSID() : "";
    resp["ip"] = connected ? wifiManager->getIP().toString() : "";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Handle OTA start
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleStart(HTTPUpload& upload, int mode) {
    Logger::info((String("OTA start: ") + upload.filename).c_str(), "API::OTA");

    otaError = false;
    otaSize = 0;
    otaStatus = "";
    otaInProgress = true;
    otaCancelRequested = false;
    otaTotal = static_cast<size_t>(upload.contentLength);

    DisplayManager::clearScreen();
    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Uploading...", 2, LCD_WHITE, LCD_BLACK,
                                    true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

    int constexpr security_space = 0x1000;
    u_int constexpr bin_mask = 0xFFFFF000;

    FSInfo fs_info;
    LittleFS.info(fs_info);
    size_t fsSize = fs_info.totalBytes;
    size_t maxSketchSpace =
        (ESP.getFreeSketchSpace() - security_space) &  // NOLINT(readability-static-accessed-through-instance)
        bin_mask;
    size_t place = (mode == U_FS) ? fsSize : maxSketchSpace;

    if (!Update.begin(place, mode)) {
        otaError = true;
        otaStatus = updaterErrorString();
        Logger::error((String("Update.begin failed: ") + otaStatus).c_str(), "API::OTA");
    }
}

/**
 * @brief Handle OTA write
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleWrite(HTTPUpload& upload) {
    if (!otaError) {
        if (otaCancelRequested) {
            Update.end();
            otaError = true;
            otaStatus = "Update canceled";
            otaInProgress = false;
            Logger::warn("OTA canceled by user", "API::OTA");

            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Canceled", 2, LCD_WHITE, LCD_BLACK,
                                            true);
            DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

            return;
        }

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaError = true;
            otaStatus = updaterErrorString();
            Logger::error((String("Write failed: ") + otaStatus).c_str(), "API::OTA");
        }

        otaSize += upload.currentSize;

        float progress = 0.0F;
        if (otaTotal > 0) {
            progress = static_cast<float>(otaSize) / static_cast<float>(otaTotal);
        }

        DisplayManager::drawLoadingBar(progress, OTA_LOADING_Y_OFFSET);
    }
}

/**
 * @brief Handle OTA end
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleEnd(HTTPUpload& /*upload*/, int mode) {
    if (!otaError) {
        if (Update.end(true)) {
            if (mode == U_FS) {
                Logger::info("OTA FS update complete, mounting file system...", "API::OTA");
                LittleFS.begin();
            }

            otaStatus = String("Update OK (") + String(otaSize) + " bytes)";
            Logger::info(otaStatus.c_str(), "API::OTA");

            DisplayManager::drawLoadingBar(1.0F, OTA_LOADING_Y_OFFSET);
            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Success!", 2, LCD_WHITE, LCD_BLACK,
                                            true);
        } else {
            otaError = true;
            otaStatus = updaterErrorString();
        }
    }
}

/**
 * @brief Handle OTA aborted
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleAborted(HTTPUpload& /*upload*/) {
    Update.end();
    otaError = true;
    otaStatus = "Update aborted";
    otaInProgress = false;
    otaCancelRequested = false;

    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Aborted", 2, LCD_WHITE, LCD_BLACK, true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);
}

/**
 * @brief Get recent logs
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray logsArray = doc["logs"].to<JsonArray>();

    size_t count = Logger::getLogCount();
    for (size_t i = 0; i < count; i++) {
        const char* entry = Logger::getLogEntry(i);
        if (entry != nullptr) {
            logsArray.add(entry);
        }
    }

    doc["count"] = count;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Download logs as a text file
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsDownload(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String logs = Logger::getLogsAsString();

    setCorsHeaders(webserver);
    webserver->raw().sendHeader("Content-Disposition", "attachment; filename=\"logs.log\"");
    webserver->raw().send(HTTP_CODE_OK, "text/plain", logs);
}

/**
 * @brief Clear log buffer
 * @param webserver Pointer to the Webserver instance
 */
void handleLogsClear(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    Logger::clearLogs();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Logs cleared";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Mask an OpenCode Go API key as sk-***<last4>
 * @param key Full API key
 *
 * @return Masked key string, or empty string when key is empty
 */
static String maskOpenCodeGoApiKey(const String& key) {
    if (key.isEmpty()) {
        return "";
    }
    constexpr size_t TAIL_LEN = 4;
    if (key.length() <= TAIL_LEN) {
        return "sk-****";
    }
    return "sk-***" + key.substring(key.length() - TAIL_LEN);
}

/**
 * @brief Get OpenCode Go usage configuration (api key masked)
 */
void handleOpenCodeGoConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["opencodego_host"] = configManager.getOpenCodeGoHost();
    doc["opencodego_path"] = configManager.getOpenCodeGoPath();
    doc["opencodego_api_key"] = maskOpenCodeGoApiKey(String(configManager.getOpenCodeGoApiKey()));
    doc["verify_tls_cert"] = configManager.getVerifyTlsCert() ? 1 : 0;
    doc["api_key_configured"] = configManager.getOpenCodeGoApiKey()[0] != '\0';

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set OpenCode Go usage configuration
 */
void handleOpenCodeGoConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    const char* host = ddoc["opencodego_host"] | "";
    const char* path = ddoc["opencodego_path"] | "";
    const char* apiKey = ddoc["opencodego_api_key"] | "";

    if (strlen(host) == 0 || strlen(path) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "opencodego_host and opencodego_path are required";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    configManager.setOpenCodeGoHost(host);
    configManager.setOpenCodeGoPath(path);
    // api_key 为空表示不修改已保存的 Key（避免 GET 回显打码值被误存回）
    if (strlen(apiKey) != 0 && !String(apiKey).startsWith("sk-***")) {
        configManager.setOpenCodeGoApiKey(apiKey);
    }
    if (ddoc["verify_tls_cert"].is<int>()) {
        configManager.setVerifyTlsCert(ddoc["verify_tls_cert"].as<int>() != 0);
    }

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["opencodego_host"] = configManager.getOpenCodeGoHost();
    doc["opencodego_path"] = configManager.getOpenCodeGoPath();
    doc["opencodego_api_key"] = maskOpenCodeGoApiKey(String(configManager.getOpenCodeGoApiKey()));
    doc["verify_tls_cert"] = configManager.getVerifyTlsCert() ? 1 : 0;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("OpenCodeGo config updated", "API");
}

/**
 * @brief TLS trust CA file path + temp file for atomic write
 */
static constexpr const char* OC_CA_PATH = "/ca.pem";
static constexpr const char* OC_CA_TMP_PATH = "/ca.pem.tmp";
static constexpr size_t OC_CA_MAX_BYTES = 8192;

/**
 * @brief GET /api/v1/opencodego/ca
 *
 * 返回当前 TLS 信任锚配置：custom（LittleFS /ca.pem，含完整 PEM）或
 * 未配置即不校验（setInsecure）。
 */
void handleOpenCodeGoCaGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    size_t bytes = 0;
    if (LittleFS.exists(OC_CA_PATH)) {
        File f = LittleFS.open(OC_CA_PATH, "r");
        if (f && f.size() > 0) {
            bytes = f.size();
            // 流式读入临时 String（PEM ~2KB），手写 JSON 需转义，借用
            // ArduinoJson 序列化保证转义正确
            String pem = f.readString();
            f.close();
            doc["source"] = "custom";
            doc["bytes"] = bytes;
            doc["pem"] = pem;
        } else if (f) {
            f.close();
        }
    }

    if (bytes == 0) {
        // 源码不再内置默认证书：未配置 /ca.pem 即不校验
        doc["source"] = "none";
        doc["bytes"] = 0;
    }

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief POST /api/v1/opencodego/ca  body {"pem":"-----BEGIN CERTIFICATE-----..."}
 *
 * 校验 PEM 结构 + 可被 BearSSL 解析后写 /ca.pem；先写临时文件再改名，
 * 避免半截文件导致下次启动校验失败。
 */
void handleOpenCodeGoCaSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    auto sendErr = [&](int code, const String& message) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = message;
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(code, "application/json", json);
    };

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        sendErr(HTTP_CODE_BAD_REQUEST, "Missing JSON body");
        return;
    }

    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, webserver->raw().arg("plain"));
    if (err) {
        sendErr(HTTP_CODE_BAD_REQUEST, "Invalid JSON");
        return;
    }

    // pem 字段可能是空串/null
    if (!ddoc["pem"].is<const char*>()) {
        sendErr(HTTP_CODE_BAD_REQUEST, "pem field is required");
        return;
    }

    String pem = ddoc["pem"].as<String>();
    pem.trim();
    if (pem.length() == 0) {
        sendErr(HTTP_CODE_BAD_REQUEST, "pem is empty");
        return;
    }
    if (pem.length() > OC_CA_MAX_BYTES) {
        sendErr(HTTP_CODE_BAD_REQUEST, "pem too large (max 8192 bytes)");
        return;
    }
    if (pem.indexOf("-----BEGIN CERTIFICATE-----") < 0 || pem.indexOf("-----END CERTIFICATE-----") < 0) {
        sendErr(HTTP_CODE_BAD_REQUEST, "pem missing BEGIN/END CERTIFICATE markers");
        return;
    }

    // BearSSL 预解析校验：构造 X509List，BearSSL 对解析不了的 PEM 会静默
    // 忽略，因此锚计数为 0 即视为无效直接拒绝
    {
        BearSSL::X509List testList;
        testList.append(pem.c_str());
        if (testList.getCount() == 0) {
            sendErr(HTTP_CODE_BAD_REQUEST, "pem parse failed (no valid certificate found)");
            Logger::warn("CA PEM rejected: BearSSL parse failed", "API");
            return;
        }
    }

    // 先写临时文件再改名，避免断电/半写导致 /ca.pem 损坏
    if (LittleFS.exists(OC_CA_TMP_PATH)) {
        LittleFS.remove(OC_CA_TMP_PATH);
    }
    File tmp = LittleFS.open(OC_CA_TMP_PATH, "w");
    if (!tmp) {
        sendErr(HTTP_CODE_INTERNAL_ERROR, "Failed to create temp file");
        return;
    }
    size_t written = tmp.print(pem);
    tmp.close();
    if (written != pem.length()) {
        LittleFS.remove(OC_CA_TMP_PATH);
        sendErr(HTTP_CODE_INTERNAL_ERROR, "Failed to write temp file");
        return;
    }
    if (LittleFS.exists(OC_CA_PATH)) {
        LittleFS.remove(OC_CA_PATH);
    }
    if (!LittleFS.rename(OC_CA_TMP_PATH, OC_CA_PATH)) {
        LittleFS.remove(OC_CA_TMP_PATH);
        sendErr(HTTP_CODE_INTERNAL_ERROR, "Failed to commit /ca.pem");
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["bytes"] = pem.length();
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info(("Custom TLS CA saved: /ca.pem (" + String(pem.length()) + " bytes)").c_str(), "API");
}

/**
 * @brief DELETE /api/v1/opencodego/ca — 删除 /ca.pem，回退内置 GTS Root R4
 */
void handleOpenCodeGoCaDelete(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (LittleFS.exists(OC_CA_PATH) && !LittleFS.remove(OC_CA_PATH)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to remove /ca.pem";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["source"] = "none";
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("Custom TLS CA deleted, verification disabled", "API");
}

