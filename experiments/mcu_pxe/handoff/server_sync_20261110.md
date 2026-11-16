# H1服务器副本：uefi-b1-20261110

2026-11-10，按用户指定名称同步到`lilihong@192.168.1.122:/disk1/lilihong/uefi-b1-20261110`。目标此前不存在，先传入独立暂存目录、校验后更名；没有覆盖旧`uefi`或`uefi-b1-20261026`工程。

来源为当前`D:/pro/edk2`。保留完整EDK平台、第三方源码/二进制依赖、H1串口虚拟网卡、同IP授权/归还和NetReturn。仅排除根Build、Git元数据、Python/Conf缓存及BaseTools C编译中间文件；脚本按shebang恢复Linux执行位，源文件字节不变。

一并放入`inputs/`中的两份Loader/D4参考FIT，以及`prebuilt-h1/`中的本机FV、NetReturn、H1 FIT、两套eMMC整包、一套SPI NOR整包和对应校验记录。预编译产物明确标为本机来源，不是服务器重新编译所得。自动整包打包入口尚未统一，原`pack-b1.sh`保留历史默认配置；不能只复制Loader就认为原脚本已经使用它。

## 传输核对

- 清单文件数：36180；总字节：465566949；另有清单自身及服务器核验结果文件。
- 压缩包：126631270字节，SHA256 `342de91783fa3267a4d854f2f6119d67bb89bef85a8510876224dce1a91e6715`。
- 服务器逐文件大小/SHA256全部通过，DSC/FDF中的H1版本、SerialNetSnpDxe和NetReturn均存在。旧uefi的make.sh和RK3588.dsc前后散列相同。
- 服务器入口：`README-server.md`、`build-h1.sh`；逐文件清单：`SERVER-SOURCE-MANIFEST.json`；核验：`SERVER-TRANSFER-VERIFICATION.json`。
- 本机传输记录：`build/mcu_pxe/handoff/server_sync_20261110/transfer-report.json`；原始来源逐文件复核未发生变化。

## 编译状态

`bash -n build-h1.sh`通过。实际运行在工具链预检阶段以2退出：旧`/disk1/lilihong/prebuilts`链接到`/disk1/rk3588-linux-5.10.226/prebuilts`，当前账号对指定GCC路径访问权限不足。服务器主机已有GCC 11.4和Python 3.10，但在本账号目录、`/opt`、`/usr/local`的有界只读搜索未找到可用AArch64交叉GCC。

未修改SDK权限、安装工具链或向原工程写入。可在取得合适工具链访问权限后，以`GCC5_AARCH64_PREFIX`指定前缀运行本目录`build-h1.sh`。同步完成不等于服务器重编成功；本机H1构建和既有离线验包结论保持原范围。未连接板卡、烧录、上电或启用网络服务。
