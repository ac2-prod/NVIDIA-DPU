#!/bin/bash
# run_lossless_e2e.sh — Issue #4 task 3: byte_group / bit_group の E2E (strict lossless)
#   final showdown の best 構成 (--thp 1, overlap=1, chunk=4M, zerocopy=1) を
#   --preproc byte_group と --preproc bit_group で各 1 本測り results_final_showdown.csv に追記。
#   どちらも純粋な並べ替え ⇒ verify=EXACT (完全可逆) であること。
#   strict lossless の throughput を THP 無圧縮(24.6) / 4KB 無圧縮(10.9) と比べる。
#   注: 論文の結論は、可逆の byte grouping＋LZ4 では 4 KiB・THP のどちらでも無圧縮を下回る、である (THP・overlap で約 9.3 GB/s)。
# 使い方: sbatch run_lossless_e2e.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:15:00
#SBATCH -J lossless_e2e
#SBATCH -o lossless_e2e_%j.out

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

for PRE in byte_group bit_group; do
  echo "===== best config, preproc=$PRE (strict lossless) ====="
  $MPIRUN $BIN --preproc $PRE
done

echo ""
echo "=== lossless E2E (best config) median 行 ==="
awk -F, '
  function g(n){return (n in c)?$(c[n]):"NA"}
  NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}
  g("stat")=="median" && g("zerocopy")=="1" && g("chunk_size_bytes")=="4194304" {
    printf "preproc=%-10s verify=%-6s ratio=%.3f pre+comp=%.2f+%.2f ms -> %.2f GB/s\n",
      g("preproc"), g("verify"), g("comp_ratio"),
      g("t_preproc_ms"), g("t_compress_ms"), g("throughput_gbps")
  }' $OUT | tail -6
echo ""
echo "確認: byte_group / bit_group が verify=EXACT であること。throughput を"
echo "      THP 無圧縮 24.6 / 4KB 無圧縮 10.9 / 非可逆の diff_byte 19.3 と比べる。"
echo "      (論文の結論: 可逆の byte grouping＋LZ4 は 4 KiB・THP のどちらでも無圧縮を下回る。)"
