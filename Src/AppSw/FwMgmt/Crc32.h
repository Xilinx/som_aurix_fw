/**
 * @file    Crc32.h
 * @brief   Software CRC-32 (Ethernet polynomial 0xEDB88320)
 *
 * Shared by FwUpdate (per-chunk and full-image verify) and
 * POST/BIST (PFlash integrity check).  Produces results
 * compatible with Python's binascii.crc32().
 */

#ifndef CRC32_H
#define CRC32_H

#include "Ifx_Types.h"

/**
 * @brief  Initialise a CRC-32 accumulator.
 * @return Initial CRC state (0xFFFFFFFF).
 */
static inline uint32 Crc32_Init(void) { return 0xFFFFFFFFu; }

/**
 * @brief  Feed data into a running CRC-32.
 *
 * @param  crc    Current CRC state (from Crc32_Init or previous Update).
 * @param  pData  Pointer to data bytes.
 * @param  len    Number of bytes.
 * @return Updated CRC state.
 */
uint32 Crc32_Update(uint32 crc, const uint8 *pData, uint32 len);

/**
 * @brief  Finalise the CRC-32 (XOR with 0xFFFFFFFF).
 *
 * @param  crc  CRC state from the last Update call.
 * @return Final CRC-32 value.
 */
static inline uint32 Crc32_Final(uint32 crc) { return crc ^ 0xFFFFFFFFu; }

/**
 * @brief  Compute CRC-32 of a contiguous buffer in one call.
 *
 * @param  pData  Pointer to data.
 * @param  len    Length in bytes.
 * @return CRC-32 value.
 */
uint32 Crc32_Calc(const uint8 *pData, uint32 len);

#endif /* CRC32_H */