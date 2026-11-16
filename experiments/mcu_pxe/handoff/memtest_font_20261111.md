# MemTest86 11.7 字库下载验证

2026-11-11：用户要求优先验证跳过 `unifont.bin`，保留自动测试及 XML、HTML 上传。只修改 PC 的 TFTP 配置；16:57实板单轮已确认不请求字库，主程序下载23.066秒，最终XML和HTML报告上传完成。正常测试后的自动归还及MCU重连仍待确认。

## 本次配置

实际文件：`D:/pro/test/build/mcu_pxe/pxe-server/tftp/blacklist.cfg`，末尾新增：

```text
"",ALL,EXACT,DISABLE_LANG
```

这条规则精确匹配空主板名；ALL 表示适用所有 BIOS 版本。当前 Connect.xml 的 MBName、BIOSVer 均为空。以后 EDK 补全 SMBIOS 板名时应改用对应名称，不能把空名称规则当作所有板型的通用配置。

`mt86.cfg`、`BOOTAA64.efi`、`unifont.bin` 均未改动，前后 SHA256 已核对。字体仍保留，规则不匹配时会继续原来的加载行为。本次不修改 MCU/EDK 固件或测试项；关闭串口测试界面是另一项工作，禁用字库不等于静默输出。

## 核查依据

- 修改前的日志顺序是主程序、blacklist.cfg、unifont.bin、mt86.cfg；字体开关必须早于普通测试配置生效。
- 只读检查实际 ARM64 程序，SHA256 `670a90df573631e6c1fd42dbbdbc9fee93ed3001760e31aed1527e1fc95281df`。Ghidra 伪代码显示引号内空名称可写入规则，EXACT 对空名称执行完整比较，DISABLE_LANG 对应标志 0x80，匹配后跳过字体初始化分支。
- 主函数 `0x7792c` 的分析证据为 `ev_976e3d9318abff28162f7240703bb9c9336c150e3da70216b8c5ddcdb6e1fd8d`；名称比较函数 `0x80b78` 的证据为 `ev_6be9905524145ec3ef8fc1178585098ee42ef69132e073bd75c1fa8679129e65`。伪代码是静态推导，不能替代本轮硬件测试；完整记录保存在下面备份目录的 `rea-evidence.json`。

## 备份与验收

备份目录：`D:/pro/test/build/mcu_pxe/handoff/font_trial_20261111_164529/`。

- `blacklist.cfg.before`：原文件；需要恢复时复制回上述 TFTP 路径。
- `mt86.cfg.reference`：本次未修改的测试配置参考副本。
- `manifest.json`：前后哈希及本次修改范围。

下一轮仍从 RK 关电、PC TCP 断开开始：MCU 控制台 `net rk` 等待 armed，再 `start rk`。核对 Tftpd64 不再读取 `unifont.bin`，正常读取 `mt86.cfg`，收到本轮 TestResult.xml 和 HTML，最后确认 EDK 归还网络及 MCU 可重连。无需为此重编或烧录固件。

前一轮字体下载 30.772 秒、主程序 21.148 秒；取消字体后的单轮主程序下载为23.066秒，见下面16:57记录。不要仅凭页面没有刷新就认定字库已跳过，也不要把报告上传成功当作 DDR 判限通过。

## 16:50 重试：引导服务器变化

用户日志确认同IP交接和2M握手成功，但PXE使用的服务器变为 `192.168.1.137`，BOOTAA64.efi查询返回TFTP错误，随后自动归还网络成功。应用未加载，故本轮没有读取blacklist.cfg，不能据此判断字体规则生效与否；正常测试后的归还及MCU实际重连仍待验。

本机只读核对结果：以太网仍为 `192.168.1.200/24`，仅测试目录的tftpd64进程监听UDP67/69；根目录、绑定IP及Option66均保持原配置，主程序仍为1607208字节。16:50的本机TFTP日志只有DHCP Discover/Request，没有文件读取。地址名称查询未获得可识别的 `.137` 设备名，现有证据不能区分另一个DHCP、ProxyDHCP或其它PXE引导通告。下一轮需确认引导服务器为 `.200` 后再验字库；不修改未知设备或公司网络服务。

## 16:57 重试：字库跳过及报告上传通过

本次文件读取重新到达本机 `.200` 的 Tftpd64，期间未修改服务器配置。日志从16:57:51的DHCP开始到17:00:10.443的HTML上传完成，主程序1607208字节下载23.066秒、1块重传；随后读取6142字节的新blacklist.cfg，本轮没有任何unifont.bin请求。正常读取mt86.cfg后持续上传状态，最终收到3043字节TestResult.xml和33962字节HTML。

本轮完整日志和检查结果分别保存在备份目录的 `tftp-font-disabled-run.log`、`runtime-verification.json`。这证明当前空主板名规则在这份11.7程序上实际跳过字库并保留报告传输；23.066秒仅为主程序下载时间，不包括上电、DHCP、约8秒的应用初始化或内存测试。

`.137`的引导通告来源仍未确认；一轮选到`.137`、下一轮访问`.200`与多个DHCP/PXE应答参与选择的情形一致，但尚无抓包证据，不认为竞争问题已解决。
