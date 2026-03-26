/*
 * MD5 hash implementation for NextUI.
 * Based on RFC 1321. Public domain.
 * Same API pattern as sha256.h/sha256.c in this project.
 *
 * All symbols prefixed with nui_md5_ to avoid collision with
 * rcheevos' internal rhash/md5 when linked with LTO.
 */

#include "md5.h"
#include <string.h>
#include <stdio.h>

/* MD5 round constants */
static const uint32_t K[64] = {
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
	0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

/* Per-round shift amounts */
static const uint8_t S[64] = {
	7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
	5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
	4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
	6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

#define ROTLEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void nui_md5_transform(NUI_MD5_CTX* ctx, const uint8_t block[NUI_MD5_BLOCK_SIZE]) {
	uint32_t M[16];
	uint32_t a, b, c, d, f, g, temp;

	/* Decode block into 16 little-endian 32-bit words */
	for (int i = 0; i < 16; i++) {
		M[i] = (uint32_t)block[i * 4]
		     | ((uint32_t)block[i * 4 + 1] << 8)
		     | ((uint32_t)block[i * 4 + 2] << 16)
		     | ((uint32_t)block[i * 4 + 3] << 24);
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];

	for (int i = 0; i < 64; i++) {
		if (i < 16) {
			f = (b & c) | (~b & d);
			g = i;
		} else if (i < 32) {
			f = (d & b) | (~d & c);
			g = (5 * i + 1) % 16;
		} else if (i < 48) {
			f = b ^ c ^ d;
			g = (3 * i + 5) % 16;
		} else {
			f = c ^ (b | ~d);
			g = (7 * i) % 16;
		}

		temp = d;
		d = c;
		c = b;
		b = b + ROTLEFT((a + f + K[i] + M[g]), S[i]);
		a = temp;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
}

void nui_md5_init(NUI_MD5_CTX* ctx) {
	ctx->count = 0;
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
}

void nui_md5_update(NUI_MD5_CTX* ctx, const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	size_t offset = (size_t)(ctx->count % NUI_MD5_BLOCK_SIZE);
	ctx->count += len;

	/* Fill partial block */
	if (offset > 0) {
		size_t avail = NUI_MD5_BLOCK_SIZE - offset;
		if (len < avail) {
			memcpy(ctx->buffer + offset, p, len);
			return;
		}
		memcpy(ctx->buffer + offset, p, avail);
		nui_md5_transform(ctx, ctx->buffer);
		p += avail;
		len -= avail;
	}

	/* Process full blocks */
	while (len >= NUI_MD5_BLOCK_SIZE) {
		nui_md5_transform(ctx, p);
		p += NUI_MD5_BLOCK_SIZE;
		len -= NUI_MD5_BLOCK_SIZE;
	}

	/* Buffer remainder */
	if (len > 0) {
		memcpy(ctx->buffer, p, len);
	}
}

void nui_md5_final(NUI_MD5_CTX* ctx, uint8_t digest[NUI_MD5_DIGEST_SIZE]) {
	uint64_t bits = ctx->count * 8;
	size_t offset = (size_t)(ctx->count % NUI_MD5_BLOCK_SIZE);

	/* Pad with 0x80 then zeros */
	ctx->buffer[offset++] = 0x80;

	if (offset > 56) {
		/* Not enough room for length; pad to end and process */
		memset(ctx->buffer + offset, 0, NUI_MD5_BLOCK_SIZE - offset);
		nui_md5_transform(ctx, ctx->buffer);
		offset = 0;
	}
	memset(ctx->buffer + offset, 0, 56 - offset);

	/* Append length in bits as 64-bit little-endian */
	for (int i = 0; i < 8; i++) {
		ctx->buffer[56 + i] = (uint8_t)(bits >> (i * 8));
	}
	nui_md5_transform(ctx, ctx->buffer);

	/* Encode state as little-endian bytes */
	for (int i = 0; i < 4; i++) {
		digest[i * 4]     = (uint8_t)(ctx->state[i]);
		digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 8);
		digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 16);
		digest[i * 4 + 3] = (uint8_t)(ctx->state[i] >> 24);
	}
}

void nui_md5_hash(const void* data, size_t len, uint8_t digest[NUI_MD5_DIGEST_SIZE]) {
	NUI_MD5_CTX ctx;
	nui_md5_init(&ctx);
	nui_md5_update(&ctx, data, len);
	nui_md5_final(&ctx, digest);
}

void nui_md5_hash_hex(const void* data, size_t len, char hex_out[33]) {
	uint8_t digest[NUI_MD5_DIGEST_SIZE];
	nui_md5_hash(data, len, digest);
	for (int i = 0; i < NUI_MD5_DIGEST_SIZE; i++) {
		sprintf(hex_out + i * 2, "%02x", digest[i]);
	}
	hex_out[32] = '\0';
}
