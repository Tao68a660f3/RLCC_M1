#include "window_manager.h"
#include "flash_font.h"
#include "mem_pool.h"
#include <stdint.h>
#include <string.h>

extern volatile uint8_t need_commit;

// ============================================================================
// 辅助函数：字符串遍历测量与绘制 (使用 Flash 字库适配器)
// ============================================================================

/**
 * @brief 测量一段字符串在画布上所需的像素尺寸
 * @param str   待测量的字符串 (UTF-8? 实际上为单字节 ASC + 双字节 GBK 混合)
 * @return Size2D 尺寸
 *
 * 使用 Font_Flash_ASC_Adapter / Font_Flash_GBK_Adapter 进行测量 (render_now=0)
 * 跳过 \r\n 换行符
 */
static Size2D _Window_MeasureText(const char *str) {
  DrawContext ctx = {0};
  Size2D size = {0};
  const char *p = str;
  while (*p) {
    if (*p == '\n' || *p == '\r') {
      if (*p == '\r' && *(p + 1) == '\n')
        p++;
      p++;
      continue;
    }
    GlyphInfo g;
    if ((uint8_t)*p < 0x80) {
      if (Font_Flash_ASC_Adapter((uint8_t)*p, &g, 0))
        Canvas_MeasureStep(&ctx, &g, &size);
      p++;
    } else {
      uint16_t gbk = ((uint16_t)(uint8_t)*p << 8) | (uint8_t)*(p + 1);
      if (Font_Flash_GBK_Adapter(gbk, &g, 0))
        Canvas_MeasureStep(&ctx, &g, &size);
      p += 2;
    }
  }
  return size;
}

/**
 * @brief 在已分配的 Canvas 上绘制字符串（支持行内竖直对齐）
 * @param ctx    绘图上下文 (需初始化 target, cur_x, cur_y 等)
 * @param str    待绘制字符串
 * @param color  颜色
 * @param valign 行内竖直对齐方式
 * @param row_h  当前行总高度 (px)
 *
 * 每个字符绘制前计算 y_offset = (row_h - glyph->height) * factor，
 * 临时偏移 ctx->cur_y 绘制后恢复，实现不同高度字符行内对齐。
 */
static void _Window_RenderText(DrawContext *ctx, const char *str,
                               CanvasColor color, Valign valign,
                               uint16_t row_h) {
  /* y_factor: 0=顶部, 128=居中, 255=底部，避免浮点运算 */
  uint8_t y_factor;
  switch (valign) {
  case VALIGN_TOP:
    y_factor = 0;
    break;
  case VALIGN_MIDDLE:
    y_factor = 128;
    break;
  case VALIGN_BOTTOM:
    y_factor = 255;
    break;
  default:
    y_factor = 0;
    break;
  }
  const char *p = str;
  while (*p) {
    if (*p == '\n' || *p == '\r') {
      if (*p == '\r' && *(p + 1) == '\n')
        p++;
      p++;
      continue;
    }
    GlyphInfo g;
    if ((uint8_t)*p < 0x80) {
      if (Font_Flash_ASC_Adapter((uint8_t)*p, &g, 1)) {
        uint16_t y_off =
            (row_h > g.height)
                ? (uint16_t)((uint32_t)(row_h - g.height) * y_factor / 255)
                : 0;
        ctx->cur_y += y_off;
        Canvas_DrawBitmap(ctx, &g, color);
        ctx->cur_y -= y_off;
      }
      p++;
    } else {
      uint16_t gbk = ((uint16_t)(uint8_t)*p << 8) | (uint8_t)*(p + 1);
      if (Font_Flash_GBK_Adapter(gbk, &g, 1)) {
        uint16_t y_off =
            (row_h > g.height)
                ? (uint16_t)((uint32_t)(row_h - g.height) * y_factor / 255)
                : 0;
        ctx->cur_y += y_off;
        Canvas_DrawBitmap(ctx, &g, color);
        ctx->cur_y -= y_off;
      }
      p += 2;
    }
  }
}

// 1. 窗口实例表（静态分配）
LED_Window window_list[MAX_WINDOWS];

/**
 * @brief 初始化窗口管理器
 */
void WindowManager_Init(void) {
  Pool_Init(); // 初始化画布内存池
  memset(window_list, 0, sizeof(window_list));
  for (int i = 0; i < MAX_WINDOWS; i++) {
    window_list[i].canvas.handle = -1; // 标记画布无效
  }
}

