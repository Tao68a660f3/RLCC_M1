#include "w25q64.h"
#include <stdio.h>
#include <string.h>

/* 常用 Flash 指令集 */
#define CMD_WRITE_ENABLE 0x06
#define CMD_READ_STATUS_1 0x05
#define CMD_WRITE_STATUS_1 0x01
#define CMD_READ_DATA 0x03
#define CMD_FAST_READ 0x0B
#define CMD_PAGE_PROGRAM 0x02
#define CMD_SECTOR_ERASE 0x20
#define CMD_BLOCK_ERASE_64K 0xD8
#define CMD_CHIP_ERASE 0xC7
#define CMD_JEDEC_ID 0x9F
#define CMD_POWER_DOWN 0xB9
#define CMD_RELEASE_POWERDOWN 0xAB

static SPI_HandleTypeDef *w25q64_hspi = NULL;

/**
 * @brief  带超时机制的 BUSY 状态轮询（防止硬件故障导致 MCU 卡死）
 */
static W25QXX_StatusTypeDef W25Q64_WaitBusy(uint32_t timeout_ms) {
  uint8_t cmd = CMD_READ_STATUS_1;
  uint8_t status = 0;
  uint32_t tickstart = HAL_GetTick();

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);

  do {
    HAL_SPI_Receive(w25q64_hspi, &status, 1, HAL_MAX_DELAY);
    if ((status & 0x01) == 0x00) { // BUSY bit cleared
      W25QXX_CS_HIGH();
      return W25QXX_OK;
    }
  } while ((HAL_GetTick() - tickstart) < timeout_ms);

  W25QXX_CS_HIGH();
  return W25QXX_TIMEOUT;
}

/**
 * @brief  写使能
 */
static W25QXX_StatusTypeDef W25Q64_WriteEnable(void) {
  uint8_t cmd = CMD_WRITE_ENABLE;
  W25QXX_CS_LOW();
  HAL_StatusTypeDef status =
      HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();
  return (status == HAL_OK) ? W25QXX_OK : W25QXX_ERROR;
}

/**
 * @brief  初始化 W25Q64 并解开写保护
 */
W25QXX_StatusTypeDef W25Q64_Init(SPI_HandleTypeDef *hspi) {
  w25q64_hspi = hspi;
  W25QXX_CS_HIGH();

  // 唤醒 Flash（防止处于休眠状态）
  W25Q64_WAKEUP();

  // 检查芯片 ID
  if (W25Q64_ReadID() != W25Q64_JEDEC_ID) {
    return W25QXX_ERROR;
  }

  return W25QXX_OK;
}

/**
 * @brief  读取 JEDEC ID
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
 * @brief  普通读取 (最高支持 50MHz)
 */
W25QXX_StatusTypeDef W25Q64_ReadData(uint32_t address, uint8_t *pBuffer,
                                     uint32_t size) {
  if (pBuffer == NULL || size == 0)
    return W25QXX_PARAM_ERR;

  uint8_t cmd[4];
  cmd[0] = CMD_READ_DATA;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 4, HAL_MAX_DELAY);
  HAL_StatusTypeDef status =
      HAL_SPI_Receive(w25q64_hspi, pBuffer, size, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  return (status == HAL_OK) ? W25QXX_OK : W25QXX_ERROR;
}

/**
 * @brief  快速读取 (Fast Read，推荐高频时使用)
 */
W25QXX_StatusTypeDef W25Q64_FastRead(uint32_t address, uint8_t *pBuffer,
                                     uint32_t size) {
  if (pBuffer == NULL || size == 0)
    return W25QXX_PARAM_ERR;

  uint8_t cmd[5];
  cmd[0] = CMD_FAST_READ;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;
  cmd[4] = 0xFF; // Dummy Byte（空周期，用于高频下让 Flash 内部流水线就绪）

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 5, HAL_MAX_DELAY);
  HAL_StatusTypeDef status =
      HAL_SPI_Receive(w25q64_hspi, pBuffer, size, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  return (status == HAL_OK) ? W25QXX_OK : W25QXX_ERROR;
}

