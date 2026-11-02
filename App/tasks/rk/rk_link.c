#include "rk_link.h"
#include "task.h"
#include <string.h>
#if defined(B1_RK_UART)
#include "pwr_ctrl.h"
#endif

/* 编号命令目录：编号显式列出，不用数组下标+1推导协议值。
 * 入队检查、发送前复核、联调名称映射共用这一张表。增项前须确认RK实现；
 * 本表不替代正式PC业务的参数、电源、RK-ready和算法生命周期检查。
 */
static const rk_command_descriptor_t s_command_table[] = {
    { 1U, "gtest-stuck-address", RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 粘滞地址测试。 */
    { 2U, "gtest-rand",          RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 随机数测试。 */
    { 3U, "gtest-xor",           RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 异或测试。 */
    { 4U, "gtest-sub",           RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 减法测试。 */
    { 5U, "gtest-mul",           RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 乘法测试。 */
    { 6U, "gtest-div",           RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 除法测试。 */
    { 7U, "gtest-or",            RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 按位或测试。 */
    { 8U, "gtest-and",           RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 按位与测试。 */
    { 9U, "gtest-seqinc",        RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 递增序列测试。 */
    {10U, "gtest-solidbits",     RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 固定位模式测试。 */
    {11U, "gtest-checkerboard",  RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 棋盘格测试。 */
    {12U, "gtest-blockseq",      RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 块序列测试。 */
    {13U, "gtest-walkbits0",     RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 0位行走测试。 */
    {14U, "gtest-walkbits1",     RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 1位行走测试。 */
    {15U, "gtest-bitspread",     RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 位扩散测试。 */
    {16U, "gtest-bitflip",       RK_COMMAND_CATEGORY_FUNCTIONAL}, /* 位翻转测试。 */
    {17U, "mats",               RK_COMMAND_CATEGORY_FUNCTIONAL}, /* MATS测试，不是保留编号。 */
    {18U, "marchx",             RK_COMMAND_CATEGORY_FUNCTIONAL}, /* March X测试。 */
    {19U, "marchc-",            RK_COMMAND_CATEGORY_FUNCTIONAL}, /* March C-测试。 */
    {20U, "marchc+",            RK_COMMAND_CATEGORY_FUNCTIONAL}, /* March C+测试。 */
    /* IDD名称严格保留RK文档括号中的大小写；本表不宣称标准测量窗口已实现。 */
    {21U, "idd_0",              RK_COMMAND_CATEGORY_PARAMETRIC},
    {22U, "idd_2P",             RK_COMMAND_CATEGORY_PARAMETRIC},
    {23U, "idd_2PS",            RK_COMMAND_CATEGORY_PARAMETRIC},
    {24U, "idd_2N",             RK_COMMAND_CATEGORY_PARAMETRIC},
    {25U, "idd_2NS",            RK_COMMAND_CATEGORY_PARAMETRIC},
    {26U, "idd_3P",             RK_COMMAND_CATEGORY_PARAMETRIC},
    {27U, "idd_3PS",            RK_COMMAND_CATEGORY_PARAMETRIC},
    {28U, "idd_3N",             RK_COMMAND_CATEGORY_PARAMETRIC},
    {29U, "idd_3NS",            RK_COMMAND_CATEGORY_PARAMETRIC},
    {30U, "idd_4R",             RK_COMMAND_CATEGORY_PARAMETRIC},
    {31U, "idd_4W",             RK_COMMAND_CATEGORY_PARAMETRIC},
    {32U, "idd_5",              RK_COMMAND_CATEGORY_PARAMETRIC},
    {33U, "idd_5AB",            RK_COMMAND_CATEGORY_PARAMETRIC},
    {34U, "idd_5PB",            RK_COMMAND_CATEGORY_PARAMETRIC},
    {35U, "idd_6",              RK_COMMAND_CATEGORY_PARAMETRIC},
    {36U, "idd_6ET",            RK_COMMAND_CATEGORY_PARAMETRIC},
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

/* TCP任务布防、RK任务更新；64位Seq及令牌须在同一临界区读写。
 * 上电前布防后，首个成功下发的心跳确定下界，拒绝旧启动/未来Seq；
 * RK定时发送可滞后、多帧同Seq，只要落在本轮窗口即有效。
 */
static struct {
    uint32_t token;
    uint64_t arm_sequence, first_tx, last_tx;
    uint8_t tx_seen, ready;
} s_ready;

int rk_link_ready_begin(uint32_t token)
{
    if (token == 0U) return -1;
    taskENTER_CRITICAL();
    memset(&s_ready, 0, sizeof(s_ready));
    s_ready.token = token;
    s_ready.arm_sequence = s_next_sequence;
    taskEXIT_CRITICAL();
    return 0;
}

int rk_link_ready_poll(uint32_t token)
{
    taskENTER_CRITICAL();
    int result = token != 0U && token == s_ready.token ? s_ready.ready : -1;
    taskEXIT_CRITICAL();
    return result;
}

void rk_link_ready_note_tx(uint64_t sequence)
{
    taskENTER_CRITICAL();
    /* 排除布防前已经分配Seq、但因UART锁/任务切换而较晚返回的旧发送。 */
    if (s_ready.token != 0U && !s_ready.ready &&
        (uint64_t)(sequence - s_ready.arm_sequence) <= INT64_MAX) {
        if (!s_ready.tx_seen) s_ready.first_tx = sequence;
        s_ready.last_tx = sequence;
        s_ready.tx_seen = 1U;
    }
    taskEXIT_CRITICAL();
}

void rk_link_ready_note_rx(const tlv_frame_t *frame)
{
    if (frame == NULL || !(frame->count == 0U ||
        (frame->count == 1U && frame->units[0].type == TLV_TYPE_HEARTBEAT &&
         frame->units[0].data_len == 0U))) return;
    taskENTER_CRITICAL();
    if (s_ready.token != 0U && s_ready.tx_seen &&
        (uint64_t)(frame->sequence - s_ready.first_tx) <=
        (uint64_t)(s_ready.last_tx - s_ready.first_tx)) s_ready.ready = 1U;
    taskEXIT_CRITICAL();
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
