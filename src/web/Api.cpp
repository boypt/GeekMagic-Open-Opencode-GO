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
#include <math.h>

#include "web/Webserver.h"
#include "web/Api.h"
#include "display/DisplayManager.h"
#include "display/SceneManager.h"
#include "display/NoticeScene.h"
#include "led/AmbientLight.h"
#include <Arduino_GFX_Library.h>
#include <WiFiClient.h>

/// 相册整幅尺寸：240x240 RGB565(LE)
static constexpr size_t ALBUM_IMG_BYTES = 240UL * 240UL * 2UL;

#include "config/ConfigManager.h"
#include "wireless/WiFiManager.h"
#include "ntp/NTPClient.h"
#include "opencodego/UsageManager.h"
#include "opencodego/StockData.h"

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
void handleDisplaySleepGet(Webserver* webserver);
void handleDisplaySleepSet(Webserver* webserver);
void handleDeleteGif(Webserver* webserver);
static void handleStockGet(Webserver* webserver);
static void handleStockSet(Webserver* webserver);
static void handleNoticeGet(Webserver* webserver);
static void handleNoticeSet(Webserver* webserver);
static void handleNoticeImageUpload(Webserver* webserver);
static void handleNoticeImageDone(Webserver* webserver);
static constexpr int WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr size_t NTP_CONFIG_DOC_SIZE = 512;
static constexpr int BEARER_LEN = 7;
/// ESP8266WebServer 的 HTTP_CODE_* 枚举里没有 409（只存在于 ESP8266HTTPClient，
/// 不值得为它把那个头文件拉进来），自备字面量。
static constexpr int HTTP_CODE_CONFLICT_409 = 409;

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

    // @openapi {get} /display/sleep version=v1 group=Display summary="Get display sleep state" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/display/sleep", HTTP_GET, [webserver]() { handleDisplaySleepGet(webserver); });

    // @openapi {post} /display/sleep version=v1 group=Display summary="Set display sleep state and suspend ambient light" requiresAuth=true
    // requestBody=application/json requestBodySchema=on:boolean example={"on":true}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/display/sleep", HTTP_POST, [webserver]() { handleDisplaySleepSet(webserver); });

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

    // @openapi {get} /balance version=v1 group=Balance summary="Get latest pushed balance lines" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/balance", HTTP_GET, [webserver]() { handleBalanceGet(webserver); });

    // @openapi {post} /balance version=v1 group=Balance summary="Push balance lines from host (1..3 strings plus optional status)" requiresAuth=true
    // requestBody=application/json requestBodySchema=lines:array,status:string
    // example={"lines":["5H 42%","WK 61%","MO 33%"],"status":"SYNC OK"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/balance", HTTP_POST, [webserver]() { handleBalanceSet(webserver); });

    // @openapi {get} /stock version=v1 group=Stock summary="Get latest pushed stock rows" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/stock", HTTP_GET, [webserver]() { handleStockGet(webserver); });

    // @openapi {post} /stock version=v1 group=Stock summary="Push stock rows from host" requiresAuth=true
    // requestBody=application/json requestBodySchema=rows:array
    // example={"rows":[{"name":"600519","change":1.23},{"name":"AAPL","change":-0.85}]}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/stock", HTTP_POST, [webserver]() { handleStockSet(webserver); });

    // @openapi {get} /notice version=v1 group=Notice summary="Get temporary notice overlay state (text or image mode)" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/notice", HTTP_GET, [webserver]() { handleNoticeGet(webserver); });

    // @openapi {post} /notice version=v1 group=Notice summary="Show a temporary notice overlay (big icon + ASCII body, auto-returns to the previous scene)" requiresAuth=true
    // requestBody=application/json requestBodySchema=level:string,seconds:integer,text:string
    // example={"level":"warning","seconds":10,"text":"Disk almost full"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/notice", HTTP_POST, [webserver]() { handleNoticeSet(webserver); });

    // @openapi {post} /notice/image version=v1 group=Notice summary="Show a temporary notice overlay drawing one streamed frame (240x240 RGB565, multipart, ?seconds=1..30)" requiresAuth=true
    // requestBody=multipart/form-data responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/notice/image", HTTP_POST, [webserver]() { handleNoticeImageDone(webserver); },
        [webserver]() { handleNoticeImageUpload(webserver); });

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

