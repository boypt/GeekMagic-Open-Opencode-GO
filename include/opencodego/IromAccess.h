// SPDX-License-Identifier: GPL-3.0-or-later
// 经 build_flags 的 -include 强制包含到所有编译单元（含 u8g2_fonts.c）。
//
// 【背景】ESP8266 的 IROM（flash 数据缓存映射 0x402xxxxx）只支持 32-bit 对齐访问，
// 8-bit 访问（如裸指针解引用生成的 l8ui）会触发 LoadStoreError Exception (3)。
// 核心仅在定义 NON32XFER_HANDLER 时安装非 32-bit 访问异常处理器（本工程
// platformio.ini 已加 -DNON32XFER_HANDLER 兜底，避免任何漏改直接崩）。
// 但逐字节走异常处理器极慢，因此这里先把 u8x8 的读取宏重映射到
// pgm_read_byte（l32i + 移位，同 pgmspace.h 的安全实现）。
//
// U8g2_for_Adafruit_GFX 的 u8g2_fonts.h 在非 AVR 平台把 U8X8_FONT_SECTION/
// U8X8_PROGMEM 定义为空、u8x8_pgm_read 定义为裸指针解引用；该头里各宏均有
// #ifndef 保护，先定义即生效，字体/PROGMEM 常量就会落进 .irom.text.*（flash）。
//
// 节名必须带 ".irom.text." + 字体名后缀：
//  - 链接脚本按 .irom.text.* 模式收进 irom0 段（固定节名 .irom.text 也匹配）；
//  - 但 ELF 对象里同名 section 会合并成一个，--gc-sections 无法丢弃未引用字体，
//    700+ 个字体全保留会撑爆 irom0；带后缀让每个字体是独立 section 才可裁剪。

#ifndef IROM_ACCESS_H
#define IROM_ACCESS_H

#ifndef __ASSEMBLER__
#include <pgmspace.h>

// 快路径：u8g2 字体数据的逐字节读取改为 pgm_read_byte（32-bit 读 + 移位）
#define u8x8_pgm_read(adr) pgm_read_byte(adr)

#define U8X8_FONT_SECTION(name) __attribute__((section(".irom.text." name)))
#define U8X8_PROGMEM __attribute__((section(".irom.text.progmem")))
#endif  // !__ASSEMBLER__

#endif  // IROM_ACCESS_H
