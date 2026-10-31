# Reproducing the experiments

[English](REPRODUCING.md) | [日本語](REPRODUCING_ja.md)

This document describes how the experiments of the paper relate to the files
in this directory, how to build and run the programs, and what is missing for
a complete re-run. Paths are relative to `netzip_bf3/` unless stated
otherwise.

The commands and options below were checked against the source code and the
scripts in this directory. They were not executed while this document was
written, because no BlueField-3 hardware was available for that purpose.

## 1. What can be reproduced from this repository

| Goal | Possible from this repository alone? |
|---|---|
| Re-plot the paper's figures from the existing measurements | **No.** The measurement CSV files and the plotting scripts used for the published figures are not included (Section 8). |
| Re-measure on BlueField-3 hardware | **Partly.** The benchmark programs for the RDMA and end-to-end measurements (Figs. 6–8) can be built from this repository. The input tensors, the exact run scripts for Figs. 6–8, and the Zstandard/Snappy sources for the screening benchmark (Fig. 1) are not included (Section 11). |
| Reproduce performance on an ordinary PC | **No.** The measurements require two BF3 DPUs with InfiniBand/RDMA and the BF3 Arm cores. The preprocessing code uses Arm NEON intrinsics on aarch64, and the RDMA programs need libibverbs and MPI. |

## 2. Provenance of the code

The files in this directory were committed to this repository on 2026-06-22
(commit `0128b40`), as a copy of the authors' development tree.

The measurement data behind the paper's figures record the commit of the
development repository that produced them, in the `commit_hash` column:
`e125de3` for Figs. 6, 7 and 8, and `42c75b6` for Fig. 1. These are not
commits of this repository. The data for Figs. 6, 7 and 8 were recorded on
2026-06-24 and 2026-07-06, after this copy was committed; the Fig. 1 data were
recorded on 2026-06-18.

The files in this directory were compared with the development repository on
2026-10-29:

- Figs. 6, 7 and 8: `benchmark_e2e_rdma.c`, `rdma_transport.c`,
  `rdma_transport.h`, `netzip.c`, `netzip.h`, `lz4.c`, `lz4.h` and the
  `Makefile` in `bf3_deploy/src/` are identical to the versions at commit
  `e125de3`.
- Fig. 1: `benchmark_generalization.c`, `netzip.c` and `netzip.h` are
  identical to the versions at commit `42c75b6`. The `Makefile` here has
  lines that were added after that commit (13 lines added, 1 line removed).

When the programs in this directory are run inside a Git checkout, they record
the current `HEAD` of that checkout (`git rev-parse --short HEAD`) in the same
column (`bf3_deploy/src/benchmark_e2e_rdma.c`, `benchmark_generalization.c`).

## 3. Hardware and software

| Item | Paper (Table 1) | Recorded in this repository |
|---|---|---|
| Nodes | 2 × NVIDIA BlueField-3 DPU | Two-node SLURM scripts (`run_*.sh`) |
| Arm cores | 16 × Cortex-A78 per DPU | Core type only (Cortex-A78, comment in `bf3_deploy/src/Makefile`); the core count is not recorded |
| Memory | 32 GB DDR5 (dual DDR5-5600 controllers) | Not recorded |
| Network | 200 Gb/s InfiniBand | Not recorded |
| OS | Rocky Linux 9.4, aarch64 | Module path `rocky-9.aarch64` in the scripts |
| Compiler | GCC 11.4.1 | Module `gcc/11` in the scripts |
| MPI | HPC-X 2.24.1-mt | Module `hpcx/2.24.1` in the scripts (see Section 10) |
| RDMA | raw ibverbs, RC QP, RDMA Write with Immediate | `bf3_deploy/src/rdma_transport.c` |
| LZ4 | 1.9.4, bundled | `bf3_deploy/src/lz4.h` (version 1.9.4) |
| xxHash | — | `bf3_deploy/src/xxhash.h` (version 0.6.5) |
| Zstandard, Snappy | — | Expected as `bf3_deploy/deps/zstd-1.5.7` and `bf3_deploy/deps/snappy-1.1.9` (not included) |

The versions of libibverbs/rdma-core, zlib, the OpenMP runtime and the
DPU firmware are not recorded in this repository.

## 4. Build

Build on a BF3 Arm (aarch64) node. With the module environment used for the
paper:

```bash
module load gcc/11
module load hpcx/2.24.1
cd netzip_bf3/bf3_deploy/src
make rdma_sanity benchmark_e2e_rdma
```

On the cluster used for the paper, the same build is done by
`bf3_deploy/build_rdma.sh` (submit it from `bf3_deploy/` with `sbatch`). The
build scripts contain cluster-specific settings (SLURM partition and node
names, `MODULEPATH`); adapt them to your system.

Compiler flags are defined in `bf3_deploy/src/Makefile`. The MPI/RDMA programs
use `mpicc -O3 -funroll-loops -fopenmp -march=armv8.2-a+fp16 -pthread -I.`.
The programs that link Zstandard and Snappy use
`-Wall -O3 -funroll-loops -fopenmp -DPROFILE` with include paths under
`../deps`.

| Make target | Sources | Builds from this repository? |
|---|---|---|
| `rdma_sanity` | `rdma_sanity.c`, `rdma_transport.c` | Yes (needs MPI and libibverbs) |
| `benchmark_e2e_rdma` | `benchmark_e2e_rdma.c`, `rdma_transport.c`, `netzip.c`, `lz4.c` | Yes (needs MPI and libibverbs) |
| `benchmark_e2e_rdma_noib` | `benchmark_e2e_rdma.c`, `netzip.c`, `lz4.c` (no `rdma_transport.c`), built with `-DNO_IBVERBS` | Yes (needs MPI; accepts only `--transport mpi`) |
| `rdma_targets` | builds `rdma_sanity` and `benchmark_e2e_rdma` | Yes (needs MPI and libibverbs) |
| `benchmark_idiff_compare` | `benchmark_idiff_compare.c`, `netzip.c`, `lz4.c` | Yes |
| `benchmark_diff_error` | `benchmark_diff_error.c`, `netzip.c` | Yes |
| `benchmark_generalization`, `benchmark_omp_scaling`, `benchmark_pipeline` | their sources, `netzip.c`, LZ4, Snappy and Zstandard objects | No: needs `bf3_deploy/deps/zstd-1.5.7` and `bf3_deploy/deps/snappy-1.1.9`, zlib and a C++ compiler |
| `benchmark`, `dump_compressed`, `benchmark_parallel`, `benchmark_l3`, `benchmark_parallel_fast`, `fast`, `benchmark_gprof`, `benchmark_gprof_internal`, `benchmark_e2e` | sources not in this repository (`benchmark.c`, `dump_compressed.c`, `benchmark_parallel.c`, `benchmark_l3.c`, `benchmark_e2e.c`) | No |
| `all` (default) | `benchmark`, `dump_compressed`, `benchmark_parallel`, `benchmark_pipeline`, `benchmark_l3` | No: most of its sources are missing, and `benchmark_pipeline` needs `bf3_deploy/deps/` |

`benchmark_decompress.c` (at the top of `netzip_bf3/`) has no rule in this
Makefile. `bf3_deploy/run_decomp_ladder.sh` runs `make benchmark_decompress`
in the parent directory `netzip_bf3/`, which has no Makefile in this
repository. The program also calls functions that are not defined in
`bf3_deploy/src/netzip.c`.

Running a bare `make` builds the default target `all`, which fails because most
of its sources are missing. Always name the targets.

## 5. Input data

The input tensors are **not included** in this repository.

- `benchmark_e2e_rdma --data PATH` reads the first `--msg-size` bytes of the
  file as raw bfloat16 values. Both MPI ranks read the file, so it must exist
  at the same path on both nodes. Without `--data`, the program generates
  synthetic data; its compression behaviour differs from the paper's tensors.
- The paper's end-to-end measurements use one 65,667,072-byte `lm_head`
  activation of Llama-3.2-1B. The run scripts refer to it as
  `bf3_data/forward/0212_lm_head.pkl.bin`.
- The screening (Fig. 1) uses all 423 activation and gradient tensors of
  Llama-3.2-1B. `run_generalization.sh` expects them as
  `../../bf3_data/forward/*.pkl.bin` and `../../bf3_data/grad/*.pkl.bin`
  relative to `bf3_deploy/src/`. `benchmark_generalization` decides whether a
  file is an activation or a gradient from `/forward/` or `/grad/` in its
  path.
- How these `.pkl.bin` files were produced from the model is not documented
  in this repository.

