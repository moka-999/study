# RK前级投放目录

把RK提供的当前完整Loader复制到这个目录，仅保留一份`.bin`文件，文件名可以保留原名；`.BIN`也可以。换版本时把旧文件移出本目录，或放到自己建立的子目录中。

然后在WSL中进入`/mnt/d/pro/edk2`，执行需要的脚本：

- `bash make-emmc-rk.sh`：生成eMMC整包。
- `bash make-spinor-rk.sh`：生成SPI NOR整包。
- `bash make-h1-wsl.sh`：连同原D4前级eMMC包全部生成。

不需要修改Python、脚本或SHA256。脚本会显示所用前级，检查LDR头与CRC，并自动记录其原路径和SHA256。多份或没有bin会在编译前提示，不按文件日期猜选，也不回退读取Downloads旧文件。源码同步脚本只更新本说明，不覆盖你放入的前级。

这里需要完整Rockchip Loader；裸SPL仍需由RK提供配套封装。离线打包通过不代表新前级与当前板卡及EDK的实际启动已验证。
