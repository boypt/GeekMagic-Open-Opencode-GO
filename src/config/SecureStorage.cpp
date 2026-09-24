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

#include <array>
#include <cstring>
#include <EEPROM.h>
#include <Logger.h>
#include <ESP8266WiFi.h>
#include <user_interface.h>

// SHA-256（FIPS 180-4 单次哈希，仅用于 SecureStorage 密钥派生；
// 本地哈希、出站无关，自包含实现、不依赖任何 TLS/SSL 组件）
namespace {
inline uint32_t shaRotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

void sha256Once(const uint8_t* data, size_t len, uint8_t out[32]) {
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    size_t padded = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < padded; off += 64) {
        uint8_t block[64] = {0};
        size_t chunk = (len > off) ? len - off : 0;
        if (chunk > 64) chunk = 64;
        for (size_t i = 0; i < chunk; i++) block[i] = data[off + i];
        if (len >= off && len < off + 64) {
            block[len - off] = 0x80;  // padding 起点恰落在本块内
        }
        if (off + 64 >= padded) {
            for (int i = 0; i < 8; i++) block[56 + i] = static_cast<uint8_t>(bitLen >> (56 - i * 8));
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = shaRotr(w[i - 15], 7) ^ shaRotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = shaRotr(w[i - 2], 17) ^ shaRotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = shaRotr(e, 6) ^ shaRotr(e, 11) ^ shaRotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = shaRotr(a, 2) ^ shaRotr(a, 13) ^ shaRotr(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    for (int i = 0; i < 8; i++) {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}
}  // namespace

#include "config/SecureStorage.h"

static const std::array<uint8_t, 4> NVS_MAGIC = {{'N', 'V', 'S', '1'}};
static constexpr uint8_t LEN_SHIFT = 8;
static constexpr uint8_t LEN_HIGH_IDX = 4;
static constexpr uint8_t LEN_LOW_IDX = 5;
static constexpr uint8_t LEN_MASK = 0xFF;
static constexpr size_t KEY_LEN = 32;

static String kvSalt;

/**
 * @brief Set the public salt used in key derivation
 *
 * @param salt The public salt string
 */
void SecureStorage::setSalt(const String& salt) { kvSalt = salt; }

/**
 * @brief Derive the obfuscation key from device-unique parameters and public salt
 *
 * @param out32 Output buffer for the 32-byte derived key
 *
 * @returns void
 */
static void deriveKey(uint8_t* out32) {
    String mac = WiFi.macAddress();
    uint32_t chip = system_get_chip_id();

    String input = mac + String(chip) + kvSalt;

    uint8_t digest[KEY_LEN];
    sha256Once(reinterpret_cast<const uint8_t*>(input.c_str()), input.length(), digest);
    memcpy(out32, digest, KEY_LEN);
}

SecureStorage::SecureStorage(size_t eepromSize) : _eepromSize(eepromSize), _doc() {}

/**
 * @brief Initialize the EEPROM-backed NVS and load any existing data
 *
 *
 * @return true on success false on failure
 */
auto SecureStorage::begin() -> bool {
    Logger::info("EEPROM init start", "SecureStorage");

    EEPROM.begin(static_cast<int>(_eepromSize));
    _dirty = false;

    if (!loadToMemory()) {
        Logger::warn("No existing NVS data found, initializing new storage", "SecureStorage");
        _doc.clear();

        if (!flushToEEPROM()) {
            Logger::error("Failed to initialize NVS in EEPROM", "SecureStorage");
            _ready = false;

            return false;
        }
    }

    _ready = true;

    return true;
}

/**
 * @brief Load existing NVS data from EEPROM into the in-memory JSON document
 *
 * The EEPROM layout is:
 *
 * - bytes 0..3: magic ('N','V','S','x'), x represents version
 *
 * - bytes 4..5: payload length (big-endian)
 *
 * - bytes 6..(6+len-1): JSON payload
 *
 * @return true on success false on failure
 */
auto const SecureStorage::loadToMemory() -> bool {
    // 2 bytes length + 4 bytes magic
    const size_t headerSize = 6;

    if (_eepromSize <= headerSize) {
        return false;
    }

    for (size_t i = 0; i < 4; ++i) {
        if (EEPROM.read((int)i) != NVS_MAGIC[i]) {
            Logger::warn("NVS magic not found in EEPROM", "SecureStorage");

            return false;
        }
    }

    uint16_t len = (static_cast<uint16_t>(EEPROM.read(LEN_HIGH_IDX)) << LEN_SHIFT) |
                   static_cast<uint16_t>(EEPROM.read(LEN_LOW_IDX));
    size_t payloadMax = _eepromSize - headerSize;

    if (len == 0 || len > payloadMax) {
        Logger::warn("Invalid NVS length in EEPROM", "SecureStorage");

        return false;
    }

    char* buf = new char[len + 1];
    for (uint16_t i = 0; i < len; ++i) {
        buf[i] = static_cast<char>(EEPROM.read(static_cast<int>(headerSize + i)));
    }

    buf[len] = '\0';

    // De-obfuscate using derived key
    std::array<uint8_t, KEY_LEN> key;
    deriveKey(key.data());
    for (uint16_t i = 0; i < len; ++i) {
        buf[i] ^= key[static_cast<size_t>(i) % KEY_LEN];
    }

    DeserializationError err = deserializeJson(_doc, buf);

    if (err) {
        Logger::warn(String("Failed to parse NVS JSON: " + String(err.c_str())).c_str(), "SecureStorage");
        _doc.clear();

        delete[] buf;

        return false;
    }

    delete[] buf;

    Logger::info("NVS data loaded from EEPROM", "SecureStorage");

    return true;
}

/**
 * @brief Flush the in-memory JSON document to EEPROM
 *
 * @return true on success false on failure
 */
auto const SecureStorage::flushToEEPROM() -> bool {
    const size_t headerSize = 6;
    size_t payloadMax = _eepromSize - headerSize;

    String out;
    out.reserve(static_cast<int>(payloadMax));
    size_t written = serializeJson(_doc, out);

    if (written == 0 || written > payloadMax) {
        Logger::error("Serialized NVS too large for EEPROM", "SecureStorage");

        return false;
    }

    for (size_t i = 0; i < 4; ++i) {
        EEPROM.write(static_cast<int>(i), NVS_MAGIC[i]);
    }

    auto len = static_cast<uint16_t>(written);
    EEPROM.write(LEN_HIGH_IDX, static_cast<uint8_t>((len >> LEN_SHIFT) & LEN_MASK));
    EEPROM.write(LEN_LOW_IDX, static_cast<uint8_t>(len & LEN_MASK));

    // Obfuscate using derived key
    std::array<uint8_t, KEY_LEN> key;
    deriveKey(key.data());
    for (uint16_t i = 0; i < len; ++i) {
        uint8_t obfuscatedString = out.charAt(i) ^ key[static_cast<size_t>(i) % KEY_LEN];
        EEPROM.write(static_cast<int>(headerSize + i), obfuscatedString);
    }

    if (!EEPROM.commit()) {
        Logger::error("EEPROM commit failed", "SecureStorage");
        _dirty = true;

        return false;
    }

    _dirty = false;
    Logger::info(("NVS commit success size " + String(written)).c_str(), "SecureStorage");

    return true;
}

/**
 * @brief Store a key/value pair in the secure NVS
 *
 * @param key The key to set
 * @param value The string value to store
 * @return true on success false otherwise
 */
auto SecureStorage::put(const char* key, const char* value) -> bool {
    if (!_ready) {
        if (!begin()) {
            Logger::error("SecureStorage not initialized", "SecureStorage");
            return false;
        };
    }

    // 非敏感配置保存也会反复写入相同凭据；已在 EEPROM 且提交成功时直接返回，
    // 避免 NVS 序列化和 sector commit 阻塞主循环并磨损 flash。
    if (!_dirty && value != nullptr) {
        const char* storedValue = _doc[key];
        if (storedValue != nullptr && strcmp(storedValue, value) == 0) {
            return true;
        }
    }

    _doc[key] = value;
    _dirty = true;

    return flushToEEPROM();
}

/**
 * @brief Remove a key from the NVS store
 *
 * @param key The key to remove
 *
 * @return true on success false otherwise
 */
auto SecureStorage::remove(const char* key) -> bool {
    if (!_ready) {
        if (!begin()) {
            return false;
        }
    }

    _doc.remove(key);
    _dirty = true;

    return flushToEEPROM();
}

/**
 * @brief Retrieve a string value from NVS
 *
 * @param key The key to read
 * @param defaultValue Value to return if key missing (optional)
 *
 * @return String containing the stored value or default
 */
auto SecureStorage::get(const char* key, const char* defaultValue) -> String {
    if (!_ready) {
        if (!begin()) {
            return defaultValue != nullptr ? String(defaultValue) : String();
        }
    }

    if (_doc[key].isNull()) {
        return defaultValue != nullptr ? String(defaultValue) : String();
    }

    const char* valuePtr = _doc[key];

    return valuePtr != nullptr ? String(valuePtr) : (defaultValue != nullptr ? String(defaultValue) : String());
}