## 6. Running the experiments

### 6.1 Launcher

`benchmark_e2e_rdma` and `rdma_sanity` must run as exactly two MPI ranks, one
per BF3 node. The run scripts in this repository use:

```bash
mpirun -np 2 --map-by ppr:1:node --bind-to none \
  -x OMP_NUM_THREADS=16 -x OMP_PROC_BIND=close -x OMP_PLACES=cores \
  -x NETZIP_IB_DEV=mlx5_0 \
  ./benchmark_e2e_rdma [options]
```

Run it in `bf3_deploy/src/`. The scripts change to `$SLURM_SUBMIT_DIR/src`,
so they are submitted from `bf3_deploy/`.

Environment variables read by `bf3_deploy/src/rdma_transport.c`:

| Variable | Meaning |
|---|---|
| `NETZIP_IB_DEV` | HCA to use, for example `mlx5_0`. Default: the first device with an ACTIVE port. |
| `NETZIP_GID_INDEX` | GID index for RoCE (Ethernet link layer). Default 3. Not used with InfiniBand. |

### 6.2 Options of `benchmark_e2e_rdma`

Defaults are those in `bf3_deploy/src/benchmark_e2e_rdma.c`. Any other
argument stops the program with `unknown arg`.

| Option | Default | Meaning |
|---|---|---|
| `--data PATH` | none (synthetic data) | Input file, raw bfloat16 |
| `--out CSV` | `results_e2e_breakdown.csv` | Output CSV (appended) |
| `--msg-size N` | 65667072 | Bytes per transfer |
| `--chunk-size N` | message size (one chunk) | Chunk size in bytes |
| `--sub-kb K` | 256 | Tile (sub-chunk) size in KiB; each tile is one LZ4 block |
| `--compress 0\|1` | 1 | 0 = uncompressed RDMA, 1 = compressed path |
| `--preproc NAME` | `diff_byte` | `byte_group`, `bit_group`, `idiff_byte` (lossless) or `diff_byte` (lossy) |
| `--overlap 0\|1` | 0 | 1 = overlap compression of the next chunk with the transfer of the current chunk |
| `--zerocopy 0\|1` | 0 | 1 = LZ4 writes directly into the registered wire buffer (no pack copy); ibverbs only |
| `--thp 0\|1` | 0 | 1 = allocate the main buffers 2 MiB-aligned and request THP with `madvise(MADV_HUGEPAGE)` |
| `--iters N` | 10 | Timed iterations (maximum 64) |
| `--warmup N` | 3 | Warm-up iterations (not recorded) |
| `--flush-iter 0\|1` | 1 | Evict the last-level cache on both nodes before each iteration (outside the timed window) |
| `--lockstep 0\|1` | 0 | Wait for the receiver after each chunk |
| `--phase-shift 0\|1` | 0 | Split preprocessing and LZ4 into separate phases; forces overlap |
| `--pre-threads N` | 0 (all threads) | Threads for preprocessing. A value from 1 to `OMP_NUM_THREADS` − 1 runs preprocessing and LZ4 as separate (non-fused) phases; LZ4 always uses all threads |
| `--cold none\|send\|recv\|both` | `none` | Evict caches after compression or reception (timed separately and subtracted) |
| `--transport ibverbs\|mpi` | `ibverbs` | Data path; `mpi` uses `MPI_Isend`/`MPI_Irecv` |
| `--rate-limit-gbps G` | 0 (off) | Limit the effective link bandwidth to G GB/s by inserting delays after chunk completions |
| `--sharedlink-out CSV` | none | Also append one summary row per run (rate-limited or not) to this CSV |
| `--shift auto\|X` | `auto` | Shift value subtracted by `diff_byte`; `auto` computes it from the data |

All sizes must be even (bfloat16 elements). With `--preproc bit_group`,
`--msg-size`, `--chunk-size` and the tile size must also be multiples of 16.

### 6.3 Execution modes used in the paper

