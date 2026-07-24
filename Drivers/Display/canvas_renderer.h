#ifndef __CANVAS_RENDERER_H
#define __CANVAS_RENDERER_H

#include "mem_pool.h"

// 颜色定义：使用位掩码，方便与运算
// 00(黑), 01(红), 10(绿), 11(黄)
typedef enum
{
	C_BLACK = 0x00, C_RED = 0x01, C_GREEN = 0x02, C_YELLOW = 0x03
} CanvasColor;

// 字符/位图元数据：两遍走逻辑的核心桥梁
typedef struct
{
	uint16_t width;        // 位图宽度 (pixel)
	uint16_t height;       // 位图高度 (pixel)
	const uint8_t *p_bits; // 指向位图点阵数据的指针
} GlyphInfo;

// 绘图上下文：记录笔尖位置与排版规则
typedef struct
{
	CanvasHandle *target;  // 当前操作的画布
	uint16_t cur_x;        // 笔尖当前 X 坐标
	uint16_t cur_y;        // 笔尖当前 Y 坐标
	uint16_t line_h;       // 当前行最高高度
	uint16_t wrap_width;   // 换行边界 (0表示不限制)
	uint16_t padding;      // 字间距/元素间距
	uint16_t line_space;   // 行间距
	uint8_t auto_wrap;    // 是否开启自动换行
} DrawContext;

// 2D 尺寸结构体
typedef struct
{
	uint16_t w;
	uint16_t h;
} Size2D;

/* --- 基础像素操作 --- */
void Canvas_WritePixel(CanvasHandle *h, uint16_t x, uint16_t y,
		CanvasColor color);
CanvasColor Canvas_GetPixel(CanvasHandle *h, uint16_t x, uint16_t y);
void Canvas_ClearCanvas(CanvasHandle *h); // 清空整个画布

/* --- 排版与绘制 --- */
// 第一遍：模拟测量，更新 limit
void Canvas_MeasureStep(DrawContext *ctx, const GlyphInfo *glyph, Size2D *limit);
// 第二遍：实际绘制
void Canvas_DrawBitmap(DrawContext *ctx, const GlyphInfo *glyph,
		CanvasColor color);

#endif
