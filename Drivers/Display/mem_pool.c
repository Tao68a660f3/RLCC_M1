#include "mem_pool.h"
#include <string.h>
// #include <stdio.h>

// 1. 真正的物理存储区
// 我们用 static 保证它只在当前文件可见，避免被外部意外修改
static uint8_t mem_pool[POOL_SIZE];
static uint16_t ring_ptr = 0; // 全局环形写指针

// 2. 块管理表
// 这是一个结构体数组，用来记录“哪一段内存被谁用了”
static MemBlock block_table[MAX_BLOCKS];

/**
 * @brief 根据颜色选择对应的画布内存模式
 *        CANVAS_R:  红色像素写入 r_ptr (形状掩码从 r_ptr 读)
 *        CANVAS_G:  绿色像素写入 g_ptr (形状掩码从 g_ptr 读)
 *        CANVAS_Y:  红绿共用同一平面 r_ptr==g_ptr
 */
CanvasMode ColorToCanvasMode(CanvasColor color) {
  switch (color) {
  case C_RED:
    return CANVAS_R;
  case C_GREEN:
    return CANVAS_G;
  case C_YELLOW:
    return CANVAS_Y;
  default:
    return CANVAS_R;
  }
}

/**
 * 初始化内存池：就像开学前清空教室所有的座位表。
 */
void Pool_Init(void) {
  // 将整个大数组清零
  memset(mem_pool, 0, sizeof(mem_pool));
  // 将管理表条目全部标记为未使用（in_use = 0）
  memset(block_table, 0, sizeof(block_table));
}

CanvasHandle Pool_AllocCanvas(uint16_t w, uint16_t h, int16_t old_handle,
                              CanvasMode mode) {
  CanvasHandle handle = {NULL, NULL, 0, 0, -1};

  // 1. 计算需求并对齐
  uint16_t byte_width = (w + 7) / 8;
  uint16_t plane_size = byte_width * h;
  if (plane_size % 2 != 0)
    plane_size++;
  // 根据 mode 计算总大小：RG 需要双平面，其他模式只需单平面
  uint16_t total_size = (mode == CANVAS_RG) ? (plane_size * 2) : plane_size;

  // --- 策略 A: 原地复用 (原地复用是最高优先级，绝对安全) ---
  if (old_handle >= 0 && old_handle < MAX_BLOCKS &&
      block_table[old_handle].in_use) {
    if (total_size <= block_table[old_handle].size) {
      handle.handle = old_handle;
      handle.width = w;
      handle.height = h;
      handle.r_ptr = &mem_pool[block_table[old_handle].offset];
      // 根据 mode 设置 g_ptr 和清零
      switch (mode) {
      case CANVAS_RG:
        handle.g_ptr = &mem_pool[block_table[old_handle].offset + plane_size];
        for (uint16_t i = 0; i < plane_size; i++) {
          handle.r_ptr[i] = 0;
          handle.g_ptr[i] = 0;
        }
        break;
      case CANVAS_R:
        handle.g_ptr = NULL;
        for (uint16_t i = 0; i < plane_size; i++)
          handle.r_ptr[i] = 0;
        break;
      case CANVAS_G:
        handle.r_ptr = NULL;
        handle.g_ptr = &mem_pool[block_table[old_handle].offset];
        for (uint16_t i = 0; i < plane_size; i++)
          handle.g_ptr[i] = 0;
        break;
      case CANVAS_Y:
        handle.g_ptr = handle.r_ptr; // 共用同一平面
        for (uint16_t i = 0; i < plane_size; i++)
          handle.r_ptr[i] = 0;
        break;
      }
      return handle;
    }
  }

  // --- 策略 B: 环形分配逻辑 (增加碰撞检测) ---
  uint16_t try_offset = ring_ptr;
  if (try_offset + total_size > POOL_SIZE) {
    try_offset = 0; // 回绕
  }

  // 【新增】防追尾检查：遍历所有正在使用的块，看是否有重叠
  for (int i = 0; i < MAX_BLOCKS; i++) {
    if (block_table[i].in_use && i != old_handle) {
      uint16_t exist_start = block_table[i].offset;
      uint16_t exist_end = exist_start + block_table[i].size;
      uint16_t new_start = try_offset;
      uint16_t new_end = try_offset + total_size;

      // 检查区间重叠 (A < B_end && B < A_end)
      if (new_start < exist_end && exist_start < new_end) {
        //				printf("Ring Collision! Slot %d occupies
        //%d-%d\n", i, 						exist_start,
        // exist_end);
        // 方案：如果撞了，说明内存彻底满了，只能返回失败
        return handle;
      }
    }
  }

  // 寻找空闲管理槽位
  int16_t found_slot = -1;
  for (int i = 0; i < MAX_BLOCKS; i++) {
    if (!block_table[i].in_use) {
      found_slot = i;
      break;
    }
  }

  if (found_slot != -1) {
    block_table[found_slot].offset = try_offset;
    block_table[found_slot].size = total_size;
    block_table[found_slot].in_use = 1;

    handle.handle = found_slot;
    handle.width = w;
    handle.height = h;
    handle.r_ptr = &mem_pool[try_offset];

    // 根据 mode 设置 g_ptr 和清零
    switch (mode) {
    case CANVAS_RG:
      handle.g_ptr = &mem_pool[try_offset + plane_size];
      for (uint16_t i = 0; i < plane_size; i++) {
        handle.r_ptr[i] = 0;
        handle.g_ptr[i] = 0;
      }
      break;
    case CANVAS_R:
      handle.g_ptr = NULL;
      for (uint16_t i = 0; i < plane_size; i++)
        handle.r_ptr[i] = 0;
      break;
    case CANVAS_G:
      handle.r_ptr = NULL;
      handle.g_ptr = &mem_pool[try_offset];
      for (uint16_t i = 0; i < plane_size; i++)
        handle.g_ptr[i] = 0;
      break;
    case CANVAS_Y:
      handle.g_ptr = handle.r_ptr; // 共用同一平面
      for (uint16_t i = 0; i < plane_size; i++)
        handle.r_ptr[i] = 0;
      break;
    }

    ring_ptr = try_offset + total_size; // 更新指针
    //		printf("Ring Alloc:%dx%d @ %d\n", w, h, try_offset);
  }
  return handle;
}

void Pool_FreeCanvas(int16_t handle) {
  if (handle < 0 || handle >= MAX_BLOCKS)
    return;

  //	printf("--- Real Free: Slot %d, Offset %d ---\n", handle,
  //			block_table[handle].offset);

  block_table[handle].in_use = 0;
  block_table[handle].offset = 0; // 必须确保 offset 归零
  block_table[handle].size = 0;   // 必须确保 size 归零
}
