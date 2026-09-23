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

#include <SPI.h>
#include <Logger.h>
#include <array>
#include <algorithm>
#include <Arduino.h>

#include "project_version.h"
#include "display/DisplayManager.h"
#include "config/ConfigManager.h"
#include "display/Gif.h"

static Gif* g_gif = nullptr;

extern ConfigManager configManager;

static Arduino_HWSPI g_lcdBus = Arduino_HWSPI(LCD_DC_GPIO, -1, &SPI, true);
static Arduino_ST7789 g_lcd = Arduino_ST7789(&g_lcdBus, -1, 0, true, LCD_W, LCD_H);

static constexpr uint32_t LCD_HARDWARE_RESET_DELAY_MS = 120;
static constexpr uint32_t LCD_BEGIN_DELAY_MS = 10;
static constexpr int16_t DISPLAY_PADDING = 10;
static constexpr int16_t DISPLAY_INFO_Y = 100;

static constexpr int WRAP_MAX_CHARS = 128;
static constexpr int WRAP_MAX_LINE_SLOTS = 10;

/**
 * @brief Push the current line buffer into the output lines array
 *
 * @param outLines The output lines array
 * @param lineBuf The current line buffer
 * @param lineLen The current line length
 * @param lineCount The current line count
 * @param maxLines The maximum number of lines allowed
 *
 * @return void
 */
static void wrapPushLine(std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS>& outLines,
                         std::array<char, WRAP_MAX_CHARS>& lineBuf, int& lineLen, int& lineCount, int maxLines) {
    if (lineCount >= maxLines) {
        return;
    }

    lineBuf[lineLen] = '\0';
    strncpy(outLines[lineCount].data(), lineBuf.data(), WRAP_MAX_CHARS - 1);
    outLines[lineCount][WRAP_MAX_CHARS - 1] = '\0';
    ++lineCount;

    lineLen = 0;
    lineBuf[0] = '\0';
}

/**
 * @brief Append a word to the current line buffer, wrapping if necessary
 *
 * @param outLines The output lines array
 * @param lineBuf The current line buffer
 * @param lineLen The current line length
 * @param wordBuf The word buffer to append
 * @param wordLen The word length
 * @param maxCharsPerLine The maximum characters per line
 * @param lineCount The current line count
 * @param maxLines The maximum number of lines allowed
 *
 * @return void
 */
static void wrapAppendWord(std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS>& outLines,
                           std::array<char, WRAP_MAX_CHARS>& lineBuf, int& lineLen,
                           std::array<char, WRAP_MAX_CHARS>& wordBuf, int& wordLen, int maxCharsPerLine, int& lineCount,
                           int maxLines) {
    if (wordLen == 0) {
        return;
    }

    if (wordLen > maxCharsPerLine) {
        if (lineLen != 0) {
            wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);
            if (lineCount >= maxLines) {
                wordLen = 0;

                return;
            }
        }
        int copyLen = (wordLen > maxCharsPerLine) ? maxCharsPerLine : wordLen;
        memcpy(lineBuf.data(), wordBuf.data(), static_cast<size_t>(copyLen));
        lineLen = copyLen;
        wordLen = 0;
        wordBuf[0] = '\0';

        return;
    }
    if (lineLen == 0) {
        memcpy(lineBuf.data(), wordBuf.data(), static_cast<size_t>(wordLen));
        lineLen = wordLen;
        wordLen = 0;
        wordBuf[0] = '\0';

        return;
    }
    if ((lineLen + 1 + wordLen) <= maxCharsPerLine) {
        lineBuf[lineLen] = ' ';
        memcpy(lineBuf.data() + lineLen + 1, wordBuf.data(), static_cast<size_t>(wordLen));
        lineLen += 1 + wordLen;
        wordLen = 0;
        wordBuf[0] = '\0';

        return;
    }
    wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);
    if (lineCount >= maxLines) {
        wordLen = 0;

        return;
    }

    memcpy(lineBuf.data(), wordBuf.data(), static_cast<size_t>(wordLen));
    lineLen = wordLen;
    wordLen = 0;
    wordBuf[0] = '\0';
}

// Screen cmd
static constexpr uint8_t ST7789_SLEEP_DELAY_MS = 120;
static constexpr uint8_t ST7789_SLEEP_OUT = 0x11;
static constexpr uint8_t ST7789_PORCH = 0xB2;
static constexpr uint8_t ST7789_PORCH_SETTINGS = 0x1F;
static constexpr uint8_t ST7789_SW_RESET = 0x01;

