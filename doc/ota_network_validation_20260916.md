> **历史资料，已停用（2026-10-27）**：当前B1/V2已移除MCU Boot和OTA，App从0x08000000直接烧录。下文仅保留当时的设计及验证证据，旧地址、命令和安装步骤不得用于当前镜像。现行操作见项目README/AGENTS中的直接烧录入口。

# V2 OTA协议接入与真实网络升级（2026-09-16）

## 结果与边界

已完成：PC 0901五动作接入、开发板部署、TCP从0.1.3升级至0.1.4、重启后0903健康确认、内部镜像及W25回读。当前板卡运行A2的0.1.4，A1保留0.1.3；完整B未覆盖，稳定区仍为此前0.1.1包，FRU不变。

本轮只操作Apollo V2。未同步B1/飞书，未提交或推送；Word接口外形沿用上一轮已检查版本。实板3+3回退、真实掉电/ECC、最大2MiB包压力、应答丢失专项及B1/B2维护联调仍待验收，不能把QEMU结果当成这些实板结论。

## 代码接入

- `App/protocol/pc/pc_ota.c/.h`：五动作查表；严格字段、整数及8位大写包ID；Base64标准字符/填充位检查；最多1024字节单块；生成0902/0903/0006。复用cJSON及固定头，不新增库。
- `pc_dispatch`：0901登记命令表，0902/0903反向拒绝；TCP只转达周期轮询和断线，不解析OTA字段。
- `ota_app`：OFF维护门、包ID绑定、前116字节早期校验、顺序写和上一成功块只读幂等、30秒接收空闲、断线从头重来、完整包复验及READY提交。
- 提交成功或提交结果不确定后均保留维护门，看门狗任务在约4–4.5秒内复位，独立于TCP发送结果。B决定安装槽、重新检查、安装、写入版本信息；App必要任务连续健康30秒才确认。
- 每次OTA查询调用`firmware_version_load()`读取W25，再核对启动记录、运行VTOR、epoch和版本载荷；不使用编译期版本组帧。下载占用与当前镜像健康分别表达。
- B/App共享安装器增加只读前缀校验入口，代码保持一致；已在板上的完整B无须为此重新烧录。

仅`ENABLE_OTA_BOOT=ON`且槽限构建启用下载。普通/隔离镜像明确返回notSupported，不能在无可信启动记录时擦写下载区或假报完成。新改代码及验证脚本注释为中文。

## 构建与离线验证

| 检查 | 结果 |
|---|---|
| 普通V2 Debug构建 | 通过，未启用OTA时编译/拒绝入口仍有效 |
| 0.1.3、0.1.4 OTA Release | 每版App 140580 B、1322项修正、包145984 B |
| 同一A1镜像转换为A2 | 与独立链接A2逐字节一致，反向A1一致 |
| 实际App/PC OTA C，QEMU+模拟板级端口 | 4组通过：正常READY、半页失败、坏CRC、提交字已写但返回错误 |
| 上述4组共同检查 | 实际cJSON/响应头、非法Base64、重复块不再写、进度保留、取消/断线/超时、包头/板型/版本提前拒绝、W25版本矛盾报错、FRU不变及提交后复位 |
| Boot Release安装回归 | 146项通过；启动管理/日志掉电模型/3+3恢复通过；4项打包反例通过 |
| 原PC、代码边界、纯接口头 | 24项、19项及GCC检查通过 |
| OTA文档合同 | 24个示例、29个字段反例通过；数据帧1454B、查询270B均未超预算 |

可复跑入口：`tests/verify_ota_app.py`（需要`--package`及`--compiler`）、`tests/verify_ota_relocation.py`、Boot的`tools/dev.ps1 ota-test`。QEMU只替换板级IO，执行正式C校验器和OTA协议模块，不宣称真实Flash故障注入已验收。

## 实板过程与证据

目标：ST-Link `54FF6D067182505239431087`，STM32H743 Rev.V、2MiB、3.26V；TCP `192.168.1.201:8888`。

