#include "ma_internal.h"
#include "ma_rewind.h"

#include <string.h>
#include <time.h>
#include <lz4.h>

RewindContext rewind_ctx = {0};

#define REWIND_ENTRY_SIZE_HINT          4096           // assumed avg entry size for capacity calc
#define REWIND_MIN_ENTRIES              8              // minimum entry table size
#define REWIND_POOL_SIZE_SMALL          3              // capture pool size for small states
#define REWIND_POOL_SIZE_LARGE          4              // capture pool size for large states
#define REWIND_LARGE_STATE_THRESHOLD    (2*1024*1024)  // 2MB threshold for pool sizing
#define REWIND_MAX_BUFFER_MB            256            // max rewind buffer size
#define REWIND_MAX_LZ4_ACCELERATION     64             // max LZ4 acceleration value

static int rewind_warn_empty = 0;

static RewindBufferState Rewind_buffer_state_locked(void) {
	if (rewind_ctx.entry_count == 0) return REWIND_BUF_EMPTY;
	// head == tail with entries means the ring buffer wrapped and is full
	if (rewind_ctx.head == rewind_ctx.tail) return REWIND_BUF_FULL;
	return REWIND_BUF_HAS_DATA;
}

static void* Rewind_worker_thread(void *arg);
static int Rewind_write_entry_locked(const uint8_t *compressed, size_t dest_len, int is_keyframe);
static int Rewind_compress_state(const uint8_t *src, size_t *dest_len, int *is_keyframe_out);
static void Rewind_wait_for_worker_idle(void);

void Rewind_free(void) {
	if (rewind_ctx.worker_running) {
		pthread_mutex_lock(&rewind_ctx.queue_mx);
		rewind_ctx.worker_stop = 1;
		pthread_cond_signal(&rewind_ctx.queue_cv);
		pthread_mutex_unlock(&rewind_ctx.queue_mx);
		pthread_join(rewind_ctx.worker, NULL);
		rewind_ctx.worker_running = 0;
	}

	if (rewind_ctx.capture_pool) {
		for (int i = 0; i < rewind_ctx.pool_size; i++) {
			if (rewind_ctx.capture_pool[i]) free(rewind_ctx.capture_pool[i]);
		}
		free(rewind_ctx.capture_pool);
	}
	if (rewind_ctx.capture_gen) free(rewind_ctx.capture_gen);
	if (rewind_ctx.capture_busy) free(rewind_ctx.capture_busy);
	if (rewind_ctx.free_stack) free(rewind_ctx.free_stack);
	if (rewind_ctx.queue) free(rewind_ctx.queue);
	if (rewind_ctx.buffer) free(rewind_ctx.buffer);
	if (rewind_ctx.entries) free(rewind_ctx.entries);
	if (rewind_ctx.state_buf) free(rewind_ctx.state_buf);
	if (rewind_ctx.scratch) free(rewind_ctx.scratch);
	if (rewind_ctx.prev_state_enc) free(rewind_ctx.prev_state_enc);
	if (rewind_ctx.prev_state_dec) free(rewind_ctx.prev_state_dec);
	if (rewind_ctx.delta_buf) free(rewind_ctx.delta_buf);
	if (rewind_ctx.locks_ready) {
		pthread_mutex_destroy(&rewind_ctx.lock);
		pthread_mutex_destroy(&rewind_ctx.queue_mx);
		pthread_cond_destroy(&rewind_ctx.queue_cv);
	}
	memset(&rewind_ctx, 0, sizeof(rewind_ctx));
	rewinding = 0;
}

void Rewind_reset(void) {
	if (!rewind_ctx.enabled) return;
	Rewind_wait_for_worker_idle();
	pthread_mutex_lock(&rewind_ctx.lock);
	rewind_ctx.head = rewind_ctx.tail = 0;
	rewind_ctx.entry_head = rewind_ctx.entry_tail = rewind_ctx.entry_count = 0;
	rewind_ctx.has_prev_enc = 0;
	rewind_ctx.has_prev_dec = 0;
	pthread_mutex_unlock(&rewind_ctx.lock);
	rewind_ctx.frame_counter = 0;
	rewind_ctx.last_push_ms = 0;
	rewind_ctx.last_step_ms = 0;
	rewind_ctx.generation += 1;

	rewind_ctx.worker_stop = 0;
	if (!rewind_ctx.generation) rewind_ctx.generation = 1; // avoid zero if it wrapped
	// clear pending async work so new snapshots don't mix with stale ones
	if (rewind_ctx.pool_size) {
		pthread_mutex_lock(&rewind_ctx.queue_mx);
		while (rewind_ctx.queue_count > 0) {
			int slot = rewind_ctx.queue[rewind_ctx.queue_head];
			rewind_ctx.queue_head = (rewind_ctx.queue_head + 1) % rewind_ctx.queue_capacity;
			rewind_ctx.queue_count -= 1;
			rewind_ctx.capture_busy[slot] = 0;
		}
		rewind_ctx.queue_head = rewind_ctx.queue_tail = 0;
		rewind_ctx.free_count = 0;
		for (int i = 0; i < rewind_ctx.pool_size; i++) {
			if (!rewind_ctx.capture_busy[i] && rewind_ctx.free_count < rewind_ctx.pool_size) {
				rewind_ctx.free_stack[rewind_ctx.free_count++] = i;
			}
		}
		pthread_mutex_unlock(&rewind_ctx.queue_mx);
	}
	rewinding = 0;
	rewind_warn_empty = 0;
}

