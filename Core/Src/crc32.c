/*
 * crc32.c
 *
 *  Created on: Aug 13, 2026
 *      Author: dinhtuan.cao
 */

#include "crc32.h"

#define CRC32_POLYNOMIAL  0xEDB88320U

uint32_t CRC32_Init(void)
{
    return 0xFFFFFFFFU;
}

uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    if (data == NULL)
        return crc;

    for (uint32_t i = 0; i < length; i++)
    {
        crc ^= data[i];

        for(uint32_t bit = 0; bit < 8U; bit++)
        {
            if ((crc & 1U) != 0U)
            crc = (crc >> 1U) ^ CRC32_POLYNOMIAL;
            else
            crc >>= 1U;
        }
    }

    return crc;
}

uint32_t CRC32_Finalize(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

uint32_t CRC32_Calculate(const uint8_t *data, uint32_t length)
{
    uint32_t crc = CRC32_Init();
    crc = CRC32_Update(crc, data, length);
    return CRC32_Finalize(crc);
}