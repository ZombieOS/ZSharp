#define _CRT_SECURE_NO_WARNINGS

#include "hash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Sha256Context {
    uint32_t state[8];
    uint64_t byte_count;
    unsigned char block[64];
    size_t block_length;
} Sha256Context;

static const uint32_t SHA256_CONSTANTS[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotate_right(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32u - amount));
}

static void sha256_transform(Sha256Context *context,
                             const unsigned char block[64]) {
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;
    for (index = 0; index < 16; index++) {
        size_t offset = index * 4;
        words[index] = ((uint32_t)block[offset] << 24) |
                       ((uint32_t)block[offset + 1] << 16) |
                       ((uint32_t)block[offset + 2] << 8) |
                       (uint32_t)block[offset + 3];
    }
    for (index = 16; index < 64; index++) {
        uint32_t s0 = rotate_right(words[index - 15], 7) ^
                      rotate_right(words[index - 15], 18) ^
                      (words[index - 15] >> 3);
        uint32_t s1 = rotate_right(words[index - 2], 17) ^
                      rotate_right(words[index - 2], 19) ^
                      (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0; index < 64; index++) {
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t first = h + sum1 + choose + SHA256_CONSTANTS[index] +
                         words[index];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(Sha256Context *context) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(context->state, initial, sizeof(initial));
    context->byte_count = 0;
    context->block_length = 0;
}

static void sha256_update(Sha256Context *context, const unsigned char *data,
                          size_t length) {
    size_t index = 0;
    context->byte_count += (uint64_t)length;
    while (index < length) {
        size_t available = sizeof(context->block) - context->block_length;
        size_t copy_length = length - index < available
            ? length - index
            : available;
        memcpy(context->block + context->block_length, data + index,
               copy_length);
        context->block_length += copy_length;
        index += copy_length;
        if (context->block_length == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->block_length = 0;
        }
    }
}

static void sha256_final(Sha256Context *context,
                         unsigned char output[ZSHARP_SHA256_SIZE]) {
    uint64_t bit_count = context->byte_count * 8u;
    size_t index;
    context->block[context->block_length++] = 0x80u;
    if (context->block_length > 56) {
        memset(context->block + context->block_length, 0,
               sizeof(context->block) - context->block_length);
        sha256_transform(context, context->block);
        context->block_length = 0;
    }
    memset(context->block + context->block_length, 0,
           56 - context->block_length);
    for (index = 0; index < 8; index++) {
        context->block[63 - index] = (unsigned char)(bit_count & 0xffu);
        bit_count >>= 8;
    }
    sha256_transform(context, context->block);
    for (index = 0; index < 8; index++) {
        output[index * 4] = (unsigned char)(context->state[index] >> 24);
        output[index * 4 + 1] =
            (unsigned char)(context->state[index] >> 16);
        output[index * 4 + 2] =
            (unsigned char)(context->state[index] >> 8);
        output[index * 4 + 3] = (unsigned char)context->state[index];
    }
}

void zsharp_project_identity(const char *project_id,
                             unsigned char output[ZSHARP_SHA256_SIZE]) {
    static const unsigned char prefix[] = "zsharp.project.identity.v1:";
    Sha256Context context;
    sha256_init(&context);
    sha256_update(&context, prefix, sizeof(prefix) - 1);
    sha256_update(&context, (const unsigned char *)project_id,
                  strlen(project_id));
    sha256_final(&context, output);
}

int zsharp_sha256_file_except(const char *path, long skip_offset,
                              size_t skip_length,
                              unsigned char output[ZSHARP_SHA256_SIZE]) {
    FILE *file;
    Sha256Context context;
    unsigned char buffer[8192];
    long position = 0;
    size_t read_count;
    if (skip_offset < 0) return 0;
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    sha256_init(&context);
    while ((read_count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        size_t begin = 0;
        size_t end = read_count;
        long chunk_end = position + (long)read_count;
        long skip_end = skip_offset + (long)skip_length;
        if (position < skip_offset && chunk_end > skip_offset) {
            end = (size_t)(skip_offset - position);
        } else if (position >= skip_offset && position < skip_end) {
            begin = skip_end < chunk_end
                ? (size_t)(skip_end - position)
                : read_count;
        }
        if (end > begin) sha256_update(&context, buffer + begin, end - begin);
        if (chunk_end > skip_end && position < skip_offset) {
            size_t after = (size_t)(skip_end - position);
            sha256_update(&context, buffer + after, read_count - after);
        }
        position = chunk_end;
    }
    if (ferror(file) || fclose(file) != 0) return 0;
    sha256_final(&context, output);
    return 1;
}

void zsharp_hash_hex(const unsigned char hash[ZSHARP_SHA256_SIZE],
                     char output[ZSHARP_SHA256_SIZE * 2 + 1]) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < ZSHARP_SHA256_SIZE; index++) {
        output[index * 2] = digits[hash[index] >> 4];
        output[index * 2 + 1] = digits[hash[index] & 0x0fu];
    }
    output[ZSHARP_SHA256_SIZE * 2] = '\0';
}
