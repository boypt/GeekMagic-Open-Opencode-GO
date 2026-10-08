// SPDX-License-Identifier: GPL-3.0-or-later
#include "opencodego/UsageManager.h"

#include <Logger.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <time.h>

#include "display/DisplayManager.h"
#include "opencodego/OpenCodeLogo.h"
#include "wireless/WiFiManager.h"

extern WiFiManager* wifiManager;

// Arduino_GFX 不继承 Adafruit_GFX，而 U8g2_for_Adafruit_GFX 解码器对显示对象
// 只调用 drawFastHLine/drawFastVLine（含 drawPixel 纯虚），用一个最小 shim
// 适配到 DisplayManager::getGfx() 的 Arduino_GFX。
class U8g2GfxShim : public Adafruit_GFX {
   public:
    explicit U8g2GfxShim(Arduino_GFX* target) : Adafruit_GFX(240, 240), _target(target) {}
    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        _target->drawPixel(x, y, color);
    }
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
        _target->drawFastHLine(x, y, w, color);
    }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
        _target->drawFastVLine(x, y, h, color);
    }

   private:
    Arduino_GFX* _target;
};

static U8g2GfxShim& u8g2Shim() {
    static U8g2GfxShim shim(DisplayManager::getGfx());
    return shim;
}

static U8G2_FOR_ADAFRUIT_GFX& u8g2() {
    static U8G2_FOR_ADAFRUIT_GFX font = [] {
        U8G2_FOR_ADAFRUIT_GFX u;
        u.begin(u8g2Shim());
        u.setFontMode(1);  // 透明字形，清底由局部重绘 fillRect 负责
        return u;
    }();
    return font;
}

// ---- 上位机推送缓冲（静态定长，零堆常驻；时间戳见 pushBalance）----
static char s_rowLabels[UsageManager::kBalanceLines][UsageManager::kRowLabelCap] = {{0}};
static char s_rowResets[UsageManager::kBalanceLines][UsageManager::kRowResetCap] = {{0}};
static uint8_t s_rowCount = 0;
static char s_status[UsageManager::kStatusCap] = {0};
static bool s_hasPush = false;
static bool s_hasStatus = false;
static int s_rowProgress[UsageManager::kBalanceLines] = {-1, -1, -1};
static time_t s_pushEpoch = 0;      // 推送时刻 UTC epoch；推送时 NTP 未同步则为 0
static uint32_t s_pushMillis = 0;   // 推送时刻 millis（age_s 基准，未同步时也有效）

// ---- 时区（同旧工程 src/config.h：北京时间 UTC+8）----
// GeekMagic NTPClient 用 configTime(0, 0, ...) 同步，time(nullptr) 为 UTC epoch；
// 本地时间 = UTC epoch + TZ_OFFSET_SEC，再用 gmtime 解释。
static constexpr uint32_t TZ_OFFSET_SEC = 8UL * 3600UL;
static constexpr time_t TIME_MIN_VALID = 1600000000;  // 同 NTPClient REASONABLE_EPOCH

// ---- 主题色（源值按直觉写真 RGB；本助手负责面板 BGR 色序的 R/B 字段交换）----
// 本面板 lcd_bgr=true → MADCTL 带 BGR 位，送入的标准 RGB565 会被面板反着
// 解释 R/B。位图路径（logo / 相册 / 实时帧）在写入前各自做了同一交换，
// 主题色经此助手保持同一口径，否则屏上红蓝会颠倒。
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(r & 0xF8) << 8) |
                                              (static_cast<uint16_t>(g & 0xFC) << 3) | (b >> 3));
    return static_cast<uint16_t>(((v & 0x001FU) << 11) | (v & 0x07E0U) | ((v & 0xF800U) >> 11));
}
// 暖色主题：底色压成极深暖棕，营造「余温 / 炭火」氛围；文字类（C_SUB/C_LABEL/
// C_WHITE）刻意保持高明度，暖色系里小字最容易被暗棕吞掉。
static constexpr uint16_t C_BG     = rgb565(0x12, 0x0A, 0x06);  // 极深暖棕（炭底）
static constexpr uint16_t C_BORDER = rgb565(0x40, 0x28, 0x1A);  // logo 下横线：深棕
static constexpr uint16_t C_SUB    = rgb565(0xB0, 0x8A, 0x6A);  // 次级文字：暖灰褐
static constexpr uint16_t C_LABEL  = rgb565(0xC9, 0xA2, 0x77);  // 引导文字：暖沙色
static constexpr uint16_t C_WHITE  = rgb565(0xFF, 0xF2, 0xE3);  // 时钟数字：暖白
static constexpr uint16_t C_RED    = rgb565(0xFF, 0x3B, 0x30);  // 失败提示：高对比纯红

// 额度三重同心环（颜色复制自 StockScene 的身份配色，仅放大几何）：
// 颜色**按行固定**、不随百分比变 —— 三个环各占一个身份色，数值完全由弧长
// 表达，色相只负责分辨「哪一个环是 5H / WK. / MO.」。Apple Watch 活动环的
// 身份配色（粉 / 绿 / 青），每环配一条**同色相压暗的独立轨道**。
// 源值仍是真 RGB，由 rgb565() 做 BGR 交换（与全项目同一口径）。
static constexpr uint16_t C_ARC_5H = rgb565(0xFA, 0x11, 0x4F);  // 外环 5H：Apple Move 粉
static constexpr uint16_t C_ARC_WK = rgb565(0x92, 0xE8, 0x2A);  // 中环 WK.：Apple Exercise 绿
static constexpr uint16_t C_ARC_MO = rgb565(0x00, 0xE5, 0xFF);  // 内环 MO.：Apple Stand 青
static constexpr uint16_t C_TRACK_5H = rgb565(0x4A, 0x0E, 0x22);  // 外环轨道：暗栗（同环压暗）
static constexpr uint16_t C_TRACK_WK = rgb565(0x24, 0x38, 0x0A);  // 中环轨道：暗绿（同环压暗）
static constexpr uint16_t C_TRACK_MO = rgb565(0x06, 0x3B, 0x42);  // 内环轨道：暗青（同环压暗）
// 行序 0/1/2 = 5H / WK. / MO.，与推送三段同序
static constexpr uint16_t C_ARC_ROW[UsageManager::kBalanceLines] = {
    C_ARC_5H, C_ARC_WK, C_ARC_MO};
static constexpr uint16_t C_TRACK_ROW[UsageManager::kBalanceLines] = {
    C_TRACK_5H, C_TRACK_WK, C_TRACK_MO};

// ---- 字体（U8g2_for_Adafruit_GFX）----
// U8g2 文字：balance 主页面只有圆环上角标签用 FONT_MINI（见下），boot 页
// 提示语与 clock 页日期行用 Helvetica Regular 10pt（u8g2_font_helvR10_tf）；
// 环 + 像素时钟都是 gfx 直绘，不走字模。
static const uint8_t* const FONT_LABEL = u8g2_font_helvR10_tf;

