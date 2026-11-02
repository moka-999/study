"""烧录顺序和拒绝边界的离线测试；工具调用全部替身化，不连接任何探针。"""
from pathlib import Path
import struct
import tempfile
import unittest
import sys
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import direct_flash as flash
import json
import zlib

SERIAL = "1234567890ABCDEF12345678"
TARGET = "Device ID : 0x450\nFlash size : 2048 KBytes\nRDP : 0xAA\nSWAP_BANK : 0x0\nBOOT_ADD0 : 0x800\n"
TARGET_223 = TARGET.replace("Flash size : 2048 KBytes", "NVM size : 2 MBytes")


def write_image(image, data, base=0x08000000):
    """生成最小ARM ELF/BIN/清单用于离线边界测试。"""
    image.write_bytes(data)
    image.with_suffix('.manifest.json').write_text(json.dumps(dict(target='b1-h743', image_length=len(data), image_crc32=f'0x{zlib.crc32(data):08X}')),encoding='utf-8')
    header=bytearray(84)
    header[:7]=b'\x7fELF\x01\x01\x01'
    struct.pack_into('<H',header,18,40)
    struct.pack_into('<II',header,24,struct.unpack_from('<I',data,4)[0],52)
    struct.pack_into('<HH',header,42,32,1)
    struct.pack_into('<8I',header,52,1,84,base,base,len(data),len(data),5,4)
    image.with_suffix('.elf').write_bytes(header+data)


def merged_image():
    data = bytearray(b"\xFF" * (flash.APP_OFFSET + 1024))
    struct.pack_into('<II', data, 0, 0x20020000, 0x08000299)
    struct.pack_into('<II', data, flash.APP_OFFSET, 0x20020000, 0x08020299)
    return bytes(data)