// ---- 16bpp 推帧共用的行组装器（live 与 notice/image 两条流共用一份实现）----
// 组装整行 480B → R/B 字段交换补偿 → writePixels 写屏；行状态由调用方持有，
// 绝不复制第二份 R/B 交换逻辑（AGENTS.md 硬件章：BGR 面板三处口径必须一致）。
static void pushFrameBytes(uint8_t* row, int& rowY, int& rowFill, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (rowY >= 240) {
            return;  // 超出整幅的余数忽略
        }

        row[rowFill++] = data[i];

        if (rowFill == 480) {
            auto* tft = reinterpret_cast<Arduino_TFT*>(DisplayManager::getGfx());
            tft->startWrite();
            tft->writeAddrWindow(0, rowY, 240, 1);
            // 同 AlbumScene：标准 RGB565(LE) 直推在本面板红蓝互换，做 R/B 字段交换补偿
            auto* px = reinterpret_cast<uint16_t*>(row);
            for (int p = 0; p < 240; p++) {
                const uint16_t v = px[p];
                px[p] = static_cast<uint16_t>(((v & 0x001FU) << 11) | (v & 0x07E0U) | ((v & 0xF800U) >> 11));
            }
            tft->writePixels(reinterpret_cast<uint16_t*>(row), 240);
            tft->endWrite();

            rowFill = 0;
            rowY++;
        }
    }
}

// ---- 抑制期的半帧复位：把行状态归零，恢复绘制后下一帧即整行对齐 ----
// （抑制期间不可能凑满一行，所以最多只残留半行）
static void resetSuppressedRowState(int& rowY, int& rowFill) {
    if (rowY != 0 || rowFill != 0) {
        rowY = 0;
        rowFill = 0;
    }
}

// ---- live 实时推图：multipart 分块流式直绘（不落盘、不整帧进 RAM）----
// 帧 = 240x240 RGB565(LE) 115200B；行缓冲 480B 组装整行后立即写屏
static bool s_liveAuthed = true;
static bool s_liveAborted = false;
static int s_liveRowY = 0;
static int s_liveRowFill = 0;
// alignas(4) 不是洁癖：pushFrameBytes() 用 reinterpret_cast<uint16_t*> 对本缓冲做
// 16 位读写（ESP8266 上奇地址 16 位访问直接 Exception 9 LoadStoreError），而链接器
// 只会把它放在前面静态量结束处、不保证 2/4 字节对齐 —— 相邻多一个单字节 bool 就
// 可能把它挤到奇地址。任何会被 16/32 位视图访问的缓冲都必须显式 alignas(4)。
alignas(4) static uint8_t s_liveRow[480];

