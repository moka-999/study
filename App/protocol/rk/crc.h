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

/* 查表 CRC16 (初始调用, init=0xFFFF) */
uint16_t crc16_modbus(const uint8_t *data, uint16_t len);

/* 流式续算: 在已有 crc 值上继续计算 (用于非连续数据段) */
uint16_t crc16_modbus_continue(uint16_t crc, const uint8_t *data, uint16_t len);

/* 单次调用: 对完整数据段计算 CRC16 并重置上下文 */
uint16_t crc16_modbus_once(const uint8_t *data, uint16_t len);

/* 逐位法 (备选/自测用, 不依赖查表) */
uint16_t crc16_modbus_bitwise(const uint8_t *data, uint16_t len);

#endif
