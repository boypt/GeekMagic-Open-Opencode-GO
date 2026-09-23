// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OpenCode Go 用量查询客户端（移植自 sd2-opencode-go-balance/src/OpenCodeGoClient.h）
// 接口: GET https://<host><path>
// Authorization: Bearer <API Key>（Anthropic 兼容 Key）
// 头文件实现，仿旧项目同构；ESP8266 BearSSL WiFiClientSecure + GTS Root R4 验签。

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <Esp.h>

#include "opencodego/cert.h"

struct OpenCodeGoWindow {
    bool present = false;  // usage.<key> 是否存在
    bool valid = true;     // status != "invalid"
    int percent = 0;       // 已用百分比
    String status;
    String resetsAt;       // ISO8601 时间

    // 与 cc-switch 提取器一致：剩余 = max(100 - percent, 0)
    int remaining() const {
        int r = 100 - percent;
        return r > 0 ? r : 0;
    }
};

struct OpenCodeGoUsage {
    bool ok = false;       // 请求 + 解析是否成功
    int http_code = 0;
    OpenCodeGoWindow rolling;  // 5 小时
    OpenCodeGoWindow weekly;   // 周额度
    OpenCodeGoWindow monthly;  // 月额度
    String error;              // 错误描述
};

namespace {

// 会话标识头：x-opencode-session: sd2-opencode-go-balance-<chipId>
String openCodeSessionId() {
    static String id;
    if (id.length() == 0) {
        id = "sd2-opencode-go-balance-" + String(ESP.getChipId(), HEX);
    }
    return id;
}

void parseWindow(JsonObject obj, OpenCodeGoWindow& out) {
    if (obj.isNull()) {
        return;
    }
    out.present = true;
    out.status = obj["status"] | "";
    out.percent = obj["percent"] | 0;
    out.resetsAt = obj["resetsAt"] | "";
    out.valid = out.status != "invalid";
}

bool isRetryableCode(int c) {
    // 401 = Key 无效；403 = Key 有效但没有 Go 订阅，重试无意义
    if (c == 401 || c == 403) return false;
    // 其余均可重试：含 0（建连失败/响应不完整）/429/5xx/解析失败
    return true;
}

// 轻量响应读取：拆 header/body、解析状态码、解码 chunked（同 sd2-common readHttpResponse）。
// 单缓冲实现：所有处理在同一 String 上原地进行（remove 去头、chunk 头原地压缩），
// 避免重复分配 large String，把请求路径 String 峰值压到单个 ~4-5KB 缓冲。
void dechunkInPlace(String& s) {
    size_t w = 0, i = 0;
    const size_t L = s.length();
    while (i < L) {
        int nl = s.indexOf("\r\n", i);
        if (nl < 0) break;
        // chunk size 行：直接 strtol 解析行首十六进制（前导空格容忍，含 ";ext" 扩展自动截断）
        long sz = strtol(s.c_str() + i, nullptr, 16);
        if (sz <= 0 || static_cast<size_t>(nl + 2 + sz) > L) break;
        const size_t src = static_cast<size_t>(nl) + 2;
        for (long k = 0; k < sz; k++) {
            s[w++] = s[src + static_cast<size_t>(k)];  // w <= src，原地前移不越写
        }
        i = src + static_cast<size_t>(sz) + 2;
    }
    s.remove(w);  // 截掉末尾残留（含最后的 "0\r\n\r\n" 尾块头部）
}

int parseStatusCodeLine(const String& statusLine) {
    int sp2 = statusLine.indexOf(' ');
    int sp3 = statusLine.indexOf(' ', sp2 + 1);
    if (sp2 > 0 && sp3 > sp2) {
        return statusLine.substring(sp2 + 1, sp3).toInt();
    }
    return 0;
}

struct OpenCodeGoHttpResponse {
    bool complete = false;
    int httpCode = 0;
    String body;
};

OpenCodeGoHttpResponse readHttpResponse(WiFiClientSecure& client, uint32_t timeoutMs) {
    OpenCodeGoHttpResponse out;
    String& raw = out.body;  // 单缓冲：读入/去头/dechunk 都在这份 String 上
    raw.reserve(4096);
    uint8_t buf[128];
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        while (client.available()) {
            int n = client.read(buf, sizeof(buf));
            if (n <= 0) break;
            raw.concat(reinterpret_cast<const char*>(buf), static_cast<unsigned int>(n));
        }
        if (!client.connected() && client.available() == 0) break;
        delay(1);
        ESP.wdtFeed();
    }

