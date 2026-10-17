#!/usr/bin/env python3
"""
Teste unitário para a tradução da instrução AArch64:
  mrs xN, tpidrro_el0 -> SHIM_MRS_TPIDRRO(xN);
"""
import unittest
import cfg_skeleton


class TestMrsTls(unittest.TestCase):
    def test_mrs_tpidrro_x0(self):
        fn = {
            'name': 'test_x0',
            'calls': {},
            'assembly': [
                '00100000: mrs x0, tpidrro_el0',
                '00100004: ret'
            ]
        }
        data = {'functions': [fn], 'strings': {}}
        c_code = cfg_skeleton.build(fn, data, [])
        self.assertIn('SHIM_MRS_TPIDRRO(x0);', c_code)
        self.assertIn('uint64_t x0;', c_code)

    def test_mrs_tpidrro_different_registers(self):
        for reg in ('x1', 'x8', 'x20'):
            with self.subTest(reg=reg):
                fn = {
                    'name': f'test_{reg}',
                    'calls': {},
                    'assembly': [
                        f'00100000: mrs {reg}, tpidrro_el0',
                        '00100004: ret'
                    ]
                }
                data = {'functions': [fn], 'strings': {}}
                c_code = cfg_skeleton.build(fn, data, [])
                self.assertIn(f'SHIM_MRS_TPIDRRO({reg});', c_code)
                self.assertIn(f'uint64_t {reg};', c_code)

    def test_mrs_tpidrro_case_insensitivity(self):
        fn = {
            'name': 'test_case',
            'calls': {},
            'assembly': [
                '00100000: MRS X8, TPIDRRO_EL0',
                '00100004: ret'
            ]
        }
        data = {'functions': [fn], 'strings': {}}
        c_code = cfg_skeleton.build(fn, data, [])
        self.assertIn('SHIM_MRS_TPIDRRO(x8);', c_code)
        self.assertIn('uint64_t x8;', c_code)

    def test_mrs_tpidrro_with_use(self):
        fn = {
            'name': 'test_use',
            'calls': {},
            'assembly': [
                '00100000: mrs x0, tpidrro_el0',
                '00100004: ldr x1, [x0, #0x18]',
                '00100008: add x0, x1, #1',
                '0010000c: ret'
            ]
        }
        data = {'functions': [fn], 'strings': {}}
        c_code = cfg_skeleton.build(fn, data, [])
        self.assertIn('SHIM_MRS_TPIDRRO(x0);', c_code)
        self.assertIn('uint64_t x0;', c_code)


if __name__ == '__main__':
    unittest.main()
