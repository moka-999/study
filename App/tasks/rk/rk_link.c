#include "rk_link.h"
#include "task.h"
#include <string.h>
#if defined(B1_RK_UART)
#include "pwr_ctrl.h"
#endif

/* 无参数命令目录：编号显式列出，不用数组下标+1推导协议值。
 * 入队检查、发送前复核、联调名称映射共用这一张表。增项前须确认RK实现；
 * 本表不替代正式PC业务的参数、电源、RK-ready和算法生命周期检查。
 */
static const rk_command_descriptor_t s_command_table[] = {
    { 1U, "gtest-stuck-address"}, /* 粘滞地址测试。 */
    { 2U, "gtest-rand"},          /* 随机数测试。 */
    { 3U, "gtest-xor"},           /* 异或测试。 */
    { 4U, "gtest-sub"},           /* 减法测试。 */
    { 5U, "gtest-mul"},           /* 乘法测试。 */
    { 6U, "gtest-div"},           /* 除法测试。 */
    { 7U, "gtest-or"},            /* 按位或测试。 */
    { 8U, "gtest-and"},           /* 按位与测试。 */
    { 9U, "gtest-seqinc"},        /* 递增序列测试。 */
    {10U, "gtest-solidbits"},     /* 固定位模式测试。 */
    {11U, "gtest-checkerboard"},  /* 棋盘格测试。 */
    {12U, "gtest-blockseq"},      /* 块序列测试。 */
    {13U, "gtest-walkbits0"},     /* 0位行走测试。 */
    {14U, "gtest-walkbits1"},     /* 1位行走测试。 */
    {15U, "gtest-bitspread"},     /* 位扩散测试。 */
    {16U, "gtest-bitflip"},       /* 位翻转测试。 */
    {17U, "mats"},               /* MATS测试，不是保留编号。 */
    {18U, "marchx"},             /* March X测试。 */
    {19U, "marchc-"},            /* March C-测试。 */
    {20U, "marchc+"},            /* March C+测试。 */
};

const rk_command_descriptor_t *rk_command_find_by_id(uint16_t command_id)
{
    for (unsigned i = 0U; i < sizeof(s_command_table) / sizeof(s_command_table[0]); ++i) {
        if (s_command_table[i].command_id == command_id) {
            return &s_command_table[i];
        }
    }
    return NULL;
}

const rk_command_descriptor_t *rk_command_find_by_name(const char *name)
{
    if (name == NULL) return NULL;
    for (unsigned i = 0U; i < sizeof(s_command_table) / sizeof(s_command_table[0]); ++i) {
        if (strcmp(s_command_table[i].name, name) == 0) {
            return &s_command_table[i];
        }
    }
    return NULL;
}

/* 命令和心跳共用的RK发送流水号；不允许外部直接修改。 */
static uint64_t s_next_sequence;
uint64_t rk_link_next_sequence(void)
{
    uint64_t sequence;
    taskENTER_CRITICAL();
    sequence = s_next_sequence++;
    taskEXIT_CRITICAL();
    return sequence;
}
int rk_link_request(const rk_request_t *request)
{
    if (request == NULL || rkRequestQueue == NULL || rkResponseQueue == NULL ||
        rk_command_find_by_id(request->command_id) == NULL) return -1;
    uint64_t ticks = (uint64_t)request->timeout_ms * configTICK_RATE_HZ / 1000U;
    if (request->timeout_ms != 0U && (ticks == 0U || ticks > INT32_MAX)) return -1;
#if defined(B1_RK_UART)
    if (pwr_ctrl_get_state() != PWR_CTRL_STATE_ON) return -1;
#endif
    return xQueueSend(rkRequestQueue, request, 0U) == pdTRUE ? 0 : -2;
}
int rk_link_receive(rk_response_t *response)
{
    return response != NULL && rkResponseQueue != NULL &&
           xQueueReceive(rkResponseQueue, response, 0U) == pdTRUE;
}
