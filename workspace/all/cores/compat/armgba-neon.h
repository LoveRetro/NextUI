// GCC < 10 lacks vld1q_u8_x4 on AArch64 (armGBA only polyfills it for 32-bit ARM)
#if defined(__aarch64__) && !defined(__clang__) && __GNUC__ < 10
#include <arm_neon.h>
static inline uint8x16x4_t vld1q_u8_x4(const uint8_t* p) {
	uint8x16x4_t r = { { vld1q_u8(p), vld1q_u8(p + 16), vld1q_u8(p + 32), vld1q_u8(p + 48) } };
	return r;
}
#endif