static constexpr uint8_t ST7789_TEARING_EFFECT = 0x35;
static constexpr uint8_t ST7789_MEMORY_ACCESS_CONTROL = 0x36;
static constexpr uint8_t ST7789_COLORMODE = 0x3A;
static constexpr uint8_t ST7789_COLORMODE_RGB565 = 0x05;

static constexpr uint8_t ST7789_POWER_B7 = 0xB7;
static constexpr uint8_t ST7789_POWER_BB = 0xBB;
static constexpr uint8_t ST7789_POWER_C0 = 0xC0;
static constexpr uint8_t ST7789_POWER_C2 = 0xC2;
static constexpr uint8_t ST7789_POWER_C3 = 0xC3;
static constexpr uint8_t ST7789_POWER_C4 = 0xC4;
static constexpr uint8_t ST7789_POWER_C6 = 0xC6;
static constexpr uint8_t ST7789_POWER_D0 = 0xD0;
static constexpr uint8_t ST7789_POWER_D6 = 0xD6;

static constexpr uint8_t ST7789_GAMMA_POS = 0xE0;
static constexpr uint8_t ST7789_GAMMA_NEG = 0xE1;
static constexpr uint8_t ST7789_GAMMA_CTRL = 0xE4;

static constexpr uint8_t ST7789_INVERSION_ON = 0x21;
static constexpr uint8_t ST7789_DISPLAY_ON = 0x29;
static constexpr uint8_t ST7789_NORMAL_DISPLAY_MODE = 0x13;

// Porch parameters used in sequence
static constexpr uint8_t ST7789_PORCH_PARAM_HS = 0x1F;
static constexpr uint8_t ST7789_PORCH_PARAM_VS = 0x1F;
static constexpr uint8_t ST7789_PORCH_PARAM_DUMMY = 0x00;
static constexpr uint8_t ST7789_PORCH_PARAM_HBP = 0x33;
static constexpr uint8_t ST7789_PORCH_PARAM_VBP = 0x33;

// Simple params for commands
static constexpr uint8_t ST7789_TEARING_PARAM_OFF = 0x00;
static constexpr uint8_t ST7789_MADCTL_PARAM_DEFAULT = 0x00;
static constexpr uint8_t ST7789_B7_PARAM_DEFAULT = 0x00;
static constexpr uint8_t ST7789_BB_PARAM_VOLTAGE = 0x36;
static constexpr uint8_t ST7789_C0_PARAM_1 = 0x2C;
static constexpr uint8_t ST7789_C2_PARAM_1 = 0x01;
static constexpr uint8_t ST7789_C3_PARAM_1 = 0x13;
static constexpr uint8_t ST7789_C4_PARAM_1 = 0x20;
static constexpr uint8_t ST7789_C6_PARAM_1 = 0x13;
static constexpr uint8_t ST7789_D6_PARAM_1 = 0xA1;
static constexpr uint8_t ST7789_D0_PARAM_1 = 0xA4;
static constexpr uint8_t ST7789_D0_PARAM_2 = 0xA1;

// Gamma parameter blocks
static constexpr std::array<uint8_t, 14> ST7789_GAMMA_POS_DATA = {0xF0, 0x08, 0x0E, 0x09, 0x08, 0x04, 0x2F,
                                                                  0x33, 0x45, 0x36, 0x13, 0x12, 0x2A, 0x2D};
static constexpr std::array<uint8_t, 14> ST7789_GAMMA_NEG_DATA = {0xF0, 0x0E, 0x12, 0x0C, 0x0A, 0x15, 0x2E,
                                                                  0x32, 0x44, 0x39, 0x17, 0x18, 0x2B, 0x2F};
static constexpr std::array<uint8_t, 3> ST7789_GAMMA_CTRL_DATA = {0x1D, 0x00, 0x00};

// Column/row address parameters
static constexpr uint8_t ST7789_ADDR_START_HIGH = 0x00;
static constexpr uint8_t ST7789_ADDR_START_LOW = 0x00;
static constexpr uint8_t ST7789_ADDR_END_HIGH = 0x00;
static constexpr uint8_t ST7789_ADDR_END_LOW = 0xEF;

/**
 * @brief Get the Arduino_GFX instance used for the LCD
 *
 * @return Pointer to the Arduino_GFX instance
 */
auto DisplayManager::getGfx() -> Arduino_GFX* { return &g_lcd; }