| Mode in the paper | Options |
|---|---|
| Uncompressed RDMA | `--compress 0`. The sender posts one RDMA Write with Immediate per chunk directly from its registered source buffer into the receiver's registered destination buffer (no preprocessing, LZ4, pack or wire buffer). With the default chunk size this is a single transfer of the whole message. |
| Compressed, serial | `--compress 1 --preproc byte_group` with the default chunk size (one chunk), `--overlap 0`, `--zerocopy 0`. The sender compresses all tiles, copies them into the wire buffer (pack), and sends. |
| Compressed, chunk-level overlap | `--compress 1 --preproc byte_group --overlap 1 --chunk-size 4194304 --zerocopy 1`. Compression of chunk c+1 overlaps the transfer of chunk c, and there is no pack copy. |
| 4 KiB pages / THP | `--thp 0` / `--thp 1` |

Implementation notes, from the source code:

- With `--thp 1`, the program reads `AnonHugePages` from `/proc/self/smaps`
  and records `thp_madvise` (huge pages present) or `thp_no_2m` in the
  `page_mode` column. With `--thp 0` it records `base_4k` without reading
  `smaps`. If the two ranks differ, it records `mixed`.
- With `--zerocopy 1`, each chunk is sent as one unsignaled RDMA Write per
  non-empty tile followed by a signaled RDMA Write with Immediate that carries
  the size header.
- Memory registration (`ibv_reg_mr`) is outside the timed window; its time is
  recorded once in `mr_reg_ms`.

### 6.4 Measurement protocol in the code

Each run executes the warm-up iterations and then the timed iterations. In each
iteration, the receiver pre-posts its receive requests, both ranks meet at an
`MPI_Barrier`, both ranks evict the last-level cache (when `--flush-iter 1`),
both ranks meet at a second barrier, the clock starts, the sender and receiver
run one transfer, both ranks meet at a third barrier, and the clock stops.
After the last timed iteration the receiver compares the restored tensor with
the original. For the lossless preprocessing modes and `--compress 0` it
requires an exact byte-for-byte match and records `EXACT` (or `FAIL`) in the
`verify` column.

### 6.5 Run scripts in this repository

All scripts are SLURM job scripts for the cluster used in the paper. They set a
partition, node names and `MODULEPATH`. The scripts that read tensors use
default data paths either under the authors' home directory or relative to
`bf3_deploy/src/` (`../../bf3_data`). Adapt these before use. Figure numbers in their comments
refer to earlier drafts.

| Script | What it runs | Output (in `bf3_deploy/src/`) |
|---|---|---|
| `run_rdma_sanity.sh` | `rdma_sanity` and `rdma_sanity --hugepages` on two nodes | `results_rdma_sanity.csv` |
| `run_e2e_breakdown.sh` | `benchmark_e2e_rdma` with `--compress 0`, `--compress 1`, 4 MiB chunks, and 4 MiB chunks with overlap (default `diff_byte`) | `results_e2e_breakdown.csv` |
| `run_msgsize_sweep.sh` | `benchmark_e2e_rdma` over a list of message sizes, `--compress 0` and `1` (default `diff_byte`), with one chunk and with 4 MiB chunks and overlap | `results_msgsize_sweep.csv` |
| `run_cachehot.sh` | cache hot/cold comparison with `--lockstep 1` and `--cold` | `results_cachehot.csv` |
| `run_final_showdown.sh` | THP: uncompressed baseline, and overlap runs with 4/8 MiB chunks, zero-copy, phase-shift and `--pre-threads 12` variants (default `diff_byte`) | `results_final_showdown.csv` |
| `run_fig8_fill.sh` | one THP serial compressed run (default `diff_byte`), one chunk | `results_final_showdown.csv` |
| `run_idiff_e2e.sh` | overlap + zero-copy with THP, `diff_byte` and `idiff_byte` | `results_final_showdown.csv` |
| `run_lossless_e2e.sh` | overlap + zero-copy with THP, `byte_group` and `bit_group` | `results_final_showdown.csv` |
| `run_sharedlink.sh` | rate-limited runs (`--rate-limit-gbps`) | `results_e2e_breakdown.csv`, `results_sharedlink.csv` |
| `run_generalization.sh` | `benchmark_generalization` over all tensors | `results_generalization.csv` |
| `run_omp_scaling.sh` | `benchmark_omp_scaling` | `results_omp_scaling.csv` |
| `run_idiff_compare.sh` | `benchmark_idiff_compare` on six tensors | `results_idiff_compare.csv` |
| `run_lossless_compare.sh` | `benchmark_idiff_compare` on six tensors | `results_lossless_compare.csv` |
| `run_diff_error.sh` | `benchmark_diff_error` over all tensors | `results_diff_error.csv` |
| `run_decomp_ladder.sh` | `benchmark_decompress` (cannot be built from this repository) | `results_decomp_step*.csv` |

