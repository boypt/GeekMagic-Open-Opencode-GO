// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OpenCode Go 用量轮询 + 主界面绘制管理器（Phase 2，移植自旧工程
// sd2-opencode-go-balance/src/main.cpp 的正式 UI）。
// 仿 DashboardManager 的 begin()/update() 静态模式：
//   begin()  画主页面（WiFi 未连接时画 boot 提示页）；
//   update() 每秒 tick 时钟（分钟变化重绘时钟盒 / 秒变化只重绘秒摆条带），
//            并按 5min 轮询（从未成功过数据时 30s 快速重试），
//            数据成功后只局部重绘正文区，失败只重绘状态行 + 底部错误条。
// 局部重绘策略与旧工程一致，无整屏重刷（boot 页除外）。

#include <Arduino.h>
#include "opencodego/OpenCodeGoClient.h"

class UsageManager {
   public:
    static void begin();
    static void update();

    // ---- 场景钩子（balance 场景经 SceneManager 调用）----
    // enterScene() 入场契约：从零绘制所有元素；exitScene() 退场：复位局部更新状态
    static void enterScene();
    static void exitScene();

    // ---- 供 UI 层读取的状态 ----
    static bool hasData();
    static const String& lastError();
    static const OpenCodeGoUsage& usage();

    // ---- 界面绘制（移植自旧工程 main.cpp）----
    static void drawBootPage(bool fail);
    static void drawMainPage();

   private:
    static void drawBody();
    static void drawUpdateRow();
    static void drawQuotaRow(int y, const char* label, const OpenCodeGoWindow& w);
    static void drawClock();
    static void drawDateLine();
    static void drawSeparator();
    static void tickUi();
};
