#ifndef NUI_MD5_H
#define NUI_MD5_H

#include <stddef.h>
#include <stdint.h>

/*
 * Standalone MD5 implementation for NextUI.
 * All symbols prefixed with nui_md5_ to avoid collision with
 * rcheevos' internal rhash/md5 when linked with LTO.
 */

#define NUI_MD5_BLOCK_SIZE  64
#define NUI_MD5_DIGEST_SIZE 16

typedef struct {
	uint32_t state[4];
	uint64_t count;
	uint8_t  buffer[NUI_MD5_BLOCK_SIZE];
} NUI_MD5_CTX;

#ifdef __cplusplus
extern "C" {
#endif

void nui_md5_init(NUI_MD5_CTX* ctx);
void nui_md5_update(NUI_MD5_CTX* ctx, const void* data, size_t len);
void nui_md5_final(NUI_MD5_CTX* ctx, uint8_t digest[NUI_MD5_DIGEST_SIZE]);

/* Convenience: hash a buffer in one shot */
void nui_md5_hash(const void* data, size_t len, uint8_t digest[NUI_MD5_DIGEST_SIZE]);

/* Convenience: hash a buffer and write hex string (32 chars + null) */
void nui_md5_hash_hex(const void* data, size_t len, char hex_out[33]);

#ifdef __cplusplus
}
#endif

#endif /* NUI_MD5_H */
