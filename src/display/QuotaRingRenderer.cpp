// SPDX-License-Identifier: GPL-3.0-or-later
#include "display/QuotaRingRenderer.h"

// 三重同心额度环的共享绘制实现。股票（右下角小环）与 balance（居中大环）
// 两场景唯一共用的一份像素算法与数学工具；两处调用点只传各自几何 / 调色板 /
// 剩余百分比。几何全部由 Ring::outerRadius 与 thickness 现算，不写死任何一页
// 的尺寸，故 Θ_min 也按各页实际尺寸现算（stock 与 balance 的 Θ_min 数值不同，
// 但公式同一套）。

namespace QuotaRingRenderer {
namespace {

// 环数固定为 3（外 / 中 / 内）
constexpr uint8_t kRingCount = 3;

// 1/255 定标的正弦表（0..90°，四分之一波）。画圆弧要取圆周坐标，为了不把 libm 的
// sinf/cosf（连带浮点库和一堆代码）带进固件，这里用 91 字节查表 + 象限映射，误差
// ≤ 0.5/255 ≈ 0.04px，240×240 屏上完全看不出来。91 字节常驻静态区（.rodata），
// 不参与堆分配。
const uint8_t kSinUnitDeg[91] = {
      0,   4,   9,  13,  18,  22,  27,  31,  35,  40,  44,  49,  53,  57,  62,
     66,  70,  75,  79,  83,  87,  91,  96, 100, 104, 108, 112, 116, 120, 124,
    127, 131, 135, 139, 143, 146, 150, 153, 157, 160, 164, 167, 171, 174, 177,
    180, 183, 186, 190, 192, 195, 198, 201, 204, 206, 209, 211, 214, 216, 219,
    221, 223, 225, 227, 229, 231, 233, 235, 236, 238, 240, 241, 243, 244, 245,
    246, 247, 248, 249, 250, 251, 252, 253, 253, 254, 254, 254, 255, 255, 255,
    255,
};

// sin(角度) 的 1/255 定标值（-255..255）。tenths = 角度×10，即 0.1° 分辨率，
// 在 kSinUnitDeg 上线性插值。取 0.1° 而不是整度，是因为额度环的扇形边界正好是
// 3.6·percent 度（如 70% → 252.0°、17% → 61.2°），整度会把边界量化掉 0.5°，
// 在大环 r=76 处就是 0.66px 的错位；0.1° 只剩 0.07px。
int16_t sinUnitTenths(uint16_t tenths) {
    tenths %= 3600U;
    uint16_t folded = 0U;
    bool negative = false;
    if (tenths < 900U) {
        folded = tenths;
    } else if (tenths < 1800U) {
        folded = 1800U - tenths;
    } else if (tenths < 2700U) {
        folded = tenths - 1800U;
        negative = true;
    } else {
        folded = 3600U - tenths;
        negative = true;
    }
    const uint16_t index = folded / 10U;   // 0..90
    const int16_t remainder = static_cast<int16_t>(folded % 10U);
    int16_t magnitude = 255;
    if (index < 90U) {
        const int16_t low = kSinUnitDeg[index];
        const int16_t high = kSinUnitDeg[index + 1U];
        magnitude = static_cast<int16_t>(low + (((high - low) * remainder + 5) / 10));
    }
    return negative ? static_cast<int16_t>(-magnitude) : magnitude;
}

// cos(角度) = sin(角度 + 90°)，复用同一张表。
int16_t cosUnitTenths(uint16_t tenths) {
    return sinUnitTenths(static_cast<uint16_t>(tenths + 900U));
}

// 32 位整数平方根（逐位恢复法）。算像素到圆心的距离要用到它 —— 引入 sqrtf() 连带
// libm 与浮点库，正是上面那张正弦表要绕开的东西，所以这里手写整数版。
uint16_t isqrt32(uint32_t value) {
    uint32_t remainder = value;
    uint32_t root = 0U;
    uint32_t bit = 1UL << 30;
    while (bit > remainder) {
        bit >>= 2;
    }
    for (; bit != 0U; bit >>= 2) {
        if (remainder >= root + bit) {
            remainder -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    return static_cast<uint16_t>(root);
}

// 两个**已交换色序**的 RGB565 之间按 ratio(0..255) 线性插值。
// BGR 交换是固定通道置换，与逐通道 lerp 可交换，所以直接在调用方 rgb565() 打包
// 出来的值上插即可，不必回到源 RGB 再转换。三段通道位宽不同（5/6/5），
// 必须各自在自己的位宽里插完再拼回去，跨位宽插会偏色。
uint16_t lerp565(uint16_t from, uint16_t to, uint8_t ratio) {
    const int32_t weight = ratio;
    const int32_t fromR = (from >> 11) & 0x1FU;
    const int32_t fromG = (from >> 5) & 0x3FU;
    const int32_t fromB = from & 0x1FU;
    const int32_t red = fromR + ((((to >> 11) & 0x1FU) - fromR) * weight + 127) / 255;
    const int32_t green = fromG + ((((to >> 5) & 0x3FU) - fromG) * weight + 127) / 255;
    const int32_t blue = fromB + (((static_cast<int32_t>(to & 0x1FU) - fromB) * weight + 127) / 255);
    return static_cast<uint16_t>((static_cast<uint32_t>(red) << 11) |
                                 (static_cast<uint32_t>(green) << 5) |
                                 static_cast<uint32_t>(blue));
}

// 圆头帽在单像素上的覆盖率（0..255）。dStart/dEnd 为像素中心到起点帽心 / 终点帽心
// 的 1/16 px 距离。帽半径 capR px = capR·16 单位，外缘再留 0.5px（8 单位）渐隐，
// 与环带边缘同一口径；取两端较小距离 = 若两个端帽覆盖到同一像素就取较大覆盖率，
// 避免重复混色。渐隐带恒 1px（16 单位）。capR 动态传入（= thickness/2），随各页几何变化。
// 单点态（remaining==0）把起点/终点帽心设成同一点，于是 min(起点距, 终点距) 退化为
// 只按这唯一的帽心算覆盖率——即一个实心圆点，不会出现第二个端帽。
uint8_t capCoverage(int16_t dStart, int16_t dEnd, uint8_t capR) {
    const int16_t dist = dStart < dEnd ? dStart : dEnd;
    int32_t edge = static_cast<int32_t>(capR) * 16 + 8 - dist;
    if (edge <= 0) {
        return 0U;
    }
    if (edge > 16) {
        edge = 16;              // 1px 宽的边缘过渡，内部恒为满覆盖
    }
    return static_cast<uint8_t>(edge * 255 / 16);
}

/// 端到端帽心弦长预算对应的**上界弧长**（0.1° 单位）：`3600 − 本值` 用作 1..99 弧长的
/// **上夹取**，防止剩余 99%（缺口仅 3.6°）被两端帽盘补满、与整圈无法区分。
/// **只做上界**：低百分比不再做下侧夹取 —— 短弧按原始角度/弧长如实呈现，短于端帽直径
/// 时干脆不画端帽（见 draw() 的 drawCaps），不会被两个大圆盘融合撑宽。
/// 必须是**算出来的**：outerRadius / thickness 一改，上界要自动跟着变，不能写死；
/// 对每环按所给实际几何现算。
/// 最小弧长 = 两个帽盘（半径 capR + 各自 0.5px AA 外沿）× 2 + 约 1px 背景缝，
/// 即帽心弦长 ≥ 2·(capR+0.5px) + 1px；实测 stock（capR=2、midR=18/12/6）：
/// {194, 288, 604} 0.1° = 19.4° / 28.8° / 60.4°；balance 放大几何得到自己的值。
uint16_t tangentArcTenths(int16_t outerRadius, uint8_t capRadius) {
    const int32_t mid16 = static_cast<int32_t>(outerRadius - capRadius) * 16;
    // 端到端帽心弦长下限 = 两个帽盘(含 0.5px AA 外沿) + 约 1px 纯背景缝。
    const int32_t visibleGap16 = 16;                                        // ~1 px 背景缝
    const int32_t capExtent16 = static_cast<int32_t>(capRadius) * 16 + 8;   // 帽半径 + 0.5px AA 外沿
    const int32_t need = 2 * capExtent16 + visibleGap16;   // stock: chord ≥ 6px
    // 弦长 = 2·mid·sin(Θ/2)；正弦表 1/255 定标，故 (2·mid16·sin)/255。
    for (uint16_t half = 1U; half <= 1800U; ++half) {
        if ((2 * mid16 * sinUnitTenths(half)) / 255 >= need) {
            return static_cast<uint16_t>(half * 2U);
        }
    }
    return 3600U;   // 不可达：中径恒远大于帽半径
}

}  // namespace

void draw(Arduino_GFX* gfx,
          int16_t boxX, int16_t boxY, int16_t boxSize,
          int16_t centerX, int16_t centerY,
          uint8_t thickness, uint16_t bg565,
          const Ring rings[3]) {
    // 只擦环区（调用方给定的方框），不碰框外元素。
    gfx->fillRect(boxX, boxY, boxSize, boxSize, bg565);

    const uint8_t capR = static_cast<uint8_t>(thickness / 2U);

    // 每个环的扇形边界方向预先算好（1/255 定标），像素循环里只做点积。
    // 形状 0 = 无弧（无值 -1 只剩暗轨道；完全没有额度数据时三段必全是 -1 —— 三圈
    // 轨道照画，不提前返回）、1 = 整圈（仅满额度 remaining>=100，不画帽，是唯一
    // 无帽态）、2 = 凸扇形、3 = 反射扇形（1..99 按**原始**剩余弧长判）、
    // 4 = 12 点单个实心圆点（仅 remaining==0：不画弧、不画第二个端帽，只画一个以
    // 12 点上端帽心为圆心、半径 capR 的身份色圆盘）。
    uint8_t shape[kRingCount] = {};
    // 1..99 是否绘制两端圆头帽：仅当两端帽心弦长 ≥ 两帽直径 (2*capR) 时才画。短于此
    // 的低值弧保留原始弧长 / 角度，不被两个大圆盘融合撑宽；帽半径本身不做缩放。
    // shape 4 单点不受它影响（点恒画）。
    uint8_t drawCaps[kRingCount] = {};
    int16_t sinT[kRingCount] = {};
    int16_t cosT[kRingCount] = {};
    // 圆头帽的圆心（形状 2/3/4 有效）：起点在角度 0、终点在角度 Θ，均落在
    // 环带中径上。帽心以 1/16 px 定点存储，像素循环里不再做除法：
    //   起点 (dx, up) = (0, midR)              ← 也是 shape 4 单点的圆心
    //   终点 (dx, up) = (midR·sinΘ/255, midR·cosΘ/255)
    int16_t capStartUp16[kRingCount] = {};
    int16_t capEndDx16[kRingCount] = {};
    int16_t capEndUp16[kRingCount] = {};
    for (uint8_t i = 0; i < kRingCount; ++i) {
        const int16_t percent = rings[i].remaining;
        if (percent < 0) {
            // 无值：只剩暗轨道（无任何额度数据时三段必全 -1）。
            shape[i] = 0U;
        } else if (percent == 0) {
            // 用尽：不画弧，只在 12 点画一个身份色实心圆点（半径 capR，直径 = 该环描边宽）。
            // 复用 12 点上端帽心 (dx=0, up=midR)，并把终点帽心也赋成同一点 —— 下方
            // capCoverage() 取两端较小距离，退化为只按这一个帽心算覆盖率（单点圆盘，
            // 不会出现第二个帽）。像素环带基准仍是轨道（sector=false），只有圆盘覆盖
            // 命中处才用弧色，故绝不等同于一个小角扇形。
            shape[i] = 4U;
            const int32_t mid16 = static_cast<int32_t>(rings[i].outerRadius - capR) * 16;
            capStartUp16[i] = static_cast<int16_t>(mid16);
            capEndDx16[i] = 0;                                  // 终点 == 起点
            capEndUp16[i] = static_cast<int16_t>(mid16);
        } else if (percent >= 100) {
            // 满额度：画满整圈、不画帽 —— 用户明确要的 100% 特殊处理，也是唯一
            // 无帽的整圈态。100→99 会从满圈变成整圈带一道约 1px 背景窄缝的弧。
            shape[i] = 1U;
        } else {
            // 弧长 = 剩余百分比 = percent·36 个 0.1°（本支只处理 1..99；0% 已由上方
            // shape=4 单点接管，不走这里）。
            uint16_t tenths = static_cast<uint16_t>(percent) * 36U;
            // **只做上侧夹取**：360°−Θ_min 防剩余 99%（缺口仅 3.6°）被两端帽盘补满、
            // 与整圈无法区分。低百分比**不再下侧抬高** —— 短弧按原始角度如实呈现，
            // 不再把例如内环 5%（18°）抬到 Θ_min（60.4°），短于端帽直径时直接不画端帽。
            const uint16_t lo = tangentArcTenths(rings[i].outerRadius, capR);
            const uint16_t hi = static_cast<uint16_t>(3600U - lo);
            if (tenths > hi) {
                tenths = hi;
            }
            sinT[i] = sinUnitTenths(tenths);
            cosT[i] = cosUnitTenths(tenths);
            shape[i] = tenths <= 1800U ? 2U : 3U;   // 按剩余弧长判（上界可能已夹）
            const int32_t mid16 = static_cast<int32_t>(rings[i].outerRadius - capR) * 16;
            capStartUp16[i] = static_cast<int16_t>(mid16);
            const int32_t ex = mid16 * sinT[i];
            const int32_t ey = mid16 * cosT[i];
            capEndDx16[i] = static_cast<int16_t>(
                ex >= 0 ? (ex + 127) / 255 : -((-ex + 127) / 255));
            capEndUp16[i] = static_cast<int16_t>(
                ey >= 0 ? (ey + 127) / 255 : -((-ey + 127) / 255));
            // 端到端帽心弦长（1/16 px）：起点 (0, capStartUp16)、终点
            // (capEndDx16, capEndUp16)。弦长 < 两帽直径 (2*capR) ⇒ 不画两端帽圆盘，
            // 让该弧以原始角度呈现而不被两大圆盘融合撑宽。
            const int32_t chordDx = capEndDx16[i];
            const int32_t chordUp =
                static_cast<int32_t>(capEndUp16[i]) - static_cast<int32_t>(capStartUp16[i]);
            const uint32_t chord16 = isqrt32(
                static_cast<uint32_t>(chordDx * chordDx + chordUp * chordUp));
            drawCaps[i] = chord16 >= static_cast<uint32_t>(2U * capR * 16U) ? 1U : 0U;
        }
    }

    // 环带的内外边界，1/16 px 定标。取 (R−THICK+0.5) 与 (R+0.5) 而不是整数
    // R−THICK / R，是为了让连续极限正好等于旧画法「取 r 与 r−1 两个整数半径」
    // 的实际着墨面积 —— 换句话说抗锯齿只改边缘质感，不改环的视觉粗细。
    int16_t inner16[kRingCount];
    int16_t outer16[kRingCount];
    for (uint8_t i = 0; i < kRingCount; ++i) {
        inner16[i] = (rings[i].outerRadius - thickness) * 16 + 8;
        outer16[i] = rings[i].outerRadius * 16 + 8;
    }

    // 逐像素遍历包围盒。包围盒随外径平方增长（stock 41²=1681、balance 153²=23409），
    // 属可接受的一次性开销（推送 5min 一次 / 入场一次）。
    for (int16_t y = boxY; y < boxY + boxSize; ++y) {
        for (int16_t x = boxX; x < boxX + boxSize; ++x) {
            const int16_t dx = x - centerX;    // 右正
            const int16_t up = centerY - y;    // 上正
            // 到圆心的距离，1/16 px 定标：isqrt(n·2^8) 恰是 16·√n。
            const int16_t r16 = static_cast<int16_t>(
                isqrt32(static_cast<uint32_t>(dx * dx + up * up) << 8));
            // 本像素在径向覆盖 [r−0.5, r+0.5]
            const int16_t low = r16 - 8;
            const int16_t high = r16 + 8;
            for (uint8_t i = 0; i < kRingCount; ++i) {
                // 环带覆盖率 0..255（像素自身径向宽 1px = 16 个 1/16 单位）
                const int16_t from = low > inner16[i] ? low : inner16[i];
                const int16_t to = high < outer16[i] ? high : outer16[i];
                const uint8_t bandCov =
                    to > from ? static_cast<uint8_t>((to - from) * 255 / 16) : 0U;
                // 扇形归属：形状 1 恒在弧上；形状 2/3 用叉积判端点方向；形状 4 恒为
                // false（单点态只有圆盘着墨，环带基准是轨道 —— 绝不能当成小角扇形）。
                // 判定点取像素中心在外缘半径上的径向投影 (ox,ou) = (dx,up)·R_outer/dist
                // —— 复用本像素已算出的 r16 作 dist（1/16px 定点），使弧端切口
                // 与外缘切线对齐；r16==0（圆心像素）无方向，跳过（本就不在任何环带）。
                bool sector = false;
                if (shape[i] == 1U) {
                    sector = true;
                } else if ((shape[i] == 2U || shape[i] == 3U) && r16 != 0) {
                    const int32_t ox =
                        static_cast<int32_t>(dx) * rings[i].outerRadius * 16 / r16;
                    const int32_t ou =
                        static_cast<int32_t>(up) * rings[i].outerRadius * 16 / r16;
                    const int32_t cross =
                        static_cast<int32_t>(sinT[i]) * ou -
                        static_cast<int32_t>(cosT[i]) * ox;
                    sector = shape[i] == 2U ? (cross >= 0 && ox >= 0)
                                            : (cross > 0 || ox >= 0);
                }
                // 圆头帽独立于环带径向区间判定（帽盘可在端角外，仍属本环）；
                // 帽盘正好等于本环横截面，故不会命中邻环的带 / 空档。形状 4 把起终帽心
                // 设成同一点，这里算出的 capCov 即 12 点那个单点圆盘的覆盖率（点恒画）。
                // 形状 2/3 仅当 drawCaps（帽心弦长 ≥ 两帽直径）时才画端帽；否则
                // capCov=0，弧带仍由上面 sector 判定着色、径向 AA 照做 —— 短弧保留
                // 原始弧长，不会被端帽融合撑宽。
                uint8_t capCov = 0U;
                const bool capActive =
                    shape[i] == 4U ||
                    ((shape[i] == 2U || shape[i] == 3U) && drawCaps[i] != 0U);
                if (capActive) {
                    const int32_t sdx = static_cast<int32_t>(dx) * 16;
                    const int32_t sup =
                        static_cast<int32_t>(up) * 16 - capStartUp16[i];
                    const int16_t sdist = static_cast<int16_t>(
                        isqrt32(static_cast<uint32_t>(sdx * sdx + sup * sup)));
                    const int32_t edx =
                        static_cast<int32_t>(dx) * 16 - capEndDx16[i];
                    const int32_t eup =
                        static_cast<int32_t>(up) * 16 - capEndUp16[i];
                    const int16_t edist = static_cast<int16_t>(
                        isqrt32(static_cast<uint32_t>(edx * edx + eup * eup)));
                    capCov = capCoverage(sdist, edist, capR);
                }
                // 环带内像素**恒着墨**（在扇形内是弧色，否则是轨道色），所以
                // 基准覆盖率就是 bandCov；帽盘可在扇形外单独补墨，取 max。
                uint8_t total = bandCov;
                if (capCov > total) {
                    total = capCov;   // 帽与带取 max：每像素至多混色一次
                }
                if (total == 0U) {
                    continue;   // 本环在此像素无着墨，试下一条环带
                }
                const bool onArc = sector || capCov != 0U;
                const uint16_t base = onArc ? rings[i].arc565 : rings[i].track565;
                // 环带**内外两侧都是页面底色 bg565**，轨道只存在于环带之内 ——
                // 所以径向抗锯齿要往 bg565 混。往轨道色混会给每个环外缘糊上一层
                // 同色光晕，既虚胖又把环色洗淡。lerp565 输入是已交换色序的值。
                gfx->drawPixel(x, y, total == 255U
                                             ? base
                                             : lerp565(bg565, base, total));
                break;   // 三条环带彼此隔空档，一个像素只可能命中一条
            }
        }
    }
}

}  // namespace QuotaRingRenderer
