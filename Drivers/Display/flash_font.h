#ifndef __FLASH_FONT_H
#define __FLASH_FONT_H

#include "canvas_renderer.h"
#include <stdint.h>

/* ====================================================================
 * 基于 W25Q64 Flash 的字库适配器
 *
 * - ASCII 适配器：支持动态切换不同 BIN 格式的等宽/变宽 ASCII 字库
 * - GBK 适配器：支持动态切换不同大小的 GBK 汉字字库
 *
 * 回退策略：
 *   Flash 硬件故障 → ASC / GBK 均回退到内部 5x8 字库（'?'）
 *   Flash 正常但字符缺失 → 从当前字库读取替代字符:
 *     ASC: 从当前 ASC 字库读取 '?' (0x3F)
 *     GBK: 从当前 GBK 字库读取 '？' (0xA1A1)
 * ==================================================================== */

/* --- 注册 ID 类型 --- */
#define FLASH_FONT_ID_INVALID (-1)

/* ====================================================================
 * ASCII 变宽字库管理（BIN 格式，带 FONT header + 256 字节宽度表）
 * ==================================================================== */

/**
 * @brief 注册一个 ASCII BIN 字库
 * @param flash_addr  字库在 Flash 中的起始地址
 * @return font_id（>=0 成功，FLASH_FONT_ID_INVALID 失败）
 *
 * 注册时自动读取 16 字节 header + 256 字节宽度表到内存
 */
int FlashASC_Register(uint32_t flash_addr);

/**
 * @brief 切换到已注册的 ASCII 字库
 * @param font_id  由 FlashASC_Register 返回的 ID
 */
void FlashASC_Select(int font_id);

/* ====================================================================
 * GBK 汉字字库管理
 * ==================================================================== */

/**
 * @brief 注册一个 GBK 字库
 * @param flash_addr  字库在 Flash 中的起始地址
 * @param w           字符宽度（像素）
 * @param h           字符高度（像素）
 * @param bpc         每字符占用的字节数
 * @return font_id（>=0 成功，FLASH_FONT_ID_INVALID 失败）
 */
int FlashGBK_Register(uint32_t flash_addr, uint16_t w, uint16_t h,
                      uint16_t bpc);

/**
 * @brief 切换到已注册的 GBK 字库
 * @param font_id  由 FlashGBK_Register 返回的 ID
 */
void FlashGBK_Select(int font_id);

/* ====================================================================
 * 适配器函数（FontDriver 风格，与 canvas_renderer 配合使用）
 * ==================================================================== */

/**
 * @brief Flash ASCII 变宽字库适配器
 * @param code         ASCII 编码值 (0-255)
 * @param out_glyph    输出字形信息
 * @param render_now   0=仅测量尺寸，1=填充点阵数据
 * @return 1=成功 / 0=失败
 *
 * 回退链：当前 ASC 字库 '?' → 内部 5x8 字库（Flash 故障时）
 */
uint8_t Font_Flash_ASC_Adapter(uint32_t code, GlyphInfo *out_glyph,
                               uint8_t render_now);

/**
 * @brief Flash GBK 汉字字库适配器
 * @param gbk_code     GBK 编码 (0x8140-0xFEFE 范围)
 * @param out_glyph    输出字形信息
 * @param render_now   0=仅测量尺寸，1=填充点阵数据
 * @return 1=成功 / 0=失败
 *
 * 回退链：当前 GBK 字库 '？' → 内部 5x8 '?'（Flash 故障时）
 */
uint8_t Font_Flash_GBK_Adapter(uint32_t gbk_code, GlyphInfo *out_glyph,
                               uint8_t render_now);

/* ====================================================================
 * 初始化与状态查询
 * ==================================================================== */

/**
 * @brief 初始化 Flash 字库系统
 * 检测 W25Q64 是否正常连接，设置 flash_ok 标志
 * 在所有 Register / Adapter 调用之前应至少调用一次
 */
void Font_Flash_Init(void);

/**
 * @brief 查询 Flash 是否正常（供外部判断回退策略）
 */
uint8_t Font_Flash_IsReady(void);

#endif /* __FLASH_FONT_H */