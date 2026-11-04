# 実験の再現

[English](REPRODUCING.md) | [日本語](REPRODUCING_ja.md)

この文書では、論文の実験とこのディレクトリのファイルの関係、プログラムのビルドと実行の方法、
完全な再実行に足りないものを説明します。特に断らない限り、パスは `netzip_bf3/` からの相対パスです。

以下のコマンドとオプションは、このディレクトリのソースコードとスクリプトに照らして確認しました。
さらに 2026-11-03 に、commit `8509f51` を新しく clone したものを、論文で使ったクラスタの BlueField-3 ノード 2 台で実行しました。
実行したのは、`bf3_deploy/build_rdma.sh` によるビルド（4 節）、`bf3_deploy/run_rdma_sanity.sh`、6.6 節の図 8 の実行例 3 つ
（それぞれ `--thp 0` と `--thp 1`）です。図 8 の実行例の入力には、このリポジトリに含まれていない論文の入力テンソル（5 節）を使いました。
どれも最後まで実行でき、`benchmark_e2e_rdma` の実行はすべて `verify=EXACT` を出力しました。
この文書のそれ以外のコマンドは実行していません。

## 1. このリポジトリから再現できること

| 目的 | このリポジトリだけで可能か |
|---|---|
| 既存の測定結果から論文の図を描き直す | **できません。** 測定結果の CSV ファイルと、公開された図に使った作図スクリプトが含まれていません（8 節）。 |
| BlueField-3 の実機で測定し直す | **一部のみ可能です。** RDMA と end-to-end の測定（図 6〜8）のベンチマークプログラムは、このリポジトリからビルドできます。入力テンソル、図 6〜8 の実行スクリプトそのもの、選定測定（図 1）に必要な Zstandard と Snappy のソースは含まれていません（11 節）。 |
| 一般的な PC で性能を再現する | **できません。** 測定には、InfiniBand/RDMA で接続した 2 台の BF3 DPU と、BF3 の Arm コアが必要です。前処理のコードは aarch64 で Arm NEON の組み込み関数（intrinsics）を使い、RDMA のプログラムには libibverbs と MPI が必要です。 |

## 2. コードの来歴

このディレクトリのファイルは、著者の開発用ツリーのコピーとして、2026-06-22 にこのリポジトリへ
コミットされました（commit `0128b40`）。
スクリプトの既定のパス（`$HOME/MICRO-2025-NetZIP/netzip_c/...`）は、著者の開発用ツリー（`netzip_c/`）を指しています。
この開発用ツリーは、NetZIP アーティファクト（<https://github.com/ece-fast-lab/MICRO-2025-NetZIP>）をローカルに clone した
ディレクトリの中に置いていました。`netzip_c/` は同アーティファクトの一部ではありません。

論文の図のもとになった測定データには、そのデータを生成した開発用リポジトリのコミットが
`commit_hash` 列に記録されています。図 6、7、8 は `e125de3`、図 1 は `42c75b6` です。
これらはこのリポジトリのコミットではありません。図 6、7、8 のデータは、このコピーをコミットした後の
2026-06-24 と 2026-07-06 に記録されました。図 1 のデータは 2026-06-18 に記録されました。

2026-10-29 に、このディレクトリのファイルを開発用リポジトリと比較しました。

- 図 6、7、8：`bf3_deploy/src/` の `benchmark_e2e_rdma.c`、`rdma_transport.c`、
  `rdma_transport.h`、`netzip.c`、`netzip.h`、`lz4.c`、`lz4.h`、`Makefile` は、commit `e125de3` の
  版と同一です。
- 図 1：`benchmark_generalization.c`、`netzip.c`、`netzip.h` は、commit `42c75b6` の版と同一です。
  ここにある `Makefile` には、そのコミットの後に加えた行があります（13 行追加、1 行削除）。

このディレクトリのプログラムを Git のチェックアウトの中で実行すると、同じ列には、その
チェックアウトの現在の `HEAD`（`git rev-parse --short HEAD`）が記録されます
（`bf3_deploy/src/benchmark_e2e_rdma.c`、`benchmark_generalization.c`）。

## 3. ハードウェアとソフトウェア

| 項目 | 論文（表 1） | このリポジトリに記録されているもの |
|---|---|---|
| ノード | 2 × NVIDIA BlueField-3 DPU | 2 ノードの SLURM スクリプト（`run_*.sh`） |
| Arm コア | DPU あたり 16 × Cortex-A78 | コアの種類のみ（Cortex-A78、`bf3_deploy/src/Makefile` のコメント）。コア数は記録なし |
| メモリ | 32 GB DDR5（DDR5-5600 コントローラ 2 基） | 記録なし |
| ネットワーク | 200 Gb/s InfiniBand | 記録なし |
| OS | Rocky Linux 9.4、aarch64 | スクリプト中のモジュールパス `rocky-9.aarch64` |
| コンパイラ | GCC 11.4.1 | スクリプト中のモジュール `gcc/11` |
| MPI | HPC-X 2.24.1-mt | スクリプト中のモジュール `hpcx/2.24.1`（10 節を参照） |
| RDMA | raw ibverbs、RC QP、RDMA Write with Immediate | `bf3_deploy/src/rdma_transport.c` |
| LZ4 | 1.9.4、同梱 | `bf3_deploy/src/lz4.h`（バージョン 1.9.4） |
| xxHash | — | `bf3_deploy/src/xxhash.h`（バージョン 0.6.5） |
| Zstandard、Snappy | — | `bf3_deploy/deps/zstd-1.5.7` と `bf3_deploy/deps/snappy-1.1.9` に置く前提（含まれていません） |

