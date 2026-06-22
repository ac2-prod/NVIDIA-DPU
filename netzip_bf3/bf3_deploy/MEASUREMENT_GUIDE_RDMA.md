# RDMA E2E 計測ガイド (測定0〜3, Fig 2/7/8 用)

目的: 「単体 30 GB/s の lz4+diff_byte が E2E では 5.69 < 11 GB/s で負ける理由」を
フェーズ分解 (測定1)・メッセージサイズ sweep (測定2)・cache hot/cold (測定3) で説明する。
転送は ibverbs RC QP + RDMA Write with Immediate (測定0で実装)。

## 構成ファイル

| ファイル | 内容 |
|---|---|
| `src/rdma_transport.{h,c}` | ibverbs RC QP トランスポート (接続確立のみ MPI、データパスは純 ibverbs) |
| `src/rdma_sanity.c` | 測定0 サニティ: 生 RDMA Write 帯域 (warm/cold × 4M〜65.7M) |
| `src/benchmark_e2e_rdma.c` | 測定1〜3 本体 (フェーズ分解計時、1バイナリをフラグで切替) |
| `build_rdma.sh` | ビルド用 sbatch (BF3 ノード上でビルド必須 — aarch64) |
| `run_rdma_sanity.sh` | 測定0 実行 → `results_rdma_sanity.csv` |
| `run_e2e_breakdown.sh` | 測定1 実行 → `results_e2e_breakdown.csv` |
| `run_msgsize_sweep.sh` | 測定2 実行 → `results_msgsize_sweep.csv` |
| `run_cachehot.sh` | 測定3 実行 → `results_cachehot.csv` |

CSV はすべて `src/` に追記される。

## 実行手順 (すべてログインノードから sbatch で投入。ログインノードでは計測しない)

```bash
cd ~/MICRO-2025-NetZIP/netzip_c/bf3_deploy

# 0) ノード空き確認 (romebf3a005-008 / thorbf3a017-032 が aarch64 BF3)
sinfo -p rome -N | grep bf3
squeue -p rome

# 1) ビルド (BF3 ノード上で実行される。ログに BUILD OK が出ること)
sbatch build_rdma.sh
#    確認: cat build_rdma_<jobid>.out
#    ibverbs が無い等で失敗した場合 → 末尾「MPIフォールバック」参照

# 2) 測定0 サニティ (必ず最初に。合格してから先へ)
sbatch run_rdma_sanity.sh
cat rdma_sanity_<jobid>.out
#    合格基準: 4M/warm ≈ 24 GB/s, 60M級 ≈ 11 GB/s (osu_bw と一致)

# 3) 測定1: E2E 内訳 (Fig 8)
sbatch run_e2e_breakdown.sh
cat e2e_breakdown_<jobid>.out          # 実行毎に median 内訳表が stdout に出る

# 4) 測定2: msg-size sweep (Fig 2, 7) — 約20〜30分
sbatch run_msgsize_sweep.sh

# 5) 測定3: cache hot/cold
sbatch run_cachehot.sh

# 6) CSV 確認 (median 行だけ見る)
cd src
head -1 results_e2e_breakdown.csv; grep ',median,' results_e2e_breakdown.csv
head -1 results_msgsize_sweep.csv;  grep ',median,' results_msgsize_sweep.csv
head -1 results_cachehot.csv;       grep ',median,' results_cachehot.csv
```

割当ノードの確認: `squeue -u $USER` (NODELIST 列) または
`scontrol show job <jobid> | grep NodeList`。ジョブスクリプト内では
`$SLURM_NODELIST` が出力ログ先頭の `node info` に表示される。

### salloc で対話的にやる場合

```bash
salloc --exclusive -p rome -w romebf3a005,romebf3a006 -N 2 --ntasks-per-node=1 -t 1:00:00
# 注意: salloc 直後のシェルは x86_64 ログインノード上。コマンドは srun 経由で流すこと。
srun -l bash -c 'hostname; uname -m'      # 割当ノード確認 (aarch64 であること)
# 以降は sbatch スクリプトの中身と同じ手順を srun bash -lc '...' で実行
```

rome が埋まっている場合は `--nodelist=thorbf3a017,thorbf3a018 -p thor` に差し替え可
(スクリプト内の `#SBATCH --nodelist` も同様に変更)。

## benchmark_e2e_rdma のフラグ (スクリプトを変えたい場合)