Of these, only `run_generalization.sh` runs the program and settings recorded
for a published figure (Fig. 1). Running it still requires the input tensors
and the Zstandard/Snappy sources, and its output has not been compared with the
published data. The E2E scripts pass `--iters 10 --warmup 3`; none of them
runs the exact configurations of Figs. 6–8.

### 6.6 Configurations behind the published figures

The run scripts for Figs. 6–8 are not included. The settings below are those
recorded in the configuration columns of the authors' measurement data (not
included) and stated in the paper.

| Figure | Program | Settings |
|---|---|---|
| Fig. 1 (screening) | `benchmark_generalization` | 423 tensor files, 16 OpenMP threads, 256 KiB tiles (a tensor smaller than 256 KiB is processed as one tile), four preprocessing modes × LZ4/Snappy/Zstd (Zstd level 3, fixed in the source). `run_generalization.sh` passes `--iters 7 --warmup 3`. The figure shows the three lossless modes. |
| Fig. 6 (RDMA bandwidth vs message size) | `benchmark_e2e_rdma` | `--compress 0`, one chunk per message, `--thp 0` and `--thp 1`, message sizes from 1 KiB to 65,667,072 bytes, five runs per point, 15 timed iterations per run up to 256 KiB and 8 from 512 KiB (inferred from the row counts; the warm-up count is not recorded). The plotted value is the mean over the five runs of `gbps_xfer` in the `median` rows. |
| Fig. 7 (Arm-side stage times vs threads) | `benchmark_e2e_rdma` | `--compress 1 --preproc byte_group --thp 1`, one chunk, `--overlap 0 --zerocopy 0`, `OMP_NUM_THREADS` = 1, 2, 4, 8 and 16; 20 timed iterations per thread count. |
| Fig. 8 (end-to-end throughput) | `benchmark_e2e_rdma` | 65,667,072 bytes, 16 threads, 256 KiB tiles, `--thp 0` and `--thp 1`, each with three modes (Section 6.3), 50 timed iterations each. |

The paper states "median over 50 timed iterations after five warm-up
iterations" for the end-to-end results. The warm-up count is not recorded in
the output.

Example invocations for Fig. 8 with 4 KiB pages, assembled from the options
above (use `--thp 1` for THP). `DATA` is a raw bfloat16 file of at least
65,667,072 bytes at the same path on both nodes.

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

## 7. Output format

`benchmark_e2e_rdma` writes its CSV on rank 1, in the current directory,
appending to an existing file. If an existing file has a different header, it
is renamed to `<name>.old.<unixtime>` and a new file is started. Each run
appends one `raw` row per timed iteration (`iter` = 0, 1, …) and one `median`,
`min` and `max` row (`iter` = -1).

Columns (header line of the CSV):

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

`throughput_gbps` is the message size divided by `t_total_ms`, in decimal
GB/s, and `comp_ratio` is the message size divided by `wire_bytes`.

`benchmark_generalization` appends to `results_generalization.csv` by default,
with the following columns:

```text
tensor,category,layer_idx,act_or_grad,size_bytes,preproc,codec,
comp_ratio,bit_exact,mismatch_bytes,
gbps_preproc,gbps_compress,gbps_decompress,gbps_restore,
omp_threads,sub_bytes,nsub,commit_hash,timestamp,status
```

## 8. Plotting

The plotting scripts that drew the published figures are not included. From
the authors' plotting scripts (not included), the plotted quantities are:

- Fig. 1: for each tensor, the compression factor (original size divided by
  compressed size) against the throughput of the slower of the sender path
  (preprocessing then compression) and the receiver path (decompression then
  restore). Each path's throughput is combined from the per-stage throughputs
  a and b as 1/(1/a + 1/b). The median of each preprocessing × codec
  combination is marked.
- Fig. 6: see Section 6.6.
- Fig. 7: the per-stage times (`t_preproc_ms`, `t_compress_ms`, `t_pack_ms`,
  `t_decompress_ms`, `t_restore_ms`) of the `median` rows.
- Fig. 8: `throughput_gbps` of the `median` rows.

