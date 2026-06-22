#!/bin/bash
# run_fig8_fill.sh — Fig 8 バー③の欠測 1 条件だけを測定して
#   results_final_showdown.csv に追記する:
#     lz4+diff_byte, --thp 1, 一括 (chunk = msg = 65,667,072), overlap=0
#   (全フェーズ直列の THP 版内訳。追記後に plot_paper_figs.py を再実行すると
#    バー③が 4KB フォールバックから THP 行へ自動で差し替わる)
# 使い方: sbatch run_fig8_fill.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:10:00
#SBATCH -J fig8_fill
#SBATCH -o fig8_fill_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11
module load hpcx/2.24.1

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"

echo "=== node info ==="
srun -l bash -c 'echo "$(hostname) arch=$(uname -m)"'

DATA="${1:-$HOME/MICRO-2025-NetZIP/netzip_c/bf3_data/forward/0212_lm_head.pkl.bin}"
[ -f "$DATA" ] || { echo "ERROR: data file not found: $DATA"; exit 1; }
OUT=results_final_showdown.csv

# OMP 設定は全測定で固定: OMP_NUM_THREADS=16, OMP_PROC_BIND=close, OMP_PLACES=cores
mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0 \
  ./benchmark_e2e_rdma --data "$DATA" --out $OUT --msg-size 65667072 \
    --thp 1 --compress 1 --overlap 0 --iters 10 --warmup 3
# chunk-size 未指定 = msg と同じ = 一括 1 チャンク (全フェーズ直列)

echo ""
echo "=== 追記された行 (lz4 / chunk=msg / overlap=0 / thp) ==="
awk -F, '
  NR==1 { for (i=1; i<=NF; i++) c[$i]=i; next }
  $(c["stat"])=="median" && $(c["codec"])=="lz4" && $(c["overlap"])=="0" && \
  $(c["chunk_size_bytes"])==$(c["msg_size_bytes"]) {
    printf "page=%s  pre+comp+pack=%s+%s+%s ms  xfer(post+cqe)=%s+%s ms  total=%s ms  ->  %s GB/s\n",
      $(c["page_mode"]), $(c["t_preproc_ms"]), $(c["t_compress_ms"]), $(c["t_pack_ms"]),
      $(c["t_post_ms"]), $(c["t_completion_ms"]), $(c["t_total_ms"]), $(c["throughput_gbps"])
  }' $OUT
echo ""
echo "次: python3 plot_paper_figs.py を再実行すると Fig 8 バー③が THP 行に差し替わる"