static size_t Rewind_free_space_locked(void) {
	RewindBufferState state = Rewind_buffer_state_locked();
	if (state == REWIND_BUF_FULL) return 0;
	if (state == REWIND_BUF_EMPTY) return rewind_ctx.capacity;
	if (rewind_ctx.head >= rewind_ctx.tail)
		return rewind_ctx.capacity - (rewind_ctx.head - rewind_ctx.tail);
	else
		return rewind_ctx.tail - rewind_ctx.head;
}

static void Rewind_drop_oldest_locked(void) {
	if (!rewind_ctx.entry_count) return;
	RewindEntry *e = &rewind_ctx.entries[rewind_ctx.entry_tail];
	rewind_ctx.tail = (e->offset + e->size) % rewind_ctx.capacity;
	rewind_ctx.entry_tail = (rewind_ctx.entry_tail + 1) % rewind_ctx.entry_capacity;
	rewind_ctx.entry_count -= 1;
	if (rewind_ctx.entry_count == 0) {
		rewind_ctx.head = rewind_ctx.tail = 0;
	}
}

// Block until the worker has drained its queue and is not holding any slots
static void Rewind_wait_for_worker_idle(void) {
	if (!rewind_ctx.worker_running || !rewind_ctx.pool_size) return;
	pthread_mutex_lock(&rewind_ctx.queue_mx);
	while (rewind_ctx.queue_count > 0 || rewind_ctx.free_count < rewind_ctx.pool_size) {
		pthread_mutex_unlock(&rewind_ctx.queue_mx);
		struct timespec ts = {0, 1000000}; // 1ms
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&rewind_ctx.queue_mx);
	}
	pthread_mutex_unlock(&rewind_ctx.queue_mx);
}

// Check if an entry overlaps with range [range_start, range_end) in a non-wrapping buffer region
static int Rewind_entry_overlaps_range(int entry_idx, size_t range_start, size_t range_end) {
	RewindEntry *e = &rewind_ctx.entries[entry_idx];
	size_t e_start = e->offset;
	size_t e_end = e->offset + e->size;
	// Check for overlap: ranges overlap if start < other_end AND other_start < end
	return (e_start < range_end) && (range_start < e_end);
}

static int Rewind_write_entry_locked(const uint8_t *compressed, size_t dest_len, int is_keyframe) {
	if (dest_len >= rewind_ctx.capacity) {
		LOG_error("Rewind: state does not fit in buffer\n");
		return 0;
	}

	// If the entry table is full, drop the oldest entry *before* writing so we don't
	// overwrite its metadata (entry_head == entry_tail when full).
	if (rewind_ctx.entry_count == rewind_ctx.entry_capacity) {
		Rewind_drop_oldest_locked();
	}

	size_t write_offset = rewind_ctx.head;

	// If this write would go past the end of the buffer, wrap to 0
	if (write_offset + dest_len > rewind_ctx.capacity) {
		write_offset = 0;
		rewind_ctx.head = 0;
		if (rewind_ctx.entry_count == 0) {
			rewind_ctx.tail = 0;
		}
	}

	// Drop any entries that overlap with the region we're about to write: [write_offset, write_offset + dest_len)
	// We need to check all entries from tail to head and drop any that overlap.
	// Since entries are stored oldest-to-newest, we drop from oldest while they overlap.
	while (rewind_ctx.entry_count > 0) {
		int oldest_idx = rewind_ctx.entry_tail;
		if (Rewind_entry_overlaps_range(oldest_idx, write_offset, write_offset + dest_len)) {
			Rewind_drop_oldest_locked();
		} else {
			break;
		}
	}

	// Still need to make room based on free space calculation
	while (rewind_ctx.entry_count > 0 && Rewind_free_space_locked() <= dest_len) {
		Rewind_drop_oldest_locked();
	}

	// Safety check: if we still can't fit, there's a logic error
	if (Rewind_free_space_locked() <= dest_len && rewind_ctx.entry_count > 0) {
		LOG_error("Rewind: unable to make room for entry (need %zu, have %zu)\n", dest_len, Rewind_free_space_locked());
		return 0;
	}

	memcpy(rewind_ctx.buffer + write_offset, compressed, dest_len);

	RewindEntry *e = &rewind_ctx.entries[rewind_ctx.entry_head];
	e->offset = write_offset;
	e->size = dest_len;
	e->is_keyframe = is_keyframe ? 1 : 0;

	rewind_ctx.head = write_offset + dest_len;
	if (rewind_ctx.head >= rewind_ctx.capacity) rewind_ctx.head = 0;

	rewind_ctx.entry_head = (rewind_ctx.entry_head + 1) % rewind_ctx.entry_capacity;
	if (rewind_ctx.entry_count < rewind_ctx.entry_capacity) {
		rewind_ctx.entry_count += 1;
	} else {
		Rewind_drop_oldest_locked();
	}
	rewind_warn_empty = 0;
	return 1;
}

