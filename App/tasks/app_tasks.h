/* B1正式任务入口；测试/波形任务不属于正式启动路径。 */
#ifndef B1_APP_TASKS_H
#define B1_APP_TASKS_H

void StartTcpServerTask(void *argument);
void StartRkTask(void *argument);
void StartMonitorTask(void *argument);

#endif
