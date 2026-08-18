/*
 * external_flash.c
 *
 *  Created on: Aug 10, 2026
 *      Author: dinhtuan.cao
 */

#include "external_flash.h"

int8_t QSPI_Flash_Init(void)
{
  // QSPI is already initialized by CubeMX: MX_QUADSPI_Init()
  // This function verifies flash is present and valid

  uint8_t id[3];

  if (QSPI_Flash_ReadID(id) != 0)
  return -1;

  // W25Q64 JEDEC ID = 0xEF 0x40 0x17
  if (id[0] != 0xEF || id[1] != 0x40 || id[2] != 0x17)
  return -1;  /* Wrong flash chip */

  if (QSPI_Flash_EnableQuadMode() != 0)
  return -1;

  return 0;
}

int8_t QSPI_Flash_ReadStatusReg2(uint8_t *sr2)
{
    if (sr2 == NULL)
        return -1;

    QSPI_CommandTypeDef cmd = {0};

    cmd.Instruction        = CMD_READ_STATUS_REG2;  // 0x35
    cmd.InstructionMode    = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode        = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode  = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode           = QSPI_DATA_1_LINE;
    cmd.NbData             = 1;
    cmd.DummyCycles        = 0;
    cmd.DdrMode            = QSPI_DDR_MODE_DISABLE;
    cmd.DdrHoldHalfCycle   = QSPI_DDR_HHC_HALF_CLK_DELAY;
    cmd.SIOOMode           = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;

    if (HAL_QSPI_Receive(&hqspi, sr2, 100) != HAL_OK)
        return -1;

    return 0;
}
int8_t QSPI_Flash_WaitReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms) {
        int8_t busy = QSPI_Flash_IsBusy();

        if (busy < 0)
            return -1;          // Cannot read status register

        if (busy == 0)
            return 0;           // BUSY bit cleared
    }

    return -1;                  // Timeout
}

int8_t QSPI_Flash_EnableQuadMode(void)
{
    uint8_t sr2;
    QSPI_CommandTypeDef cmd = {0};

    /* 1. Read Status Register-2 */
    if (QSPI_Flash_ReadStatusReg2(&sr2) != 0)
        return -1;

    /* 2. QE already enabled: no write is needed */
    if ((sr2 & STATUS_REG2_QE) != 0)
        return 0;

    /* 3. Enable writes to the status register */
    cmd.Instruction       = CMD_WRITE_ENABLE;      // 0x06
    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode       = QSPI_ADDRESS_NONE;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode          = QSPI_DATA_NONE;
    cmd.NbData            = 0;

    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;

    /* 4. Set only QE bit; preserve all other SR2 bits */
    sr2 |= STATUS_REG2_QE;

    cmd.Instruction = CMD_WRITE_STATUS_REG2; // 0x31
    cmd.DataMode = QSPI_DATA_1_LINE;
    cmd.NbData = 1;

    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;

    if (HAL_QSPI_Transmit(&hqspi, &sr2, 100) != HAL_OK)
        return -1;

    /* 5. Wait until W25Q64 finishes writing SR2 */
    if (QSPI_Flash_WaitReady(100) != 0)
        return -1;

    /* 6. Read back SR2 and confirm QE */
    if (QSPI_Flash_ReadStatusReg2(&sr2) != 0)
        return -1;

    return ((sr2 & STATUS_REG2_QE) != 0) ? 0 : -1;
}

/**
 * Read W25Q64 device ID via QSPI indirect mode
 * Uses HAL_QSPI_Command() to send 0x9F command
 * Uses HAL_QSPI_Receive() to read 3 ID bytes
 */
int8_t QSPI_Flash_ReadID(uint8_t *id)
{
  if (!id)
  return -1;

  QSPI_CommandTypeDef cmd = {0};

  cmd.Instruction       = 0x9F;  /* READ ID command */
  cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
  cmd.AddressMode       = QSPI_ADDRESS_NONE;
  cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
  cmd.DataMode          = QSPI_DATA_1_LINE;
  cmd.NbData            = 3;  /* Read 3 ID bytes */
  cmd.DummyCycles       = 0;
  cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
  cmd.DdrHoldHalfCycle  = QSPI_DDR_HHC_HALF_CLK_DELAY;
  cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

  /* Send command (instruction phase) */
  if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
  return -1;

  /* Receive data (data phase) - NbData already set in cmd */
  memset(id, 0, 3);
  if (HAL_QSPI_Receive(&hqspi, id, 100) != HAL_OK)
  return -1;

  return 0;
}

/**
 * Check if W25Q64 is busy (bit 0 of status register)
 */
int8_t QSPI_Flash_IsBusy(void)
{
  QSPI_CommandTypeDef cmd = {0};
  uint8_t status = 0;

  cmd.Instruction       = CMD_READ_STATUS;
  cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
  cmd.AddressMode       = QSPI_ADDRESS_NONE;
  cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
  cmd.DataMode          = QSPI_DATA_1_LINE;
  cmd.NbData            = 1;  /* Read 1 status byte */
  cmd.DummyCycles       = 0;

  if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
  return -1;

  if (HAL_QSPI_Receive(&hqspi, &status, 100) != HAL_OK)
  return -1;

  return (status & STATUS_BUSY) ? 1 : 0;
}

/**
 * Erase 64KB block in W25Q64 via QSPI indirect mode
 * Uses CMD 0xD8 (Block Erase 64KB)
 */
