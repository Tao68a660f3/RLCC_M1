#include "flash_font.h"
#include "inner_font.h"
#include "w25q64.h"
#include <string.h>

/* ====================================================================
 * 基于 W25Q64 Flash 的字库适配器 — 实现
 *
 * ASCII 字库: BIN 格式（FONT header + 256 宽度表 + 变宽点阵）
 * GBK 字库:  原始点阵格式（水平扫描 MSB-first）
 *
 * 注意：所有适配器输出的点阵格式均为 水平扫描 MSB-first
 *       以配合 Canvas_DrawBitmap 的渲染逻辑
 * ==================================================================== */

/* --- 常量定义 ----------------------------------------------------------------
 */
#define MAX_ASC_SLOTS 8    /* 最多注册 8 个 ASCII 字库 */
#define MAX_GBK_SLOTS 8    /* 最多注册 8 个 GBK 字库   */
#define GLYPH_BUF_SIZE 256 /* 输出点阵缓冲区（32x32 最大） */

/* --- Flash 字库头部偏移 ------------------------------------------------------
 */
#define BIN_HEADER_MAGIC_OFF 0
#define BIN_HEADER_HEIGHT_OFF 4
#define BIN_HEADER_WIDTH_OFF 5
#define BIN_HEADER_BPC_OFF 6 /* uint16, LE */
#define BIN_HEADER_CONFIG_OFF 8
#define BIN_HEADER_RESV_OFF 9 /* 7 bytes reserved */
#define BIN_HEADER_SIZE 16
#define BIN_WIDTH_TABLE_SIZE 256

/* ====================================================================
 * 数据结构
 * ==================================================================== */

/** ASCII 变宽字库槽位 */
typedef struct {
  uint32_t flash_addr;  /* Flash 起始地址                      */
  uint8_t height;       /* 字库高度（像素）                    */
  uint8_t max_width;    /* 字库最大宽度（点阵宽度）            */
  uint16_t bpc;         /* 每字符字节数                        */
  uint8_t config;       /* header 配置字节                     */
  uint8_t is_vert_scan; /* config bit 1: 1=垂直扫描            */
  uint8_t is_lsb;       /* config bit 2: 1=LSb 优先            */
  uint8_t stride;       /* 原始点阵每行/列步进字节数          */
  uint8_t widths[256];  /* 宽度表（已缓存到 RAM）              */
  uint8_t valid;        /* 1=槽位已被占用                      */
} ASCSlot;

/** GBK 汉字字库槽位 */
typedef struct {
  uint32_t flash_addr; /* Flash 起始地址                      */
  uint16_t width;      /* 字符宽度（像素）                    */
  uint16_t height;     /* 字符高度（像素）                    */
  uint16_t bpc;        /* 每字符字节数                        */
  uint8_t valid;       /* 1=槽位已被占用                      */
} GBKSlot;

/* --- 全局状态 ---------------------------------------------------------------
 */
static uint8_t g_flash_ok = 0;
static ASCSlot g_asc_slots[MAX_ASC_SLOTS];
static int g_asc_active = FLASH_FONT_ID_INVALID;
static GBKSlot g_gbk_slots[MAX_GBK_SLOTS];
static int g_gbk_active = FLASH_FONT_ID_INVALID;
static uint8_t g_glyph_buf[GLYPH_BUF_SIZE];

/* ====================================================================
 * 底层 Flash 读写封装
 * ==================================================================== */

/** 从 Flash 读取数据，失败返回 0 */
static uint8_t _flash_read(uint32_t addr, uint8_t *buf, uint32_t len) {
  if (!g_flash_ok)
    return 0;
  return (W25Q64_ReadData(addr, buf, len) == W25QXX_OK);
}

