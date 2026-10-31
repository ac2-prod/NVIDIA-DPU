#include "netzip.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <omp.h>

#ifdef __aarch64__
#include <arm_neon.h>
#endif


// --- Helper Functions for bfloat16 conversion ---

// bfloat16 is essentially the top 16 bits of a float32.
// Conversion from float32 to bfloat16 typically involves truncation or rounding.
// Torch's default seems to be rounding, but here we'll implement truncation for simplicity first,
// or check if we need to match PyTorch exactly (Round-to-Nearest-Even).
// Let's implement Round-to-Nearest-Even to be closer to PyTorch.

union FloatUint32 {
    float f;
    uint32_t u;
};

float bf16_to_fp32(uint16_t b) {
    union FloatUint32 val;
    val.u = (uint32_t)b << 16;
    return val.f;
}

uint16_t fp32_to_bf16(float f) {
    union FloatUint32 val;
    val.f = f;
    uint32_t u = val.u;

    // Check for NaN
    if ((u & 0x7FFFFFFF) > 0x7F800000) {
        return (uint16_t)(u >> 16); 
    }

    // Round-to-Nearest-Even (RNE)
    // lsb: bit 16
    // guard: bit 15
    // sticky: bits 0-14
    
    uint32_t rounding_bias = 0x7FFF + ((u >> 16) & 1);
    return (uint16_t)((u + rounding_bias) >> 16);
}


// --- Bit Grouping ---

#ifdef __aarch64__
static inline uint8x8_t transpose8x8(uint8x8_t x) {
    // Hacker's Delight 8x8 bit transpose suitable for 64-bit register
    // x is uint8x8_t (64 bits)
    uint64x1_t v = vreinterpret_u64_u8(x);
    
    uint64_t m1 = 0x00AA00AA00AA00AAULL;
    uint64_t m2 = 0x0000CCCC0000CCCCULL;
    uint64_t m3 = 0x00000000F0F0F0F0ULL;

    uint64x1_t t;
    
    // t = (x ^ (x >> 7)) & m1;  x = x ^ t ^ (t << 7);
    t = vand_u64(veor_u64(v, vshr_n_u64(v, 7)), vcreate_u64(m1));
    v = veor_u64(veor_u64(v, t), vshl_n_u64(t, 7));
    
    // t = (x ^ (x >> 14)) & m2; x = x ^ t ^ (t << 14);
    t = vand_u64(veor_u64(v, vshr_n_u64(v, 14)), vcreate_u64(m2));
    v = veor_u64(veor_u64(v, t), vshl_n_u64(t, 14));
    
    // t = (x ^ (x >> 28)) & m3; x = x ^ t ^ (t << 28);
    t = vand_u64(veor_u64(v, vshr_n_u64(v, 28)), vcreate_u64(m3));
    v = veor_u64(veor_u64(v, t), vshl_n_u64(t, 28));
    
    return vreinterpret_u8_u64(v);
}
#endif

void netzip_bit_group(const uint16_t* input, size_t num_elements, uint8_t* output) {
    // We want to group the i-th bit of all elements together.
    
    memset(output, 0, num_elements * 2);
    size_t plane_size = (num_elements + 7) / 8;
    
    size_t i = 0;

#ifdef __aarch64__
    // NEON Optimization: Process 8 elements (128 bits) at a time
    // We produce exactly 1 byte for each of the 16 planes in one go.
    size_t main_loop_end = num_elements - (num_elements % 8);
    size_t num_chunks = main_loop_end / 8;
    
    #pragma omp parallel for
    for (size_t k = 0; k < num_chunks; ++k) {
        size_t local_i = k * 8;
        size_t byte_idx = local_i / 8;
        
        // Load 8x uint16_t
        uint16x8_t val = vld1q_u16(input + local_i);
        
        // De-interleave into Low and High bytes
        uint8x16_t val_u8 = vreinterpretq_u8_u16(val);
        // vuzp1q_u8(v, v) extracts even bytes (Low bytes of u16) into lower half
        // vuzp2q_u8(v, v) extracts odd bytes (High bytes of u16) into lower half
        // (Note: u16 is little endian, so low byte is even idx, high byte is odd idx)
        
        uint8x16_t low_dup = vuzp1q_u8(val_u8, val_u8);
        uint8x16_t high_dup = vuzp2q_u8(val_u8, val_u8);
        
        uint8x8_t low = vget_low_u8(low_dup);
        uint8x8_t high = vget_low_u8(high_dup);
        
        // Transpose 8x8 bit matrices
        // After transpose, byte k contains the k-th bit of all 8 elements.
        uint8x8_t low_t = transpose8x8(low);
        uint8x8_t high_t = transpose8x8(high);
        
        // Store results scattered to planes
        // low_t contains planes 0..7
        // high_t contains planes 8..15
        
        // Unroll stores
        output[0 * plane_size + byte_idx] = vget_lane_u8(low_t, 0);
        output[1 * plane_size + byte_idx] = vget_lane_u8(low_t, 1);
        output[2 * plane_size + byte_idx] = vget_lane_u8(low_t, 2);
        output[3 * plane_size + byte_idx] = vget_lane_u8(low_t, 3);
        output[4 * plane_size + byte_idx] = vget_lane_u8(low_t, 4);
        output[5 * plane_size + byte_idx] = vget_lane_u8(low_t, 5);
        output[6 * plane_size + byte_idx] = vget_lane_u8(low_t, 6);
        output[7 * plane_size + byte_idx] = vget_lane_u8(low_t, 7);

        output[8 * plane_size + byte_idx] = vget_lane_u8(high_t, 0);
        output[9 * plane_size + byte_idx] = vget_lane_u8(high_t, 1);
        output[10 * plane_size + byte_idx] = vget_lane_u8(high_t, 2);
        output[11 * plane_size + byte_idx] = vget_lane_u8(high_t, 3);
        output[12 * plane_size + byte_idx] = vget_lane_u8(high_t, 4);
        output[13 * plane_size + byte_idx] = vget_lane_u8(high_t, 5);
        output[14 * plane_size + byte_idx] = vget_lane_u8(high_t, 6);
        output[15 * plane_size + byte_idx] = vget_lane_u8(high_t, 7);
    }
    i = main_loop_end;
#endif
    
    for (; i < num_elements; ++i) {
        uint16_t val = input[i];
        size_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8; // 0..7
        
        // For each of the 16 bit planes
        for (int plane = 0; plane < 16; ++plane) {
            if ((val >> plane) & 1) {
                output[plane * plane_size + byte_idx] |= (1 << bit_idx);
            }
        }
    }
}