```
--data PATH        入力 (raw bf16)。省略時は合成データ
--msg-size N       転送総バイト数 [既定 65667072 = lm_head 全体]
--chunk-size N     チャンク分割 [既定 = msg-size (一括)]
--compress 0|1     0 = baseline 直送 [1]
--overlap 0|1      1 = 圧縮(c+1)∥転送(c) の double-buffer パイプライン [0]
--lockstep 0|1     1 = チャンク毎に受信側の伸長完了 ack を待つ (フェーズ完全直列化) [0]
                   cold × 複数チャンク時は自動有効 (下記バグ修正参照)
--cold none|send|recv|both   測定3 の LLC 追い出し位置 [none]
--transport ibverbs|mpi      [ibverbs]
--iters/--warmup   [10 / 3]   --flush-iter 0|1  反復前 LLC flush [1]
--shift auto|X     diff_byte の shift [auto = netzip_find_min (単体測定と同じ)]
--out CSV
```

### バグ修正 (2026-06-11): cold × チャンクモードの t_total 負値

旧版はチャンクモードで rank0 の圧縮(c+1) と rank1 の eviction+伸長(c) がノード間で
並行するのに、wall から「両 rank の eviction 合計」を引いていたため、圧縮に隠れていた
eviction 分まで二重に引いて t_total が負 (-26.5ms) / unaccounted が大きな負 (-11.3ms)
になっていた。修正: `--lockstep 1` (受信側がチャンク c の伸長を終えて 0 バイト ack を
返すまで送信側は c+1 を始めない)。全フェーズが 1 本の critical path に直列化されるので
eviction 除外が厳密になり、unaccounted は ack RTT 程度 (≈0, 非負) になる。
cold × 複数チャンクでは自動有効化される。`run_cachehot.sh` は **hot 行にも**
`--lockstep 1` を付ける (hot/cold 差を cache 効果だけにするため同期条件を統一)。
**lockstep 行の t_total は非 lockstep 行とパイプライン重畳分だけ異なるので、
hot/cold 比較は必ず lockstep 同士で行うこと。**

### CSV スキーマ変更と自動退避

`lockstep` 列 (overlap の次) と sanity の `page_mode` 列を追加した。各バイナリは既存
CSV のヘッダが現スキーマと違う場合 `<name>.old.<unixtime>` へ自動退避して新規作成する
(旧データは消えない)。再測した chunk=4MB 行と旧 chunk=full 行を混ぜたい場合は
退避ファイルから median 行をコピーするのではなく、`run_cachehot.sh` を丸ごと再実行して
1 ファイルに揃えるのを推奨 (chunk=full は lockstep 無関係なので値は再現するはず)。

## 計測設計ノート (CSV を解釈する前に読むこと)

- **preproc/compress の分離**: 単体 30 GB/s を出した `benchmark_pipeline run_direct` と
  同一の fused 実行 (256KB サブチャンクを同一コアで preproc→LZ4) を保ったまま、
  サブチャンク毎にスレッド内で個別計時し、**fused 領域の wall を実測時間比で按分**して
  `t_preproc_ms` / `t_compress_ms` に分けている (decompress/restore も同様)。
  Case A/B 判定には合計 `t_preproc+t_compress` を見てもよい (按分に依存しない)。
- **t_post**: `ibv_post_send` 呼び出しに要した時間。**t_completion**: post 後 CQE 取得まで
  (= 実転送時間)。`--overlap 1` では completion は「圧縮で隠れなかった転送残り時間」になる。
- **t_wait_recv** (rank1 の受信完了待ち) は送信側フェーズと同時刻に進行するため、
  `t_unaccounted = t_total − Σ(他フェーズ)` の Σ には入れていない。
  一括・逐次 (chunk=msg, overlap=0) の行が最も解釈しやすい (全フェーズ直列)。
  チャンクあり時は rank0 の圧縮(c+1)と rank1 の伸長(c) がノード間で自然に重なるため
  unaccounted が負になり得る (仕様)。
- **t_evict_excl_ms**: cold モードの LLC 追い出し時間。t_total からは除外済み。
- **mr_reg_ms**: `ibv_reg_mr` 登録時間 (両 rank 合計、setup で1回だけ計測し全行に記載)。
- **gbps_*** 列はすべて元データサイズ (msg_size_bytes) 基準。
  `gbps_xfer = msg / (t_post + t_completion)` なので、圧縮ありでは wire が小さい分
  リンク実効帯域より大きく出る (それが圧縮の利得)。ワイヤ実帯域は
  `wire_bytes / (t_post+t_completion)` で別途計算できる。
- **sweep の 64M**: 64MiB (67,108,864) はファイル (65,667,072 B) を超えるため
  decimal 64MB (64,000,000) を使用。