/**
 * @brief  擦除 4KB 扇区 (典型耗时 45ms, 最长 400ms)
 */
W25QXX_StatusTypeDef W25Q64_EraseSector(uint32_t address) {
  if (W25Q64_WriteEnable() != W25QXX_OK)
    return W25QXX_ERROR;

  uint8_t cmd[4];
  cmd[0] = CMD_SECTOR_ERASE;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 4, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  return W25Q64_WaitBusy(500); // 500ms 超时限制
}

/**
 * @brief  擦除 64KB 大块 (适合快速大面积擦除，典型耗时 150ms)
 */
W25QXX_StatusTypeDef W25Q64_EraseBlock64K(uint32_t address) {
  if (W25Q64_WriteEnable() != W25QXX_OK)
    return W25QXX_ERROR;

  uint8_t cmd[4];
  cmd[0] = CMD_BLOCK_ERASE_64K;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 4, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  return W25Q64_WaitBusy(2000); // 2000ms 超时限制
}

/**
 * @brief  整片全擦除 (耗时极长，约 20~100秒)
 */
W25QXX_StatusTypeDef W25Q64_EraseChip(void) {
  if (W25Q64_WriteEnable() != W25QXX_OK)
    return W25QXX_ERROR;

  uint8_t cmd = CMD_CHIP_ERASE;
  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  return W25Q64_WaitBusy(100000); // 100秒超时限制
}

/**
 * @brief  底层单页编程 (<=256 字节)
 */
static W25QXX_StatusTypeDef W25Q64_WritePage(uint32_t address, uint8_t *pBuffer,
                                             uint16_t size) {
  if (W25Q64_WriteEnable() != W25QXX_OK)
    return W25QXX_ERROR;

  uint8_t cmd[4];
  cmd[0] = CMD_PAGE_PROGRAM;
  cmd[1] = (address >> 16) & 0xFF;
  cmd[2] = (address >> 8) & 0xFF;
  cmd[3] = address & 0xFF;

  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, cmd, 4, HAL_MAX_DELAY);
  HAL_SPI_Transmit(w25q64_hspi, pBuffer, size, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();

  return W25Q64_WaitBusy(50); // 页写入典型 0.4ms，50ms 超时超安全
}

/**
 * @brief  自动跨页边界的连续写入
 */
W25QXX_StatusTypeDef W25Q64_WriteBuffer(uint32_t address, uint8_t *pBuffer,
                                        uint32_t size) {
  if (pBuffer == NULL || size == 0)
    return W25QXX_PARAM_ERR;

  W25QXX_StatusTypeDef status = W25QXX_OK;
  uint16_t page_offset = address % W25QXX_PAGE_SIZE;
  uint16_t bytes_left_in_page = W25QXX_PAGE_SIZE - page_offset;

  if (size <= bytes_left_in_page) {
    return W25Q64_WritePage(address, pBuffer, size);
  } else {
    // 1. 写完首页剩余空间
    status = W25Q64_WritePage(address, pBuffer, bytes_left_in_page);
    if (status != W25QXX_OK)
      return status;

    address += bytes_left_in_page;
    pBuffer += bytes_left_in_page;
    size -= bytes_left_in_page;

    // 2. 整页循环写入
    while (size >= W25QXX_PAGE_SIZE) {
      status = W25Q64_WritePage(address, pBuffer, W25QXX_PAGE_SIZE);
      if (status != W25QXX_OK)
        return status;

      address += W25QXX_PAGE_SIZE;
      pBuffer += W25QXX_PAGE_SIZE;
      size -= W25QXX_PAGE_SIZE;
    }

    // 3. 写尾巴数据
    if (size > 0) {
      status = W25Q64_WritePage(address, pBuffer, size);
    }
  }
  return status;
}

/**
 * @brief  进入深度掉电低功耗模式
 */
