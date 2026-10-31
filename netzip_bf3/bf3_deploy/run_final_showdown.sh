#!/bin/bash
# run_final_showdown.sh — 最終対決 v2: 送信側CPU削減 (pack排除 + preproc競合緩和)
#
#   前回結果: 圧縮 best 19.1 GB/s (lz4+diff_byte, 非可逆) < THP ベースライン 24.6 GB/s。
#   送信側 CPU (preproc+comp+pack ≈ 3.1ms) を削る最適化の効果を測る (当時は 2.2ms まで削れば 30 GB/s と見積もった)。
#   注: このスクリプトは --preproc を指定しないので、lz4 行は既定の diff_byte (非可逆、論文の評価対象外) で測る。
#   注: 論文の結論は、可逆の byte grouping＋LZ4 では 4 KiB・THP のどちらでも無圧縮を下回る、である。
#
#   最適化:
#     --zerocopy 1    pack 排除。LZ4 出力を直接 wire スロット (bound 固定オフセット) へ
#                     書き、転送はサブ毎の実バイトのみを scatter RDMA Write
#                     (転送バイトは膨張ゼロ、リモート footprint のみ膨張 →
#                      CSV の wire_footprint_bytes で実測確認)
#     --phase-shift 1 位相ずらし。preproc/LZ4 を分離し、チャンク c-1 の post を
#                     c の preproc 完了後に行う → preproc 中は DMA 非活動
#                     (preproc 46→33 GB/s 低下が NIC DMA との帯域競合かの切り分け)
#     --pre-threads 12 (2b) preproc だけ 12 スレッドに絞り帯域の食い合いを減らす
#                     (LZ4 は 16 のまま; phase-shift が効かない場合のフォールバック)
#
#   比較: lz4 行の throughput_gbps を 24.6 GB/s (THP 無圧縮ベースライン) と比べる
#
# 使い方: sbatch run_final_showdown.sh [datafile]
#SBATCH -p rome
#SBATCH --nodelist=romebf3a005,romebf3a006
#SBATCH -N 2
#SBATCH --ntasks-per-node=1
#SBATCH --exclusive
#SBATCH -t 00:30:00
#SBATCH -J final_showdown
#SBATCH -o final_showdown_%j.out

export MODULEPATH=/global/software/rocky-9.aarch64/modfiles/langs:/global/software/rocky-9.aarch64/modfiles/tools:/global/software/rocky-9.aarch64/modfiles/apps
source /etc/profile.d/modules.sh
module purge 2>/dev/null
module load gcc/11
module load hpcx/2.24.1

cd "$SLURM_SUBMIT_DIR/src" 2>/dev/null || cd "$HOME/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src"

echo "=== node info ==="
srun -l bash -c 'echo "$(hostname) arch=$(uname -m)"'
echo "=== THP status ==="
srun -l bash -c 'cat /sys/kernel/mm/transparent_hugepage/enabled'

DATA="${1:-$HOME/MICRO-2025-NetZIP/netzip_c/bf3_data/forward/0212_lm_head.pkl.bin}"
[ -f "$DATA" ] || { echo "ERROR: data file not found: $DATA"; exit 1; }
OUT=results_final_showdown.csv
# スキーマ変更 (zerocopy/phase_shift/pre_threads/wire_footprint 列追加) のため
# 旧 results_final_showdown.csv は初回実行時に .old.<unixtime> へ自動退避される

# OMP 設定は全測定で固定: OMP_NUM_THREADS=16, OMP_PROC_BIND=close, OMP_PLACES=cores
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"
BIN="./benchmark_e2e_rdma --data $DATA --out $OUT --msg-size 65667072 --thp 1 --iters 10 --warmup 3"

# A) THP ベースライン (無圧縮一括) — 比較基準値 (≈24.6 GB/s)
echo "===== A: baseline no-compress (THP) ====="
$MPIRUN $BIN --compress 0

# B) 最適化マトリクス: chunk {4M,8M} × zerocopy {0,1} × phase-shift {0,1}
#    (zc=0, ps=0 が前回の fused 系列 = 参照点)
for CHUNK in 4194304 8388608; do
  for ZC in 0 1; do
    for PS in 0 1; do
      echo "===== B: lz4 chunk=$CHUNK zerocopy=$ZC phase-shift=$PS ====="
      $MPIRUN $BIN --compress 1 --overlap 1 --chunk-size $CHUNK \
        --zerocopy $ZC --phase-shift $PS
    done
  done
done

# C) 2b フォールバック: preproc 12 スレッド (zerocopy=1 固定)
for CHUNK in 4194304 8388608; do
  for PS in 0 1; do
    echo "===== C: lz4 chunk=$CHUNK zerocopy=1 phase-shift=$PS pre-threads=12 ====="
    $MPIRUN $BIN --compress 1 --overlap 1 --chunk-size $CHUNK \
      --zerocopy 1 --phase-shift $PS --pre-threads 12
  done
done

echo ""
echo "=== 最終対決 v2 サマリ (median 行) ==="
# 列名ベースで読む。列が無い場合 (旧スキーマの CSV) は "NA" を出して
# 警告する — $(c[名前]) は名前が無いと $0 (行全体) になり 0 と誤表示されるため。
awk -F, '
  function g(n) { return (n in c) ? $(c[n]) : "NA" }
  NR==1 {
    for (i=1; i<=NF; i++) c[$i]=i
    if (!("zerocopy" in c))
      print "WARN: この CSV は旧スキーマ (zerocopy 列なし)。バイナリを再ビルドして再実行すること。"
    next
  }
  g("stat")=="median" {
    foot = "NA"
    if (("wire_footprint_bytes" in c) && ("wire_bytes" in c) && $(c["wire_bytes"]) > 0)
      foot = sprintf("%.2f", $(c["wire_footprint_bytes"]) / $(c["wire_bytes"]))
    printf "%-5s chunk=%-9s zc=%s ps=%s pre=%-2s  pre+comp+pack=%5.2f+%5.2f+%5.2f ms  cqe=%5.2f  ->  %6.2f GB/s (foot x%s)\n",
      g("codec"), g("chunk_size_bytes"), g("zerocopy"), g("phase_shift"),
      g("pre_threads"),
      g("t_preproc_ms"), g("t_compress_ms"), g("t_pack_ms"),
      g("t_completion_ms"), g("throughput_gbps"), foot
  }' $OUT
echo ""
echo "比較: lz4 行の throughput を A 行 (THP 無圧縮, 約 24.6 GB/s) と比べる"
echo "見方: zc=1 で t_pack=0 / ps=1 で t_preproc が 46GB/s 相当 (4M:0.18ms, 8M:0.36ms) に"
echo "      回復していれば DMA 競合説と整合的。foot は zerocopy のリモート footprint 膨張率。"