static void livePushBytes(const uint8_t* data, size_t len) {
    // 只在 live 场景落笔：通知覆盖层（或任何其它场景）在场时照收字节但丢弃，
    // 否则推帧会把画面直接画穿到别人的屏上。
    if (strcmp(SceneManager::currentName(), "live") != 0) {
        resetSuppressedRowState(s_liveRowY, s_liveRowFill);
        return;
    }

    pushFrameBytes(s_liveRow, s_liveRowY, s_liveRowFill, data, len);
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

void handleDisplaySleepGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["sleeping"] = DisplayManager::isSleeping();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

void handleDisplaySleepSet(Webserver* webserver) {
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

    JsonDocument ddoc;
    const DeserializationError err = deserializeJson(ddoc, webserver->raw().arg("plain"));
    if (err || !ddoc["on"].is<bool>()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON or missing on";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    const bool on = ddoc["on"].as<bool>();
    const bool wasSleeping = DisplayManager::isSleeping();
    if (on) {
        DisplayManager::setSleeping(true);
        AmbientLight::suspend();
    } else if (wasSleeping) {
        // 背光先恢复，氛围灯再恢复，最后让当前场景整屏 enter()。
        DisplayManager::setSleeping(false);
        AmbientLight::resume();
        SceneManager::redrawCurrent();
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["sleeping"] = DisplayManager::isSleeping();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
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

// 场景名是否存在：SceneManager::find() 是私有的，用公开的场景列表自检。
// switchTo() 对「未知场景」和「被 notice 闸门拦下」都只返回 false，
// 两者语义不同（400 vs 409），必须先区分开。
static bool isKnownScene(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    for (int i = 0; i < SceneManager::sceneCount(); i++) {
        if (strcmp(SceneManager::sceneNameAt(i), name) == 0) {
            return true;
        }
    }

    return false;
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
    // notice 闸门拦下的切换不是「未知场景」，是「被临时通知覆盖层挡着」→ 回 409。
    // isKnownScene() 先把未知场景摘出去：它同样是 switchTo 返回 false，但属 400。
    const bool blockedByNotice = !ok && isKnownScene(scene) && NoticeScene::isActive();

    JsonDocument resp;
    resp["status"] = ok ? "ok" : "error";
    resp["current"] = SceneManager::currentName();

    if (blockedByNotice) {
        char message[96];
        snprintf(message, sizeof(message), "notice active (%us remaining), scene switch ignored",
                 static_cast<unsigned>(NoticeScene::remainingSeconds()));
        resp["message"] = message;
    } else if (!ok) {
        resp["message"] = "scene switch failed (unknown scene or scene refused)";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(ok ? HTTP_CODE_OK : (blockedByNotice ? HTTP_CODE_CONFLICT_409 : HTTP_CODE_BAD_REQUEST),
                          "application/json", jsonOut);

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
 * @brief POST /api/v1/balance — 上位机推送额度文本（推送契约，与上位机脚本共享）
 *
 * body {"lines":["l1","l2","l3"], "status":"可选状态行"}（body < 1024B）
 * lines 1..3 个字符串，渲染到三行额度槽位（超宽截断显示不断言）；
 * status 缺省时状态行显示 UPD HH:MM。成功 → 200 {"ok":true} 并立即重绘。
 */
static constexpr size_t BALANCE_BODY_MAX = 1024;

void handleBalanceSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    auto sendErr = [&](int code, const char* message) {
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

    String body = webserver->raw().arg("plain");
    if (body.length() >= BALANCE_BODY_MAX) {
        sendErr(HTTP_CODE_BAD_REQUEST, "body too large (max 1023 bytes)");
        return;
    }

    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);
    if (err) {
        sendErr(HTTP_CODE_BAD_REQUEST, "Invalid JSON");
        return;
    }

    bool hasStatus = false;
    const char* status = "";
    if (!ddoc["status"].isNull()) {
        if (!ddoc["status"].is<const char*>()) {
            sendErr(HTTP_CODE_BAD_REQUEST, "status must be a string");
            return;
        }
        status = ddoc["status"].as<const char*>();
        hasStatus = (status != nullptr && status[0] != '\0');
    }

    // 三段字段（labels / progress / resets）均可选：出现时须 1..3 且彼此等长，
    // 元素可 null。整行 lines 通道已退役——标签/百分比/重置分开推送，
    // 设备零语义照单渲染（缺省：标签回落 5H/WK./MO.、重置显示 "--"、进度空槽）。
    const char* labels[UsageManager::kBalanceLines] = {nullptr, nullptr, nullptr};
    const char* resets[UsageManager::kBalanceLines] = {nullptr, nullptr, nullptr};
    int progress[UsageManager::kBalanceLines] = {-1, -1, -1};

    auto parseStrings = [&](const char* field, const char** out, int& count) -> bool {
        count = 0;
        if (ddoc[field].isNull()) {
            return true;
        }
        const String badArray = String(field) + " must be an array of 1..3 strings or null";
        if (!ddoc[field].is<JsonArray>()) {
            sendErr(HTTP_CODE_BAD_REQUEST, badArray.c_str());
            return false;
        }
        JsonArray a = ddoc[field].as<JsonArray>();
        if (a.size() < 1 || a.size() > UsageManager::kBalanceLines) {
            sendErr(HTTP_CODE_BAD_REQUEST, badArray.c_str());
            return false;
        }
        for (JsonVariantConst v : a) {
            if (!v.isNull() && !v.is<const char*>()) {
                const String badEntry = String(field) + " entries must be string or null";
                sendErr(HTTP_CODE_BAD_REQUEST, badEntry.c_str());
                return false;
            }
        }
        uint8_t k = 0;
        for (JsonVariantConst v : a) {
            out[k++] = v.isNull() ? nullptr : v.as<const char*>();
        }
        count = static_cast<int>(a.size());
        return true;
    };

    auto parseProgress = [&](int* out, int& count) -> bool {
        count = 0;
        if (ddoc["progress"].isNull()) {
            return true;
        }
        if (!ddoc["progress"].is<JsonArray>()) {
            sendErr(HTTP_CODE_BAD_REQUEST, "progress must be an array of 1..3 ints (0..100) or null");
            return false;
        }
        JsonArray a = ddoc["progress"].as<JsonArray>();
        if (a.size() < 1 || a.size() > UsageManager::kBalanceLines) {
            sendErr(HTTP_CODE_BAD_REQUEST, "progress must be an array of 1..3 ints (0..100) or null");
            return false;
        }
        uint8_t k = 0;
        for (JsonVariantConst v : a) {
            if (v.isNull()) {
                out[k++] = -1;
                continue;
            }
            if (!v.is<int>()) {
                sendErr(HTTP_CODE_BAD_REQUEST, "progress entries must be int 0..100 or null");
                return false;
            }
            int p = v.as<int>();
            if (p < 0 || p > 100) {
                sendErr(HTTP_CODE_BAD_REQUEST, "progress entries must be int 0..100 or null");
                return false;
            }
            out[k++] = p;
        }
        count = static_cast<int>(a.size());
        return true;
    };

    int nLabels = 0;
    int nProgress = 0;
    int nResets = 0;
    if (!parseStrings("labels", labels, nLabels) || !parseProgress(progress, nProgress) ||
        !parseStrings("resets", resets, nResets)) {
        return;
    }
    uint8_t rowCount = 0;
    const int counts[3] = {nLabels, nProgress, nResets};
    for (int c : counts) {
        if (c == 0) {
            continue;  // 字段缺省
        }
        if (rowCount == 0) {
            rowCount = static_cast<uint8_t>(c);
        } else if (static_cast<int>(rowCount) != c) {
            sendErr(HTTP_CODE_BAD_REQUEST, "labels/progress/resets must have equal length");
            return;
        }
    }

    for (uint8_t r = 0; r < rowCount; r++) {
        UsageManager::setRowLabel(r, labels[r]);
        UsageManager::setRowProgress(r, progress[r]);
        UsageManager::setRowReset(r, resets[r]);
    }

    UsageManager::pushBalance(rowCount, status, hasStatus);

    // 同 live 推图：推送到达即接管 balance 场景（退场重绘契约）；已在 balance 则不动
    if (strcmp(SceneManager::currentName(), "balance") != 0) {
        SceneManager::switchTo("balance");
    }

    JsonDocument doc;
    doc["ok"] = true;
    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("Balance push accepted", "API");
}

/**
 * @brief GET /api/v1/balance — 查询最近一次推送
 *
 * → {"labels":[...],"progress":[...],"resets":[...],"status":"...",
 *    "ts":<epoch 或 0>,"age_s":<秒>}
 * 空串 / null 表示该行无数据；未同步过 NTP 时 ts=0。
 */
void handleBalanceGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray labels = doc["labels"].to<JsonArray>();
    JsonArray progress = doc["progress"].to<JsonArray>();
    JsonArray resets = doc["resets"].to<JsonArray>();
    for (uint8_t i = 0; i < UsageManager::kBalanceLines; i++) {
        labels.add(UsageManager::rowLabel(i));
        const int v = UsageManager::rowProgress(i);
        if (v < 0) {
            progress.add(nullptr);
        } else {
            progress.add(v);
        }
        resets.add(UsageManager::rowReset(i));
    }
    doc["status"] = UsageManager::statusText();
    doc["ts"] = static_cast<unsigned long>(UsageManager::pushEpoch());
    doc["age_s"] = static_cast<unsigned long>(UsageManager::pushAgeSec());

    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

// 上界按「5 行行情 + 3 槽额度」的最坏 payload 留余量：rows 约 140B、balance
// 包装约 60B，留给未来可选字段的余量仍在 1KB 出头；1280 让宿主还能顺带塞
// labels 之类的附加字段而不撞 413。
static constexpr size_t STOCK_BODY_MAX = 1280;

static void sendStockError(Webserver* webserver, int code, const char* message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;

    char json[128];
    serializeJson(doc, json, sizeof(json));
    setCorsHeaders(webserver);
    webserver->raw().send(code, "application/json", json);
}

static void handleStockGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray rows = doc["rows"].to<JsonArray>();
    for (uint8_t i = 0; i < StockData::rowCount(); i++) {
        StockData::Row row;
        if (!StockData::getRow(i, &row)) {
            continue;
        }

        JsonObject rowJson = rows.add<JsonObject>();
        rowJson["name"] = row.name;
        rowJson["change"] = row.change;
    }
    // 额度是可选附属数据：没推过就整个键省略，宿主能靠「键是否存在」区分
    // 「本次没推」与「推了空数组要求清空」两种语义。
    if (StockData::hasQuota()) {
        JsonArray progress = doc["balance"]["progress"].to<JsonArray>();
        for (uint8_t i = 0; i < StockData::QUOTA_MAX; i++) {
            const int8_t v = StockData::quota(i);
            if (v < 0) {
                progress.add(nullptr);
            } else {
                progress.add(v);
            }
        }
    }
    doc["ts"] = static_cast<unsigned long>(StockData::updatedAtMs());
    doc["age_s"] = static_cast<unsigned long>(StockData::ageSeconds());

    // 上界估算：5 行 {"name":"600519","change":-1.23} ≈ 5*28=140 + 3 个百分比
    // 与 balance 包装 ≈ 60 + ts/age_s/rows 外壳 ≈ 40，余量充足。
    char json[512];
    serializeJson(doc, json, sizeof(json));
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

static void handleStockSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "Missing JSON body");
        return;
    }

    const String& body = webserver->raw().arg("plain");
    if (body.length() >= STOCK_BODY_MAX) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "body too large (max 1279 bytes)");
        return;
    }

    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);
    if (err) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "Invalid JSON");
        return;
    }

    if (!ddoc["rows"].is<JsonArray>()) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "rows must be an array of 0..5 objects");
        return;
    }

    JsonArray inputRows = ddoc["rows"].as<JsonArray>();
    if (inputRows.size() > StockData::MAX_ROWS) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "rows must contain at most 5 objects");
        return;
    }

    // 可选额度块：先整体校验（含元素类型/范围），确认无误后才允许写 StockData，
    // 免得校验到一半失败却已经污染了屏上状态。
    // quotaPresent=false 表示 payload 里根本没有 balance 键 → 额度状态完全不动
    //（取不到额度的宿主继续沿用上次推送的条，而不是被空数据抹掉）。
    int8_t quota[StockData::QUOTA_MAX] = {-1, -1, -1};
    uint8_t quotaCount = 0;
    // 「键缺失」与「键存在但不是对象」必须分开：isNull() 对缺失与显式 null 同为真，
    // 所以先用它判缺失，再用 as<JsonObjectConst>() + isNull() 判类型（沿用本文件行校验的口径）。
    bool quotaPresent = !ddoc["balance"].isNull();
    JsonObjectConst balance = ddoc["balance"].as<JsonObjectConst>();
    if (quotaPresent && balance.isNull()) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "balance must be an object");
        return;
    }
    if (quotaPresent) {
        if (!balance["progress"].is<JsonArrayConst>()) {
            sendStockError(webserver, HTTP_CODE_BAD_REQUEST,
                           "balance.progress must be an array of 0..3 null|0..100");
            return;
        }
        JsonArrayConst progress = balance["progress"].as<JsonArrayConst>();
        if (progress.size() > StockData::QUOTA_MAX) {
            sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "balance.progress must contain at most 3 items");
            return;
        }
        quotaCount = static_cast<uint8_t>(progress.size());
        for (uint8_t i = 0; i < quotaCount; i++) {
            JsonVariantConst item = progress[i];
            if (item.isNull()) {
                quota[i] = -1;
                continue;
            }
            // is<int>() 对 58 为真、对 58.5 / "58" / 越界整数均为假，正好是本处要的整数判定
            if (!item.is<int>()) {
                sendStockError(webserver, HTTP_CODE_BAD_REQUEST,
                               "balance.progress items must be null or an integer 0..100");
                return;
            }
            const int value = item.as<int>();
            if (value < 0 || value > 100) {
                sendStockError(webserver, HTTP_CODE_BAD_REQUEST,
                               "balance.progress items must be null or an integer 0..100");
                return;
            }
            quota[i] = static_cast<int8_t>(value);
        }
    }

    StockData::Row rows[StockData::MAX_ROWS] = {};
    uint8_t rowCount = static_cast<uint8_t>(inputRows.size());
    for (uint8_t i = 0; i < rowCount; i++) {
        // 用 as<JsonObjectConst>() + isNull() 判定元素类型：is<JsonObject>() 作用在
        // JsonVariantConst 上会因 ArduinoJson 版本差异把真实对象判为非对象（→ 400）。
        JsonObjectConst inputRow = inputRows[i].as<JsonObjectConst>();
        if (inputRow.isNull()) {
            sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "each row must be an object");
            return;
        }
        if (!inputRow["name"].is<const char*>()) {
            sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "row name must be a string");
            return;
        }

        const char* name = inputRow["name"].as<const char*>();
        const size_t nameLength = name == nullptr ? 0 : strlen(name);
        if (nameLength < 1 || nameLength >= StockData::NAME_MAX) {
            sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "row name must be 1..9 printable ASCII bytes");
            return;
        }
        for (size_t j = 0; j < nameLength; j++) {
            const unsigned char ch = static_cast<unsigned char>(name[j]);
            if (ch < 0x20 || ch > 0x7E) {
                sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "row name must be 1..9 printable ASCII bytes");
                return;
            }
        }

        JsonVariantConst changeValue = inputRow["change"];
        if (!changeValue.is<int>() && !changeValue.is<float>() && !changeValue.is<double>()) {
            sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "row change must be a finite number");
            return;
        }

        const float change = changeValue.as<float>();
        if (!isfinite(change) || change < -100000.0f || change > 100000.0f) {
            sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "row change must be finite and within -100000..100000");
            return;
        }

        memcpy(rows[i].name, name, nameLength);
        rows[i].name[nameLength] = '\0';
        rows[i].change = change;
    }

    if (rowCount == 0) {
        StockData::clear();
    } else if (!StockData::setRows(rows, rowCount)) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "invalid stock rows");
        return;
    }
    // 行写完再写额度：额度是附属数据，行失效时不该先动它。
    // quotaPresent 为 false 时完全不调用 setQuota，保留上次的条。
    if (quotaPresent && !StockData::setQuota(quota, quotaCount)) {
        sendStockError(webserver, HTTP_CODE_BAD_REQUEST, "invalid balance progress");
        return;
    }

    DisplayManager::requestFullRedraw();
    if (strcmp(SceneManager::currentName(), "stock") != 0) {
        SceneManager::switchTo("stock");
    }

    JsonDocument doc;
    doc["ok"] = true;
    char json[32];
    serializeJson(doc, json, sizeof(json));
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

