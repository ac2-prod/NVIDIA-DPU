
// --- Byte Delta Encoding (Matching Python) ---
void netzip_byte_delta_encode(const uint8_t* input, size_t num_bytes, uint8_t* output) {
    if (num_bytes == 0) return;
    output[0] = input[0];
    
    // Simple scalar loop
    for (size_t i = 1; i < num_bytes; ++i) {
        output[i] = (uint8_t)(input[i] - input[i-1]);
    }
}