libibverbs/rdma-core、zlib、OpenMP ランタイム、DPU ファームウェアのバージョンは、
このリポジトリには記録されていません。

## 4. ビルド

BF3 の Arm（aarch64）ノード上でビルドします。論文で使ったモジュール環境では次のとおりです。

```bash
module load gcc/11
module load hpcx/2.24.1
cd netzip_bf3/bf3_deploy/src
make rdma_sanity benchmark_e2e_rdma
```

論文で使ったクラスタでは、同じビルドを `bf3_deploy/build_rdma.sh` で行います
（`bf3_deploy/` から `sbatch` で投入します）。ビルドスクリプトにはクラスタ固有の設定
（SLURM のパーティションとノード名、`MODULEPATH`）が入っています。お使いの環境に合わせて
書き換えてください。

コンパイラのフラグは `bf3_deploy/src/Makefile` で定義しています。MPI/RDMA のプログラムは
`mpicc -O3 -funroll-loops -fopenmp -march=armv8.2-a+fp16 -pthread -I.` を使います。
Zstandard と Snappy をリンクするプログラムは、`-Wall -O3 -funroll-loops -fopenmp -DPROFILE` と
`../deps` 以下のインクルードパスを使います。

| make ターゲット | ソース | このリポジトリからビルドできるか |
|---|---|---|
| `rdma_sanity` | `rdma_sanity.c`、`rdma_transport.c` | できる（MPI と libibverbs が必要） |
| `benchmark_e2e_rdma` | `benchmark_e2e_rdma.c`、`rdma_transport.c`、`netzip.c`、`lz4.c` | できる（MPI と libibverbs が必要） |
| `benchmark_e2e_rdma_noib` | `benchmark_e2e_rdma.c`、`netzip.c`、`lz4.c`（`rdma_transport.c` なし）を `-DNO_IBVERBS` でビルド | できる（MPI が必要。`--transport mpi` のみ受け付ける） |
| `rdma_targets` | `rdma_sanity` と `benchmark_e2e_rdma` をビルド | できる（MPI と libibverbs が必要） |
| `benchmark_idiff_compare` | `benchmark_idiff_compare.c`、`netzip.c`、`lz4.c` | できる |
| `benchmark_diff_error` | `benchmark_diff_error.c`、`netzip.c` | できる |
| `benchmark_generalization`、`benchmark_omp_scaling`、`benchmark_pipeline` | 各ソース、`netzip.c`、LZ4、Snappy、Zstandard のオブジェクト | できない：`bf3_deploy/deps/zstd-1.5.7` と `bf3_deploy/deps/snappy-1.1.9`、zlib、C++ コンパイラが必要 |
| `benchmark`、`dump_compressed`、`benchmark_parallel`、`benchmark_l3`、`benchmark_parallel_fast`、`fast`、`benchmark_gprof`、`benchmark_gprof_internal`、`benchmark_e2e` | ソースがこのリポジトリにない（`benchmark.c`、`dump_compressed.c`、`benchmark_parallel.c`、`benchmark_l3.c`、`benchmark_e2e.c`） | できない |
| `all`（既定） | `benchmark`、`dump_compressed`、`benchmark_parallel`、`benchmark_pipeline`、`benchmark_l3` | できない：ソースの多くがなく、`benchmark_pipeline` には `bf3_deploy/deps/` が必要 |

`benchmark_decompress.c`（`netzip_bf3/` の直下）には、この Makefile にビルド規則がありません。
`bf3_deploy/run_decomp_ladder.sh` は親ディレクトリ `netzip_bf3/` で `make benchmark_decompress` を
実行しますが、このリポジトリの `netzip_bf3/` には Makefile がありません。また、このプログラムは
`bf3_deploy/src/netzip.c` に定義されていない関数を呼んでいます。

引数なしで `make` を実行すると既定のターゲット `all` がビルドされ、ソースの多くがないため失敗します。
ターゲットは必ず指定してください。