- **sweep の 2 系列** (`results_msgsize_sweep.csv` 内): ①single-shot (chunk=msg,
  overlap=0) = 転送カーブを純粋に測る系列、②chunk=4MiB + overlap=1 (msg ≥ 4M のみ,
  圧縮あり/なし両方) = Fig 7 のパイプライン系列。`chunk_size_bytes` と `overlap` 列で
  区別する。
- 反復前に両ノードで LLC flush (64MB read+write スイープ) を行う = 入力データは
  毎回 DRAM cold (実運用の勾配転送と同条件)。
- imm データに wire バイト数を載せ、サブチャンク圧縮サイズはペイロード先頭の
  int32 ヘッダで受信側へ伝える (v5 と同じワイヤフォーマット)。

## Case A / B の判定方法 (測定1の結果から)

`results_e2e_breakdown.csv` の **compress=lz4, chunk=65667072, overlap=0, stat=median 行**を見る:

- **Case A (圧縮器は無罪)**: `gbps_compress`(または msg/(t_preproc+t_compress) の合算 ≈
  `1/(1/gbps_preproc+1/gbps_compress)`) が **≈30 GB/s を維持**
  → 律速は転送/バッファ配置。裏付け:
  - 測定2: 無圧縮の throughput_gbps がサイズ増で 24→11 GB/s に落ちるカーブ (Fig 2)、
    圧縮ありがそれをどこで上回る/下回るか (Fig 7)
  - 測定3: hot vs cold_send の t_completion 差 (NIC が LLC から読めるかの効果)
- **Case B (単体測定が過大評価)**: E2E 中の `gbps_compress` が **≈5 GB/s 級に低下**
  → DRAM 競合・cache miss で圧縮自体が崩れている。
  単体30 GB/s との差の原因切り分けは `--flush-iter 0` (入力 warm) で再実行して比較。

補助指標:
- `t_completion` (圧縮あり) vs 無圧縮行の `t_completion`: wire が 1/1.98 なのに
  転送時間が 1/1.98 になっていなければ、転送側にも非線形要因 (cold バッファ読み出し) がある
  → 測定3の hot/cold 差で定量化。
- v5 (5.69 GB/s) 相当は `chunk=4194304, overlap=1` の行。overlap=0 との差が
  「パイプライン化で隠せた/隠せなかった時間」。

## hugepages 検証 (崖の正体 = NIC MTT ミス仮説)

`rdma_sanity --hugepages` で送受信バッファを 2MB ページにして再測する
(`run_rdma_sanity.sh` は 4KB 版 → hugepages 版の順に両方実行する)。

- 仕組み: `mmap(MAP_HUGETLB|MAP_POPULATE)` → プール不足なら自動で
  `posix_memalign(2MB) + madvise(MADV_HUGEPAGE)` (THP) にフォールバック。
  実際に何になったかは stdout の `page_mode=` と smaps の
  `KernelPageSize / AnonHugePages`、CSV の `page_mode` 列で確認
  (hugetlb_2m / thp_madvise / base_4k / mixed)。
- 事前確認 (ジョブログに自動出力されるが手動なら):
  ```bash
  srun -p rome -w romebf3a005 bash -c 'grep Huge /proc/meminfo; cat /sys/kernel/mm/transparent_hugepage/enabled'
  ```
  - `Hugepagesize: 2048 kB` で `HugePages_Free >= 40` なら MAP_HUGETLB が通る
    (65.7MB ≈ 32 ページ + 余裕)。
  - 足りない場合 (root が必要): `echo 64 | sudo tee /proc/sys/vm/nr_hugepages`
    (ノード毎に必要。管理者依頼になる場合が多い)
  - root が無い場合: THP フォールバックで十分。`transparent_hugepage/enabled` が
    `[always]` か `[madvise]` であること (`[never]` だと base_4k のまま →
    管理者に THP 有効化を依頼)。
- 判定:
  - **65.7M warm/cold が ~24 GB/s に回復** → 崖の原因は NIC のアドレス変換
    キャッシュ (MTT) ミス。4KB ページでは 65.7MB = 約16,000 MTT エントリで
    キャッシュ溢れ、2MB ページでは ~32 エントリで常駐。
  - **~11 GB/s のまま** → MTT ではなく DRAM 帯域律速 (LLC に収まらないバッファの
    streaming read が原因)。4M warm が 24 のままで大サイズだけ落ちる形が維持される。
  - 副次指標: `mr_reg_ms` (ページ数が 1/512 になるので hugepages で大きく短縮するはず)。

## 最終対決「THP 対 THP」 (run_final_showdown.sh)

