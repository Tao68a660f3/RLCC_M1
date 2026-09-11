#include "font_config.h"
#include "flash_font.h"

/* ====================================================================
 * 字体配置实现
 *
 * 将 font_offsets.txt 中的地址与 GBK 元数据绑定，
 * 提供 FONT_xxx 宏序号 → Flash 适配器 slot ID 的映射
 * ==================================================================== */

/* --- ASCII 字库地址表（顺序与 FONT_ASC_xxx 宏一一对应） --------------- */
static const uint32_t g_asc_addrs[FONT_ASC_COUNT] = {
    FONT_ADDR_ASC0805, FONT_ADDR_ASC12,   FONT_ADDR_ASC1212, FONT_ADDR_ASC16,
    FONT_ADDR_ASC16G,  FONT_ADDR_ASC1608, FONT_ADDR_ASCVE14, FONT_ADDR_ASC2010,
    FONT_ADDR_ASCVE20, FONT_ADDR_ASC2410, FONT_ADDR_ASC2412, FONT_ADDR_ASC24G,
};

/* --- GBK 字库元数据表（顺序与 FONT_GBK_xxx 宏一一对应） -------------- */
typedef struct {
  uint32_t flash_addr;
  uint16_t width;
  uint16_t height;
  uint16_t bpc; /* ( (width+7)/8 ) * height */
} GBKMeta;

#define BPC(w, h) ((uint16_t)(((w) + 7) / 8) * (h))

static const GBKMeta g_gbk_metas[FONT_GBK_COUNT] = {
    {FONT_ADDR_GBK1212S, 12, 12, BPC(12, 12)},
    {FONT_ADDR_GBK1616H, 16, 16, BPC(16, 16)},
    {FONT_ADDR_GBK1616S, 16, 16, BPC(16, 16)},
    {FONT_ADDR_GBK1624M, 16, 24, BPC(16, 24)},
    {FONT_ADDR_GBK1624Y, 16, 24, BPC(16, 24)},
    {FONT_ADDR_GBK2424S, 24, 24, BPC(24, 24)},
    {FONT_ADDR_GBK2432S, 24, 32, BPC(24, 32)},
};

/* --- 映射表：宏序号 → Flash 适配器 slot ID（-1 表示未注册成功） ------ */
static int g_asc_map[FONT_ASC_COUNT]; /* 初始化为 -1 */
static int g_gbk_map[FONT_GBK_COUNT]; /* 初始化为 -1 */

/* ====================================================================
 * 实现
 * ==================================================================== */

void Font_Config_Init(void) {
  int i;

  /* 初始化映射表 */
  for (i = 0; i < FONT_ASC_COUNT; i++)
    g_asc_map[i] = FLASH_FONT_ID_INVALID;
  for (i = 0; i < FONT_GBK_COUNT; i++)
    g_gbk_map[i] = FLASH_FONT_ID_INVALID;

  /* 初始化 Flash 字库适配器 */
  Font_Flash_Init();

  /* --- 注册所有 ASCII 字库 --- */
  for (i = 0; i < FONT_ASC_COUNT; i++) {
    g_asc_map[i] = FlashASC_Register(g_asc_addrs[i]);
  }

  /* --- 注册所有 GBK 字库 --- */
  for (i = 0; i < FONT_GBK_COUNT; i++) {
    g_gbk_map[i] =
        FlashGBK_Register(g_gbk_metas[i].flash_addr, g_gbk_metas[i].width,
                          g_gbk_metas[i].height, g_gbk_metas[i].bpc);
  }

  /* 默认选中第一个 ASC 和第一个 GBK 字库 */
  if (g_asc_map[0] >= 0)
    FlashASC_Select(g_asc_map[0]);
  if (g_gbk_map[0] >= 0)
    FlashGBK_Select(g_gbk_map[0]);
}

void Font_Select_ASC(int font_id) {
  if (font_id >= 0 && font_id < FONT_ASC_COUNT && g_asc_map[font_id] >= 0)
    FlashASC_Select(g_asc_map[font_id]);
}

void Font_Select_GBK(int font_id) {
  if (font_id >= 0 && font_id < FONT_GBK_COUNT && g_gbk_map[font_id] >= 0)
    FlashGBK_Select(g_gbk_map[font_id]);
}

int Font_IsASCReady(int font_id) {
  if (font_id >= 0 && font_id < FONT_ASC_COUNT)
    return (g_asc_map[font_id] >= 0) ? 1 : 0;
  return 0;
}

int Font_IsGBKReady(int font_id) {
  if (font_id >= 0 && font_id < FONT_GBK_COUNT)
    return (g_gbk_map[font_id] >= 0) ? 1 : 0;
  return 0;
}