// --- Byte Grouping ---

void netzip_byte_group(const uint16_t* input, size_t num_elements, uint8_t* output) {
    uint8_t* low_ptr = output;
    uint8_t* high_ptr = output + num_elements;
    
    size_t i = 0;

#ifdef __aarch64__
    // NEON Optimization
    // Process 16 elements (32 bytes) per iteration using 128-bit registers
    size_t num_blocks = num_elements / 16;
    size_t main_loop_end = num_blocks * 16;
    
    // OpenMP Parallel For using canonical loop form (k=0 to N)
    // This ensures OpenMP definitely parallelizes it.
    #pragma omp parallel
    {
        #pragma omp master
        {
            // printf("[Debug] netzip_byte_group running with %d threads\n", omp_get_num_threads());
        }

        #pragma omp for
        for (size_t k = 0; k < num_blocks; ++k) {
            size_t idx = k * 16;
            uint16x8_t v1 = vld1q_u16(input + idx);
            uint16x8_t v2 = vld1q_u16(input + idx + 8);

            uint8x16_t b1 = vreinterpretq_u8_u16(v1); // 16 bytes: L H L H L H L H ...
            uint8x16_t b2 = vreinterpretq_u8_u16(v2); 
            
            // uzp1 takes even elements (0, 2, 4...) -> Low bytes
            // uzp2 takes odd elements (1, 3, 5...) -> High bytes
            uint8x16_t low = vuzp1q_u8(b1, b2);
            uint8x16_t high = vuzp2q_u8(b1, b2);
            
            // Store
            vst1q_u8(low_ptr + idx, low);
            vst1q_u8(high_ptr + idx, high);
        }
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; ++i) {
        uint16_t val = input[i];
        low_ptr[i] = (uint8_t)(val & 0xFF);
        high_ptr[i] = (uint8_t)(val >> 8);
    }
}


// --- Delta Encoding ---

// --- Delta Encoding ---

void netzip_diff_encode(const uint16_t* input, size_t num_elements, double shift_val, uint16_t* output) {
    size_t i = 0;

    // NEON optimization disabled to Ensure Double Precision match with Python
    // Python uses float64 for shift subtraction. Doing this in float32 NEON causes precision mismatch.
    // Falls back to scalar double implementation below.
    /*
#ifdef __aarch64__
    // ... (NEON code removed/disabled) ...
#endif
    */

    for (; i < num_elements; ++i) {
        // 1. Convert bfloat16 (uint16) to float32
        float f = bf16_to_fp32(input[i]);

        
        // 2. Subtract shift_val (using double precision to match Python)
        double diff = (double)f - (double)shift_val;
        
        // 3. Convert back to bfloat16 (uint16)
        output[i] = fp32_to_bf16((float)diff);
    }
}

void netzip_diff_int(const uint16_t* input1, const uint16_t* input2, size_t num_elements, uint16_t* output) {
    for (size_t i = 0; i < num_elements; ++i) {
        // 単純な整数減算。
        // C言語では、符号なし演算はラップアラウンドしますが、これはオーバーフローフラグを無視した場合の
        // "int16"減算（生のビット列として見た場合）の望ましい動作です。
        // Python: difference_int16 = all_bf16_step1 - all_bf16_step0 (as int16)
        output[i] = input1[i] - input2[i];
    }
}

void netzip_diff_bfloat16(const uint16_t* input1, const uint16_t* input2, size_t num_elements, uint16_t* output) {
    for (size_t i = 0; i < num_elements; ++i) {
        float f1 = bf16_to_fp32(input1[i]);
        float f2 = bf16_to_fp32(input2[i]);
        float diff = f1 - f2;
        output[i] = fp32_to_bf16(diff);
    }
}

// --- Inverse Transformations ---

// --- Inverse Transformations ---

void netzip_bit_ungroup(const uint8_t* input, size_t num_elements, uint16_t* output) {
    // Reverse of bit_group
    size_t plane_size = (num_elements + 7) / 8;
    memset(output, 0, num_elements * 2);

    // Parallelizing bit_ungroup is tricky because of the scattered writes ( |= ) 
    // to the same output elements from different planes if we parallelize by plane.
    // However, if we parallelize by byte_idx (output index), each thread owns a chunk of 'output'.
    // It needs to read from 16 different 'input' locations (planes).
    // This is safe. Read-only input, Disjoint output.

    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        size_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;

        uint16_t val = 0;
        for (int plane = 0; plane < 16; ++plane) {
            // Get the bit from the corresponding plane
            uint8_t packed_byte = input[plane * plane_size + byte_idx];
            
            if ((packed_byte >> bit_idx) & 1) {
                val |= (1 << plane);
            }
        }
        output[i] = val;
    }
}

