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

#ifndef INCLUDE_DISPLAY_SCENE_H
#define INCLUDE_DISPLAY_SCENE_H

/**
 * 场景接口：所有整屏界面实现本接口，由 SceneManager 独占调度。
 *
 * 切换契约（退场重绘）：
 *   exit()  退场——停止本场景的绘制/定时/解码并释放资源，退场后禁止再碰屏；
 *   enter() 入场——必须从零绘制本场景所有元素（全量重绘），无法开始返回 false；
 *   update() 每个 loop 仅由 SceneManager 驱动当前场景（场景间天然互斥，
 *           例如 GIF 解码与 TLS 额度拉取的堆内存竞争）。
 */
class Scene {
   public:
    virtual ~Scene() = default;

    /// 场景名（web API 切换用，小写短标识，如 "balance"）
    virtual auto name() const -> const char* = 0;

    /// 入场：param 为场景参数（可空），返回 false 表示无法开始
    virtual auto enter(const char* param) -> bool = 0;

    /// 每 loop 驱动当前场景
    virtual auto update() -> void = 0;

    /// 退场：停画并释放本场景资源，禁止绘制
    virtual auto exit() -> void = 0;
};

#endif  // INCLUDE_DISPLAY_SCENE_H