/**
 * @brief Apply the LCD backlight brightness via PWM
 *
 * 旧工程语义：brightness 0..1023 越大越亮，背光为低电平点亮
 * （LCD_BACKLIGHT_ACTIVE_LOW=true，等价 sd2 PWM_INVERTED：off=1023）。
 * 这里对外统一用 0..100 百分比，duty = percent*1023/100，输出取反。
 * percent==0 视为关断（off = 1023，低电平点亮时全灭）。
 *
 * @param percent Brightness percentage in range [0, 100]
 *
 * @return void
 */
void DisplayManager::setBacklight(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }

    const uint8_t pin = static_cast<uint8_t>(LCD_BACKLIGHT_GPIO);
    pinMode(pin, OUTPUT);
    analogWriteRange(1023);
    if (percent == 0) {
        // 关断：反相语义下输出全高（=旧工程 PWM_INVERTED off）
        analogWrite(pin, LCD_BACKLIGHT_ACTIVE_LOW ? 1023 : 0);
    } else {
        const uint32_t duty = (static_cast<uint32_t>(percent) * 1023U) / 100U;
        analogWrite(pin, LCD_BACKLIGHT_ACTIVE_LOW ? static_cast<uint16_t>(1023U - duty) : static_cast<uint16_t>(duty));
    }

    Logger::info(("Backlight: " + String(percent) + "%").c_str(), "DisplayManager");
}

/**
 * @brief Turn the LCD backlight on
 *
 * 按配置亮度（lcd_brightness，1..100）点亮而非无条件全亮
 *
 * @return void
 */
static inline void lcdBacklightOn() {
    DisplayManager::setBacklight(configManager.getLCDBrightness());
}

/**
 * @brief Write a single command byte to the ST7789 via the data bus
 *
 * @return void
 */
static inline void ST7789_WriteCommand(uint8_t cmd) { g_lcdBus.writeCommand(cmd); }

/**
 * @brief Write a single data byte to the ST7789 via the data bus
 *
 * @return void
 */
static inline void ST7789_WriteData(uint8_t data) { g_lcdBus.write(data); }

// MADCTL 位定义（ST7789 0x36 命令）
static constexpr uint8_t LCD_MADCTL_MX = 0x40;
static constexpr uint8_t LCD_MADCTL_MV = 0x20;
static constexpr uint8_t LCD_MADCTL_MY = 0x80;
static constexpr uint8_t LCD_MADCTL_RGB = 0x00;
static constexpr uint8_t LCD_MADCTL_BGR = 0x08;

/**
 * @brief Re-send MADCTL (0x36) with runtime-configurable mirror bits
 *
 * Arduino_GFX 的 setRotation() 内部已经按 rotation 发过一次 MADCTL（0x36），
 * 这里按当前 rotation 对应的默认 MADCTL 值重发一遍，再按
 * ConfigManager 的 lcd_mirror_x/lcd_mirror_y 翻转 MX(0x40)/MY(0x80) 位。
 * 所有后续绘制均按校正后的扫描方向进行，无需坐标补偿。
 *
 * @param rotation 当前 rotation 值（0-7），用于取得 Arduino_GFX 的默认 MADCTL 值
 *
 * @return void
 */
static void lcdApplyMirrorMADCTL(uint8_t rotation) {
    // 与 Arduino_GFX Arduino_ST7789::setRotation 的默认 MADCTL 值一致
    static constexpr uint8_t ROTATION_MADCTL[8] = {
        LCD_MADCTL_RGB,                                       // 0: RGB
        static_cast<uint8_t>(LCD_MADCTL_MX | LCD_MADCTL_MV),  // 1: MX|MV|RGB
        static_cast<uint8_t>(LCD_MADCTL_MX | LCD_MADCTL_MY),  // 2: MX|MY|RGB
        static_cast<uint8_t>(LCD_MADCTL_MY | LCD_MADCTL_MV),  // 3: MY|MV|RGB
        LCD_MADCTL_MX,                                        // 4: MX|RGB
        static_cast<uint8_t>(LCD_MADCTL_MX | LCD_MADCTL_MY | LCD_MADCTL_MV),  // 5: MX|MY|MV|RGB
        LCD_MADCTL_MY,                                        // 6: MY|RGB
        LCD_MADCTL_MV,                                        // 7: MV|RGB
    };

    uint8_t madctl = ROTATION_MADCTL[rotation & 0x07];

    if (configManager.getLCDBgr()) {
        // 旧 sd2（TFT_eSPI ST7789_2）在 240x240 下因 CGRAM_OFFSET 默认走 BGR（0x08），
        // 关闭时保持 Arduino_GFX 默认 RGB 色序
        madctl |= LCD_MADCTL_BGR;
    }
    if (configManager.getLCDMirrorX()) {
        madctl |= LCD_MADCTL_MX;
    }
    if (configManager.getLCDMirrorY()) {
        madctl |= LCD_MADCTL_MY;
    }

    g_lcdBus.beginWrite();
    ST7789_WriteCommand(ST7789_MEMORY_ACCESS_CONTROL);
    ST7789_WriteData(madctl);
    g_lcdBus.endWrite();

    Logger::info(("MADCTL applied: 0x" + String(madctl, HEX) + " mirror_x=" + (configManager.getLCDMirrorX() ? "1" : "0") +
                  " mirror_y=" + (configManager.getLCDMirrorY() ? "1" : "0") +
                  " bgr=" + (configManager.getLCDBgr() ? "1" : "0"))
                     .c_str(),
                 "DisplayManager");
}