// ===========================================================================
// notice —— 临时通知覆盖层（API 接线层）
// 场景本体在 Scenes.cpp（只读），这里只做三件事：校验、预载（prepareText /
// prepareImage）、激活（switchTo("notice")）。预载 ≠ 激活，顺序不可颠倒：
// 全部字段校验通过之前一个字节都不许写进预载缓冲。
// 期间不理会其他场景切换请求由 SceneManager::switchTo() 的闸门保证。
// ===========================================================================

static constexpr size_t NOTICE_BODY_MAX = 1024;  // 与其它端点同惯例
static constexpr size_t NOTICE_TEXT_MAX = NoticeScene::kTextCap - 1;  // 199 字节正文上限

static const char* const kNoticeLevelNames[3] = {"info", "warning", "critical"};

static void sendNoticeError(Webserver* webserver, int code, const char* message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;

    char json[160];
    serializeJson(doc, json, sizeof(json));
    setCorsHeaders(webserver);
    webserver->raw().send(code, "application/json", json);
}

/// 大小写不敏感的 ASCII 比较（不建 String，坑 #7）
static auto asciiLowerEq(const char* a, const char* b) -> bool {
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        i++;
    }
    // 长度不同（一边先见 NUL）也判不等
    return a[i] == '\0' && b[i] == '\0';
}

