/* Shared input generator for the freestanding VM and native optimizer benchmark. */
static u32 mix(u32 x) {
    x ^= x >> 16; x *= 0x21f0aaad;
    x ^= x >> 15; x *= 0x735a2d97;
    return x ^ (x >> 15);
}
static void expected(u32 page, u8 profile, u8 *out) {
    u32 base = page;
    if (profile == 'R' || profile == 'D') base %= 256;
    if (profile == 'W') base %= 32768;
    if (profile == 'C') base %= 4;
    u32 seed = mix(base ^ 0x64e31b0d);
    if (profile == 'F') {
        /* Different ranks and axes on each side of the cut. No representation
         * or cut positions are supplied to the pager. */
        for (u32 r = 0; r < 64; r++) {
            for (u32 c = 0; c < 64; c++) {
                u32 key = r < 32 ? r * 101 + (c * 13 + (seed & 15)) % 5
                                 : (r * 3 + (seed & 7)) % 7 + c * 131;
                out[r * 64 + c] = (u8)mix(seed + key);
            }
        }
        return;
    }
    if (profile == 'C' || profile == 'D' || profile == 'R' || profile == 'W' || profile == 'U' || (profile == 'M' && (page & 1))) {
        for (u32 k = 0; k < PAGE; k += 4) {
            u32 w = mix(seed + k);
            __builtin_memcpy(out + k, &w, sizeof w);
        }
        if (profile == 'C') {
            /* Four unrelated dense bases deliberately occupy the same fingerprint chain. */
            __builtin_memset(out, 0, 8);
            __builtin_memset(out + PAGE / 2, 0, 8);
            __builtin_memset(out + PAGE - 8, 0, 8);
        }
        if (profile == 'D' && page >= 256) {
            for (u32 j = 0; j < 4; j++) {
                u32 edit = mix(page + 37 * j);
                out[edit % PAGE] ^= (u8)((edit >> 16) | 1);
            }
        }
        return;
    }
    u32 mask[2] = {seed | 1u, mix(seed) & ~1u};
    for (u32 r = 0; r < 64; r++) {
        u32 w = mix(seed + r);
        u8 v[2] = {(u8)w, (u8)(w >> 8)};
        for (u32 c = 0; c < 64; c++)
            out[r * 64 + c] = v[(mask[c / 32] >> (c % 32)) & 1];
    }
}