/**
 * @brief 配置窗口基础物理属性
 */
void Window_Config(uint8_t idx, int16_t x, int16_t y, uint16_t w, uint16_t h) {
  if (idx >= MAX_WINDOWS)
    return;
  window_list[idx].x = x;
  window_list[idx].y = y;
  window_list[idx].w = w;
  window_list[idx].h = h;
  window_list[idx].is_active = 1;
}

/**
 * @brief 设置窗口对齐逻辑 (计算 x_offset)
 */
void Window_SetAlignment(uint8_t idx, WinAlign align) {
  if (idx >= MAX_WINDOWS || window_list[idx].canvas.handle == -1)
    return;

  LED_Window *win = &window_list[idx];
  uint16_t content_w = win->canvas.width;

  switch (align) {
  case ALIGN_LEFT:
    win->x_offset = 0;
    break;
  case ALIGN_CENTER:
    win->x_offset = (win->w - content_w) / 2;
    break;
  case ALIGN_RIGHT:
    win->x_offset = win->w - content_w;
    break;
  }
}

// ============================================================================
// 窗口填充文本（Flash 字库适配器版）
// ============================================================================

/**
 * @brief 使用 Flash 字库适配器填充文本到指定窗口的画布
 * @param win_idx 窗口索引
 * @param str     待渲染的字符串 (混合 ASC + GBK)
 * @param color   前景色
 * @param mode    画布内存模式 (CANVAS_R / CANVAS_RG / CANVAS_Y / CANVAS_G)
 * @param valign  竖直对齐方式 (顶部/居中/底部)
 *
 * 流程：测量字符串尺寸 → Pool_AllocCanvas (带复用) → 绘制 → 原子切换窗口引用
 * 绘制完后根据 valign 和窗口高度计算 y_offset 实现竖直对齐
 */
void Window_FillText(uint8_t win_idx, const char *str, CanvasColor color,
                     CanvasMode mode, Valign valign) {
  if (win_idx >= MAX_WINDOWS || str == NULL)
    return;

  LED_Window *win = &window_list[win_idx];

  // --- 1. 测量阶段 ---
  Size2D size = _Window_MeasureText(str);

  // --- 2. 分配阶段（带原位复用检测） ---
  int16_t old_h = win->canvas.handle;
  CanvasHandle new_canvas = Pool_AllocCanvas(size.w, size.h, old_h, mode);

  if (new_canvas.handle != -1) {
    // --- 3. 绘制阶段 ---
    DrawContext ctx = {0};
    ctx.target = &new_canvas;
    ctx.cur_x = 0;
    ctx.cur_y = 0;
    _Window_RenderText(&ctx, str, color, valign, size.h);

    // --- 4. 原子切换 ---
    if (new_canvas.handle != old_h && old_h != -1) {
      Pool_FreeCanvas(old_h);
    }
    win->canvas = new_canvas;
  }
}

// ============================================================================
// 块搬运辅助内联函数
// ============================================================================

/**
 * @brief 根据 Canvas 字节 v 和 on_byte 生成 8 字节 AND 掩码
 * @param v       Canvas 字节 (1 位/像素, 共 8 个像素)
 * @param on_byte 像素亮时 AND 的值 (如 Q0 红: 0xFE; 灭时 AND 0xFF 不变)
 * @return 64 位掩码，每字节对应一个像素
 *
 * 流程：初始全 1(灭)，遍历 v 的每个位，为 1 的位对应字节中清除 on_byte 的零位，
 *       使该字节对应的 LED 亮起。
 */
static inline uint64_t mk_mask(uint8_t v, uint8_t on_byte) {
  uint64_t m = 0xFFFFFFFFFFFFFFFFULL; // 全 1(灭)
  uint8_t inv = ~on_byte;             // 需要清零的位
  if (v & 0x80)
    m &= ~((uint64_t)inv << 0);
  if (v & 0x40)
    m &= ~((uint64_t)inv << 8);
  if (v & 0x20)
    m &= ~((uint64_t)inv << 16);
  if (v & 0x10)
    m &= ~((uint64_t)inv << 24);
  if (v & 0x08)
    m &= ~((uint64_t)inv << 32);
  if (v & 0x04)
    m &= ~((uint64_t)inv << 40);
  if (v & 0x02)
    m &= ~((uint64_t)inv << 48);
  if (v & 0x01)
    m &= ~((uint64_t)inv << 56);
  return m;
}

