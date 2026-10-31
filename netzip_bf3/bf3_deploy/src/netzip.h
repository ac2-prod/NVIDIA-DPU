#ifndef NETZIP_H
#define NETZIP_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define CLOCK_MONOTONIC 1

typedef struct {
    // E2E timestamps
    struct timespec t_start;
    struct timespec t_end;
    
    // Breakdown durations (ns)
    long long setup_ns;
    
    // Diff breakdown
    long long diff_total_ns;
    long long min_ns;
    long long bf16_to_fp32_ns;
    long long sub_ns;
    long long fp32_to_bf16_ns;
    
    // Group breakdown
    long long group_total_ns;
    long long byte_group_ns;
    long long bit_group_ns;
    
    // Codec
    long long codec_ns;
    
    // Finalize
    long long finalize_ns;
    
    long long e2e_ns;
} NetZIPProfile;

// Helper to get ns diff
static inline long long get_ns_diff(struct timespec start, struct timespec end) {
    return (long long)(end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
}

// Global or Thread-local profile object
extern __thread NetZIPProfile g_profile;

void netzip_reset_profile();

// New functions
float netzip_find_min(const uint16_t* input, size_t num_elements);
void netzip_diff_encode_profiled(const uint16_t* input, size_t num_elements, double shift_val, uint16_t* output, NetZIPProfile* stats);


#ifdef __cplusplus
extern "C" {
#endif

// Core NetZIP transformation functions

/**
 * @brief Performs bit grouping on an array of 16-bit integers.
 * 
 * Rearranges bits such that the i-th bit of all elements are grouped together.
 * Similar to slicing bits and packing them.
 * 
 * @param input Pointer to the input array of uint16_t (representing bfloat16 or int16)
 * @param num_elements Number of elements in the input array
 * @param output Pointer to the output buffer. Must be large enough (num_elements * 2 bytes).
 */
void netzip_bit_group(const uint16_t* input, size_t num_elements, uint8_t* output);

/**
 * @brief Performs byte grouping on an array of 16-bit integers.
 * 
 * Separates the high bytes and low bytes of the input elements.
 * 
 * @param input Pointer to the input array of uint16_t
 * @param num_elements Number of elements in the input array
 * @param output Pointer to the output buffer. Must be large enough (num_elements * 2 bytes).
 */
void netzip_byte_group(const uint16_t* input, size_t num_elements, uint8_t* output);

/**
 * @brief Performs delta encoding on bfloat16 valus.
 * 
 * Converts bfloat16 (as uint16_t) to float32, subtracts a shift value, 
 * and converts back to bfloat16 (as uint16_t).
 * 
 * @param input Pointer to the input array of uint16_t
 * @param num_elements Number of elements in the input array
 * @param shift_val The value to subtract (in float32)
 * @param output Pointer to the output buffer (array of uint16_t).
 */
void netzip_diff_encode(const uint16_t* input, size_t num_elements, double shift_val, uint16_t* output);
void netzip_byte_delta_encode(const uint8_t* input, size_t num_bytes, uint8_t* output);

/**
 * @brief Fused diff_encode + byte_group in a single NEON pass.
 *
 * Halves DRAM traffic vs two-pass (read once, write once).
 * Uses truncation rounding when narrowing the fp32 difference to bf16 (up to 1 ULP
 * of the difference value). The round trip is therefore lossy: the restored tensor
 * is not bit-exact, and its error is not bounded to +/-1 ULP of the original value.
 *
 * @param input     Input bf16 array (uint16_t)
 * @param num_elements  Number of elements
 * @param shift_val Shift value (float32) to subtract
 * @param output    Output buffer (num_elements*2 bytes): [low_bytes | high_bytes]
 */
void netzip_diff_byte_group(const uint16_t* input, size_t num_elements,
                             float shift_val, uint8_t* output);

/**
 * @brief Serial (no-OMP) versions for use inside OMP parallel regions.
 *
 * OMP parallel 内から呼ぶ版。内部に #pragma omp parallel for を持たないため
 * team-creation overhead が発生しない。各 OMP スレッドが自分の担当 sub_elem 要素を渡す。
 */
void netzip_byte_group_serial(const uint16_t* input, size_t num_elements, uint8_t* output);
void netzip_bit_group_serial(const uint16_t* input, size_t num_elements, uint8_t* output);

/**
 * @brief Serial (no-OMP) inverses of byte_group / bit_group. 純粋な並べ替えの逆 ⇒
 *        round-trip は bit-exact (memcmp 完全一致)。OMP parallel 内から呼ぶ用。
 */
void netzip_byte_ungroup_serial(const uint8_t* input, size_t num_elements, uint16_t* output);
void netzip_bit_ungroup_serial(const uint8_t* input, size_t num_elements, uint16_t* output);
void netzip_diff_encode_serial(const uint16_t* input, size_t num_elements,
                                float shift_val, uint16_t* output);
void netzip_diff_byte_group_serial(const uint16_t* input, size_t num_elements,
                                    float shift_val, uint8_t* output);

/**
 * @brief Inverse of netzip_diff_byte_group_serial: fused byte_ungroup + diff_decode
 *        in a single NEON pass (serial, no OMP).
 *
 * OMP parallel 内から呼ぶ用。team-creation overhead なし。
 * input layout (= diff_byte_group の出力): [low_bytes (num_elements) | high_bytes (num_elements)]
 * 各要素を 16bit に再結合し、bf16 として shift_val を加算して元の bf16 を近似復元する
 * (非可逆: bit 一致せず、誤差は ±1 ULP に収まらない)。
 *
 * @param input     byte-grouped + diff-encoded 入力 (num_elements*2 bytes)
 * @param num_elements  要素数
 * @param shift_val 復元時に加算する shift 値 (group 側と同じ値)
 * @param output    出力 bf16 配列 (uint16_t, num_elements 要素)
 */
void netzip_diff_byte_ungroup_serial(const uint8_t* input, size_t num_elements,
                                     float shift_val, uint16_t* output);

/**
 * @brief 整数 (bit-exact, lossless) diff_byte。OMP parallel 内から呼ぶ serial 版。
 *
 * 入力を生 uint16 列とみなし、サブチャンク内で d[i]=x[i]-x[i-1] (mod 2^16) を取り
 * byte grouping する。shift は不要 (整数演算のみ)。round-trip は完全可逆。
 *
 * forward 出力レイアウト: [low_bytes (num_elements) | high_bytes (num_elements)]
 */
void netzip_idiff_byte_group_serial(const uint16_t* input, size_t num_elements,
                                    uint8_t* output);

/**
 * @brief netzip_idiff_byte_group_serial の逆: byte_ungroup + 累積和 (mod 2^16)。
 *        bit-exact に元の uint16 列を復元する。
 */
void netzip_idiff_byte_ungroup_serial(const uint8_t* input, size_t num_elements,
                                      uint16_t* output);


/**
 * @brief Performs element-wise integer subtraction (input1 - input2).
 * 
 * @param input1 Pointer to the first array of uint16_t
 * @param input2 Pointer to the second array of uint16_t
 * @param num_elements Number of elements
 * @param output Pointer to the output buffer
 */
void netzip_diff_int(const uint16_t* input1, const uint16_t* input2, size_t num_elements, uint16_t* output);

/**
 * @brief Performs element-wise bfloat16 subtraction (input1 - input2).
 * 
 * Converts inputs to float32, subtracts, and converts back to bfloat16.
 * 
 * @param input1 Pointer to the first array of uint16_t
 * @param input2 Pointer to the second array of uint16_t
 * @param num_elements Number of elements
 * @param output Pointer to the output buffer
 */
void netzip_diff_bfloat16(const uint16_t* input1, const uint16_t* input2, size_t num_elements, uint16_t* output);

// --- Inverse Transformations ---

/**
 * @brief Reverse of netzip_bit_group.
 * 
 * Reassembles 16-bit integers from the bit-grouped buffer.
 * 
 * @param input Pointer to the bit-grouped input buffer.
 * @param num_elements Number of elements to reconstruct.
 * @param output Pointer to the output array of uint16_t.
 */
void netzip_bit_ungroup(const uint8_t* input, size_t num_elements, uint16_t* output);

/**
 * @brief Reverse of netzip_byte_group.
 * 
 * Reassembles 16-bit integers from low/high byte arrays.
 * 
 * @param input Pointer to the byte-grouped input buffer.
 * @param num_elements Number of elements to reconstruct.
 * @param output Pointer to the output array of uint16_t.
 */
void netzip_byte_ungroup(const uint8_t* input, size_t num_elements, uint16_t* output);

/**
 * @brief Reverse of netzip_diff_encode (scalar).
 * 
 * @param input Pointer to the diff-encoded input array.
 * @param num_elements Number of elements.
 * @param shift_val The shift value used during encoding.
 * @param output Pointer to the output array.
 */
void netzip_diff_decode(const uint16_t* input, size_t num_elements, float shift_val, uint16_t* output);

/**
 * @brief Reverse of netzip_diff_int.
 * 
 * Performs element-wise integer addition (input_diff + base).
 * 
 * @param input_diff Pointer to the difference array.
 * @param base Pointer to the base array (subtrahend in encoding).
 * @param num_elements Number of elements.
 * @param output Pointer to the output array.
 */
void netzip_diff_int_decode(const uint16_t* input_diff, const uint16_t* base, size_t num_elements, uint16_t* output);

/**
 * @brief Reverse of netzip_diff_bfloat16.
 * 
 * Performs element-wise bfloat16 addition.
 * 
 * @param input_diff Pointer to the difference array.
 * @param base Pointer to the base array.
 * @param num_elements Number of elements.
 * @param output Pointer to the output array.
 */
void netzip_diff_bfloat16_decode(const uint16_t* input_diff, const uint16_t* base, size_t num_elements, uint16_t* output);


// Helper for bfloat16 conversion if needed externally
float bf16_to_fp32(uint16_t b);
uint16_t fp32_to_bf16(float f);

#ifdef __cplusplus
}
#endif

#endif // NETZIP_H
