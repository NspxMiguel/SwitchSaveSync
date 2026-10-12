#include "ota.h"
#include <stdio.h>
#include <string.h>

/* FIPS 180-4 SHA-256, streaming in 64-byte blocks. */
static const uint32_t constants[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
    0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3,
    0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354,
    0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3,
    0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
    0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
static uint32_t rotate(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void block(uint32_t state[8], const unsigned char data[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16
            | (uint32_t)data[i * 4 + 2] << 8 | data[i * 4 + 3];
    for (int i = 16; i < 64; i++)
    {
        uint32_t a = w[i - 15], b = w[i - 2];
        w[i] = w[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) + w[i - 7]
            + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5],
             g = state[6], h = state[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t t = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g))
            + constants[i] + w[i];
        uint32_t u = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t;
        d = c;
        c = b;
        b = a;
        a = t + u;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
bool ota_verify_file(const char* path, const unsigned char expected[32])
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
        0x1f83d9ab, 0x5be0cd19 };
    unsigned char data[64];
    size_t n;
    uint64_t length = 0;
    while ((n = fread(data, 1, 64, file)) == 64)
    {
        block(state, data);
        length += 64;
    }
    bool ok = !ferror(file);
    fclose(file);
    length += n;
    data[n++] = 0x80;
    if (n > 56)
    {
        memset(data + n, 0, 64 - n);
        block(state, data);
        n = 0;
    }
    memset(data + n, 0, 56 - n);
    uint64_t bits = length * 8;
    for (int i = 0; i < 8; i++)
        data[63 - i] = (unsigned char)(bits >> (i * 8));
    block(state, data);
    unsigned diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= ((state[i / 4] >> (24 - (i % 4) * 8)) & 255) ^ expected[i];
    return ok && !diff;
}
