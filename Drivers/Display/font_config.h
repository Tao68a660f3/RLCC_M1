#ifndef __FONT_CONFIG_H
#define __FONT_CONFIG_H

#include <stdint.h>

/* ====================================================================
 * 字体 Flash 地址定义
 * 源文件: resources/fonts/font_offsets.txt
 * ==================================================================== */

/* --- ASCCII 变宽字库 --- */
#define FONT_ADDR_ASC0805 0x000000
#define FONT_ADDR_ASC12 0x001000
#define FONT_ADDR_ASC1212 0x002000
#define FONT_ADDR_ASC16 0x004000
#define FONT_ADDR_ASC16G 0x006000
#define FONT_ADDR_ASC1608 0x008000
#define FONT_ADDR_ASCVE14 0x00A000
#define FONT_ADDR_ASC2010 0x00D000
#define FONT_ADDR_ASCVE20 0x010000
#define FONT_ADDR_ASC2410 0x014000
#define FONT_ADDR_ASC2412 0x018000
#define FONT_ADDR_ASC24G 0x01C000

/* --- GBK 汉字字库 --- */
#define FONT_ADDR_GBK1212S 0x020000
#define FONT_ADDR_GBK1616H 0x0A2000
#define FONT_ADDR_GBK1616S 0x14F000
#define FONT_ADDR_GBK1624M 0x1FC000
#define FONT_ADDR_GBK1624Y 0x2FF000
#define FONT_ADDR_GBK2424S 0x402000
#define FONT_ADDR_GBK2432S 0x587000

/* ====================================================================
 * 字体序号宏定义
 *
 * 使用示例：
 *   Font_Select_ASC(FONT_ASC_2412);
 *   Font_Select_GBK(FONT_GBK_2432S);
 * ==================================================================== */

/* --- ASCII 字体序号 --- */
#define FONT_ASC_0805 0
#define FONT_ASC_12 1
#define FONT_ASC_1212 2
#define FONT_ASC_16 3
#define FONT_ASC_16G 4
#define FONT_ASC_1608 5
#define FONT_ASCVE_14 6
#define FONT_ASC_2010 7
#define FONT_ASCVE_20 8
#define FONT_ASC_2410 9
#define FONT_ASC_2412 10
#define FONT_ASC_24G 11

#define FONT_ASC_COUNT 12 /* ASCII 字体总数 */

/* --- GBK 字体序号 --- */
#define FONT_GBK_1212S 0
#define FONT_GBK_1616H 1
#define FONT_GBK_1616S 2
#define FONT_GBK_1624M 3
#define FONT_GBK_1624Y 4
#define FONT_GBK_2424S 5
#define FONT_GBK_2432S 6

#define FONT_GBK_COUNT 7 /* GBK 字体总数 */

/* ====================================================================
 * 函数声明
 * ==================================================================== */

/**
 * @brief 初始化字体配置
 *
 * 调用 Font_Flash_Init() 检测 Flash，
 * 然后注册所有 ASC + GBK 字库到 Flash 适配器
 *
 * 在系统初始化阶段调用一次即可
 */
void Font_Config_Init(void);

/**
 * @brief 选择活跃的 ASC 字体
 * @param font_id  字体序号宏（如 FONT_ASC_2412）
 *                 若字体未注册成功，Select 将被忽略
 */
void Font_Select_ASC(int font_id);

/**
 * @brief 选择活跃的 GBK 字体
 * @param font_id  字体序号宏（如 FONT_GBK_2432S）
 *                 若字体未注册成功，Select 将被忽略
 */
void Font_Select_GBK(int font_id);

/** @brief 查询 ASC 字体 font_id 是否注册成功 */
int Font_IsASCReady(int font_id);

/** @brief 查询 GBK 字体 font_id 是否注册成功 */
int Font_IsGBKReady(int font_id);

#endif /* __FONT_CONFIG_H */