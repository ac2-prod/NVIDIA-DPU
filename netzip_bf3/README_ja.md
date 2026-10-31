# netzip_bf3：NVIDIA BlueField-3 の Arm コアでの通信圧縮

[English](README.md) | [日本語](README_ja.md)

このディレクトリには、次の論文の実験コードが入っています。

> Taiga Kobayashi, Tomohiro Ueno, and Ryohei Kobayashi. 2026.
> **Characterizing Communication Compression on NVIDIA BlueField-3 DPU for
> Distributed Training.** In Workshop Proceedings of the 55th International
> Conference on Parallel Processing (ICPP Workshops '26). ACM, 24–34.
> <https://doi.org/10.1145/3816891.3834898>
>
> SPIN-NVSC Workshop @ ICPP 2026 で発表。

## 概要

この論文は、NVIDIA BlueField-3（BF3）DPU の Arm コアが、分散学習の通信の圧縮処理をオフロードする先
（compression-offload point）としてどこまで使えるかを評価したものです。コードは、bfloat16 テンソルに NetZIP 型の前処理
（バイトグループ化と、それに関連する変換）をかけ、続けて LZ4 で圧縮する処理を BF3 の Arm コア上で
行います。その結果を、raw ibverbs（RC キューペア、RDMA Write with Immediate）による
BF3 間の通信経路として測定します。測定する経路には、RDMA バッファのページ構成
（既定の 4 KiB ページか Transparent Huge Pages）と、圧縮と RDMA 転送のチャンク単位の
重ね合わせ（overlap）が含まれます。

論文では、無圧縮の RDMA 経路は 4 KiB ページで約 10.9 GB/s、Transparent Huge Pages で
約 24.6 GB/s でした。可逆圧縮の経路（バイトグループ化＋LZ4）は、どちらの場合も無圧縮の経路を
下回りました。結果の全体とその解釈は論文を参照してください。

## このディレクトリに含まれるもの

ソースファイルとスクリプトのコメントの多くは日本語です。コメント、スクリプト、
`plot_paper_figs.py`、`MEASUREMENT_GUIDE_RDMA.md` に出てくる図番号は以前の草稿のもので、
公開された論文の図番号ではありません。

| パス | 役割 |
|---|---|
| `bf3_deploy/src/benchmark_e2e_rdma.c` | 2 ランク（MPI）の end-to-end ベンチマーク。無圧縮 RDMA、圧縮の逐次版、圧縮のチャンク単位 overlap 版を、4 KiB または THP のバッファで測定します。計測した反復ごとに、段ごとの時間を含む CSV 行を 1 行書きます。論文の図 6、7、8 は、著者の開発用リポジトリの commit `e125de3` 時点のこのプログラムで測定しました。このファイルはその版と同一です（[REPRODUCING_ja.md](REPRODUCING_ja.md#2-コードの来歴) を参照）。 |
| `bf3_deploy/src/rdma_transport.c`、`rdma_transport.h` | libibverbs を使い、RC QP 上の RDMA Write with Immediate でデータを送る最小限の転送層。MPI は接続の確立と同期にのみ使います。 |
| `bf3_deploy/src/netzip.c`、`netzip.h` | bfloat16 の前処理変換とその逆変換：バイトグループ化、ビットグループ化、整数差分＋バイトグループ化（可逆）、浮動小数点差分＋バイトグループ化（非可逆）。 |
| `bf3_deploy/src/benchmark_generalization.c` | テンソルファイル群に対するコーデックの選定測定：4 種の前処理 × LZ4、Snappy、Zstd。論文の選定（図 1）は、commit `42c75b6` 時点のこのプログラムで測定しました。このファイルはその版と同一です。 |
| `bf3_deploy/src/rdma_sanity.c` | ヒュージページの有無による RDMA Write 帯域の確認。 |
| `bf3_deploy/src/benchmark_omp_scaling.c`、`benchmark_idiff_compare.c`、`benchmark_diff_error.c`、`benchmark_pipeline.c`、`netzip_delta.c` | 論文の開発過程で使ったベンチマークと補助コード。公開された図には使っていません。 |
| `benchmark_decompress.c` | 論文の開発過程で使った伸長ベンチマーク。このリポジトリだけではビルドできません（[REPRODUCING_ja.md](REPRODUCING_ja.md#4-ビルド) を参照）。 |
| `bf3_deploy/src/lz4.c`、`lz4.h`、`lz4frame.c`、`lz4frame.h`、`lz4hc.c`、`lz4hc.h`、`xxhash.c`、`xxhash.h` | 同梱した第三者の LZ4 1.9.4 と xxHash 0.6.5（BSD 2-Clause License）。 |
| `bf3_deploy/src/Makefile` | ビルド規則。 |
| `bf3_deploy/build_rdma.sh`、`build_generalization.sh`、`build_idiff.sh` | 論文で使ったクラスタ向けに書いた SLURM のビルドスクリプト。 |
| `bf3_deploy/run_*.sh`（15 本） | 開発中に使った SLURM の実行スクリプト。それぞれが何を実行するかは [REPRODUCING_ja.md](REPRODUCING_ja.md#65-このリポジトリの実行スクリプト) にあります。 |
| `bf3_deploy/src/plot_paper_figs.py` | 以前の草稿の作図スクリプト。公開された論文の図は描きません。 |
| `bf3_deploy/MEASUREMENT_GUIDE_RDMA.md` | 開発中の測定記録（日本語）。中の数値、図番号、結論は公開された論文より前のもので、論文の一部ではありません。「Issue #N」は著者の内部の管理番号です。 |

## このディレクトリに含まれないもの

- 論文の図のもとになった測定データ（CSV ファイル）。
- 公開された図を描いた作図スクリプト。
- 図 6、7、8 のデータを取った実行スクリプト。
- 入力テンソル（Llama-3.2-1B から得た bfloat16 テンソル）。
- 選定測定のベンチマークに必要な Zstandard と Snappy のソース。

その影響と、完全な再実行に必要なものは [REPRODUCING_ja.md](REPRODUCING_ja.md) で説明しています。

## 必要な環境

性能の測定には BlueField-3 の実機が必要です。一般的な PC では再現できません。

- InfiniBand で接続した 2 台の NVIDIA BlueField-3 DPU。プログラムは BF3 の Arm コア上
  （aarch64 の Linux）で動かします。論文では 200 Gb/s の InfiniBand と Rocky Linux 9.4 を
  使いました。
- OpenMP を使える GCC。論文では GCC 11.4.1 を使いました。スクリプトはモジュール `gcc/11` を
  読み込みます。
- `mpicc` と `mpirun` を提供する MPI 実装。論文には HPC-X 2.24.1-mt と記載しています。
  スクリプトはモジュール `hpcx/2.24.1` を読み込みます。
- RDMA のプログラムには libibverbs（`infiniband/verbs.h`）。
- `benchmark_generalization` には、zlib、C++ コンパイラ、`bf3_deploy/deps/` に置いた
  Zstandard 1.5.7 と Snappy 1.1.9 のソース。

## ビルドと実行

RDMA のプログラムは BF3（aarch64）のノード上でビルドします。

```bash
cd netzip_bf3/bf3_deploy/src
make rdma_sanity benchmark_e2e_rdma
```

`benchmark_e2e_rdma` は、BF3 ノード 1 台につき 1 つ、ちょうど 2 つの MPI ランクで実行する
必要があります。オプション、論文で使った設定、出力の形式、完全な再実行に足りないものは
[REPRODUCING_ja.md](REPRODUCING_ja.md) で説明しています。

## ライセンス

- コードと文書：MIT License。[LICENSE](LICENSE) を参照してください。
- [LICENSE-DATA.md](LICENSE-DATA.md) に記載したデータファイル（本研究で作成した実験データ・
  測定結果）：CC BY 4.0。現時点では記載されたデータファイルはありません。
- 同梱した LZ4 と xxHash のファイルは、元の BSD 2-Clause License のままです。

リポジトリ全体の [LICENSING.md](../LICENSING.md) も参照してください。

## 引用

このコードを使う場合は、論文を引用してください。BibTeX は [CITATION.bib](CITATION.bib) に
あります。