/// level：大小写不敏感接受 info / warning / critical，其余一律 false
static auto parseNoticeLevel(const char* value, NoticeScene::Level& out) -> bool {
    if (value == nullptr) {
        return false;
    }
    if (asciiLowerEq(value, "info")) {
        out = NoticeScene::LevelInfo;
        return true;
    }
    if (asciiLowerEq(value, "warning")) {
        out = NoticeScene::LevelWarning;
        return true;
    }
    if (asciiLowerEq(value, "critical")) {
        out = NoticeScene::LevelCritical;
        return true;
    }
    return false;
}

/// seconds：只接受 1..kMaxSeconds 的**纯十进制整数**（无符号、无空白、无小数点、
/// 不接受 "5" 这类字符串语义），越界/负数/字符串一律 false。
static auto parseNoticeSeconds(const char* value, uint16_t& out) -> bool {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }

    long parsed = 0;
    for (const char* p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;  // 负号 / 小数点 / '+' / 空白 / 字母
        }
        parsed = parsed * 10 + (*p - '0');
        if (parsed > 100000L) {
            return false;  // 早就越界，不必继续累加
        }
    }

    if (parsed < 1 || parsed > static_cast<long>(NoticeScene::kMaxSeconds)) {
        return false;
    }

    out = static_cast<uint16_t>(parsed);
    return true;
}

