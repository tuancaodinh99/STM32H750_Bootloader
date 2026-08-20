/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "quadspi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bootloader.h"
#include "external_flash.h"
#include "crc32.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#if (CRC_TEST == 1)
static const uint8_t test[] = "123456789";
#endif
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_QUADSPI_Init();
  /* USER CODE BEGIN 2 */

  // Small delay to ensure all peripherals are ready
  HAL_Delay(100);

  // ===== BOOTLOADER STARTUP BANNER =====
  UART_SendString(&huart1, "\r\n");
  UART_SendLine(&huart1, "==========================================");
  UART_SendLine(&huart1, "STM32H750 BOOTLOADER v1.0");
  UART_SendLine(&huart1, "128KB Internal Flash (HSI 480MHz)");
  UART_SendLine(&huart1, "QSPI XIP Mode (W25Q64 @ 0x90000000)");
  UART_SendLine(&huart1, "==========================================");
#if (CRC_TEST == 1)
  char msg[32];
  uint32_t crc;
  crc = CRC32_Calculate(test, 9);
  snprintf(msg, sizeof(msg), "CRC = 0x%08lX", (unsigned long)crc);
  UART_SendLine(&huart1, msg);
  UART_SendLine(&huart1, "Expected: 0xCBF43926");
  /* Expected: 0xCBF43926 */
#endif
  UART_SendString(&huart1, "\r\n");

  // ===== Initialize QSPI and verify W25Q64 =====
  UART_SendLine(&huart1, "[*] Initializing QSPI...");
  HAL_Delay(200);  /* Wait for QSPI to stabilize */

  /* Debug: Multiple retry attempts to read ID */
  uint8_t qspi_id[3] = {0, 0, 0};
  int8_t read_result = -1;
  int attempt_count = 0;

  for (attempt_count = 0; attempt_count < 3; attempt_count++) 
  {
      memset(qspi_id, 0, 3);
      read_result = QSPI_Flash_ReadID(qspi_id);

      char debug_msg[80];

      snprintf(debug_msg, sizeof(debug_msg),
               "[DEBUG] Attempt %d: result=%d, ID: %02X %02X %02X",
               attempt_count + 1, read_result, qspi_id[0], qspi_id[1], qspi_id[2]);
    
      UART_SendLine(&huart1, debug_msg);

      /* If we got valid ID, break */
      if (read_result == 0 && qspi_id[0] == 0xEF) {
          break;
      }

      HAL_Delay(100);  /* Wait before retry */
  }

  if (QSPI_Flash_Init() != 0) 
  {
      UART_SendLine(&huart1, "");
      UART_SendLine(&huart1, "[!] ERROR: QSPI init failed or W25Q64 not found");
      return -1;
  }
  UART_SendLine(&huart1, "[OK] W25Q64 detected (0xEF 0x40 0x17)");
  UART_SendString(&huart1, "\r\n");

  /*
   * NOTE: Memory-mapped mode is NOT enabled here.
   *
   * The QSPI peripheral can only be in one mode at a time:
   *   - Indirect mode  → erase / program (used by firmware update)
   *   - Memory-mapped  → XIP read (used to boot the application)
   *
   * If we enter memory-mapped mode now, the erase/program commands
   * in Bootloader_UpdateFirmware() will silently fail because the
   * peripheral is locked.  We therefore defer memory-mapped mode to
   * the normal-boot branch below, and let the update branch enter it
   * only when it needs to read back data for CRC verification.
   */

  uint32_t boot_timeout = HAL_GetTick() + 3000;

  uint8_t update_requested = 0;
  uint8_t update_cmd = 0;  /* Single-byte command buffer */

  while (HAL_GetTick() < boot_timeout) 
  {
      /* Check for UPDATE command on UART (1byte, non-blocking) */
      if (HAL_UART_Receive(&huart1, &update_cmd, 1, 100) == HAL_OK) 
      {
          if (update_cmd == CMD_UPDATE_START)
          {
              update_requested = 1;
              break;
          }
      }
      HAL_Delay(100);
  }

  if (update_requested)
  {
    /* Handle firmware update */
    UART_SendString(&huart1,"\r\n");
    if (Bootloader_UpdateFirmware() == 0)
    {
      UART_SendLine(&huart1, "Update successful, rebooting...");
      UART_SendString(&huart1, "\r\n");
      HAL_Delay(100);
      NVIC_SystemReset();  /* Reboot → bootloader loads new app */
    }
    else
    {
      UART_SendLine(&huart1, "Update failed, returning to bootloader");
    }
    UART_SendString(&huart1, "\r\n");
  }
  else
  {
    /* Boot to application — enter memory-mapped mode now for XIP */
    UART_SendLine(&huart1, "Timeout → booting to application @ 0x90000000");
    UART_SendString(&huart1, "\r\n");

    UART_SendLine(&huart1, "[*] Enabling memory-mapped mode (XIP)...");
    if (QSPI_Flash_EnterMemoryMappedMode() != 0)
    {
      UART_SendLine(&huart1, "[!] ERROR: Failed to enable memory-mapped mode");
      return -1;
    }
    UART_SendLine(&huart1, "[OK] Memory-mapped mode enabled @ 0x90000000");

#if (RSA_SECURE_BOOT_ENABLE == 1)
    /* --- RSA SIGNATURE VERIFICATION (before jump) --- */
    if (Bootloader_VerifyRSASignature() != 0)
    {
        UART_SendLine(&huart1, "[!] ERROR: RSA signature invalid!");
        UART_SendLine(&huart1, "[!] Refusing to boot — staying in bootloader");
        /* Send error code to host (if connected) */
        Bootloader_SendErrorCmd("RSA_FAIL", ERROR_RSA);
        /* Do NOT jump. Fall through to infinite loop (bootloader safe mode). */
    }
    else
    {
        UART_SendLine(&huart1, "[*] About to call Bootloader_JumpToApplication()...");
        HAL_Delay(100);
        Bootloader_JumpToApplication();
    }
#else
    UART_SendLine(&huart1, "[*] About to call Bootloader_JumpToApplication()...");
    HAL_Delay(100);

    Bootloader_JumpToApplication();
#endif

    /* If we reach here, jump failed */
    UART_SendLine(&huart1, "[!] ERROR: Jump to application failed!");
    UART_SendLine(&huart1, "[!] Falling back to bootloader...");
    /* Never returns */
  }

  /* Fallback: infinite loop if update fails */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_Delay(1000);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 60;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 3;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// UART interrupt callback (for future use in Stage 3)
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        // TODO: Stage 3 - Handle firmware upload commands
    }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