図 1 には、Zstandard 1.5.7 と Snappy 1.1.9 のソースを `bf3_deploy/deps/` に置き、`bf3_deploy/src/` で
`make benchmark_generalization` を実行します（クラスタでは、`bf3_deploy/` から `bf3_deploy/build_generalization.sh` を
`sbatch` で投入します）。`bf3_deploy/build_idiff.sh` は `benchmark_idiff_compare` をビルドし、`benchmark_e2e_rdma` を
ビルドし直します。
Snappy 1.1.9 のソースには `snappy-stubs-public.h` が含まれていません。このファイルは Snappy の CMake の設定が
`snappy-stubs-public.h.in` から生成するもので、`Makefile` は `-I../deps/snappy-1.1.9` を加えるだけです。著者が
`bf3_deploy/deps/` をどう準備したかは記録がなく、確認していません。また、ここにある `Makefile` は commit `42c75b6` の
ものと異なります（2 節）。

## 5. 入力データ

入力テンソルは、このリポジトリには**含まれていません**。

- `benchmark_e2e_rdma --data PATH` は、ファイルの先頭 `--msg-size` バイトを bfloat16 の生の値として
  読みます。両方の MPI ランクがファイルを読むので、両ノードの同じパスにファイルが必要です。
  `--data` を省略すると合成データを生成します。その圧縮の振る舞いは論文のテンソルとは異なります。
- 論文の end-to-end の測定には、Llama-3.2-1B の `lm_head` の活性化値（activation）テンソル 1 個
  （65,667,072 バイト）を使いました。実行スクリプトでは `bf3_data/forward/0212_lm_head.pkl.bin` として参照しています。
- 選定測定（図 1）には、Llama-3.2-1B の活性化値と勾配の全 423 テンソルを使いました。
  `run_generalization.sh` は、`bf3_deploy/src/` からの相対パスで `../../bf3_data/forward/*.pkl.bin`
  と `../../bf3_data/grad/*.pkl.bin` にあることを前提にしています。`benchmark_generalization` は、
  パスに `/forward/` と `/grad/` のどちらが含まれるかで、活性化値か勾配かを判定します。
- これらの `.pkl.bin` ファイルをモデルからどう作ったかは、このリポジトリには記載されていません。

## 6. 実験の実行

### 6.1 起動方法

`benchmark_e2e_rdma` と `rdma_sanity` は、BF3 ノード 1 台につき 1 つ、ちょうど 2 つの MPI ランクで
実行する必要があります。このリポジトリの実行スクリプトは次を使っています。

```bash
mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0 \
  ./benchmark_e2e_rdma [options]
```

`bf3_deploy/src/` で実行します。スクリプトは `$SLURM_SUBMIT_DIR/src` に移動するので、
`bf3_deploy/` から投入します。

`bf3_deploy/src/rdma_transport.c` が読む環境変数：

| 変数 | 意味 |
|---|---|
| `NETZIP_IB_DEV` | 使う HCA。例：`mlx5_0`。既定では、ACTIVE なポートを持つ最初のデバイス。 |
| `NETZIP_GID_INDEX` | RoCE（Ethernet リンク層）の場合の GID インデックス。既定は 3。InfiniBand では使いません。 |

### 6.2 `benchmark_e2e_rdma` のオプション

既定値は `bf3_deploy/src/benchmark_e2e_rdma.c` のものです。これ以外の引数を与えると、
`unknown arg` と表示して終了します。

| オプション | 既定値 | 意味 |
|---|---|---|
| `--data PATH` | なし（合成データ） | 入力ファイル、bfloat16 の生データ |
| `--out CSV` | `results_e2e_breakdown.csv` | 出力 CSV（追記） |
| `--msg-size N` | 65667072 | 1 回の転送のバイト数 |
| `--chunk-size N` | メッセージサイズ（1 チャンク） | チャンクのバイト数 |
| `--sub-kb K` | 256 | タイル（サブチャンク）の大きさ（KiB）。各タイルが 1 つの LZ4 ブロックになります |
| `--compress 0\|1` | 1 | 0 = 無圧縮の RDMA、1 = 圧縮経路 |
| `--preproc NAME` | `diff_byte` | `byte_group`、`bit_group`、`idiff_byte`（可逆）または `diff_byte`（非可逆） |
| `--overlap 0\|1` | 0 | 1 = 次のチャンクの圧縮と、現在のチャンクの転送を重ねる |
| `--zerocopy 0\|1` | 0 | 1 = LZ4 が登録済みの wire バッファへ直接書く（pack のコピーなし）。ibverbs のみ |
| `--thp 0\|1` | 0 | 1 = 主なバッファを 2 MiB 境界で確保し、`madvise(MADV_HUGEPAGE)` で THP を要求する |
| `--iters N` | 10 | 計測する反復の回数（最大 64） |
| `--warmup N` | 3 | ウォームアップの反復回数（記録されません） |
| `--flush-iter 0\|1` | 1 | 各反復の前に両ノードで最終レベルキャッシュを追い出す（計時の外） |
| `--lockstep 0\|1` | 0 | チャンクごとに受信側を待つ |
| `--phase-shift 0\|1` | 0 | 前処理と LZ4 を別の段に分ける。overlap が強制されます |
| `--pre-threads N` | 0（全スレッド） | 前処理のスレッド数。1 から `OMP_NUM_THREADS` − 1 の値を与えると、前処理と LZ4 を別々の（融合しない）段として実行します。LZ4 は常に全スレッドを使います |
| `--cold none\|send\|recv\|both` | `none` | 圧縮後または受信後にキャッシュを追い出す（別に計時して差し引く） |
| `--transport ibverbs\|mpi` | `ibverbs` | データ経路。`mpi` は `MPI_Isend`/`MPI_Irecv` を使います |
| `--rate-limit-gbps G` | 0（無効） | チャンクの完了後に待ちを入れ、実効リンク帯域を G GB/s に制限する |
| `--sharedlink-out CSV` | なし | 実行ごとに（帯域制限の有無にかかわらず）要約行を 1 行、この CSV にも追記する |
| `--shift auto\|X` | `auto` | `diff_byte` が差し引くシフト値。`auto` はデータから求めます |

