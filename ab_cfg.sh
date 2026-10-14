#!/system/bin/sh
# A = switch_particle_bench (fonte original + shim), B = reconstructed_switch_bin (C do Qwen sobre o esqueleto do CFG)
# 20 pares em cpu7, ordem alternada (AB, BA, ...) e 1 s de pausa entre execucoes.
cd /data/local/tmp
k=1
while [ $k -le 20 ]; do
  if [ $((k % 2)) -eq 1 ]; then order="A B"; else order="B A"; fi
  for x in $order; do
    if [ "$x" = A ]; then bin=switch_particle_bench; else bin=reconstructed_switch_bin; fi
    sleep 1
    out=$(taskset 80 ./$bin | grep -E "Checksum|Tempo" | tr '\n' ' ')
    echo "$k $x $out"
  done
  k=$((k + 1))
done
