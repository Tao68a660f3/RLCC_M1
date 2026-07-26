#ifndef __MEM_POOL_H
#define __MEM_POOL_H

#include <stddef.h>
#include <stdint.h>

// 颜色定义：使用位掩码，方便与运算
// 00(黑), 01(红), 10(绿), 11(黄)
typedef enum {
  C_BLACK = 0x00,
  C_RED = 0x01,
  C_GREEN = 0x02,
  C_YELLOW = 0x03
} CanvasColor;

// 画布内存模式：控制红绿色平面的分配策略
typedef enum {
  CANVAS_RG, // 红 + 绿 两个独立平面 (当前行为)
  CANVAS_R,  // 仅红色平面,  g_ptr = NULL
  CANVAS_G,  // 仅绿色平面,  r_ptr = NULL
  CANVAS_Y   // 红绿共用一平面 (纯黄模式), r_ptr = g_ptr
} CanvasMode;

#define POOL_SIZE 24576 // 静态内存池 (16K → 24K)
#define MAX_BLOCKS 20   // 最大同时存在的内存块数量 (12 → 20)

// 内存块句柄：应用层操作内容的凭证
typedef struct {
  uint8_t *r_ptr;  // 红色位平面起始地址
  uint8_t *g_ptr;  // 绿色位平面起始地址
  uint16_t width;  // 内容实际物理宽度
  uint16_t height; // 内容实际物理高度
  int16_t handle;  // 在管理表中的索引 (-1表示无效)
} CanvasHandle;

// 管理表条目
typedef struct {
  uint16_t offset; // 在 pool 中的起始偏移
  uint16_t size;   // 占用的总字节数
  uint8_t in_use;  // 占用标志
} MemBlock;

/* 基础 API */
CanvasMode ColorToCanvasMode(CanvasColor color);
void Pool_Init(void);
CanvasHandle Pool_AllocCanvas(uint16_t w, uint16_t h, int16_t old_handle,
                              CanvasMode mode);
void Pool_FreeCanvas(int16_t handle);

#endif
