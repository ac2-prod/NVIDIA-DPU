#!/bin/bash
# build_rdma.sh — BF3 (aarch64) ノード上で rdma_sanity / benchmark_e2e_rdma をビルドする
# 使い方: sbatch build_rdma.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH -t 00:10:00
#SBATCH -J netzip_build
#SBATCH -o build_rdma_%j.out

# BF3 (aarch64) 用モジュール環境 (x86_64 ログインノードの MODULEPATH を上書き)
export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11
# v6 (benchmark_e2e_rdma) は通信スレッドを使わないためプレーン hpcx でよい
module load hpcx/2.24.1

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"

echo "=== node / ibverbs 環境確認 ==="
echo "host=$(hostname) arch=$(uname -m)"
ls -l /usr/include/infiniband/verbs.h 2>&1
ibv_devinfo 2>&1 | grep -E "hca_id|link_layer|state|active_mtu" | head -20

echo "=== build ==="
make rdma_sanity benchmark_e2e_rdma || { echo "BUILD FAILED"; exit 1; }
file rdma_sanity benchmark_e2e_rdma
echo "BUILD OK"