void netzip_byte_ungroup(const uint8_t* input, size_t num_elements, uint16_t* output) {
    // Reverse of byte_group
    const uint8_t* low_ptr = input;
    const uint8_t* high_ptr = input + num_elements;

    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        uint8_t low = low_ptr[i];
        uint8_t high = high_ptr[i];
        output[i] = (uint16_t)low | ((uint16_t)high << 8);
    }
}

// --- byte_ungroup (serial, no OMP, NEON) ---
// netzip_byte_group_serial の逆。OMP parallel 内から呼ぶ用 (team-creation overhead なし)。
// input layout: [low_bytes (num_elements) | high_bytes (num_elements)]
// 純粋な並べ替えの逆操作なので完全可逆 (bit-exact)。
void netzip_byte_ungroup_serial(const uint8_t* input, size_t num_elements,
                                uint16_t* output) {
    const uint8_t* low_ptr  = input;
    const uint8_t* high_ptr = input + num_elements;
    size_t i = 0;

#ifdef __aarch64__
    size_t num_blocks16  = num_elements / 16;
    size_t main_loop_end = num_blocks16 * 16;
    for (size_t k = 0; k < num_blocks16; ++k) {
        size_t idx = k * 16;
        uint8x16_t l = vld1q_u8(low_ptr  + idx);
        uint8x16_t h = vld1q_u8(high_ptr + idx);
        // low/high バイトをインターリーブ再結合: [l0,h0,l1,h1,...]
        vst1q_u8((uint8_t*)(output + idx),     vzip1q_u8(l, h));  // elems 0..7
        vst1q_u8((uint8_t*)(output + idx + 8), vzip2q_u8(l, h));  // elems 8..15
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; ++i)
        output[i] = (uint16_t)low_ptr[i] | ((uint16_t)high_ptr[i] << 8);
}

// --- bit_ungroup (serial, no OMP) ---
// netzip_bit_group_serial の逆。OMP parallel 内から呼ぶ用。
// 16 プレーン (各 plane_size バイト) からビットを集めて u16 を復元する。
// 完全可逆 (bit-exact)。forward は NEON 転置で高速だが、逆は要素毎にプレーンを
// 走査する scalar 実装 (restore スループットはこの非対称性を反映する)。
void netzip_bit_ungroup_serial(const uint8_t* input, size_t num_elements,
                               uint16_t* output) {
    size_t plane_size = (num_elements + 7) / 8;
    for (size_t i = 0; i < num_elements; ++i) {
        size_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;
        uint16_t val = 0;
        for (int plane = 0; plane < 16; ++plane) {
            uint8_t packed = input[plane * plane_size + byte_idx];
            if ((packed >> bit_idx) & 1) val |= (uint16_t)(1u << plane);
        }
        output[i] = val;
    }
}

void netzip_diff_decode(const uint16_t* input, size_t num_elements, float shift_val, uint16_t* output) {
    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        // 1. Convert diff (bfloat16/uint16) to float32
        float f_diff = bf16_to_fp32(input[i]);
        
        // 2. Add shift_val
        float orig = f_diff + shift_val;
        
        // 3. Convert back to bfloat16
        output[i] = fp32_to_bf16(orig);
    }
}

void netzip_diff_int_decode(const uint16_t* input_diff, const uint16_t* base, size_t num_elements, uint16_t* output) {
    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        // output = base + diff
        output[i] = base[i] + input_diff[i];
    }
}

void netzip_diff_bfloat16_decode(const uint16_t* input_diff, const uint16_t* base, size_t num_elements, uint16_t* output) {
    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        float f_diff = bf16_to_fp32(input_diff[i]);
        float f_base = bf16_to_fp32(base[i]);
        float orig = f_base + f_diff;
        output[i] = fp32_to_bf16(orig);
    }
}




// --- Byte Delta Encoding (Matching Python) ---
void netzip_byte_delta_encode(const uint8_t* input, size_t num_bytes, uint8_t* output) {
    if (num_bytes == 0) return;
    output[0] = input[0];
    
    // Simple scalar loop
    for (size_t i = 1; i < num_bytes; ++i) {
        output[i] = (uint8_t)(input[i] - input[i-1]);
    }
}

// --- Profiling Support ---

__thread NetZIPProfile g_profile;

void netzip_reset_profile() {
    memset(&g_profile, 0, sizeof(NetZIPProfile));
}

float netzip_find_min(const uint16_t* input, size_t num_elements) {
    float min_val = INFINITY;
    
#ifdef __aarch64__
    // NEON Optimization
    // Initialize min vector with INF
    float32x4_t v_min = vdupq_n_f32(INFINITY);
    
    size_t num_blocks = num_elements / 8;
    size_t main_loop_end = num_blocks * 8;
    
    // We can't easily reduce across threads for strict min without synchronization.
    // Given preproc is often memory bound or close to it, single thread or reduction logic is needed.
    // For simplicity in this context, let's do single-threaded NEON or just assume OpenMP reduction is used by caller if needed.
    // But here we implement scalar/vector min search.
    
    for (size_t i = 0; i < main_loop_end; i += 8) {
        uint16x8_t q_in = vld1q_u16(input + i);
        
        // Split low/high
        uint32x4_t u_low = vmovl_u16(vget_low_u16(q_in));
        uint32x4_t u_high = vmovl_u16(vget_high_u16(q_in));
        
        // Valid bf16 -> fp32 (shift 16)
        u_low = vshlq_n_u32(u_low, 16);
        u_high = vshlq_n_u32(u_high, 16);
        
        float32x4_t f_low = vreinterpretq_f32_u32(u_low);
        float32x4_t f_high = vreinterpretq_f32_u32(u_high);
        
        v_min = vminq_f32(v_min, f_low);
        v_min = vminq_f32(v_min, f_high);
    }
    
    // Reduce vector
    float min_arr[4];
    vst1q_f32(min_arr, v_min);
    for (int j = 0; j < 4; j++) {
        if (min_arr[j] < min_val) min_val = min_arr[j];
    }
    size_t start_i = main_loop_end;
#else
    size_t start_i = 0;
#endif

    for (size_t i = start_i; i < num_elements; ++i) {
        float f = bf16_to_fp32(input[i]);
        if (f < min_val) min_val = f;
    }
    return min_val;
}

