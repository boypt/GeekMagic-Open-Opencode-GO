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

#ifndef INCLUDE_LED_AMBIENTLIGHT_H
#define INCLUDE_LED_AMBIENTLIGHT_H

#include <Arduino.h>

/// WS2812 数据脚（本机加装的氛围灯；与 LCD 引脚 13/14/0/2/5 无冲突）
static constexpr int WS2812_PIN = 12;
/// 灯珠数量（氛围灯为 1；多颗同色显示，改动后重刷）
static constexpr int WS2812_LED_COUNT = 1;

/**
 * WS2812 氛围灯：开关 / 颜色 / 亮度 / 模式（常亮·呼吸·彩虹），
 * 状态持久化在 config.json（led_* 字段）。update() 由 main loop 无条件驱动
 *（氛围灯独立于显示场景，不受场景调度影响）。
 */
class AmbientLight {
   public:
    static void begin();
    static void update();
    /// 按当前配置立即重算并写灯
    static void apply();
    /// 以下 setter 修改配置并立即生效（持久化由调用方 configManager.save()）
    static void setOn(bool on);
    static void setMode(int mode);
    static void setColor(uint8_t r, uint8_t g, uint8_t b);
    static void setBrightness(int percent);
};

#endif  // INCLUDE_LED_AMBIENTLIGHT_H