    int hEnd = raw.indexOf("\r\n\r\n");
    int sepLen = 4;
    if (hEnd < 0) {
        hEnd = raw.indexOf("\n\n");
        sepLen = 2;
    }
    if (hEnd < 0) {
        return out;  // 不完整响应
    }

    // header 只在这一小块临时 String 上解析（几百 B），body 保留在 raw 中
    String headers = raw.substring(0, hEnd);
    int nl = headers.indexOf('\n');
    String statusLine = (nl >= 0) ? headers.substring(0, nl) : headers;
    statusLine.trim();
    out.httpCode = parseStatusCodeLine(statusLine);
    bool chunked = headers.indexOf("chunked") >= 0;
    raw.remove(0, static_cast<unsigned int>(hEnd + sepLen));
    if (chunked) {
        dechunkInPlace(raw);
    }
    out.complete = true;
    return out;
}

// 单次请求：host/path/apiKey/verifyTlsCert 由参数传入
bool fetchOpenCodeGoUsageOnce(OpenCodeGoUsage& out, const char* host, const char* path,
                              const char* apiKey, bool verifyTlsCert, uint32_t timeout_ms) {
    BearSSL::X509List trust;
    WiFiClientSecure client;
    // TLS 缓冲区削减：BearSSL 默认 rx 16384 + tx 512 ≈ 17.3KB heap，改为
    // 4096/512（≈5KB），给 instruction cache/stack/JSON 解析留 heap 余量。
    // 说明：rx < 16384 依赖服务器支持 TLS MFLN (RFC 6066)；当前 OpenCode
    // 服务器已验证支持 MFLN。若切换 host 后握手失败（getLastSSLError 报
    // handshake/write error），说明对端不支持 MFLN：临时回退默认缓冲
    // （删掉本行，代价 heap +12KB，需同步减小其他内存占用）。
    client.setBufferSizes(4096, 512);
    if (verifyTlsCert) {
        trust.append(GTS_ROOT_R4_PEM);
        client.setTrustAnchors(&trust);
    } else {
        client.setInsecure();  // 仅应急：证书链异常环境
    }
    client.setTimeout(timeout_ms);  // Stream 超时单位为 ms（勿除以 1000）

    uint32_t t0 = millis();
    if (!client.connect(host, 443)) {
        char sslErr[64] = {0};
        client.getLastSSLError(sslErr, sizeof(sslErr));
        Serial.printf("TLS connect failed after %lu ms, ssl=%s\n",
                      static_cast<unsigned long>(millis() - t0), sslErr);
        out.error = "Network error";
        client.stop();
        return false;
    }

    String req = String("GET ") + path + " HTTP/1.1\r\n" +
                 "Host: " + host + "\r\n" +
                 "Authorization: Bearer " + apiKey + "\r\n" +
                 "User-Agent: cc-switch/1.0\r\n" +
                 "Accept: application/json\r\n" +
                 "x-opencode-session: " + openCodeSessionId() + "\r\n" +
                 "Connection: close\r\n\r\n";
    client.print(req);
    client.flush();

    OpenCodeGoHttpResponse resp = readHttpResponse(client, timeout_ms);
    client.stop();
    if (!resp.complete) {
        out.error = "Network error";
        return false;
    }

    out.http_code = resp.httpCode;
    Serial.printf("HTTP %d, body %u B\n", out.http_code, static_cast<unsigned>(resp.body.length()));

    // http_code==0：建连失败/响应不完整，视为可重试网络错误
    if (out.http_code == 0) {
        out.error = "Network error";
        return false;
    }
    if (resp.body.length() == 0) {
        out.error = "Empty response";
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp.body);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        out.error = "Response error";
        return false;
    }

    if (out.http_code == 200) {
        JsonObject usage = doc["usage"].as<JsonObject>();
        if (!usage.isNull()) {
            parseWindow(usage["rolling"].as<JsonObject>(), out.rolling);
            parseWindow(usage["weekly"].as<JsonObject>(), out.weekly);
            parseWindow(usage["monthly"].as<JsonObject>(), out.monthly);
        }

        // 顶层三窗口全缺视为失败
        if (!out.rolling.present && !out.weekly.present && !out.monthly.present) {
            out.error = "Response error";
            return false;
        }
        out.ok = true;
        return true;
    }

    String apiErr = doc["error"]["message"] | "";
    if (apiErr.length() > 0) {
        Serial.printf("API error: %s\n", apiErr.c_str());
    }

    // 401 = Key 无效；403 = Key 有效但没有 Go 订阅
    if (out.http_code == 401) {
        out.error = "Bad API key";
    } else if (out.http_code == 403) {
        out.error = "No Go plan";
    } else if (out.http_code == 429) {
        out.error = "HTTP 429";
    } else if (out.http_code >= 500) {
        out.error = "Server error";
    } else {
        out.error = "HTTP " + String(out.http_code);
    }
    return false;
}

}  // namespace