**確定済みの前提 (rdma_sanity --hugepages の結果)**: THP(2MB) で崖が完全に消え、
全サイズ 24.7 GB/s。→ 崖の正体は NIC アドレス変換 (MTT) ミスであり DRAM 律速ではない。

そこで「ベースラインが 24.7 GB/s に回復した土俵で圧縮はまだ勝てるか」を測る:

```bash
sbatch run_final_showdown.sh     # → src/results_final_showdown.csv
```

- 全行 msg=65,667,072。`--thp 1` で src/dst/wire/scratch/tls の全主要バッファを
  2MB 境界 + madvise(MADV_HUGEPAGE) 確保 (実測ページ化を smaps で検出、
  CSV の `page_mode` 列: thp_madvise / thp_no_2m / base_4k / mixed)。
- 行構成: 無圧縮一括 (THP ベースライン ≈24.7 想定) + lz4 overlap=1 ×
  chunk ∈ {4M, 8M, 16M, 32M} (チャンク拡大で preproc 効率 31→46 GB/s に近づくか vs
  overlap 粒度が粗くなり先頭チャンク圧縮がパイプライン立ち上がりを止めるトレードオフ)、
  さらに同条件の `--thp 0` 参考行 (THP 寄与の分離)。
- ジョブ末尾に awk サマリ (codec / page_mode / chunk / gbps_compress / gbps_xfer /
  throughput) が出る。
- **判定**: lz4 行の best `throughput_gbps` >
  無圧縮 thp_madvise 行 (≈24.7) → **hugepages 適用後もなお圧縮で線速超え** (主結果)。
  超えなければ → **圧縮の利益は 4KB ページ環境 (= デフォルト設定) 限定** が結論。
- 注: chunk=32M は 2 チャンク (32MiB + 30.6MB) しかなく overlap がほぼ効かない。
  立ち上がりコスト ≈ 先頭チャンクの圧縮時間 = chunk/46GB/s なので、
  期待値は throughput ≈ msg / (first_chunk_comp + max(comp, xfer) 残り) で粗く見積れる。

## 最終対決 v2: 送信側 CPU 削減 (run_final_showdown.sh, 2026-06-13 更新)

前回: 圧縮 best 19.1 < THP ベースライン 24.6 GB/s。送信側 CPU
(preproc+comp+pack ≈ 3.1ms) を 2.2ms に削れば 30 GB/s で勝てる。2 つの最適化:

- **`--zerocopy 1` (pack 排除)**: LZ4 出力を直接 wire スロットの
  `LZ4_compressBound` 固定オフセットへ書く → `t_pack = 0`。
  転送は「サブチャンク毎に実バイトのみを RDMA Write (unsignaled) × n_sub +
  最後にサイズ表ヘッダを Write-with-Imm (signaled)」の scatter 方式。
  RC の順序保証で imm の CQE 観測 = 全サブ着弾。
  **転送バイトは膨張ゼロ** (隙間は送らない)。膨張するのはリモートスロットの
  footprint のみ → `wire_footprint_bytes` 列に実測記録 (膨張率 = footprint/wire
  ≈ bound/実圧縮サイズ ≈ 圧縮率倍。受信側の読み出しが散在する点が実コスト)。
  ibverbs 専用 (--transport mpi とは併用不可)。
- **`--phase-shift 1` (位相ずらし, 2a)**: fused では「preproc 完了点」が無いため、
  compress を preproc 全サブ → LZ4 全サブの非 fused 2 領域に分割し、
  チャンク c-1 の post を「c の preproc 完了後」に移動:
  `preproc(c) [DMA非活動] → post(c-1) → LZ4(c) ∥ DMA(c-1) → CQE(c-1)`。
  preproc は決して DMA と重ならない → preproc の 46→33 GB/s 低下が NIC DMA との
  メモリ帯域競合なら、ps=1 で t_preproc が 46 GB/s 相当 (4M: 0.18ms / 8M: 0.36ms)
  に回復するはず。overlap=1 を強制、lockstep/cold と併用不可。
  この場合 t_preproc/t_compress は按分でなく真の領域 wall。
- **`--pre-threads 12` (2b)**: 非 fused の preproc 領域だけ 12 スレッド
  (LZ4 は 16 のまま)。phase-shift で回復しない場合の帯域食い合い緩和策。

実行: `sbatch run_final_showdown.sh` → `results_final_showdown.csv`
(スキーマ変更により旧ファイルは .old.* へ自動退避)。
構成: A) THP 無圧縮ベースライン 1 本、B) chunk {4M,8M} × zc {0,1} × ps {0,1}、
C) pre-threads 12 (zc=1) × ps {0,1} × chunk {4M,8M}。ジョブ末尾に awk サマリ。