static int Rewind_compress_state(const uint8_t *src, size_t *dest_len, int *is_keyframe_out) {
	if (!rewind_ctx.scratch || !dest_len) return -1;
	if (is_keyframe_out) *is_keyframe_out = 1; // default to keyframe
	if (!rewind_ctx.compress) {
		*dest_len = rewind_ctx.state_size;
		memcpy(rewind_ctx.scratch, src, rewind_ctx.state_size);
		if (is_keyframe_out) *is_keyframe_out = 1; // raw snapshots are always keyframes
		if (!rewind_ctx.logged_first) {
			rewind_ctx.logged_first = 1;
			LOG_info("Rewind: compression disabled, storing %zu bytes per snapshot\n", rewind_ctx.state_size);
		}
		return 0;
	}

	// Delta compression: XOR current state with previous state
	// The result is mostly zeros for similar consecutive states, which compresses much faster
	const uint8_t *compress_src = src;
	int used_delta = 0;
	if (rewind_ctx.has_prev_enc && rewind_ctx.prev_state_enc && rewind_ctx.delta_buf) {
		size_t state_size = rewind_ctx.state_size;
		uint8_t *delta = rewind_ctx.delta_buf;
		const uint8_t *prev = rewind_ctx.prev_state_enc;
		// Byte-by-byte XOR to avoid unaligned memory access issues
		for (size_t i = 0; i < state_size; i++) {
			delta[i] = src[i] ^ prev[i];
		}
		compress_src = delta;
		used_delta = 1;
	}

	int max_dst = (int)rewind_ctx.scratch_size;
	// acceleration: 1=default speed, higher=faster but slightly lower ratio
	int accel = rewind_ctx.lz4_acceleration > 0 ? rewind_ctx.lz4_acceleration : MINARCH_DEFAULT_REWIND_LZ4_ACCELERATION;
	int res = LZ4_compress_fast((const char*)compress_src, (char*)rewind_ctx.scratch, (int)rewind_ctx.state_size, max_dst, accel);
	if (res <= 0) return -1;
	*dest_len = (size_t)res;

	// Report whether this was a keyframe (full state) or delta
	if (is_keyframe_out) *is_keyframe_out = used_delta ? 0 : 1;

	// Update prev_state_enc with the current state for next delta
	if (rewind_ctx.prev_state_enc) {
		memcpy(rewind_ctx.prev_state_enc, src, rewind_ctx.state_size);
		rewind_ctx.has_prev_enc = 1;
	}

	return 0;
}