/**
 * @brief 计算指定区和颜色的 on_byte (AND 掩码)
 * @param color_bits 颜色位: 1=RED, 2=GREEN, 3=YELLOW
 * @param quad       区号 0~3
 * @return on_byte, 亮像素 AND 此值, 灭像素 AND 0xFF
 *
 * 例: Q0 Red=0xFE(bit0=0), Q1 Green=0xF7(bit3=0), Q0 Yellow=0xFC(bit0,1=0)
 */
static inline uint8_t on_byte_for(uint8_t color_bits, uint8_t quad) {
  return ~((uint8_t)color_bits << (quad * 2)) & 0xFF;
}

/**
 * @brief 计算灭字节掩码 (OR 清空用)
 * @param quad 区号 0~3
 * @return 该区对应位为 1 的字节, OR 入显存可将该区像素置灭
 *
 * 例: Q0=0x03, Q1=0x0C, Q2=0x30, Q3=0xC0
 */
static inline uint8_t clear_byte_for(uint8_t quad) {
  return (uint8_t)(0x03 << (quad * 2));
}

// ============================================================================
// 核心渲染函数：行级块搬运
// ============================================================================

/**
 * @brief 判断 lyric 模式下整个块是否完全在高亮侧/底色侧/分裂
 * @return 0=全高亮侧, 1=全底色侧, 2=分裂(需逐像素)
 */
static inline uint8_t _block_side(int16_t bx, uint8_t bw, uint16_t h_px) {
  // bx: 块起始画布坐标(含偏移), bw: 块宽度(8或4)
  if (bx + (int16_t)bw <= (int16_t)h_px)
    return 0; // 完全在高亮侧
  if (bx >= (int16_t)h_px)
    return 1; // 完全在底色侧
  return 2;   // 分裂
}

/**
 * @brief 将窗口的 Canvas 以块搬运方式写入 frame_buffer
 * @param win        目标窗口指针
 * @param h_px      歌词高亮分界像素, 0xFFFF=静态文本模式
 * @param color_high 歌词亮色 (静态文本时忽略)
 * @param color_base 歌词底色 (静态文本时忽略)
 *
 * 静态文本模式 (h_px==0xFFFF):
 *   - 从 Canvas 读取红/绿两平面，用固定 on_r/on_g 着色
 *
 * 歌词着色模式 (h_px != 0xFFFF):
 *   - Canvas 作为形状掩码 (CANVAS_R 单色画布)
 *   - cx < h_px 用 color_high, 否则用 color_base
 *   - 跨分裂点块丢给逐像素兜底
 *
 * off_x/off_y: 额外偏移叠加在 win->x/y 上（歌词场景传 cfg->offset_x/y，其他传
 * 0） 画布坐标 = vx - win->x_offset, vy - win->y_offset
 */
