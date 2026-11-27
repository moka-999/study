#include "rk_ddr_capacity.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

enum {
    CHANNELS_PER_MIRROR = 2,
    RK3588_CHANNEL_COUNT = RK_DDR_MIRROR_COUNT * CHANNELS_PER_MIRROR,
    RK3588_MAX_MIB = 32U * 1024U, /* B1 对整板容量的校验上限，单位 MiB。 */
    DDR_LINE_CAPACITY = 128       /* 双 Rank 的 channel 行也能完整容纳。 */
};

/* 行和通道仅由默认任务使用；启动容量和整轮快照也由 TCP 任务访问。 */
static char s_line[DDR_LINE_CAPACITY];
static size_t s_length;
static bool s_skip_tail, s_powered;
static bool s_table_open;
static uint32_t s_channels[RK3588_CHANNEL_COUNT];
static const char *s_boot_type;
static rk_ddr_chip_t s_boot_chips[RK_DDR_MIRROR_COUNT];
static bool s_boot_in_round, s_wait_power_on, s_round_table_done, s_round_frozen;
static rk_ddr_capacity_t s_round;

/* 调用方持有临界区。收表期间逐行更新，表结束或已发 010D 后保留该轮快照。 */
static void copy_boot_to_round(void)
{
    if (!s_boot_in_round || s_round_table_done || s_round_frozen) return;
    memcpy(s_round.chips, s_boot_chips, sizeof(s_boot_chips));
    s_round.current_mib = (uint32_t)(s_boot_chips[0].size_mib + s_boot_chips[1].size_mib);
    s_round_table_done = !s_table_open && s_round.current_mib != 0U;
}

static void begin_boot(bool powered, const char *type, bool power_edge)
{
    memset(s_channels, 0, sizeof(s_channels));
    s_boot_type = type;
    taskENTER_CRITICAL();
    s_table_open = powered;
    if (power_edge && powered) s_wait_power_on = false;
    s_boot_in_round = powered && s_round.round_active && !s_wait_power_on;
    memset(s_boot_chips, 0, sizeof(s_boot_chips));
    copy_boot_to_round();
    taskEXIT_CRITICAL();
}

/* B1 的 CH0/1 属于镜像 0，CH2/3 属于镜像 1。Size 已包含该通道所有 Rank。 */
static void publish_channels(void)
{
    uint32_t total = 0U;
    unsigned seen = 0U;
    for (unsigned i = 0U; i < RK3588_CHANNEL_COUNT; ++i) {
        total += s_channels[i];
        if (s_channels[i]) ++seen;
    }
    if (total > RK3588_MAX_MIB) memset(s_channels, 0, sizeof(s_channels));
    taskENTER_CRITICAL();
    for (unsigned i = 0U; i < RK_DDR_MIRROR_COUNT; ++i) {
        uint32_t first = s_channels[i * CHANNELS_PER_MIRROR];
        uint32_t second = s_channels[i * CHANNELS_PER_MIRROR + 1U];
        s_boot_chips[i].memory_type = (first || second) ? s_boot_type : NULL;
        s_boot_chips[i].size_mib = (uint64_t)first + second;
    }
    if (seen == RK3588_CHANNEL_COUNT && total <= RK3588_MAX_MIB && s_boot_type) s_table_open = false;
    copy_boot_to_round();
    taskEXIT_CRITICAL();
}

/* DDRbin 用 Size=<十进制数>MB 表示单通道容量。截断行须已经收到单位后的空格。 */
static uint32_t parse_channel_mib(const char *text, bool truncated)
{
    uint32_t mib = 0U;
    while (*text >= '0' && *text <= '9') {
        mib = mib * 10U + (unsigned)(*text - '0');
        if (mib > RK3588_MAX_MIB) return 0U;
        ++text;
    }
    if (text[0] != 'M' || text[1] != 'B') return 0U;
    return (text[2] == ' ' || (text[2] == '\0' && !truncated)) ? mib : 0U;
}

