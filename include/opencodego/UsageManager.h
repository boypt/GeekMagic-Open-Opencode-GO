// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Balance 主界面绘制管理器（推送模式）：上位机经 POST /api/v1/balance
// 推送三行额度文本 + 可选状态行，本模块只负责渲染，不再做任何出站拉取。
// 仿 DashboardManager 的 begin()/update() 静态模式：
//   begin()  标记启动（首帧绘制仍由 enterScene / 首次联网就绪触发）；
//   update() 每秒 tick 时钟（分钟变化差量更新 HH/MM 字形 / 秒变化只摆动分隔线），
//            并消费 DisplayManager::requestFullRedraw() 整屏重绘（推送到达时触发）。
// 局部重绘策略与旧工程一致，无整屏重刷（整屏重绘/入场除外）。
// 存储为静态定长 char 缓冲（零 String 常驻、零 >4KB 分配，见坑 #9）。

#include <Arduino.h>
#include <time.h>

class UsageManager {
   public:
    // ---- 推送缓冲容量（含末尾 NUL）----
    static constexpr size_t kBalanceLines = 3;
    static constexpr size_t kLineCap = 64;    // 每行额度文本
    static constexpr size_t kStatusCap = 48;  // 状态行文本

    static void begin();
    static void update();

    // ---- 场景钩子（balance 场景经 SceneManager 调用）----
    // enterScene() 入场契约：从零绘制所有元素；exitScene() 退场：复位局部更新状态
    static void enterScene();
    static void exitScene();

    // ---- 上位机推送入口（POST /api/v1/balance 调用）----
    // lines[0..nLines) 为 1..3 行文本（NULL 行视为空）；status 为可选状态行
    // （hasStatus=false 或空串表示缺省 → 状态行显示 UPD HH:MM）。
    // 拷贝截断至定长缓冲，记录时间戳并 requestFullRedraw()，调用方直接回 200。
    static void pushBalance(const char* const* lines, uint8_t nLines, const char* status,
                            bool hasStatus);

    // ---- 供 UI 层 / API 层读取的状态 ----
    static bool hasPush();
    static const char* lineAt(uint8_t i);  // ""（未推送槽位）或行文本，永不返回 NULL
    static bool hasStatus();
    static const char* statusText();  // "" 或状态行文本，永不返回 NULL
    static time_t pushEpoch();        // 推送时刻 UTC epoch；推送时未同步则为 0
    static uint32_t pushAgeSec();     // 距推送秒数；未推送过为 0

    // ---- 界面绘制 ----
    static void drawBootPage(bool fail);
    static void drawMainPage();

    // ---- 纯时钟场景页（clock 场景：七段时分秒 + 顶部小字日期星期）----
    static void drawClockPage();
    static void tickClockPage();

   private:
    static void drawBody();
    static void drawUpdateRow();
    static void drawQuotaRow(int y, const char* text);
    static void drawClock();
    static void drawDateLine();
    static void drawSeparator();
    static void tickUi();
};