/// 正文归一化 + 逐字节合法性校验（这是拦截 CJK 上屏的关键，见坑 #11）：
/// \r\n 归一成 \n，孤立 \r 视作 \n；只允许 0x20..0x7E 与 '\n'，
/// 其余（控制字符 / >=0x80 的汉字与 UTF-8 续字节）一律 false。
/// 返回 false = 字节非法；outLength == 0 = 合法但空（由调用方判 400）。
static auto normalizeNoticeText(const char* src, char* dst, size_t cap, size_t& outLength) -> bool {
    size_t written = 0;

    for (const char* p = src; p != nullptr && *p != '\0'; p++) {
        char ch = *p;

        if (ch == '\r') {
            if (*(p + 1) == '\n') {
                continue;  // \r\n → 由随后的 \n 落笔
            }
            ch = '\n';  // 孤立 \r 视作换行
        }

        if (ch != '\n') {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte > 0x7E) {
                return false;
            }
        }

        if (written + 1 >= cap) {
            return false;  // 超长：留不出末尾 NUL
        }
        dst[written++] = ch;
    }

    dst[written] = '\0';
    outLength = written;
    return true;
}

/**
 * @brief POST /api/v1/notice —— 文字通知（大图标 + ASCII 正文，1..30 秒后自动切回）
 * body: {"level":"info|warning|critical","seconds":5,"text":"..."}
 */
