#!/bin/bash
# run_sharedlink.sh — Issue #10 補助: 低速/共有リンクで lossless 圧縮が無圧縮に勝つことの実証
#   実効リンク帯域を leaky-bucket pacing (--rate-limit-gbps) で段階制御し、各帯域で
#   3 構成の E2E throughput を測る:
#     - 無圧縮 (baseline)
#     - lz4+diff_byte  (lossy 1.98x; 勝敗文脈の参考)
#     - lz4+byte_group (strict lossless 1.46x, verify=EXACT; 主役)
#   理論: lossless E2E = min(計算上限~9.3, 1.46×b)。b < ~9.3 で無圧縮(=b)を上回るはず。
#   構成は showdown best: --thp 1 overlap=1 chunk=4M zerocopy=1。
# 使い方: sbatch run_sharedlink.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:30:00
#SBATCH -J sharedlink
#SBATCH -o sharedlink_%j.out

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
SL=results_sharedlink.csv

# OMP 固定。レート制限は送信側 main スレッドの busy-wait で行う。
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"
COMMON="--data $DATA --out results_e2e_breakdown.csv --sharedlink-out $SL \
  --msg-size 65667072 --thp 1 --overlap 1 --chunk-size 4194304 \
  --iters 10 --warmup 3"

# 実効帯域レベル (GB/s)。クロスオーバー ~9.3 付近を細かく。
LEVELS="24 16 12 10 9.3 9 8 7 5 3"

for BW in $LEVELS; do
  echo "===================== rate-limit ${BW} GB/s ====================="
  echo "--- no-compress ---"
  $MPIRUN ./benchmark_e2e_rdma $COMMON --rate-limit-gbps $BW --compress 0
  echo "--- lz4+diff_byte (lossy) ---"
  $MPIRUN ./benchmark_e2e_rdma $COMMON --rate-limit-gbps $BW --compress 1 --zerocopy 1 --preproc diff_byte
  echo "--- lz4+byte_group (lossless) ---"
  $MPIRUN ./benchmark_e2e_rdma $COMMON --rate-limit-gbps $BW --compress 1 --zerocopy 1 --preproc byte_group
done

# 永続化保険: 測定直後に共有FS上へタイムスタンプ付きバックアップを残す
# (作業コピーが誤って消えても復元できるように。$SL は append-only で書かれる)
if [ -s "$SL" ]; then
  cp -p "$SL" "${SL%.csv}.$(date +%Y%m%dT%H%M%S).bak.csv"
  echo "backup: $(ls -1 ${SL%.csv}.*.bak.csv | tail -1)  ($(wc -l < "$SL") lines in $SL)"
else
  echo "WARNING: $SL が空または存在しない (--sharedlink-out 失敗?)"
fi

echo ""
echo "############### サマリ ###############"
echo "=== results_sharedlink.csv (target帯域別の 3 構成 E2E) ==="
# 既知の LEVELS 順で集計 (gawk 専用の asorti/asort は使わない)
awk -F, -v levels="$LEVELS" 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}{
  k=$(c["target_bw_gbps"])+0; pre=$(c["preproc"]);
  meas[k]=$(c["measured_bw_gbps"]);
  if(pre=="none"){none[k]=$(c["e2e_gbps"])}
  else if(pre=="diff_byte"){diff[k]=$(c["e2e_gbps"])}
  else if(pre=="byte_group"){byte[k]=$(c["e2e_gbps"]); byv[k]=$(c["verify"])} }
END{
  printf "%-9s %-11s | %-11s %-11s %-16s | %s\n","target","measured","no-compress","diff(lossy)","byte_grp(loss)","byte>nocomp?";
  nl=split(levels,L," ");
  for(i=1;i<=nl;i++){k=L[i]+0;
    win=(byte[k]>none[k])?"YES lossless-wins":"no";
    printf "%-9s %-11.2f | %-11.2f %-11.2f %-16s | %s\n",
      L[i],meas[k],none[k],diff[k],sprintf("%.2f(%s)",byte[k],byv[k]),win} }' $SL

echo ""
echo "=== クロスオーバー帯域 (byte_group が無圧縮を上回る境界) ==="
awk -F, -v levels="$LEVELS" 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;next}{
  k=$(c["target_bw_gbps"])+0; pre=$(c["preproc"]);
  if(pre=="none")none[k]=$(c["e2e_gbps"]); if(pre=="byte_group")byte[k]=$(c["e2e_gbps"]) }
END{
  nl=split(levels,L," ");
  # LEVELS は降順 (24..3) なので、勝→負に変わる境界を探す
  prev=-1;
  for(i=1;i<=nl;i++){k=L[i]+0; w=(byte[k]>none[k])?1:0;
    if(prev>=0 && w==1 && pw==0) printf "  crossover between b=%.2f (lose) and b=%.2f (win) — 理論 ~9.3 GB/s\n",pk,k;
    prev=k; pk=k; pw=w}
  print "  (上表で byte_group>no-compress の YES/no 境界を確認。~9.3 付近なら理論一致)" }' $SL

echo ""
echo "次: src/ で python3 plot_paper_figs.py → figs/fig_sharedlink.{png,pdf}"