**判定**: lz4 行の best throughput > 24.6 GB/s (A 行) →
「hugepages 環境でも送信側 CPU 削減により圧縮が線速超え」。
診断の見方: zc=1 で t_pack=0 になっているか / ps=1 で t_preproc が回復するか
(回復すれば DMA 競合説確定、しなければ preproc 低下は別要因 = LLC 競合等) /
foot 膨張率が想定 (≈圧縮率倍) 通りか。

## 論文図の生成 (src/plot_paper_figs.py → src/figs/)

```bash
cd ~/MICRO-2025-NetZIP/netzip_c/bf3_deploy/src
python3 plot_paper_figs.py --dry-run    # 行選択の検証のみ (matplotlib 不要)
python3 plot_paper_figs.py              # figs/ に PNG(300dpi) + PDF を生成
```

- Fig 2 (`fig2_rdma_cliff`): sanity CSV → 4KB/THP × warm/cold の帯域カーブ、
  LLC 32MB 縦線 + 200Gb/s 線速横線。
- Fig 7 (`fig7_e2e_throughput`): sweep CSV → 無圧縮/圧縮直列/圧縮overlap の3系列 +
  THP 基準線 (showdown の none 行)。クロスオーバー (~29 MB) を自動計算して注記。
- Fig 3 (`fig3_pareto`): 圧縮率 vs 圧縮スループット Pareto (results_generalization.csv,
  5076 ok 行)。全点を preproc 別色で半透明散布 + (preproc×codec) 毎の中央値マーカー12点
  (色=preproc, 形=codec)。**lossless 3種(byte/bit/idiff)=塗りつぶし、diff_byte(lossy)=中抜き**で
  住み分けを明示 (lossless は ratio ~1.3–1.7× で可逆、diff_byte は ~3.3×だが活性49.5%/
  勾配84.1%破壊)。「高圧縮率かつ可逆」な構成は存在しないことを示す。英語キャプションを stdout 出力。
- Fig 8 (`fig8_phase_breakdown`): 横 stacked bar 4本。**lz4/THP/一括/overlap=0 は
  未測定**のため breakdown の 4KB 行で代替し脚注表示 (THP で測るなら:
  `mpirun ... ./benchmark_e2e_rdma --data ... --thp 1 --compress 1` を1本流して
  results_final_showdown.csv に追記すれば自動で差し替わる)。
- CSV はスキーマ世代混在 → 列名アクセス + 既定値補完。median 行のみ、
  重複条件は timestamp 最新を採用。使った行は stdout に全て表示される。

**Python 環境の注意**: `~/.local` の numpy/matplotlib は **aarch64 ビルド**
(BF3 ノードで `pip install --user` した物)。
- BF3 ノード上ならそのまま `python3 plot_paper_figs.py` で動く。
- x86 ログインノードでは `PYTHONNOUSERSITE=1` で ~/.local を回避し venv を使う:
  ```bash
  PY=/global/software/rocky-9.x86_64/modules/langs/python/3.10/bin/python3
  PYTHONNOUSERSITE=1 $PY -m venv ~/venv_plot_x86
  PYTHONNOUSERSITE=1 ~/venv_plot_x86/bin/pip install matplotlib
  PYTHONNOUSERSITE=1 ~/venv_plot_x86/bin/python plot_paper_figs.py
  ```
- **`pip install --user matplotlib` は絶対にしない** (x86 wheel が aarch64 環境を
  上書きして BF3 側が壊れる)。

## Issue #10: 低速/共有リンクで lossless 圧縮が無圧縮に勝つ

THP 環境では無圧縮 24.6 で圧縮は負けるが、実運用のリンク共有・低速リンクで実効帯域が
線速を下回ると、strict-lossless の byte_group が無圧縮を上回る。理論:
**lossless E2E = min(計算上限 ~9.3, 1.46×b)** が無圧縮(=b)を上回るのは **b < ~9.3 GB/s**。

実装: benchmark_e2e_rdma に `--rate-limit-gbps X`（leaky-bucket pacing。完了 wire 累積が
target を超えないよう送信側 main スレッドで busy-wait し実効帯域を X に制限）と
`--sharedlink-out FILE`（results_sharedlink 形式の1行追記）を追加。pacing 検証済
(target 9 → wire 9.0, 論理 E2E 13.1 = 1.46×9)。

```bash
sbatch build_rdma.sh          # benchmark_e2e_rdma 再ビルド (rate limiter 追加のため必須)
sbatch run_sharedlink.sh      # 2 ノード, → results_sharedlink.csv
cd src && python3 plot_paper_figs.py   # → figs/fig_sharedlink.{png,pdf}
```

