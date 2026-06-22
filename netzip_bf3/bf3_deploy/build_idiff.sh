#!/bin/bash
# build_idiff.sh — Issue #4/#5 のビルド (BF3 aarch64 ノード上)
#   - benchmark_idiff_compare : 単体比較 (byte_group/bit_group/diff_byte/idiff_byte 4 種)
#   - benchmark_e2e_rdma       : --preproc で 4 種を切替可能 (再ビルド)
# 使い方: sbatch build_idiff.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH -t 00:10:00
#SBATCH -J idiff_build
#SBATCH -o build_idiff_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11
module load hpcx/2.24.1

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"

echo "=== node: $(hostname) arch=$(uname -m) ==="
echo "=== build benchmark_idiff_compare (gcc, no MPI) ==="
make benchmark_idiff_compare || { echo "BUILD FAILED (idiff_compare)"; exit 1; }
echo "=== rebuild benchmark_e2e_rdma (mpicc + ibverbs) ==="
make benchmark_e2e_rdma || { echo "BUILD FAILED (e2e_rdma)"; exit 1; }

file benchmark_idiff_compare benchmark_e2e_rdma
echo "BUILD OK"