`bf3_deploy/src/plot_paper_figs.py` is the plot script of an earlier draft. It
reads CSV files from `bf3_deploy/src/` (for example `results_rdma_sanity.csv`,
`results_msgsize_sweep.csv`, `results_final_showdown.csv`,
`results_e2e_breakdown.csv`, `results_generalization.csv`,
`results_omp_scaling.csv`) and writes PNG and PDF files to
`bf3_deploy/src/figs/`. It needs Python with matplotlib. Its figures are based
mainly on the lossy `diff_byte` configuration and do not correspond to the
figures of the published paper. Without its input CSV files it stops with an
error.

## 9. Paper figures and tables

| Paper item | Program in this repository | Run script in this repository | Data and plot script |
|---|---|---|---|
| Fig. 1, screening | `bf3_deploy/src/benchmark_generalization.c` | `bf3_deploy/run_generalization.sh` | Not included |
| Figs. 2–5, schematics | — | — | Not included |
| Table 1, setup | — | — | Section 3 |
| Fig. 6, RDMA bandwidth vs message size | `bf3_deploy/src/benchmark_e2e_rdma.c` | Not included | Not included |
| Fig. 7, Arm-side stage times vs threads | `bf3_deploy/src/benchmark_e2e_rdma.c` | Not included | Not included |
| Fig. 8, end-to-end throughput | `bf3_deploy/src/benchmark_e2e_rdma.c` | Not included (closest: `run_lossless_e2e.sh` for the THP overlap bar, but with `--iters 10 --warmup 3`) | Not included |

The paper's `osu_bw` numbers (OSU Micro-Benchmarks) are not covered by any
file in this repository.

## 10. Manual steps and external dependencies

- Copy the input tensor to the same path on both nodes and pass it with
  `--data`.
- Place the Zstandard 1.5.7 and Snappy 1.1.9 sources under
  `bf3_deploy/deps/` before building `benchmark_generalization`.
- Edit the SLURM headers (partition, node names), `MODULEPATH`, module names
  and default data paths in `build_*.sh` and `run_*.sh` for your cluster.
- Set `NETZIP_IB_DEV` if the default HCA selection is not the one you want.
- For 4 KiB-page runs, the program does not check `smaps`. Check
  `AnonHugePages` in `/proc/<pid>/smaps` yourself if you need that
  confirmation.
- `make clean` also deletes object files under `../deps`.

### Differences between the paper and this repository

| Topic | Paper | This repository |
|---|---|---|
| Iterations | Median over 50 timed iterations after five warm-up iterations (E2E results) | Program defaults `--iters 10 --warmup 3`; all E2E run scripts pass these values |
| MPI | HPC-X 2.24.1-mt | Scripts load `hpcx/2.24.1`; a Makefile comment says the plain (non `-mt`) build is sufficient |
| 4 KiB-mode check | AnonHugePages checked in `/proc/<pid>/smaps` | Not checked by the program with `--thp 0` |
| Cache eviction | Not mentioned | Last-level cache evicted before every iteration by default (`--flush-iter 1`), outside the timed window |
| Chunking | The 65.7 MB buffer is divided into 4 MiB RDMA chunks (16 chunks in the main configuration) | In the recorded data, only the overlapped runs use 4 MiB chunks; the uncompressed and serial runs of Fig. 8 use one chunk of 65,667,072 bytes |
| Preprocessing default | Byte grouping + LZ4 is the evaluated method | Default `--preproc` is the lossy `diff_byte`; `--preproc byte_group` must be given |
| Default message size (comment) | — | The header comment of `benchmark_e2e_rdma.c` says the default is the file size; the code always uses 65667072 |
| `make all` | — | `MEASUREMENT_GUIDE_RDMA.md` suggests that `make all` builds `benchmark_omp_scaling`; the `all` target does not include it, and the sources of `all` are missing |

## 11. What is missing for a complete re-run

To re-plot the published figures:

- The measurement CSV files for Figs. 1, 6, 7 and 8.
- The plotting scripts for those figures.

To re-measure on BF3 hardware:

- The Llama-3.2-1B input tensors, and a description of how they were exported.
- The run scripts with the exact settings of Figs. 6, 7 and 8 (Section 6.6
  lists the settings).
- The Zstandard 1.5.7 and Snappy 1.1.9 sources for `benchmark_generalization`.
- The exact versions of the toolchain and system libraries (Section 3).
- For Fig. 1, the `Makefile` as of commit `42c75b6` (Section 2).
