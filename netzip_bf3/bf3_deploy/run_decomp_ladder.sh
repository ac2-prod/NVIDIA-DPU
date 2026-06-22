#!/bin/bash
# run_decomp_ladder.sh — Fig 6: 伸長最適化アブレーション (--step {1,2,3,4,6}) を
#   隔離・排他ノードでクリーンに再測定し、結果を bf3_deploy/src/ に書き出す。
#   ベンチ本体は親ディレクトリ netzip_c/ の benchmark_decompress (parent netzip.c 使用)。
#   単一テンソル 65.7MB の伸長なので数分で完了。1 ノードで可 (RDMA/MPI 不要)。
# 使い方: sbatch run_decomp_ladder.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:20:00
#SBATCH -J decomp_ladder
#SBATCH -o decomp_ladder_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11

# benchmark_decompress は親 netzip_c/ にある (parent netzip.c を使うアブレーション本体)
ROOT="${SLURM_SUBMIT_DIR:-$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy}"
PAR="$ROOT/.."                       # = netzip_c/
SRC="$ROOT/src"
cd "$PAR" || { echo "cannot cd $PAR"; exit 1; }
echo "=== node: $(hostname) arch=$(uname -m) ==="

echo "=== build benchmark_decompress (parent netzip_c/) ==="
make benchmark_decompress || { echo "BUILD FAILED"; exit 1; }
file benchmark_decompress

# OMP 設定固定 (ベンチは内部で 1..16 スレッドを sweep する)
export OMP_PLACES=cores OMP_PROC_BIND=close OMP_DYNAMIC=false

DATA="${1:-$PAR/bf3_data/forward/0212_lm_head.pkl.bin}"
[ -f "$DATA" ] || { echo "ERROR: data not found: $DATA"; exit 1; }

# 各ステップを測定し、CSV を直接 src/ へ (plot_paper_figs.py が読む場所)
for STEP in 1 2 3 4 6; do
  echo "===== --step $STEP ====="
  ./benchmark_decompress --input "$DATA" --step $STEP --repeat 10 \
    --out "$SRC/results_decomp_step${STEP}.csv"
done

echo ""
echo "=== Fig6 用 16T throughput (各ステップ) ==="
for STEP in 1 2 3 4 6; do
  awk -F, -v s=$STEP 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;next} $1==16{
    printf "  step%s 16T: %6.2f GB/s (ratio=%.3f)\n",s,$(c["throughput_gbps"]),$(c["ratio"])}' \
    "$SRC/results_decomp_step${STEP}.csv"
done

echo ""
echo "次: src/ で python3 plot_paper_figs.py → figs/fig6_ablation.{png,pdf}"