サイズはすべて偶数（bfloat16 の要素単位）である必要があります。`--preproc bit_group` のときは、
`--msg-size`、`--chunk-size`、タイルの大きさが 16 の倍数である必要もあります。

### 6.3 論文で使った実行モード

| 論文での呼び方 | オプション |
|---|---|
| 無圧縮 RDMA | `--compress 0`。送信側は、チャンクごとに 1 つの RDMA Write with Immediate を、登録済みの送信元バッファから受信側の登録済みの受信先バッファへ直接発行します（前処理、LZ4、pack、wire バッファなし）。チャンクサイズが既定値なら、メッセージ全体を 1 回で転送します。 |
| 圧縮、逐次版 | `--compress 1 --preproc byte_group`。チャンクサイズは既定（1 チャンク）、`--overlap 0`、`--zerocopy 0`。送信側は全タイルを圧縮し、wire バッファへコピー（pack）してから送ります。 |
| 圧縮、チャンク単位の overlap | `--compress 1 --preproc byte_group --overlap 1 --chunk-size 4194304 --zerocopy 1`。チャンク c+1 の圧縮とチャンク c の転送が重なり、pack のコピーはありません。 |
| 4 KiB ページ / THP | `--thp 0` / `--thp 1` |

ソースコードから分かる実装上の注意：

- `--thp 1` のとき、プログラムは `/proc/self/smaps` の `AnonHugePages` を読み、`page_mode` 列に
  `thp_madvise`（ヒュージページあり）または `thp_no_2m` を記録します。`--thp 0` のときは
  `smaps` を読まずに `base_4k` と記録します。2 つのランクで異なる場合は `mixed` と記録します。
- `--zerocopy 1` のとき、各チャンクは、空でないタイルごとに完了通知を生成しない（unsignaled）
  RDMA Write を 1 つずつ送り、最後にサイズヘッダを運ぶ完了通知つき（signaled）の
  RDMA Write with Immediate を送ります。
- メモリ登録（`ibv_reg_mr`）は計時の外です。その時間は `mr_reg_ms` に 1 回だけ記録されます。

### 6.4 コードの計測手順

各実行は、ウォームアップの反復を行ってから、計測する反復を行います。各反復では、受信側が受信要求を
先に出し、両ランクが `MPI_Barrier` で揃い、両ランクが最終レベルキャッシュを追い出し
（`--flush-iter 1` のとき）、両ランクが 2 回目のバリアで揃ってから計時を開始し、送信側と受信側が
1 回の転送を行い、両ランクが 3 回目のバリアで揃った時点で計時を終了します。最後の計測反復のあと、受信側は
復元したテンソルを元のテンソルと比較します。可逆の前処理と `--compress 0` では、バイト単位の
完全一致を求め、`verify` 列に `EXACT`（または `FAIL`）を記録します。

### 6.5 このリポジトリの実行スクリプト

スクリプトはすべて、論文で使ったクラスタ向けの SLURM のジョブスクリプトです。パーティション、
ノード名、`MODULEPATH` を設定しています。テンソルを読むスクリプトの既定のデータパスは、
著者のホームディレクトリ以下か、`bf3_deploy/src/` からの相対パス（`../../bf3_data`）です。
使う前に書き換えてください。コメントと表示文の図番号は、この研究の以前の版（草稿と SWoPP 論文）のものです。

