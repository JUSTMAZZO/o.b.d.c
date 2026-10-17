"""test_atomics_q_stores.py - Testes unitarios para atomicos exclusivos e instrucoes vetoriais Q."""
import unittest
import cfg_skeleton


def make_fn(asm_lines, name="test_fn", calls=None):
    return {
        "name": name,
        "entry_point": "00100000",
        "decompiled_c": "",
        "assembly": asm_lines,
        "resolved_data": {},
        "calls": calls or {},
        "loops": []
    }


def build_c(asm_lines, name="test_fn"):
    fn = make_fn(asm_lines, name)
    data = {"functions": [fn], "strings": {}, "calls": {}}
    return cfg_skeleton.build(fn, data, [])


class TestAtomicsAndQStores(unittest.TestCase):
    def test_01_exclusive_atomics_classify_and_emit(self):
        asm = [
            "00100000: ldaxr w0, [x1]",
            "00100004: stlxr w2, w0, [x1]",
            "00100008: clrex",
            "0010000c: ret"
        ]
        text = build_c(asm)

        self.assertIn("__atomic_load_n", text)
        self.assertIn("__ATOMIC_ACQUIRE", text)
        self.assertIn("__atomic_store_n", text)
        self.assertIn("__ATOMIC_RELEASE", text)
        self.assertIn("/* clrex */", text)

    def test_02_relaxed_atomics(self):
        asm = [
            "00100000: ldxr x0, [x1]",
            "00100004: stxr w2, x0, [x1]",
            "00100008: ret"
        ]
        text = build_c(asm)

        self.assertIn("__atomic_load_n", text)
        self.assertIn("__ATOMIC_RELAXED", text)
        self.assertIn("__atomic_store_n", text)

    def test_03_q_stores_and_loads(self):
        asm = [
            "00100000: movi v31.4S, #0",
            "00100004: str q31, [x0, #0x40]",
            "00100008: ldr q0, [x1]",
            "0010000c: str q0, [x2]",
            "00100010: str q0, [x3]",
            "00100014: ret"
        ]
        text = build_c(asm)

        self.assertIn("unsigned __int128", text)
        self.assertIn("q0", text)
        self.assertIn("*(unsigned __int128 *)", text)
        self.assertIn("*(unsigned __int128 *)((uint8_t *)a0 + 64) = 0;", text)

    def test_04_ldp_stp_q_registers(self):
        asm = [
            "00100000: ldp q0, q1, [x0]",
            "00100004: stp q0, q1, [x1]",
            "00100008: ret"
        ]
        text = build_c(asm)

        self.assertIn("q0 = *(unsigned __int128 *)", text)
        self.assertIn("q1 = *(unsigned __int128 *)", text)
        self.assertIn("*(unsigned __int128 *)", text)

    def test_05_vector_lane_operations(self):
        asm = [
            "00100000: mov v0.s[1], w1",
            "00100004: mov w0, v0.s[1]",
            "00100008: ret"
        ]
        text = build_c(asm)

        self.assertIn("((uint32_t *)&", text)
        self.assertIn("[1]", text)

    def test_06_auxiliary_instructions(self):
        asm = [
            "00100000: clz w0, w1",
            "00100004: clz x2, x3",
            "00100008: rbit w4, w5",
            "0010000c: rbit x6, x7",
            "00100010: add x0, x0, x2",
            "00100014: add x0, x0, x4",
            "00100018: add x0, x0, x6",
            "0010001c: ret"
        ]
        text = build_c(asm)

        self.assertIn("__builtin_clz(", text)
        self.assertIn("__builtin_clzll(", text)
        self.assertIn("__builtin_bitreverse32(", text)
        self.assertIn("__builtin_bitreverse64(", text)

    def test_07_udf_instruction(self):
        asm = [
            "00100000: udf #0",
            "00100004: ret"
        ]
        text = build_c(asm)

        self.assertIn("abort();", text)

    def test_08_ccmp_condition_short_circuit(self):
        asm = [
            "00100000: cmp x0, #0x0",
            "00100004: ccmp x1, #0x0, #0x4, ne",
            "00100008: b.eq 0x100014",
            "0010000c: mov w0, #0x1",
            "00100010: ret",
            "00100014: mov w0, #0x0",
            "00100018: ret"
        ]
        text = build_c(asm)

        self.assertIn("if (", text)
        self.assertNotIn("xzcv", text)  # nzcv nunca deve ser emitido como variavel xzcv


if __name__ == '__main__':
    unittest.main()
