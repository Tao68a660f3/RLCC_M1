// font_engine.c
#include "font_engine.h"
#include "canvas_renderer.h"
#include "ff.h" // 确保包含 FatFS
#include "fonts.h"
#include <string.h>

typedef struct {
  uint8_t height;
  uint8_t max_width;
  uint16_t bytes_per_char;
  uint8_t widths[256];
  FIL *file_handle; // 绑定的文件句柄
} CustomFont_t;

extern FIL f_gbk16; // 汉字库文件句柄
extern FIL f_asc16; // ASCII字库句柄
// 内部静态缓冲区，用来给 GlyphInfo.p_bits 提供地址
static uint8_t internal_buf[MAX_GLYPH_BUF_SIZE];
static CustomFont_t cur_vfont; // 当前变宽字库信息

uint8_t Font_ASCII_5x8_Adapter(uint32_t code, GlyphInfo *out_glyph,
                               uint8_t render_now) {
  // 1. 基本信息
  out_glyph->width = 6;
  out_glyph->height = 8;

  if (!render_now) {
    out_glyph->p_bits = NULL; // 测量模式不给指针
    return 1;
  }

  // 2. 转换逻辑：将【纵向 5 字节】 转换为 【横向 8 字节】
  // 这样渲染器就可以用统一的横向扫描逻辑画图了
  memset(internal_buf, 0, MAX_GLYPH_BUF_SIZE);

  uint32_t font_ptr = (code - char_offset) * 5;

  for (uint8_t row = 0; row < 8; row++) {
    uint8_t row_byte = 0;
    for (uint8_t col = 0; col < 5; col++) {
      // 原字库是第 col 字节的 (0x80 >> row) 位
      if (Chars[font_ptr + col] & (0x80 >> row)) {
        row_byte |= (0x80 >> col); // 填入新字节的对应位
      }
    }
    internal_buf[row] = row_byte; // 存储为横向的一行
  }

  out_glyph->p_bits = internal_buf;
  return 1;
}

/**
 * @brief GBK16 字符点阵适配器 (兼容旧 HZK16)
 * @param gbk_code: 传入的 16位 编码 (High << 8 | Low)
 */
uint8_t Font_GBK16_Adapter(uint32_t gbk_code, GlyphInfo *out_glyph,
                           uint8_t render_now) {
  out_glyph->width = 16;
  out_glyph->height = 16;

  if (!render_now)
    return 1;
  if (f_gbk16.obj.fs == NULL)
    return 0;

  uint8_t h = (uint8_t)(gbk_code >> 8);
  uint8_t l = (uint8_t)(gbk_code & 0xFF);
  uint32_t char_idx = 0xFFFFFFFF; // 字符序号

  // 1. 常用符号区 (0xA1A1 - 0xA9FE)
  if (h >= 0xA1 && h <= 0xA9 && l >= 0xA1 && l <= 0xFE) {
    char_idx = (uint32_t)(h - 0xA1) * 94 + (l - 0xA1);
  }
  // 2. GBK扩充5区 (0xA840 - 0xA9A0)
  else if (h >= 0xA8 && h <= 0xA9 && l >= 0x40 && l <= 0xA0) {
    if (l == 0x7F)
      return 0; // 7F为无效码位
    char_idx = 846 + (uint32_t)(h - 0xA8) * 97 + (l - 0x40);
  }
  // 3. 核心汉字区 (0xB0A1 - 0xF7FE)
  else if (h >= 0xB0 && h <= 0xF7 && l >= 0xA1 && l <= 0xFE) {
    char_idx = 1040 + (uint32_t)(h - 0xB0) * 94 + (l - 0xA1);
  }
  // 4. GBK扩充3区 (0x8140 - 0xA0FE)
  else if (h >= 0x81 && h <= 0xA0 && l >= 0x40 && l <= 0xFE) {
    if (l == 0x7F)
      return 0; // 7F为无效码位
    char_idx = 7808 + (uint32_t)(h - 0x81) * 191 + (l - 0x40);
  }
  // 5. GBK扩充4区第一段 (0xAA40 - 0xFDA0)
  else if (h >= 0xAA && h <= 0xFD && l >= 0x40 && l <= 0xA0) {
    if (l == 0x7F)
      return 0; // 7F为无效码位
    char_idx = 13920 + (uint32_t)(h - 0xAA) * 97 + (l - 0x40);
  }
  // 6. GBK扩充4区第二段 (0xFE40 - 0xFE4F)
  else if (h == 0xFE && l >= 0x40 && l <= 0x4F) {
    // 此段不含0x7F，无需检查
    char_idx = 22068 + (l - 0x40);
  }

  if (char_idx == 0xFFFFFFFF)
    return 0; // 未命中

  // 每个字符 32 字节
  uint32_t offset = char_idx * 32;

  UINT br;
  if (f_lseek(&f_gbk16, offset) == FR_OK &&
      f_read(&f_gbk16, internal_buf, 32, &br) == FR_OK && br == 32) {
    out_glyph->p_bits = internal_buf;
    return 1;
  }
  return 0;
}