/**
 * @brief Run a vendor-specific initialization sequence for the ST7789 panel
 *
 *  - Sleep out (0x11)
 *
 *  - Porch settings (0xB2)
 *
 *  - Tearing effect on (0x35)
 *
 *  - Memory access control/MADCTL (0x36)
 *
 *  - Color mode to 16-bit RGB565 (0x3A)
 *
 *  - Various power control settings (0xB7, 0xBB, 0xC0-0xC6, 0xD0, 0xD6)
 *
 *  - Gamma correction settings (0xE0, 0xE1, 0xE4)
 *
 *  - Display inversion on (0x21)
 *
 *  - Display on (0x29)
 *
 *  - Full window setup and RAMWR command (0x2A, 0x2B, 0x2C)
 *
 * @return void
 */
static void lcdRunVendorInit() {
    g_lcdBus.beginWrite();

    ST7789_WriteCommand(ST7789_SLEEP_OUT);
    delay(ST7789_SLEEP_DELAY_MS);

    ST7789_WriteCommand(ST7789_PORCH);
    ST7789_WriteData(ST7789_PORCH_PARAM_HS);
    ST7789_WriteData(ST7789_PORCH_PARAM_VS);
    ST7789_WriteData(ST7789_PORCH_PARAM_DUMMY);
    ST7789_WriteData(ST7789_PORCH_PARAM_HBP);
    ST7789_WriteData(ST7789_PORCH_PARAM_VBP);

    ST7789_WriteCommand(ST7789_TEARING_EFFECT);
    ST7789_WriteData(ST7789_TEARING_PARAM_OFF);

    ST7789_WriteCommand(ST7789_MEMORY_ACCESS_CONTROL);
    ST7789_WriteData(ST7789_MADCTL_PARAM_DEFAULT);

    ST7789_WriteCommand(ST7789_COLORMODE);
    ST7789_WriteData(ST7789_COLORMODE_RGB565);

    ST7789_WriteCommand(ST7789_POWER_B7);
    ST7789_WriteData(ST7789_B7_PARAM_DEFAULT);

    ST7789_WriteCommand(ST7789_POWER_BB);
    ST7789_WriteData(ST7789_BB_PARAM_VOLTAGE);

    ST7789_WriteCommand(ST7789_POWER_C0);
    ST7789_WriteData(ST7789_C0_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C2);
    ST7789_WriteData(ST7789_C2_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C3);
    ST7789_WriteData(ST7789_C3_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C4);
    ST7789_WriteData(ST7789_C4_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_C6);
    ST7789_WriteData(ST7789_C6_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_D6);
    ST7789_WriteData(ST7789_D6_PARAM_1);

    ST7789_WriteCommand(ST7789_POWER_D0);
    ST7789_WriteData(ST7789_D0_PARAM_1);
    ST7789_WriteData(ST7789_D0_PARAM_2);

    ST7789_WriteCommand(ST7789_POWER_D6);
    ST7789_WriteData(ST7789_D6_PARAM_1);

    ST7789_WriteCommand(ST7789_GAMMA_POS);
    for (uint8_t v : ST7789_GAMMA_POS_DATA) {
        ST7789_WriteData(v);
    }

    ST7789_WriteCommand(ST7789_GAMMA_NEG);
    for (uint8_t v : ST7789_GAMMA_NEG_DATA) {
        ST7789_WriteData(v);
    }

    ST7789_WriteCommand(ST7789_GAMMA_CTRL);
    for (uint8_t v : ST7789_GAMMA_CTRL_DATA) {
        ST7789_WriteData(v);
    }

    ST7789_WriteCommand(ST7789_INVERSION_ON);

    ST7789_WriteCommand(ST7789_DISPLAY_ON);

    ST7789_WriteCommand(ST7789_CASET);
    ST7789_WriteData(ST7789_ADDR_START_HIGH);
    ST7789_WriteData(ST7789_ADDR_START_LOW);
    ST7789_WriteData(ST7789_ADDR_END_HIGH);
    ST7789_WriteData(ST7789_ADDR_END_LOW);

    ST7789_WriteCommand(ST7789_RASET);
    ST7789_WriteData(ST7789_ADDR_START_HIGH);
    ST7789_WriteData(ST7789_ADDR_START_LOW);
    ST7789_WriteData(ST7789_ADDR_END_HIGH);
    ST7789_WriteData(ST7789_ADDR_END_LOW);

    ST7789_WriteCommand(ST7789_RAMWR);

    g_lcdBus.endWrite();
}

