# MCU / RK 同 IP：连接时更新配置的离线准备

2026-11-10。目标为一个槽位沿用一个业务 IP：正常阶段由 MCU 使用，SLT 网络阶段由 RK 使用，退出后归还 MCU。IP 仍取 MCU 当前生效地址，不增加 `+128` 地址规则。

本文记录 Windows 配置辅助工具及其离线验证。后续 MCU/EDK 手动交接已在 [H1 实现](handoff/README.md)中完成源码接入，保留完整 MCU、使用默认 SPL 引导；实板验证及正式自动 SLT 接入尚未完成。本轮没有启动 DHCP/PXE/TFTP、连接板卡或烧录。旧 D4 不是完整固件的同 IP 实现，不能混用其镜像。

## 已实现的最小工具

[`same_ip_bindings.py`](same_ip_bindings.py) 使用 Python 标准库和 Windows INI API，直接更新指定 Tftpd64 INI 中的静态条目：

```ini
[DHCP]
PoolSize=0
06:2A:DF:95:2D:04=192.168.1.201
```

- 每个 RK MAC 一条记录；同一 MAC 可在空闲时更新 IP，其它设备记录不变。
- 检查 MAC、服务器网段、网络/广播地址、服务器地址和重复 IP。此检查仅针对指定 INI，不能证明实际网络没有其它占用者。
- 本轮地址锁记录在 `[SLT Binding Locks]`，包含调用方提供的 session；锁定时核对当前 TCP 对端 IP，防止过期连接锁到另一地址。
- 锁定后同 IP 重复登记和同 session 重复加锁可幂等；改变 IP、删除、换 session 或以旧 session 解锁均拒绝。解锁必须由上层确认 RK 已退出，不由断线或超时自动触发。
- `.ini.lock` 串行化本工具的写操作；遗留文件不会自动清除。它不能阻止其它程序直接编辑 INI。
- 逐键写入后回读，不覆盖整个配置；保留 `Services`、`PoolSize`、TFTP 路径、启动文件及其它设置。不会启用 DHCP。
- 禁用旧绑定时写 `255.255.255.255`。这是 **Tftpd64 4.74 专用的 INADDR_NONE 禁用标记**，不是分给 RK 的地址；搭配 `PoolSize=0`，静态查询不匹配且不退回旧注册表值。

## 连接成功时怎样调用

上位机可以使用原有 TCP 连接的对端地址，不新增 PC 命令，也不让工具再开一条连接抢占单客户端。现有 `0102` 只有型号信息，没有唯一设备标识或 RK MAC，所以本轮接口显式接收已经确认的 RK MAC。

```python
from same_ip_bindings import update_binding, lock_binding, unlock_binding

# 由现有连接提供 mcu_ip；rk_mac 来自已确认的设备配对，不由 IP 猜测。
update_binding(ini_path, rk_mac, mcu_ip)
# SLT 交接前校验并锁定。session 每轮新建，不能重复用旧轮次编号。
lock_binding(ini_path, rk_mac, session, mcu_ip)
# 后续 MCU / EDK 明确完成退出、撤销使用地址权限后，才由调用方执行：
unlock_binding(ini_path, rk_mac, session)
```

**这些调用只完成本地配置登记和锁定，尚未接入现有上位机的连接回调。** 它们不使 MCU 让出 IP、不授予 EDK 使用许可、不驱动 Tftpd64，也不验证 DHCP 实际应答。

当前 SerialNet 虚拟 MAC 是 MCU MAC 的首字节 `02` 改为 `06`，其余五字节不变；该规则来自实验源码，不能默认套到未来物理网卡。第一轮使用已经从日志确认的 RK MAC。若以后通过 ARP 辅助配对，必须核对同二层网络和新鲜邻居记录，不能把路由器 MAC 当成板卡 MAC。

## Tftpd64 的读取依据及边界

