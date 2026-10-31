#!/bin/bash
# run_msgsize_sweep.sh — 測定2: メッセージサイズ sweep (論文 Fig 2, 7 用)
#   無圧縮 vs lz4+diff_byte (--preproc 未指定の既定。非可逆で論文の評価対象外) で 4K〜65.7MB を sweep し results_msgsize_sweep.csv に追記。
#   全サイズ 0212_lm_head.pkl.bin の先頭 N バイトを使用 (データ特性を固定)。
# 使い方: sbatch run_msgsize_sweep.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 01:00:00
#SBATCH -J msgsize_sweep
#SBATCH -o msgsize_sweep_%j.out

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
OUT=results_msgsize_sweep.csv

# OMP 設定は全測定で固定: OMP_NUM_THREADS=16, OMP_PROC_BIND=close, OMP_PLACES=cores
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"

# サイズ系列: 4K..32M 前後は刻みを細かく。
# 注: 64MiB(67,108,864) はファイルサイズ(65,667,072)を超えるため decimal 64MB(64,000,000) を使用。
SIZES="4096 16384 65536 262144 1048576 4194304 16777216 25165824 29360128 \
33554432 37748736 41943040 50331648 64000000 65667072"

for SZ in $SIZES; do
  for COMP in 0 1; do
    echo "--- msg=$SZ compress=$COMP (single-shot) ---"
    # チャンク化なし (msg 全体を 1 RDMA Write): 転送カーブを純粋に測る
    $MPIRUN ./benchmark_e2e_rdma --data "$DATA" --out $OUT \
      --msg-size $SZ --compress $COMP --iters 10 --warmup 3
  done
done

# ── Fig 7 用追加系列: 4MiB チャンク + overlap=1 (圧縮(c+1) ∥ 転送(c) パイプライン) ──
# 無圧縮側も同じ chunk/overlap 条件で測り、比較を公平にする。msg >= 4M のみ。
SIZES2="4194304 16777216 25165824 33554432 50331648 65667072"   # 4M 16M 24M 32M 48M 65.7M

for SZ in $SIZES2; do
  for COMP in 0 1; do
    echo "--- msg=$SZ compress=$COMP (chunk=4MiB overlap=1) ---"
    $MPIRUN ./benchmark_e2e_rdma --data "$DATA" --out $OUT \
      --msg-size $SZ --chunk-size 4194304 --overlap 1 --compress $COMP \
      --iters 10 --warmup 3
  done
done

echo "=== $OUT (median rows) ==="
head -1 $OUT
grep ",median," $OUT