/** 从 Flash 读取一个 uint16（小端） */
static uint16_t _flash_read_u16(uint32_t addr) {
  uint8_t b[2];
  if (!_flash_read(addr, b, 2))
    return 0;
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

/* ====================================================================
 * 内部 5x8 回退适配器
 * 从 inner_font.h 的 Chars[] 数组中提取字形，
 * 将垂直扫描（5 字节/字符）转换为水平扫描 MSB-first 格式
 * ==================================================================== */
static uint8_t _fallback_5x8(uint32_t code, GlyphInfo *out_glyph,
                             uint8_t render_now) {
  out_glyph->width = 6; /* 5 像素宽 + 1 像素间距 */
  out_glyph->height = 8;

  if (!render_now) {
    out_glyph->p_bits = NULL;
    return 1;
  }

  /* 若编码无效，回退到 '?' (0x3F) */
  uint8_t c;
  if (code >= char_offset && code < (uint32_t)(char_offset + char_numsum))
    c = (uint8_t)code;
  else
    c = '?';

  memset(g_glyph_buf, 0, GLYPH_BUF_SIZE);

  /* Chars[] 是垂直扫描：每个字节是一列，MSB 是顶行
   * 转换为 Canvas_DrawBitmap 所需的水平扫描 MSB-first */
  uint16_t font_ptr = (c - char_offset) * 5;

  for (uint8_t row = 0; row < 8; row++) {
    uint8_t row_byte = 0;
    for (uint8_t col = 0; col < 5; col++) {
      if (Chars[font_ptr + col] & (0x80 >> row))
        row_byte |= (0x80 >> col);
    }
    g_glyph_buf[row] = row_byte;
  }

  out_glyph->p_bits = g_glyph_buf;
  return 1;
}

/* ====================================================================
 * ASC 原始点阵解码器
 * 将任意扫描方向/位序的原始点阵，打包为水平扫描 MSB-first 格式
 * ==================================================================== */

/**
 * @brief 判断原始点阵中 (x, y) 像素是否点亮
 *
 * @param raw       原始点阵数据
 * @param max_w     点阵原始宽度（max_width）
 * @param h         点阵高度
 * @param is_vert   是否垂直扫描
 * @param is_lsb    是否 LSb 优先
 * @param x         输出水平坐标
 * @param y         输出垂直坐标
 */
static uint8_t _asc_decode_pixel(const uint8_t *raw, uint8_t max_w, uint8_t h,
                                 uint8_t is_vert, uint8_t is_lsb, uint16_t x,
                                 uint16_t y) {
  /* 原始点阵的有效范围：
   *   水平扫描: x ∈ [0, max_w), y ∈ [0, h)
   *   垂直扫描: x ∈ [0, max_w), y ∈ [0, h)
   * 超出此范围的像素（例如 ext_w 添加的额外列）不存在于原始数据中，返回 0
   */
  uint8_t main_limit = is_vert ? max_w : h;
  uint8_t sub_limit = is_vert ? h : max_w;

  uint8_t m = is_vert ? (uint8_t)x : (uint8_t)y;
  uint8_t s = is_vert ? (uint8_t)y : (uint8_t)x;

  if (m >= main_limit || s >= sub_limit)
    return 0;

  uint8_t stride = is_vert ? ((h + 7) / 8) : ((max_w + 7) / 8);

  uint16_t byte_pos = (uint16_t)m * stride + (s / 8);
  uint8_t bit_pos = is_lsb ? (s % 8) : (7 - (s % 8));

  return (raw[byte_pos] >> bit_pos) & 1;
}

/**
 * @brief 将原始点阵打包为水平扫描 MSB-first 格式
 *
 * @param out       输出缓冲区（大小需 ≥ h * ((out_w+7)/8)）
 * @param out_w     输出宽度（像素，即实际字符宽度）
 * @param h         输出高度（像素）
 * @param raw       原始点阵数据
 * @param max_w     原始点阵宽度（max_width）
 * @param is_vert   是否垂直扫描
 * @param is_lsb    是否 LSb 优先
 */
static void _asc_pack_horizontal(uint8_t *out, uint8_t out_w, uint8_t h,
                                 const uint8_t *raw, uint8_t max_w,
                                 uint8_t is_vert, uint8_t is_lsb) {
  uint8_t out_stride = (out_w + 7) / 8;
  memset(out, 0, (uint16_t)h * out_stride);

  for (uint8_t y = 0; y < h; y++) {
    for (uint8_t x = 0; x < out_w; x++) {
      if (_asc_decode_pixel(raw, max_w, h, is_vert, is_lsb, x, y)) {
        out[(uint16_t)y * out_stride + (x / 8)] |= (0x80 >> (x % 8));
      }
    }
  }
}

/* ====================================================================
 * GBK 索引计算（与 C# CUSTOM_GBK_READ.cs 新版逻辑对齐）
 *
 * 5 个区 + 补丁区，不跳过 0x7F，总计 22084 个编码位置
 * ==================================================================== */
static int _gbk_get_index(uint16_t code) {
  uint8_t h = (uint8_t)(code >> 8);
  uint8_t l = (uint8_t)(code & 0xFF);

  /* 1. 常用符号区: 0xA1A1 - 0xA9FE (846 字) */
  if (h >= 0xA1 && h <= 0xA9 && l >= 0xA1 && l <= 0xFE)
    return (h - 0xA1) * 94 + (l - 0xA1);

  int base = 846;

  /* 2. GBK 扩充 5 区: 0xA840 - 0xA9A0 (194 字, 每行 97) */
  if (h >= 0xA8 && h <= 0xA9 && l >= 0x40 && l <= 0xA0)
    return base + (h - 0xA8) * 97 + (l - 0x40);

  base += 194;

  /* 3. 核心汉字区: 0xB0A1 - 0xF7FE (6768 字) */
  if (h >= 0xB0 && h <= 0xF7 && l >= 0xA1 && l <= 0xFE)
    return base + (h - 0xB0) * 94 + (l - 0xA1);

  base += 6768;

  /* 4. GBK 扩充 3 区: 0x8140 - 0xA0FE (6112 字, 每行 191) */
  if (h >= 0x81 && h <= 0xA0 && l >= 0x40 && l <= 0xFE)
    return base + (h - 0x81) * 191 + (l - 0x40);

  base += 6112;

  /* 5. GBK 扩充 4 区: 0xAA40 - 0xFDA0 (8148 字, 每行 97) */
  if (h >= 0xAA && h <= 0xFD && l >= 0x40 && l <= 0xA0)
    return base + (h - 0xAA) * 97 + (l - 0x40);

  base += 8148;

  /* 6. 补丁区: 0xFE40 - 0xFE4F (16 字) */
  if (h == 0xFE && l >= 0x40 && l <= 0x4F)
    return base + (l - 0x40);

  return -1;
}

/* ====================================================================
 * ASCII 字库管理
 * ==================================================================== */

int FlashASC_Register(uint32_t flash_addr) {
  if (!g_flash_ok)
    return FLASH_FONT_ID_INVALID;

  /* 找空闲槽位 */
  int slot = FLASH_FONT_ID_INVALID;
  for (int i = 0; i < MAX_ASC_SLOTS; i++) {
    if (!g_asc_slots[i].valid) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return FLASH_FONT_ID_INVALID;

  /* 读取 header */
  uint8_t header[BIN_HEADER_SIZE];
  if (!_flash_read(flash_addr, header, BIN_HEADER_SIZE))
    return FLASH_FONT_ID_INVALID;

  /* 校验 Magic */
  if (header[0] != 'F' || header[1] != 'O' || header[2] != 'N' ||
      header[3] != 'T')
    return FLASH_FONT_ID_INVALID;

  /* 解析 header */
  ASCSlot *s = &g_asc_slots[slot];
  s->flash_addr = flash_addr;
  s->height = header[BIN_HEADER_HEIGHT_OFF];
  s->max_width = header[BIN_HEADER_WIDTH_OFF];
  s->bpc = (uint16_t)header[BIN_HEADER_BPC_OFF] |
           ((uint16_t)header[BIN_HEADER_BPC_OFF + 1] << 8);
  s->config = header[BIN_HEADER_CONFIG_OFF];
  s->is_vert_scan = (s->config & 0x02) ? 1 : 0;
  s->is_lsb = (s->config & 0x04) ? 1 : 0;

  if (s->is_vert_scan)
    s->stride = (s->height + 7) / 8;
  else
    s->stride = (s->max_width + 7) / 8;

  /* 读取宽度表 */
  if (!_flash_read(flash_addr + BIN_HEADER_SIZE, s->widths, 256)) {
    s->valid = 0;
    return FLASH_FONT_ID_INVALID;
  }

  s->valid = 1;
  return slot;
}

void FlashASC_Select(int font_id) {
  if (font_id >= 0 && font_id < MAX_ASC_SLOTS && g_asc_slots[font_id].valid)
    g_asc_active = font_id;
  else
    g_asc_active = FLASH_FONT_ID_INVALID;
}

/* ====================================================================
 * GBK 字库管理
 * ==================================================================== */

int FlashGBK_Register(uint32_t flash_addr, uint16_t w, uint16_t h,
                      uint16_t bpc) {
  if (!g_flash_ok)
    return FLASH_FONT_ID_INVALID;

  int slot = FLASH_FONT_ID_INVALID;
  for (int i = 0; i < MAX_GBK_SLOTS; i++) {
    if (!g_gbk_slots[i].valid) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return FLASH_FONT_ID_INVALID;

  GBKSlot *s = &g_gbk_slots[slot];
  s->flash_addr = flash_addr;
  s->width = w;
  s->height = h;
  s->bpc = bpc;
  s->valid = 1;
  return slot;
}

void FlashGBK_Select(int font_id) {
  if (font_id >= 0 && font_id < MAX_GBK_SLOTS && g_gbk_slots[font_id].valid)
    g_gbk_active = font_id;
  else
    g_gbk_active = FLASH_FONT_ID_INVALID;
}

/* ====================================================================
 * ASCII 适配器
 * ==================================================================== */

uint8_t Font_Flash_ASC_Adapter(uint32_t code, GlyphInfo *out_glyph,
                               uint8_t render_now) {
  /* --- 1. Flash 故障 → 内部 5x8 --- */
  if (!g_flash_ok)
    return _fallback_5x8(code, out_glyph, render_now);

  /* --- 2. 无活跃字库 → 失败 --- */
  if (g_asc_active < 0)
    return 0;

  const ASCSlot *s = &g_asc_slots[g_asc_active];

  /* --- 3. 编码范围检查 --- */
  if (code > 255)
    return _fallback_5x8('?', out_glyph, render_now);

  /* --- 4. 宽度表查询 --- */
  uint8_t raw_width = s->widths[code];

  /* 该字符不存在于当前字库 → 尝试同字库的 '?' */
  if (raw_width == 0) {
    code = '?'; /* 0x3F */
    raw_width = s->widths[code];
    if (raw_width == 0)
      return _fallback_5x8('?', out_glyph, render_now);
  }

  /* --- 5. 计算实际显示宽度 --- */
  /* Python: if not config & 0x01: ext_w = 1 */
  uint8_t actual_width = raw_width + ((s->config & 0x01) ? 0 : 1);

  out_glyph->width = actual_width;
  out_glyph->height = s->height;

  if (!render_now) {
    out_glyph->p_bits = NULL;
    return 1;
  }

  /* --- 6. 从 Flash 读取原始点阵数据 --- */
  uint32_t data_offset = s->flash_addr + BIN_HEADER_SIZE +
                         BIN_WIDTH_TABLE_SIZE + (uint32_t)code * s->bpc;

  /* 临时缓冲区存放原始格式点阵（最大 128 字节足够） */
  uint8_t raw_buf[128];
  if (!_flash_read(data_offset, raw_buf, s->bpc))
    return _fallback_5x8('?', out_glyph, render_now);

  /* --- 7. 解码并重排为水平扫描 MSB-first 格式 --- */
  _asc_pack_horizontal(g_glyph_buf, actual_width, s->height, raw_buf,
                       s->max_width, s->is_vert_scan, s->is_lsb);

  out_glyph->p_bits = g_glyph_buf;
  return 1;
}

/* ====================================================================
 * GBK 适配器
 * ==================================================================== */

uint8_t Font_Flash_GBK_Adapter(uint32_t gbk_code, GlyphInfo *out_glyph,
                               uint8_t render_now) {
  /* --- 1. Flash 故障 → 内部 5x8 '?' --- */
  if (!g_flash_ok)
    return _fallback_5x8('?', out_glyph, render_now);

  /* --- 2. 无活跃字库 → 失败 --- */
  if (g_gbk_active < 0)
    return 0;

  const GBKSlot *s = &g_gbk_slots[g_gbk_active];

  /* --- 3. 计算字符索引 --- */
  int char_idx = _gbk_get_index((uint16_t)(gbk_code & 0xFFFF));

  /* 编码不识别 → 尝试当前 GBK 字库的 '？' (0xA1A1) */
  if (char_idx < 0) {
    char_idx = _gbk_get_index(0xA1A1);
    if (char_idx < 0)
      return _fallback_5x8('?', out_glyph, render_now);
  }

  out_glyph->width = s->width;
  out_glyph->height = s->height;

  if (!render_now) {
    out_glyph->p_bits = NULL;
    return 1;
  }

  /* --- 4. 从 Flash 读取点阵数据 --- */
  uint32_t offset = s->flash_addr + (uint32_t)char_idx * s->bpc;

  if (!_flash_read(offset, g_glyph_buf, s->bpc))
    return _fallback_5x8('?', out_glyph, render_now);

  out_glyph->p_bits = g_glyph_buf;
  return 1;
}

/* ====================================================================
 * 初始化与状态查询
 * ==================================================================== */

void Font_Flash_Init(void) {
  /* 检测 W25Q64 是否在线 */
  uint32_t id = W25Q64_ReadID();
  g_flash_ok = (id == W25Q64_JEDEC_ID);

  /* 清空所有槽位 */
  memset(g_asc_slots, 0, sizeof(g_asc_slots));
  memset(g_gbk_slots, 0, sizeof(g_gbk_slots));
  g_asc_active = FLASH_FONT_ID_INVALID;
  g_gbk_active = FLASH_FONT_ID_INVALID;
}

uint8_t Font_Flash_IsReady(void) { return g_flash_ok; }