// ---- 布局常量（balance 主页面：logo + 居中巨型同心额度环 + 上角迷你标签 + 底部像素时钟）----
//   ┌─ logo(居中, y=1..32) ─ 横线 y=40 ────────────┐
//   │ UPD   ┃      三重同心额度环（放大版）     ┃ RST   │ 擦底框 x/y 44..196
//   │ HH:MM ┃      外 5H / 中 WK. / 内 MO.     ┃ 5H: … │ 圆心 (120,120)
//   │       ┃                                 ┃ 1W: … │ 标签行带：左 x0..43
//   │       ┃                                 ┃ 1M: … │           右 x197..239
//   └─ HH:MM:SS (textSize 3, y=204) ───────────┘ 擦除带 y=202..229
//
// 环几何 = stock 场景右下角圆环的放大版，推导关系不变：
//   THICK=16、CAP_R=THICK/2=8、MID_R=R−CAP_R；
//   径向带 外 [60,76] / 中 [36,52] / 内 [12,28]，环间空档 8px、中心 ⌀24px 空洞；
//   弧两端半径 8px 的圆头帽正好填满所在环带的 16px 横截面、不外溢到邻环。
static constexpr int RING_CX = 120;
static constexpr int RING_CY = 120;
static constexpr int RING_R[UsageManager::kBalanceLines] = {76, 52, 28};  // 各环外缘
static constexpr int RING_THICK = 16;                       // 每环 16px 描边（向内取）
static constexpr int RING_CAP_R = RING_THICK / 2;           // 圆头帽半径 = 8px
static constexpr int RING_MID_R[UsageManager::kBalanceLines] = {
    RING_R[0] - RING_CAP_R, RING_R[1] - RING_CAP_R, RING_R[2] - RING_CAP_R};
// 擦底方框 = (外径+1)² = 153²，以圆心居中对称（x/y 44..196）。
// 与相邻元素两个方向都错开：logo 横线 y=40（下沿空 3px）、时钟擦除带
// y≥202（上沿空 5px）→ 时钟整条擦除绝不与环框相交。
static constexpr int RING_BOX = 153;
static constexpr int RING_X = RING_CX - RING_R[0];  // 44
static constexpr int RING_Y = RING_CY - RING_R[0];  // 44

// 底部居中像素时钟：textSize 3 每字 18px，"00:00:00" 共 144px，x=48..191。
// 分段擦除只在自己条带内（整条 x=46..193 / y=202..229），不整行横扫。
static constexpr int CLOCK_X = 48;   // (240-144)/2 居中
static constexpr int CLOCK_Y = 204;
static constexpr int CLOCK_W = 144;  // == textWidthPx("00:00:00", 3)

// ---- 圆环上角迷你标签（左 UPD 两行 / 右 RST 四行）----
// 字体 FONT_MINI = u8g2_font_4x6_tf：4px 定宽、字高 6px（ascent_A=5，字形实占
// y_top..y_top+5），是本固件最小的全 ASCII 字模。选它而不选 gfx 内建字体
// textSize 1（6px/字）是几何决定：右块示例行 `5H: HH:MM` 共 9 字符，4px 字宽
// 36px 放得进 43px 右边距，6px 字宽要 54px 放不下。
// 位置贴环框上角：左块文字右对齐 x=43（紧贴环框左缘 RING_X=44）、右块文字
// 左对齐 x=197（紧贴环框右缘 RING_X+RING_BOX），两块首行字形顶 == 环框顶缘
// RING_Y=44 → 块角正好咬住环擦底框的上角。行距 8px == 行带高 8px，相邻行带
// 无缝拼接；每行只清自己那条行带（绝不横扫整行），且行带 x/y 全程在环擦底框
// （x/y 44..196）与底部时钟擦除带（x46..193/y202..229）之外。
static const uint8_t* const FONT_MINI = u8g2_font_4x6_tf;
static constexpr int MINI_PITCH = 8;   // 行距 == 行带高：相邻行带无缝拼接不重叠
static constexpr int MINI_BAND_H = 8;  // 字形 6px（y_top..y_top+5）+ 上下各 1px 余量
// 渲染行容量 22：行带最多显示 10~11 字符（含 '~'），上游串再长也先截存
// 再按带宽收缩，快照只存实际显示内容
static constexpr size_t kMiniLineCap = 22;
// 左块 UPD：标题行 + 更新时刻值行（右对齐，行带 x=0..43 恒在环框外）
static constexpr int UPD_BAND_X = 0;
static constexpr int UPD_BAND_W = RING_X;             // 44 → 行带 x 0..43
static constexpr int UPD_TEXT_RIGHT = RING_X - 1;     // 43：文字右缘贴环框左上角
static constexpr int UPD_Y = RING_Y;                  // 44：首行顶 == 环框顶缘
// 右块 RST：标题行 + 5H/1W/1M 三行重置值（左对齐，行带 x=197..239 恒在环框外）
static constexpr int RST_BAND_X = RING_X + RING_BOX;  // 197
static constexpr int RST_BAND_W = 240 - RST_BAND_X;   // 43 → 行带 x 197..239
static constexpr int RST_TEXT_X = RST_BAND_X;         // 197：文字左缘贴环框右上角
static constexpr int RST_Y = RING_Y;                  // 44：与左块同高对齐

// ---- 运行状态 ----
static bool ever_started = false;

// 上次绘制的本地分钟（hour*60+min）/ 秒；-1 = 尚未绘制
static int lastClockMinute = -1;
static int lastClockSecond = -1;

// 主页面是否已绘制：只有绘制过后才允许时钟 tick / 局部重绘（boot 页除外）
static bool mainPageDrawn = false;

// ---------- 屏上额度快照（推送局部更新基准）----------
// 旧布局的行快照（标签/重置/进度三维度 134B）随三段进度条一起删掉：屏上数据
// 主体是三个百分比（环弧长），快照也只存三个 int（-1 = 空槽）→ 12B 静态、零堆。
static int s_shownQuota[UsageManager::kBalanceLines] = {-1, -1, -1};
// 圆环上角标签的屏上快照（渲染后串，快照 == 屏上像素）：UPD 值行 +
// RST 三行显示行。标题行 UPD/RST 是常量文案，只有整屏/入场才画，无需快照。
static char s_shownUpd[kMiniLineCap] = {0};
static char s_shownReset[UsageManager::kBalanceLines][kMiniLineCap] = {{0}};
// 快照是否可信（整屏绘制或局部重画后才为 true；退场/未画过主页面时为 false）
static bool s_shownValid = false;
// 推送到达待刷新标志：只走局部更新，不与显示设置变更的整屏重绘共用一个信号
static bool s_pushRefreshPending = false;