int8_t QSPI_Flash_EraseBlock(uint32_t address)
{
  if (address >= QSPI_FLASH_MAX_SIZE)
  return -1;
  if (address % QSPI_FLASH_BLOCK_SIZE != 0)
  return -1;  /* Must be 64KB aligned (0xD8 erases a 64KB block) */

  /* Send WRITE_ENABLE (0x06) first */
  QSPI_CommandTypeDef cmd = {0};
  cmd.Instruction       = CMD_WRITE_ENABLE;
  cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
  cmd.AddressMode       = QSPI_ADDRESS_NONE;
  cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
  cmd.DataMode          = QSPI_DATA_NONE;
  cmd.NbData            = 0;
  cmd.DummyCycles       = 0;

  if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
  return -1;

  /* Send ERASE command (0xD8) with 24-bit address */
  cmd.Instruction = CMD_BLOCK_ERASE_64K;   /* Block erase 64KB */
  cmd.AddressMode = QSPI_ADDRESS_1_LINE;
  cmd.AddressSize = QSPI_ADDRESS_24_BITS;
  cmd.Address     = address;
  cmd.NbData      = 0;

  if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
  return -1;

  /* Wait for erase to complete */
  uint32_t timeout = HAL_GetTick() + 5000;  /* 5 seconds */
  while (QSPI_Flash_IsBusy() && HAL_GetTick() < timeout);

  return 0;
}

/**
 * Write up to 256 bytes (one page) to W25Q64 via QSPI indirect mode
 * Uses CMD 0x32 (Quad Input Page Program)
 */
int8_t QSPI_Flash_WritePage(uint32_t address, const uint8_t *data, uint32_t length)
{
    if (address >= QSPI_FLASH_MAX_SIZE || length > QSPI_FLASH_PAGE_SIZE)
    return -1;

    if ((address % QSPI_FLASH_PAGE_SIZE) + length > QSPI_FLASH_PAGE_SIZE)
    return -1;

    if (!data || length == 0)
        return -1;

    /* Send WRITE_ENABLE first */
    QSPI_CommandTypeDef cmd = {0};
    cmd.Instruction     = CMD_WRITE_ENABLE;
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode     = QSPI_ADDRESS_NONE;
    cmd.DataMode        = QSPI_DATA_NONE;
    cmd.NbData          = 0;

    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;

#if (WRITE_DATA_OPTIONS == WRITE_DATA_QUAD_LINE)
    /* Send Quad Input Page Program command with address and data */
    cmd.Instruction = CMD_QUAD_INPUT_PAGE_PROG;   /* Quad Input Page Program */
    cmd.AddressMode = QSPI_ADDRESS_1_LINE;
    cmd.AddressSize = QSPI_ADDRESS_24_BITS;
    cmd.Address     = address;
    cmd.DataMode    = QSPI_DATA_4_LINES;
    cmd.NbData      = length;
#else
    /* Send PAGE_PROGRAM command with address and data */
    cmd.Instruction = CMD_PAGE_PROG;    /* Page Program (standard, not quad) */
    cmd.AddressMode = QSPI_ADDRESS_1_LINE;
    cmd.AddressSize = QSPI_ADDRESS_24_BITS;
    cmd.Address     = address;
    cmd.DataMode    = QSPI_DATA_1_LINE;
    cmd.NbData      = length;
#endif
    if (HAL_QSPI_Command(&hqspi, &cmd, 100) != HAL_OK)
        return -1;

    if (HAL_QSPI_Transmit(&hqspi, (uint8_t *)data, 1000) != HAL_OK)
        return -1;

    /* Wait for write to complete */
    uint32_t timeout = HAL_GetTick() + 1000;
    while (QSPI_Flash_IsBusy() && HAL_GetTick() < timeout);

    return 0;
}

/**
 * Enable QSPI Memory-Mapped Mode (XIP - Execute In Place)
 * After calling this, W25Q64 memory is mapped to 0x90000000
 * CPU can execute code directly from this address without explicit read commands
 *
 * Returns: 0 = OK, -1 = Error
 */
int8_t QSPI_Flash_EnterMemoryMappedMode(void)
{
    QSPI_CommandTypeDef cmd = {0};
    QSPI_MemoryMappedTypeDef mem_mapped = {0};

    /* Configure read command for quad-output fast read (0xEB) */
    cmd.Instruction       = CMD_QUAD_OUTPUT_READ;        /* Fast quad read command */
    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.AddressMode       = QSPI_ADDRESS_4_LINES;        /* 24-bit address on 4 lines */
    cmd.AddressSize       = QSPI_ADDRESS_24_BITS;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    cmd.DataMode          = QSPI_DATA_4_LINES;           /* Read data on 4 lines (quad) */
    cmd.DummyCycles       = 6;                           /* W25Q64 requires 6 dummy cycles for 0xEB */
    cmd.DdrMode           = QSPI_DDR_MODE_DISABLE;
    cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

    /* Memory-mapped mode settings */
    mem_mapped.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;
    mem_mapped.TimeOutPeriod = 0;

    /* Enter memory-mapped mode */
    if (HAL_QSPI_MemoryMapped(&hqspi, &cmd, &mem_mapped) != HAL_OK)
        return -1;

    return 0;
}

/**
 * Exit QSPI Memory-Mapped Mode and return to indirect mode.
 *
 * After HAL_QSPI_MemoryMapped() the peripheral is locked: erase/program
 * commands will fail until memory-mapped mode is aborted.  This function
 * aborts the current operation and returns the QSPI to indirect mode so
 * that QSPI_Flash_EraseBlock() / QSPI_Flash_WritePage() work again.
 *
 * Returns: 0 = OK, -1 = Error
 */
int8_t QSPI_Flash_ExitMemoryMappedMode(void)
{
    /* HAL_QSPI_Abort() stops any ongoing transfer and returns the
     * peripheral to indirect mode, clearing the memory-mapped lock. */
    if (HAL_QSPI_Abort(&hqspi) != HAL_OK)
        return -1;

    return 0;
}