void netzip_diff_encode_profiled(const uint16_t* input, size_t num_elements, double shift_val, uint16_t* output, NetZIPProfile* stats) {
#ifdef PROFILE
    struct timespec t0, t1, t2, t3;
    
    // 1. BF16 -> FP32
    // We need a temporary float buffer to measure strictly separate phases.
    // This overhead is part of "profiling cost" but gives us breakdown.
    float* temp_fp32 = (float*)malloc(sizeof(float) * num_elements);
    if (!temp_fp32) return; // Should handle error
    
    clock_gettime(CLOCK_MONOTONIC, &t0);
    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        temp_fp32[i] = bf16_to_fp32(input[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    
    if (stats) {
        long long delta = get_ns_diff(t0, t1);
        #pragma omp atomic
        stats->bf16_to_fp32_ns += delta;
        // Verbose Debug
        if (delta == 0 && stats->bf16_to_fp32_ns < 1000) {
             printf("[DEBUG] bf16_to_fp32: t0=%ld.%09ld, t1=%ld.%09ld, delta=%lld\n", 
                    t0.tv_sec, t0.tv_nsec, t1.tv_sec, t1.tv_nsec, delta);
        }
    }
    
    // 2. Sub (FP32 arithmetic)
    clock_gettime(CLOCK_MONOTONIC, &t1);
    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        temp_fp32[i] -= shift_val;
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    
    if (stats) {
        long long delta = get_ns_diff(t1, t2);
        #pragma omp atomic
        stats->sub_ns += delta;
    }
    
    // 3. FP32 -> BF16 (Rounding/Conversion)
    clock_gettime(CLOCK_MONOTONIC, &t2);
    #pragma omp parallel for
    for (size_t i = 0; i < num_elements; ++i) {
        output[i] = fp32_to_bf16(temp_fp32[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);
    
    if (stats) {
        long long delta = get_ns_diff(t2, t3);
        #pragma omp atomic
        stats->fp32_to_bf16_ns += delta;
    }
    
    free(temp_fp32);
#else
    if (stats) {
        printf("[WARNING] netzip_diff_encode_profiled called but PROFILE macro is NOT defined! Stats will be 0.\n");
    }
    // Fallback to fused implementation if generic
    netzip_diff_encode(input, num_elements, shift_val, output);
#endif
}

// === Serial versions for use inside OMP parallel regions ===
// OMP parallel 内から呼ぶ版。内部に #pragma omp を持たない。

// --- byte_group (serial, no OMP) ---
void netzip_byte_group_serial(const uint16_t* input, size_t num_elements, uint8_t* output) {
    uint8_t* low_ptr  = output;
    uint8_t* high_ptr = output + num_elements;
    size_t i = 0;

#ifdef __aarch64__
    size_t num_blocks    = num_elements / 16;
    size_t main_loop_end = num_blocks * 16;

    for (size_t k = 0; k < num_blocks; ++k) {
        size_t idx = k * 16;
        uint16x8_t v1 = vld1q_u16(input + idx);
        uint16x8_t v2 = vld1q_u16(input + idx + 8);

        uint8x16_t b1 = vreinterpretq_u8_u16(v1);
        uint8x16_t b2 = vreinterpretq_u8_u16(v2);

        vst1q_u8(low_ptr  + idx, vuzp1q_u8(b1, b2));
        vst1q_u8(high_ptr + idx, vuzp2q_u8(b1, b2));
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; ++i) {
        uint16_t val = input[i];
        low_ptr[i]  = (uint8_t)(val & 0xFF);
        high_ptr[i] = (uint8_t)(val >> 8);
    }
}

// --- bit_group (serial, no OMP, batch-of-64 optimized) ---
// 最適化: 64要素（8グループ×8要素）をバッチ処理。
// 8×8バイト行列転置（TRN命令）でプレーン方向に再配置し、
// vst1_u8 で 8byte 一括ストア。散乱バイトストア回数を 128→32/batch に削減。
void netzip_bit_group_serial(const uint16_t* input, size_t num_elements, uint8_t* output) {
    memset(output, 0, num_elements * 2);
    size_t plane_size = (num_elements + 7) / 8;
    size_t i = 0;

#ifdef __aarch64__
    // === Batch-of-64: 8 groups of 8 elements per iteration ===
    size_t num_batches = num_elements / 64;

    for (size_t batch = 0; batch < num_batches; batch++) {
        size_t base      = batch * 64;
        size_t byte_base = batch * 8;

        // Step 1: transpose8x8 on each of 8 groups
        uint8x8_t low_r[8], high_r[8];
        for (int g = 0; g < 8; g++) {
            uint16x8_t val   = vld1q_u16(input + base + g * 8);
            uint8x16_t val_u8 = vreinterpretq_u8_u16(val);
            uint8x8_t low  = vget_low_u8(vuzp1q_u8(val_u8, val_u8));
            uint8x8_t high = vget_low_u8(vuzp2q_u8(val_u8, val_u8));
            low_r[g]  = transpose8x8(low);
            high_r[g] = transpose8x8(high);
        }

        // Step 2: 8×8 byte matrix transpose on low_r[0..7]
        // low_r[g][p] = plane p's byte from group g
        // → col[p] = {low_r[0][p], ..., low_r[7][p]} = 8 bytes for plane p
        //
        // 3-stage TRN transpose: byte→16bit→32bit
        uint8x8x2_t  s0 = vtrn_u8(low_r[0], low_r[1]);
        uint8x8x2_t  s1 = vtrn_u8(low_r[2], low_r[3]);
        uint8x8x2_t  s2 = vtrn_u8(low_r[4], low_r[5]);
        uint8x8x2_t  s3 = vtrn_u8(low_r[6], low_r[7]);

        uint16x4x2_t m0 = vtrn_u16(vreinterpret_u16_u8(s0.val[0]), vreinterpret_u16_u8(s1.val[0]));
        uint16x4x2_t m1 = vtrn_u16(vreinterpret_u16_u8(s0.val[1]), vreinterpret_u16_u8(s1.val[1]));
        uint16x4x2_t m2 = vtrn_u16(vreinterpret_u16_u8(s2.val[0]), vreinterpret_u16_u8(s3.val[0]));
        uint16x4x2_t m3 = vtrn_u16(vreinterpret_u16_u8(s2.val[1]), vreinterpret_u16_u8(s3.val[1]));

        uint32x2x2_t u0 = vtrn_u32(vreinterpret_u32_u16(m0.val[0]), vreinterpret_u32_u16(m2.val[0]));
        uint32x2x2_t u1 = vtrn_u32(vreinterpret_u32_u16(m0.val[1]), vreinterpret_u32_u16(m2.val[1]));
        uint32x2x2_t u2 = vtrn_u32(vreinterpret_u32_u16(m1.val[0]), vreinterpret_u32_u16(m3.val[0]));
        uint32x2x2_t u3 = vtrn_u32(vreinterpret_u32_u16(m1.val[1]), vreinterpret_u32_u16(m3.val[1]));

        // Store low planes 0-7 (8 bytes per plane)
        vst1_u8(output + 0 * plane_size + byte_base, vreinterpret_u8_u32(u0.val[0]));
        vst1_u8(output + 1 * plane_size + byte_base, vreinterpret_u8_u32(u2.val[0]));
        vst1_u8(output + 2 * plane_size + byte_base, vreinterpret_u8_u32(u1.val[0]));
        vst1_u8(output + 3 * plane_size + byte_base, vreinterpret_u8_u32(u3.val[0]));
        vst1_u8(output + 4 * plane_size + byte_base, vreinterpret_u8_u32(u0.val[1]));
        vst1_u8(output + 5 * plane_size + byte_base, vreinterpret_u8_u32(u2.val[1]));
        vst1_u8(output + 6 * plane_size + byte_base, vreinterpret_u8_u32(u1.val[1]));
        vst1_u8(output + 7 * plane_size + byte_base, vreinterpret_u8_u32(u3.val[1]));

        // Step 3: Same 8×8 byte transpose on high_r[0..7] for planes 8-15
        s0 = vtrn_u8(high_r[0], high_r[1]);
        s1 = vtrn_u8(high_r[2], high_r[3]);
        s2 = vtrn_u8(high_r[4], high_r[5]);
        s3 = vtrn_u8(high_r[6], high_r[7]);

        m0 = vtrn_u16(vreinterpret_u16_u8(s0.val[0]), vreinterpret_u16_u8(s1.val[0]));
        m1 = vtrn_u16(vreinterpret_u16_u8(s0.val[1]), vreinterpret_u16_u8(s1.val[1]));
        m2 = vtrn_u16(vreinterpret_u16_u8(s2.val[0]), vreinterpret_u16_u8(s3.val[0]));
        m3 = vtrn_u16(vreinterpret_u16_u8(s2.val[1]), vreinterpret_u16_u8(s3.val[1]));

        u0 = vtrn_u32(vreinterpret_u32_u16(m0.val[0]), vreinterpret_u32_u16(m2.val[0]));
        u1 = vtrn_u32(vreinterpret_u32_u16(m0.val[1]), vreinterpret_u32_u16(m2.val[1]));
        u2 = vtrn_u32(vreinterpret_u32_u16(m1.val[0]), vreinterpret_u32_u16(m3.val[0]));
        u3 = vtrn_u32(vreinterpret_u32_u16(m1.val[1]), vreinterpret_u32_u16(m3.val[1]));

        vst1_u8(output +  8 * plane_size + byte_base, vreinterpret_u8_u32(u0.val[0]));
        vst1_u8(output +  9 * plane_size + byte_base, vreinterpret_u8_u32(u2.val[0]));
        vst1_u8(output + 10 * plane_size + byte_base, vreinterpret_u8_u32(u1.val[0]));
        vst1_u8(output + 11 * plane_size + byte_base, vreinterpret_u8_u32(u3.val[0]));
        vst1_u8(output + 12 * plane_size + byte_base, vreinterpret_u8_u32(u0.val[1]));
        vst1_u8(output + 13 * plane_size + byte_base, vreinterpret_u8_u32(u2.val[1]));
        vst1_u8(output + 14 * plane_size + byte_base, vreinterpret_u8_u32(u1.val[1]));
        vst1_u8(output + 15 * plane_size + byte_base, vreinterpret_u8_u32(u3.val[1]));
    }

    // Remaining groups (8 elements at a time, < 64 elements)
    i = num_batches * 64;
    size_t remaining_groups = (num_elements - i) / 8;
    for (size_t k = 0; k < remaining_groups; k++) {
        size_t local_i  = i + k * 8;
        size_t byte_idx = local_i / 8;

        uint16x8_t val   = vld1q_u16(input + local_i);
        uint8x16_t val_u8 = vreinterpretq_u8_u16(val);
        uint8x8_t low  = vget_low_u8(vuzp1q_u8(val_u8, val_u8));
        uint8x8_t high = vget_low_u8(vuzp2q_u8(val_u8, val_u8));
        uint8x8_t low_t  = transpose8x8(low);
        uint8x8_t high_t = transpose8x8(high);

        uint8_t tmp_lo[8], tmp_hi[8];
        vst1_u8(tmp_lo, low_t);
        vst1_u8(tmp_hi, high_t);
        for (int p = 0; p < 8; p++) {
            output[p * plane_size + byte_idx]       = tmp_lo[p];
            output[(p + 8) * plane_size + byte_idx] = tmp_hi[p];
        }
    }
    i += remaining_groups * 8;
#endif

    for (; i < num_elements; ++i) {
        uint16_t val = input[i];
        size_t byte_idx = i / 8;
        uint8_t bit_idx = i % 8;
        for (int plane = 0; plane < 16; ++plane) {
            if ((val >> plane) & 1) {
                output[plane * plane_size + byte_idx] |= (1 << bit_idx);
            }
        }
    }
}

// --- diff_encode (serial, NEON optimized, float32 truncation) ---
// 元の netzip_diff_encode は double 精度スカラーで遅い (2.2ms)。
// この版は float32 + NEON で高速化。truncation 丸め (差分値の bf16 化で最大 1 ULP。ただし
// 復元後は bit 一致せず、元値に対する誤差は ±1 ULP に収まらない = 非可逆)。
void netzip_diff_encode_serial(const uint16_t* input, size_t num_elements,
                                float shift_val, uint16_t* output) {
    size_t i = 0;

#ifdef __aarch64__
    size_t num_blocks16  = num_elements / 16;
    size_t main_loop_end = num_blocks16 * 16;
    float32x4_t v_shift  = vdupq_n_f32(shift_val);

    for (size_t k = 0; k < num_blocks16; ++k) {
        size_t idx = k * 16;

        uint16x8_t q0 = vld1q_u16(input + idx);
        uint16x8_t q1 = vld1q_u16(input + idx + 8);

        // q0: bf16→fp32→sub→truncate→bf16
        float32x4_t f0_lo = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(q0)),  16)), v_shift);
        float32x4_t f0_hi = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(q0)), 16)), v_shift);
        uint16x8_t d0 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f0_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f0_hi), 16));

        // q1: 同様
        float32x4_t f1_lo = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(q1)),  16)), v_shift);
        float32x4_t f1_hi = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(q1)), 16)), v_shift);
        uint16x8_t d1 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f1_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f1_hi), 16));

        vst1q_u16(output + idx,     d0);
        vst1q_u16(output + idx + 8, d1);
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; i++) {
        float f = bf16_to_fp32(input[i]);
        output[i] = fp32_to_bf16(f - shift_val);
    }
}