- 帯域レベル {24,16,12,10,9.3,9,8,7,5,3} GB/s × 3 構成（無圧縮 / lz4+diff_byte(lossy) /
  lz4+byte_group(lossless)）を showdown best 構成（--thp 1 overlap=1 chunk=4M zerocopy=1）で測定。
- CSV: method,target_bw_gbps,measured_bw_gbps,codec,preproc,lossless,e2e_gbps,comp_ratio,
  verify,commit,timestamp。measured_bw = wire/(post+completion) median（≈target を検証）。
- ジョブ末尾 awk: target 帯域別の 3 構成 E2E 表 + byte_group>無圧縮 のクロスオーバー。
  byte_group は verify=EXACT（strict lossless）であること。
- Fig (`fig_sharedlink`): x=実効帯域(=無圧縮E2E), y=E2E。byte_group が無圧縮を上回る領域
  (b<~9.3) を網掛け + crossover 注記。**lossyに頼らない前向きな成果**（理論一致 ~9.3 GB/s）。

## §3.6: diff_byte 復元誤差の大きさ (ULP / 相対 / 絶対)

これまで mismatch_bytes (バイト数) しか測っておらず、誤差の「大きさ」が未測定だった。
`benchmark_diff_error.c` が全 423 テンソルで diff_byte forward→inverse を回し、
uint16(bf16) 要素単位の ULP / 相対 / 絶対誤差とサブチャンク位置×誤差を算出する。

```bash
sbatch build_generalization.sh    # benchmark_diff_error も含めるなら make benchmark_diff_error 追記
#  (個別: cd src && make benchmark_diff_error)
sbatch run_diff_error.sh          # 1 ノード, → results_diff_error.csv (反復不要・数分)
```

**重要な事実**: float diff_byte は **定数シフト** (`d=fp32_to_bf16(bf16_to_fp32(x)-shift)`,
shift=テンソル min) であり **累積 diff ではない**。累積和は整数版 idiff_byte 側 (そちらは
bit-exact 誤差ゼロ)。よって diff_byte は要素間で誤差を累積しない — 位置ビンが平坦なら
その実証になる (task 2)。

**ULP 距離の定義**: bf16 ビットパターン u を実数値に単調な順序キーへ写像し距離を取る
(sign-magnitude → 単調整数, Dawson 16bit 版):
`ord(u) = (u&0x8000) ? (0x8000-(u&0x7FFF)) : (0x8000+(u&0x7FFF))`、
`ulp = |ord(x')-ord(x)|`。±0 は同一視 (距離 0)、符号跨ぎはゼロを 1 段として数える
(+ε→−ε は 2 ULP)、NaN/Inf (指数 all 1) は統計から除外し n_special に計上。
相対誤差は x=0 を除外 (n_zero に記録)。

CSV (1 テンソル 1 行): tensor,category,layer_idx,act_or_grad,size_bytes,n_elem,n_zero,
n_special,preproc,elem_mismatch_pct,ulp_max,ulp_mean,ulp_pct_{0,1,2,3,4plus},
relerr_{max,mean},abserr_{max,mean},pos_ulp_b0..b9,sub_bytes,shift_val,commit,timestamp,status。
ジョブ末尾 awk: ① act/grad の ULP/誤差サマリ ② 位置×ULP (b0..b9) ③ category 別 ulp_mean
④ 未計測一覧。

**予備測定 (x86 scalar=RNE; BF3 は NEON=truncation で実パイプライン一致・要再測)**:
diff_byte は **±1 ULP に全く収まらない**。lm_head 活性 ulp_mean≈16 (1-2 ULP 主体だが
4+ ULP が ~6%, ulp_max≈15600)、**勾配は壊滅的** (lm_head 勾配 ulp_mean≈11600,
4+ ULP が 99.8%)。絶対誤差は小さい (勾配 1.5e-5) が、定数シフトの truncation 誤差が
shift スケールで載るため、小さい勾配値の **相対/ULP 精度が壊れる**。位置ビンは平坦
→ 累積ではなく per-element の現象。「±1 ULP near-lossless」という従来の表現は
絶対誤差閾値 (maxerr<1.0) に基づくもので、ULP/相対では誤り。

## Fig 4 / 5: OpenMP スレッドスケーリング

16 Arm コアの並列が各フェーズにどう効くかを定量化し「並列は必要だが十分でない
(前処理局所性・復元・メモリ帯域で頭打ち)」を示す。lm_head 65.7MB を diff_byte+LZ4
(主役) と none+snappy (対照: スケールするが無圧縮) で、スレッド数 1,2,4,8,16 を振る。