void Window_BlitToScreen(LED_Window *win, uint16_t h_px, uint8_t color_high,
                         uint8_t color_base, int16_t off_x, int16_t off_y) {
  if (!win->is_active || win->canvas.handle == -1)
    return;

  CanvasHandle *cv = &win->canvas;
  uint16_t cv_bw = (cv->width + 7) / 8; // Canvas 每行字节数
  uint8_t wb = write_idx;               // 快照当前写缓冲区索引

  int16_t base_x = win->x + off_x;
  int16_t base_y = win->y + off_y;
  uint16_t w = win->w;
  uint16_t h = win->h;

  // 预计算偏移量: screen_x -> canvas_x = screen_x - (base_x + win->x_offset)
  int16_t canvas_origin_x = base_x + win->x_offset;

  // 歌词模式标志
  uint8_t is_lyric = (h_px != 0xFFFF);

  for (uint16_t vy = 0; vy < h; vy++) {
    // --- 计算屏幕行号并钳位 ---
    int16_t sy = base_y + (int16_t)vy;
    if (sy < 0 || sy >= HEIGHT)
      continue;

    uint8_t q = (uint8_t)(sy / 16);  // 区号 0~3
    uint8_t sr = (uint8_t)(sy % 16); // 扫描行号
    uint8_t *fb_row = frame_buffer[wb][sr];

    // --- 水平范围钳位 ---
    int16_t start_x = base_x;
    int16_t end_x = base_x + (int16_t)w;
    if (start_x < 0)
      start_x = 0;
    if (end_x > WIDTH)
      end_x = WIDTH;
    if (start_x >= end_x)
      continue;

    // ============================================================
    // 阶段 1: OR 清理 — 将本区像素全部置 1(灭)
    // ============================================================
    uint8_t cb = clear_byte_for(q);
    uint16_t cw = (uint16_t)cb * 0x0101; // 2 字节展开

    int16_t sx = start_x;
    // 对齐到 4 字节前的零头
    while ((sx & 3) && sx < end_x) {
      fb_row[sx] |= cb;
      sx++;
    }
    // 4 字节对齐块
    for (; sx + 3 < end_x; sx += 4) {
      *(uint32_t *)&fb_row[sx] |= (uint32_t)cw * 0x00010001;
    }
    // 尾部零头
    for (; sx < end_x; sx++) {
      fb_row[sx] |= cb;
    }

    // ============================================================
    // 阶段 2: 画布内容搬运
    // ============================================================
    int16_t cy = (int16_t)vy - win->y_offset;
    if (cy < 0 || cy >= (int16_t)cv->height)
      continue; // 画布外行, 已清完

    /* 确定形状源：CANVAS_G 时 r_ptr == NULL，用 g_ptr 替代 */
    uint8_t *shape_ptr = cv->r_ptr ? cv->r_ptr : cv->g_ptr;
    uint8_t *r_line = shape_ptr + (uint16_t)cy * cv_bw;
    /* has_g: 独立绿色平面（仅 CANVAS_RG 模式） */
    uint8_t has_g = (cv->g_ptr != NULL && cv->g_ptr != cv->r_ptr);
    uint8_t *g_line = has_g ? (cv->g_ptr + (uint16_t)cy * cv_bw) : NULL;
    /* is_yellow: 红绿共用同一平面，需要产生黄色 */
    uint8_t is_yellow = (cv->g_ptr != NULL && cv->g_ptr == cv->r_ptr);
    /* is_green_only: 仅有绿色平面 */
    uint8_t is_green_only = (cv->r_ptr == NULL && cv->g_ptr != NULL);

    // 静态文本 vs 歌词模式的 on_byte 选择
    uint8_t on_r, on_g;
    if (is_lyric) {
      // 歌词模式: on_r/on_g 暂不用(块级用 color_high/color_base 动态选择)
      on_r = 0xFF;
      on_g = 0xFF;
    } else if (is_yellow) {
      /* CANVAS_Y: 形状源同一平面编码所有颜色，用黄色 on_byte */
      on_r = on_byte_for(3, q);
      on_g = 0xFF;
    } else if (is_green_only) {
      /* CANVAS_G: 形状源来自 g_ptr，只染绿色 */
      on_r = on_byte_for(2, q);
      on_g = 0xFF;
    } else {
      /* CANVAS_R / CANVAS_RG: 正常红/绿两平面 */
      on_r = on_byte_for(1, q);
      on_g = on_byte_for(2, q);
    }

    sx = start_x;
    while (sx < end_x) {
      int16_t cx = sx - canvas_origin_x;
      if (cx < 0) {
        sx++;
        continue;
      }

      // ---- 8 像素块搬运 (条件：8 对齐 + 完全在画布内) ----
      // 注：不使用 *(uint64_t*) 以避免 STRD 不对齐异常 (DRIVER_WIDTH=129 非 8
      // 倍数)
      if ((sx & 7) == 0 && cx + 7 < (int16_t)cv->width) {
        uint16_t cb_idx = (uint16_t)(cx / 8);
        uint8_t bo = (uint8_t)(cx & 7); // bit offset 0~7

        if (cb_idx + 1 < cv_bw) {
          // 从 Canvas 跨字节取 8 位 (仅红色平面)
          uint16_t r_pair =
              ((uint16_t)r_line[cb_idx] << 8) | r_line[cb_idx + 1];
          uint8_t r_byte = (uint8_t)(r_pair >> (8 - bo));

          uint64_t mask;

          if (is_lyric) {
            // --- 歌词着色模式 ---
            uint8_t side = _block_side(cx, 8, h_px);
            if (side == 2) {
              goto fallback_per_pixel; // 分裂块，逐像素
            }
            uint8_t on_byte =
                on_byte_for((side == 0) ? color_high : color_base, q);
            mask = mk_mask(r_byte, on_byte);
          } else {
            // --- 静态文本模式 ---
            mask = mk_mask(r_byte, on_r);
            if (has_g) {
              uint16_t g_pair =
                  ((uint16_t)g_line[cb_idx] << 8) | g_line[cb_idx + 1];
              uint8_t g_byte = (uint8_t)(g_pair >> (8 - bo));
              mask &= mk_mask(g_byte, on_g);
            }
          }

          // 拆为两次 32 位写，避免 STRD 不对齐异常
          uint32_t mask_lo = (uint32_t)(mask >> 0);
          uint32_t mask_hi = (uint32_t)(mask >> 32);
          *(uint32_t *)&fb_row[sx + 0] &= mask_lo;
          *(uint32_t *)&fb_row[sx + 4] &= mask_hi;
          sx += 8;
          continue;
        }
      } else if ((sx & 3) == 0 && cx + 3 < (int16_t)cv->width) {
        // ---- 4 像素块搬运 (4 字节对齐 + 完全在画布内) ----
        // 用于 64 位不对齐时的次优路径
        uint16_t cb_idx = (uint16_t)(cx / 8);
        uint8_t bo = (uint8_t)(cx & 7);

        if (cb_idx + 1 < cv_bw) {
          uint16_t r_pair =
              ((uint16_t)r_line[cb_idx] << 8) | r_line[cb_idx + 1];
          uint8_t r_byte = (uint8_t)(r_pair >> (8 - bo));

          uint64_t full;

          if (is_lyric) {
            uint8_t side = _block_side(cx, 4, h_px);
            if (side == 2) {
              goto fallback_per_pixel;
            }
            uint8_t on_byte =
                on_byte_for((side == 0) ? color_high : color_base, q);
            full = mk_mask(r_byte, on_byte);
          } else {
            full = mk_mask(r_byte, on_r);
            if (has_g) {
              uint16_t g_pair =
                  ((uint16_t)g_line[cb_idx] << 8) | g_line[cb_idx + 1];
              uint8_t g_byte = (uint8_t)(g_pair >> (8 - bo));
              full &= mk_mask(g_byte, on_g);
            }
          }

          fb_row[sx + 0] = (uint8_t)(fb_row[sx + 0] & (full >> 0));
          fb_row[sx + 1] = (uint8_t)(fb_row[sx + 1] & (full >> 8));
          fb_row[sx + 2] = (uint8_t)(fb_row[sx + 2] & (full >> 16));
          fb_row[sx + 3] = (uint8_t)(fb_row[sx + 3] & (full >> 24));
          sx += 4;
          continue;
        }
      }

    // ---- 逐像素兜底 (非对齐块 / 分裂块 / 边界) ----
    fallback_per_pixel:
      if (cx >= 0 && cx < (int16_t)cv->width) {
        uint16_t cb_idx = (uint16_t)(cx / 8);
        uint8_t bit_pos = (uint8_t)(cx & 7);
        uint8_t bit_mask = (uint8_t)(1 << (7 - bit_pos));

        if (r_line[cb_idx] & bit_mask) {
          if (is_lyric) {
            uint8_t final_on =
                on_byte_for((cx < (int16_t)h_px) ? color_high : color_base, q);
            fb_row[sx] &= final_on;
          } else {
            fb_row[sx] &= on_r;
          }
        }
        if (!is_lyric && has_g && (g_line[cb_idx] & bit_mask)) {
          fb_row[sx] &= on_g;
        }
      }
      // 画布外像素: 已在 OR 阶段清灭，无需处理
      sx++;
    }
  }
}

void WindowManager_Single_Process(uint8_t i) {
  LED_Window *win = &window_list[i];
  if (!win->is_active)
    return;

  // --- 1. 处理分频滚动逻辑 ---
  if (win->scroll_divider > 0) {
    win->tick_counter++;
    if (win->tick_counter >= win->scroll_divider) {
      win->tick_counter = 0;
      win->x_offset += win->scroll_step;

      // 简单的循环滚动保护：当内容完全滚出左侧，从右侧回来
      if (win->scroll_step < 0 && win->x_offset < -(int16_t)win->canvas.width) {
        win->x_offset = win->w;
        win->scroll_counter++;
      } else if (win->scroll_step > 0 && win->x_offset > win->w) {
        win->x_offset = -(int16_t)win->canvas.width;
        win->scroll_counter++;
      }
    }
  }
  // --- 2. 执行渲染搬运 (块搬运，静态文本模式) ---
  Window_BlitToScreen(win, 0xFFFF, 0, 0, 0, 0);
}

/**
 * @brief 窗口大总管：逻辑更新 + 渲染执行
 * 在 While(1) 中调用
 */
void WindowManager_Process(void) {
  if (need_commit)
    return;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    WindowManager_Single_Process(i);
  }
  need_commit = 1;
}