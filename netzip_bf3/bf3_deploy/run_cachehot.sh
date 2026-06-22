#!/bin/bash
# run_cachehot.sh — 測定3: cache-hot / cache-cold 比較
#   hot      : 圧縮完了 → 即 ibv_post_send (現行パス)
#   cold_send: 圧縮完了後に 64MB スイープで LLC(32MB) を追い出してから post (追い出しは計時除外)
#   cold_recv: 受信完了後に追い出してから伸長
#   チャンク 4MiB と 65.7MB(一括) の 2 条件 → results_cachehot.csv
# 使い方: sbatch run_cachehot.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:30:00
#SBATCH -J cachehot
#SBATCH -o cachehot_%j.out

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
OUT=results_cachehot.csv

# OMP 設定は全測定で固定: OMP_NUM_THREADS=16, OMP_PROC_BIND=close, OMP_PLACES=cores
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"
# --lockstep 1: チャンク毎に受信側の伸長完了 ack を待つ (全条件で統一)。
#   cold 条件で必須 (旧版は rank0 圧縮 ∥ rank1 evict のノード間オーバーラップにより
#   evict 除外が過剰になり t_total が負になるバグがあった)。
#   hot にも付けるのは hot/cold 差を cache 効果だけにするため (同期条件を揃える)。
BIN="./benchmark_e2e_rdma --data $DATA --out $OUT --compress 1 --lockstep 1 --iters 10 --warmup 3"

for CHUNK in 4194304 65667072; do          # 4MiB チャンク / 一括
  for COLD in none send recv both; do      # hot / cold_send / cold_recv / cold_both
    echo "--- chunk=$CHUNK cold=$COLD ---"
    $MPIRUN $BIN --chunk-size $CHUNK --cold $COLD
  done
done

echo "=== $OUT (median rows) ==="
head -1 $OUT
grep ",median," $OUT