证据目录：`D:\pro\test\build\ota-network-hw-20260916`。本机原始Flash备份不纳入源码仓库。

1. 备份内部Flash全2MiB；W25前0x410000字节双遍读取完全一致。`backup.json`记录SHA-256；原FRU与此前备份一致。
2. 通过RAM维护工具仅向W25下载区写入0.1.3包并追加READY；由既有B安装到非活动A1，未直接SWD烧写A1。TCP先查到trial/0.1.3，后确认confirmed；因此本轮也覆盖A2→A1同Bank安装。
3. 真实TCP验证字段/方向/非法Base64、半包、1024字节块、重复begin/重复块、同偏移不同数据、错序、未收齐finish、取消、维护门及心跳。
4. 下载中断线后重新begin返回nextOffset=0；保持心跳但不传数据，30秒后自动取消。
5. 完整传入末字节故意损坏的包，finish返回crcMismatch，原0.1.3继续运行；再从0完整传入正确0.1.4包。
6. finish返回0902/accepted/ready；重复finish不重复提交，cancel被拒绝。断开连接后自动复位，B安装A2；PC重连查询目标包得到0903，固件0.1.4且confirmed=true，重复查询及心跳通过。
7. SWD读取实际A2：VTOR=0x08100000，健康掩码0x1F，failed=0，确认时刻31521ms；CFSR/HFSR均0。12个RTOS任务、空闲堆20208 B。这是空闲时快照，不代表最坏栈/堆压力已验收。
8. 内部Flash全量回读：A1的140580字节等于0.1.3镜像；A2的140580字节等于由0.1.4 A1转换的镜像。B的128KiB及A2顶部预留128KiB均与升级前一致。
9. W25完整相关区域回读：下载区等于所发145984字节包；FRU 8KiB及整个稳定区2MiB逐字节未变。日志快照代数27、epoch7、NORMAL、active=A2、previous=A1、valid_mask=3、pending=0、failures=0、last_error=0。
10. 收尾已退出RAM助手并复位恢复App；`final-query.json`记录收尾后的网络状态，不以此前epoch7快照代替新一次启动状态。

关键证据：`network-014.json`（逐请求/响应，省略Base64正文）、`final-swd.log`、`readback-report.json`、`final-internal.bin`、`final-w25.bin`。

| 对象 | SHA-256 |
|---|---|
| 升级前内部Flash | `4c29ec0ea64cf52a65eb8ea1a792c842c8d8cb9c6030c6708cd3d8af28d658ff` |
| 升级前W25相关区域 | `80e2bd17a7b4cfc1c73209935ab2012992de27ca76aa119345e5ec703362e275` |
| 0.1.4发送包 | `7711f024b6150db990fe7b10c793c885bb490a83b41b68cc487a650e6fc641a2` |
| A2实际镜像 | `80a88a17f59cbf8efaa68ef53c42ffe23aadc18fc2dd0b43f5b6fc3ee02fc4ee` |
| FRU前后不变 | `2aa4dd421dc9e905f737d5f648ecd374dbd0ae9a8e7f3ca88716a99078d2d41c` |

## 后续使用

`tests/pc_ota_network_test.py --report <新报告文件>`默认只查询。加`--exercise --package <包>`会写下载区并测试拒绝/取消/超时；加`--upgrade`才提交升级并重启。必须先获实板操作授权、保留备份，报告不覆盖旧证据。下一包必须高于当前0.1.4，不能拿同一版本重复安装冒充升级。

构建OTA包沿用`tests/verify_ota_relocation.py --ota-boot --version <版本> --build-type Release`并提供原GNU/CMake/Ninja路径，再调用`../bootloader/tools/pack_ota.py --app-build <已验证A1目录>`。禁止把普通`build_debug/boot.elf`（旧最小跳转B）覆盖到本板，或绕过启动记录直接烧普通App。

本轮使用ponytail原则复用现有安装器、存储锁、cJSON和帧结构；只增加必要的OTA协议模块，没有另建升级框架。
