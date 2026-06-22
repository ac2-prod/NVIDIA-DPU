#!/bin/bash
# run_idiff_compare.sh — Issue #5 task 3: diff_byte vs idiff_byte の単体比較
#   16 スレッド・256KB サブチャンク連続割当 (run_direct 準拠) で、
#   LZ4 圧縮率 / preproc・compress・decompress・restore スループット (DRAM write 込み) /
#   round-trip bit-exact 検証 を測り results_idiff_compare.csv に追記する。
#   複数の勾配・テンソルで圧縮率が lm_head 特有でないかも確認する。
#   ※ 単体ベンチなので 1 ノードでよい。MPI 不要。
# 使い方: sbatch run_idiff_compare.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:15:00
#SBATCH -J idiff_cmp
#SBATCH -o idiff_compare_%j.out

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
# 主対象 + 圧縮率の汎化確認用に異なるテンソル数種 (>=4MB を優先, 端数サブチャンクは切捨て)
./benchmark_idiff_compare --out results_idiff_compare.csv --iters 7 --warmup 2 \
  $D/forward/0212_lm_head.pkl.bin \
  $D/grad/0000_lm_head.pkl.bin \
  $D/grad/0003_model.layers.15.mlp.up_proj.pkl.bin \
  $D/grad/0005_model.layers.15.mlp.gate_proj.pkl.bin \
  $D/forward/0009_model.layers.0.mlp.gate_proj.pkl.bin \
  $D/forward/0011_model.layers.0.mlp.up_proj.pkl.bin

echo ""
echo "=== results_idiff_compare.csv ==="
column -s, -t results_idiff_compare.csv | \
  awk 'NR==1{print;next}{print}' | cut -c1-160
echo ""
echo "判定: 全 idiff_byte 行が bit_exact=yes (mismatch_bytes=0) であること。"
echo "      comp_ratio が diff_byte 比でどれだけ落ちるか確認 (1.9 前後維持なら採用)。"
