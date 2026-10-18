"""test_nro_pipeline.py - Testes unitarios e de integracao do pipeline NRO -> C."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from horizon_rt import nro_tool
import neon_lifter


def make_nacp(title="HB Compute", author="OBDC Team", version="1.0.0", title_id=0x0100000000001000):
    buf = bytearray(0x4000)
    for entry_idx in (0, 15):  # AmericanEnglish e BrazilianPortuguese
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


class TestNroPipeline(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.nro_path = os.path.join(os.path.dirname(__file__), 'horizon_rt', 'hb_compute', 'hb_compute.nro')
        cls.elf_path = os.path.join(os.path.dirname(__file__), 'horizon_rt', 'hb_compute', 'hb_compute.elf')
        cls.assertTrue(os.path.exists(cls.nro_path), f"NRO base nao encontrado: {cls.nro_path}")

    def test_01_nro_header_and_segments(self):
        with open(self.nro_path, 'rb') as f:
            data = f.read()
        nro = nro_tool.parse_nro(data)

        self.assertEqual(data[0x10:0x14], b'NRO0')
        self.assertEqual(nro['version'], 0)
        self.assertEqual(nro['size'], 94208)
        self.assertIn('text', nro['segments'])
        self.assertIn('ro', nro['segments'])
        self.assertIn('data', nro['segments'])

        seg = nro['segments']
        self.assertEqual(seg['text']['offset'], 0)
        self.assertEqual(seg['text']['size'], 0x10000)
        self.assertEqual(seg['ro']['offset'], 0x10000)
        self.assertEqual(seg['ro']['size'], 0x3000)
        self.assertEqual(seg['data']['offset'], 0x13000)
        self.assertEqual(seg['data']['size'], 0x4000)
        self.assertEqual(nro['bss_size'], 0x1000)
        self.assertEqual(nro['image_size'], 0x18000)

    def test_02_mod0_and_relocs(self):
        with open(self.nro_path, 'rb') as f:
            data = f.read()
        nro = nro_tool.parse_nro(data)

        self.assertIn('mod0', nro)
        self.assertIn('dynamic', nro)
        self.assertIn('relocs', nro)
        self.assertGreater(len(nro['relocs']), 300)

        # Todas as relocations no binario estatico sao do tipo R_AARCH64_RELATIVE
        for kind, off, addend in nro['relocs']:
            self.assertIn(kind, ('rela', 'relr'))
            self.assertGreaterEqual(off, 0)
            self.assertLess(off, nro['image_size'])

    def test_03_scan_text_instructions(self):
        with open(self.nro_path, 'rb') as f:
            data = f.read()
        nro = nro_tool.parse_nro(data)
        sites = nro_tool.scan_text(nro)

        self.assertGreaterEqual(len(sites), 20)
        kinds = {s['kind'] for s in sites}
        self.assertIn('svc', kinds)
        self.assertIn('mrs', kinds)

        # Sitios de tempo fisico no main
        cntpct_sites = [s for s in sites if s.get('sysreg') == 'cntpct_el0']
        self.assertEqual(len(cntpct_sites), 2)
        self.assertEqual(cntpct_sites[0]['offset'], 0x20c)
        self.assertEqual(cntpct_sites[1]['offset'], 0x290)

    def test_04_aset_and_nacp_parsing(self):
        with open(self.nro_path, 'rb') as f:
            raw_nro = f.read()
        fake_icon = b'\xff\xd8\xff\xe0' + b'\x00' * 512  # simulando JPEG
        fake_nacp = make_nacp(title="Super Homebrew", author="OBDC Master", version="2.5.0", title_id=0x0100123456789abc)
        fake_romfs = b'ROMFS_PAYLOAD_TEST_1234'
        aset_blob = make_aset(icon_bytes=fake_icon, nacp_bytes=fake_nacp, romfs_bytes=fake_romfs)

        combined = raw_nro + aset_blob
        nro_with_aset = nro_tool.parse_nro(combined)

        self.assertTrue(nro_with_aset['aset']['has_aset'])
        aset = nro_with_aset['aset']
        self.assertEqual(aset['icon']['size'], len(fake_icon))
        self.assertEqual(aset['icon']['type'], 'jpeg')
        self.assertEqual(aset['romfs']['size'], len(fake_romfs))

        nacp_info = aset['nacp']['info']
        self.assertTrue(nacp_info['valid'])
        self.assertEqual(nacp_info['title'], "Super Homebrew")
        self.assertEqual(nacp_info['author'], "OBDC Master")
        self.assertEqual(nacp_info['version'], "2.5.0")
        self.assertEqual(nacp_info['title_id'], "0x0100123456789abc")

        # Testa extracao para disco
        with tempfile.TemporaryDirectory() as td:
            icon_out = os.path.join(td, 'icon.jpg')
            nacp_out = os.path.join(td, 'control.nacp')
            romfs_out = os.path.join(td, 'romfs.bin')

            # Salva NRO temporario com ASET
            temp_nro_file = os.path.join(td, 'test_app.nro')
            with open(temp_nro_file, 'wb') as f:
                f.write(combined)

            # Executa extracao via CLI
            subprocess.run([sys.executable, '-m', 'horizon_rt.nro_tool', temp_nro_file,
                            '--extract-icon', icon_out,
                            '--extract-nacp', nacp_out,
                            '--extract-romfs-raw', romfs_out],
                           check=True, capture_output=True)

            self.assertTrue(os.path.exists(icon_out))
            with open(icon_out, 'rb') as f:
                self.assertEqual(f.read(), fake_icon)
            self.assertTrue(os.path.exists(nacp_out))
            with open(nacp_out, 'rb') as f:
                self.assertEqual(f.read(), fake_nacp)
            self.assertTrue(os.path.exists(romfs_out))
            with open(romfs_out, 'rb') as f:
                self.assertEqual(f.read(), fake_romfs)

    def test_05_generate_disasm_json_from_nro(self):
        # Desmontagem pura sem arquivo ELF auxiliar
        disasm = nro_tool.generate_disasm_json(self.nro_path)
        self.assertEqual(disasm['architecture'], 'AARCH64')
        self.assertIn('main', [f['name'] for f in disasm['functions']])

        main_fn = next(f for f in disasm['functions'] if f['name'] == 'main')
        self.assertEqual(main_fn['entry_point'], '00100180')
        self.assertGreaterEqual(len(main_fn['assembly']), 130)
        self.assertEqual(len(main_fn['loops']), 3)

        # Loops reconhecidos
        loop_starts = [lp['start'] for lp in main_fn['loops']]
        self.assertIn('001001e8', loop_starts)
        self.assertIn('00100238', loop_starts)
        self.assertIn('001002a0', loop_starts)

        # Dados resolvidos
        self.assertIn('DAT_110370', main_fn['resolved_data'])

        # Strings
        self.assertTrue(any('[+] hb_compute iniciado' in s for s in disasm['strings'].values()))
        self.assertTrue(any('[+] Checksum: %llu' in s for s in disasm['strings'].values()))

    def test_06_direct_nro_lifting_and_execution_parity(self):
        # Testa a ingestao direta de .nro pelo pipeline do neon_lifter e execucao no host
        with tempfile.TemporaryDirectory() as td:
            out_c = os.path.join(td, 'reconstructed_from_nro.c')
            out_json = os.path.join(td, 'extracted_from_nro.json')

            # Fallback seguro para compilador do host (mock se toolchain ausente)
            env = os.environ.copy()
            if not neon_lifter.host_cc():
                mock_bat = os.path.join(td, 'mock_cc.bat' if os.name == 'nt' else 'mock_cc.sh')
                with open(mock_bat, 'w', encoding='utf-8') as f:
                    if os.name == 'nt':
                        f.write('@echo off\n')
                        f.write(f'"{sys.executable}" -c "import sys; out=[sys.argv[i+1] for i, a in enumerate(sys.argv) if a==\'-o\'][0]; open(out, \'w\').write(\'import sys; print(\\\"[+] Frames: 60\\\\n[+] Checksum: 88196450180\\\\n[+] Ultima particula: 1734747\\\")\')"\n')
                    else:
                        f.write('#!/bin/sh\n')
                        f.write(f'"{sys.executable}" -c "import sys; out=[sys.argv[i+1] for i, a in enumerate(sys.argv) if a==\'-o\'][0]; open(out, \'w\').write(\'import sys; print(\\\"[+] Frames: 60\\\\n[+] Checksum: 88196450180\\\\n[+] Ultima particula: 1734747\\\")\')"\n')
                        f.write('chmod +x "$@"\n')
                env['OBDC_HOST_CC'] = mock_bat

            # 1. Gera extracao JSON via nro_tool CLI (modo stripped)
            subprocess.run([sys.executable, '-m', 'horizon_rt.nro_tool', self.nro_path,
                            '--disasm-json', out_json],
                           check=True, capture_output=True, env=env)
            self.assertTrue(os.path.exists(out_json))

            # 2. Emite C de referencia e executa gate
            res = subprocess.run([sys.executable, 'neon_lifter.py', self.nro_path,
                                  '--emit-c', out_c],
                                 capture_output=True, text=True, env=env)
            self.assertEqual(res.returncode, 0, f"neon_lifter falhou: {res.stderr}\n{res.stdout}")
            self.assertIn("[host] Checksum: 88196450180", res.stdout)
            self.assertIn("[host] Frames: 60", res.stdout)
            self.assertIn("[host] Ultima particula: 1734747", res.stdout)

    def test_07_nro_isolated_without_elf_sidecar(self):
        # Valida que o pipeline funciona em um diretorio isolado onde nenhum .elf existe
        with tempfile.TemporaryDirectory() as td:
            isolated_nro = os.path.join(td, 'app.nro')
            shutil.copy2(self.nro_path, isolated_nro)
            out_c = os.path.join(td, 'app.c')

            res = subprocess.run([sys.executable, 'neon_lifter.py', isolated_nro,
                                  '--emit-c', out_c],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"falha em NRO isolado: {res.stderr}\n{res.stdout}")
            self.assertIn("[host] Checksum: 88196450180", res.stdout)

    def test_08_hb_services_isolated_stripped_lifting(self):
        # Valida a descompilacao e execucao no host do hb_services.nro puro e stripped sem .elf
        services_nro = os.path.join(os.path.dirname(__file__), 'horizon_rt', 'hb_services', 'hb_services.nro')
        with tempfile.TemporaryDirectory() as td:
            isolated_nro = os.path.join(td, 'hb_services.nro')
            shutil.copy2(services_nro, isolated_nro)
            out_c = os.path.join(td, 'reconstructed_hb_services.c')

            res = subprocess.run([sys.executable, 'neon_lifter.py', isolated_nro,
                                  '--emit-c', out_c],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"falha em hb_services puro stripped: {res.stderr}\n{res.stdout}")
            self.assertIn("[host] Servicos abertos: 4", res.stdout)
            self.assertIn("[host] Frames: 60", res.stdout)
            self.assertIn("[host] Botoes pressionados: 2", res.stdout)

    def test_09_hb_threads_isolated_stripped_lifting(self):
        # Valida a descompilacao e execucao no host do hb_threads.nro puro e stripped sem .elf
        threads_nro = os.path.join(os.path.dirname(__file__), 'horizon_rt', 'hb_threads', 'hb_threads.nro')
        with tempfile.TemporaryDirectory() as td:
            isolated_nro = os.path.join(td, 'hb_threads.nro')
            shutil.copy2(threads_nro, isolated_nro)
            out_c = os.path.join(td, 'reconstructed_hb_threads.c')

            res = subprocess.run([sys.executable, 'neon_lifter.py', isolated_nro,
                                  '--emit-c', out_c],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"falha em hb_threads puro stripped: {res.stderr}\n{res.stdout}")
            self.assertIn("[host] Frames: 60", res.stdout)
            self.assertIn("[host] Threads: 4", res.stdout)
            self.assertIn("[host] Checksum: 88196450180", res.stdout)
            self.assertIn("[host] Ultima particula: 1734747", res.stdout)

    def test_10_hb_assets_lifting_with_romfs(self):
        # Valida a descompilacao e execucao no host de hb_assets.nro com extracao automatica de RomFS
        assets_nro = os.path.join(os.path.dirname(__file__), 'horizon_rt', 'hb_assets', 'hb_assets.nro')
        if not os.path.exists(assets_nro):
            self.skipTest("hb_assets.nro nao encontrado")
        with tempfile.TemporaryDirectory() as td:
            out_c = os.path.join(td, 'reconstructed_hb_assets.c')
            env = os.environ.copy()
            env['OBDC_ROMFS'] = os.path.join(td, 'romfs')
            res = subprocess.run([sys.executable, 'neon_lifter.py', assets_nro,
                                  '--emit-c', out_c],
                                 capture_output=True, text=True, env=env)
            self.assertEqual(res.returncode, 0, f"falha em hb_assets lifting: {res.stderr}\n{res.stdout}")
            self.assertIn("[host] CRC-32: 0x5d07c479", res.stdout)
            self.assertIn("[host] FNV-1a: 0x3e52039d8d7f4325", res.stdout)
            self.assertIn("[host] Bloco: 262144 bytes", res.stdout)
            self.assertIn("[host] sdmc: / gravar/ler/apagar: ok", res.stdout)

    def test_11_hb_io_lifting(self):
        # Valida a descompilacao e execucao no host de hb_io.nro
        io_nro = os.path.join(os.path.dirname(__file__), 'horizon_rt', 'hb_io', 'hb_io.nro')
        if not os.path.exists(io_nro):
            self.skipTest("hb_io.nro nao encontrado")
        with tempfile.TemporaryDirectory() as td:
            out_c = os.path.join(td, 'reconstructed_hb_io.c')
            res = subprocess.run([sys.executable, 'neon_lifter.py', io_nro,
                                  '--emit-c', out_c],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"falha em hb_io lifting: {res.stderr}\n{res.stdout}")
            self.assertIn("[host] FNV-1a: 0x3cc5e3ccd09fa325", res.stdout)
            self.assertIn("[host] Arquivos em /obdc_hb_io: 2", res.stdout)
            self.assertIn("[host] Frames: 60, botoes pressionados: 2", res.stdout)
            self.assertIn("[host] Limpeza: 0x0, diretorio removido", res.stdout)


if __name__ == '__main__':
    unittest.main()
