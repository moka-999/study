/*
 * ===================================================================
 * SPI 接口 — STM32 HAL 实现
 * ===================================================================
 * SPI2: PB13=SCK, PB14=MISO, PB15=MOSI, CS 由上层 GPIO 控制
 * ===================================================================
 */
#include "hal_if.h"
#include "spi.h"

static int _xfer(spi_if_t *b, uint8_t *tx, uint8_t *rx, uint16_t l)
{ return HAL_SPI_TransmitReceive(b->handle,tx,rx,l,100)==HAL_OK?IF_OK:IF_ERR; }

static int _send(spi_if_t *b, uint8_t *d, uint16_t l)
{ return HAL_SPI_Transmit(b->handle,d,l,100)==HAL_OK?IF_OK:IF_ERR; }

/* ── 工厂实例 ── */
spi_if_t spi2_if = { .handle=&hspi2, .transfer=_xfer, .send=_send };
