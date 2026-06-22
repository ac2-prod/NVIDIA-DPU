#!/bin/bash
# build_generalization.sh — 一般化測定バイナリのビルド (BF3 aarch64 ノード上)
#   benchmark_generalization は snappy(C++)/zstd をリンクするため初回は
#   codec オブジェクトのビルドに数分かかる。
# 使い方: sbatch build_generalization.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH -t 00:20:00
#SBATCH -J gen_build
#SBATCH -o build_generalization_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"
echo "=== node: $(hostname) arch=$(uname -m) ==="

echo "=== build benchmark_generalization (lz4 + snappy + zstd) ==="
make benchmark_generalization || { echo "BUILD FAILED"; exit 1; }
file benchmark_generalization
echo "BUILD OK"