| スクリプト | 実行する内容 | 出力（`bf3_deploy/src/` に作成） |
|---|---|---|
| `run_rdma_sanity.sh` | 2 ノードで `rdma_sanity` と `rdma_sanity --hugepages` | `results_rdma_sanity.csv` |
| `run_e2e_breakdown.sh` | `benchmark_e2e_rdma` を `--compress 0`、`--compress 1`、4 MiB チャンク、4 MiB チャンク＋overlap で実行（既定の `diff_byte`） | `results_e2e_breakdown.csv` |
| `run_msgsize_sweep.sh` | `benchmark_e2e_rdma` をメッセージサイズの一覧で、`--compress 0` と `1`（既定の `diff_byte`）、1 チャンクと、4 MiB チャンク＋overlap で実行 | `results_msgsize_sweep.csv` |
| `run_cachehot.sh` | `--lockstep 1` と `--cold` によるキャッシュの hot/cold の比較 | `results_cachehot.csv` |
| `run_final_showdown.sh` | THP：無圧縮の基準と、4/8 MiB チャンクの overlap に zero-copy、phase-shift、`--pre-threads 12` を組み合わせた実行（既定の `diff_byte`） | `results_final_showdown.csv` |
| `run_fig8_fill.sh` | THP の圧縮逐次版を 1 本（既定の `diff_byte`、1 チャンク） | `results_final_showdown.csv` |
| `run_idiff_e2e.sh` | THP で overlap＋zero-copy、`diff_byte` と `idiff_byte` | `results_final_showdown.csv` |
| `run_lossless_e2e.sh` | THP で overlap＋zero-copy、`byte_group` と `bit_group` | `results_final_showdown.csv` |
| `run_sharedlink.sh` | 帯域を制限した実行（`--rate-limit-gbps`） | `results_e2e_breakdown.csv`、`results_sharedlink.csv` |
| `run_generalization.sh` | 全テンソルで `benchmark_generalization` | `results_generalization.csv` |
| `run_omp_scaling.sh` | `benchmark_omp_scaling` | `results_omp_scaling.csv` |
| `run_idiff_compare.sh` | 6 個のテンソルで `benchmark_idiff_compare` | `results_idiff_compare.csv` |
| `run_lossless_compare.sh` | 6 個のテンソルで `benchmark_idiff_compare` | `results_lossless_compare.csv` |
| `run_diff_error.sh` | 全テンソルで `benchmark_diff_error` | `results_diff_error.csv` |
| `run_decomp_ladder.sh` | `benchmark_decompress`（このリポジトリからはビルドできない） | `results_decomp_step*.csv` |

このうち、公開された図（図 1）について記録されたプログラムと設定で実行するのは
`run_generalization.sh` だけです。それでも実行には入力テンソルと Zstandard/Snappy のソースが必要で、
その出力は公開されたデータと比較していません。
E2E のスクリプトは `--iters 10 --warmup 3` を渡しており、どれも図 6〜8 の設定そのものでは
実行しません。

### 6.6 公開された図のもとになった設定

図 6〜8 の実行スクリプトは含まれていません。以下の設定は、著者の測定データ（含まれていません）の
設定列に記録されているもの、および論文に記載されているものです。

| 図 | プログラム | 設定 |
|---|---|---|
| 図 1（選定） | `benchmark_generalization` | 423 個のテンソルファイル、OpenMP 16 スレッド、256 KiB のタイル（256 KiB より小さいテンソルは 1 タイルとして処理）、4 種の前処理 × LZ4/Snappy/Zstd（Zstd のレベルはソースで 3 に固定）。`run_generalization.sh` は `--iters 7 --warmup 3` を渡します。図には可逆の 3 モードを示しています。 |
| 図 6（RDMA 帯域とメッセージサイズ） | `benchmark_e2e_rdma` | `--compress 0`、メッセージあたり 1 チャンク、`--thp 0` と `--thp 1`、メッセージサイズは 1 KiB から 65,667,072 バイトまで、各点 5 回の実行。計測反復は 1 回の実行あたり、256 KiB までは 15 回、512 KiB からは 8 回（行数から推定。ウォームアップの回数は記録されていません）。描いた値は、`median` 行の `gbps_xfer` の 5 回の実行の平均です。 |
| 図 7（Arm 側の段ごとの時間とスレッド数） | `benchmark_e2e_rdma` | `--compress 1 --preproc byte_group --thp 1`、1 チャンク、`--overlap 0 --zerocopy 0`、`OMP_NUM_THREADS` = 1、2、4、8、16、それぞれ 20 回の計測反復。 |
| 図 8（end-to-end のスループット） | `benchmark_e2e_rdma` | 65,667,072 バイト、16 スレッド、256 KiB のタイル、`--thp 0` と `--thp 1` のそれぞれで 3 モード（6.3 節）、それぞれ 50 回の計測反復。 |

論文は end-to-end の結果について「5 回のウォームアップのあと 50 回の計測反復の中央値」と
記載しています。ウォームアップの回数は出力に記録されません。

`benchmark_generalization` の引数は `[--out CSV] [--iters N] [--warmup N] [--sub-kb K] FILE...` です
（既定値は `results_generalization.csv`、5、2、256）。スレッド数は `OMP_NUM_THREADS` に従います。最後の完全なタイルより
後ろの端数バイトは処理しません。タイルの大きさが 16 バイトの倍数でないと、`bit_group` は飛ばされます
（状態は `skip:bit_group_needs_16x`）。