void W25Q64_PowerDown(void) {
  uint8_t cmd = CMD_POWER_DOWN;
  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();
}

/**
 * @brief  从掉电模式唤醒
 */
void W25Q64_WAKEUP(void) {
  uint8_t cmd = CMD_RELEASE_POWERDOWN;
  W25QXX_CS_LOW();
  HAL_SPI_Transmit(w25q64_hspi, &cmd, 1, HAL_MAX_DELAY);
  W25QXX_CS_HIGH();
}

/* ------------------ 升级版驱动测试用例 ------------------ */
void W25Q64_Test_Run(void) {
  W25QXX_StatusTypeDef status;

  printf(
      "\r\n================ W25Q64 Enhanced Driver Test ================\r\n");

  // 1. 测试初始化与 JEDEC ID 读取
  printf("1. Initializing & Checking JEDEC ID...\r\n");
  status = W25Q64_Init(w25q64_hspi);
  uint32_t id = W25Q64_ReadID();
  printf("   JEDEC ID: 0x%06X ", (unsigned int)id);

  if (status == W25QXX_OK && id == W25Q64_JEDEC_ID) {
    printf("[OK]\r\n");
  } else {
    printf("[FAIL] Initialization Error or Wrong ID!\r\n");
    return;
  }

  // 2. 测试 4KB 扇区擦除 + FastRead（快速读取）
  uint32_t test_addr = 0x002000; // 测试地址：扇区 2
  printf("2. Erasing Sector at 0x%06X...\r\n", (unsigned int)test_addr);
  status = W25Q64_EraseSector(test_addr);

  if (status != W25QXX_OK) {
    printf("   [FAIL] Erase Sector Timeout/Error code: %d\r\n", status);
    return;
  }

  uint8_t read_check[32] = {0};
  // 使用新增的 FastRead 验证擦除结果
  W25Q64_FastRead(test_addr, read_check, sizeof(read_check));
  uint8_t erase_ok = 1;
  for (int i = 0; i < sizeof(read_check); i++) {
    if (read_check[i] != 0xFF) {
      erase_ok = 0;
      break;
    }
  }
  printf("   Erase & FastRead Check (Expected 0xFF): %s\r\n",
         erase_ok ? "[OK]" : "[FAIL]");
  if (!erase_ok)
    return;

  // 3. 测试跨页连续写入 + 数据回读
  uint8_t tx_buf[] =
      "STM32F401 SPI 21MHz Fast Read & Timeout Protection Passed!";
  uint8_t rx_buf[sizeof(tx_buf)] = {0};
  uint32_t cross_page_addr = 0x0020F0; // 跨页地址测试

  printf("3. Writing Data Across Page Boundary to 0x%06X...\r\n",
         (unsigned int)cross_page_addr);
  status = W25Q64_WriteBuffer(cross_page_addr, tx_buf, sizeof(tx_buf));
  if (status != W25QXX_OK) {
    printf("   [FAIL] WriteBuffer Error code: %d\r\n", status);
    return;
  }

  printf("4. Fast Reading Back Data...\r\n");
  W25Q64_FastRead(cross_page_addr, rx_buf, sizeof(tx_buf));

  printf("   TX: %s\r\n", tx_buf);
  printf("   RX: %s\r\n", rx_buf);

  if (memcmp(tx_buf, rx_buf, sizeof(tx_buf)) == 0) {
    printf("   Data Verification: [OK]\r\n");
  } else {
    printf("   Data Verification: [FAIL]\r\n");
    return;
  }

  // 4. 测试 64KB 大块擦除功能
  printf("5. Testing 64KB Block Erase at 0x010000...\r\n");
  status = W25Q64_EraseBlock64K(0x010000);
  if (status == W25QXX_OK) {
    printf("   64KB Block Erase: [OK]\r\n");
  } else {
    printf("   64KB Block Erase: [FAIL]\r\n");
    return;
  }

  printf("====== Result: ALL ENHANCED TESTS PASSED! ======\r\n");
}