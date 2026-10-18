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
        self.assertIn('threadCreate((Thread *)(b21), sub_1004f0', text)
        self.assertIn('threadStart((Thread *)(b21))', text)
        self.assertIn('mutexLock((Mutex *)((uintptr_t)(0x117ea8)))', text)
        self.assertIn('svcSignalProcessWideKey((u32 *)((uintptr_t)(0x117eb0))', text)
        self.assertIn('threadWaitForExit((Thread *)(b19))', text)
        self.assertIn('threadClose((Thread *)', text)

    def test_worker_func_emission(self):
        """Verifica se sub_1004f0 e emitida com range [start, end), buffers BSS e operacoes atomicas."""
        d = nro_tool.generate_disasm_json('horizon_rt/hb_threads/hb_threads.nro', elf_path='horizon_rt/hb_threads/hb_threads.elf')
        fn = next(f for f in d['functions'] if f['name'] == 'main')
        text = cfg_skeleton.build(fn, d, [])
        self.assertIn('static void sub_1004f0(void *a0) {', text)
        self.assertIn('start = *(uint32_t *)a0;', text)
        self.assertIn('end = *(uint32_t *)((uint8_t *)a0 + 4);', text)
        self.assertIn('buf_x0 = *(uint32_t **)(uintptr_t)0x117eb8;', text)
        self.assertIn('buf_x19 = *(uint32_t **)(uintptr_t)0x117ec0;', text)
        self.assertIn('__atomic_fetch_add((uint64_t *)(uintptr_t)0x117ec8, sum, __ATOMIC_SEQ_CST);', text)
        self.assertIn('__atomic_fetch_add((uint32_t *)(uintptr_t)0x117ed0, 1, __ATOMIC_SEQ_CST);', text)
        self.assertIn('*(uint32_t *)(uintptr_t)0x117ed0', text)
        self.assertIn('*(uint64_t *)(uintptr_t)0x117ec8', text)
        self.assertIn('*(uint32_t *)((uint8_t *)(*(uintptr_t *)(uintptr_t)0x117ec0) + 399996)', text)

    def test_ndk_compilation_hb_threads_and_services(self):
        """Compila ambos reconstructed_hb_threads.c e reconstructed_hb_services.c com NDK r28 Clang -Werror."""
        ndk_clang = r"C:\Users\user\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android35-clang.cmd"
        if not os.path.exists(ndk_clang):
            self.skipTest("NDK Clang nao encontrado neste ambiente")

        for c_file in ('reconstructed_hb_threads.c', 'reconstructed_hb_services.c'):
            res = subprocess.run([ndk_clang, '-O3', '-Wall', '-Wextra', '-Werror', '-I', '.', '-c', '-o', 'nul', c_file],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Falha ao compilar {c_file} com NDK Clang:\n{res.stderr}")

    def test_generic_applethook_callback_inference(self):
        """Valida que o callback de appletHook tem assinatura e tipo inferidos genericamente, sem checar on_applet."""
        import json
        with open('ghidra_extraction_hb_fb.json', 'r', encoding='utf-8') as f:
            d = json.load(f)
        for fn in d['functions']:
            if fn['name'] == 'on_applet':
                fn['name'] = 'custom_hook_cb'
        main_fn = next(fn for fn in d['functions'] if fn['name'] == 'main')
        text = cfg_skeleton.build(main_fn, d, [])
        self.assertIn('static void custom_hook_cb(AppletHookType hook, void *param)', text)
        self.assertIn('(void)param;', text)
        self.assertIn('appletHook(&sp_0x90, custom_hook_cb, 0);', text)

    def test_generic_applethook_decompiled_c_casts_and_ampersand(self):
        """Garante inferência de callbacks com operador & e typecasts em decompiled_c."""
        import json
        with open('ghidra_extraction_hb_fb.json', 'r', encoding='utf-8') as f:
            d = json.load(f)
        for fn in d['functions']:
            if fn['name'] == 'on_applet':
                fn['name'] = 'hook_cb_casted'
        main_fn = next(fn for fn in d['functions'] if fn['name'] == 'main')
        main_fn['decompiled_c'] = main_fn['decompiled_c'].replace('on_applet', '(AppletHookFn)&hook_cb_casted')
        text = cfg_skeleton.build(main_fn, d, [])
        self.assertIn('static void hook_cb_casted(AppletHookType hook, void *param)', text)
        self.assertIn('(void)param;', text)

    def test_generic_applethook_assembly_register_mov(self):
        """Garante inferência de callbacks quando o endereço é movido via registradores (ex: mov x1, x19)."""
        d = {
            'functions': [
                {
                    'name': 'main',
                    'entry_point': '0x100000',
                    'assembly': [
                        '00100004: adrp x19,0x100000',
                        '00100008: add x19,x19,#0x500',
                        '0010000c: mov x1, x19',
                        '00100010: bl appletHook'
                    ]
                },
                {
                    'name': 'reg_cb',
                    'entry_point': '0x100500',
                    'assembly': ['00100500: ret']
                }
            ]
        }
        cb_eps, cb_names = cfg_skeleton.find_applet_hook_callbacks(d)
        self.assertIn(0x100500, cb_eps)
        self.assertIn('reg_cb', cb_names)

if __name__ == '__main__':
    unittest.main()


