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

#include "led/AmbientLight.h"

#include <Adafruit_NeoPixel.h>

#include "config/ConfigManager.h"

extern ConfigManager configManager;

static Adafruit_NeoPixel s_strip(WS2812_LED_COUNT, WS2812_PIN, NEO_GRB + NEO_KHZ800);

/// 效果 tick 间隔（呼吸/彩虹）
static constexpr uint32_t EFFECT_TICK_MS = 20U;
/// 呼吸保持约 10s 一轮；相位从真实时间派生，主循环繁忙时不会拖慢节奏
static constexpr uint32_t BREATH_PERIOD_MS = 10000U;
static constexpr uint16_t BREATH_ENVELOPE_MAX = 4095U;
/// WS2812 在极低 PWM 下可能闪断；呼吸时每个颜色通道都不进入不可靠区
static constexpr uint8_t BREATH_MIN_CHANNEL = 10U;
static uint32_t s_lastTickMs = 0;
static uint32_t s_breathPhaseStartMs = 0;
static uint16_t s_phase = 0;  // 彩虹色相
static uint32_t s_lastSentColor = 0;
static bool s_lastSentColorValid = false;
static bool s_forceShow = true;  // begin / 库状态变更后，下一帧必须重发

/// 余弦包络经近似 gamma 2.2 校正后的 Q12 曲线；正弦平方使 0/1 周期点斜率均为零
/// 曲线左右对称，只存前半段，258B flash，换取暗段足够的定点精度
static const uint16_t s_breathEnvelopeQ12[129] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2,
    3, 4, 5, 7, 8, 10, 12, 15, 18, 21, 25, 30, 35, 40, 46, 53,
    61, 69, 78, 88, 99, 111, 123, 137, 152, 168, 185, 203, 223, 243, 265, 289,
    313, 339, 366, 395, 425, 457, 490, 525, 561, 599, 638, 678, 720, 764, 809, 855,
    903, 953, 1003, 1056, 1109, 1164, 1220, 1277, 1336, 1395, 1456, 1517, 1580, 1643, 1708, 1773,
    1839, 1905, 1972, 2039, 2107, 2175, 2243, 2311, 2379, 2447, 2515, 2583, 2651, 2718, 2784, 2850,
    2915, 2979, 3042, 3104, 3165, 3225, 3284, 3341, 3396, 3450, 3503, 3553, 3602, 3649, 3694, 3737,
    3777, 3816, 3852, 3886, 3918, 3947, 3973, 3997, 4019, 4038, 4054, 4067, 4078, 4086, 4092, 4095,
    4095,
};

/// 把配置里的 RGB × 亮度百分比换算成灯珠颜色（mode 的动态效果在 update 里再调制）
static auto scaledColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightnessPercent, uint8_t gain255) -> uint32_t {
    const uint32_t scale = (static_cast<uint32_t>(brightnessPercent) * gain255) / 100U;

    return s_strip.Color(static_cast<uint8_t>(r * scale / 255U), static_cast<uint8_t>(g * scale / 255U),
                         static_cast<uint8_t>(b * scale / 255U));
}

/// 呼吸色只保留最后一次整数除法，避免亮度百分比与包络分别截断后在暗端形成台阶
static auto breathColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightnessPercent, uint16_t envelope) -> uint32_t {
    const uint32_t scaleQ12 = static_cast<uint32_t>(brightnessPercent) * envelope;
    constexpr uint32_t SCALE_DENOMINATOR = 100U * BREATH_ENVELOPE_MAX;
    const auto channel = [scaleQ12](uint8_t value) -> uint8_t {
        const uint8_t scaled = static_cast<uint8_t>(value * scaleQ12 / SCALE_DENOMINATOR);
        return scaled < BREATH_MIN_CHANNEL ? BREATH_MIN_CHANNEL : scaled;
    };

    return s_strip.Color(channel(r), channel(g), channel(b));
}