// 当前应上屏的第 index 槽位百分比（-1 = 无值空槽，只留轨道）。
// setRowProgress 已钳到 0..100/-1，pushBalance 也会清掉本次行数外的旧值，
// 所以直接读推送缓冲即可，无需再做显示回落（labels 不上屏；status/resets
// 在右上/左上标签块原样展示，见 buildMiniValueLine）。
static int ringPercent(uint8_t index) {
    return index < UsageManager::kBalanceLines ? s_rowProgress[index] : -1;
}

// 绘制后同步快照（drawQuotaRings 收尾调用），保证「快照 == 屏上像素」，
// 整屏与局部两条路径共用同一套记录逻辑。
static void captureQuotaShown() {
    for (uint8_t i = 0; i < UsageManager::kBalanceLines; i++) {
        s_shownQuota[i] = ringPercent(i);
    }
    s_shownValid = true;
}

// ---------- 本地时间工具 ----------
static bool timeSynced() { return time(nullptr) > TIME_MIN_VALID; }

// 当前本地时间字段（UTC epoch + 时区偏移后按 UTC 解释）
static struct tm localTmNow() {
    time_t t = time(nullptr) + TZ_OFFSET_SEC;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    return tmv;
}

static int currentLocalMinute() {
    if (!timeSynced()) return -1;
    struct tm tmv = localTmNow();
    return tmv.tm_hour * 60 + tmv.tm_min;
}

static int currentLocalSecond() {
    if (!timeSynced()) return -1;
    return localTmNow().tm_sec;
}

// ---------- 界面工具 ----------
// 调用前须 u8g2().setFont(...)；测宽与绘制用同一字体。
// 参数 y 为字形顶部（同旧 TFT_eSPI TL_DATUM），内部按当前字体 ascent 折算基线
static void drawText(int x, int y, const char* s, uint16_t color) {
    u8g2().setForegroundColor(color);
    u8g2().drawUTF8(static_cast<int16_t>(x),
                    static_cast<int16_t>(y + u8g2().getFontAscent()), s);
}

static int textWidth(const char* s) {
    return static_cast<int>(u8g2().getUTF8Width(s));
}

// Arduino_GFX 的内建 6px 字体没有测量 API，按其实际字宽做像素测量（同 Scenes.cpp）。
static int textWidthPx(const char* text, int textSize) {
    return text == nullptr ? 0 : static_cast<int>(strlen(text)) * 6 * textSize;
}

// logo 位图恢复 16bpp 原始位图（23 色保真），按 TFT_eSPI pushImage 字节序
// 预交换，此处还原为标准 RGB565；0x0000 为透明色，只绘制字标像素。
// 位置/尺寸/逐点绘制路径不变；位图存 flash(.irom.text.progmem，~11.4KB、
// 0 RAM)，读取走 pgm_read_word（32-bit 访问，见 IromAccess.h）。
// 【字节序证据】TFT_eSPI 中：
//  - Processors/TFT_eSPI_ESP8266.c: pushPixels(const void*) 直接
//    spi.writePattern(data,...) 原样发字节、不做交换；而 tft_Write_16(C)
//    定义为 (C)<<8|(C)>>8 内部交换（TFT_eSPI_ESP8266.h:161）；
//  - TFT_eSPI.cpp pushImage(PROGMEM data, transp)：buffer[] = pgm_read_word
//    原值不经交换交给 pushPixels → 表内字节序即 SPI 发送序（高字节次序由
//    表内容决定）→ oc_logo 表为「预交换（大端）」字节序，非真实 RGB565；
//  - 本工程 Arduino_GFX 期望真实 RGB565，故绘制前须 (c<<8)|(c>>8) 还原，
//    与旧固件颜色一致【交换必须在判透明之前还原？见下】——透明判定：
//    旧代码 `if (!_swapBytes) transp = transp>>8|transp<<8;` 后比较的是
//    表内原始值 vs 交换后的 transp（0x0000 交换不变）→ 即按表内原始值判 0；
//    新代码 (c==0) 先判再交换，语义一致。旧工程未调用 setSwapBytes（全仓
//    grep 无），_swapBytes 默认 false（TFT_eSPI.cpp:463）。
static void drawLogo(int x, int y) {
    auto* gfx = DisplayManager::getGfx();
    for (int j = 0; j < OC_LOGO_H; j++) {
        const uint16_t* row = &OC_LOGO_BITS[j * OC_LOGO_W];
        for (int k = 0; k < OC_LOGO_W; k++) {
            uint16_t c = static_cast<uint16_t>(pgm_read_word(&row[k]));
            if (c == OC_LOGO_NO_COLOR) {
                continue;
            }
            c = static_cast<uint16_t>((c << 8) | (c >> 8));
            gfx->drawPixel(static_cast<int16_t>(x + k), static_cast<int16_t>(y + j),
                           c);
        }
    }
}

// ---------- 七段数码管大字（像素级复刻旧工程 TFT_eSPI Font 7）----------
// 字模见 include/opencodego/SegFont7.h：由 TFT_eSPI Font7srle.c 的 8-bit RLE
// 原始字模按 drawChar() 同款算法逐像素解码的 1bpp 行位图（32x48/字符，
// MSB 左），渲染逐像素与旧固件一致。数字格 32x48，'-' 占满 32px 宽
// （同旧固件字宽表 widtbl_f7s），'-' 前备用的 ':' 实际 12px 宽。
// 渲染：每行把连续置位段合并为 drawFastHLine（等价旧固件 textsize=1 的
// fillRect 行内 run 绘制），只画字不填底；颜色 C_WHITE。
// 表存 flash，读取走 pgm_read_dword（IROM 32-bit 访问约束，见 IromAccess.h）。
// 现仅 clock 场景页使用（balance 主页面的时钟已改 textSize 3 像素时钟）。

#include "opencodego/SegFont7.h"

// 单个数字字符（'0'-'9' / '-' / ':'，':' 宽 12 其余 32），左上角 (x, y)
static void drawSegDigit(int x, int y, char ch, uint16_t color) {
    int idx;
    if (ch >= '0' && ch <= '9') {
        idx = ch - '0';
    } else if (ch == '-') {
        idx = SEG7_DASH;
    } else if (ch == ':') {
        idx = SEG7_COLON;
    } else {
        return;
    }
    auto* gfx = DisplayManager::getGfx();
    for (int row = 0; row < SEG7_H; row++) {
        uint32_t bits = pgm_read_dword(&kSeg7Font[idx][row]);
        if (bits == 0) continue;
        int k = 0;
        while (k < SEG7_W) {
            if (!(bits & (1u << (31 - k)))) {
                k++;
                continue;
            }
            int run = 0;
            while (k + run < SEG7_W && (bits & (1u << (31 - (k + run))))) run++;
            gfx->drawFastHLine(static_cast<int16_t>(x + k),
                               static_cast<int16_t>(y + row),
                               static_cast<int16_t>(run), color);
            k += run;
        }
    }
}

