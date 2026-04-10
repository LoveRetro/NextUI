/*****************************************************************************
 * SHA-256 implementation (self-contained, no external dependencies)
 *
 * Based on FIPS 180-4. Public domain reference style.
 *****************************************************************************/

#include "sha256.h"
#include <string.h>

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(NUI_SHA256_CTX* ctx, const uint8_t* data) {
	uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)data[i * 4] << 24) |
		       ((uint32_t)data[i * 4 + 1] << 16) |
		       ((uint32_t)data[i * 4 + 2] << 8) |
		       ((uint32_t)data[i * 4 + 3]);
	}
	for (i = 16; i < 64; i++) {
		w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
	}

	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + w[i];
		t2 = EP0(a) + MAJ(a, b, c);
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void nui_sha256_init(NUI_SHA256_CTX* ctx) {
	ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
	ctx->count = 0;
}

void nui_sha256_update(NUI_SHA256_CTX* ctx, const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	size_t fill = (size_t)(ctx->count & 0x3f);
	ctx->count += len;

	if (fill) {
		size_t avail = NUI_SHA256_BLOCK_SIZE - fill;
		if (len < avail) {
			memcpy(ctx->buffer + fill, p, len);
			return;
		}
		memcpy(ctx->buffer + fill, p, avail);
		sha256_transform(ctx, ctx->buffer);
		p += avail;
		len -= avail;
	}
	while (len >= NUI_SHA256_BLOCK_SIZE) {
		sha256_transform(ctx, p);
		p += NUI_SHA256_BLOCK_SIZE;
		len -= NUI_SHA256_BLOCK_SIZE;
	}
	if (len) {
		memcpy(ctx->buffer, p, len);
	}
}

void nui_sha256_final(NUI_SHA256_CTX* ctx, uint8_t digest[NUI_SHA256_DIGEST_SIZE]) {
	uint64_t bits = ctx->count * 8;
	size_t fill = (size_t)(ctx->count & 0x3f);
	int i;

	/* Padding */
	ctx->buffer[fill++] = 0x80;
	if (fill > 56) {
		memset(ctx->buffer + fill, 0, NUI_SHA256_BLOCK_SIZE - fill);
		sha256_transform(ctx, ctx->buffer);
		fill = 0;
	}
	memset(ctx->buffer + fill, 0, 56 - fill);

	/* Length in bits, big-endian */
	for (i = 0; i < 8; i++) {
		ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - i * 8));
	}
	sha256_transform(ctx, ctx->buffer);

	/* Output */
	for (i = 0; i < 8; i++) {
		digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

void nui_sha256_hash(const void* data, size_t len, uint8_t digest[NUI_SHA256_DIGEST_SIZE]) {
	NUI_SHA256_CTX ctx;
	nui_sha256_init(&ctx);
	nui_sha256_update(&ctx, data, len);
	nui_sha256_final(&ctx, digest);
}