/**
 * @brief Run the legacy sd2 (TFT_eSPI ST7789_2) minimal initialization sequence
 *
 * 移植自旧工程 TFT_eSPI TFT_Drivers/ST7789_2_Init.h（commandList 内容逐条对应：
 * 1/8/10/16/17 行），仅 SLPOUT/COLMOD/MADCTL/CASET/PASET/INVON/NORON/DISPON，
 * 无 porch/电源/gamma 配置（面板保持上电默认值）。MADCTL 一律发 0x00，
 * BGR 色序随后由 lcdApplyMirrorMADCTL() 按 lcd_bgr 配置接管。
 *
 * @return void
 */
static void lcdRunSd2Init() {
    // ST7789_2_Defines.h / ST7789_2_Init.h 中的命令与常量
    static constexpr uint8_t SD2_SLPOUT = 0x11;   // TFT_SLPOUT
    static constexpr uint8_t SD2_COLMOD = 0x3A;   // TFT_COLMOD
    static constexpr uint8_t SD2_MADCTL = 0x36;   // TFT_MADCTL
    static constexpr uint8_t SD2_CASET = 0x2A;    // TFT_CASET
    static constexpr uint8_t SD2_PASET = 0x2B;    // TFT_PASET
    static constexpr uint8_t SD2_INVON = 0x21;    // TFT_INVON
    static constexpr uint8_t SD2_NORON = 0x13;    // TFT_NORON
    static constexpr uint8_t SD2_DISPON = 0x29;   // TFT_DISPON
    static constexpr uint8_t SD2_COLMOD_16BPP = 0x55;
    static constexpr uint8_t SD2_ADDR_240_START_HIGH = 0x00;
    static constexpr uint8_t SD2_ADDR_240_START_LOW = 0x00;
    static constexpr uint8_t SD2_ADDR_END_HIGH = 0x00;
    static constexpr uint8_t SD2_ADDR_END_LOW = 0xF0;

    g_lcdBus.beginWrite();

    ST7789_WriteCommand(SD2_SLPOUT);
    delay(ST7789_SLEEP_DELAY_MS);

    ST7789_WriteCommand(SD2_COLMOD);
    ST7789_WriteData(SD2_COLMOD_16BPP);
    delay(10);

    ST7789_WriteCommand(SD2_MADCTL);
    ST7789_WriteData(0x00);

    ST7789_WriteCommand(SD2_CASET);
    ST7789_WriteData(SD2_ADDR_240_START_HIGH);
    ST7789_WriteData(SD2_ADDR_240_START_LOW);
    ST7789_WriteData(SD2_ADDR_END_HIGH);
    ST7789_WriteData(SD2_ADDR_END_LOW);

    ST7789_WriteCommand(SD2_PASET);
    ST7789_WriteData(SD2_ADDR_240_START_HIGH);
    ST7789_WriteData(SD2_ADDR_240_START_LOW);
    ST7789_WriteData(SD2_ADDR_END_HIGH);
    ST7789_WriteData(SD2_ADDR_END_LOW);

    ST7789_WriteCommand(SD2_INVON);
    delay(10);

    ST7789_WriteCommand(SD2_NORON);
    delay(10);

    ST7789_WriteCommand(SD2_DISPON);
    delay(255);

    g_lcdBus.endWrite();
}

/**
 * @brief Perform a hardware reset of the LCD panel
 *
 * Toggles the RST GPIO if defined, with appropriate delays
 *
 * @return void
 */
static void lcdHardReset() {
    pinMode((uint8_t)LCD_RST_GPIO, OUTPUT);
    digitalWrite((uint8_t)LCD_RST_GPIO, HIGH);
    delay(LCD_HARDWARE_RESET_DELAY_MS);
    digitalWrite((uint8_t)LCD_RST_GPIO, LOW);
    delay(LCD_HARDWARE_RESET_DELAY_MS);
    digitalWrite((uint8_t)LCD_RST_GPIO, HIGH);
    delay(LCD_HARDWARE_RESET_DELAY_MS);
}