static void read_line(bool truncated)
{
    static const char channel[] = "channel[";
    static const char manufacturer[] = "Manufacturer ID:";
    s_line[s_length] = '\0';
    if (strncmp(s_line, "DDR ", 4U) == 0 && strstr(s_line, "fwver:")) {
        /* RK 可以独立复位；训练横幅和紧接通道表的内存类型行都可标识新启动。 */
        begin_boot(true, NULL, false);
    } else if (strncmp(s_line, "LPDDR4X,", sizeof("LPDDR4X,") - 1U) == 0) {
        begin_boot(true, "LPDDR4X", false);
    } else if (strncmp(s_line, "LPDDR4,", sizeof("LPDDR4,") - 1U) == 0) {
        begin_boot(true, "LPDDR4", false);
    } else if (strncmp(s_line, manufacturer, sizeof(manufacturer) - 1U) == 0 || strcmp(s_line, "out") == 0) {
        /* DDRbin 已结束此表，只有一路的镜像也可立即按已读容量上报。 */
        taskENTER_CRITICAL();
        s_table_open = false;
        copy_boot_to_round();
        taskEXIT_CRITICAL();
    } else if (s_table_open && strncmp(s_line, channel, sizeof(channel) - 1U) == 0) {
        const char *id = s_line + sizeof(channel) - 1U;
        if (id[0] < '0' || id[0] >= '0' + RK3588_CHANNEL_COUNT ||
            id[1] != ']' || id[2] != ' ') return;
        static const char size_key[] = " Size=";
        const char *size = strstr(id + 2U, size_key);
        if (!size) return;
        uint32_t mib = parse_channel_mib(size + sizeof(size_key) - 1U, truncated);
        if (!mib) return;
        unsigned index = (unsigned)(id[0] - '0');
        /* 同一通道改报另一容量时，从该行重新收齐整表。 */
        if (s_channels[index] && s_channels[index] != mib) memset(s_channels, 0, sizeof(s_channels));
        s_channels[index] = mib;
        publish_channels();
    }
}

void rk_ddr_capacity_observe(bool powered, const uint8_t *bytes, int length)
{
    if (powered != s_powered) {
        s_powered = powered;
        s_length = 0U;
        s_skip_tail = false;
        begin_boot(powered, NULL, true);
    }
    if (!powered) return;
    if (length < 0) {
        /* 丢块只截断当前行；同次启动中已读完整的类型和通道行仍有效。 */
        s_length = 0U;
        s_skip_tail = true;
        return;
    }
    for (int i = 0; i < length; ++i) {
        uint8_t byte = bytes[i];
        if (byte == '\r' || byte == '\n') {
            if (!s_skip_tail && s_length) read_line(false);
            s_length = 0U;
            s_skip_tail = false;
        } else if (!s_skip_tail) {
            if (s_length < sizeof(s_line) - 1U && byte != '\0') {
                s_line[s_length++] = (char)byte;
            } else {
                read_line(true);
                s_skip_tail = true;
            }
        }
    }
}

void rk_ddr_capacity_round_begin(void)
{
    taskENTER_CRITICAL();
    memset(s_round.chips, 0, sizeof(s_round.chips));
    s_round.current_mib = 0U;
    s_round.round_active = true;
    s_round_table_done = false;
    s_round_frozen = false;
    s_boot_in_round = false;
    s_wait_power_on = false;
    taskEXIT_CRITICAL();
}

void rk_ddr_capacity_use_running_boot(void)
{
    taskENTER_CRITICAL();
    s_wait_power_on = false;
    s_boot_in_round = s_round.round_active;
    copy_boot_to_round();
    taskEXIT_CRITICAL();
}

void rk_ddr_capacity_expect_cold_boot(void)
{
    taskENTER_CRITICAL();
    if (!s_round_frozen) {
        s_wait_power_on = true;
        s_boot_in_round = false;
        s_round_table_done = false;
        memset(s_round.chips, 0, sizeof(s_round.chips));
        s_round.current_mib = 0U;
    }
    taskEXIT_CRITICAL();
}

bool rk_ddr_capacity_take_chip_info(bool expired, rk_ddr_chip_t chips[RK_DDR_MIRROR_COUNT])
{
    taskENTER_CRITICAL();
    bool ready = s_round.round_active && (s_round_frozen || s_round_table_done || expired);
    if (ready) {
        memcpy(chips, s_round.chips, sizeof(s_round.chips));
        s_round_frozen = true;
    }
    taskEXIT_CRITICAL();
    return ready;
}

void rk_ddr_capacity_round_end(bool completed)
{
    taskENTER_CRITICAL();
    if (completed && s_round.round_active) {
        s_round.last_completed_mib = s_round.current_mib;
        s_round.has_completed = true;
    }
    memset(s_round.chips, 0, sizeof(s_round.chips));
    s_round.current_mib = 0U;
    s_round.round_active = false;
    s_boot_in_round = false;
    taskEXIT_CRITICAL();
}

rk_ddr_capacity_t rk_ddr_capacity_snapshot(void)
{
    taskENTER_CRITICAL();
    rk_ddr_capacity_t snapshot = s_round;
    taskEXIT_CRITICAL();
    return snapshot;
}
