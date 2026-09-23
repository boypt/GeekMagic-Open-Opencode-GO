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
static uint32_t s_lastTickMs = 0;
static uint16_t s_phase = 0;  // 呼吸相位 / 彩虹色相

/// 把配置里的 RGB × 亮度百分比换算成灯珠颜色（mode 的动态效果在 update 里再调制）
static auto scaledColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightnessPercent, uint8_t gain255) -> uint32_t {
    const uint32_t scale = (static_cast<uint32_t>(brightnessPercent) * gain255) / 100U;

    return s_strip.Color(static_cast<uint8_t>(r * scale / 255U), static_cast<uint8_t>(g * scale / 255U),
                         static_cast<uint8_t>(b * scale / 255U));
}

/// HSV(0..65535 色相) -> 32bit，彩虹模式用（Adafruit 自带 ColorHSV 亦可，此处保持简单）
static auto hueColor(uint16_t hue) -> uint32_t {
    return Adafruit_NeoPixel::ColorHSV(hue);
}

auto AmbientLight::apply() -> void {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    configManager.getLedColor(r, g, b);

    const uint8_t brightness = configManager.getLedBrightness();

    if (!configManager.getLedOn()) {
        s_strip.clear();
        s_strip.show();
        return;
    }

    if (configManager.getLedMode() == 2) {
        // 彩虹：颜色随时钟推进，亮度由全局亮度控制
        const uint32_t c = hueColor(s_phase);
        const uint8_t cr = (uint8_t)(c >> 16);
        const uint8_t cg = (uint8_t)(c >> 8);
        const uint8_t cb = (uint8_t)c;
        s_strip.fill(scaledColor(cr, cg, cb, brightness, 255));
    } else if (configManager.getLedMode() == 1) {
        // 呼吸：亮度按三角波调制（update 里推进相位后重调）
        const uint8_t wave = (s_phase < 256) ? static_cast<uint8_t>(s_phase) : static_cast<uint8_t>(511 - s_phase);
        s_strip.fill(scaledColor(r, g, b, brightness, wave));
    } else {
        s_strip.fill(scaledColor(r, g, b, brightness, 255));
    }

    s_strip.show();
}

auto AmbientLight::begin() -> void {
    s_strip.begin();
    s_strip.setBrightness(255);  // 全局亮度由配置在 scaledColor 里统一缩放
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
    } else {
        s_phase = static_cast<uint16_t>((s_phase + 1) % 512);  // 呼吸三角波 0..511
    }

    apply();
}

auto AmbientLight::setOn(bool on) -> void {
    configManager.setLedOn(on);
    apply();
}

auto AmbientLight::setMode(int mode) -> void {
    configManager.setLedMode(mode);
    s_phase = 0;
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
