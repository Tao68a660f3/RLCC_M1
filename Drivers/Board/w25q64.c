#include "w25q64.h"
#include <stdio.h>
#include <string.h>

/* 指令集定义 */
#define CMD_WRITE_ENABLE 0x06
#define CMD_READ_STATUS_1 0x05
#define CMD_READ_DATA 0x03
#define CMD_PAGE_PROGRAM 0x02
#define CMD_SECTOR_ERASE 0x20
#define CMD_JEDEC_ID 0x9F

static SPI_HandleTypeDef *w25q64_hspi = NULL;

/**
 * @brief 初始化 W25Q64 绑定的 SPI 句柄
 */
void W25Q64_Init(SPI_HandleTypeDef *hspi) {
  w25q64_hspi = hspi;
  W25QXX_CS_HIGH(); // 默认拉高片选
}

/**
 * @brief 等待内部操作完成 (BUSY 标志清零)
 */
static void W25Q64_WaitBusy(void) {
  uint8_t cmd = CMD_READ_STATUS_1;
  uint8_t status = 0;

  do {
    W25QXX_CS_LOW();
    HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);
    HAL_SPI_Receive(w25q64_hspi, &status, 1, HAL_MAX_DELAY);
    W25QXX_CS_HIGH();
  } while ((status & 0x01) == 0x01);
}

/**
 * @brief 发送写使能命令
 */
static void W25Q64_WriteEnable(void) {
  uint8_t cmd = CMD_WRITE_ENABLE;
  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();
}

/**
 * @brief 读取 JEDEC ID
 */
uint32_t W25Q64_ReadID(void) {
  uint8_t cmd = CMD_JEDEC_ID;
  uint8_t id_buf[3] = {0};

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);
  HAL_SPI_Receive(w25q64_hspi, id_buf, 3, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  return ((uint32_t)id_buf[0] << 16) | ((uint32_t)id_buf[1] << 8) | id_buf[2];
}

/**
 * @brief 从指定地址读取任意长度数据
 */
void W25Q64_ReadData(uint32_t address, uint8_t *pBuffer, uint32_t size) {
  uint8_t cmd[4];
  cmd[0] = CMD_READ_DATA;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 4, HAL_MAX_DELAY);
  HAL_SPI_Receive(w25q64_hspi, pBuffer, size, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();
}

/**
 * @brief 擦除 4KB 扇区
 */
void W25Q64_EraseSector(uint32_t address) {
  uint8_t cmd[4];
  cmd[0] = CMD_SECTOR_ERASE;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;

  W25Q64_WriteEnable();
  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 4, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  W25Q64_WaitBusy();
}

/**
 * @brief 单页写入 (不越界 256 字节)
 */
static void W25Q64_WritePage(uint32_t address, uint8_t *pBuffer,
                             uint16_t size) {
  uint8_t cmd[4];
  cmd[0] = CMD_PAGE_PROGRAM;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;

  W25Q64_WriteEnable();
  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 4, HAL_MAX_DELAY);
  HAL_SPI_Transmit(w25q64_hspi, pBuffer, size, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  W25Q64_WaitBusy();
}

/**
 * @brief 自动处理跨页边界的连续写入逻辑
 */
void W25Q64_WriteBuffer(uint32_t address, uint8_t *pBuffer, uint32_t size) {
  uint16_t page_offset = address % W25QXX_PAGE_SIZE;
  uint16_t bytes_left_in_page = W25QXX_PAGE_SIZE - page_offset;

  if (size <= bytes_left_in_page) {
    W25Q64_WritePage(address, pBuffer, size);
  } else {
    // 1. 先写完当前页剩余空间
    W25Q64_WritePage(address, pBuffer, bytes_left_in_page);
    address += bytes_left_in_page;
    pBuffer += bytes_left_in_page;
    size -= bytes_left_in_page;

    // 2. 整页写入
    while (size >= W25QXX_PAGE_SIZE) {
      W25Q64_WritePage(address, pBuffer, W25QXX_PAGE_SIZE);
      address += W25QXX_PAGE_SIZE;
      pBuffer += W25QXX_PAGE_SIZE;
      size -= W25QXX_PAGE_SIZE;
    }

    // 3. 写入最后一页剩余尾巴数据
    if (size > 0) {
      W25Q64_WritePage(address, pBuffer, size);
    }
  }
}

/* ------------------ 测试用例实现 ------------------ */
void W25Q64_Test_Run(void) {
  printf("\r\n================ W25Q64 Test Start ================\r\n");

  // 1. 测试读取 ID
  uint32_t id = W25Q64_ReadID();
  printf("1. Reading JEDEC ID: 0x%06X ", (unsigned int)id);
  if (id == W25Q64_JEDEC_ID) {
    printf("[OK]\r\n");
  } else {
    printf("[FAIL] Expected: 0x%06X\r\n", W25Q64_JEDEC_ID);
    return;
  }

  // 2. 测试擦除扇区
  uint32_t test_addr = 0x001000; // 测试地址：扇区 1
  printf("2. Erasing Sector at 0x%06X...\r\n", (unsigned int)test_addr);
  W25Q64_EraseSector(test_addr);

  uint8_t read_check[32] = {0};
  W25Q64_ReadData(test_addr, read_check, sizeof(read_check));
  uint8_t erase_ok = 1;
  for (int i = 0; i < sizeof(read_check); i++) {
    if (read_check[i] != 0xFF) {
      erase_ok = 0;
      break;
    }
  }
  printf("   Erase Check (Expected 0xFF): %s\r\n",
         erase_ok ? "[OK]" : "[FAIL]");
  if (!erase_ok)
    return;

  // 3. 测试跨页写入 & 读回校验
  uint8_t tx_buf[] =
      "STM32F401 SPI Flash Driver Test: Writing across pages automatically!";
  uint8_t rx_buf[sizeof(tx_buf)] = {0};

  // 故意选择一个非页对齐的地址（0x0010F0），测试自动切页处理逻辑
  uint32_t cross_page_addr = 0x0010F0;
  printf("3. Writing Data across page boundary to 0x%06X...\r\n",
         (unsigned int)cross_page_addr);
  W25Q64_WriteBuffer(cross_page_addr, tx_buf, sizeof(tx_buf));

  printf("4. Reading Back Data...\r\n");
  W25Q64_ReadData(cross_page_addr, rx_buf, sizeof(tx_buf));

  printf("   TX: %s\r\n", tx_buf);
  printf("   RX: %s\r\n", rx_buf);

  if (memcmp(tx_buf, rx_buf, sizeof(tx_buf)) == 0) {
    printf("====== Result: ALL TESTS PASSED! ======\r\n");
  } else {
    printf("====== Result: DATA MISMATCH! ======\r\n");
  }
}