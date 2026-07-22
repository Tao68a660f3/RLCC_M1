#ifndef __W25Q64_H
#define __W25Q64_H

#include "main.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>


/* ------------------ 硬件引脚控制 ------------------ */
#define W25QXX_CS_LOW()                                                        \
  HAL_GPIO_WritePin(W25QXX_CS_GPIO_Port, W25QXX_CS_Pin, GPIO_PIN_RESET)
#define W25QXX_CS_HIGH()                                                       \
  HAL_GPIO_WritePin(W25QXX_CS_GPIO_Port, W25QXX_CS_Pin, GPIO_PIN_SET)

/* ------------------ Flash 参数定义 ------------------ */
#define W25Q64_JEDEC_ID 0xEF4017
#define W25QXX_PAGE_SIZE 256
#define W25QXX_SECTOR_SIZE 4096

/* ------------------ 函数声明 ------------------ */
void W25Q64_Init(SPI_HandleTypeDef *hspi);
uint32_t W25Q64_ReadID(void);
void W25Q64_ReadData(uint32_t address, uint8_t *pBuffer, uint32_t size);
void W25Q64_EraseSector(uint32_t address);
void W25Q64_WriteBuffer(uint32_t address, uint8_t *pBuffer, uint32_t size);

/* 测试用例入口 */
void W25Q64_Test_Run(void);

#endif /* __W25Q64_H */