int Rewind_init(size_t state_size) {
	Rewind_free();
	// pull current option values directly
	int enable = rewind_cfg_enable;
	int buf_mb = rewind_cfg_buffer_mb;
	int gran = rewind_cfg_granularity;
	int audio = rewind_cfg_audio;
	int compress = rewind_cfg_compress;
	if (!enable) {
		return 0;
	}
	if (!state_size) {
		LOG_info("Rewind: core reported zero serialize size, disabling\n");
		return 0;
	}

	// Bounds check before size_t conversion to avoid negative int issues
	if (buf_mb < 1) buf_mb = 1;
	if (buf_mb > REWIND_MAX_BUFFER_MB) buf_mb = REWIND_MAX_BUFFER_MB;
	size_t buffer_mb = (size_t)buf_mb;

	rewind_ctx.capacity = buffer_mb * 1024 * 1024;
	rewind_ctx.compress = compress;
	if (!rewind_ctx.compress && rewind_ctx.capacity <= state_size) {
		LOG_warn("Rewind: raw snapshots (%zu bytes) do not fit in %zu-byte buffer; falling back to compression\n",
			state_size, rewind_ctx.capacity);
		rewind_ctx.compress = 1;
	}
	int accel = rewind_cfg_lz4_acceleration;
	if (accel < 1) accel = 1;
	if (accel > REWIND_MAX_LZ4_ACCELERATION) accel = REWIND_MAX_LZ4_ACCELERATION;
	rewind_ctx.lz4_acceleration = accel;
	rewind_ctx.logged_first = 0;
	if (rewind_ctx.compress) {
		LOG_info("Rewind: config enable=%i bufferMB=%i interval=%ims audio=%i compression=lz4 (accel=%i)\n",
			enable, buf_mb, gran, audio, rewind_ctx.lz4_acceleration);
	} else {
		LOG_info("Rewind: config enable=%i bufferMB=%i interval=%ims audio=%i compression=raw\n",
			enable, buf_mb, gran, audio);
	}
	rewind_ctx.buffer = calloc(1, rewind_ctx.capacity);
	if (!rewind_ctx.buffer) {
		LOG_error("Rewind: failed to allocate buffer\n");
		return 0;
	}

	rewind_ctx.state_size = state_size;
	rewind_ctx.state_buf = calloc(1, state_size);
	if (!rewind_ctx.state_buf) {
		LOG_error("Rewind: failed to allocate state buffer\n");
		Rewind_free();
		return 0;
	}

	rewind_ctx.scratch_size = LZ4_compressBound((int)state_size);
	if (!rewind_ctx.compress) rewind_ctx.scratch_size = state_size;
	rewind_ctx.scratch = calloc(1, rewind_ctx.scratch_size);
	if (!rewind_ctx.scratch) {
		LOG_error("Rewind: failed to allocate scratch buffer\n");
		Rewind_free();
		return 0;
	}

	// Allocate delta compression buffers (separate for encode/decode to avoid race conditions)
	rewind_ctx.prev_state_enc = calloc(1, state_size);
	rewind_ctx.prev_state_dec = calloc(1, state_size);
	rewind_ctx.delta_buf = calloc(1, state_size);
	if (!rewind_ctx.prev_state_enc || !rewind_ctx.prev_state_dec || !rewind_ctx.delta_buf) {
		LOG_error("Rewind: failed to allocate delta buffers\n");
		Rewind_free();
		return 0;
	}
	rewind_ctx.has_prev_enc = 0;
	rewind_ctx.has_prev_dec = 0;

	int entry_cap = rewind_ctx.capacity / REWIND_ENTRY_SIZE_HINT;
	if (entry_cap < REWIND_MIN_ENTRIES) entry_cap = REWIND_MIN_ENTRIES;
	rewind_ctx.entry_capacity = entry_cap;
	rewind_ctx.entries = calloc(entry_cap, sizeof(RewindEntry));
	if (!rewind_ctx.entries) {
		LOG_error("Rewind: failed to allocate entry table\n");
		Rewind_free();
		return 0;
	}

	rewind_ctx.granularity_frames = 1;
	rewind_ctx.interval_ms = gran < 1 ? 1 : gran; // treat granularity as milliseconds always
	rewind_ctx.use_time_cadence = 1;
	double fps = core.fps > 1.0 ? core.fps : 60.0;
	int frame_ms = (int)(1000.0 / fps);
	if (frame_ms < 1) frame_ms = 1;
	// Capture interval in milliseconds (time-based only)
	int capture_ms = rewind_ctx.interval_ms;
	if (capture_ms < frame_ms) capture_ms = frame_ms;
	// Play back at the capture cadence (match recorded speed) but never faster than native frame time
	int playback_ms = capture_ms;
	if (playback_ms < frame_ms) playback_ms = frame_ms;
	rewind_ctx.playback_interval_ms = playback_ms;
	LOG_info("Rewind: capture_ms=%d, playback_ms=%d (state size=%zu bytes, buffer=%zu bytes, entries=%d)\n",
		capture_ms, playback_ms, state_size, rewind_ctx.capacity, rewind_ctx.entry_capacity);
	rewind_ctx.audio = audio;
	rewind_ctx.enabled = 1;
	rewind_ctx.generation = 1;
	rewind_ctx.worker_stop = 0;
	rewind_ctx.queue_head = rewind_ctx.queue_tail = rewind_ctx.queue_count = 0;


	pthread_mutex_init(&rewind_ctx.lock, NULL);
	pthread_mutex_init(&rewind_ctx.queue_mx, NULL);
	pthread_cond_init(&rewind_ctx.queue_cv, NULL);
	rewind_ctx.locks_ready = 1;

	// set up async capture buffers
	// Larger states need a deeper pool to avoid drops; cap to a modest size to limit RAM
	rewind_ctx.pool_size = (state_size > REWIND_LARGE_STATE_THRESHOLD) ? REWIND_POOL_SIZE_LARGE : REWIND_POOL_SIZE_SMALL;
	if (rewind_ctx.pool_size < 1) rewind_ctx.pool_size = 1;
	rewind_ctx.capture_pool = calloc(rewind_ctx.pool_size, sizeof(uint8_t*));
	rewind_ctx.capture_gen = calloc(rewind_ctx.pool_size, sizeof(unsigned int));
	rewind_ctx.capture_busy = calloc(rewind_ctx.pool_size, sizeof(uint8_t));
	rewind_ctx.free_stack = calloc(rewind_ctx.pool_size, sizeof(int));
	rewind_ctx.queue = calloc(rewind_ctx.pool_size, sizeof(int));
	if (!rewind_ctx.capture_pool || !rewind_ctx.capture_gen || !rewind_ctx.capture_busy || !rewind_ctx.free_stack || !rewind_ctx.queue) {
		LOG_error("Rewind: failed to allocate async capture buffers\n");
		Rewind_free();
		return 0;
	}
	for (int i = 0; i < rewind_ctx.pool_size; i++) {
		rewind_ctx.capture_pool[i] = calloc(1, state_size);
		if (!rewind_ctx.capture_pool[i]) {
			LOG_error("Rewind: failed to allocate capture slot %i\n", i);
			Rewind_free();
			return 0;
		}
		rewind_ctx.free_stack[i] = i;
	}
	rewind_ctx.queue_capacity = rewind_ctx.pool_size;
	rewind_ctx.free_count = rewind_ctx.pool_size;

	if (pthread_create(&rewind_ctx.worker, NULL, Rewind_worker_thread, NULL) != 0) {
		// fallback to synchronous path
		LOG_error("Rewind: failed to start worker thread, falling back to synchronous capture\n");
		rewind_ctx.pool_size = 0;
		rewind_ctx.queue_capacity = 0;
		rewind_ctx.free_count = 0;
	} else {
		rewind_ctx.worker_running = 1;
	}

	LOG_info("Rewind: enabled (%zu bytes buffer, cadence %i %s)\n", rewind_ctx.capacity,
		rewind_ctx.use_time_cadence ? rewind_ctx.interval_ms : rewind_ctx.granularity_frames,
		rewind_ctx.use_time_cadence ? "ms" : "frames");
	return 1;
}

