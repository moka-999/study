/* MCP4725 Fast Write及RK3588 Boot模式电压映射。 */
#include "mcp4725.h"

/* 顺序与boot_mode_t严格一致，数值来自B1原理图P24 Table 1。 */
static const uint16_t s_boot_target_mv[BOOT_MODE_COUNT] = {
    0U, 300U, 600U, 900U, 1200U, 1500U, 1800U,
};

int mcp4725_write_fast(mcp4725_t *device, uint16_t code,
                       mcp4725_power_mode_t power_mode)
{
    uint8_t payload[2];

    if (!device || !device->bus || !device->bus->write_bytes ||
        code > MCP4725_DAC_MAX_CODE || power_mode > MCP4725_POWER_DOWN_500K)
        return IF_ERR;

    /* Fast Write第1字节：00 + PD1:PD0 + D11:D8；第2字节：D7:D0。 */
    payload[0] = (uint8_t)(((uint8_t)power_mode << 4) | (uint8_t)(code >> 8));
    payload[1] = (uint8_t)code;
    return device->bus->write_bytes(device->bus, device->address,
                                    payload, sizeof(payload));
}

int mcp4725_mv_to_code(uint16_t output_mv, uint16_t vdd_mv, uint16_t *code)
{
    uint32_t scaled;

    if (!code || vdd_mv == 0U || output_mv > vdd_mv) return IF_ERR;
    /* VOUT=code/4095*VDD，加vdd/2后做整数四舍五入。 */
    scaled = (uint32_t)output_mv * MCP4725_DAC_MAX_CODE + vdd_mv / 2U;
    *code = (uint16_t)(scaled / vdd_mv);
    return IF_OK;
}

int mcp4725_boot_mode_target_mv(boot_mode_t mode, uint16_t *target_mv)
{
    if (!target_mv || mode >= BOOT_MODE_COUNT) return IF_ERR;
    *target_mv = s_boot_target_mv[mode];
    return IF_OK;
}

int mcp4725_set_boot_mode(mcp4725_t *device, boot_mode_t mode)
{
    uint16_t target_mv;
    uint16_t code;

    if (!device || mcp4725_boot_mode_target_mv(mode, &target_mv) != IF_OK ||
        mcp4725_mv_to_code(target_mv, device->vdd_mv, &code) != IF_OK)
        return IF_ERR;
    return mcp4725_write_fast(device, code, MCP4725_POWER_NORMAL);
}
