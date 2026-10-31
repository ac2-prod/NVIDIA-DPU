#!/bin/bash
# run_lossless_compare.sh — Issue #4 task 2: 4 前処理の単体比較
#   byte_group / bit_group (純並べ替え, lossless) /
#   diff_byte (float, ±1ULP lossy) / idiff_byte (整数 diff, lossless)
#   を同条件 (16T, 256KB サブチャンク連続割当, run_direct 準拠) で:
#     LZ4 圧縮率 / preproc・compress・decompress・restore スループット (DRAM write 込み) /
#     round-trip bit-exact 検証
#   を測り results_lossless_compare.csv に追記する。
#   データは idiff のときと同じセット (lm_head + 実勾配 + mlp 4種) で汎化も確認。
#   ※ 単体ベンチなので 1 ノードでよい。MPI 不要。
# 使い方: sbatch run_lossless_compare.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:20:00
#SBATCH -J lossless_cmp
#SBATCH -o lossless_compare_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"
echo "=== node: $(hostname) arch=$(uname -m) ==="

export OMP_NUM_THREADS=16
export OMP_PROC_BIND=close
export OMP_PLACES=cores

D=../../bf3_data
./benchmark_idiff_compare --out results_lossless_compare.csv --iters 7 --warmup 2 \
  $D/forward/0212_lm_head.pkl.bin \
  $D/grad/0000_lm_head.pkl.bin \
  $D/grad/0003_model.layers.15.mlp.up_proj.pkl.bin \
  $D/grad/0005_model.layers.15.mlp.gate_proj.pkl.bin \
  $D/forward/0009_model.layers.0.mlp.gate_proj.pkl.bin \
  $D/forward/0011_model.layers.0.mlp.up_proj.pkl.bin

echo ""
echo "=== results_lossless_compare.csv (preproc / ratio / bit_exact / 速度) ==="
awk -F, 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;
  printf "%-26s %-10s %-9s %-8s %7s %7s %7s %7s\n","data","preproc","bit_exact","ratio","pre","cmp","dec","res";next}
  {printf "%-26s %-10s %-9s %-8.4f %7.2f %7.2f %7.2f %7.2f\n",
   substr($(c["data"]),1,26),$(c["preproc"]),$(c["bit_exact"]),$(c["comp_ratio"]),
   $(c["gbps_preproc"]),$(c["gbps_compress"]),$(c["gbps_decompress"]),$(c["gbps_restore"])}' \
  results_lossless_compare.csv
echo ""
echo "判定: byte_group / bit_group / idiff_byte が全て bit_exact=yes であること。"
echo "      圧縮率を near-lossless(diff_byte ~1.98) と idiff_byte(~1.32) の間で比較。"