```bash
sbatch build_generalization.sh    # benchmark_omp_scaling も make all/このtarget で
# (個別: make benchmark_omp_scaling)
sbatch run_omp_scaling.sh         # 1 ノード, → results_omp_scaling.csv
cd src && python3 plot_paper_figs.py   # → figs/fig4_thread_scaling, fig5_phase_scaling
```

- `benchmark_omp_scaling.c`: スレッド数を `num_threads(p)` 句で内部制御 (omp_set_dynamic(0)、
  affinity は OMP_PLACES=cores/PROC_BIND=close)。各フェーズ独立 OMP 領域、256KB サブチャンク、
  DRAM write 込み、warmup3+iters7 median。
- CSV 列: threads,preproc,codec,phase,time_ms,gbps,speedup_vs_1t,efficiency,comp_ratio,
  commit_hash,timestamp。speedup(p)=T(1)/T(p), efficiency=speedup/p。
- Fig 4 (`fig4_thread_scaling`): 左=speedup(理想線形入り) / 右=efficiency。preproc/restore が
  理想に近く、compress/decompress が早期飽和 (efficiency が 16T で大きく低下) するのが要点。
- Fig 5 (`fig5_phase_scaling`): スレッド別フェーズ積み上げ (Fig8 と同系色)。総時間が縮みつつ
  構成比が変わる様子。
- CSV が無い場合 plot_paper_figs.py は Fig4/5 を自動スキップ (他図は生成)。

## データセット一般化測定 (全 423 テンソル × 4 preproc × 3 codec)

Llama-3.2-1B 全 16 層フル分解 = forward/(活性) 212 + grad/(勾配) 211 = **423 テンソル**を
全数測定 (間引かない)。各テンソルを byte_group / bit_group / diff_byte / idiff_byte ×
LZ4 / Snappy / Zstd-3 で回し、圧縮率・各フェーズ GB/s・bit-exact を測る。

```bash
sbatch build_generalization.sh     # snappy(C++)/zstd リンク, 初回数分
sbatch run_generalization.sh       # 1 ノード, → results_generalization.csv
```

- `benchmark_generalization.c`: ファイル名から category(18種)/layer_idx/act_or_grad を
  自動分類 (全 423 が分類済み, "other" ゼロを確認)。16T・256KB サブチャンク・DRAM write 込み・
  warmup3+iters7 の median。
- **堅牢性**: 小さすぎるテンソル (< 256KB; rotary 65536B のみ) はサブチャンク数 1 に
  フォールバック。1 個の失敗で全体を止めず status 列に ok/skip:理由/error:理由 を記録し続行、
  末尾でエラー一覧表示。実データは全サイズ 16 の倍数なので **skip/error は出ない想定**。
- CSV 列: tensor,category,layer_idx,act_or_grad,size_bytes,preproc,codec,comp_ratio,
  bit_exact,mismatch_bytes,gbps_{preproc,compress,decompress,restore},omp_threads,sub_bytes,
  nsub,commit_hash,timestamp,status。
- ジョブ末尾 awk サマリ: ① category×preproc 平均圧縮率(LZ4) ② 活性 vs 勾配(category別)
  ③ 層深さ×圧縮率 ④ diff_byte vs byte_group/idiff のカテゴリ間一貫性 ⑤ 未計測一覧
  ⑥ bit_exact サニティ (diff_byte=no, 他=yes)。
- **想定実行時間**: 総処理データ ~0.8GB × 4preproc × 3codec × 10iters × 4phase。
  zstd-3 圧縮と bit_group の scalar 逆変換が律速で **概ね 10〜30 分**
  (SBATCH 上限は安全側に 4h)。
- Zstd は level 3 (Pareto 高速端, 既定) 固定。LZ4 は block (E2E と同じ LZ4_compress_default)。

## Issue #4: byte_group / bit_group 単体の lossless E2E

idiff_byte は bit-exact だが圧縮率 1.32・E2E 8.16 GB/s で全環境で負けた。diff を使わない
**byte_group / bit_group は純粋な並べ替え (完全可逆) で、整数 diff と違い出力が圧縮しやすい**
はず → strict lossless で勝てる構成がないか確認する。

- `--preproc byte_group` : low/high バイト平面に分離 (`netzip_byte_group_serial` /
  新規 `netzip_byte_ungroup_serial`, NEON)。
- `--preproc bit_group`  : 16 ビットプレーンに分離 (`netzip_bit_group_serial` /
  新規 `netzip_bit_ungroup_serial`)。forward は NEON 転置で高速、inverse は scalar
  (restore スループットはこの非対称を反映)。bit_group は msg/chunk/sub が全て 16 の
  倍数である必要 (startup で検査; showdown 構成は満たす)。
