#include "b2_service.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "pc_test_service.h"
#include "pc_power_service.h"
#include "ota_app.h"
#include "b2_board.h"
#include "network_config.h"
#include <string.h>

/* 以下配置仅由TCP任务读写。B2任务通过队列传值，不跨任务读取PC静态状态。 */
static QueueHandle_t s_requests;
static QueueHandle_t s_replies;
static uint8_t s_tester_id;
static uint8_t s_site_id;
static uint8_t s_thermal_mask;
static uint8_t s_thermal_modes[B2_MIRROR_COUNT];

int b2_service_prepare(void)
{
    s_requests = xQueueCreate(1U, sizeof(b2_request_t));
    s_replies = xQueueCreate(1U, sizeof(b2_service_reply_t));
    return s_requests && s_replies;
}

void b2_service_exchange(const b2_request_t *request, b2_service_reply_t *reply)
{
    // 先初始化应答为失败，避免业务处理函数未执行或异常返回时未设置应答
    reply->status = B2_STATUS_FAILED;
    reply->tester_id = 0U;
    reply->site_id = 0U;

    if (!s_requests || !s_replies) return;
    /* 每次只允许一个B2事务等待业务完成，下一帧仍由硬件中断有界收存。 */
    if (xQueueSend(s_requests, request, portMAX_DELAY) != pdPASS) return;
    (void)xQueueReceive(s_replies, reply, portMAX_DELAY);
}

static int network_change_busy(int connected)
{
    if (connected || pc_test_service_busy() || pc_power_service_busy()) return 1;
#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
    ota_download_progress_t progress;
    ota_download_get_progress(&progress);
    if (progress.state == OTA_DOWNLOAD_RECEIVING || progress.state == OTA_DOWNLOAD_READY ||
        progress.state == OTA_DOWNLOAD_COMMIT_UNKNOWN) return 1;
#endif
    return 0;
}

static b2_status_t apply_request(const b2_request_t *request, int connected)
{

    if (request->status != B2_STATUS_OK) return request->status;
    //获取当前板卡的槽位号和测试器ID，如果请求中没有提供测试器ID，则使用当前生效的ID
    uint8_t site = b2_board_site_id();
    uint8_t tester = request->has_tester_id ? request->tester_id : s_tester_id;
    //判断是否需要更改IP配置，如果需要更改，则检查是否有PC连接或正在进行的业务，如果有则返回忙状态
    if (site < 1U || site > 8U) return B2_STATUS_FAILED;
    int change_ip = tester != 0U && (tester != s_tester_id || site != s_site_id);
    if (change_ip && network_change_busy(connected)) return B2_STATUS_BUSY;

    /* 改IP只在无PC连接、无在途业务时允许，此时整轮已结束，不会同时写结果灯。
     * 同IP下先原子更新本轮显示；失败则不提交模式缓存，不出现半份状态成功。
     */
     //如果请求中包含热模式配置，则调用pc_test_round_update_modes函数更新热模式
    if (!pc_test_round_update_modes(request->thermal_mask, request->thermal_mode))
        return B2_STATUS_FAILED;
    //如果需要更改IP配置，则调用network_config_set_tester函数设置新的测试器ID和槽位号
    if (change_ip && !network_config_set_tester(tester, site)) return B2_STATUS_FAILED;
    //如果更改IP配置成功，则更新当前生效的测试器ID和槽位号
    if (change_ip) { s_tester_id = tester; s_site_id = site; }
    //如果请求中包含加热模式配置，则更新模式缓存
    for (size_t i = 0U; i < B2_MIRROR_COUNT; ++i) {
        if ((request->thermal_mask & (1U << i)) != 0U)
            s_thermal_modes[i] = request->thermal_mode[i];
    }
    s_thermal_mask |= request->thermal_mask;
    //返回成功状态
    return B2_STATUS_OK;
}
//connect：0表示无PC连接，1表示已接受PC连接
void b2_service_poll(int connected)
{

    b2_request_t request;
    // 仅在有请求时才调用apply_request，避免无请求时误改网络配置
    if (!s_requests ||
        xQueueReceive(s_requests, &request, 0U) != pdPASS)
        return;

    b2_service_reply_t reply = {0};

    reply.status = apply_request(&request, connected);
    reply.tester_id = s_tester_id;
    reply.site_id = s_tester_id ? s_site_id : b2_board_site_id();

    /* 唯一B2任务只会在取走上一个应答后再提交请求，因此应答槽不应占用。 */
    if (xQueueSend(s_replies, &reply, 0U) != pdPASS) configASSERT(0);
}

uint8_t b2_service_thermal_modes(uint8_t modes[B2_MIRROR_COUNT])
{
    //给tcp任务提供当前已知的热模式配置，返回热模式掩码
    memcpy(modes, s_thermal_modes, sizeof(s_thermal_modes));
    return s_thermal_mask;
}

uint8_t b2_service_site_id(void)
{
    //给tcp任务提供当前生效的槽位号，如果尚未采用B2配置，则返回0
    return s_tester_id ? s_site_id : 0U;
}