/**
 * @brief Ensure the LCD is initialized and ready for drawing
 *
 * @return void
 */
static void lcdEnsureInit() {
    Logger::info("Initialization started", "DisplayManager");

    lcdBacklightOn();

    uint8_t rotation = configManager.getLCDRotationSafe();

    // SPI mode 3 is required. This toggles the pin from LOW to HIGH after reset, which my guess
    // is after reset "initializes" the SPI interface of the display, as CS is tied to GND?
    // ...strange that SPI_MODE0 will not work as the IC doesn't care about CLK's polarity
    g_lcdBus.begin((int32_t)LCD_SPI_HZ, (int8_t)LCD_SPI_MODE);
    lcdHardReset();

    const bool useSd2Init = configManager.getLCDInitSd2();
    if (useSd2Init) {
        lcdRunSd2Init();
    } else {
        lcdRunVendorInit();
    }
    Logger::info(("Init profile: " + String(useSd2Init ? "sd2 (ST7789_2 minimal)" : "vendor") +
                  " BGR=" + (configManager.getLCDBgr() ? "1" : "0"))
                     .c_str(),
                 "DisplayManager");
    delay(LCD_BEGIN_DELAY_MS);

    g_lcd.setRotation(rotation);
    lcdApplyMirrorMADCTL(rotation);

    Logger::info(
        ("Width=" + String(g_lcd.width()) + " height=" + String(g_lcd.height()) + " rotation=" + String(rotation))
            .c_str(),
        "DisplayManager");

    g_lcd.fillScreen(LCD_BLACK);
    g_lcd.setTextColor(LCD_WHITE, LCD_BLACK);

    Logger::info("Initialization completed", "DisplayManager");
}

/**
 * @brief Wrap text into lines fitting within max characters and lines
 *
 * @param text The input text to wrap
 * @param maxCharsPerLine Maximum characters allowed per line
 * @param maxLines Maximum number of lines allowed
 * @param outLines Output array to hold the wrapped lines
 *
 * @return The number of lines used
 */
static auto lcdWrapTextToBuffer(const String& text, int maxCharsPerLine, int maxLines,
                                std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS>& outLines) -> int {
    int lineCount = 0;

    for (auto& row : outLines) {
        row[0] = '\0';
    }

    std::array<char, WRAP_MAX_CHARS> lineBuf{};
    std::array<char, WRAP_MAX_CHARS> wordBuf{};
    int lineLen = 0;
    int wordLen = 0;

    for (uint32_t i = 0; i < text.length(); ++i) {
        char chr = text.charAt(i);

        if (chr == '\r') {
            continue;
        }

        if (chr == '\n') {
            wrapAppendWord(outLines, lineBuf, lineLen, wordBuf, wordLen, maxCharsPerLine, lineCount, maxLines);
            wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);

            if (lineCount >= maxLines) {
                break;
            }

            continue;
        }

        if (chr == ' ' || chr == '\t') {
            wrapAppendWord(outLines, lineBuf, lineLen, wordBuf, wordLen, maxCharsPerLine, lineCount, maxLines);

            if (lineCount >= maxLines) {
                break;
            }

            continue;
        }

        if (wordLen + 1 < WRAP_MAX_CHARS) {
            wordBuf[wordLen++] = chr;
            wordBuf[wordLen] = '\0';
        }
    }

    wrapAppendWord(outLines, lineBuf, lineLen, wordBuf, wordLen, maxCharsPerLine, lineCount, maxLines);

    if (lineLen != 0 && lineCount < maxLines) {
        wrapPushLine(outLines, lineBuf, lineLen, lineCount, maxLines);
    }

    if (lineCount == 0) {
        outLines[0][0] = '\0';
        lineCount = 1;
    }

    return lineCount;
}

/**
 * @brief Draw text on the display with simple word-wrapping
 *
 * @param startX Starting X coordinate in pixels
 * @param startY Starting Y coordinate in pixels
 * @param text The text to draw (can contain newlines)
 * @param textSize Font size multiplier (integer)
 * @param fgColor Foreground color (16-bit RGB565)
 * @param bgColor Background color (16-bit RGB565)
 * @param clearBg If true, clears the background rectangle before drawing
 *
 * @return void
 */