以下は、上のオプションから組み立てた、4 KiB ページでの図 8 の実行例です（THP では `--thp 1`）。
`DATA` は 65,667,072 バイト以上の bfloat16 の生データで、両ノードの同じパスに置きます。

```bash
MPIRUN="mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0"

# uncompressed
$MPIRUN ./benchmark_e2e_rdma --data "$DATA" --out results_e2e.csv \
  --msg-size 65667072 --compress 0 --thp 0 --iters 50 --warmup 5

# compressed, serial
$MPIRUN ./benchmark_e2e_rdma --data "$DATA" --out results_e2e.csv \
  --msg-size 65667072 --compress 1 --preproc byte_group --thp 0 \
  --iters 50 --warmup 5

# compressed, chunk-level overlap without pack copy
$MPIRUN ./benchmark_e2e_rdma --data "$DATA" --out results_e2e.csv \
  --msg-size 65667072 --compress 1 --preproc byte_group --overlap 1 \
  --chunk-size 4194304 --zerocopy 1 --thp 0 --iters 50 --warmup 5
```

## 7. 出力の形式

`benchmark_e2e_rdma` は、ランク 1 が現在のディレクトリに CSV を書き、既存のファイルには追記します。
既存のファイルのヘッダが異なる場合は、そのファイルを `<name>.old.<unixtime>` に改名して新しく
作ります。各実行は、計測した反復ごとに `raw` 行を 1 行（`iter` = 0, 1, …）と、`median`、`min`、
`max` の行を 1 行ずつ（`iter` = -1）追記します。

列（CSV のヘッダ行）：

```text
codec,preproc,msg_size_bytes,chunk_size_bytes,sub_size_bytes,wire_bytes,wire_footprint_bytes,
buffer_placement,transport,overlap,lockstep,page_mode,
zerocopy,phase_shift,pre_threads,omp_threads,stat,iter,
t_preproc_ms,t_compress_ms,t_pack_ms,t_post_ms,t_completion_ms,
t_wait_recv_ms,t_decompress_ms,t_restore_ms,t_evict_excl_ms,
t_total_ms,t_unaccounted_ms,
gbps_preproc,gbps_compress,gbps_xfer,gbps_decompress,gbps_restore,
throughput_gbps,comp_ratio,shift_val,mr_reg_ms,verify,commit_hash,timestamp
```

`throughput_gbps` はメッセージサイズを `t_total_ms` で割った値（10 進の GB/s）、`comp_ratio` は
メッセージサイズを `wire_bytes` で割った値です。

`benchmark_generalization` は、既定では `results_generalization.csv` に追記します。列は次のとおりです。

```text
tensor,category,layer_idx,act_or_grad,size_bytes,preproc,codec,
comp_ratio,bit_exact,mismatch_bytes,
gbps_preproc,gbps_compress,gbps_decompress,gbps_restore,
omp_threads,sub_bytes,nsub,commit_hash,timestamp,status
```

## 8. 作図

公開された図を描いた作図スクリプトは含まれていません。著者の作図スクリプト（含まれていません）に
よると、描いた量は次のとおりです。

- 図 1：テンソルごとに、圧縮倍率（compression factor、元のサイズ ÷ 圧縮後のサイズ）を、送信側の経路
  （前処理のあと圧縮）と受信側の経路（伸長のあと復元）のうち遅い方の経路のスループットに対して
  描いたもの。各経路のスループットは、段ごとのスループット a、b から 1/(1/a + 1/b) で合成します。
  前処理 × コーデックの組合せごとの中央値を示しています。
- 図 6：6.6 節を参照。
- 図 7：`median` 行の段ごとの時間（`t_preproc_ms`、`t_compress_ms`、`t_pack_ms`、
  `t_decompress_ms`、`t_restore_ms`）。
- 図 8：`median` 行の `throughput_gbps`。

`bf3_deploy/src/plot_paper_figs.py` は、この研究の以前の版の作図スクリプトです（冒頭に SWoPP 論文用とあります）。`bf3_deploy/src/` の
CSV ファイル（たとえば `results_rdma_sanity.csv`、`results_msgsize_sweep.csv`、
`results_final_showdown.csv`、`results_e2e_breakdown.csv`、`results_generalization.csv`、
`results_omp_scaling.csv`）を読み、`bf3_deploy/src/figs/` に PNG と PDF を書きます。
matplotlib を入れた Python が必要です。その図は主に非可逆の `diff_byte` の構成にもとづいており、
公開された論文の図には対応しません。入力の CSV ファイルがないとエラーで止まります。

## 9. 論文の図と表

