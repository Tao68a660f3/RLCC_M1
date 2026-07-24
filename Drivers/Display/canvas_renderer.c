#include "canvas_renderer.h"
#include <string.h>

/**
 * @brief 核心点写入函数 (位平面处理)
 */
void Canvas_WritePixel(CanvasHandle *h, uint16_t x, uint16_t y,
                       CanvasColor color) {
  // 严格检查：handle有效性、坐标范围、空指针
  if (h->handle == -1 || x >= h->width || y >= h->height) {
    return;
  }

  uint16_t byte_width = (h->width + 7) / 8;
  uint32_t offset = (uint32_t)y * byte_width + (x / 8);
  uint8_t bit_mask = 0x80 >> (x % 8);

  // 红色平面 (存在时才写)
  if (h->r_ptr) {
    if (color & C_RED)
      h->r_ptr[offset] |= bit_mask;
    else
      h->r_ptr[offset] &= ~bit_mask;
  }

  // 绿色平面 (存在且不与红色共用时才写)
  if (h->g_ptr && h->g_ptr != h->r_ptr) {
    if (color & C_GREEN)
      h->g_ptr[offset] |= bit_mask;
    else
      h->g_ptr[offset] &= ~bit_mask;
  }
}

/**
 * @brief 获取某个点的颜色 (用于混合或判断)
 */
CanvasColor Canvas_GetPixel(CanvasHandle *h, uint16_t x, uint16_t y) {
  if (h->handle == -1 || x >= h->width || y >= h->height)
    return C_BLACK;

  uint16_t byte_width = (h->width + 7) / 8;
  uint32_t offset = (uint32_t)y * byte_width + (x / 8);
  uint8_t bit_mask = 0x80 >> (x % 8);

  uint8_t color = 0;
  if (h->r_ptr && (h->r_ptr[offset] & bit_mask))
    color |= C_RED;
  if (h->g_ptr && (h->g_ptr[offset] & bit_mask))
    color |= C_GREEN;
  // CANVAS_Y: g_ptr == r_ptr，两if都取同一位置，
  // C_RED|C_GREEN = C_YELLOW，语义正确

  return (CanvasColor)color;
}

/**
 * @brief 清空画布 (物理擦除)
 */
void Canvas_ClearCanvas(CanvasHandle *h) {
  if (h->handle == -1)
    return;
  uint32_t size = ((h->width + 7) / 8) * h->height;
  if (h->r_ptr)
    memset(h->r_ptr, 0, size);
  if (h->g_ptr && h->g_ptr != h->r_ptr)
    memset(h->g_ptr, 0, size);
}

/**
 * @brief 内部函数：处理笔尖换行逻辑 (供测量和绘制通用)
 */
static void _Canvas_HandleWrap(DrawContext *ctx, uint16_t next_w) {
  if (ctx->auto_wrap && ctx->wrap_width > 0) {
    if (ctx->cur_x + next_w > ctx->wrap_width && ctx->cur_x > 0) {
      ctx->cur_x = 0;
      ctx->cur_y += (ctx->line_h + ctx->line_space);
      ctx->line_h = 0;
    }
  }
}

/**
 * @brief 测量步骤 (不写内存)
 */
void Canvas_MeasureStep(DrawContext *ctx, const GlyphInfo *glyph,
                        Size2D *limit) {
  if (!glyph)
    return;

  _Canvas_HandleWrap(ctx, glyph->width);

  // 步进笔尖
  ctx->cur_x += (glyph->width + ctx->padding);
  if (glyph->height > ctx->line_h)
    ctx->line_h = glyph->height;

  // 更新边界矩形
  if (ctx->cur_x > limit->w)
    limit->w = ctx->cur_x;
  if (ctx->cur_y + ctx->line_h > limit->h)
    limit->h = ctx->cur_y + ctx->line_h;
}

/**
 * @brief 绘制位图 (实际搬运点阵)
 */
void Canvas_DrawBitmap(DrawContext *ctx, const GlyphInfo *glyph,
                       CanvasColor color) {
  if (!ctx->target || !glyph || !glyph->p_bits)
    return;

  _Canvas_HandleWrap(ctx, glyph->width);

  uint16_t byte_w = (glyph->width + 7) / 8;

  for (uint16_t y = 0; y < glyph->height; y++) {
    for (uint16_t x = 0; x < glyph->width; x++) {
      // 检查内容是否会超出画布物理边界
      if (ctx->cur_x + x < ctx->target->width &&
          ctx->cur_y + y < ctx->target->height) {
        // 取得原始点阵中的位 (假设横向扫描)
        if (glyph->p_bits[y * byte_w + (x / 8)] & (0x80 >> (x % 8))) {
          Canvas_WritePixel(ctx->target, ctx->cur_x + x, ctx->cur_y + y, color);
        }
      }
    }
  }

  // 步进笔尖坐标
  ctx->cur_x += (glyph->width + ctx->padding);
  if (glyph->height > ctx->line_h)
    ctx->line_h = glyph->height;
}
