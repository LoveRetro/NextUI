#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

#define MINARCH_DEFAULT_REWIND_ENABLE           0
#define MINARCH_DEFAULT_REWIND_BUFFER_MB        64
#define MINARCH_DEFAULT_REWIND_GRANULARITY      16
#define MINARCH_DEFAULT_REWIND_AUDIO            0
#define MINARCH_DEFAULT_REWIND_LZ4_ACCELERATION 2

typedef struct {
	size_t offset;
	size_t size;
	uint8_t is_keyframe;  // 1 if this entry is a full state, 0 if delta-encoded
} RewindEntry;

typedef struct {
	uint8_t *buffer;
	size_t capacity;
	size_t head;
	size_t tail;

	RewindEntry *entries;
	int entry_capacity;
	int entry_head;
	int entry_tail;
	int entry_count;

	uint8_t *state_buf;
	size_t state_size;
	uint8_t *scratch;
	size_t scratch_size;

	// Delta compression: store XOR of current vs previous state
	uint8_t *prev_state_enc;   // previous state for delta encoding (compression)
	uint8_t *prev_state_dec;   // previous state for delta decoding (decompression)
	uint8_t *delta_buf;        // scratch buffer for XOR result
	int has_prev_enc;          // 1 if prev_state_enc is valid
	int has_prev_dec;          // 1 if prev_state_dec is valid

	int granularity_frames;
	int interval_ms;
	uint32_t last_push_ms;
	uint32_t last_step_ms;
	int playback_interval_ms;
	int use_time_cadence;
	int frame_counter;
	unsigned int generation;
	int enabled;
	int audio;
	int compress;
	int lz4_acceleration;
	int logged_first;

	// async capture/compression
	pthread_t worker;
	pthread_mutex_t lock;
	pthread_mutex_t queue_mx;
	pthread_cond_t queue_cv;
	int worker_stop;
	int worker_running;
	int locks_ready;

	uint8_t **capture_pool;
	unsigned int *capture_gen;
	uint8_t *capture_busy;
	int pool_size;
	int free_count;
	int *free_stack;

	int queue_capacity;
	int queue_head;
	int queue_tail;
	int queue_count;
	int *queue;
} RewindContext;

typedef enum {
	REWIND_BUF_EMPTY    = 0,
	REWIND_BUF_HAS_DATA = 1,
	REWIND_BUF_FULL     = 2,
} RewindBufferState;

enum {
	REWIND_STEP_EMPTY    = 0, // buffer empty or disabled
	REWIND_STEP_OK       = 1, // stepped back successfully
	REWIND_STEP_CADENCE  = 2, // waiting for playback cadence (don't run core)
};

extern RewindContext rewind_ctx;

int  Rewind_init(size_t state_size);
void Rewind_free(void);
void Rewind_reset(void);
void Rewind_push(int force);
int  Rewind_step_back(void);
void Rewind_sync_encode_state(void);
void Rewind_on_state_change(void);
