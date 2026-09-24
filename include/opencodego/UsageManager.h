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
    static constexpr size_t kRowLabelCap = 8;   // label 最多 7 字节
    static constexpr size_t kRowResetCap = 16;  // reset 最多 15 字节
    static constexpr size_t kStatusCap = 48;

    static void begin();
    static void update();

    // ---- 场景钩子（balance 场景经 SceneManager 调用）----
    // enterScene() 入场契约：从零绘制所有元素；exitScene() 退场：复位局部更新状态
    static void enterScene();
    static void exitScene();

    // ---- 上位机推送入口（POST /api/v1/balance 调用）----
    // labels/progress/resets 由 API 在调用前逐项写入；status 为可选状态行。
    // 记录行数、状态行、时间戳并 requestFullRedraw()，调用方直接回 200。
    static void pushBalance(uint8_t rowCount, const char* status, bool hasStatus);

    // ---- 供 UI 层 / API 层读取的状态 ----
    static bool hasPush();
    static bool hasStatus();
    static const char* statusText();
    static void setRowLabel(uint8_t index, const char* label);
    static void setRowReset(uint8_t index, const char* reset);
    static void setRowProgress(uint8_t index, int value);
    static const char* rowLabel(uint8_t index);  // ""（未设置）永不返回 NULL
    static int rowProgress(uint8_t index);
    static const char* rowReset(uint8_t index);  // ""（未设置）永不返回 NULL
    static time_t pushEpoch();
    static uint32_t pushAgeSec();

    // ---- 界面绘制 ----
    static void drawBootPage(bool fail);
    static void drawMainPage();

    // ---- 纯时钟场景页（clock 场景：七段时分秒 + 顶部小字日期星期）----
    static void drawClockPage();
    static void tickClockPage();

   private:
    static void drawBody();
    static void drawUpdateRow();
    static void drawQuotaRow(int y, uint8_t index);
    static void drawClock();
    static void drawDateLine();
    static void drawSeparator();
    static void tickUi();
};
