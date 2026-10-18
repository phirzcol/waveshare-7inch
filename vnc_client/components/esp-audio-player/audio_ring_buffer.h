#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to an audio ring buffer instance */
typedef struct audio_ring_buffer *audio_ring_buffer_t;

/**
 * Create a ring buffer with the specified capacity.
 *
 * Allocates the buffer from PSRAM and initializes internal synchronization primitives.
 *
 * @param capacity Size of the ring buffer in bytes (e.g. 128 * 1024)
 * @return Handle to the ring buffer, or NULL on allocation failure
 */
audio_ring_buffer_t audio_ring_buffer_create(size_t capacity);

/**
 * Destroy a ring buffer and free all associated resources.
 *
 * Ensures the reader task is stopped before freeing memory.
 *
 * @param rb Pointer to the ring buffer handle (set to NULL after destroy)
 */
void audio_ring_buffer_destroy(audio_ring_buffer_t *rb);

/**
 * Start the ring buffer reader task for the given file.
 *
 * Resets internal state, launches the reader task on Core 1, and blocks
 * until the buffer is at least 25% full or EOF is reached.
 *
 * @param rb Ring buffer handle
 * @param fp FILE pointer to read from (must remain valid until stop is called)
 */
void audio_ring_buffer_start(audio_ring_buffer_t rb, FILE *fp);

/**
 * Stop the ring buffer reader task and flush the buffer.
 *
 * Signals the reader task to abort, waits for it to exit, and resets state.
 *
 * @param rb Ring buffer handle
 */
void audio_ring_buffer_stop(audio_ring_buffer_t rb);

/**
 * Read data from the ring buffer (consumer side).
 *
 * Copies up to `len` bytes from the ring buffer into `dest`. Blocks if the
 * buffer is empty until data becomes available or the timeout expires.
 * Returns 0 if EOF has been reached and the buffer is empty.
 *
 * @param rb      Ring buffer handle
 * @param dest    Destination buffer to copy data into
 * @param len     Maximum number of bytes to read
 * @param timeout Maximum time to wait for data (FreeRTOS ticks)
 * @return Number of bytes actually read, or 0 on EOF/timeout
 */
size_t audio_ring_buffer_read(audio_ring_buffer_t rb, uint8_t *dest, size_t len, TickType_t timeout);

/**
 * Check if the ring buffer has reached end-of-file and is fully drained.
 *
 * @param rb Ring buffer handle
 * @return true if the reader hit EOF and no data remains in the buffer
 */
bool audio_ring_buffer_eof(audio_ring_buffer_t rb);

/**
 * Get the total file size of the currently loaded file.
 */
size_t audio_ring_buffer_get_file_size(audio_ring_buffer_t rb);

/**
 * Get the byte offset where audio data starts (after headers/ID3 tags).
 */
size_t audio_ring_buffer_get_audio_offset(audio_ring_buffer_t rb);

/**
 * Get total bytes read from the file so far (audio data only).
 */
size_t audio_ring_buffer_get_bytes_read(audio_ring_buffer_t rb);

/**
 * Seek to a specific file offset. Stops the reader, flushes the buffer,
 * seeks the file, and restarts the reader with pre-fill.
 *
 * @param rb Ring buffer handle
 * @param file_offset Absolute byte offset in the file to seek to
 */
void audio_ring_buffer_seek(audio_ring_buffer_t rb, size_t file_offset);

#ifdef __cplusplus
}
#endif
