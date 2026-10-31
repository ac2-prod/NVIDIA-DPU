#!/bin/bash
# run_e2e_breakdown.sh — 測定1: E2E 時間内訳 (論文 Fig 8 用)
#   圧縮あり/なし × 一括(65.7MB)/4MiBチャンク × 逐次/オーバーラップ を
#   フェーズ分解付きで計測し results_e2e_breakdown.csv に追記する。
# 使い方: sbatch run_e2e_breakdown.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:20:00
#SBATCH -J e2e_breakdown
#SBATCH -o e2e_breakdown_%j.out

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
OUT=results_e2e_breakdown.csv

# OMP 設定は全測定で固定: OMP_NUM_THREADS=16, OMP_PROC_BIND=close, OMP_PLACES=cores
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"
BIN="./benchmark_e2e_rdma --data $DATA --out $OUT --iters 10 --warmup 3"

# (a) 一括 1 チャンク (65.7MB), 逐次 — フェーズが完全に直列化された最もクリーンな内訳 (Fig 8 の主データ)
$MPIRUN $BIN --compress 0
$MPIRUN $BIN --compress 1

# (b) 4MiB チャンク, 逐次 — チャンク化の影響
$MPIRUN $BIN --compress 0 --chunk-size 4194304
$MPIRUN $BIN --compress 1 --chunk-size 4194304

# (c) 4MiB チャンク, オーバーラップ (圧縮(c+1) ∥ 転送(c); v5 パイプラインに相当)
#     completion はオーバーラップで「隠れなかった転送時間の残り」を意味する
$MPIRUN $BIN --compress 0 --chunk-size 4194304 --overlap 1
$MPIRUN $BIN --compress 1 --chunk-size 4194304 --overlap 1

# ── ibverbs がどうしても動かない場合の MPI フォールバック (UCX_TLS=rc で RDMA transport を強制) ──
# RDMAで動いている事の事前確認: ucx_info -d | grep -A3 "Transport: rc"
# $MPIRUN -x UCX_TLS=rc -x UCX_NET_DEVICES=mlx5_0:1 $BIN --compress 1 --transport mpi

echo "=== $OUT (median rows) ==="
head -1 $OUT
grep ",median," $OUT | tail -12