static void lcdDrawTextWrapped(int16_t startX, int16_t startY, const String& text, uint8_t textSize, uint16_t fgColor,
                               uint16_t bgColor, bool clearBg) {
    const auto screenW = static_cast<int16_t>(g_lcd.width());
    const auto screenH = static_cast<int16_t>(g_lcd.height());

    if (startX < 0) {
        startX = 0;
    }

    if (startY < 0) {
        startY = 0;
    }

    if (startX >= screenW || startY >= screenH) {
        Logger::warn("Text start position out of bounds", "DisplayManager");

        return;
    }

    const auto charW = static_cast<int16_t>(6 * textSize);
    const auto charH = static_cast<int16_t>(8 * textSize);
    if (charW <= 0 || charH <= 0) {
        Logger::warn("Invalid character dimensions", "DisplayManager");

        return;
    }

    int maxCharsPerLine = (screenW - startX) / charW;
    int maxLines = (screenH - startY) / charH;
    if (maxCharsPerLine <= 0 || maxLines <= 0) {
        Logger::warn("No space for text", "DisplayManager");

        return;
    }

    if (maxLines > WRAP_MAX_LINE_SLOTS) {
        maxLines = WRAP_MAX_LINE_SLOTS;
    }

    std::array<std::array<char, WRAP_MAX_CHARS>, WRAP_MAX_LINE_SLOTS> lines{};
    int lineCount = lcdWrapTextToBuffer(text, maxCharsPerLine, maxLines, lines);

    if (clearBg) {
        const auto heightPixels = static_cast<int16_t>(static_cast<int>(lineCount) * static_cast<int>(charH));
        g_lcd.fillRect(startX, startY, static_cast<int16_t>(screenW - startX), static_cast<int16_t>(heightPixels),
                       bgColor);
    }

    g_lcd.setTextSize(textSize);
    g_lcd.setTextColor(fgColor, bgColor);

    for (int li = 0; li < lineCount; ++li) {
        g_lcd.setCursor(startX, static_cast<int16_t>(startY + li * charH));
        g_lcd.print(lines[li].data());
    }
}

/**
 * @brief Initialize the DisplayManager and LCD
 *
 * Ensures the LCD is initialized and ready for drawing
 *
 * @return void
 */
auto DisplayManager::begin() -> void { lcdEnsureInit(); }

/**
 * @brief Apply a new display rotation at runtime
 *
 * @param rotation Rotation value in range [0, 7]
 *
 * @return void
 */
auto DisplayManager::setRotation(uint8_t rotation, String currentIP) -> void {
    g_lcd.setRotation(rotation);
    lcdApplyMirrorMADCTL(rotation);
    DisplayManager::drawStartup(currentIP);

    Logger::info(("Rotation set to " + String(rotation)).c_str(), "DisplayManager");
}

/**
 * @brief Re-run the LCD init sequence (profile) and MADCTL with current settings
 *
 * 用于 Web 切换 lcd_bgr / lcd_init_sd2 后立即生效：重新硬复位 + 按当前
 * profile（厂商序列或旧 sd2 精简序列）初始化，再重发 MADCTL。
 *
 * @return void
 */
auto DisplayManager::applyPanelProfile() -> void {
    lcdEnsureInit();

    String currentIP = "unknown";
    DisplayManager::drawStartup(currentIP);
}

/**
 * @brief Draw the startup screen on the LCD
 *
 * @return void
 */
auto DisplayManager::drawStartup(String currentIP) -> void {
    int constexpr rgbDelayMs = 1000;

    g_lcd.fillScreen(LCD_RED);
    delay(rgbDelayMs);
    g_lcd.fillScreen(LCD_GREEN);
    delay(rgbDelayMs);
    g_lcd.fillScreen(LCD_BLUE);
    delay(rgbDelayMs);

    g_lcd.fillScreen(LCD_BLACK);

    int constexpr titleY = 10;
    int constexpr fontSize = 2;

    DisplayManager::drawTextWrapped(DISPLAY_PADDING, titleY, "GeekMagic Open Firmware", fontSize, LCD_WHITE, LCD_BLACK,
                                    false);
    DisplayManager::drawTextWrapped(DISPLAY_PADDING, titleY + THREE_LINES_SPACE, String(PROJECT_VER_STR), fontSize,
                                    LCD_WHITE, LCD_BLACK, false);
    DisplayManager::drawTextWrapped(DISPLAY_PADDING, (titleY + THREE_LINES_SPACE + TWO_LINES_SPACE), "IP: " + currentIP,
                                    fontSize, LCD_WHITE, LCD_BLACK, false);

    const int16_t box = 40;
    const int16_t gap = 20;
    const int16_t boxY = titleY + (THREE_LINES_SPACE * 2) + ONE_LINE_SPACE;

    g_lcd.fillRect(DISPLAY_PADDING, boxY, box, box, LCD_RED);
    g_lcd.fillRect((int16_t)(DISPLAY_PADDING + box + gap), boxY, box, box, LCD_GREEN);
    g_lcd.fillRect((int16_t)(DISPLAY_PADDING + (box + gap) * 2), boxY, box, box, LCD_BLUE);

    yield();

    Logger::info("Startup screen drawn", "DisplayManager");
}