// ---------- 额度环的数学工具（复制自 Scenes.cpp 的 StockScene::drawQuota 同款）----------
// 1/255 定标的正弦表（0..90°，四分之一波）。画圆弧要取圆周坐标，为了不把 libm 的
// sinf/cosf（连带浮点库和一堆代码）带进固件，这里用 91 字节查表 + 象限映射，误差
// ≤ 0.5/255 ≈ 0.04px，240×240 屏上完全看不出来。91 字节常驻静态区（.rodata），
// 不参与堆分配。
static const uint8_t kSinUnitDeg[91] = {
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
// 在 r=76 处就是 0.66px 的错位；0.1° 只剩 0.07px。
static int16_t sinUnitTenths(uint16_t tenths) {
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
static int16_t cosUnitTenths(uint16_t tenths) {
    return sinUnitTenths(static_cast<uint16_t>(tenths + 900U));
}

// 32 位整数平方根（逐位恢复法）。算像素到圆心的距离要用到它 —— 引入 sqrtf() 连带
// libm 与浮点库，正是上面那张正弦表要绕开的东西，所以这里手写整数版。
static uint16_t isqrt32(uint32_t value) {
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
// BGR 交换是固定通道置换，与逐通道 lerp 可交换，所以直接在本文件 rgb565() 打包
// 出来的值上插即可，不必回到源 RGB 再转换。三段通道位宽不同（5/6/5），
// 必须各自在自己的位宽里插完再拼回去，跨位宽插会偏色。
static uint16_t lerp565(uint16_t from, uint16_t to, uint8_t ratio) {
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
// 的 1/16 px 距离。帽半径 RING_CAP_R(8)px = RING_CAP_R·16 单位，外缘再留 0.5px
// （8 单位）渐隐，与环带边缘同一口径；两端帽取覆盖率较大者（1~3% 时两帽重叠，
// 取 max 防止重复混色）。渐隐带恒 1px（16 单位），与 stock 场景同口径。
static uint8_t capCoverage(int16_t dStart, int16_t dEnd) {
    const int16_t dist = dStart < dEnd ? dStart : dEnd;
    int32_t edge = RING_CAP_R * 16 + 8 - dist;
    if (edge <= 0) {
        return 0U;
    }
    if (edge > 16) {
        edge = 16;              // 1px 宽的边缘过渡，内部恒为满覆盖
    }
    return static_cast<uint8_t>(edge * 255 / 16);
}

// ---------- 三重同心额度环（居中放大版）----------
// 外/中/内 = 5H / WK. / MO.，无任何文字；弧长 = 百分比（唯一的数据通道，
// 50% 半圈、100% 整圈），环色 = 行身份，不随数值变。从 12 点起顺时针扫：
// x = cx + r·sin(deg)，y = cy − r·cos(deg)。轨道恒画满整圈（无值 -1 / 0%
// 只留轨道，没有任何数据时也是三圈空轨道）。
// 环区自清底（fillRect 只覆盖居中 153×153 框），本身就是自洽的局部更新单元，
// 不碰 logo/横线/底部时钟（时钟擦除带 y≥202，与环框 y≤196 错开）。
//
// 画法：**逐像素距离场 + 径向覆盖率抗锯齿**（复制自 StockScene::drawQuota，
// 仅放大几何常量）。不要改回按角度逐点落笔 —— 旧画法有三处根因：
//   ① 相邻角度大量落在同一像素互相覆盖、偶尔跳 2px；
//   ② 坐标 `>>16` 对负数是向 −∞ 取整，圆心左右两侧取整规律不一致；
//   ③ 整数半径硬切无法表达亚像素覆盖，内外缘必然生硬。
// 遍历包围盒每个像素：整数开方算到圆心距离，落在环带内的按覆盖率混色 ——
// 径向完全对称、每像素只画一次、内外缘真抗锯齿。包围盒 153²=23k 像素随外径
// 平方增长，属可接受的一次性开销（推送 5min 一次 / 入场一次）。
// 相邻两环不会互相污染：环带之间有 8px 空白，抗锯齿最多向外洇 0.5px；帽盘
// 半径取描边一半（8px）、圆心在中径上，恰好等于本环 16px 的横截面，也不会
// 伸进邻环或空档。三带互不重叠，一个像素至多命中一条环带。
//
// 圆头帽（Apple 活动环的标志性端头）：仅 0 < percent < 100 时画。在弧的起、
// 终两个端点各放一个半径 8px 的圆盘，圆心落在环带中径、位于端角上；圆盘会向
// 扇形角外鼓出，把生硬的平切口变成圆头。帽盘的着墨判定**独立于**环带的径向
// 区间（端角外的像素不在扇形内，却仍在帽内），两者覆盖率取 max 合并，于是每
// 像素至多混色一次；1~3% 时起终两帽重叠，取 max 同样避免重复混色。
//
// 扇形边界（弧的端头）用**叉积**判，不比较角度也不比较余弦：
//   A = cross(vΘ, P) = sinΘ·u − cosΘ·dx   —— A ≥ 0 ⇔ α ≤ Θ（在 α ≥ Θ−180 支上）
//   B = dx ≥ 0                            —— ⇔ α ∈ [0,180]
// 凸扇形 Θ≤180 取 A ≥ 0 且 B；反射扇形 Θ>180 取补得 A > 0 或 B。判据对 P
// 线性，误差只有坐标的 1/255。别用「cos α ≥ cos Θ/2」半角写法：cos 在 0 附近
// 极平坦，1% 档（Θ=3.6°）的余弦差不足半个 LSB，会整圈判反。
void UsageManager::drawQuotaRings() {
    auto* gfx = DisplayManager::getGfx();
    // 只擦环区（居中 153×153 框 x/y 44..196），不碰 logo/横线/底部时钟。
    gfx->fillRect(RING_X, RING_Y, RING_BOX, RING_BOX, C_BG);

    // 每个环的扇形边界方向预先算好（1/255 定标），像素循环里只做点积。
    // 形状 0 = 无弧（无值 -1 与 0% 都只剩轨道）、1 = 整圈、2 = 凸扇形、
    // 3 = 反射扇形。没有额度数据时三段必全是 -1 → 三圈轨道照画，不提前返回。
    uint8_t shape[kBalanceLines] = {};
    int16_t sinT[kBalanceLines] = {};
    int16_t cosT[kBalanceLines] = {};
    // 圆头帽的圆心（仅形状 2/3 有效）：起点在角度 0、终点在角度 Θ，均落在
    // 环带中径上。帽心以 1/16 px 定点存储，像素循环里不再做除法：
    //   起点 (dx, up) = (0, midR)
    //   终点 (dx, up) = (midR·sinΘ/255, midR·cosΘ/255)
    int16_t capStartUp16[kBalanceLines] = {};
    int16_t capEndDx16[kBalanceLines] = {};
    int16_t capEndUp16[kBalanceLines] = {};
    for (uint8_t i = 0; i < kBalanceLines; ++i) {
        const int percent = ringPercent(i);
        if (percent <= 0) {
            shape[i] = 0U;
        } else if (percent >= 100) {
            shape[i] = 1U;   // 整圈：上面两个判据在 Θ=360° 都退化，单独短路
        } else {
            // Θ = 3.6·percent 度 = percent·36 个 0.1°；交叉相乘比不出浮点。
            const uint16_t tenths = static_cast<uint16_t>(percent) * 36U;
            sinT[i] = sinUnitTenths(tenths);
            cosT[i] = cosUnitTenths(tenths);
            shape[i] = percent <= 50 ? 2U : 3U;
            const int32_t mid16 = static_cast<int32_t>(RING_MID_R[i]) * 16;
            capStartUp16[i] = static_cast<int16_t>(mid16);
            const int32_t ex = mid16 * sinT[i];
            const int32_t ey = mid16 * cosT[i];
            capEndDx16[i] = static_cast<int16_t>(
                ex >= 0 ? (ex + 127) / 255 : -((-ex + 127) / 255));
            capEndUp16[i] = static_cast<int16_t>(
                ey >= 0 ? (ey + 127) / 255 : -((-ey + 127) / 255));
        }
    }

    // 环带的内外边界，1/16 px 定标。取 (R−THICK+0.5) 与 (R+0.5) 而不是整数
    // R−THICK / R，是为了让连续极限正好等于旧画法「取 r 与 r−1 两个整数半径」
    // 的实际着墨面积 —— 换句话说抗锯齿只改边缘质感，不改环的视觉粗细。
    int16_t inner16[kBalanceLines];
    int16_t outer16[kBalanceLines];
    for (uint8_t i = 0; i < kBalanceLines; ++i) {
        inner16[i] = (RING_R[i] - RING_THICK) * 16 + 8;
        outer16[i] = RING_R[i] * 16 + 8;
    }

    // 153×153 = 23409 次迭代（包围盒随外径平方增长，可接受）。
    for (int16_t y = RING_Y; y < RING_Y + RING_BOX; ++y) {
        for (int16_t x = RING_X; x < RING_X + RING_BOX; ++x) {
            const int16_t dx = x - RING_CX;    // 右正
            const int16_t up = RING_CY - y;    // 上正
            // 到圆心的距离，1/16 px 定标：isqrt(n·2^8) 恰是 16·√n。
            const int16_t r16 = static_cast<int16_t>(
                isqrt32(static_cast<uint32_t>(dx * dx + up * up) << 8));
            // 本像素在径向覆盖 [r−0.5, r+0.5]
            const int16_t low = r16 - 8;
            const int16_t high = r16 + 8;
            for (uint8_t i = 0; i < kBalanceLines; ++i) {
                // 环带覆盖率 0..255（像素自身径向宽 1px = 16 个 1/16 单位）
                const int16_t from = low > inner16[i] ? low : inner16[i];
                const int16_t to = high < outer16[i] ? high : outer16[i];
                const uint8_t bandCov =
                    to > from ? static_cast<uint8_t>((to - from) * 255 / 16) : 0U;
                // 扇形归属：形状 1 恒在弧上；形状 2/3 用叉积判端点方向。
                // 判定点取像素中心在外缘半径上的径向投影 (ox,ou) = (dx,up)·R_outer/dist
                // —— 复用本像素已算出的 r16 作 dist（1/16px 定点），使弧端切口
                // 与外缘切线对齐；r16==0（圆心像素）无方向，跳过（本就不在任何环带）。
                bool sector = false;
                if (shape[i] == 1U) {
                    sector = true;
                } else if (shape[i] != 0U && r16 != 0) {
                    const int32_t ox =
                        static_cast<int32_t>(dx) * RING_R[i] * 16 / r16;
                    const int32_t ou =
                        static_cast<int32_t>(up) * RING_R[i] * 16 / r16;
                    const int32_t cross =
                        static_cast<int32_t>(sinT[i]) * ou -
                        static_cast<int32_t>(cosT[i]) * ox;
                    sector = shape[i] == 2U ? (cross >= 0 && ox >= 0)
                                            : (cross > 0 || ox >= 0);
                }
                // 圆头帽独立于环带径向区间判定（帽盘可在端角外，仍属本环）；
                // 帽盘正好等于本环 16px 横截面，故不会命中邻环的带/空档。
                uint8_t capCov = 0U;
                if (shape[i] == 2U || shape[i] == 3U) {
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
                    capCov = capCoverage(sdist, edist);
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
                const uint16_t base = onArc ? C_ARC_ROW[i] : C_TRACK_ROW[i];
                // 环带**内外两侧都是页面底色（C_BG 暖棕）**，轨道只存在于环带
                // 之内 —— 所以径向抗锯齿要往 C_BG 混。往轨道色混会给每个环外缘
                // 糊上一层同色光晕，既虚胖又把环色洗淡。lerp565 输入是已交换
                // 色序的值，勿二次交换。
                gfx->drawPixel(x, y, total == 255U
                                             ? base
                                             : lerp565(C_BG, base, total));
                break;   // 三条环带彼此隔 8px 空白，一个像素只可能命中一条
            }
        }
    }

    captureQuotaShown();  // 画完即同步屏上快照（整屏/局部两条路径共用）
}

// ---------- 底部居中像素时钟（HH:MM:SS，textSize 3）----------
// 复用 stock 场景 drawClock 的分段擦除模式：分钟字段变化才擦写 HH:MM:，
// 平时每秒只擦写 SS 两个字符（36px）；整条擦除只在入场/整屏重绘时做，
// 且只擦自己条带（x=46..193 / y=202..229，与环框 y≤196 不相交）。
// 未同步时显示 "--:--:--" 占位（ASCII only），同步后由 tickUi 差量接管。
void UsageManager::drawClock(bool minuteChanged, bool fullRedraw) {
    auto* gfx = DisplayManager::getGfx();
    char minuteText[8];
    char secondText[3];
    const bool synced = timeSynced();
    if (synced) {
        const struct tm tmv = localTmNow();
        strftime(minuteText, sizeof(minuteText), "%H:%M:", &tmv);
        strftime(secondText, sizeof(secondText), "%S", &tmv);
    } else {
        strlcpy(minuteText, "--:--:", sizeof(minuteText));
        strlcpy(secondText, "--", sizeof(secondText));
    }

    gfx->setTextSize(3);
    gfx->setTextColor(C_WHITE);
    // 整条时钟恒为 CLOCK_W 宽（字形宽度固定，数字位数不变），水平居中。
    const int16_t minuteWidth = textWidthPx("00:00:", 3);
    const int16_t secondX = CLOCK_X + minuteWidth;
    const int16_t secondWidth = textWidthPx("00", 3);
    if (fullRedraw) {
        // 只擦时钟自己那一条（x=46..193 / y=202..229），绝不能 fillRect(0,…,
        // 240,…) 横扫 —— 圆环擦底框占 x/y 44..196，整行擦除会把三层同心环抹掉。
        // 左右各留 2px 余量，保证字号下沿与首尾字形都不残留。
        gfx->fillRect(CLOCK_X - 2, CLOCK_Y - 2, CLOCK_W + 4, 28, C_BG);
    }
    if (fullRedraw || minuteChanged) {
        // 分钟跳变时整条擦除不会执行，必须先擦掉本区域再画，否则新旧分钟
        // 数字直接叠在一起（14:37 → 14:38 会看到 7 与 8 重影）。
        if (!fullRedraw) {
            gfx->fillRect(CLOCK_X, CLOCK_Y - 2, minuteWidth, 28, C_BG);
        }
        gfx->setCursor(CLOCK_X, CLOCK_Y);
        gfx->print(minuteText);
    }
    // 仅擦写秒字段，避免每秒把整条时钟重新推送到 SPI。
    // 高度与全量路径一致，避免字号下沿残留上一秒字形。
    gfx->fillRect(secondX, CLOCK_Y - 2, secondWidth, 28, C_BG);
    gfx->setCursor(secondX, CLOCK_Y);
    gfx->print(secondText);

    if (synced) {
        lastClockMinute = currentLocalMinute();
        lastClockSecond = currentLocalSecond();
    }
}

// 常驻 tick（update() 每轮进一次，仅变化时碰屏；全部走像素差量，无清底闪烁）：
//  - 分钟变化：擦写 HH:MM: 字段
//  - 秒变化：只擦写 SS 字段
//  未同步时保持 "--:--:--" 占位不动。
void UsageManager::tickUi() {
    if (!timeSynced()) {
        return;
    }
    const int m = currentLocalMinute();
    const int s = currentLocalSecond();
    if (m == lastClockMinute && s == lastClockSecond) {
        return;
    }
    drawClock(m != lastClockMinute, false);  // 内部记录 lastClock*
}

// ---------- 圆环上角迷你标签（左 UPD / 右 RST 两块）----------
// 设备零语义：值行内容 = 推送缓冲**原样**上屏，转换全在上位机
// tools/push_auto.py 预格式化（时间换算 / 5H·1W·1M 前缀 / 无值占位都在上游），
// 设备端不做任何时间或重置解析，只管排版：
//   i=0 UPD 行 ← status 原样（上游预格式化成 "HH:MM" 更新时刻），空 → "--:--"
//   i=1..3 RST 行 ← resets[i-1] 原样（上游预格式化的完整显示行，如
//            "5H: R14d3h"），空 → "--"
// 空串回落 "--:--"/"--" 只是「无值」显示占位（同额度空槽口径），非内容换算。

// 行宽超行带宽时逐字符收缩、末尾换 '~'（同 stock 场景名称截断口径）。
// 4px 定宽字模下 RST 行带 43px 可容 10 字符（40px），上游典型行
// "5H: R14d3h"（10 字符）正好不触发截断。
static void fitMiniLine(char* s, int maxWidth) {
    u8g2().setFont(FONT_MINI);
    size_t len = strlen(s);
    // 每轮缩短一个字符并在新的末位换 '~'（内容截断标记）；len>1 保底，
    // 最坏退化成单个 '~'，绝不空转。
    while (len > 1 && textWidth(s) > maxWidth) {
        len--;
        s[len - 1] = '~';
        s[len] = '\0';
    }
}

// 值行 i 的当前应上屏串（0 = UPD 值行，1..3 = RST 三行显示串）
static void buildMiniValueLine(uint8_t i, char* out, size_t cap) {
    const char* raw = (i == 0) ? s_status : s_rowResets[i - 1];
    const char* empty = (i == 0) ? "--:--" : "--";
    strlcpy(out, raw[0] != '\0' ? raw : empty, cap);
    fitMiniLine(out, i == 0 ? UPD_BAND_W : RST_BAND_W);
}

// 渲染串与屏上快照是否不一致（快照 == 屏上像素；相同 → 零绘制）
static bool miniLineChanged(uint8_t i, const char* text) {
    return strcmp(text, i == 0 ? s_shownUpd : s_shownReset[i - 1]) != 0;
}

// 画一条标签行：只清自己那条行带（MINI_BAND_H 高，绝不横扫整行）再写字。
// rightAlign = 左块右对齐到 alignX；否则从 alignX 左对齐起排。
static void drawMiniLineAt(int bandX, int bandW, int y, int alignX,
                           bool rightAlign, const char* text, uint16_t color) {
    auto* gfx = DisplayManager::getGfx();
    gfx->fillRect(bandX, y - 1, bandW, MINI_BAND_H, C_BG);
    u8g2().setFont(FONT_MINI);
    drawText(rightAlign ? alignX + 1 - textWidth(text) : alignX, y, text, color);
}

// 单条值行上屏并同步快照（差量路径用；标题行是常量，只在整组绘制里出现）
static void drawMiniValueLine(uint8_t i, const char* text) {
    if (i == 0) {
        drawMiniLineAt(UPD_BAND_X, UPD_BAND_W, UPD_Y + MINI_PITCH, UPD_TEXT_RIGHT,
                       true, text, C_WHITE);
        strlcpy(s_shownUpd, text, kMiniLineCap);
        return;
    }
    const uint8_t r = i - 1;
    drawMiniLineAt(RST_BAND_X, RST_BAND_W, RST_Y + (r + 1) * MINI_PITCH,
                   RST_TEXT_X, false, text, C_WHITE);
    strlcpy(s_shownReset[r], text, kMiniLineCap);
}

// 两块标签整组上屏（入场 / 整屏重绘路径）：标题行 C_SUB + 值行 C_WHITE，
// 逐行擦各自的行带，收尾同步值行快照。
void UsageManager::drawMiniLabels() {
    char line[kMiniLineCap];
    drawMiniLineAt(UPD_BAND_X, UPD_BAND_W, UPD_Y, UPD_TEXT_RIGHT, true,
                   "UPD", C_SUB);
    drawMiniLineAt(RST_BAND_X, RST_BAND_W, RST_Y, RST_TEXT_X, false,
                   "RST", C_SUB);
    for (uint8_t i = 0; i < 4; i++) {
        buildMiniValueLine(i, line, sizeof(line));
        drawMiniValueLine(i, line);
    }
}

// ---------- 页面组装 ----------
// 正文区域（居中额度环 + 圆环上角标签 + 底部像素时钟）；调用前须保证该区域已清底
void UsageManager::drawBody() {
    drawQuotaRings();
    drawMiniLabels();
    drawClock(true, true);
}

void UsageManager::drawMainPage() {
    u8g2();  // 惰性初始化 U8g2 字形引擎（boot / clock 页沿用）
    auto* gfx = DisplayManager::getGfx();
    gfx->startWrite();
    gfx->fillScreen(C_BG);
    drawLogo((240 - OC_LOGO_W) / 2, 1);
    gfx->drawFastHLine(16, 40, 208, C_BORDER);
    drawBody();
    gfx->endWrite();
}

// ---------- 推送局部刷新 ----------
// 与像素时钟的差量重绘同一思路：推送只可能改动三个额度百分比（环弧长）与
// 四条标签值行（UPD / RST 显示串），因此只重画「屏上真的变了」的
// 环块与标签行（环块自清底、标签行各自清自己的行带），内容完全一致时一次
// 绘制都不做 → 消除整屏 fillScreen 造成的闪烁。
// 返回是否真的碰了屏（全一致时 false → 零绘制）
bool UsageManager::redrawChangedQuota() {
    // 先算差异再决定是否碰屏：全一致 → 零绘制（连 startWrite 都不开）
    bool ringsChanged = false;
    for (uint8_t i = 0; i < kBalanceLines; i++) {
        if (ringPercent(i) != s_shownQuota[i]) {
            ringsChanged = true;
            break;
        }
    }
    char lines[4][kMiniLineCap];
    bool lineChanged[4] = {false, false, false, false};
    bool labelsChanged = false;
    for (uint8_t i = 0; i < 4; i++) {
        buildMiniValueLine(i, lines[i], sizeof(lines[i]));
        lineChanged[i] = miniLineChanged(i, lines[i]);
        labelsChanged = labelsChanged || lineChanged[i];
    }
    if (!ringsChanged && !labelsChanged) {
        return false;
    }

    u8g2();
    auto* gfx = DisplayManager::getGfx();
    gfx->startWrite();
    // 环块只在弧长变化时整块重画（自清底只覆盖 153×153 框，不碰框外标签）；
    // 标签行各画各的，只动快照不一致的那条。
    if (ringsChanged) {
        drawQuotaRings();  // 自清底 + 收尾同步环快照
    }
    for (uint8_t i = 0; i < 4; i++) {
        if (lineChanged[i]) {
            drawMiniValueLine(i, lines[i]);
        }
    }
    gfx->endWrite();
    return true;
}

// ---------- 纯时钟页（clock 场景）----------
// 顶部小字日期星期 + 七段大字 HH:MM:SS（复用 SegFont7 字模/主题色/时间辅助）
static constexpr int CLOCK_PAGE_CX = 120;
static constexpr int CLOCK_PAGE_DATE_Y = 34;
static constexpr int CLOCK_PAGE_TIME_Y = 96;
static constexpr int CLOCK_PAGE_TIME_BAND_X = 8;
static constexpr int CLOCK_PAGE_TIME_BAND_W = 224;

static int clockPageLastSec = -1;
static int clockPageLastDay = -1;
static char clockPageShown[16] = {0};  // 上次绘制的时间串，逐字形差量更新

static String clockPageTimeString() {
    if (!timeSynced()) {
        return "--:--:--";
    }

    const struct tm tmv = localTmNow();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    return String(buf);
}

// 按 32bit 字模行位图把置位段合并成 drawFastHLine
static void drawSegRuns(int x, int y, uint32_t bits, uint16_t color) {
    auto* gfx = DisplayManager::getGfx();
    int k = 0;
    while (k < SEG7_W) {
        if (!(bits & (1u << (31 - k)))) {
            k++;
            continue;
        }
        int run = 0;
        while (k + run < SEG7_W && (bits & (1u << (31 - (k + run))))) {
            run++;
        }
        gfx->drawFastHLine(static_cast<int16_t>(x + k), static_cast<int16_t>(y), static_cast<int16_t>(run), color);
        k += run;
    }
}

// 像素差量重绘单个字形：仅翻转新旧字形的差异段（无清底 → 消除秒跳闪烁）
static void redrawSegGlyphDiff(int x, int y, char oldCh, char newCh, uint16_t color) {
    const auto idxOf = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch == '-') {
            return SEG7_DASH;
        }
        if (ch == ':') {
            return SEG7_COLON;
        }
        return -1;
    };

    const int iOld = idxOf(oldCh);
    const int iNew = idxOf(newCh);

    if (iOld == iNew) {
        return;
    }

    auto* gfx = DisplayManager::getGfx();
    gfx->startWrite();

    for (int row = 0; row < SEG7_H; row++) {
        const uint32_t o = (iOld >= 0) ? pgm_read_dword(&kSeg7Font[iOld][row]) : 0U;
        const uint32_t n = (iNew >= 0) ? pgm_read_dword(&kSeg7Font[iNew][row]) : 0U;

        drawSegRuns(x, y + row, o & ~n, C_BG);   // 旧有新无 → 擦除
        drawSegRuns(x, y + row, n & ~o, color);  // 新有旧无 → 点亮
    }

    gfx->endWrite();
}

// 时间行逐字形更新：force=全量绘制（入场），否则只动变化的字形
static void drawClockPageTime(bool force) {
    static const int kGlyphW[8] = {32, 32, 12, 32, 32, 12, 32, 32};  // HH:MM:SS
    const String s = clockPageTimeString();
    const int startX = CLOCK_PAGE_CX - 216 / 2;

    int x = startX;

    for (int i = 0; i < 8; i++) {
        if (force) {
            drawSegDigit(x, CLOCK_PAGE_TIME_Y, s[i], C_WHITE);
        } else {
            redrawSegGlyphDiff(x, CLOCK_PAGE_TIME_Y, clockPageShown[i], s[i], C_WHITE);
        }

        clockPageShown[i] = s[i];
        x += kGlyphW[i];
    }
}

static void drawClockPageDate() {
    auto* gfx = DisplayManager::getGfx();
    gfx->fillRect(0, CLOCK_PAGE_DATE_Y - 2, 240, 18, C_BG);

    String s;

    if (timeSynced()) {
        static const char* const kWeekday[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        const struct tm tmv = localTmNow();
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s", 1900 + tmv.tm_year, 1 + tmv.tm_mon, tmv.tm_mday,
                 kWeekday[tmv.tm_wday % 7]);
        s = buf;
    } else {
        s = "--";
    }

    u8g2().setFont(FONT_LABEL);
    drawText(CLOCK_PAGE_CX - textWidth(s.c_str()) / 2, CLOCK_PAGE_DATE_Y, s.c_str(), C_SUB);
}

// 纯时钟页入场：从零绘制所有元素（场景切换契约）
void UsageManager::drawClockPage() {
    u8g2();
    auto* gfx = DisplayManager::getGfx();
    gfx->startWrite();
    gfx->fillScreen(C_BG);
    drawClockPageDate();
    gfx->endWrite();

    memset(clockPageShown, 0, sizeof(clockPageShown));
    drawClockPageTime(true);  // 全量绘制并记录字形状态

    clockPageLastSec = timeSynced() ? currentLocalSecond() : -1;
    clockPageLastDay = timeSynced() ? localTmNow().tm_yday : -1;
}

// 秒级 tick：字形像素差量更新（无清底闪烁）；换日只重绘日期行
void UsageManager::tickClockPage() {
    if (!timeSynced()) {
        return;
    }

    const struct tm tmv = localTmNow();

    if (tmv.tm_yday != clockPageLastDay) {
        clockPageLastDay = tmv.tm_yday;
        drawClockPageDate();
    }

    if (tmv.tm_sec == clockPageLastSec) {
        return;
    }
    clockPageLastSec = tmv.tm_sec;

    drawClockPageTime(false);
}

void UsageManager::drawBootPage(bool fail) {
    u8g2();  // 惰性初始化 U8g2 字形引擎
    auto* gfx = DisplayManager::getGfx();
    gfx->fillScreen(C_BG);
    drawLogo((240 - OC_LOGO_W) / 2, 90);
    const char* hint = fail ? "WiFi failed, retrying..." : "Connecting WiFi...";
    u8g2().setFont(FONT_LABEL);
    int hw = textWidth(hint);
    drawText((240 - hw) / 2, 140, hint, fail ? C_RED : C_LABEL);
}

// ---------- 推送接收（无轮询：数据源为 POST /api/v1/balance）----------
void UsageManager::begin() {
    ever_started = true;
    Logger::info("UsageManager initialized (push mode)", "Balance");
}

void UsageManager::pushBalance(uint8_t rowCount, const char* status, bool hasStatus) {
    s_rowCount = rowCount > kBalanceLines ? kBalanceLines : rowCount;
    // 清除本次行数之外的旧字段，避免短推送后显示上一次残留。
    for (uint8_t i = s_rowCount; i < kBalanceLines; i++) {
        s_rowLabels[i][0] = '\0';
        s_rowResets[i][0] = '\0';
        s_rowProgress[i] = -1;
    }
    if (hasStatus && status != nullptr && status[0] != '\0') {
        strlcpy(s_status, status, kStatusCap);
        s_hasStatus = true;
    } else {
        s_status[0] = '\0';
        s_hasStatus = false;
    }
    s_hasPush = true;
    s_pushMillis = millis();
    s_pushEpoch = timeSynced() ? time(nullptr) : 0;
    // 立即刷新显示：只置「推送待刷新」，由 update() 走局部更新
    //（比对屏上额度环/标签快照，只重画变化环块与标签行；严禁绘制启动屏，见坑 #4）
    // 注意不碰 DisplayManager::requestFullRedraw()：那是显示设置变更
    //（rotation / 面板 profile）的整屏重绘信号，两者互不降级。
    s_pushRefreshPending = true;
    Logger::info("Balance push stored", "Balance");
}

// 场景入场：必须从零绘制所有元素（场景切换契约）
void UsageManager::enterScene() {
    mainPageDrawn = false;
    lastClockMinute = -1;
    lastClockSecond = -1;
    s_pushRefreshPending = false;  // 入场即全量绘制，吞掉入场前积压的推送刷新
    s_shownValid = false;

    if (WiFiManager::isConnected()) {
        drawMainPage();
        mainPageDrawn = true;
    } else {
        drawBootPage(true);
    }
}

// 场景退场：复位局部更新状态，防止下次 tick 按旧基准补笔留残影
void UsageManager::exitScene() {
    mainPageDrawn = false;
    lastClockMinute = -1;
    lastClockSecond = -1;
    s_shownValid = false;  // 屏上内容已不属本场景，快照作废
}

void UsageManager::update() {
    if (!ever_started) {
        return;
    }

    const bool wifiReady = (wifiManager != nullptr) && !wifiManager->isApMode() &&
                           WiFiManager::isConnected();

    // 显示设置（rotation / 面板 profile）变更后的整屏重绘主页面
    //（用推送缓冲/占位符绘制；不看 wifiReady；严禁绘制启动屏，见坑 #4）
    // 整屏绘制顺带把屏上快照刷新为当前内容
    if (DisplayManager::consumeFullRedrawRequest()) {
        drawMainPage();
        mainPageDrawn = true;
        Logger::info("UsageManager: main page redrawn", "Balance");
    }

    // 上位机推送：只重画与推送缓冲/屏上快照不一致的环块与标签值行（内容全同则零绘制）
    if (s_pushRefreshPending) {
        s_pushRefreshPending = false;
        if (mainPageDrawn && s_shownValid) {
            if (redrawChangedQuota()) {
                Logger::info("UsageManager: quota/labels redrawn (diff only)", "Balance");
            } else {
                Logger::info("UsageManager: balance push unchanged (no redraw)", "Balance");
            }
        } else {
            // 还没画过主页面（首绘 / boot 页期间）→ 无快照可比，退回整屏
            drawMainPage();
            mainPageDrawn = true;
        }
    }

    // 首次联网就绪时画主页面（同旧工程 onConnected hook）
    if (wifiReady && !mainPageDrawn) {
        drawMainPage();
        mainPageDrawn = true;
    }

    // 常驻时钟 tick：与推送无关，放在最后，WiFi 掉线时也照样走时；
    // 内部自行跳过未同步；boot 页/未画主页面期间不碰屏
    if (mainPageDrawn) {
        tickUi();
    }
}

// ---- UI 层 / API 层访问器（返回静态缓冲指针，调用方立即拷贝）----
bool UsageManager::hasPush() { return s_hasPush; }

const char* UsageManager::statusText() { return s_status; }

void UsageManager::setRowLabel(uint8_t index, const char* label) {
    if (index >= kBalanceLines) return;
    if (label == nullptr || label[0] == '\0') {
        s_rowLabels[index][0] = '\0';
    } else {
        strlcpy(s_rowLabels[index], label, kRowLabelCap);
    }
}

void UsageManager::setRowReset(uint8_t index, const char* reset) {
    if (index >= kBalanceLines) return;
    if (reset == nullptr || reset[0] == '\0') {
        s_rowResets[index][0] = '\0';
    } else {
        strlcpy(s_rowResets[index], reset, kRowResetCap);
    }
}

bool UsageManager::hasStatus() { return s_hasStatus; }

const char* UsageManager::rowLabel(uint8_t index) {
    if (index >= kBalanceLines) return "";
    return s_rowLabels[index];
}

int UsageManager::rowProgress(uint8_t index) {
    if (index >= kBalanceLines) return -1;
    return s_rowProgress[index];
}

const char* UsageManager::rowReset(uint8_t index) {
    if (index >= kBalanceLines) return "";
    return s_rowResets[index];
}

void UsageManager::setRowProgress(uint8_t index, int value) {
    if (index >= kBalanceLines) return;
    // value<0 = 本行无进度数据：存 -1（空槽）。否则钳到 0..100。
    s_rowProgress[index] = value < 0 ? -1 : (value > 100 ? 100 : value);
}

time_t UsageManager::pushEpoch() { return s_pushEpoch; }

uint32_t UsageManager::pushAgeSec() {
    if (!s_hasPush) return 0;
    return (millis() - s_pushMillis) / 1000UL;
}
