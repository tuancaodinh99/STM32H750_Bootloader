/*
 * crc32.h
 *
 *  Created on: Aug 13, 2026
 *      Author: dinhtuan.cao
 */

#ifndef INC_CRC32_H_
#define INC_CRC32_H_

#include <stdint.h>
#include <stddef.h>

#define CRC_TEST (0) /* 1 -> Enable ; 0 -> Disable  */

uint32_t CRC32_Init(void);
uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t length);
uint32_t CRC32_Finalize(uint32_t crc);
uint32_t CRC32_Calculate(const uint8_t *data, uint32_t length);

#endif /* INC_CRC32_H_ */