- どちらも純並べ替え ⇒ verify=EXACT (memcmp 完全一致)。ならなければ実装バグ。

```bash
sbatch build_idiff.sh              # 4 種対応版を再ビルド (idiff と共通)
# task 2: 4 種単体比較 (byte/bit/diff/idiff) → results_lossless_compare.csv (1 ノード)
sbatch run_lossless_compare.sh
# task 3: byte_group/bit_group の E2E best 構成 → results_final_showdown.csv (2 ノード)
sbatch run_lossless_e2e.sh
```

判定: ① byte_group/bit_group が verify=EXACT、② 圧縮率と E2E が
near-lossless(diff_byte 1.98 / 19.3) と strict(idiff_byte 1.32 / 8.16) の間のどこか、
③ byte_group の E2E が THP 無圧縮 24.6 や 4KB 無圧縮 10.9 を上回れば主結果候補。

## Issue #5: idiff_byte (整数, bit-exact lossless) vs diff_byte (float, ±1ULP)

現行 diff_byte は浮動小数の算術 diff で復元時 ±1 ULP の丸め (77.8% の要素が lossy)。
idiff_byte は入力を生 uint16 列とみなしサブチャンク内で `d[i]=x[i]-x[i-1] (mod 2^16)`
→ byte grouping、逆変換は累積和。整数演算のみ → round-trip 完全可逆 (memcmp 一致)。
shift (netzip_find_min) は float ドメインの処理なので整数版では不要・未使用。
NEON ベクトル化済 (forward=ずらし減算, inverse=Hillis–Steele prefix-sum)。
実装は `netzip.c` の `netzip_idiff_byte_group_serial` / `netzip_idiff_byte_ungroup_serial`。

```bash
# ビルド (idiff_compare 新規 + e2e_rdma 再ビルド)
sbatch build_idiff.sh

# task 3: 単体比較 (圧縮率・各フェーズ速度・bit-exact 検証) → results_idiff_compare.csv
sbatch run_idiff_compare.sh        # 1 ノードでよい (MPI 不要)

# task 4: E2E best 構成を idiff 版でも (lossless 主結果) → results_final_showdown.csv
sbatch run_idiff_e2e.sh            # 2 ノード
```

- `benchmark_idiff_compare [--out CSV] [--iters N] [--warmup N] [--sub-kb K] file...`:
  16T・256KB サブチャンク連続割当 (run_direct 準拠)、各フェーズを独立領域で計時
  (DRAM write 込み)、warmup 後 median。各行に bit_exact / mismatch_bytes /
  comp_ratio / 4 フェーズの GB/s / commit / timestamp。複数ファイルで圧縮率の汎化確認。
- E2E: `benchmark_e2e_rdma --preproc idiff_byte|diff_byte` で切替 (既定 diff_byte)。
  idiff のとき verify は **memcmp 完全一致 (EXACT) を要求** (FAIL なら mismatch バイト数を表示)。
  CSV の preproc 列で区別。
- **完了判定**: ① idiff 行が全て bit_exact=yes / verify=EXACT、
  ② comp_ratio が diff_byte 比でどれだけ落ちるか (1.9 前後維持なら採用)、
  ③ 各フェーズ速度が diff_byte 同等か。

## MPI フォールバック (ibverbs がどうしても動かない場合)

1. RDMA transport であることを確認: ノード上で `ucx_info -d | grep -B1 -A3 "rc"`
   (rc/rc_mlx5 が出ること)
2. `run_e2e_breakdown.sh` 末尾のコメントアウト行を有効化
   (`--transport mpi` + `-x UCX_TLS=rc -x UCX_NET_DEVICES=mlx5_0:1`)。
   CSV の transport 列に mpi と記録される。
3. ビルド自体が `-libverbs` で失敗する場合: `make benchmark_e2e_rdma_noib` で
   MPI 専用バイナリをビルド (`--transport mpi` のみ受け付ける)。

## 完了条件チェックリスト

1. [ ] サニティ合格 (4M warm ≈24 / 60M級 ≈11 GB/s, data verify PASS)
2. [ ] `results_e2e_breakdown.csv` (圧縮あり/なし × 一括/4MiB × 逐次/overlap)
3. [ ] `results_msgsize_sweep.csv` (無圧縮 vs lz4+diff_byte × 15 サイズ)
4. [ ] `results_cachehot.csv` (hot/cold_send/cold_recv/cold_both × チャンク2条件)
5. [ ] 上記 Case A/B 判定 (gbps_compress の median を見る)