static void* Rewind_worker_thread(void *arg) {
	(void)arg;

	while (1) {
		pthread_mutex_lock(&rewind_ctx.queue_mx);
		while (!rewind_ctx.worker_stop && rewind_ctx.queue_count == 0) {
			pthread_cond_wait(&rewind_ctx.queue_cv, &rewind_ctx.queue_mx);
		}
		if (rewind_ctx.worker_stop && rewind_ctx.queue_count == 0) {
			pthread_mutex_unlock(&rewind_ctx.queue_mx);
			break;
		}

		int slot = rewind_ctx.queue[rewind_ctx.queue_head];
		rewind_ctx.queue_head = (rewind_ctx.queue_head + 1) % rewind_ctx.queue_capacity;
		rewind_ctx.queue_count -= 1;
		unsigned int gen = rewind_ctx.capture_gen[slot];
		pthread_mutex_unlock(&rewind_ctx.queue_mx);

		if (gen != rewind_ctx.generation) {
			// stale snapshot, drop quietly
			pthread_mutex_lock(&rewind_ctx.queue_mx);
			rewind_ctx.capture_busy[slot] = 0;
			rewind_ctx.free_stack[rewind_ctx.free_count++] = slot;
			pthread_mutex_unlock(&rewind_ctx.queue_mx);
			continue;
		}

		size_t dest_len = rewind_ctx.scratch_size;
		int is_keyframe = 1;
		pthread_mutex_lock(&rewind_ctx.lock);
		if (gen == rewind_ctx.generation) {
			int res = Rewind_compress_state(rewind_ctx.capture_pool[slot], &dest_len, &is_keyframe);
			if (res == 0) {
				Rewind_write_entry_locked(rewind_ctx.scratch, dest_len, is_keyframe);
			} else {
				LOG_error("Rewind: compression failed (%i)\n", res);
			}
		}
		// If generation changed mid-flight, drop silently after releasing the slot
		pthread_mutex_unlock(&rewind_ctx.lock);

		pthread_mutex_lock(&rewind_ctx.queue_mx);
		rewind_ctx.capture_busy[slot] = 0;
		rewind_ctx.free_stack[rewind_ctx.free_count++] = slot;
		pthread_mutex_unlock(&rewind_ctx.queue_mx);
	}

	return NULL;
}