static void handleNoticeSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "Missing JSON body");
        return;
    }

    const String& body = webserver->raw().arg("plain");
    if (body.length() >= NOTICE_BODY_MAX) {
        sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "body too large (max 1023 bytes)");
        return;
    }

    JsonDocument ddoc;
    if (deserializeJson(ddoc, body)) {
        sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "Invalid JSON");
        return;
    }

    // ---- 字段校验（全部通过之前不预载任何字节）----
    // isUnbound() 判「键不存在」；键存在但类型/取值不对一律 400（含显式 null）
    NoticeScene::Level level = NoticeScene::LevelInfo;
    JsonVariantConst levelValue = ddoc["level"];
    if (!levelValue.isUnbound()) {
        if (!levelValue.is<const char*>() || !parseNoticeLevel(levelValue.as<const char*>(), level)) {
            sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "level must be info, warning or critical");
            return;
        }
    }

    uint16_t seconds = NoticeScene::kDefaultSeconds;
    JsonVariantConst secondsValue = ddoc["seconds"];
    if (!secondsValue.isUnbound()) {
        if (!secondsValue.is<int>()) {
            sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "seconds must be an integer 1..30");
            return;
        }
        const int raw = secondsValue.as<int>();
        if (raw < 1 || raw > static_cast<int>(NoticeScene::kMaxSeconds)) {
            sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "seconds must be an integer 1..30");
            return;
        }
        seconds = static_cast<uint16_t>(raw);
    }

    if (!ddoc["text"].is<const char*>()) {
        sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "text must be a string");
        return;
    }

    char text[NoticeScene::kTextCap];
    size_t textLength = 0;
    if (!normalizeNoticeText(ddoc["text"].as<const char*>(), text, NOTICE_TEXT_MAX + 1, textLength)) {
        sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST,
                        "text must be 1..199 printable ASCII bytes (\\n allowed, non-ASCII rejected)");
        return;
    }
    if (textLength == 0) {
        sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "text must not be empty");
        return;
    }

    // ---- 预载 → 激活 → 应答 ----
    NoticeScene::prepareText(level, text, seconds);
    SceneManager::switchTo("notice");

    JsonDocument doc;
    doc["ok"] = true;
    doc["scene"] = "notice";
    doc["seconds"] = seconds;
    doc["remaining"] = NoticeScene::remainingSeconds();
    char json[128];
    serializeJson(doc, json, sizeof(json));
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief GET /api/v1/notice —— 通知状态查询（脚本与 --check 用）
 */