/// HSV(0..65535 色相) -> 32bit，彩虹模式用（Adafruit 自带 ColorHSV 亦可，此处保持简单）
static auto hueColor(uint16_t hue) -> uint32_t {
    return Adafruit_NeoPixel::ColorHSV(hue);
}

/// 灯珠会保持最后一次锁存值，相同颜色不再重复占用有误码风险的传输窗口
static auto showColor(uint32_t color) -> void {
    if (s_forceShow || !s_lastSentColorValid || color != s_lastSentColor) {
        s_strip.fill(color);
        s_strip.show();
        s_lastSentColor = color;
        s_lastSentColorValid = true;
        s_forceShow = false;
    }
}

static auto clearColor() -> void {
    if (s_forceShow || !s_lastSentColorValid || s_lastSentColor != 0U) {
        s_strip.clear();
        s_strip.show();
        s_lastSentColor = 0U;
        s_lastSentColorValid = true;
        s_forceShow = false;
    }
}

auto AmbientLight::apply() -> void {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    configManager.getLedColor(r, g, b);

    const uint8_t brightness = configManager.getLedBrightness();

    if (!configManager.getLedOn()) {
        clearColor();
        return;
    }

    uint32_t color = 0;
    if (configManager.getLedMode() == 2) {
        // 彩虹：颜色随时钟推进，亮度由全局亮度控制
        const uint32_t c = hueColor(s_phase);
        const uint8_t cr = (uint8_t)(c >> 16);
        const uint8_t cg = (uint8_t)(c >> 8);
        const uint8_t cb = (uint8_t)c;
        color = scaledColor(cr, cg, cb, brightness, 255);
    } else if (configManager.getLedMode() == 1) {
        // 呼吸：从启动时刻直接求相位，20ms tick 即使漏掉也不会累积相位误差
        const uint32_t elapsedMs = millis() - s_breathPhaseStartMs;
        const uint8_t phase = static_cast<uint8_t>((elapsedMs % BREATH_PERIOD_MS) * 256U / BREATH_PERIOD_MS);
        const uint8_t tableIndex = (phase <= 128U) ? phase : static_cast<uint8_t>(255U - phase);
        color = breathColor(r, g, b, brightness, s_breathEnvelopeQ12[tableIndex]);
    } else {
        color = scaledColor(r, g, b, brightness, 255);
    }

    showColor(color);
}

auto AmbientLight::begin() -> void {
    s_strip.begin();
    s_strip.setBrightness(255);  // 全局亮度由配置在 scaledColor / breathColor 里统一缩放
    s_lastSentColorValid = false;
    s_forceShow = true;
    s_lastTickMs = millis();
    s_breathPhaseStartMs = s_lastTickMs;  // 从暗端起呼吸，切入时不会先亮一下
    apply();
}

auto AmbientLight::update() -> void {
    if (!configManager.getLedOn() || configManager.getLedMode() == 0) {
        return;
    }

    const uint32_t now = millis();
    if (now - s_lastTickMs < EFFECT_TICK_MS) {
        return;
    }
    s_lastTickMs = now;

    if (configManager.getLedMode() == 2) {
        s_phase = static_cast<uint16_t>(s_phase + 256);  // 约 5s 一圈
    }
    // 呼吸相位在 apply() 中按 millis() 求值，漏掉的 tick 会在下一帧直接补上

    apply();
}

auto AmbientLight::setOn(bool on) -> void {
    configManager.setLedOn(on);
    apply();
}

auto AmbientLight::setMode(int mode) -> void {
    const int oldMode = configManager.getLedMode();
    configManager.setLedMode(mode);
    if (oldMode != mode) {
        s_phase = 0;
        s_lastTickMs = millis();
        s_breathPhaseStartMs = s_lastTickMs;
    }
    apply();
}

auto AmbientLight::setColor(uint8_t r, uint8_t g, uint8_t b) -> void {
    configManager.setLedColor(r, g, b);
    apply();
}

auto AmbientLight::setBrightness(int percent) -> void {
    configManager.setLedBrightness(percent);
    apply();
}
