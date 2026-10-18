"""test_nro_aset.py - Testes unitarios da extracao de RomFS e secao ASET em nro_tool.py (Fase 4)."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

from horizon_rt import nro_tool


def make_nacp(title="HB Assets Test", author="OBDC Team", version="1.0.0", title_id=0x0100000000002000):
    buf = bytearray(0x4000)
    for entry_idx in (0, 15):
        off = entry_idx * 0x300
        t_bytes = title.encode('utf-8')[:0x200]
        buf[off:off + len(t_bytes)] = t_bytes
        a_bytes = author.encode('utf-8')[:0x100]
        buf[off + 0x200:off + 0x200 + len(a_bytes)] = a_bytes
    v_bytes = version.encode('ascii')[:16]
    buf[0x3060:0x3060 + len(v_bytes)] = v_bytes
    struct.pack_into('<Q', buf, 0x3070, title_id)
    struct.pack_into('<Q', buf, 0x3078, title_id)
    return bytes(buf)


def make_aset(icon_bytes=b'', nacp_bytes=b'', romfs_bytes=b''):
    header = bytearray(0x38)
    header[0:4] = b'ASET'
    struct.pack_into('<I', header, 4, 0)

    cur_off = 0x38
    struct.pack_into('<QQ', header, 0x08, cur_off, len(icon_bytes))
    cur_off += len(icon_bytes)

    struct.pack_into('<QQ', header, 0x18, cur_off, len(nacp_bytes))
    cur_off += len(nacp_bytes)

    struct.pack_into('<QQ', header, 0x28, cur_off, len(romfs_bytes))

    return bytes(header) + icon_bytes + nacp_bytes + romfs_bytes


def make_synthetic_nro_header(size=0x1000):
    header = bytearray(size)
    header[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', header, 0x14, 0)
    struct.pack_into('<I', header, 0x18, size)
    # MOD0 offset em 0x40
    struct.pack_into('<I', header, 0x04, 0x40)
    header[0x40:0x44] = b'MOD0'
    # MOD0 fields (dynamic, bss_start, bss_end, eh_frame_hdr_start, eh_frame_hdr_end, module_object)
    struct.pack_into('<6i', header, 0x44, 0x20, 0x100, 0x100, 0, 0, 0)
    # dynamic section at 0x60
    struct.pack_into('<qQ', header, 0x60, 0, 0)
    return bytes(header)


def pack_synthetic_romfs(files):
    dir_tree = {'': {'dirs': {}, 'files': {}, 'name': ''}}
    for path, data in sorted(files.items()):
        parts = path.replace('\\', '/').strip('/').split('/')
        cur = dir_tree['']
        for p in parts[:-1]:
            if p not in cur['dirs']:
                cur['dirs'][p] = {'dirs': {}, 'files': {}, 'name': p}
            cur = cur['dirs'][p]
        cur['files'][parts[-1]] = data

    all_dirs = []

    def collect_dirs(node):
        all_dirs.append(node)
        for cname in sorted(node['dirs'].keys()):
            collect_dirs(node['dirs'][cname])

    collect_dirs(dir_tree[''])

    dir_offsets = {}
    cur_d_off = 0
    for d in all_dirs:
        dir_offsets[id(d)] = cur_d_off
        nlen = len(d['name'].encode('utf-8'))
        cur_d_off += 24 + ((nlen + 3) & ~3)

    all_files = []
    file_offsets = {}
    cur_f_off = 0
    data_bytes = bytearray()
    file_data_info = {}

    for d in all_dirs:
        d['file_list'] = []
        for fname in sorted(d['files'].keys()):
            fdata = d['files'][fname]
            f_info = {'name': fname, 'dir_id': id(d), 'size': len(fdata)}
            d['file_list'].append(f_info)
            all_files.append(f_info)
            file_offsets[id(f_info)] = cur_f_off
            cur_f_off += 32 + ((len(fname.encode('utf-8')) + 3) & ~3)

            doff = len(data_bytes)
            data_bytes.extend(fdata)
            pad = (16 - (len(data_bytes) % 16)) % 16
            data_bytes.extend(b'\x00' * pad)
            file_data_info[id(f_info)] = (doff, len(fdata))

    file_meta_table = bytearray()
    for d in all_dirs:
        flist = d['file_list']
        for i, f_info in enumerate(flist):
            sib_off = file_offsets[id(flist[i + 1])] if i + 1 < len(flist) else 0xFFFFFFFF
            doff, dsz = file_data_info[id(f_info)]
            nbytes = f_info['name'].encode('utf-8')
            pad = ((len(nbytes) + 3) & ~3) - len(nbytes)
            entry = struct.pack('<IIQQII', dir_offsets[d['parent_id']] if 'parent_id' in d else 0,
                                sib_off, doff, dsz, 0xFFFFFFFF, len(nbytes)) + nbytes + b'\x00' * pad
            file_meta_table.extend(entry)

    for d in all_dirs:
        children = [d['dirs'][k] for k in sorted(d['dirs'].keys())]
        for c in children:
            c['parent_id'] = id(d)

    dir_meta_table = bytearray()
    for d in all_dirs:
        children = [d['dirs'][k] for k in sorted(d['dirs'].keys())]
        child_dir_off = dir_offsets[id(children[0])] if children else 0xFFFFFFFF
        child_file_off = file_offsets[id(d['file_list'][0])] if d['file_list'] else 0xFFFFFFFF
        parent_off = dir_offsets[d['parent_id']] if 'parent_id' in d else 0
        sib_off = 0xFFFFFFFF
        if 'parent_id' in d:
            p_node = next(n for n in all_dirs if id(n) == d['parent_id'])
            p_children = [p_node['dirs'][k] for k in sorted(p_node['dirs'].keys())]
            idx = p_children.index(d)
            if idx + 1 < len(p_children):
                sib_off = dir_offsets[id(p_children[idx + 1])]

        nbytes = d['name'].encode('utf-8')
        pad = ((len(nbytes) + 3) & ~3) - len(nbytes)
        entry = struct.pack('<6I', parent_off, sib_off, child_dir_off, child_file_off, 0xFFFFFFFF, len(nbytes)) + nbytes + b'\x00' * pad
        dir_meta_table.extend(entry)

    data_offset = 0x200
    dir_hash_off = data_offset + len(data_bytes)
    dir_hash_sz = 12
    dir_meta_off = dir_hash_off + dir_hash_sz
    dir_meta_sz = len(dir_meta_table)
    file_hash_off = dir_meta_off + dir_meta_sz
    file_hash_sz = 12
    file_meta_off = file_hash_off + file_hash_sz
    file_meta_sz = len(file_meta_table)

    header = struct.pack('<10Q', 0x50, dir_hash_off, dir_hash_sz, dir_meta_off, dir_meta_sz,
                         file_hash_off, file_hash_sz, file_meta_off, file_meta_sz, data_offset)

    romfs_bytes = bytearray()
    romfs_bytes.extend(header)
    romfs_bytes.extend(b'\x00' * (data_offset - len(header)))
    romfs_bytes.extend(data_bytes)
    romfs_bytes.extend(b'\xff' * dir_hash_sz)
    romfs_bytes.extend(dir_meta_table)
    romfs_bytes.extend(b'\xff' * file_hash_sz)
    romfs_bytes.extend(file_meta_table)
    return bytes(romfs_bytes)


class TestNroAset(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base_dir = os.path.dirname(os.path.abspath(__file__))
        cls.hb_assets_dir = os.path.join(cls.base_dir, 'horizon_rt', 'hb_assets')
        cls.hb_assets_nro = os.path.join(cls.hb_assets_dir, 'hb_assets.nro')

        # Se hb_assets.nro nao existir, assegurar geracao automatica
        if not os.path.exists(cls.hb_assets_nro):
            cls._build_hb_assets_nro()

        cls.assertTrue(os.path.exists(cls.hb_assets_nro), f"hb_assets.nro nao encontrado: {cls.hb_assets_nro}")

    @classmethod
    def _build_hb_assets_nro(cls):
        # 1. Gera romfs local
        subprocess.run([sys.executable, os.path.join(cls.hb_assets_dir, 'gen_romfs.py')],
                       cwd=cls.hb_assets_dir, check=True, capture_output=True)

        build_romfs = r'C:\devkitPro\tools\bin\build_romfs.exe'
        elf2nro = r'C:\devkitPro\tools\bin\elf2nro.exe'
        gcc = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc.exe'

        if os.path.exists(build_romfs) and os.path.exists(elf2nro) and os.path.exists(gcc):
            env = os.environ.copy()
            env['DEVKITPRO'] = r'C:\devkitPro'
            env['PATH'] = r'C:\devkitPro\devkitA64\bin;C:\devkitPro\tools\bin;' + env.get('PATH', '')

            tmp_o = os.path.join(cls.hb_assets_dir, 'hb_assets.o')
            tmp_elf = os.path.join(cls.hb_assets_dir, 'hb_assets.elf')
            tmp_romfs = os.path.join(cls.hb_assets_dir, 'romfs.img')

            try:
                subprocess.run([gcc, '-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE',
                                '-g', '-Wall', '-Wextra', '-O2', '-ffunction-sections', '-D__SWITCH__',
                                r'-IC:\devkitPro\libnx\include', '-c', 'hb_assets.c', '-o', tmp_o],
                               cwd=cls.hb_assets_dir, env=env, check=True, capture_output=True)
                subprocess.run([gcc, r'-specs=C:\devkitPro\libnx\switch.specs', '-g', '-march=armv8-a+crc+crypto',
                                '-mtune=cortex-a57', '-mtp=soft', '-fPIE', tmp_o,
                                r'-LC:\devkitPro\libnx\lib', '-lnx', '-o', tmp_elf],
                               cwd=cls.hb_assets_dir, env=env, check=True, capture_output=True)
                subprocess.run([build_romfs, os.path.join(cls.hb_assets_dir, 'romfs'), tmp_romfs],
                               cwd=cls.hb_assets_dir, env=env, check=True, capture_output=True)
                subprocess.run([elf2nro, tmp_elf, cls.hb_assets_nro, f'--romfs={tmp_romfs}'],
                               cwd=cls.hb_assets_dir, env=env, check=True, capture_output=True)
            finally:
                for tmp in (tmp_o, tmp_elf, tmp_romfs):
                    if os.path.exists(tmp):
                        try:
                            os.remove(tmp)
                        except OSError:
                            pass
        else:
            # Fallback em Python puro construindo o NRO com ASET
            bloco_data = bytes(((i * 167 + 13) ^ (i >> 9)) & 0xFF for i in range(256 * 1024))
            info_data = b"o.b.d.c hb_assets: RomFS de teste\n"
            romfs_blob = pack_synthetic_romfs({'data/bloco.bin': bloco_data, 'texto/info.txt': info_data})
            aset_blob = make_aset(romfs_bytes=romfs_blob)
            base_nro_path = os.path.join(cls.base_dir, 'horizon_rt', 'hb_compute', 'hb_compute.nro')
            if os.path.exists(base_nro_path):
                with open(base_nro_path, 'rb') as f:
                    base_nro = f.read()
            else:
                base_nro = make_synthetic_nro_header(0x10000)
            with open(cls.hb_assets_nro, 'wb') as f:
                f.write(base_nro + aset_blob)

    def test_01_find_aset_section_hb_assets_nro(self):
        """Valida que find_aset_section detecta e valida a secao ASET no hb_assets.nro."""
        with open(self.hb_assets_nro, 'rb') as f:
            data = f.read()

        aset = nro_tool.find_aset_section(data)
        self.assertIsNotNone(aset, "find_aset_section deve retornar um dict para hb_assets.nro")
        self.assertTrue(aset['has_aset'])
        self.assertEqual(aset['version'], 0)
        self.assertGreater(aset['romfs']['size'], 0)
        self.assertIn('data', aset['romfs'])
        self.assertEqual(len(aset['romfs']['data']), aset['romfs']['size'])

    def test_02_extract_romfs_image_hb_assets_nro(self):
        """Valida a extracao hierarquica da particao RomFS de hb_assets.nro e confere integridade."""
        with open(self.hb_assets_nro, 'rb') as f:
            data = f.read()

        aset = nro_tool.find_aset_section(data)
        self.assertIsNotNone(aset)
        romfs_bytes = aset['romfs']['data']

        with tempfile.TemporaryDirectory() as td:
            extracted = nro_tool.extract_romfs_image(romfs_bytes, td)
            self.assertIsInstance(extracted, list)
            self.assertGreaterEqual(len(extracted), 2)

            bloco_path = os.path.join(td, 'data', 'bloco.bin')
            info_path = os.path.join(td, 'texto', 'info.txt')

            self.assertTrue(os.path.exists(bloco_path), f"Arquivo nao encontrado no disco: {bloco_path}")
            self.assertTrue(os.path.exists(info_path), f"Arquivo nao encontrado no disco: {info_path}")

            # Verifica casamento tanto com caminho relativo quanto absoluto/normalizado
            self.assertTrue(any('bloco.bin' in f for f in extracted))
            self.assertTrue(any('info.txt' in f for f in extracted))
            self.assertIn('data/bloco.bin', extracted)
            self.assertIn('texto/info.txt', extracted)

            # Confere integridade exata dos dados (oraculo: gen_romfs.py / hb_assets.c)
            with open(bloco_path, 'rb') as f:
                bloco_data = f.read()
            self.assertEqual(len(bloco_data), 262144)
            self.assertEqual(zlib.crc32(bloco_data), 0x5d07c479)
            self.assertEqual(bloco_data[0x10000:0x10004].hex(), '8d34db82')

            with open(info_path, 'rb') as f:
                info_data = f.read()
            self.assertEqual(info_data, b"o.b.d.c hb_assets: RomFS de teste\n")

    def test_03_cli_extract_romfs_custom_dir(self):
        """Testa o comando CLI com --extract-romfs <dir_customizado>."""
        with tempfile.TemporaryDirectory() as td:
            res = subprocess.run([sys.executable, '-m', 'horizon_rt.nro_tool', self.hb_assets_nro,
                                  '--extract-romfs', td],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"nro_tool CLI falhou: {res.stderr}\n{res.stdout}")
            self.assertIn("[nro_tool] RomFS extraido", res.stdout)

            bloco = os.path.join(td, 'data', 'bloco.bin')
            info = os.path.join(td, 'texto', 'info.txt')
            self.assertTrue(os.path.exists(bloco))
            self.assertTrue(os.path.exists(info))

            with open(bloco, 'rb') as f:
                self.assertEqual(zlib.crc32(f.read()), 0x5d07c479)

    def test_04_cli_extract_romfs_default_dir(self):
        """Testa o comando CLI com flag --extract-romfs sem argumento (padrao: ./romfs)."""
        env = os.environ.copy()
        env['PYTHONPATH'] = self.base_dir
        with tempfile.TemporaryDirectory() as work_dir:
            res = subprocess.run([sys.executable, '-m', 'horizon_rt.nro_tool', self.hb_assets_nro,
                                  '--extract-romfs'],
                                 cwd=work_dir, env=env, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"nro_tool CLI falhou: {res.stderr}\n{res.stdout}")

            expected_romfs_dir = os.path.join(work_dir, 'romfs')
            self.assertTrue(os.path.exists(expected_romfs_dir))
            bloco = os.path.join(expected_romfs_dir, 'data', 'bloco.bin')
            info = os.path.join(expected_romfs_dir, 'texto', 'info.txt')
            self.assertTrue(os.path.exists(bloco))
            self.assertTrue(os.path.exists(info))

    def test_05_synthetic_nro_without_aset_non_blocking(self):
        """Testa caso de borda: NRO sintético sem ASET nao deve crashar e deve emitir aviso nao bloqueante."""
        # Cria NRO valido removendo o trailer ASET
        with open(self.hb_assets_nro, 'rb') as f:
            full_data = f.read()
        nro_meta = nro_tool.parse_nro(full_data)
        synthetic_nro = full_data[:nro_meta['size']]

        # 1. find_aset_section retorna None
        self.assertIsNone(nro_tool.find_aset_section(synthetic_nro))

        # 2. parse_aset retorna has_aset: False
        aset_res = nro_tool.parse_aset(synthetic_nro)
        self.assertFalse(aset_res['has_aset'])

        # 3. CLI nao crasha com NRO sem ASET e emite aviso nao bloqueante
        env = os.environ.copy()
        env['PYTHONPATH'] = self.base_dir
        with tempfile.TemporaryDirectory() as td:
            dummy_nro_file = os.path.join(td, 'no_aset.nro')
            with open(dummy_nro_file, 'wb') as f:
                f.write(synthetic_nro)

            res = subprocess.run([sys.executable, '-m', 'horizon_rt.nro_tool', dummy_nro_file,
                                  '--extract-romfs'],
                                 cwd=td, env=env, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"CLI nao deve crashar com NRO sem ASET: {res.stderr}")
            self.assertIn("[nro_tool] NRO sem secao ASET/RomFS", res.stdout)

    def test_06_edge_cases_corrupted_and_truncated_romfs(self):
        """Testa robustez de extract_romfs_image e find_aset_section com entradas corrompidas ou vazias."""
        with tempfile.TemporaryDirectory() as td:
            # Entrada vazia
            self.assertEqual(nro_tool.extract_romfs_image(b'', td), [])
            self.assertIsNone(nro_tool.find_aset_section(b''))

            # Entrada menor que o cabecalho de 80 bytes
            self.assertEqual(nro_tool.extract_romfs_image(b'NOT_A_VALID_ROMFS_BUFFER', td), [])
            self.assertIsNone(nro_tool.find_aset_section(b'ASET_TOO_SHORT'))

            # Cabecalho RomFS zerado
            self.assertEqual(nro_tool.extract_romfs_image(b'\x00' * 80, td), [])

            # Cabecalho com offsets que extrapolam o tamanho total do buffer
            corrupt_header = struct.pack('<10Q', 0x50, 0x100000, 100, 0x200000, 100,
                                         0x300000, 100, 0x400000, 100, 0x500000)
            self.assertEqual(nro_tool.extract_romfs_image(corrupt_header, td), [])

    def test_07_synthetic_multi_level_romfs_hierarchy(self):
        """Valida extracao de estruturas hierarquicas complexas (subpastas aninhadas e multiplos arquivos)."""
        test_files = {
            'nivel1/arquivo_a.txt': b"Conteudo nivel 1 A",
            'nivel1/subnivel2/arquivo_b.bin': b"\x01\x02\x03\x04\x05\xaa\xbb\xcc",
            'nivel1/subnivel2/fundo/arquivo_c.dat': b"Profundidade 3 de teste",
            'raiz.txt': b"Arquivo diretamente na raiz do RomFS"
        }
        romfs_blob = pack_synthetic_romfs(test_files)
        with tempfile.TemporaryDirectory() as td:
            extracted = nro_tool.extract_romfs_image(romfs_blob, td)
            self.assertEqual(len(extracted), 4)

            for rel_path, expected_content in test_files.items():
                self.assertIn(rel_path, extracted)
                full_p = os.path.join(td, rel_path)
                self.assertTrue(os.path.exists(full_p), f"Falta extrair: {full_p}")
                with open(full_p, 'rb') as f:
                    self.assertEqual(f.read(), expected_content)

    def test_08_path_traversal_protection(self):
        """Garante que entradas com tentativa de directory traversal ('..') sao bloqueadas."""
        malicious_files = {
            'normal.txt': b"normal",
            '../escaped.txt': b"tentativa de saida da raiz",
        }
        romfs_blob = pack_synthetic_romfs(malicious_files)
        with tempfile.TemporaryDirectory() as td:
            target_sub = os.path.join(td, 'sub')
            extracted = nro_tool.extract_romfs_image(romfs_blob, target_sub)
            # O arquivo 'escaped.txt' fora de target_sub nao deve ser escrito
            escaped_path = os.path.join(td, 'escaped.txt')
            self.assertFalse(os.path.exists(escaped_path), "Seguranca violada: arquivo gravado fora de target_sub")


if __name__ == '__main__':
    unittest.main()
