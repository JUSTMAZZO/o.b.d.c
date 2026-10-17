#!/usr/bin/env bash
set -e

if ! command -v clang &> /dev/null; then
    for d in ../tools/llvm-mingw-*/bin; do
        if [ -d "$d" ]; then
            export PATH="$d:$PATH"
            break
        fi
    done
fi

CFLAGS="-O2 -pthread"

echo "=== Verificando Regressão do Lifter nos 5 JSONs de Referência ==="

# 1. array
python neon_lifter.py ghidra_extraction_array.json --emit-c test_array.c >/dev/null
clang $CFLAGS test_array.c -o test_array_bin
OUT_ARRAY=$(./test_array_bin)
echo "$OUT_ARRAY" | grep -q "12249998500128" && echo "✅ array: OK" || (echo "❌ array: FALHOU ($OUT_ARRAY)" && exit 1)

# 2. hybrid
python neon_lifter.py ghidra_extraction_hybrid.json --emit-c test_hybrid.c >/dev/null
clang $CFLAGS test_hybrid.c -o test_hybrid_bin
OUT_HYBRID=$(./test_hybrid_bin)
echo "$OUT_HYBRID" | grep -q "151550809095744" && echo "✅ hybrid: OK" || (echo "❌ hybrid: FALHOU ($OUT_HYBRID)" && exit 1)

# 3. aos (esperado: 60 frames e checksum 88196450180)
python neon_lifter.py ghidra_extraction_aos.json --emit-c test_aos.c >/dev/null
clang $CFLAGS test_aos.c -o test_aos_bin
OUT_AOS=$(./test_aos_bin)
echo "$OUT_AOS" | grep -q "88196450180" && echo "✅ aos: OK" || (echo "❌ aos: FALHOU ($OUT_AOS)" && exit 1)

# 4. switch (esperado: 60 frames e checksum 88196450180)
python neon_lifter.py ghidra_extraction_switch.json --emit-c test_switch.c >/dev/null
clang $CFLAGS test_switch.c -o test_switch_bin
OUT_SWITCH=$(./test_switch_bin)
echo "$OUT_SWITCH" | grep -q "88196450180" && echo "✅ switch: OK" || (echo "❌ switch: FALHOU ($OUT_SWITCH)" && exit 1)

# 5. hb_compute (esperado: checksum 88196450180)
python neon_lifter.py ghidra_extraction_hb_compute.json --emit-c test_hb_compute.c >/dev/null
clang $CFLAGS test_hb_compute.c -o test_hb_compute_bin
OUT_COMPUTE=$(./test_hb_compute_bin)
echo "$OUT_COMPUTE" | grep -q "88196450180" && echo "✅ hb_compute: OK" || (echo "❌ hb_compute: FALHOU ($OUT_COMPUTE)" && exit 1)

# Limpeza
rm -f test_*.c test_*_bin test_*.exe

echo "=== Todos os 5 oráculos passaram com paridade estrita! ==="