// --- Fused: diff_encode + byte_group in one NEON pass (serial, no OMP) ---
// OMP parallel 内から呼ぶ用。team-creation overhead なし。
// 各 OMP スレッドが自分の担当範囲 (sub_elem 要素) を渡して呼ぶ。
void netzip_diff_byte_group_serial(const uint16_t* input, size_t num_elements,
                                    float shift_val, uint8_t* output) {
    uint8_t* low_ptr  = output;
    uint8_t* high_ptr = output + num_elements;

    size_t i = 0;

#ifdef __aarch64__
    size_t num_blocks16  = num_elements / 16;
    size_t main_loop_end = num_blocks16 * 16;
    float32x4_t v_shift  = vdupq_n_f32(shift_val);

    for (size_t k = 0; k < num_blocks16; ++k) {
        size_t idx = k * 16;

        uint16x8_t q0 = vld1q_u16(input + idx);
        uint16x8_t q1 = vld1q_u16(input + idx + 8);

        float32x4_t f0_lo = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(q0)),  16)), v_shift);
        float32x4_t f0_hi = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(q0)), 16)), v_shift);
        uint16x8_t d0 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f0_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f0_hi), 16));

        float32x4_t f1_lo = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(q1)),  16)), v_shift);
        float32x4_t f1_hi = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(q1)), 16)), v_shift);
        uint16x8_t d1 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f1_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f1_hi), 16));

        uint8x16_t b0 = vreinterpretq_u8_u16(d0);
        uint8x16_t b1 = vreinterpretq_u8_u16(d1);
        vst1q_u8(low_ptr  + idx, vuzp1q_u8(b0, b1));
        vst1q_u8(high_ptr + idx, vuzp2q_u8(b0, b1));
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; i++) {
        float f    = bf16_to_fp32(input[i]);
        uint16_t d = fp32_to_bf16(f - shift_val);
        low_ptr[i]  = (uint8_t)(d & 0xFF);
        high_ptr[i] = (uint8_t)(d >> 8);
    }
}

