# Optimization flags for BlueField-3 (Cortex-A78)
OPT_FLAGS = -O3 -funroll-loops -fopenmp

# Paths
BENCH_DIR = ../deps
ZSTD_DIR = $(BENCH_DIR)/zstd-1.5.7/lib
SNAPPY_DIR = $(BENCH_DIR)/snappy-1.1.9

# Flags
CFLAGS = -Wall $(OPT_FLAGS) -I$(SNAPPY_DIR) -I$(ZSTD_DIR)
CXXFLAGS = -Wall $(OPT_FLAGS) -I$(SNAPPY_DIR)

# Default to profiled build
CFLAGS += -DPROFILE

# Linker flags (Link objects directly + math + zlib + stdc++)
LDFLAGS = -lm -lz -lstdc++

# Source / Object definitions
# --------------------------------

# Snappy
SNAPPY_SRCS = $(SNAPPY_DIR)/snappy.cc $(SNAPPY_DIR)/snappy-c.cc $(SNAPPY_DIR)/snappy-sinksource.cc $(SNAPPY_DIR)/snappy-stubs-internal.cc
SNAPPY_OBJS = snappy.o snappy-c.o snappy-sinksource.o snappy-stubs-internal.o

# Zstd
ZSTD_SRCS_C = $(wildcard $(ZSTD_DIR)/common/*.c) $(wildcard $(ZSTD_DIR)/compress/*.c) $(wildcard $(ZSTD_DIR)/decompress/*.c)
ZSTD_SRCS_S = $(wildcard $(ZSTD_DIR)/decompress/*.S)
ZSTD_OBJS = $(ZSTD_SRCS_C:.c=.o) $(ZSTD_SRCS_S:.S=.o)

# LZ4
LZ4_OBJS = lz4.o lz4hc.o lz4frame.o xxhash.o

# --------------------------------
# Targets
# --------------------------------

all: benchmark dump_compressed benchmark_parallel benchmark_pipeline benchmark_l3

# E2E MPI benchmark (BF3 2-node, NetZIP pipeline).
# mpicc (hpcx) を使い、バンドル LZ4 を直接コンパイルする (pkg-config 不要)。
# ※ hpcx をロード済みであること: module load gcc/11 && module load hpcx/2.24.1
MPICC     ?= mpicc
# 通信専用スレッド版のため pthread が必要。MPI_THREAD_MULTIPLE は hpcx-mt を要する。
E2E_FLAGS  = -O3 -funroll-loops -fopenmp -march=armv8.2-a+fp16 -pthread -I.

benchmark_e2e: benchmark_e2e.c netzip.c netzip.h lz4.c lz4.h
	$(MPICC) $(E2E_FLAGS) -o benchmark_e2e benchmark_e2e.c netzip.c lz4.c -lm -lpthread

# ─── v6: ibverbs RDMA 版 (フェーズ分解計時 / msg-size sweep / cache hot-cold) ───
# データパスは libibverbs (RC QP + RDMA Write with Imm)。接続確立のみ MPI。
# ビルドは BF3 (aarch64) ノード上で行うこと。プレーン hpcx/2.24.1 でよい (-mt 不要)。
rdma_targets: rdma_sanity benchmark_e2e_rdma

rdma_sanity: rdma_sanity.c rdma_transport.c rdma_transport.h
	$(MPICC) $(E2E_FLAGS) -o rdma_sanity rdma_sanity.c rdma_transport.c -lm -libverbs

benchmark_e2e_rdma: benchmark_e2e_rdma.c rdma_transport.c rdma_transport.h netzip.c netzip.h lz4.c lz4.h
	$(MPICC) $(E2E_FLAGS) -o benchmark_e2e_rdma benchmark_e2e_rdma.c rdma_transport.c netzip.c lz4.c -lm -libverbs

# libibverbs が無い環境向けフォールバック (--transport mpi 専用バイナリ)
benchmark_e2e_rdma_noib: benchmark_e2e_rdma.c netzip.c netzip.h lz4.c lz4.h
	$(MPICC) $(E2E_FLAGS) -DNO_IBVERBS -o benchmark_e2e_rdma_noib benchmark_e2e_rdma.c netzip.c lz4.c -lm

# ─── Issue #5: idiff_byte (整数 bit-exact) vs diff_byte (float) 比較 (MPI 不要) ───
# BF3 (aarch64) ノード上でビルドすること。
benchmark_idiff_compare: benchmark_idiff_compare.c netzip.c netzip.h lz4.c lz4.h
	$(CC) -O3 -funroll-loops -fopenmp -march=armv8.2-a+fp16 -I. \
	  -o benchmark_idiff_compare benchmark_idiff_compare.c netzip.c lz4.c -lm

# ─── データセット一般化測定 (全 423 テンソル × 4 preproc × 3 codec, MPI 不要) ───
# snappy(C++)/zstd を使うため codec オブジェクトをリンク。BF3 (aarch64) でビルド。
benchmark_generalization: benchmark_generalization.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o benchmark_generalization benchmark_generalization.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)

benchmark_generalization.o: benchmark_generalization.c netzip.h lz4.h
	$(CC) $(CFLAGS) -c benchmark_generalization.c

# ─── Fig4/Fig5: OpenMP スレッドスケーリング (diff_byte+lz4 / none+snappy, MPI 不要) ───
benchmark_omp_scaling: benchmark_omp_scaling.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o benchmark_omp_scaling benchmark_omp_scaling.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)

benchmark_omp_scaling.o: benchmark_omp_scaling.c netzip.h lz4.h
	$(CC) $(CFLAGS) -c benchmark_omp_scaling.c

# ─── §3.6: diff_byte 復元誤差 (ULP/相対/絶対, サブチャンク位置相関) — MPI/codec 不要 ───
benchmark_diff_error: benchmark_diff_error.c netzip.c netzip.h
	$(CC) -O3 -funroll-loops -fopenmp -march=armv8.2-a+fp16 -I. \
	  -o benchmark_diff_error benchmark_diff_error.c netzip.c -lm

# 0. L3-Cache-Aware Benchmark (sequential chunks, all threads per phase)
benchmark_l3: benchmark_l3.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o benchmark_l3 benchmark_l3.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)

# 0b. Pipeline Benchmark (pthread producer/consumer)
benchmark_pipeline: benchmark_pipeline.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o benchmark_pipeline benchmark_pipeline.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS) -lpthread

# 1. Benchmark Parallel (Profiled)
benchmark_parallel: benchmark_parallel.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o benchmark_parallel benchmark_parallel.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)

# 2. Benchmark Parallel (Fast / No Profile)
# Recursively calls make with CFLAGS stripped of -DPROFILE
fast: clean_objs
	$(MAKE) benchmark_parallel_fast CFLAGS="$(filter-out -DPROFILE,$(CFLAGS))"

benchmark_parallel_fast: benchmark_parallel.c netzip.c netzip.h $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -c benchmark_parallel.c -o benchmark_parallel.o
	$(CC) $(CFLAGS) -c netzip.c -o netzip.o
	$(CC) $(CFLAGS) -o benchmark_parallel_fast benchmark_parallel.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)

# 3. Single Thread Benchmark
benchmark: benchmark.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o benchmark benchmark.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)

# 4. Dump Compressed Tool
dump_compressed: dump_compressed.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o dump_compressed dump_compressed.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)

# 5. Gprof Benchmark (Profiled with -pg)
# Use recursive make to ensure all objects (including deps) are compiled with -pg
# We REMOVE -DPROFILE to profile the actual fast production code, not the instrumented slow code.
benchmark_gprof: clean_objs
	$(MAKE) benchmark_gprof_internal CFLAGS="$(filter-out -DPROFILE,$(CFLAGS)) -pg"

benchmark_gprof_internal: benchmark_parallel.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS)
	$(CC) $(CFLAGS) -o benchmark_gprof benchmark_parallel.o netzip.o $(LZ4_OBJS) $(SNAPPY_OBJS) $(ZSTD_OBJS) $(LDFLAGS)



# --------------------------------
# Rules
# --------------------------------

# Main objects
benchmark_l3.o: benchmark_l3.c netzip.h lz4.h
	$(CC) $(CFLAGS) -c benchmark_l3.c

benchmark_pipeline.o: benchmark_pipeline.c netzip.h lz4.h
	$(CC) $(CFLAGS) -c benchmark_pipeline.c

benchmark_parallel.o: benchmark_parallel.c netzip.h lz4.h
	$(CC) $(CFLAGS) -c benchmark_parallel.c

benchmark.o: benchmark.c netzip.h lz4.h
	$(CC) $(CFLAGS) -c benchmark.c

dump_compressed.o: dump_compressed.c netzip.h lz4.h
	$(CC) $(CFLAGS) -c dump_compressed.c

netzip.o: netzip.c netzip.h
	$(CC) $(CFLAGS) -c netzip.c

# LZ4
lz4.o: lz4.c lz4.h
	$(CC) $(CFLAGS) -c lz4.c

lz4hc.o: lz4hc.c lz4hc.h lz4.h
	$(CC) $(CFLAGS) -c lz4hc.c

lz4frame.o: lz4frame.c lz4frame.h lz4.h lz4hc.h
	$(CC) $(CFLAGS) -c lz4frame.c

xxhash.o: xxhash.c xxhash.h
	$(CC) $(CFLAGS) -c xxhash.c

# Snappy (C++)
snappy.o: $(SNAPPY_DIR)/snappy.cc
	$(CXX) $(CXXFLAGS) -c $(SNAPPY_DIR)/snappy.cc

snappy-c.o: $(SNAPPY_DIR)/snappy-c.cc
	$(CXX) $(CXXFLAGS) -c $(SNAPPY_DIR)/snappy-c.cc

snappy-sinksource.o: $(SNAPPY_DIR)/snappy-sinksource.cc
	$(CXX) $(CXXFLAGS) -c $(SNAPPY_DIR)/snappy-sinksource.cc

snappy-stubs-internal.o: $(SNAPPY_DIR)/snappy-stubs-internal.cc
	$(CXX) $(CXXFLAGS) -c $(SNAPPY_DIR)/snappy-stubs-internal.cc

# Zstd (Generic rule)
$(ZSTD_DIR)/%.o: $(ZSTD_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(ZSTD_DIR)/%.o: $(ZSTD_DIR)/%.S
	$(CC) $(CFLAGS) -c $< -o $@


# Clean
clean:
	rm -f *.o benchmark dump_compressed benchmark_parallel benchmark_parallel_fast benchmark_pipeline benchmark_l3 benchmark_e2e rdma_sanity benchmark_e2e_rdma benchmark_e2e_rdma_noib benchmark_idiff_compare benchmark_generalization benchmark_omp_scaling benchmark_diff_error
	find ../deps -name "*.o" -delete

clean_objs:
	rm -f *.o
