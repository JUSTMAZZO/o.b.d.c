import unittest
import os
import subprocess
import tempfile
import cfg_skeleton
from horizon_rt import nro_tool

class TestStrictCAndBss(unittest.TestCase):
    def test_stack_struct_address_operator(self):
        """Verifica se acessos a structs na pilha usam operador de endereco &."""
        d = nro_tool.generate_disasm_json('horizon_rt/hb_services/hb_services.nro', elf_path='horizon_rt/hb_services/hb_services.elf')
        fn = next(f for f in d['functions'] if f['name'] == 'main')
        sk = cfg_skeleton.Skeleton(fn, d, [], shim_header='switch_hle_shim.h')
        text = '\n'.join(sk.function_text())
        self.assertIn('(uint8_t *)&sp_0x38', text)
        self.assertNotIn('(uint8_t *)sp_0x38', text)

    def test_open_service_parameter_typing(self):
        """Verifica se o parametro a1 em open_service e tipado como uint32_t a1."""
        d = nro_tool.generate_disasm_json('horizon_rt/hb_services/hb_services.nro', elf_path='horizon_rt/hb_services/hb_services.elf')
        fn = next(f for f in d['functions'] if f['name'] == 'open_service')
        sk = cfg_skeleton.Skeleton(fn, d, [], shim_header='switch_hle_shim.h')
        text = '\n'.join(sk.function_text())
        self.assertIn('open_service(void *a0, uint32_t a1)', text)
        self.assertNotIn('open_service(void *a0, void *a1)', text)

    def test_bss_pointer_stores_and_free(self):
        """Verifica se stores de ponteiro em BSS usam *(uintptr_t *) e free usa *(void **)."""
        d = nro_tool.generate_disasm_json('horizon_rt/hb_threads/hb_threads.nro', elf_path='horizon_rt/hb_threads/hb_threads.elf')
        fn = next(f for f in d['functions'] if f['name'] == 'main')
        sk = cfg_skeleton.Skeleton(fn, d, [], shim_header='switch_hle_shim.h')
        text = '\n'.join(sk.function_text())
        self.assertIn('*(uintptr_t *)(0x117ec0) = (uintptr_t)', text)
        self.assertIn('*(uintptr_t *)(0x117eb8) = (uintptr_t)', text)
        self.assertIn('free(*(void * *)(0x117ec0));', text)
        if os.path.exists('reconstructed_hb_threads.c'):
            with open('reconstructed_hb_threads.c', encoding='utf-8') as f:
                c_text = f.read()
            self.assertIn('*(uintptr_t *)(0x117ec0) = (uintptr_t)buf_x19;', c_text)
            self.assertIn('*(uintptr_t *)(0x117eb8) = (uintptr_t)buf_x0;', c_text)

    def test_canonical_shim_casts(self):
        """Verifica se chamadas a funcoes do shim tem casts canonicos explícitos."""
        d = nro_tool.generate_disasm_json('horizon_rt/hb_threads/hb_threads.nro', elf_path='horizon_rt/hb_threads/hb_threads.elf')
        fn = next(f for f in d['functions'] if f['name'] == 'main')
        sk = cfg_skeleton.Skeleton(fn, d, [], shim_header='switch_hle_shim.h')
        text = '\n'.join(sk.function_text())
        self.assertIn('threadCreate((Thread *)(b21), (ThreadFunc)((uintptr_t)(0x1004f0))', text)
        self.assertIn('threadStart((Thread *)(b21))', text)
        self.assertIn('mutexLock((Mutex *)((uintptr_t)(0x117ea8)))', text)
        self.assertIn('svcSignalProcessWideKey((u32 *)((uintptr_t)(0x117eb0))', text)
        self.assertIn('threadWaitForExit((Thread *)(b19))', text)
        self.assertIn('threadClose((Thread *)', text)

    def test_ndk_compilation_hb_threads_and_services(self):
        """Compila ambos reconstructed_hb_threads.c e reconstructed_hb_services.c com NDK r28 Clang -Werror."""
        ndk_clang = r"C:\Users\user\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android35-clang.cmd"
        if not os.path.exists(ndk_clang):
            self.skipTest("NDK Clang nao encontrado neste ambiente")

        for c_file in ('reconstructed_hb_threads.c', 'reconstructed_hb_services.c'):
            res = subprocess.run([ndk_clang, '-O3', '-Wall', '-Wextra', '-Werror', '-I', '.', '-c', c_file],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Falha ao compilar {c_file} com NDK Clang:\n{res.stderr}")

if __name__ == '__main__':
    unittest.main()
