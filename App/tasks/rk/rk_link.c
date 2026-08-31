/* RK链路共享序号：私有状态只通过一个受保护的入口访问。 */
#include "rk_link.h"
#include "task.h"

static uint64_t s_next_sequence;

uint64_t rk_link_next_sequence(void)
{
    uint64_t sequence;
    taskENTER_CRITICAL();
    sequence = s_next_sequence++;
    taskEXIT_CRITICAL();
    return sequence;
}
