# netzip_bf3: Communication compression on NVIDIA BlueField-3 Arm cores

[English](README.md) | [日本語](README_ja.md)

This directory contains the experiment code for the following paper.

> Taiga Kobayashi, Tomohiro Ueno, and Ryohei Kobayashi. 2026.
> **Characterizing Communication Compression on NVIDIA BlueField-3 DPU for
> Distributed Training.** In Workshop Proceedings of the 55th International
> Conference on Parallel Processing (ICPP Workshops '26). ACM, 24–34.
> <https://doi.org/10.1145/3816891.3834898>
>
> Presented at the SPIN-NVSC Workshop @ ICPP 2026.

## Overview

The paper evaluates how far the Arm cores of an NVIDIA BlueField-3 (BF3) DPU
can serve as a compression-offload point for distributed-training traffic.
The code implements NetZIP-style preprocessing of bfloat16 tensors (byte
grouping and related transforms) followed by LZ4 compression on the BF3 Arm
cores. It measures the result as a BF3-to-BF3 communication path over raw
ibverbs (RC queue pair, RDMA Write with Immediate). The measured path
includes the RDMA-buffer page backing (default 4 KiB pages or Transparent Huge
Pages) and chunk-level overlap between compression and RDMA transfer.

In the paper, the uncompressed RDMA path reaches about 10.9 GB/s with 4 KiB
pages and about 24.6 GB/s with Transparent Huge Pages. The lossless compressed
path (byte grouping + LZ4) stays below the uncompressed path in both cases.
See the paper for the full results and their interpretation.

## What this directory contains

Most comments in the source files and scripts are written in Japanese.
Figure numbers that appear in comments, scripts, `plot_paper_figs.py` and
`MEASUREMENT_GUIDE_RDMA.md` refer to earlier drafts, not to the published
paper.

| Path | Role |
|---|---|
| `bf3_deploy/src/benchmark_e2e_rdma.c` | Two-rank (MPI) end-to-end benchmark. It covers uncompressed RDMA, compressed serial, and compressed chunk-level overlap, with 4 KiB or THP-backed buffers. It writes one CSV row per timed iteration with per-stage times. The paper's Figs. 6, 7 and 8 were measured with this program at commit `e125de3` of the authors' development repository; this file is identical to that version (see [REPRODUCING.md](REPRODUCING.md#2-provenance-of-the-code)). |
| `bf3_deploy/src/rdma_transport.c`, `rdma_transport.h` | Minimal RC-QP RDMA Write-with-Immediate transport over libibverbs. MPI is used only for connection setup and synchronization. |
| `bf3_deploy/src/netzip.c`, `netzip.h` | bfloat16 preprocessing transforms and their inverses: byte grouping, bit grouping, integer delta with byte grouping (lossless), and float difference with byte grouping (lossy). |
| `bf3_deploy/src/benchmark_generalization.c` | Codec screening over a set of tensor files: four preprocessing modes × LZ4, Snappy and Zstd. The paper's screening (Fig. 1) was measured with this program at commit `42c75b6`; this file is identical to that version. |
| `bf3_deploy/src/rdma_sanity.c` | RDMA Write bandwidth check with and without huge pages. |
| `bf3_deploy/src/benchmark_omp_scaling.c`, `benchmark_idiff_compare.c`, `benchmark_diff_error.c`, `benchmark_pipeline.c`, `netzip_delta.c` | Benchmarks and helper code from the development of the paper. They are not used for the published figures. |
| `benchmark_decompress.c` | Decompression benchmark from the development of the paper. It cannot be built from this repository (see [REPRODUCING.md](REPRODUCING.md#4-build)). |
| `bf3_deploy/src/lz4.c`, `lz4.h`, `lz4frame.c`, `lz4frame.h`, `lz4hc.c`, `lz4hc.h`, `xxhash.c`, `xxhash.h` | Bundled third-party LZ4 1.9.4 and xxHash 0.6.5 (BSD 2-Clause License). |
| `bf3_deploy/src/Makefile` | Build rules. |
| `bf3_deploy/build_rdma.sh`, `build_generalization.sh`, `build_idiff.sh` | SLURM build scripts written for the cluster used in the paper. |
| `bf3_deploy/run_*.sh` (15 scripts) | SLURM run scripts used during development. [REPRODUCING.md](REPRODUCING.md#65-run-scripts-in-this-repository) lists what each one runs. |
| `bf3_deploy/src/plot_paper_figs.py` | Plot script for an earlier draft. It does not draw the figures of the published paper. |
| `bf3_deploy/MEASUREMENT_GUIDE_RDMA.md` | Development measurement log (Japanese). Its numbers, figure numbers and conclusions predate the published paper and are not part of it. "Issue #N" refers to the authors' internal tracker. |

## What this directory does not contain

- The measurement data (CSV files) behind the paper's figures.
- The plotting scripts that drew the published figures.
- The run scripts that produced the data for Figs. 6, 7 and 8.
- The input tensors (bfloat16 tensors derived from Llama-3.2-1B).
- The Zstandard and Snappy sources that the screening benchmark needs.

[REPRODUCING.md](REPRODUCING.md) explains the consequences and what is needed
for a full re-run.

## Requirements

Performance measurements need BlueField-3 hardware. They cannot be reproduced
on an ordinary PC.

- Two NVIDIA BlueField-3 DPUs connected by InfiniBand, with programs running
  on the BF3 Arm cores (aarch64 Linux). The paper used 200 Gb/s InfiniBand and
  Rocky Linux 9.4.
- GCC with OpenMP. The paper used GCC 11.4.1; the scripts load the module
  `gcc/11`.
- An MPI implementation that provides `mpicc` and `mpirun`. The paper lists
  HPC-X 2.24.1-mt; the scripts load the module `hpcx/2.24.1`.
- libibverbs (`infiniband/verbs.h`) for the RDMA programs.
- For `benchmark_generalization`: zlib, a C++ compiler, and the Zstandard
  1.5.7 and Snappy 1.1.9 sources placed under `bf3_deploy/deps/`.

## Build and run

Build the RDMA programs on a BF3 (aarch64) node:

```bash
cd netzip_bf3/bf3_deploy/src
make rdma_sanity benchmark_e2e_rdma
```

`benchmark_e2e_rdma` must run as exactly two MPI ranks, one per BF3 node.
[REPRODUCING.md](REPRODUCING.md) describes the options, the configurations
used in the paper, the output format, and what is missing for a full re-run.

## License

- Code and documentation: MIT License. See [LICENSE](LICENSE).
- Data files listed in [LICENSE-DATA.md](LICENSE-DATA.md) (experimental data
  and measurement results created in this study): CC BY 4.0. No data files
  are listed at present.
- Bundled LZ4 and xxHash files keep their own BSD 2-Clause License.

See also the repository-level [LICENSING.md](../LICENSING.md).

## Citation

If you use this code, please cite the paper. BibTeX is in
[CITATION.bib](CITATION.bib).
