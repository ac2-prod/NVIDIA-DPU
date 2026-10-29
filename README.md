# NVIDIA-DPU

[English](README.md) | [日本語](README_ja.md)

A repository for evaluating the effectiveness of NVIDIA DPU.

This repository collects code and experiment material from research on NVIDIA
DPUs (Data Processing Units). Each research result lives in its own
subdirectory. Each subdirectory has its own README, reproduction notes,
license files and citation information.

## Contents

| Directory | What it contains | Publication |
|---|---|---|
| [`netzip_bf3/`](netzip_bf3/README.md) | Experiment code for lossless communication compression (NetZIP-style preprocessing and LZ4) on the Arm cores of NVIDIA BlueField-3, evaluated as a BF3-to-BF3 RDMA communication path | Taiga Kobayashi, Tomohiro Ueno, and Ryohei Kobayashi. "Characterizing Communication Compression on NVIDIA BlueField-3 DPU for Distributed Training." ICPP Workshops '26 (SPIN-NVSC Workshop @ ICPP 2026). [doi:10.1145/3816891.3834898](https://doi.org/10.1145/3816891.3834898) |

Build instructions, reproduction steps and citation information are kept in
each subdirectory. Start from the README of the directory you are interested
in.

## License

This repository has no repository-wide license. Licenses are set per
subdirectory. See [LICENSING.md](LICENSING.md).
