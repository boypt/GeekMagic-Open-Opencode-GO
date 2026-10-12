// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>

// 三重同心额度环的**共享绘制器**：股票场景右下角小环（41×41）与 balance 居中大环
// （153×153）共用同一套逐像素距离场算法，以及配套的三角查表 / 整数开方 / 混色 /
// 端帽数学。调用方只传各自的布局、调色板与剩余百分比；像素算法口径由此保持一致，
// 不再有两份平行实现互相漂移。
//
// 本文件只暴露一个固定三环、非模板、非虚的自由函数。所有几何/状态都是 draw() 内的
// 自动局部栈数组（每环几个标量），不引入模板、策略开关、动态堆或静态工作数组。
namespace QuotaRingRenderer {

// 单环参数。
//   outerRadius：环外缘半径（px）。
//   remaining  ：-1 = 无值（只画暗轨道）；0 = 12 点单个实心圆点；1..99 = 剩余弧；
//                100 = 整圈。语义为**剩余额度百分比**。
//   arc565 / track565：亮弧色 / 暗轨道色，由调用方的 rgb565() 预交换好面板
//     BGR 色序后传入（本渲染器不做任何色序转换，也不认识源 RGB）。
struct Ring {
    int16_t outerRadius;
    int16_t remaining;
    uint16_t arc565;
    uint16_t track565;
};

// 绘制三重同心环。
//   gfx             ：目标显示对象（像素直绘）。
//   boxX/boxY/boxSize：擦底方框（屏幕绝对坐标，正方形），draw() 只在此框内 fillRect
//                      与落笔，绝不触碰框外元素。
//   centerX/centerY ：圆心（屏幕绝对坐标）。
//   thickness       ：每环径向描边宽度（px，向内取）。
//   bg565           ：环区底色（径向抗锯齿往外混的色，由调用方给出）。
//   rings           ：调用方局部栈上构造的 3 元素数组，行序 0/1/2 = 外 / 中 / 内。
//
// remaining 语义：<0 只画暗轨道；==0 不画弧，只在 12 点画一个身份色实心圆点（半径
// capR = thickness/2，直径 = 该环描边宽；环带其余部分仍是暗轨道）；>=100 画满整圈、
// 不画端帽（唯一无帽态）；1..99 弧长取 remaining*36 个 0.1°，**只做上侧夹取**
// （tenths ≤ 3600−Θ_min，防 99% 缺口被两端帽补满而与整圈无法区分；低百分比不再
// 下侧抬高，短弧按原始角度/弧长如实呈现）。端帽仅在两端帽心弦长 ≥ 两帽直径 (2*capR)
// 时绘制 —— 更短的弧不画端帽，免得被两个大圆盘融合撑宽（帽半径本身不缩放）。
// 环色按行身份固定、不随数值变；亮弧/亮点 = 剩余额度、暗轨道 = 已消耗掉的那段。
void draw(Arduino_GFX* gfx,
          int16_t boxX, int16_t boxY, int16_t boxSize,
          int16_t centerX, int16_t centerY,
          uint8_t thickness, uint16_t bg565,
          const Ring rings[3]);

}  // namespace QuotaRingRenderer