本机 4.74 源码位置：`E:/MYZR-RK3588/uefi/pxe-test/tftpd64-source/tftpd64-4.74`。实验 EXE 文件版本为 `4.74.0.0`。

- `src/_services/bootpd.c:205`：`DHCP_StaticAssignation` 按 BOOTP `chaddr` 查 `[DHCP]` 的大写冒号 MAC 键。
- 同文件 701、748 行：Discover 和 Request 每次重新查询，静态地址没有额外应用缓存。
- 同文件 1320 行：运行期 `PoolSize=0` 且静态查询返回 `INADDR_NONE` 时忽略请求。
- `src/_libs/registry/registry.c:41`：使用 `GetPrivateProfileString`。INI 缺键时会回退注册表；空键检查使用 1024 字节枚举缓冲，所以禁用工具使用非空 INADDR_NONE 标记，不依赖多板配置下的空键枚举。
- 当前 INI `Services=1` 只有 TFTP；`5` 为 TFTP+DHCP。本工具始终保留原值。首次启用/绑定网卡是单独的运行配置步骤，不能靠 MAC 更新接口代替。

部署前仍须核对实际 INI 路径（含 `TFTP_INI` 等覆盖）、服务版本、运行期配置及注册表遗留 MAC；本工具不修改或枚举注册表，也不能发现未列在 INI 中的遗留映射。静态条目更新不会使 RK 立即放弃已有 IP，必须在 RK 停止用旧地址后重新走受控 DHCP。

旧 `pxe_proxy.py` 占用 UDP 67，不能与 Tftpd64 DHCP 按原方式同时启动。共享网络其它 DHCP 的应答也需要在实测中排除。仅通过 Windows API 跨进程读回，不等于已经验证 Tftpd64 的 Discover/ACK 或网络热更新。

## 正式自动 SLT 的目标顺序（尚未接入）

1. PC 已连接 MCU，按当前对端 IP 登记配对；本轮 SLT 开始前，核对并锁定地址。
2. MCU 完成准备并确保 PC 已收到相应响应，进入明确的计划断线流程；保留整轮和 SLT 归属。
3. MCU 停止使用该 IP，仍保持以太网及串口桥工作；向 EDK 发出本轮使用许可。
4. EDK 重新取地址并核对与 MCU 授权值一致。只有此后才进入 PXE/MemTest86 网络流程。
5. EDK 停止所有网络使用并明确释放。MCU 恢复 IP 和 ARP 通告，PC 重连、重新关联原 SLT，再按原格式发 `010B` 收尾。
6. 上层确认本轮已结束后解除配置锁；下一次连接可以更新地址。

不能用 SNP `STOP`、DHCP RELEASE、掉线、空闲超时或“报告文件出现”单独证明整个 RK 已放弃 IP。现有普通 TCP 连接不会迁移到 EDK；当前固件还没有计划断线重关联路径。H1 已实现独立于测试轮次的手动握手、串口协议切换和归还；本轮默认 SPL 直接加载 EDK，不需要新增 SPL 继续引导命令。以上涉及自动整轮/SLT 归属的步骤仍待后续接入。

## 离线验证入口

在 `D:/pro/test` 运行：

```powershell
python -B -m unittest discover -s experiments/mcu_pxe/tests -p test_same_ip_bindings.py -v
python -B -m unittest discover -s experiments/mcu_pxe/tests -p test_pxe_proxy.py -v
```

测试在临时目录使用真实 Windows INI API，覆盖多板更新、重复/非法地址、锁定和旧 session、过期连接预期 IP、保留其它设置、进程间读回和写锁冲突；不打开网络端口。旧 PXE 代理回归仍只允许提供启动文件，不产生地址租约。

`build/same_ip_handoff_20261110/` 保存本轮候选 INI 和离线流程报告。候选配置 DHCP 仍关闭。离线通过只表示登记和门禁工作，实际同 IP 的 DHCP、ARP、下载/报告回传及交还后 PC 重连都未验收。
