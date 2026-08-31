/*
 * Apollo V2 SPI2同步适配：AD7124开发映射，Mode 3由CubeMX配置。
 * 本层不切片选、不解析器件寄存器；片选由上层围绕完整事务控制。
 * B1的AD7124使用SPI4，其SPI2为B2保活从机，不能直接沿用本实例。
 */
#include "board_interfaces.h"
#include "spi.h"

#include <stddef.h>

#define SPI_TRANSFER_TIMEOUT_MS 100U

static int spi_transfer(spi_if_t *bus, uint8_t *tx_buffer,
                        uint8_t *rx_buffer, uint16_t length)
{
    if (bus == NULL || bus->handle == NULL || tx_buffer == NULL ||
        rx_buffer == NULL || length == 0U) {
        return IF_ERR;
    }
    return HAL_SPI_TransmitReceive(bus->handle, tx_buffer, rx_buffer, length,
                                   SPI_TRANSFER_TIMEOUT_MS) == HAL_OK
               ? IF_OK : IF_ERR;
}

static int spi_send(spi_if_t *bus, uint8_t *buffer, uint16_t length)
{
    if (bus == NULL || bus->handle == NULL || buffer == NULL || length == 0U) {
        return IF_ERR;
    }
    return HAL_SPI_Transmit(bus->handle, buffer, length,
                           SPI_TRANSFER_TIMEOUT_MS) == HAL_OK ? IF_OK : IF_ERR;
}

spi_if_t spi2_if = {
    .handle = &hspi2,
    .transfer = spi_transfer,
    .send = spi_send,
};