// --- Fused inverse: byte_ungroup + diff_decode in one NEON pass (serial, no OMP) ---
// netzip_diff_byte_group_serial の逆。OMP parallel 内から呼ぶ用 (team-creation overhead なし)。
// 各 OMP スレッドが自分の担当範囲 (sub_elem 要素) を渡して呼ぶ。
void netzip_diff_byte_ungroup_serial(const uint8_t* input, size_t num_elements,
                                     float shift_val, uint16_t* output) {
    const uint8_t* low_ptr  = input;
    const uint8_t* high_ptr = input + num_elements;

    size_t i = 0;

#ifdef __aarch64__
    size_t num_blocks16  = num_elements / 16;
    size_t main_loop_end = num_blocks16 * 16;
    float32x4_t v_shift  = vdupq_n_f32(shift_val);

    for (size_t k = 0; k < num_blocks16; ++k) {
        size_t idx = k * 16;

        // low/high バイト平面を読み戻し、bf16 値へインターリーブで再結合。
        uint8x16_t l = vld1q_u8(low_ptr  + idx);
        uint8x16_t h = vld1q_u8(high_ptr + idx);
        uint16x8_t d0 = vreinterpretq_u16_u8(vzip1q_u8(l, h)); // elems 0..7
        uint16x8_t d1 = vreinterpretq_u16_u8(vzip2q_u8(l, h)); // elems 8..15

        // bf16 -> fp32 (bf16 を上位16bitへ), shift を加算, bf16 へ戻す (truncation)。
        float32x4_t f0_lo = vaddq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(d0)),  16)), v_shift);
        float32x4_t f0_hi = vaddq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(d0)), 16)), v_shift);
        uint16x8_t o0 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f0_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f0_hi), 16));

        float32x4_t f1_lo = vaddq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(d1)),  16)), v_shift);
        float32x4_t f1_hi = vaddq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(d1)), 16)), v_shift);
        uint16x8_t o1 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f1_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f1_hi), 16));

        vst1q_u16(output + idx,     o0);
        vst1q_u16(output + idx + 8, o1);
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; i++) {
        uint16_t d = (uint16_t)low_ptr[i] | ((uint16_t)high_ptr[i] << 8);
        float f    = bf16_to_fp32(d);
        output[i]  = fp32_to_bf16(f + shift_val);
    }
}