| 論文の項目 | このリポジトリのプログラム | このリポジトリの実行スクリプト | データと作図スクリプト |
|---|---|---|---|
| 図 1、選定 | `bf3_deploy/src/benchmark_generalization.c` | `bf3_deploy/run_generalization.sh` | 含まれていない |
| 図 2〜5、模式図 | — | — | 含まれていない |
| 表 1、評価環境 | — | — | 3 節 |
| 図 6、RDMA 帯域とメッセージサイズ | `bf3_deploy/src/benchmark_e2e_rdma.c` | 含まれていない | 含まれていない |
| 図 7、Arm 側の段ごとの時間とスレッド数 | `bf3_deploy/src/benchmark_e2e_rdma.c` | 含まれていない | 含まれていない |
| 図 8、end-to-end のスループット | `bf3_deploy/src/benchmark_e2e_rdma.c` | 含まれていない（最も近いのは THP の overlap の棒に対応する `run_lossless_e2e.sh`。ただし `--iters 10 --warmup 3`） | 含まれていない |

論文の `osu_bw`（OSU Micro-Benchmarks）の数値は、このリポジトリのどのファイルにも対応しません。

## 10. 手作業の手順と外部への依存

- 入力テンソルを両ノードの同じパスにコピーし、`--data` で指定します。
- `benchmark_generalization` をビルドする前に、Zstandard 1.5.7 と Snappy 1.1.9 のソースを
  `bf3_deploy/deps/` に置きます。
- `build_*.sh` と `run_*.sh` の SLURM ヘッダ（パーティション、ノード名）、`MODULEPATH`、
  モジュール名、既定のデータパスを、お使いのクラスタに合わせて書き換えます。
- 既定の HCA の選択が望むものと違う場合は、`NETZIP_IB_DEV` を設定します。
- 4 KiB ページでの実行では、プログラムは `smaps` を確認しません。その確認が必要な場合は、
  `/proc/<pid>/smaps` の `AnonHugePages` を自分で確認してください。
- `make clean` は `../deps` 以下のオブジェクトファイルも削除します。
- `--thp 1` には、`/sys/kernel/mm/transparent_hugepage/enabled` が `[always]` か `[madvise]` である必要があります。
  そうでないと `page_mode` は `thp_no_2m` と記録されます。`[always]` のときは `--thp 0` のバッファにもヒュージページが
  使われ得ますが、プログラムは `base_4k` と記録します。`run_rdma_sanity.sh` と `run_final_showdown.sh` はこの設定を表示します。
- 論文の `osu_bw` の数値には OSU Micro-Benchmarks が必要です。このリポジトリには含まれていません。
- `bf3_deploy/src/netzip.c` の `netzip_bit_group` と `netzip_bit_group_serial` は、要素数 N が 8 の倍数
  （大きさが 16 バイトの倍数）であることを前提にしています。それ以外の N では最大 16 × ceil(N/8) バイトまで書き込み、
  `netzip.h` が `netzip_bit_group` について示す 2N バイトの出力バッファを超えます。その場合、出力の一部は書き込み前に 0 で消されません。
  逆変換の `netzip_bit_ungroup` と `netzip_bit_ungroup_serial` も同じ前提で、それ以外の N では最大 16 × ceil(N/8) バイトまで読み込みます。
  このリポジトリのプログラムは、条件を満たす大きさでしかこれらの関数を呼びません。
  影響があるのは、自分のコードからこれらの関数を呼ぶ場合だけです。

### 論文とこのリポジトリの相違点

