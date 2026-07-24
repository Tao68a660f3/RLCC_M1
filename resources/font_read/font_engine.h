#ifndef __FONT_ENGINE_H
#define __FONT_ENGINE_H

// font_engine.h
#include "canvas_renderer.h"
#include "ff.h"

// 假设我们最大支持 32x32 的字符
#define MAX_GLYPH_BUF_SIZE 128

typedef struct {
  uint16_t width;
  uint16_t height;
  uint8_t bitmap[MAX_GLYPH_BUF_SIZE]; // 驱动把处理好的横向点阵放在这
} GlyphBuffer;

// 现在的驱动不再返回指针，而是填充这个 Buffer
typedef uint8_t (*FontDriver)(uint32_t code, GlyphInfo *out_info,
                              uint8_t render_now);

uint8_t Font_ASCII_5x8_Adapter(uint32_t code, GlyphInfo *out_glyph,
                               uint8_t render_now);

uint8_t Font_GBK16_Adapter(uint32_t gbk_code, GlyphInfo *out_glyph,
                           uint8_t render_now);

uint8_t Font_ASC16_File_Adapter(uint32_t ascii_code, GlyphInfo *out_glyph,
                                uint8_t render_now);

uint8_t Font_Custom_Bin_Adapter(uint32_t code, GlyphInfo *out_glyph,
                                uint8_t render_now);

uint8_t Font_Engine_Load_Bin(FIL *p_file);

#endif
