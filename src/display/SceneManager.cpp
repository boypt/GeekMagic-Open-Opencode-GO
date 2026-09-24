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

#include "display/SceneManager.h"

#include <Arduino.h>
#include <Logger.h>
#include <string.h>

static Scene* s_scenes[SCENE_MAX] = {nullptr};
static int s_sceneCount = 0;
static Scene* s_current = nullptr;
static char s_currentParam[SCENE_PARAM_MAX] = {0};

auto SceneManager::addScene(Scene* scene) -> bool {
    if (scene == nullptr || s_sceneCount >= SCENE_MAX) {
        return false;
    }

    s_scenes[s_sceneCount++] = scene;

    return true;
}

auto SceneManager::find(const char* name) -> Scene* {
    if (name == nullptr) {
        return nullptr;
    }

    for (int i = 0; i < s_sceneCount; i++) {
        if (strcmp(s_scenes[i]->name(), name) == 0) {
            return s_scenes[i];
        }
    }

    return nullptr;
}

auto SceneManager::switchTo(const char* name, const char* param) -> bool {
    Scene* next = find(name);
    if (next == nullptr) {
        Logger::warn((String("SceneManager: unknown scene ") + String(name)).c_str(), "Scene");
        return false;
    }

    char requested[SCENE_PARAM_MAX] = {0};
    if (param != nullptr) {
        strncpy(requested, param, sizeof(requested) - 1);
    }

    Scene* prev = s_current;

    // 退场：旧场景停画/释放（同场景切换也先退场，用于换参数重放）
    if (prev != nullptr) {
        prev->exit();
    }

    // 入场：enter() 契约上必须全量绘制所有元素
    if (next->enter(requested)) {
        s_current = next;
        strncpy(s_currentParam, requested, sizeof(s_currentParam) - 1);
        s_currentParam[sizeof(s_currentParam) - 1] = '\0';
        Logger::info((String("SceneManager: -> ") + String(next->name())).c_str(), "Scene");
        return true;
    }

    // 入场失败：回滚重绘上一场景（退场重绘契约，绝不留半屏/黑底）
    Logger::warn((String("SceneManager: enter failed, roll back to ") +
                  String(prev != nullptr ? prev->name() : "(none)"))
                     .c_str(),
                 "Scene");

    if (prev != nullptr && prev != next && prev->enter(s_currentParam)) {
        s_current = prev;
        return false;
    }

    s_current = nullptr;

    return false;
}

auto SceneManager::update() -> void {
    if (s_current != nullptr) {
        s_current->update();
    }
}

auto SceneManager::redrawCurrent() -> void {
    if (s_current != nullptr) {
        // 唤醒只重放当前场景的 enter()，不经过 switchTo 的注册/退场/回滚路径。
        s_current->enter(s_currentParam);
    }
}

auto SceneManager::current() -> Scene* { return s_current; }

auto SceneManager::currentName() -> const char* {
    return s_current != nullptr ? s_current->name() : "";
}

auto SceneManager::currentParam() -> const char* { return s_currentParam; }

auto SceneManager::sceneCount() -> int { return s_sceneCount; }

auto SceneManager::sceneNameAt(int index) -> const char* {
    if (index < 0 || index >= s_sceneCount) {
        return "";
    }

    return s_scenes[index]->name();
}
