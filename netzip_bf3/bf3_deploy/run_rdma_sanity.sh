#!/bin/bash
# run_rdma_sanity.sh — 測定0サニティ: 生 RDMA Write 帯域が既知値と一致するか確認
#   合格基準: 4MB warm ≈ 24 GB/s (osu_bw 4MB), 60MB級 ≈ 11 GB/s (osu_bw 60MB)
# 使い方: sbatch run_rdma_sanity.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:10:00
#SBATCH -J rdma_sanity
#SBATCH -o rdma_sanity_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11
module load hpcx/2.24.1

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"

echo "=== node info ==="
srun -l bash -c 'echo "$(hostname) arch=$(uname -m)"'

# ── hugepages 設定確認 (両ノード) ──
# HugePages_Total が 40 未満だと --hugepages は MAP_HUGETLB に失敗し THP に
# フォールバックする (65.7MB ≈ 32 × 2MBページ + 余裕で 40 ページ必要)。
# プールを増やすには root が必要: echo 64 | sudo tee /proc/sys/vm/nr_hugepages
# root が無い場合は THP フォールバックで可 (enabled が [always] か [madvise] なら有効。
# その場合 CSV の page_mode=thp_madvise、smaps の AnonHugePages で 2MB 化を確認)。
echo "=== hugepages status ==="
srun -l bash -c 'grep Huge /proc/meminfo; echo -n "THP: "; cat /sys/kernel/mm/transparent_hugepage/enabled'

# OMP 設定は全測定で固定 (サニティは単スレッドだが条件を揃えるため明示)
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"

echo "=== (1) baseline 4KB pages ==="
$MPIRUN ./rdma_sanity

echo "=== (2) 2MB hugepages (MTT 仮説検証: 崖が消えるか) ==="
$MPIRUN ./rdma_sanity --hugepages

echo "=== results_rdma_sanity.csv (tail) ==="
tail -30 results_rdma_sanity.csv
