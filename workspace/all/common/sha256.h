#ifndef NUI_SHA256_H
#define NUI_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUI_SHA256_BLOCK_SIZE  64
#define NUI_SHA256_DIGEST_SIZE 32

typedef struct {
	uint32_t state[8];
	uint64_t count;
	uint8_t  buffer[NUI_SHA256_BLOCK_SIZE];
} NUI_SHA256_CTX;

void nui_sha256_init(NUI_SHA256_CTX* ctx);
void nui_sha256_update(NUI_SHA256_CTX* ctx, const void* data, size_t len);
void nui_sha256_final(NUI_SHA256_CTX* ctx, uint8_t digest[NUI_SHA256_DIGEST_SIZE]);

/* Convenience: hash a buffer in one shot */
void nui_sha256_hash(const void* data, size_t len, uint8_t digest[NUI_SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* NUI_SHA256_H */
