/*
 * B1 SPI4 硬件适配
 *
 * AD7124 使用 SPI4：PE11=NCS、PE12=SCK、PE13=MISO、PE14=MOSI。
 * 片选由 AD7124 驱动按事务拉低/释放，本文件只负责阻塞式字节收发。
 * SPI2 属于 B1-B2 保活从机，职责不同，禁止在这里复用或导出为 ADC 总线。
 */
#include "board_interfaces.h"
#include "main.h"

extern SPI_HandleTypeDef hspi4;

#define SPI4_TRANSFER_TIMEOUT_MS 100U

static int spi4_transfer(spi_if_t *bus, uint8_t *tx_data,
                         uint8_t *rx_data, uint16_t length)
{
    if (!bus || !bus->handle || !tx_data || !rx_data || length == 0U)
        return IF_ERR;

    return HAL_SPI_TransmitReceive((SPI_HandleTypeDef *)bus->handle,
                                   tx_data, rx_data, length,
                                   SPI4_TRANSFER_TIMEOUT_MS) == HAL_OK ?
           IF_OK : IF_ERR;
}

static int spi4_send(spi_if_t *bus, uint8_t *data, uint16_t length)
{
    if (!bus || !bus->handle || !data || length == 0U)
        return IF_ERR;

    return HAL_SPI_Transmit((SPI_HandleTypeDef *)bus->handle,
                            data, length, SPI4_TRANSFER_TIMEOUT_MS) == HAL_OK ?
           IF_OK : IF_ERR;
}

/* B1 AD7124 的唯一正式 SPI 总线实例。 */
spi_if_t spi4_if = {
    .handle = &hspi4,
    .transfer = spi4_transfer,
    .send = spi4_send,
};