void Rewind_push(int force) {
	if (!rewind_ctx.enabled) return;
	if (!rewind_ctx.buffer || !rewind_ctx.state_buf) return;

	uint32_t now_ms = SDL_GetTicks();
	if (!force) {
		if (rewind_ctx.use_time_cadence) {
			if (rewind_ctx.last_push_ms && (int)(now_ms - rewind_ctx.last_push_ms) < rewind_ctx.interval_ms) return;
			rewind_ctx.last_push_ms = now_ms;
		} else {
			rewind_ctx.frame_counter += 1;
			if (rewind_ctx.frame_counter < rewind_ctx.granularity_frames) return;
			rewind_ctx.frame_counter = 0;
		}
	} else {
		rewind_ctx.frame_counter = 0;
		rewind_ctx.last_push_ms = now_ms;
	}

	if (!core.serialize || !core.serialize_size) return;

	if (rewind_ctx.worker_running && rewind_ctx.pool_size) {
		int slot = -1;
		while (1) {
			pthread_mutex_lock(&rewind_ctx.queue_mx);
			if (rewind_ctx.free_count && rewind_ctx.queue_count < rewind_ctx.queue_capacity) {
				slot = rewind_ctx.free_stack[--rewind_ctx.free_count];
				rewind_ctx.capture_busy[slot] = 1;
				pthread_mutex_unlock(&rewind_ctx.queue_mx);
				break;
			}
			// No free slot: synchronously process the oldest queued capture to preserve ordering
			if (rewind_ctx.queue_count > 0) {
				int queued_slot = rewind_ctx.queue[rewind_ctx.queue_head];
				unsigned int gen = rewind_ctx.capture_gen[queued_slot];
				rewind_ctx.queue_head = (rewind_ctx.queue_head + 1) % rewind_ctx.queue_capacity;
				rewind_ctx.queue_count -= 1;
				pthread_mutex_unlock(&rewind_ctx.queue_mx);

				size_t dest_len = rewind_ctx.scratch_size;
				int is_keyframe = 1;
				pthread_mutex_lock(&rewind_ctx.lock);
				if (gen == rewind_ctx.generation) {
					int res = Rewind_compress_state(rewind_ctx.capture_pool[queued_slot], &dest_len, &is_keyframe);
					if (res == 0) {
						Rewind_write_entry_locked(rewind_ctx.scratch, dest_len, is_keyframe);
					} else {
						LOG_error("Rewind: compression failed (%i)\n", res);
					}
				}
				pthread_mutex_unlock(&rewind_ctx.lock);

				pthread_mutex_lock(&rewind_ctx.queue_mx);
				rewind_ctx.capture_busy[queued_slot] = 0;
				rewind_ctx.free_stack[rewind_ctx.free_count++] = queued_slot;
				pthread_mutex_unlock(&rewind_ctx.queue_mx);
				// loop again to try to grab a free slot for the current frame
				continue;
			}
			pthread_mutex_unlock(&rewind_ctx.queue_mx);
			break;
		}

		if (slot < 0) {
			// worker is busy; fall back to synchronous capture so we don't miss cadence
			if (!core.serialize(rewind_ctx.state_buf, rewind_ctx.state_size)) {
				LOG_error("Rewind: serialize failed (sync fallback)\n");
				return;
			}

			size_t dest_len = rewind_ctx.scratch_size;
			int is_keyframe = 1;
			pthread_mutex_lock(&rewind_ctx.lock);
			int res = Rewind_compress_state(rewind_ctx.state_buf, &dest_len, &is_keyframe);
			if (res != 0) {
				pthread_mutex_unlock(&rewind_ctx.lock);
				LOG_error("Rewind: compression failed (sync fallback) (%i)\n", res);
				return;
			}

			Rewind_write_entry_locked(rewind_ctx.scratch, dest_len, is_keyframe);
			pthread_mutex_unlock(&rewind_ctx.lock);
			return;
		}

		uint8_t *buf = rewind_ctx.capture_pool[slot];
		if (!core.serialize(buf, rewind_ctx.state_size)) {
			LOG_error("Rewind: serialize failed\n");
			pthread_mutex_lock(&rewind_ctx.queue_mx);
			rewind_ctx.capture_busy[slot] = 0;
			rewind_ctx.free_stack[rewind_ctx.free_count++] = slot;
			pthread_mutex_unlock(&rewind_ctx.queue_mx);
			return;
		}

		rewind_ctx.capture_gen[slot] = rewind_ctx.generation;
		pthread_mutex_lock(&rewind_ctx.queue_mx);
		rewind_ctx.queue[rewind_ctx.queue_tail] = slot;
		rewind_ctx.queue_tail = (rewind_ctx.queue_tail + 1) % rewind_ctx.queue_capacity;
		rewind_ctx.queue_count += 1;
		pthread_cond_signal(&rewind_ctx.queue_cv);
		pthread_mutex_unlock(&rewind_ctx.queue_mx);

		return;
	}

	// synchronous fallback (thread not available)
	if (!core.serialize(rewind_ctx.state_buf, rewind_ctx.state_size)) {
		LOG_error("Rewind: serialize failed\n");
		return;
	}

	size_t dest_len = rewind_ctx.scratch_size;
	int is_keyframe = 1;
	pthread_mutex_lock(&rewind_ctx.lock);
	int res = Rewind_compress_state(rewind_ctx.state_buf, &dest_len, &is_keyframe);
	if (res != 0) {
		pthread_mutex_unlock(&rewind_ctx.lock);
		LOG_error("Rewind: compression failed (%i)\n", res);
		return;
	}

	Rewind_write_entry_locked(rewind_ctx.scratch, dest_len, is_keyframe);
	pthread_mutex_unlock(&rewind_ctx.lock);

}

