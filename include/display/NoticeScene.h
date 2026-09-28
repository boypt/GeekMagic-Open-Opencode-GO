// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

/**
 * notice 场景：临时通知覆盖层。内容由 API 层校验后经 prepareText()/prepareImage()
 * 预载（Scene::enter() 只带 param，装不下正文），再 switchTo("notice") 入场渲染。
 * 入场时记录返回目标，计时结束自动切回；期间其他场景切换请求被忽略。
 */
class NoticeScene {
   public:
    enum Level : uint8_t { LevelInfo = 0, LevelWarning = 1, LevelCritical = 2 };

    // ---- 容量与默认值（API 层的校验上限也用这几个常量）----
    static constexpr uint16_t kMaxSeconds = 30;
    static constexpr uint16_t kDefaultSeconds = 5;
    static constexpr size_t kTextCap = 200;  // 正文上限（含末尾 NUL）

    // ---- 内容预载（可在 notice 已激活时重复调用 = 换内容并重置计时）----
    static void prepareText(Level level, const char* text, uint16_t seconds);
    static void prepareImage(uint16_t seconds);

    // ---- 状态查询（GET /api/v1/notice 用）----
    static bool isActive();
    static bool isImageMode();
    static Level level();
    static const char* text();            // 永不为 NULL；图像模式返回 ""
    static uint16_t totalSeconds();
    static uint16_t remainingSeconds();  // 已向上取整；非激活时返回 0
};
