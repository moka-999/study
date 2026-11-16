"""将已配对的 H1 源码同步到指定完整 EDK2 工程；备份被覆盖文件，不编译或烧录。"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import shutil
from local_compat import firmware_compat_changes


ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[3]
COMMON = REPO.parent / 'b1/App/protocol/serial_net'
DRIVER = Path('edk2-platforms/Silicon/Rockchip/Drivers/SerialNetSnpDxe')
APP = Path('edk2-platforms/Silicon/Rockchip/Applications/NetReturn')
PLATFORM = Path('edk2-platforms/Platform/Rockchip/RK3588')
BOOT_MANAGER = Path('edk2/ArmPkg/Library/PlatformBootManagerLib')
PXE = Path('edk2/NetworkPkg/UefiPxeBcDxe')
DRIVER_INF = 'Silicon/Rockchip/Drivers/SerialNetSnpDxe/SerialNetSnpDxe.inf'
APP_INF = 'Silicon/Rockchip/Applications/NetReturn/NetReturn.inf'
FVFS_INF = 'MdeModulePkg/Universal/FvSimpleFileSystemDxe/FvSimpleFileSystemDxe.inf'
MARKER_OLD = 'B1-SERIAL-NET-20261103-D4'
MARKER_PREVIOUS = 'B1-SERIAL-NET-20261110-H1'
MARKER_AUTO = 'B1-SERIAL-NET-20261111-H1A'
MARKER_BAUD = 'B1-SERIAL-NET-20261111-H1B'
MARKER_NEW = 'B1-SERIAL-NET-20261112-H1C'


def dhcp_handoff_changes(workspace):
    """只在PXE首次DISCOVER增加授权IP提示，REQUEST保留标准DHCP实现。"""
    changes = {PXE / name: (ROOT / name).read_bytes()
               for name in ('HandoffDhcp4.c', 'HandoffDhcp4.h', 'NetHandoffProtocol.h')}
    path = PXE / 'PxeBcDhcp4.c'
    text = (workspace / path).read_text(encoding='utf-8-sig')
    include = '#include "PxeBcImpl.h"'
    anchor = '  case Dhcp4SendDiscover:\n    if (Packet->Length > PXEBC_DHCP4_PACKET_MAX_SIZE) {'
    hook = '''  case Dhcp4SendDiscover:
    /* 仅授权虚拟网卡申请本轮MCU地址；新包所有权在成功返回后交给DHCP驱动。 */
    Status = HandoffDhcp4Discover (Private->Controller, This, Packet, NewPacket);
    if (EFI_ERROR (Status)) return Status;
    if (*NewPacket != NULL) {
      if ((*NewPacket)->Length > PXEBC_DHCP4_PACKET_MAX_SIZE) {
        FreePool (*NewPacket);
        *NewPacket = NULL;
        return EFI_ABORTED;
      }
      Packet = *NewPacket;
    }
    if (Packet->Length > PXEBC_DHCP4_PACKET_MAX_SIZE) {'''
    if text.count(include) != 1:
        raise ValueError('PXE DHCP包含入口不符合已核对基线')
    if '#include "HandoffDhcp4.h"' not in text:
        text = text.replace(include, include + '\n#include "HandoffDhcp4.h"')
    if 'HandoffDhcp4Discover (' not in text:
        if text.count(anchor) != 1:
            raise ValueError('PXE DHCP DISCOVER回调不符合已核对基线')
        text = text.replace(anchor, hook)
    elif text.count(hook) != 1:
        raise ValueError('已有DHCP授权入口被修改，请先核对差异')
    changes[path] = text.encode('utf-8')
    path = PXE / 'UefiPxeBcDxe.inf'
    text = (workspace / path).read_text(encoding='utf-8-sig')
    anchor = '  PxeBcDhcp4.c\n'
    if text.count(anchor) != 1:
        raise ValueError('PXE源文件列表不符合已核对基线')
    for name in ('HandoffDhcp4.c', 'HandoffDhcp4.h', 'NetHandoffProtocol.h'):
        if '  ' + name + '\n' not in text:
            text = text.replace(anchor, anchor + '  ' + name + '\n')
    changes[path] = text.encode('utf-8')
    return changes


def automatic_boot_changes(workspace):
    """在控制台和Shell注册完成后接入一次自动流程，返回后保留完整手动菜单。"""
    changes = {BOOT_MANAGER / name: (ROOT / name).read_bytes()
               for name in ('SltAutoBoot.c', 'SltAutoBoot.h', 'NetHandoffProtocol.h')}
    path = BOOT_MANAGER / 'PlatformBm.c'
    text = (workspace / path).read_text(encoding='utf-8-sig')
    include = '#include "PlatformBm.h"'
    anchor = '  PlatformRegisterFvBootOption (&gUefiShellFileGuid, L"UEFI Shell", 0, &Key);'
    if text.count(include) != 1 or text.count(anchor) != 1:
        raise ValueError('Arm平台启动入口不符合已核对基线')
    if '#include "SltAutoBoot.h"' not in text:
        text = text.replace(include, include + '\n#include "SltAutoBoot.h"')
    hook = '''
  /* 授权会话只自动执行一次；完成、失败或ESC后留在菜单，不再次自动PXE。 */
  if (SltAutoBoot ()) {
    EFI_BOOT_MANAGER_LOAD_OPTION Menu;
    Status = EfiBootManagerGetBootManagerMenu (&Menu);
    if (!EFI_ERROR (Status)) {
      for (;;) EfiBootManagerBoot (&Menu);
    }
  }'''
    if 'if (SltAutoBoot ())' not in text:
        text = text.replace(anchor, anchor + '\n' + hook)
    elif text.count(hook) != 1:
        raise ValueError('已有自动启动入口被修改，请先核对差异')
    changes[path] = text.encode('utf-8')
    path = BOOT_MANAGER / 'PlatformBootManagerLib.inf'
    text = (workspace / path).read_text(encoding='utf-8-sig')
    for anchor, additions in (
        ('  PlatformBm.h', ('SltAutoBoot.c', 'SltAutoBoot.h', 'NetHandoffProtocol.h')),
        ('  PrintLib', ('TimerLib',)),
        ('  gEfiSimpleFileSystemProtocolGuid', ('gEfiSimpleNetworkProtocolGuid',)),
    ):
        if text.count(anchor + '\n') != 1:
            raise ValueError('Arm平台启动库依赖不符合已核对基线')
        for addition in additions:
            if '  ' + addition + '\n' not in text:
                text = text.replace(anchor + '\n', anchor + '\n  ' + addition + '\n')
    changes[path] = text.encode('utf-8')
    return changes


def sync(workspace):
    """仅覆盖本实验涉及的固定路径；备份和散列留在 MCU 工程 build 内。"""
    workspace = workspace.resolve(strict=True)
    if workspace != Path('D:/pro/edk2').resolve(strict=True):
        raise ValueError('本轮仅允许同步到 D:/pro/edk2，不覆盖其他来源工程')
    changes = {}
    for name in ('SerialNetSnpDxe.c', 'SerialNetSnpDxe.inf', 'NetHandoffProtocol.h'):
        changes[DRIVER / name] = (ROOT / name).read_bytes()
    for name in ('serial_net.c', 'serial_net.h', 'ip_guard.c', 'ip_guard.h'):
        changes[DRIVER / name] = (COMMON / name).read_bytes()
    for name in ('NetReturn.c', 'NetReturn.inf', 'NetHandoffProtocol.h'):
        changes[APP / name] = (ROOT / name).read_bytes()
    for suffix in ('dsc', 'fdf'):
        relative = PLATFORM / ('RK3588.' + suffix)
        text = (workspace / relative).read_text(encoding='utf-8-sig')
        if text.count(DRIVER_INF) != 1 or text.count(APP_INF) > 1 or text.count(FVFS_INF) > 1:
            raise ValueError('平台组件列表不符合 D4/H1 基线，停止覆盖')
        for module in (APP_INF, FVFS_INF):
            if module not in text:
                line = ('  INF ' if suffix == 'fdf' else '  ') + DRIVER_INF
                text = text.replace(line, line + '\n' + ('  INF ' if suffix == 'fdf' else '  ') + module)
        if suffix == 'dsc':
            if sum(text.count(marker) for marker in (MARKER_OLD, MARKER_PREVIOUS, MARKER_AUTO, MARKER_BAUD, MARKER_NEW)) != 1:
                raise ValueError('平台版本标记不符合 D4/H1 基线')
            text = text.replace(MARKER_OLD, MARKER_NEW).replace(MARKER_PREVIOUS, MARKER_NEW).replace(MARKER_AUTO, MARKER_NEW).replace(MARKER_BAUD, MARKER_NEW)
        changes[relative] = text.encode('utf8')
    changes[Path('HANDOFF-H1.md')] = (ROOT / 'README.md').read_bytes()
    for name in ('make-h1-wsl.sh', 'make-emmc-d4.sh', 'make-emmc-rk.sh', 'make-spinor-rk.sh'):
        changes[Path(name)] = (ROOT / name).read_bytes()
    changes[Path('loader/rk/README.md')] = (ROOT / 'loader-rk-README.md').read_bytes()
    changes.update(automatic_boot_changes(workspace))
    changes.update(dhcp_handoff_changes(workspace))
    changes.update(firmware_compat_changes(workspace))
    # 旧版主机工具把已关闭的 FILE* 传给 %s；修正实参，不关闭 GCC 的错误检查。
    old = 'Error(NULL, 0, 4001, "Resource", "memory cannot be allocated  of %s", InFileHandle);'
    new = 'Error(NULL, 0, 4001, "Resource", "memory cannot be allocated  of %s", InFile);'
    for tool in ('GenFfs', 'GenSec'):
        relative = Path(f'edk2/BaseTools/Source/C/{tool}/{tool}.c')
        text = (workspace / relative).read_text(encoding='utf8')
        if text.count(old) + text.count(new) != 1:
            raise ValueError(f'{tool} 错误路径与已审阅补丁不一致，停止覆盖')
        changes[relative] = text.replace(old, new).encode('utf8')
    backup = REPO / 'build/mcu_pxe/handoff' / ('edk2-before-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    backup.mkdir(parents=True, exist_ok=False)
    manifest = {'workspace': str(workspace), 'backup': str(backup), 'files': {}}
    # 先完整备份，再写入新源码；异常时保留备份供人工恢复，不自动回滚他人修改。
    for relative, data in changes.items():
        target = workspace / relative
        old_hash = None
        if target.exists():
            saved = backup / relative
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, saved)
            old_hash = hashlib.sha256(saved.read_bytes()).hexdigest()
        manifest['files'][relative.as_posix()] = {
            'before_sha256': old_hash, 'after_sha256': hashlib.sha256(data).hexdigest()}
    for relative, data in changes.items():
        target = workspace / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        if target.read_bytes() != data:
            raise OSError(f'源码同步回读不一致：{target}')
    (backup / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf8')
    print(json.dumps({'workspace': str(workspace), 'backup': str(backup), 'files': len(changes)}, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk2', type=Path, default=Path('D:/pro/edk2'))
    sync(parser.parse_args().edk2)
