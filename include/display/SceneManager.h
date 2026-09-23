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

#ifndef INCLUDE_DISPLAY_SCENEMANAGER_H
#define INCLUDE_DISPLAY_SCENEMANAGER_H

#include "display/Scene.h"

/// 可注册场景上限（够用即可，RAM 敏感）
static constexpr int SCENE_MAX = 8;
/// 场景参数最大长度（如 GIF 文件名）
static constexpr int SCENE_PARAM_MAX = 64;

/**
 * 场景调度器：显示面的唯一所有者入口。
 * 所有场景切换必须经 switchTo()——它保证退场重绘契约：
 * 旧场景 exit() 后新场景 enter() 全量绘制；入场失败回滚重绘上一场景。
 */
class SceneManager {
   public:
    static auto addScene(Scene* scene) -> bool;
    static auto switchTo(const char* name, const char* param = nullptr) -> bool;
    static auto update() -> void;

    static auto current() -> Scene*;
    static auto currentName() -> const char*;
    static auto currentParam() -> const char*;
    static auto sceneCount() -> int;
    static auto sceneNameAt(int index) -> const char*;

   private:
    static auto find(const char* name) -> Scene*;
};

#endif  // INCLUDE_DISPLAY_SCENEMANAGER_H
