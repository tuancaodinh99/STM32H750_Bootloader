/*
 * external_flash.h
 *
 *  Created on: Aug 10, 2026
 *      Author: dinhtuan.cao
 */

#ifndef INC_EXTERNAL_FLASH_H_
#define INC_EXTERNAL_FLASH_H_

#include "quadspi.h"
#include <string.h>

/* --- W25Q64 geometry (per Winbond datasheet) --- */
#define QSPI_FLASH_MAX_SIZE     (8 * 1024 * 1024) /* W25Q64: 8MB = 64Mbit        */
#define QSPI_FLASH_BLOCK_SIZE   (64 * 1024)       /* W25Q64: 64KB, erase 0xD8    */
#define QSPI_FLASH_SECTOR_SIZE  ( 4 * 1024)       /* W25Q64: 4KB,  erase 0x20    */
#define QSPI_FLASH_PAGE_SIZE     256              /* W25Q64: 256 B, program unit */

/* QSPI Memory Mapping */
#define QSPI_BASE_ADDR          0x90000000          /* QSPI mapped address */
#define APP_QSPI_ADDR           0x90000000          /* App starts at QSPI base */
#define APP_QSPI_MAX_SIZE       (8 * 1024 * 1024)   /* Full 8 MB used for app */

/* W25Q64 QSPI Commands (for Quad mode) */
#define CMD_READ_ID              0x9F
#define CMD_READ_STATUS_REG2     0x35
#define CMD_WRITE_STATUS_REG2    0x31
#define CMD_ENABLE_QUAD_MODE     0x38
#define CMD_PAGE_PROG            0x02
#define CMD_QUAD_INPUT_PAGE_PROG 0x32
#define CMD_QUAD_OUTPUT_READ     0xEB   /* Fast quad read */
#define CMD_BLOCK_ERASE_64K      0xD8
#define CMD_READ_STATUS          0x05
#define CMD_WRITE_ENABLE         0x06

#define STATUS_REG2_QE           0x02

#define STATUS_BUSY              0x01

/* Writing options for W25Q64 QSPI */
#define WRITE_DATA_1_LINE        0
#define WRITE_DATA_QUAD_LINE     1
#define WRITE_DATA_OPTIONS       (WRITE_DATA_QUAD_LINE)

int8_t QSPI_Flash_Init(void);
int8_t QSPI_Flash_ReadID(uint8_t *id);
int8_t QSPI_Flash_EraseBlock(uint32_t address);
int8_t QSPI_Flash_IsBusy(void);
int8_t QSPI_Flash_WritePage(uint32_t address, const uint8_t *data, uint32_t length);
int8_t QSPI_Flash_EnterMemoryMappedMode(void);
int8_t QSPI_Flash_ExitMemoryMappedMode(void);


int8_t QSPI_Flash_ReadStatusReg2(uint8_t *sr2);
int8_t QSPI_Flash_WaitReady(uint32_t timeout_ms);
int8_t QSPI_Flash_EnableQuadMode(void);

#endif /* INC_EXTERNAL_FLASH_H_ */