static void handleNoticeGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    const bool active = NoticeScene::isActive();
    doc["active"] = active;

    if (active) {
        doc["mode"] = NoticeScene::isImageMode() ? "image" : "text";
        doc["level"] = kNoticeLevelNames[NoticeScene::level() % 3];
        doc["text"] = NoticeScene::text();
        doc["total"] = NoticeScene::totalSeconds();
        doc["remaining"] = NoticeScene::remainingSeconds();
    } else {
        doc["mode"] = "none";
        doc["total"] = 0;
        doc["remaining"] = 0;
    }

    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

// ---- notice/image：multipart 分块流式直绘（帧格式与 /album/live 完全一致）----
// 240x240 RGB565(LE) 115200B；480B 行缓冲逐行直绘、不保存。
// seconds 走 query 参数（multipart body 是裸帧，装不下 JSON）。
static bool s_noticeAuthed = false;
static bool s_noticeSecondsOk = false;
static bool s_noticeAborted = false;
static uint16_t s_noticeSeconds = NoticeScene::kDefaultSeconds;
static int s_noticeRowY = 0;
static int s_noticeRowFill = 0;
// 同 s_liveRow：会被 pushFrameBytes 以 uint16_t* 视图访问，必须 4 字节对齐
alignas(4) static uint8_t s_noticeRow[480];

static void noticeImagePushBytes(const uint8_t* data, size_t len) {
    // 只在 notice 场景落笔：倒计时到期切回别的场景后，残留的帧不许再画。
    if (strcmp(SceneManager::currentName(), "notice") != 0) {
        resetSuppressedRowState(s_noticeRowY, s_noticeRowFill);
        return;
    }

    pushFrameBytes(s_noticeRow, s_noticeRowY, s_noticeRowFill, data, len);
}

static void handleNoticeImageUpload(Webserver* webserver) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START) {
        s_noticeAuthed = validateBearerToken(webserver);
        s_noticeSecondsOk = false;
        s_noticeAborted = false;
        s_noticeSeconds = NoticeScene::kDefaultSeconds;
        s_noticeRowY = 0;
        s_noticeRowFill = 0;

        if (!s_noticeAuthed) {
            return;
        }

        // seconds 走 query 参数；缺省 kDefaultSeconds，非法值一帧都不画
        // （upload 阶段发不出应答，由 done 回调回 400）
        if (webserver->raw().hasArg("seconds")) {
            s_noticeSecondsOk = parseNoticeSeconds(webserver->raw().arg("seconds").c_str(), s_noticeSeconds);
        } else {
            s_noticeSecondsOk = true;
        }

        if (!s_noticeSecondsOk) {
            Logger::warn("Notice image: invalid seconds query", "API::Notice");
            return;
        }

        // 预载 ≠ 激活：先 prepareImage 再 switchTo
        NoticeScene::prepareImage(s_noticeSeconds);
        SceneManager::switchTo("notice");

        return;
    }

    if (!s_noticeAuthed || !s_noticeSecondsOk) {
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        noticeImagePushBytes(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        s_noticeAborted = true;
    }
}

static void handleNoticeImageDone(Webserver* webserver) {
    if (!s_noticeAuthed) {
        JsonDocument resp;
        resp["status"] = "error";
        resp["message"] = "Invalid or missing token";

        String jsonOut;
        serializeJson(resp, jsonOut);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", jsonOut);
        return;
    }

    if (!s_noticeSecondsOk) {
        sendNoticeError(webserver, HTTP_CODE_BAD_REQUEST, "seconds must be an integer 1..30");
        return;
    }

    JsonDocument resp;
    if (s_noticeAborted) {
        resp["status"] = "error";
        resp["message"] = "upload aborted";
        resp["rows"] = s_noticeRowY;

        char json[128];
        serializeJson(resp, json, sizeof(json));
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    resp["ok"] = true;
    resp["scene"] = "notice";
    resp["seconds"] = s_noticeSeconds;
    resp["rows"] = s_noticeRowY;

    char json[128];
    serializeJson(resp, json, sizeof(json));
    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

