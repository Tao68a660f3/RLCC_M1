#ifndef __FONT_CONFIG_H
#define __FONT_CONFIG_H

#include <stdint.h>

/* ====================================================================
 * 字体 Flash 地址定义
 * 源文件: resources/fonts/font_offsets.txt
 * ==================================================================== */

/* --- ASCCII 变宽字库 --- */
#define FONT_ADDR_ASC0805 0x000000
#define FONT_ADDR_ASC16 0x001000
#define FONT_ADDR_ASC16S 0x003000
#define FONT_ADDR_ASC1608 0x005000
#define FONT_ADDR_ASC1610 0x007000
#define FONT_ADDR_ASC1616 0x00A000
#define FONT_ADDR_ASC2010 0x00D000
#define FONT_ADDR_ASC2410 0x010000
#define FONT_ADDR_ASC2412 0x014000
#define FONT_ADDR_ASCVE20 0x018000

/* --- GBK 汉字字库 --- */
#define FONT_ADDR_GBK1616H 0x01C000
#define FONT_ADDR_GBK1616S 0x0C9000
#define FONT_ADDR_GBK1624M 0x176000
#define FONT_ADDR_GBK1624Y 0x279000
#define FONT_ADDR_GBK2432S 0x37C000
#define FONT_ADDR_GBK2432H 0x582000

/* ====================================================================
 * 字体序号宏定义
 *
 * 使用示例：
 *   Font_Select_ASC(FONT_ASC_2412);
 *   Font_Select_GBK(FONT_GBK_2432S);
 * ==================================================================== */

/* --- ASCII 字体序号 --- */
#define FONT_ASC_0805 0
#define FONT_ASC_16 1
#define FONT_ASC_16S 2
#define FONT_ASC_1608 3
#define FONT_ASC_1610 4
#define FONT_ASC_1616 5
#define FONT_ASC_2010 6
#define FONT_ASC_2410 7
#define FONT_ASC_2412 8
#define FONT_ASCV_20 9

#define FONT_ASC_COUNT 10 /* ASCII 字体总数 */

/* --- GBK 字体序号 --- */
#define FONT_GBK_1616H 0
#define FONT_GBK_1616S 1
#define FONT_GBK_1624M 2
#define FONT_GBK_1624Y 3
#define FONT_GBK_2432S 4
#define FONT_GBK_2432H 5

#define FONT_GBK_COUNT 6 /* GBK 字体总数 */

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