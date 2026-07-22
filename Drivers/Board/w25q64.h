#ifndef __W25Q64_H
#define __W25Q64_H

#include "main.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

/* ------------------ 硬件引脚控置 ------------------ */
#define W25QXX_CS_LOW()                                                        \
  HAL_GPIO_WritePin(W25QXX_CS_GPIO_Port, W25QXX_CS_Pin, GPIO_PIN_RESET)
#define W25QXX_CS_HIGH()                                                       \
  HAL_GPIO_WritePin(W25QXX_CS_GPIO_Port, W25QXX_CS_Pin, GPIO_PIN_SET)

/* ------------------ 参数与状态码定义 ------------------ */
#define W25Q64_JEDEC_ID 0xEF4017
#define W25QXX_PAGE_SIZE 256
#define W25QXX_SECTOR_SIZE 4096
#define W25QXX_BLOCK_SIZE 65536

typedef enum {
  W25QXX_OK = 0x00,
  W25QXX_ERROR = 0x01,
  W25QXX_TIMEOUT = 0x02,
  W25QXX_PARAM_ERR = 0x03
} W25QXX_StatusTypeDef;

/* ------------------ API 函数声明 ------------------ */
W25QXX_StatusTypeDef W25Q64_Init(SPI_HandleTypeDef *hspi);
uint32_t W25Q64_ReadID(void);

/* 读取系列 */
W25QXX_StatusTypeDef W25Q64_ReadData(uint32_t address, uint8_t *pBuffer,
                                     uint32_t size);
W25QXX_StatusTypeDef W25Q64_FastRead(uint32_t address, uint8_t *pBuffer,
                                     uint32_t size);

/* 写入与擦除 */
W25QXX_StatusTypeDef W25Q64_EraseSector(uint32_t address);
W25QXX_StatusTypeDef W25Q64_EraseBlock64K(uint32_t address);
W25QXX_StatusTypeDef W25Q64_EraseChip(void);
W25QXX_StatusTypeDef W25Q64_WriteBuffer(uint32_t address, uint8_t *pBuffer,
                                        uint32_t size);

/* 低功耗控制 */
void W25Q64_PowerDown(void);
void W25Q64_WAKEUP(void);

/* 测试用例入口 */
void W25Q64_Test_Run(void);

#endif /* __W25Q64_H */