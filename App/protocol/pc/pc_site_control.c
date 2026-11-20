/* B1站点开关由TCP任务串行推进；多个在途010C共用一次实际下电。 */
#include "pc_site_control.h"
#include "pc_test_service.h"
#include "b2_service.h"
#include "result_led.h"
#include "log_console.h"

typedef enum { SITE_OPEN, SITE_CLOSING, SITE_CLOSED, SITE_STOP_FAILED } site_state_t;
static site_state_t s_state = SITE_OPEN;
static pc_site_config_t s_requested;

/* TCP可以在一次recv中交付多条开关指令。每条accepted各保留一个终态，不借用JSON或堆。 */
#define SITE_REPLY_CAPACITY 16U
static struct {
    uint8_t enabled, mirror_mask, ready;
    const char *error;
} s_replies[SITE_REPLY_CAPACITY];
static unsigned s_head, s_count;

int pc_site_enabled(void)
{
    return s_state == SITE_OPEN;
}

static void apply_site_outputs(uint8_t enabled, uint8_t mirror_mask)
{
    _Static_assert(RESULT_LED_LPDDR0 == 0 && RESULT_LED_LPDDR1 == 1 && PC_SITE_MIRROR_COUNT == 2U,
                   "当前B1将两个LPDDR镜像的dut[0]映射到结果灯位0和1");
    /* eMCP未列入本板的两个DUT组；开站时继续显示其原有整轮状态。 */
    uint32_t lights = enabled ? mirror_mask | (1U << RESULT_LED_EMCP) : 0U;
    log_console_set_site_enabled(enabled);
    result_led_set_site(enabled, lights);
}

const char *pc_site_configure(const pc_site_config_t *config, int *deferred)
{
    *deferred = 0;
    if (!b2_service_matches_site(config->tester_id, config->site_id)) return "invalidArgument";
    int closing = s_state == SITE_CLOSING;
    int needs_stop = s_state == SITE_STOP_FAILED || (!config->enabled && s_state == SITE_OPEN);
    /* 先预留原连接应答位置；超出容量的请求没有被受理，不改变已提交的快照。 */
    if (s_count == SITE_REPLY_CAPACITY) return "busy";
    if (closing || needs_stop || s_count) {
        unsigned tail = (s_head + s_count) % SITE_REPLY_CAPACITY;
        s_replies[tail].enabled = config->enabled;
        s_replies[tail].mirror_mask = config->mirror_mask;
        s_replies[tail].ready = !closing && !needs_stop;
        s_replies[tail].error = NULL;
        ++s_count;
        *deferred = 1;
    }
    s_requested = *config;
    if (needs_stop) {
        s_state = SITE_CLOSING;
        apply_site_outputs(0U, 0U);
        pc_test_service_stop_site();
    } else if (!closing) {
        s_state = config->enabled ? SITE_OPEN : SITE_CLOSED;
        apply_site_outputs(config->enabled, config->mirror_mask);
    }
    return NULL;
}

void pc_site_stop_finished(const char *error)
{
    if (s_state != SITE_CLOSING) return;
    s_state = SITE_STOP_FAILED;
    if (!error) {
        s_state = s_requested.enabled ? SITE_OPEN : SITE_CLOSED;
        if (s_requested.enabled) apply_site_outputs(1U, s_requested.mirror_mask);
    }
    for (unsigned i = 0U; i < s_count; ++i) {
        unsigned index = (s_head + i) % SITE_REPLY_CAPACITY;
        if (s_replies[index].ready) continue;
        s_replies[index].ready = 1U;
        s_replies[index].error = error;
        /* OFF已实际完成。等待中的开站快照若被后来不同的快照覆盖，不能报已生效。 */
        if (!error && s_replies[index].enabled &&
            (!s_requested.enabled || s_replies[index].mirror_mask != s_requested.mirror_mask))
            s_replies[index].error = "cancelled";
    }
}

int pc_site_take_reply(const char **error)
{
    if (!s_count || !s_replies[s_head].ready) return 0;
    *error = s_replies[s_head].error;
    s_head = (s_head + 1U) % SITE_REPLY_CAPACITY;
    --s_count;
    return 1;
}

void pc_site_disconnected(void)
{
    s_head = s_count = 0U;
}
