"""离线验证RK前级投放、换版本及错误文件拒绝；不运行前级或访问板卡。"""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_wsl as pack

OUTPUT = pack.BUILD / 'handoff/tests/loader-input'


class LoaderInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        path, _ = pack.LOADERS['normal_d4_loader_h1']
        cls.fixture = pack.wsl_path(path).read_bytes()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='case-', dir=OUTPUT)
        self.edk = Path(self.temporary.name).resolve()
        # 清理范围仅限本测试刚创建的目录，不能触及真实前级或EDK工程。
        self.assertEqual(self.edk.parent, OUTPUT.resolve())
        self.addCleanup(self.temporary.cleanup)
        self.directory = self.edk / pack.RK_LOADER_DIRECTORY
        self.directory.mkdir(parents=True)

    def read_rk(self):
        return pack.read_loaders(self.edk, [pack.VARIANTS['emmc-rk'], pack.VARIANTS['spinor-rk']])

    def test_original_name_and_subdirectory_backup(self):
        path = self.directory / 'RK 新前级 20261110.BIN'
        path.write_bytes(self.fixture)
        archive = self.directory / 'archive'
        archive.mkdir()
        (archive / 'old.bin').write_bytes(self.fixture)
        (self.directory / '说明.txt').write_text('只选择当前目录的bin', encoding='utf-8')
        data, sources = self.read_rk()
        self.assertEqual(list(data), [pack.RK_LOADER_KEY])
        self.assertEqual(data[pack.RK_LOADER_KEY], self.fixture)
        self.assertEqual(sources[pack.RK_LOADER_KEY]['path'], str(path.resolve()))

    def test_replacement_records_new_hash_without_configuration(self):
        path = self.directory / 'current.bin'
        path.write_bytes(self.fixture)
        _, before = self.read_rk()
        # 人工修改仅作为离线样本，证明新散列可接受，不作为可烧录前级。
        updated = bytearray(self.fixture)
        updated[-5] ^= 1
        updated[-4:] = pack.checksum(updated[:-4]).to_bytes(4, 'little')
        path.write_bytes(updated)
        data, after = self.read_rk()
        self.assertNotEqual(before[pack.RK_LOADER_KEY]['sha256'], after[pack.RK_LOADER_KEY]['sha256'])
        self.assertEqual(after[pack.RK_LOADER_KEY]['sha256'], pack.sha(updated))
        self.assertEqual(data[pack.RK_LOADER_KEY], updated)

    def test_missing_and_ambiguous_input(self):
        with self.assertRaisesRegex(ValueError, '请将当前RK完整Loader'):
            self.read_rk()
        for name in ('old.bin', 'new.bin'):
            (self.directory / name).write_bytes(self.fixture)
        with self.assertRaisesRegex(ValueError, '只能保留一份'):
            self.read_rk()

    def test_invalid_header_and_crc(self):
        path = self.directory / 'current.bin'
        for value in (b'', b'bare spl without loader header', self.fixture[:-1],
                      self.fixture[:-5] + bytes([self.fixture[-5] ^ 1]) + self.fixture[-4:]):
            with self.subTest(length=len(value)):
                path.write_bytes(value)
                with self.assertRaisesRegex(ValueError, '完整Loader|CRC错误'):
                    self.read_rk()

    def test_d4_does_not_require_rk_input(self):
        data, sources = pack.read_loaders(self.edk, [pack.VARIANTS['emmc-d4']])
        self.assertEqual(list(data), ['normal_d4_loader_h1'])
        self.assertEqual(data['normal_d4_loader_h1'], self.fixture)
        self.assertEqual(sources['normal_d4_loader_h1']['sha256'], pack.sha(self.fixture))


if __name__ == '__main__':
    unittest.main()