// 带重试的获取入口：指数退避（等待 = retry_interval_ms * attempt，上限 30000ms
// + random(0,1000) 抖动，attempt 从 1 计），直到成功或用完 max_attempts 次。
// 401/403 立即返回；http_code==0 视为可重试网络错误（保留 "Network error"）。
// 等待用 200ms 分片 yield + WiFi 状态检查；attempt 前掉线则 WiFi.reconnect()。
inline bool fetchOpenCodeGoUsage(OpenCodeGoUsage& out, const char* host, const char* path,
                                 const char* apiKey, bool verifyTlsCert,
                                 uint32_t timeout_ms = 15000,
                                 uint32_t retry_interval_ms = 10000,
                                 int max_attempts = 3) {
    bool ok = false;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        out = OpenCodeGoUsage();
        // 每次 attempt 开始前检查 WiFi，掉线则尝试重连
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            delay(500);
            if (WiFi.status() != WL_CONNECTED) {
                out.error = "WiFi lost";
                Serial.printf("Fetch attempt %d/%d failed: %s (HTTP %d)\n",
                              attempt, max_attempts, out.error.c_str(), out.http_code);
                if (attempt < max_attempts) {
                    uint32_t wait_ms = retry_interval_ms * static_cast<uint32_t>(attempt);
                    if (wait_ms > 30000) wait_ms = 30000;
                    wait_ms += random(0, 1000);
                    Serial.printf("Retry in %lu ms\n", static_cast<unsigned long>(wait_ms));
                    for (uint32_t waited = 0; waited < wait_ms; waited += 200) {
                        yield();
                        delay(200);
                        if (WiFi.status() != WL_CONNECTED) break;
                    }
                }
                continue;
            }
        }
        ok = fetchOpenCodeGoUsageOnce(out, host, path, apiKey, verifyTlsCert, timeout_ms);
        if (ok) {
            if (attempt > 1) {
                Serial.printf("Fetch recovered on attempt %d\n", attempt);
            }
            return true;
        }
        Serial.printf("Fetch attempt %d/%d failed: %s (HTTP %d)\n",
                      attempt, max_attempts, out.error.c_str(), out.http_code);
        // 鉴权/订阅类错误重试无意义，直接返回
        if (!isRetryableCode(out.http_code)) return false;
        if (attempt < max_attempts) {
            // 指数退避：等待 = retry_interval_ms * attempt，上限 30000ms + 抖动
            uint32_t wait_ms = retry_interval_ms * static_cast<uint32_t>(attempt);
            if (wait_ms > 30000) wait_ms = 30000;
            wait_ms += random(0, 1000);
            Serial.printf("Retry in %lu ms\n", static_cast<unsigned long>(wait_ms));
            // 分片等待：每 200ms 一片 yield()，片间检查 WiFi，掉线提前跳出
            for (uint32_t waited = 0; waited < wait_ms; waited += 200) {
                yield();
                delay(200);
                if (WiFi.status() != WL_CONNECTED) break;
            }
        }
    }
    return ok;
}
