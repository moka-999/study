/*
 * ===================================================================
 * Modbus CRC-16 — MCU-RK TLV 协议帧校验
 * ===================================================================
 *
 * 仅用于MCU-RK的0x5A/0xA5帧；PC-MCU固定头+JSON协议不使用本CRC。
 * 算法: Modbus CRC-16 (多项式 0x8005, 初始值 0xFFFF)
 * 查表法: 256 项预计算表, O(n) 时间
 *
 * 测试向量: "123456789" → 0x4B37
 * ===================================================================
 */
#ifndef __TLV_CRC_H
#define __TLV_CRC_H

#include <stdint.h>

/**
 * @brief 查表计算初值0xFFFF的Modbus CRC-16。
 * @param[in] data 至少可读len字节；仅len为0时可为NULL。
 * @param[in] len 数据字节数。
 * @return CRC值；len为0时返回0xFFFF。
 * @note 不校验数据指针；返回值是校验值，不是错误码。
 */
uint16_t crc16_modbus(const uint8_t *data, uint16_t len);

/**
 * @brief 在上一段CRC上续算，可跨不连续缓冲区。
 * @param[in] crc 上一段返回的CRC，首段可用0xFFFF。
 * @param[in] data 至少可读len字节；仅len为0时可为NULL。
 * @param[in] len 数据字节数。
 * @return 续算CRC；len为0时原样返回crc。
 * @note 不校验数据指针；返回值是校验值，不是错误码。
 */
uint16_t crc16_modbus_continue(uint16_t crc, const uint8_t *data, uint16_t len);

/**
 * @brief 单次计算Modbus CRC-16，等价于crc16_modbus。
 * @param[in] data 至少可读len字节；仅len为0时可为NULL。
 * @param[in] len 数据字节数。
 * @return CRC值；len为0时返回0xFFFF，无共享上下文。
 * @note 不校验数据指针；返回值是校验值，不是错误码。
 */
uint16_t crc16_modbus_once(const uint8_t *data, uint16_t len);

/**
 * @brief 逐位计算Modbus CRC-16，供交叉校验。
 * @param[in] data 至少可读len字节；仅len为0时可为NULL。
 * @param[in] len 数据字节数。
 * @return CRC值；len为0时返回0xFFFF。
 * @note 不校验数据指针；返回值是校验值，不是错误码。
 */
uint16_t crc16_modbus_bitwise(const uint8_t *data, uint16_t len);

#endif
