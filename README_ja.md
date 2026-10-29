# NVIDIA-DPU

[English](README.md) | [日本語](README_ja.md)

A repository for evaluating the effectiveness of NVIDIA DPU.
（NVIDIA DPU の有効性を評価するためのリポジトリです。）

NVIDIA の DPU（Data Processing Unit）に関する研究のコードと実験資料をまとめたリポジトリです。
研究成果ごとにサブディレクトリを分けています。各サブディレクトリに、README、再現手順、
ライセンスファイル、引用情報があります。

## 収録内容

| ディレクトリ | 内容 | 発表 |
|---|---|---|
| [`netzip_bf3/`](netzip_bf3/README_ja.md) | NVIDIA BlueField-3 の Arm コア上で可逆通信圧縮（NetZIP 型の前処理と LZ4）を行い、BF3 間の RDMA 通信経路として評価した実験コード | Taiga Kobayashi, Tomohiro Ueno, and Ryohei Kobayashi. "Characterizing Communication Compression on NVIDIA BlueField-3 DPU for Distributed Training." ICPP Workshops '26（SPIN-NVSC Workshop @ ICPP 2026）. [doi:10.1145/3816891.3834898](https://doi.org/10.1145/3816891.3834898) |

ビルド方法、再現手順、引用情報は各サブディレクトリにあります。
関心のあるディレクトリの README から読んでください。

## ライセンス

リポジトリ全体に一律に適用するライセンスはありません。ライセンスはサブディレクトリごとに
定めています。[LICENSING.md](LICENSING.md) を参照してください。
