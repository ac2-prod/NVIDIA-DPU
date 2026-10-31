#!/bin/bash
# run_omp_scaling.sh — 論文 Fig4 (スレッドスケーリング) / Fig5 (スレッド別フェーズ内訳)
#   lm_head 65.7MB を diff_byte+LZ4 (開発時の主対象。非可逆で論文の評価には入っていない) と none+snappy (対照) で、
#   スレッド数 1,2,4,8,16 を振り各フェーズの時間/スループット/speedup/efficiency を測る。
#   16T・256KB サブチャンク・DRAM write 込み・warmup3+iters7 の median (既存と同条件)。
#   ※ 単体処理なので RDMA 不要・1 ノード。MPI 不要。
# 使い方: sbatch run_omp_scaling.sh
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005
#SBATCH -N 1
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:20:00
#SBATCH -J omp_scaling
#SBATCH -o omp_scaling_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"
echo "=== node: $(hostname) arch=$(uname -m) ==="

# スレッド数は num_threads(p) 句で内部制御するが、affinity は環境に従う。
# OMP_NUM_THREADS=16 (上限), PLACES=cores, PROC_BIND=close, DYNAMIC=false 固定。
export OMP_NUM_THREADS=16
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export OMP_DYNAMIC=false

DATA="${1:-../../bf3_data/forward/0212_lm_head.pkl.bin}"
[ -f "$DATA" ] || { echo "ERROR: data not found: $DATA"; exit 1; }
OUT=results_omp_scaling.csv

./benchmark_omp_scaling --data "$DATA" --out $OUT --iters 7 --warmup 3

echo ""
echo "=== Fig4 用: speedup / efficiency (diff_byte+lz4) ==="
awk -F, 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}
  $(c["preproc"])=="diff_byte" && $(c["codec"])=="lz4" {
    printf "%-11s %2dT  %7.2f ms  %6.2f GB/s  speedup=%5.2f  eff=%.2f\n",
      $(c["phase"]),$(c["threads"]),$(c["time_ms"]),$(c["gbps"]),
      $(c["speedup_vs_1t"]),$(c["efficiency"]) }' $OUT

echo ""
echo "=== Fig5 用: スレッド別フェーズ時間 (diff_byte+lz4, ms) ==="
awk -F, 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}
  $(c["preproc"])=="diff_byte" && $(c["codec"])=="lz4" {
    t=$(c["threads"]); ph=$(c["phase"]); tm[t SUBSEP ph]=$(c["time_ms"]); T[t]=1 }
  END{ printf "%-8s %10s %10s %10s %10s %10s\n","threads","preproc","compress","decompress","restore","total";
    np=split("1 2 4 8 16",L," ");
    for(i=1;i<=np;i++){ t=L[i]; if(!(t in T))continue;
      pp=tm[t SUBSEP "preproc"]; cm=tm[t SUBSEP "compress"];
      dc=tm[t SUBSEP "decompress"]; rs=tm[t SUBSEP "restore"];
      printf "%-8s %10.2f %10.2f %10.2f %10.2f %10.2f\n",t,pp,cm,dc,rs,pp+cm+dc+rs } }' $OUT

echo ""
echo "=== 対照 (none+snappy: スケールするが圧縮しない) speedup ==="
awk -F, 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}
  $(c["preproc"])=="none" && $(c["codec"])=="snappy" && $(c["phase"])=="compress" {
    printf "compress %2dT speedup=%5.2f eff=%.2f (ratio=%.2f)\n",
      $(c["threads"]),$(c["speedup_vs_1t"]),$(c["efficiency"]),$(c["comp_ratio"]) }' $OUT

echo ""
echo "判定: preproc/restore は理想線形に近く、compress/decompress が早期飽和する"
echo "      (efficiency が 16T で大きく落ちる) ことを確認 → 並列は必要だが不十分。"
echo "次: src/ で  python3 plot_paper_figs.py  → figs/fig4_thread_scaling, fig5_phase_scaling"