int Rewind_step_back(void) {
	if (!rewind_ctx.enabled) return REWIND_STEP_EMPTY;
	uint32_t now_ms = SDL_GetTicks();
	if (rewind_ctx.playback_interval_ms > 0 && rewind_ctx.last_step_ms &&
		(int)(now_ms - rewind_ctx.last_step_ms) < rewind_ctx.playback_interval_ms) {
		// still rewinding, just waiting for cadence; don't run core, just re-render
		return REWIND_STEP_CADENCE;
	}

	// On first rewind step, we need to:
	// 1. Wait for any pending compression to finish (so entry indices are stable)
	// 2. Copy the last compressed state as our delta reference
	if (!rewinding && rewind_ctx.compress && rewind_ctx.prev_state_dec) {
		// Wait for worker to finish all pending compressions
		Rewind_wait_for_worker_idle();
		pthread_mutex_lock(&rewind_ctx.lock);
		if (rewind_ctx.has_prev_enc && rewind_ctx.prev_state_enc) {
			memcpy(rewind_ctx.prev_state_dec, rewind_ctx.prev_state_enc, rewind_ctx.state_size);
			rewind_ctx.has_prev_dec = 1;
		} else {
			rewind_ctx.has_prev_dec = 0;
		}
		pthread_mutex_unlock(&rewind_ctx.lock);
	}

	pthread_mutex_lock(&rewind_ctx.lock);
	RewindBufferState state = Rewind_buffer_state_locked();
	if (state == REWIND_BUF_EMPTY) {
		pthread_mutex_unlock(&rewind_ctx.lock);
		if (!rewind_warn_empty) {
			LOG_info("Rewind: no buffered states yet\n");
			rewind_warn_empty = 1;
		}
		return REWIND_STEP_EMPTY;
	}

	int idx = rewind_ctx.entry_head - 1;
	if (idx < 0) idx += rewind_ctx.entry_capacity;
	RewindEntry *e = &rewind_ctx.entries[idx];

	int decode_ok = 1;
	if (rewind_ctx.compress) {
		// Decompress into delta_buf first (it may contain XOR delta or full state)
		int res = LZ4_decompress_safe((const char*)rewind_ctx.buffer + e->offset,
			(char*)rewind_ctx.delta_buf, (int)e->size, (int)rewind_ctx.state_size);
		if (res < (int)rewind_ctx.state_size) {
			LOG_error("Rewind: decompress failed (res=%i, want=%zu, compressed=%zu, offset=%zu, idx=%d head=%d tail=%d count=%d buf_head=%zu buf_tail=%zu)\n",
				res, rewind_ctx.state_size, e->size, e->offset, idx, rewind_ctx.entry_head, rewind_ctx.entry_tail, rewind_ctx.entry_count, rewind_ctx.head, rewind_ctx.tail);
			decode_ok = 0;
		} else if (e->is_keyframe) {
			// This is a keyframe (full state), just copy it directly
			memcpy(rewind_ctx.state_buf, rewind_ctx.delta_buf, rewind_ctx.state_size);
			if (rewind_ctx.prev_state_dec) {
				memcpy(rewind_ctx.prev_state_dec, rewind_ctx.state_buf, rewind_ctx.state_size);
				rewind_ctx.has_prev_dec = 1;
			}
		} else if (rewind_ctx.has_prev_dec && rewind_ctx.prev_state_dec) {
			// Delta decompression: XOR the delta with prev_state_dec to recover the actual state
			// prev_state_dec holds the current state (state N), delta = state_N XOR state_(N-1)
			// So: state_(N-1) = delta XOR state_N = delta XOR prev_state_dec
			size_t state_size = rewind_ctx.state_size;
			uint8_t *result = rewind_ctx.state_buf;
			const uint8_t *delta = rewind_ctx.delta_buf;
			const uint8_t *prev = rewind_ctx.prev_state_dec;
			// Byte-by-byte XOR to avoid unaligned memory access issues
			for (size_t i = 0; i < state_size; i++) {
				result[i] = delta[i] ^ prev[i];
			}
			// Update prev_state_dec to the state we just recovered (for next rewind step)
			memcpy(rewind_ctx.prev_state_dec, result, state_size);
		} else {
			// Delta frame but no previous state - this shouldn't happen with proper keyframe tracking
			// Fall back to treating it as a full state (may produce incorrect results)
			LOG_warn("Rewind: delta frame without previous state, results may be incorrect\n");
			memcpy(rewind_ctx.state_buf, rewind_ctx.delta_buf, rewind_ctx.state_size);
			if (rewind_ctx.prev_state_dec) {
				memcpy(rewind_ctx.prev_state_dec, rewind_ctx.state_buf, rewind_ctx.state_size);
				rewind_ctx.has_prev_dec = 1;
			}
		}
	} else {
		if (e->size != rewind_ctx.state_size) {
			LOG_error("Rewind: raw snapshot size mismatch (got=%zu, want=%zu, offset=%zu)\n",
				e->size, rewind_ctx.state_size, e->offset);
			decode_ok = 0;
		} else {
			memcpy(rewind_ctx.state_buf, rewind_ctx.buffer + e->offset, rewind_ctx.state_size);
		}
	}
	if (!decode_ok) {
		// On decode failure, drop the corrupted newest entry instead of oldest
		rewind_ctx.entry_head = idx;
		rewind_ctx.entry_count -= 1;
		if (rewind_ctx.entry_count == 0) {
			rewind_ctx.head = rewind_ctx.tail = 0;
		}
		pthread_mutex_unlock(&rewind_ctx.lock);
		return REWIND_STEP_EMPTY;
	}

	if (!core.unserialize(rewind_ctx.state_buf, rewind_ctx.state_size)) {
		LOG_error("Rewind: unserialize failed\n");
		Rewind_drop_oldest_locked();
		pthread_mutex_unlock(&rewind_ctx.lock);
		return REWIND_STEP_EMPTY;
	}

	// pop newest
	rewind_ctx.entry_head = idx;
	rewind_ctx.entry_count -= 1;
	if (rewind_ctx.entry_count == 0) {
		rewind_ctx.head = rewind_ctx.tail = 0;
	}
	pthread_mutex_unlock(&rewind_ctx.lock);

	rewinding = 1;
	rewind_ctx.last_step_ms = now_ms;
	return REWIND_STEP_OK;
}

// Call this when rewind ends to sync the encode buffer with the last decoded state
// Also clears old entries that were compressed with a different delta chain
void Rewind_sync_encode_state(void) {
	if (!rewind_ctx.enabled || !rewind_ctx.compress) return;
	if (!rewinding) return; // Only sync if we were actually rewinding

	pthread_mutex_lock(&rewind_ctx.lock);

	// The decoder's prev_state_dec contains the state we rewound to.
	// Use it as the new reference for future compressions so the existing
	// rewind history remains valid and we can continue rewinding further back.
	if (rewind_ctx.has_prev_dec && rewind_ctx.prev_state_dec && rewind_ctx.prev_state_enc) {
		memcpy(rewind_ctx.prev_state_enc, rewind_ctx.prev_state_dec, rewind_ctx.state_size);
		rewind_ctx.has_prev_enc = 1;
	} else {
		rewind_ctx.has_prev_enc = 0;
	}

	pthread_mutex_unlock(&rewind_ctx.lock);
}

void Rewind_on_state_change(void) {
	Rewind_reset();
	Rewind_push(1);
	LOG_info("Rewind: state changed, buffer re-seeded\n");
}
