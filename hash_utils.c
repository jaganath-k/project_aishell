#include "hash_utils.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── MD5 (RFC 1321) ────────────────────────────────────────────────────── */

#define MD5_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static const uint32_t md5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint32_t md5_S[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,
};

static void md5_block(uint32_t s[4], const uint8_t b[64])
{
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)b[i*4]
             | ((uint32_t)b[i*4+1] << 8)
             | ((uint32_t)b[i*4+2] << 16)
             | ((uint32_t)b[i*4+3] << 24);

    uint32_t A = s[0], B = s[1], C = s[2], D = s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t F; unsigned g;
        if      (i < 16) { F = (B & C) | (~B & D); g = (unsigned)i; }
        else if (i < 32) { F = (D & B) | (~D & C); g = (5u*(unsigned)i + 1u) % 16u; }
        else if (i < 48) { F = B ^ C ^ D;           g = (3u*(unsigned)i + 5u) % 16u; }
        else             { F = C ^ (B | ~D);         g = (7u*(unsigned)i)      % 16u; }
        F += A + md5_K[i] + M[g];
        A = D; D = C; C = B; B = B + MD5_ROTL(F, md5_S[i]);
    }
    s[0] += A; s[1] += B; s[2] += C; s[3] += D;
}

const char *md5_hexdigest(FILE *fp)
{
    static char hex[33];
    uint32_t s[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    uint64_t bits = 0;
    uint8_t  buf[64];
    size_t   n;

    while ((n = fread(buf, 1, 64, fp)) == 64) {
        md5_block(s, buf);
        bits += 512;
    }
    bits += (uint64_t)n * 8;
    buf[n++] = 0x80;
    if (n > 56) { while (n < 64) buf[n++] = 0; md5_block(s, buf); n = 0; }
    while (n < 56) buf[n++] = 0;
    for (int i = 0; i < 8; i++) buf[56+i] = (uint8_t)(bits >> (i*8)); /* LE */
    md5_block(s, buf);

    for (int i = 0; i < 4; i++)
        snprintf(hex + i*8, 9, "%02x%02x%02x%02x",
                 s[i]&0xffu, (s[i]>>8)&0xffu, (s[i]>>16)&0xffu, (s[i]>>24)&0xffu);
    hex[32] = '\0';
    return hex;
}

/* ── SHA-256 (FIPS 180-4) ──────────────────────────────────────────────── */

static const uint32_t K256[64] = {
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
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define ROTR32(x,n)  (((x)>>(n))|((x)<<(32-(n))))
#define S256_CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define S256_MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define S256_EP0(a)  (ROTR32(a,2)^ROTR32(a,13)^ROTR32(a,22))
#define S256_EP1(e)  (ROTR32(e,6)^ROTR32(e,11)^ROTR32(e,25))
#define S256_SG0(x)  (ROTR32(x,7)^ROTR32(x,18)^((x)>>3))
#define S256_SG1(x)  (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static void sha256_block(uint32_t s[8], const uint8_t b[64])
{
    uint32_t W[64];
    for (int i = 0; i < 16; i++)
        W[i] = ((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)
              |((uint32_t)b[i*4+2]<<8)|(uint32_t)b[i*4+3];
    for (int i = 16; i < 64; i++)
        W[i] = S256_SG1(W[i-2]) + W[i-7] + S256_SG0(W[i-15]) + W[i-16];

    uint32_t a=s[0],b2=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + S256_EP1(e) + S256_CH(e,f,g) + K256[i] + W[i];
        uint32_t t2 = S256_EP0(a) + S256_MAJ(a,b2,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b2; b2=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b2; s[2]+=c; s[3]+=d;
    s[4]+=e; s[5]+=f;  s[6]+=g; s[7]+=h;
}

const char *sha256_hexdigest(FILE *fp)
{
    static char hex[65];
    uint32_t s[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    uint64_t bits = 0;
    uint8_t  buf[64];
    size_t   n;

    while ((n = fread(buf, 1, 64, fp)) == 64) {
        sha256_block(s, buf);
        bits += 512;
    }
    bits += (uint64_t)n * 8;
    buf[n++] = 0x80;
    if (n > 56) { while (n < 64) buf[n++] = 0; sha256_block(s, buf); n = 0; }
    while (n < 56) buf[n++] = 0;
    for (int i = 7; i >= 0; i--) buf[56+(7-i)] = (uint8_t)(bits >> (i*8)); /* BE */
    sha256_block(s, buf);

    for (int i = 0; i < 8; i++)
        snprintf(hex + i*8, 9, "%08x", s[i]);
    hex[64] = '\0';
    return hex;
}