class FlashTests(unittest.TestCase):
    def test_probe_selection(self):
        listing = "ST-LINK SN : " + SERIAL
        self.assertEqual(flash.select_serial(listing, ""), SERIAL)
        many = listing + "\nST-LINK SN : ABCDEF123456789012345678"
        self.assertEqual(flash.select_serial(many, SERIAL), SERIAL)
        for output, requested in (("", ""), (many, ""), (listing, "000000000000000000000000")):
            with self.assertRaises(RuntimeError):
                flash.select_serial(output, requested)

    def test_target_guard(self):
        flash.check_target(TARGET)
        for text in (TARGET.replace("0x450", "0x413"), TARGET.replace("2048", "1024"),
                     TARGET.replace("0xAA", "0xBB"), TARGET.replace("BANK : 0x0", "BANK : 0x1"),
                     TARGET.replace("0x800", "0x810"), TARGET.replace("RDP : 0xAA", "")):
            with self.assertRaises(RuntimeError):
                flash.check_target(text)

    def test_target_accepts_cubeprogrammer_223_output(self):
        flash.check_target(TARGET_223)

    def test_target_capacity_units(self):
        for label in ("Flash size", "NVM size"):
            for size in ("2048 KBytes", "2 MBytes", "2097152 Bytes"):
                with self.subTest(label=label, size=size):
                    flash.check_target(TARGET.replace("Flash size : 2048 KBytes", f"{label} : {size}"))
        flash.check_target(TARGET_223.replace("NVM size    : 2 MBytes", "  nvm SIZE\t:\t2 mbytes  "))
        flash.check_target(TARGET + "NVM size : 2 MBytes\n")

    def test_target_rejects_unknown_or_conflicting_capacity(self):
        for size in ("1024 KBytes", "1 MBytes", "4 MBytes", "2048 MBytes", "20480 KBytes",
                     "2048 Kbits", "2", "-2 MBytes", "2.5 MBytes", "2 MBytes unknown"):
            with self.subTest(size=size), self.assertRaises(RuntimeError):
                flash.check_target(TARGET_223.replace("2 MBytes", size))
        for text in (TARGET.replace("Flash size : 2048 KBytes\n", ""),
                     TARGET + "NVM size : 1 MBytes\n", TARGET + "NVM size : unknown\n",
                     TARGET + "NVM size :\n"):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                flash.check_target(text)

    def run_deploy(self, failure=None, target_output=TARGET):
        calls = []
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name)
            image = folder / "input.bin"
            data = merged_image()
            write_image(image, data)

            def fake(programmer, arguments, log):
                del programmer, log
                calls.append(arguments)
                if arguments[:2] == ["-l", "st-link"]:
                    return "ST-LINK SN : " + SERIAL
                self.assertIn("sn=" + SERIAL, arguments)
                if "displ" in arguments:
                    if failure == "target":
                        return target_output.replace("0x450", "0x413")
                    if failure == "capacity":
                        return TARGET_223.replace("2 MBytes", "1 MBytes")
                    return target_output
                if "-u" in arguments:
                    target = Path(arguments[-1])
                    if target.name == "internal-before.bin":
                        target.write_bytes(b"\xFF" * (1 if failure == "backup" else flash.FLASH_SIZE))
                    else:
                        target.write_bytes(b"broken" if failure == "readback" else data)
                if "-w" in arguments and failure == "write":
                    raise RuntimeError("模拟下载失败")
                return "OK"

            with patch("direct_flash.run_cli", side_effect=fake), patch("builtins.print"):
                if failure:
                    with self.assertRaises(RuntimeError):
                        flash.deploy(Path("programmer"), "", image, "b1-h743", folder / "run")
                else:
                    flash.deploy(Path("programmer"), "", image, "b1-h743", folder / "run")
                    self.assertTrue((folder / "run/backup.sha256").is_file())
            return calls

    def test_backup_before_erase_and_readback_before_run(self):
        for target_output in (TARGET, TARGET_223):
            with self.subTest(target=target_output):
                calls = self.run_deploy(target_output=target_output)
                self.assertEqual(len(calls), 6)
                self.assertIn("-u", calls[2])
                self.assertIn("-w", calls[3])
                self.assertFalse(any("-e" in cmd for cmd in calls))
                self.assertIn("mode=Normal", calls[1])
                self.assertTrue(all("mode=HOTPLUG" in cmd for cmd in calls[2:]))
                self.assertIn("-v", calls[3])
                self.assertIn("-u", calls[4])
                self.assertIn("-rst", calls[5])
                self.assertFalse(any("-el" in cmd for cmd in calls))

    def test_bad_target_or_backup_never_erases(self):
        for failure in ("target", "capacity", "backup"):
            calls = self.run_deploy(failure)
            self.assertFalse(any("-e" in cmd or "-w" in cmd or "-rst" in cmd for cmd in calls))
            if failure in ("target", "capacity"):
                self.assertFalse(any("-u" in cmd for cmd in calls))

    def test_write_or_verify_failure_never_starts(self):
        for failure in ("write", "readback"):
            calls = self.run_deploy(failure)
            self.assertEqual(sum("-w" in cmd for cmd in calls), 1)
            self.assertFalse(any("-rst" in cmd for cmd in calls))

    def test_image_contract(self):
        with tempfile.TemporaryDirectory() as name:
            image=Path(name)/'app.bin'
            data=merged_image()
            write_image(image,data)
            self.assertEqual(flash.check_image(image,'b1-h743'),data)
            with self.assertRaises(RuntimeError): flash.check_image(image,'apollo-v2-h743')
            write_image(image,data,base=0x08020000)
            with self.assertRaises(RuntimeError): flash.check_image(image,'b1-h743')
            write_image(image,data)
            image.write_bytes(data+b'broken')
            with self.assertRaises(RuntimeError): flash.check_image(image,'b1-h743')
            write_image(image,data)
            elf=image.with_suffix('.elf'); elf.write_bytes(elf.read_bytes()[:-1]+b'!')
            with self.assertRaises(RuntimeError): flash.check_image(image,'b1-h743')

    def test_rejects_missing_or_invalid_app(self):
        with tempfile.TemporaryDirectory() as name:
            image = Path(name) / 'app.bin'
            for offset, value in ((0, 0x20020001), (4, 0x08020001),
                                  (flash.APP_OFFSET, 0x24020000),
                                  (flash.APP_OFFSET + 4, 0x08000001),
                                  (flash.APP_OFFSET + 4, 0x08020300),
                                  (flash.APP_OFFSET + 4, 0x08100001)):
                data = bytearray(merged_image())
                struct.pack_into('<I', data, offset, value)
                write_image(image, data)
                with self.subTest(offset=offset, value=value), self.assertRaises(RuntimeError):
                    flash.check_image(image, 'b1-h743')
            write_image(image, merged_image()[:flash.APP_OFFSET])
            with self.assertRaises(RuntimeError): flash.check_image(image, 'b1-h743')

    def test_wrong_image_never_opens_probe(self):
        with tempfile.TemporaryDirectory() as name, patch("direct_flash.run_cli") as tool:
            image = Path(name) / "ram.bin"
            write_image(image, struct.pack("<II", 0x20020000, 0x24000009) + b"app payload")
            with self.assertRaises(RuntimeError):
                flash.deploy(Path("programmer"), "", image, "b1-h743", Path(name) / "run")
            tool.assert_not_called()


if __name__ == "__main__":
    unittest.main()
