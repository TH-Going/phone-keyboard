#include "app.h"

/* ------------------------------------------------------------------ */
/* SHA-1 (RFC 3174), only used for the WebSocket accept key            */
/* ------------------------------------------------------------------ */

static uint32_t rotate_left(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_block(uint32_t state[5], const unsigned char block[64])
{
    uint32_t words[80];
    for (int i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = rotate_left(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_digest(const unsigned char *data, size_t len, unsigned char out[20])
{
    uint32_t state[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t padded = ((len + 8) / 64 + 1) * 64;
    unsigned char *buffer = (unsigned char *)calloc(padded, 1);
    if (!buffer) {
        memset(out, 0, 20);
        return;
    }

    if (len > 0) {
        memcpy(buffer, data, len);
    }
    buffer[len] = 0x80;

    uint64_t bit_length = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) {
        buffer[padded - 1 - (size_t)i] = (unsigned char)(bit_length >> (8 * i));
    }

    for (size_t offset = 0; offset < padded; offset += 64) {
        sha1_block(state, buffer + offset);
    }
    free(buffer);

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(state[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Base64 (RFC 4648)                                                   */
/* ------------------------------------------------------------------ */

void base64_encode(const unsigned char *data, size_t len, char *out, size_t cap)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((len + 2) / 3) * 4 + 1;
    if (!out || cap < needed) {
        if (out && cap > 0) {
            out[0] = 0;
        }
        return;
    }

    size_t written = 0;
    size_t index = 0;
    while (index + 2 < len) {
        uint32_t triple = ((uint32_t)data[index] << 16) |
                          ((uint32_t)data[index + 1] << 8) |
                          (uint32_t)data[index + 2];
        out[written++] = table[(triple >> 18) & 0x3F];
        out[written++] = table[(triple >> 12) & 0x3F];
        out[written++] = table[(triple >> 6) & 0x3F];
        out[written++] = table[triple & 0x3F];
        index += 3;
    }

    if (index < len) {
        uint32_t triple = (uint32_t)data[index] << 16;
        if (index + 1 < len) {
            triple |= (uint32_t)data[index + 1] << 8;
        }
        out[written++] = table[(triple >> 18) & 0x3F];
        out[written++] = table[(triple >> 12) & 0x3F];
        out[written++] = (index + 1 < len) ? table[(triple >> 6) & 0x3F] : '=';
        out[written++] = '=';
    }

    out[written] = 0;
}