/**
 * @brief Draw text on the display with simple word-wrapping
 *
 * @param x Starting X coordinate in pixels
 * @param y Starting Y coordinate in pixels
 * @param text The text to draw (can contain newlines)
 * @param textSize Font size multiplier (integer)
 * @param fg Foreground color (16-bit RGB565)
 * @param bg Background color (16-bit RGB565)
 * @param clearBg If true, clears the background rectangle before drawing
 *
 * @return void
 */
void DisplayManager::drawTextWrapped(int16_t xPos, int16_t yPos, const String& text, uint8_t textSize, uint16_t fgColor,
                                     uint16_t bgColor, bool clearBg) {
    lcdDrawTextWrapped(xPos, yPos, text, textSize, fgColor, bgColor, clearBg);
}

/**
 * @brief Draw a loading bar on the display
 *
 * @param progress Progress value between 0.0 (empty) and 1.0 (full)
 * @param yPos Y coordinate of the top of the loading bar
 * @param barWidth Width of the loading bar in pixels
 * @param barHeight Height of the loading bar in pixels
 * @param fgColor Foreground color (16-bit RGB565)
 * @param bgColor Background color (16-bit RGB565)
 */
void DisplayManager::drawLoadingBar(float progress, int yPos, int barWidth, int barHeight, uint16_t fgColor,
                                    uint16_t bgColor) {
    auto barXPos = (static_cast<int32_t>(LCD_W) - static_cast<int32_t>(barWidth)) / 2;
    auto barXPos16 = static_cast<int16_t>(barXPos);
    auto yPos16 = static_cast<int16_t>(yPos);
    auto barWidth16 = static_cast<int16_t>(barWidth);
    auto barHeight16 = static_cast<int16_t>(barHeight);

    g_lcd.fillRect(barXPos16, yPos16, barWidth16, barHeight16, bgColor);

    auto fillWidthF = static_cast<float>(barWidth) * progress;
    auto fillWidth16 = static_cast<int16_t>(fillWidthF);
    if (fillWidth16 > 0) {
        g_lcd.fillRect(barXPos16, yPos16, fillWidth16, barHeight16, fgColor);
    }

    yield();
}

/**
 * @brief Play a single GIF file in full screen mode (blocking)
 *
 * @param path Path to the GIF file on LittleFS
 * @param timeMs Duration to play the GIF in milliseconds (0 = play full GIF)
 * @return true if played successfully, false on error
 */
auto DisplayManager::playGifFullScreen(const String& path, uint32_t timeMs) -> bool {
    if (g_gif == nullptr) {
        g_gif = new Gif();
        if (g_gif == nullptr) {
            Logger::error("Failed to allocate GIF decoder", "DisplayManager");
            return false;
        }
    }

    g_gif->stop();

    if (!g_gif->begin()) {
        return false;
    }

    DisplayManager::clearScreen();

    g_gif->setLoopEnabled(timeMs == 0);

    const bool started = g_gif->playOne(path);
    if (!started) {
        return false;
    }

    if (timeMs == 0) {
        return true;
    }

    const uint32_t startMs = millis();
    const uint32_t endMs = startMs + timeMs;

    while (g_gif->isPlaying() && static_cast<int32_t>(millis() - endMs) < 0) {
        g_gif->update();
        yield();
    }

    if (g_gif->isPlaying()) {
        g_gif->stop();
    }

    return true;
}

/**
 * @brief Stop GIF playback if playing
 *
 * @return true
 */
auto DisplayManager::stopGif() -> bool {
    if (g_gif != nullptr) {
        g_gif->stop();
    }

    DisplayManager::clearScreen();

    return true;
}
/**
 * @brief Update the GIF decoder (should be called regularly in loop)
 *
 * @return void
 */
auto DisplayManager::update() -> void {
    if (g_gif != nullptr) {
        g_gif->update();
    }
}

/**
 * @brief Clear the entire display to black
 *
 * @return void
 */
auto DisplayManager::clearScreen() -> void { g_lcd.fillScreen(LCD_BLACK); }