/**
 * @brief ASC16 适配器 (8x16 ASCII)
 */
uint8_t Font_ASC16_File_Adapter(uint32_t ascii_code, GlyphInfo *out_glyph,
                                uint8_t render_now) {
  // 尝试从 SD 卡读取 ASC16 (8x16)
  // 检查文件句柄是否有效 (FatFS 中 obj.fs 为空代表文件未打开)
  if (f_asc16.obj.fs != NULL) {
    out_glyph->width = 8;
    out_glyph->height = 16;

    if (!render_now)
      return 1;

    uint32_t offset = ascii_code * 16;
    UINT br;
    if (f_lseek(&f_asc16, offset) == FR_OK &&
        f_read(&f_asc16, internal_buf, 16, &br) == FR_OK && br == 16) {
      out_glyph->p_bits = internal_buf;
      return 1;
    }
  }

  // --- 如果走到这里，说明 SD 卡读取失败，使用内置 5x8 适配器保底 ---
  return Font_ASCII_5x8_Adapter(ascii_code, out_glyph, render_now);
}

/**
 * @brief 通用变宽二进制字库适配器（带 5x8 保底功能）
 * 支持从 Header 自动获取尺寸，从内存宽度表获取变宽信息
 */
uint8_t Font_Custom_Bin_Adapter(uint32_t code, GlyphInfo *out_glyph,
                                uint8_t render_now) {
  // --- 第一层防御：基本的参数与文件就绪检查 ---
  // 如果 code 超出 ASCII 范围，或者文件压根没打开
  if (code > 255 || cur_vfont.file_handle == NULL ||
      cur_vfont.file_handle->obj.fs == NULL) {
    return Font_ASCII_5x8_Adapter(code, out_glyph, render_now);
  }

  // --- 第二层防御：测量模式下的逻辑 ---
  // 如果宽度表里该字符宽度为 0（代表 BIN 里没这个字符），也走保底
  if (cur_vfont.widths[code] == 0) {
    return Font_ASCII_5x8_Adapter(code, out_glyph, render_now);
  }

  out_glyph->width = cur_vfont.widths[code] + 1;
  out_glyph->height = cur_vfont.height;

  if (!render_now)
    return 1;

  // --- 第三层防御：读取过程中的健壮性 ---
  memset(internal_buf, 0, MAX_GLYPH_BUF_SIZE);

  uint32_t offset = 16 + 256 + (code * cur_vfont.bytes_per_char);
  static uint8_t raw_tmp[128]; // 确保足够装下最大点阵
  UINT br;

  if (f_lseek(cur_vfont.file_handle, offset) != FR_OK ||
      f_read(cur_vfont.file_handle, raw_tmp, cur_vfont.bytes_per_char, &br) !=
          FR_OK ||
      br != cur_vfont.bytes_per_char) {
    // 如果寻址或读取失败（比如 SD 卡被拔掉），依然靠 5x8 救场
    return Font_ASCII_5x8_Adapter(code, out_glyph, render_now);
  }

  // --- 正常的重排逻辑 ---
  int src_row_stride = (cur_vfont.max_width + 7) / 8;
  for (int h = 0; h < cur_vfont.height; h++) {
    // 假设渲染器要求的宽度步进对齐
    int dest_row_stride = (out_glyph->width + 7) / 8;
    if (h * dest_row_stride < MAX_GLYPH_BUF_SIZE) {
      memcpy(&internal_buf[h * dest_row_stride], &raw_tmp[h * src_row_stride],
             src_row_stride);
    }
  }

  out_glyph->p_bits = internal_buf;
  return 1;
}

uint8_t Font_Engine_Load_Bin(FIL *p_file) {
  uint8_t header[16];
  UINT br;

  f_lseek(p_file, 0);
  if (f_read(p_file, header, 16, &br) != FR_OK || br != 16)
    return 0;

  // 校验 Magic Number 'FONT'
  if (memcmp(header, "FONT", 4) != 0)
    return 0;

  cur_vfont.height = header[4];
  cur_vfont.max_width = header[5];
  // 小端解析 uint16 的 bytes_per_char
  cur_vfont.bytes_per_char = header[6] | (header[7] << 8);

  // 读取 256 字节宽度表到内存
  if (f_read(p_file, cur_vfont.widths, 256, &br) != FR_OK || br != 256)
    return 0;

  cur_vfont.file_handle = p_file;
  return 1;
}