| 項目 | 論文 | このリポジトリ |
|---|---|---|
| 反復回数 | 5 回のウォームアップのあと 50 回の計測反復の中央値（E2E の結果） | プログラムの既定値は `--iters 10 --warmup 3`。E2E の実行スクリプトはすべてこの値を渡しています |
| MPI | HPC-X 2.24.1-mt | スクリプトは `hpcx/2.24.1` を読み込みます。Makefile のコメントには、通常版（`-mt` ではない）で足りるとあります |
| 4 KiB モードの確認 | `/proc/<pid>/smaps` で AnonHugePages を確認 | `--thp 0` のとき、プログラムは確認しません |
| キャッシュの追い出し | 記載なし | 既定で各反復の前に最終レベルキャッシュを追い出します（`--flush-iter 1`、計時の外） |
| チャンク分割 | 65.7 MB のバッファを 4 MiB の RDMA チャンクに分割（主構成では 16 チャンク） | 記録されたデータでは、4 MiB のチャンクを使うのは overlap の実行だけです。図 8 の無圧縮と逐次版の実行は、65,667,072 バイトの 1 チャンクです |
| 前処理の既定値 | 評価した方式はバイトグループ化＋LZ4 | `--preproc` の既定値は非可逆の `diff_byte`。`--preproc byte_group` の指定が必要です |
| メッセージサイズの既定値（コメント） | — | `benchmark_e2e_rdma.c` 冒頭のコメントは `--msg-size` の既定値を「ファイルサイズ or 65667072」としていますが、`--msg-size` を省くと、コードは常に 65667072 を使います |
| `make all` | — | `MEASUREMENT_GUIDE_RDMA.md` は `make all` で `benchmark_omp_scaling` がビルドされるように書いていますが、`all` ターゲットには含まれておらず、`all` のソースも不足しています |
| `diff_byte` をほぼ可逆とする記述 | 評価していない。論文が評価するのは、ビットが完全に一致する方式（バイトグループ化、ビットグループ化、整数差分＋バイトグループ化）だけです | `netzip.c`、`netzip.h`、`benchmark_e2e_rdma.c`、`benchmark_idiff_compare.c`、`benchmark_generalization.c`、`run_generalization.sh`、`run_idiff_e2e.sh`、`run_lossless_compare.sh`、`run_lossless_e2e.sh`、`MEASUREMENT_GUIDE_RDMA.md` のコメントと表示文は、非可逆の `diff_byte` を、誤差が「1 ULP」や「±1 ULP」、あるいは「near-lossless」と書いています。`MEASUREMENT_GUIDE_RDMA.md` の「§3.6」の節は、`diff_byte` の誤差が ±1 ULP に全く収まらないと書いています。ただし同節はその数値を x86（スカラー、最近接偶数丸め）での予備測定とし、BF3 の NEON 経路は切り捨て丸めなので測り直しが必要と書いています |
| 4 KiB の崖の原因 | アドレス変換の粒度と関係し、RNIC 側のアドレス変換の負荷と整合的。MTT ミスの回数は数えていません | `benchmark_e2e_rdma.c` と `MEASUREMENT_GUIDE_RDMA.md` は、原因を NIC の MTT ミスと書いています。`rdma_sanity.c` の表示文は、ヒュージページで回復すれば原因は NIC の MTT ミスである、と判定します |
| 圧縮が無圧縮 RDMA を上回るか | 65.7 MB の E2E では、可逆のバイトグループ化＋LZ4 は 4 KiB ページでも THP でも無圧縮を下回ります（overlap で、4 KiB は 8.9 対 10.9 GB/s、THP は 9.3 対 24.6 GB/s） | `MEASUREMENT_GUIDE_RDMA.md`、`run_final_showdown.sh`、`run_lossless_e2e.sh`、`run_idiff_e2e.sh` には、圧縮が「勝てる」、ある実行が「主結果」といった開発中の見立てが残っています。そこでの圧縮の数値の多くは、非可逆の `diff_byte` で測ったものです |
| 低速・共有リンク | 実効帯域が約 9 GB/s を大きく下回る共有・混雑リンクでは、可逆圧縮の競争力が増し得る。交差点の定量化は今後の課題 | `run_sharedlink.sh`、`plot_paper_figs.py`、`MEASUREMENT_GUIDE_RDMA.md` は、帯域を制限した測定で可逆圧縮が無圧縮に勝つことを示したと書いています。この測定は、ペーシング（`--rate-limit-gbps`）で遅いリンクを模擬したものです |
| 標準出力のまとめの表記 | — | `benchmark_e2e_rdma` の標準出力のまとめは、`--preproc` に関係なく、圧縮ありの実行をすべて `lz4+diff_byte` と表示します。実際に使った前処理は CSV の `preproc` 列に記録されます |
| 図番号と節番号 | — | コメント、実行スクリプトの表示文、`plot_paper_figs.py`、`MEASUREMENT_GUIDE_RDMA.md` にある図番号と節番号（たとえば「Fig 2/7/8」「§3.6」）はこの研究の以前の版（草稿と SWoPP 論文）のもので、出版された論文の番号ではありません |
| 圧縮チャンクの転送（overlap 経路） | 各チャンクを RDMA Write with Immediate で転送（3.4 節、表 1） | `--zerocopy 1` では、各チャンクを、空でないタイルごとの通知なしの RDMA Write と、サイズのヘッダを運ぶ通知ありの RDMA Write with Immediate 1 回で送ります（`benchmark_e2e_rdma.c` の `post_send_chunk`） |

コメント、表示文、`MEASUREMENT_GUIDE_RDMA.md` は開発中に書いたもので、そのまま残しています。
論文と食い違う箇所は、論文の記述に従ってください。

## 11. 完全な再実行に足りないもの

公開された図を描き直すには：

- 図 1、6、7、8 の測定結果の CSV ファイル。
- それらの図の作図スクリプト。

BF3 の実機で測定し直すには：

- Llama-3.2-1B の入力テンソルと、その書き出し方法の説明。
- 図 6、7、8 の設定そのものを使った実行スクリプト（設定は 6.6 節に記載）。
- `benchmark_generalization` 用の Zstandard 1.5.7 と Snappy 1.1.9 のソース。
- ツールチェーンとシステムライブラリの正確なバージョン（3 節）。
- 図 1 については、commit `42c75b6` 時点の `Makefile`（2 節）と、著者が `bf3_deploy/deps/` をどう準備したか（4 節）。
- `osu_bw`（OSU Micro-Benchmarks）の実行スクリプトと結果。
- 図 6、7 で使ったウォームアップの回数（出力に記録されていません）。