// ===========================================================================
// 整数 (bit-exact, lossless) 版: idiff_byte
// ---------------------------------------------------------------------------
// 入力を生の uint16 列とみなし、サブチャンク内で隣接差分を mod 2^16 で取り、
// その後 byte grouping する。浮動小数 diff_byte が truncation rounding により復元後に
// bit 一致しない (非可逆。誤差は ±1 ULP に収まらない) のに対し、こちらは整数演算のみ
// なので round-trip が完全可逆 (memcmp 一致)。
//
//   forward : d[0] = x[0],  d[i] = x[i] - x[i-1]  (mod 2^16) → low/high byte 平面
//   inverse : x[0] = d[0],  x[i] = x[i-1] + d[i]  (mod 2^16)
//
// shift (netzip_find_min) は浮動小数ドメインの前処理なので整数版では不要・未使用。
// 差分はサブチャンク境界をまたがない (各 OMP スレッドが独立サブチャンクを担当する
// 並列性のため)。これは float 版 diff_byte と同じ粒度 (サブチャンク内 reset)。
// ===========================================================================

// --- 整数 diff + byte_group (serial, no OMP) ---
// 出力レイアウト: [low_bytes (num_elements) | high_bytes (num_elements)]
void netzip_idiff_byte_group_serial(const uint16_t* input, size_t num_elements,
                                    uint8_t* output) {
    uint8_t* low_ptr  = output;
    uint8_t* high_ptr = output + num_elements;
    if (num_elements == 0) return;

    size_t i = 0;
    uint16_t prev = 0;   // サブチャンク先頭は前値 0 ⇒ d[0] = x[0]

#ifdef __aarch64__
    // 16要素ブロック: 2本の u16x8 を差分し、byte_group の vuzp で平面分割。
    size_t num_blocks16  = num_elements / 16;
    size_t main_loop_end = num_blocks16 * 16;

    for (size_t k = 0; k < num_blocks16; ++k) {
        size_t idx = k * 16;
        uint16x8_t q0 = vld1q_u16(input + idx);       // x[idx   .. idx+7]
        uint16x8_t q1 = vld1q_u16(input + idx + 8);    // x[idx+8 .. idx+15]

        // x[i-1] を 1 レーンずらして埋める。先頭レーンは直前ブロックの prev。
        uint16x8_t p0 = vsetq_lane_u16(prev, vextq_u16(q0, q0, 7), 0); // [prev, x0..x6]
        uint16x8_t p1 = vextq_u16(q0, q1, 7);                          // [x7, x8..x14]

        uint16x8_t d0 = vsubq_u16(q0, p0);   // mod 2^16 は u16 wrap で自動
        uint16x8_t d1 = vsubq_u16(q1, p1);

        uint8x16_t b0 = vreinterpretq_u8_u16(d0);
        uint8x16_t b1 = vreinterpretq_u8_u16(d1);
        vst1q_u8(low_ptr  + idx, vuzp1q_u8(b0, b1));
        vst1q_u8(high_ptr + idx, vuzp2q_u8(b0, b1));

        prev = vgetq_lane_u16(q1, 7);        // 次ブロックの x[i-1]
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; i++) {
        uint16_t x = input[i];
        uint16_t d = (uint16_t)(x - prev);
        low_ptr[i]  = (uint8_t)(d & 0xFF);
        high_ptr[i] = (uint8_t)(d >> 8);
        prev = x;
    }
}

