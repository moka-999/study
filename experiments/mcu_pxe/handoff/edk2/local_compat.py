"""旧 Rockchip 固件的宿主 libc 依赖替换；仅生成本地待写内容，不改变硬件控制逻辑。"""
from pathlib import Path


ROCKCHIP = Path('edk2-platforms/Silicon/Rockchip')


def firmware_compat_changes(workspace):
    """返回经过明确匹配的源码替换；原件备份和实际写入由 sync_edk2 统一完成。"""
    changes = {}

    def replace_file(relative, replacements):
        text = (workspace / relative).read_text(encoding='utf8')
        for old, new in replacements:
            if new and new in text:
                continue
            if text.count(old) == 1:
                text = text.replace(old, new)
            elif old not in text and (not new or new in text):
                continue
            else:
                raise ValueError(f'宿主头兼容补丁与源码不符：{relative}')
        changes[relative] = text.encode('utf8')

    replace_file(ROCKCHIP / 'Library/PWMLib/PWMLib.c', [
        ('#include <stdlib.h>', '#include <Library/BaseLib.h>'),
        ('  ldiv_t PeriodDiv, DutyDiv;\n', ''),
        ('  PeriodDiv = ldiv((UINT64)FREQUENCY_24M * Data->PeriodNs, 1000000000);\n'
         '  DutyDiv = ldiv((UINT64)FREQUENCY_24M * Data->DutyNs, 1000000000);\n\n'
         '  PeriodCycle = PeriodDiv.quot;\n  DutyCycle = DutyDiv.quot;',
         '  PeriodCycle = DivU64x32((UINT64)FREQUENCY_24M * Data->PeriodNs, 1000000000);\n'
         '  DutyCycle = DivU64x32((UINT64)FREQUENCY_24M * Data->DutyNs, 1000000000);'),
    ])
    replace_file(ROCKCHIP / 'Library/PWMLib/PWMLib.inf', [
        ('[LibraryClasses]\n  DebugLib', '[LibraryClasses]\n  BaseLib\n  DebugLib'),
    ])
    replace_file(ROCKCHIP / 'Drivers/PciPlatform/PcieInit.c', [
        ('#include <stdlib.h>\n', ''),
        ('#include <string.h>', '#include <Library/BaseMemoryLib.h>'),
        ('#include <malloc.h>\n', ''),
        ('#include <stdio.h>\n', ''),
        ('memset(priv, 0, sizeof(struct rk_pcie));', 'ZeroMem(priv, sizeof(struct rk_pcie));'),
    ])
    replace_file(ROCKCHIP / 'Drivers/Vop2Dxe/Vop2Dxe.c', [
        ('#include <string.h>', '#include <Library/BaseMemoryLib.h>'),
        ('memset(RockchipVop2->VpPlaneMask, 0, sizeof(VOP2_VP_PLANE_MASK) * VOP2_VP_MAX);',
         'ZeroMem(RockchipVop2->VpPlaneMask, sizeof(VOP2_VP_PLANE_MASK) * VOP2_VP_MAX);'),
    ])
    for directory, name in (('PciPlatform', 'PciPlatform'), ('Vop2Dxe', 'Vop2Dxe')):
        replace_file(ROCKCHIP / f'Drivers/{directory}/{name}.inf', [
            ('  BaseLib\n', '  BaseLib\n  BaseMemoryLib\n'),
        ])
    replace_file(ROCKCHIP / 'Drivers/NorFlashDxe/NorFlashDxe.c', [
        ('#include <string.h>\n', ''),
        ('memcpy(&TmpBuf[Align], Buf, ToUpdate);', 'CopyMem(&TmpBuf[Align], Buf, ToUpdate);'),
    ])
    for relative in ('Drivers/SdhciHostDxe/SdhciHostDxe.c',
                     'Library/DisplayLib/DwHdmiQpLib.c', 'Library/DisplayLib/AnalogixDpLib.c'):
        replace_file(ROCKCHIP / relative, [('#include <string.h>\n', '')])
    return changes
