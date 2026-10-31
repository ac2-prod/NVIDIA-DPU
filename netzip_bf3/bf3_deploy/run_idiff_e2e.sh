#!/bin/bash
# run_idiff_e2e.sh — Issue #5 task 4: lossless (idiff_byte) 版の E2E 主結果
#   final showdown の best 構成 (--thp 1, overlap=1, chunk=4M, zerocopy=1) を
#   diff_byte (float, lossy ±1ULP) と idiff_byte (整数, bit-exact) の両方で 1 本ずつ測り、
#   lossless 化の E2E スループット影響を取る。results_final_showdown.csv に追記。
#   (preproc 列で diff_byte / idiff_byte を区別。idiff 行は verify=EXACT であること)
# 使い方: sbatch run_idiff_e2e.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:15:00
#SBATCH -J idiff_e2e
#SBATCH -o idiff_e2e_%j.out

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
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"
BIN="./benchmark_e2e_rdma --data $DATA --out $OUT --msg-size 65667072 \
  --thp 1 --compress 1 --overlap 1 --chunk-size 4194304 --zerocopy 1 \
  --iters 10 --warmup 3"

echo "===== best config, preproc=diff_byte (float, lossy) ====="
$MPIRUN $BIN --preproc diff_byte

echo "===== best config, preproc=idiff_byte (integer, bit-exact) ====="
$MPIRUN $BIN --preproc idiff_byte

echo ""
echo "=== idiff vs diff (best config) median 行 ==="
awk -F, '
  function g(n){return (n in c)?$(c[n]):"NA"}
  NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}
  g("stat")=="median" && g("zerocopy")=="1" && g("chunk_size_bytes")=="4194304" {
    printf "preproc=%-10s verify=%-6s ratio=%.3f wire_foot=%s pre+comp=%.2f+%.2f ms -> %.2f GB/s\n",
      g("preproc"), g("verify"), g("comp_ratio"), g("wire_footprint_bytes"),
      g("t_preproc_ms"), g("t_compress_ms"), g("throughput_gbps")
  }' $OUT | tail -4
echo ""
echo "判定: idiff_byte 行が verify=EXACT、throughput が diff_byte 同等であること。"