// --- 整数 byte_ungroup + 累積和 (serial, no OMP), idiff の逆 ---
// 入力レイアウト: [low_bytes (num_elements) | high_bytes (num_elements)]
void netzip_idiff_byte_ungroup_serial(const uint8_t* input, size_t num_elements,
                                      uint16_t* output) {
    const uint8_t* low_ptr  = input;
    const uint8_t* high_ptr = input + num_elements;
    if (num_elements == 0) return;

    size_t i = 0;
    uint16_t prev = 0;   // x[i] = prev + d[i]; 先頭 prev=0 ⇒ x[0]=d[0]

#ifdef __aarch64__
    size_t num_blocks16  = num_elements / 16;
    size_t main_loop_end = num_blocks16 * 16;
    const uint16x8_t z = vdupq_n_u16(0);

    for (size_t k = 0; k < num_blocks16; ++k) {
        size_t idx = k * 16;
        // low/high バイト平面を読み戻し、d[i] (u16) をインターリーブ再結合。
        uint8x16_t l = vld1q_u8(low_ptr  + idx);
        uint8x16_t h = vld1q_u8(high_ptr + idx);
        uint16x8_t d0 = vreinterpretq_u16_u8(vzip1q_u8(l, h));  // d[idx   .. idx+7]
        uint16x8_t d1 = vreinterpretq_u16_u8(vzip2q_u8(l, h));  // d[idx+8 .. idx+15]

        // 各 8 レーン内で inclusive prefix-sum (Hillis–Steele, ゼロ埋めシフト)。
        // vextq_u16(z, v, 8-s): v を s レーン分だけ上位へずらし下位をゼロ埋め。
        uint16x8_t v0 = d0;
        v0 = vaddq_u16(v0, vextq_u16(z, v0, 7));   // += shift1
        v0 = vaddq_u16(v0, vextq_u16(z, v0, 6));   // += shift2
        v0 = vaddq_u16(v0, vextq_u16(z, v0, 4));   // += shift4
        v0 = vaddq_u16(v0, vdupq_n_u16(prev));     // + 直前までの累積
        prev = vgetq_lane_u16(v0, 7);

        uint16x8_t v1 = d1;
        v1 = vaddq_u16(v1, vextq_u16(z, v1, 7));
        v1 = vaddq_u16(v1, vextq_u16(z, v1, 6));
        v1 = vaddq_u16(v1, vextq_u16(z, v1, 4));
        v1 = vaddq_u16(v1, vdupq_n_u16(prev));
        prev = vgetq_lane_u16(v1, 7);

        vst1q_u16(output + idx,     v0);
        vst1q_u16(output + idx + 8, v1);
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; i++) {
        uint16_t d = (uint16_t)low_ptr[i] | ((uint16_t)high_ptr[i] << 8);
        prev = (uint16_t)(prev + d);
        output[i] = prev;
    }
}

// --- Fused: diff_encode + byte_group in one NEON pass (OMP parallel版) ---
// Memory traffic: read N×2 bytes (input) + write N×2 bytes (output).
// vs two-pass: read N×2 + write N×2 (diff) + read N×2 + write N×2 (group) = 4× the traffic.
// This halves preproc DRAM traffic → critical for reaching 20+ GB/s.
// ※ OMP parallel 内から呼ばないこと (team-creation overhead が発生する)。
void netzip_diff_byte_group(const uint16_t* input, size_t num_elements,
                             float shift_val, uint8_t* output) {
    uint8_t* low_ptr  = output;
    uint8_t* high_ptr = output + num_elements;

    size_t i = 0;

#ifdef __aarch64__
    size_t num_blocks16  = num_elements / 16;
    size_t main_loop_end = num_blocks16 * 16;

    #pragma omp parallel for schedule(static)
    for (size_t k = 0; k < num_blocks16; ++k) {
        float32x4_t v_shift = vdupq_n_f32(shift_val);
        size_t idx = k * 16;

        // Load 16 bf16 values as two uint16x8_t
        uint16x8_t q0 = vld1q_u16(input + idx);
        uint16x8_t q1 = vld1q_u16(input + idx + 8);

        // diff_encode q0 (truncation rounding: up to 1 ULP of the difference value. The round
        // trip is lossy: restored values are not bit-exact and not bounded to +/-1 ULP)
        float32x4_t f0_lo = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(q0)),  16)), v_shift);
        float32x4_t f0_hi = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(q0)), 16)), v_shift);
        uint16x8_t d0 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f0_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f0_hi), 16));

        // diff_encode q1
        float32x4_t f1_lo = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(q1)),  16)), v_shift);
        float32x4_t f1_hi = vsubq_f32(
            vreinterpretq_f32_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(q1)), 16)), v_shift);
        uint16x8_t d1 = vcombine_u16(
            vshrn_n_u32(vreinterpretq_u32_f32(f1_lo), 16),
            vshrn_n_u32(vreinterpretq_u32_f32(f1_hi), 16));

        // byte_group: split low/high bytes via uzp
        uint8x16_t b0 = vreinterpretq_u8_u16(d0);
        uint8x16_t b1 = vreinterpretq_u8_u16(d1);
        vst1q_u8(low_ptr  + idx, vuzp1q_u8(b0, b1));
        vst1q_u8(high_ptr + idx, vuzp2q_u8(b0, b1));
    }
    i = main_loop_end;
#endif

    for (; i < num_elements; i++) {
        float f    = bf16_to_fp32(input[i]);
        uint16_t d = fp32_to_bf16(f - shift_val);
        low_ptr[i]  = (uint8_t)(d & 0xFF);
        high_ptr[i] = (uint8_t)(d >> 8);
    }
}

