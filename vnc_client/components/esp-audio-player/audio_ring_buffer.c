/**
 * @file audio_ring_buffer.c
 * @brief Producer-consumer ring buffer for audio playback pipeline.
 *
 * A dedicated reader task pre-fills a 128KB PSRAM ring buffer from a FILE*,
 * decoupling SD card read latency from the real-time audio decode path.
 */

#include "audio_ring_buffer.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "ring_buf";

/* Event group bits */
#define BIT_DATA_AVAILABLE  (BIT0)
#define BIT_SPACE_AVAILABLE (BIT1)
#define BIT_ABORT           (BIT2)
#define BIT_READER_DONE     (BIT3)

/* Reader task reads in 4KB chunks — balances SD card efficiency and abort responsiveness */
#define READ_CHUNK_SIZE     (4 * 1024)

/* Reader task configuration */
#define READER_TASK_STACK   (4 * 1024)
#define READER_TASK_PRIO    (4)
#define READER_TASK_CORE    (1)

/* Pre-fill percentage before start() returns */
#define PREFILL_PERCENT     (25)

/* Timeout waiting for reader task to exit on stop() */
#define READER_STOP_TIMEOUT_MS (2000)

struct audio_ring_buffer {
    uint8_t *buffer;
    size_t head;
    size_t tail;
    size_t capacity;

    EventGroupHandle_t events;
    portMUX_TYPE spinlock;

    volatile bool abort_flag;
    volatile bool eof_flag;

    TaskHandle_t reader_task;
    FILE *fp;

    /* Position tracking for seek/time display */
    size_t file_size;           /* Total file size in bytes */
    size_t bytes_read_total;    /* Total bytes read from file by reader task */
    size_t audio_data_offset;   /* Offset where audio data starts (after headers) */
};

/**
 * Returns the number of bytes available for reading (head - tail, with wrap).
 * Must be called with spinlock held or from a context where head/tail are stable.
 */
static inline size_t available_bytes(audio_ring_buffer_t rb)
{
    size_t h = rb->head;
    size_t t = rb->tail;
    if (h >= t) {
        return h - t;
    }
    return rb->capacity - t + h;
}

/**
 * Returns the number of bytes of free space in the buffer.
 * We reserve 1 byte to distinguish full from empty.
 */
static inline size_t free_space(audio_ring_buffer_t rb)
{
    return rb->capacity - 1 - available_bytes(rb);
}

/**
 * Reader task: reads from FILE* into the ring buffer in 4KB chunks.
 * Signals BIT_DATA_AVAILABLE after each write. Exits on abort or EOF.
 */
static void reader_task_fn(void *arg)
{
    audio_ring_buffer_t rb = (audio_ring_buffer_t)arg;

    ESP_LOGI(TAG, "reader task started");

    while (!rb->abort_flag) {
        /* Check how much space is available */
        portENTER_CRITICAL(&rb->spinlock);
        size_t space = free_space(rb);
        portEXIT_CRITICAL(&rb->spinlock);

        if (space < READ_CHUNK_SIZE) {
            /* Signal that data is available (buffer is nearly full) */
            xEventGroupSetBits(rb->events, BIT_DATA_AVAILABLE);

            /* Wait for space or abort */
            EventBits_t bits = xEventGroupWaitBits(
                rb->events,
                BIT_SPACE_AVAILABLE | BIT_ABORT,
                pdTRUE,   /* clear on exit */
                pdFALSE,  /* wait for any bit */
                pdMS_TO_TICKS(100));

            if (bits & BIT_ABORT) {
                break;
            }
            continue;
        }

        /* Determine how much to read this iteration (up to READ_CHUNK_SIZE) */
        size_t to_read = READ_CHUNK_SIZE;
        if (to_read > space) {
            to_read = space;
        }

        /* Calculate contiguous space from head to end of buffer */
        size_t head_pos = rb->head;
        size_t contig = rb->capacity - head_pos;
        if (to_read > contig) {
            to_read = contig;
        }

        /* Read from file */
        size_t bytes_read = fread(rb->buffer + head_pos, 1, to_read, rb->fp);

        if (bytes_read == 0) {
            /* EOF or error */
            rb->eof_flag = true;
            ESP_LOGI(TAG, "reader reached EOF");
            xEventGroupSetBits(rb->events, BIT_DATA_AVAILABLE);
            break;
        }

        /* Advance head */
        portENTER_CRITICAL(&rb->spinlock);
        rb->head = (head_pos + bytes_read) % rb->capacity;
        rb->bytes_read_total += bytes_read;
        portEXIT_CRITICAL(&rb->spinlock);

        /* Signal data available to consumer */
        xEventGroupSetBits(rb->events, BIT_DATA_AVAILABLE);

        /* Yield briefly to allow other SDMMC slot users (ESP-Hosted SDIO)
         * to acquire the shared SDMMC host mutex. Without this, continuous
         * fread() calls starve Slot 1 and cause WiFi SDIO bus lockups. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "reader task exiting");
    xEventGroupSetBits(rb->events, BIT_READER_DONE);
    rb->reader_task = NULL;
    vTaskDelete(NULL);
}

audio_ring_buffer_t audio_ring_buffer_create(size_t capacity)
{
    audio_ring_buffer_t rb = (audio_ring_buffer_t)heap_caps_calloc(1, sizeof(struct audio_ring_buffer), MALLOC_CAP_DEFAULT);
    if (!rb) {
        ESP_LOGE(TAG, "failed to allocate ring buffer struct");
        return NULL;
    }

    rb->buffer = (uint8_t *)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!rb->buffer) {
        ESP_LOGE(TAG, "failed to allocate %u bytes from PSRAM", (unsigned)capacity);
        free(rb);
        return NULL;
    }

    rb->events = xEventGroupCreate();
    if (!rb->events) {
        ESP_LOGE(TAG, "failed to create event group");
        free(rb->buffer);
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->abort_flag = false;
    rb->eof_flag = false;
    rb->reader_task = NULL;
    rb->fp = NULL;
    portMUX_INITIALIZE(&rb->spinlock);

    ESP_LOGI(TAG, "created ring buffer, capacity=%u bytes", (unsigned)capacity);
    return rb;
}

void audio_ring_buffer_destroy(audio_ring_buffer_t *rb)
{
    if (!rb || !*rb) {
        return;
    }

    audio_ring_buffer_t r = *rb;

    /* Ensure reader task is stopped */
    if (r->reader_task) {
        audio_ring_buffer_stop(r);
    }

    if (r->events) {
        vEventGroupDelete(r->events);
    }

    if (r->buffer) {
        free(r->buffer);
    }

    free(r);
    *rb = NULL;

    ESP_LOGI(TAG, "ring buffer destroyed");
}

void audio_ring_buffer_start(audio_ring_buffer_t rb, FILE *fp)
{
    if (!rb || !fp) {
        return;
    }

    /* Reset state */
    portENTER_CRITICAL(&rb->spinlock);
    rb->head = 0;
    rb->tail = 0;
    portEXIT_CRITICAL(&rb->spinlock);

    rb->abort_flag = false;
    rb->eof_flag = false;
    rb->fp = fp;
    rb->bytes_read_total = 0;

    /* Get file size for position tracking */
    long cur = ftell(fp);
    fseek(fp, 0, SEEK_END);
    rb->file_size = (size_t)ftell(fp);
    fseek(fp, cur, SEEK_SET);
    rb->audio_data_offset = (size_t)cur;  /* Where audio data starts (after headers) */

    /* Clear all event bits */
    xEventGroupClearBits(rb->events, BIT_DATA_AVAILABLE | BIT_SPACE_AVAILABLE | BIT_ABORT | BIT_READER_DONE);

    /* Create reader task on Core 1 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        reader_task_fn,
        "rb_reader",
        READER_TASK_STACK,
        rb,
        READER_TASK_PRIO,
        &rb->reader_task,
        READER_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create reader task");
        return;
    }

    /* Block until buffer is 25% full or EOF */
    size_t prefill_target = rb->capacity * PREFILL_PERCENT / 100;
    ESP_LOGI(TAG, "pre-filling buffer, target=%u bytes", (unsigned)prefill_target);

    while (!rb->eof_flag && !rb->abort_flag) {
        portENTER_CRITICAL(&rb->spinlock);
        size_t avail = available_bytes(rb);
        portEXIT_CRITICAL(&rb->spinlock);

        if (avail >= prefill_target) {
            break;
        }

        /* Wait for data with a short timeout to re-check conditions */
        xEventGroupWaitBits(rb->events, BIT_DATA_AVAILABLE, pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "pre-fill complete, available=%u bytes", (unsigned)available_bytes(rb));
}

void audio_ring_buffer_stop(audio_ring_buffer_t rb)
{
    if (!rb) {
        return;
    }

    if (!rb->reader_task) {
        /* Reader already stopped, just reset state */
        portENTER_CRITICAL(&rb->spinlock);
        rb->head = 0;
        rb->tail = 0;
        portEXIT_CRITICAL(&rb->spinlock);
        rb->eof_flag = false;
        rb->abort_flag = false;
        return;
    }

    /* Signal abort */
    rb->abort_flag = true;
    xEventGroupSetBits(rb->events, BIT_ABORT | BIT_SPACE_AVAILABLE);

    /* Wait for reader task to exit */
    EventBits_t bits = xEventGroupWaitBits(
        rb->events,
        BIT_READER_DONE,
        pdTRUE,   /* clear on exit */
        pdFALSE,
        pdMS_TO_TICKS(READER_STOP_TIMEOUT_MS));

    if (!(bits & BIT_READER_DONE)) {
        ESP_LOGW(TAG, "reader task did not exit within timeout");
    }

    /* Reset state */
    portENTER_CRITICAL(&rb->spinlock);
    rb->head = 0;
    rb->tail = 0;
    portEXIT_CRITICAL(&rb->spinlock);

    rb->eof_flag = false;
    rb->abort_flag = false;
    rb->fp = NULL;
}

size_t audio_ring_buffer_read(audio_ring_buffer_t rb, uint8_t *dest, size_t len, TickType_t timeout)
{
    if (!rb || !dest || len == 0) {
        return 0;
    }

    /* Check if data is available */
    portENTER_CRITICAL(&rb->spinlock);
    size_t avail = available_bytes(rb);
    portEXIT_CRITICAL(&rb->spinlock);

    if (avail == 0) {
        /* If EOF and empty, return 0 immediately */
        if (rb->eof_flag) {
            return 0;
        }

        /* Block waiting for data */
        EventBits_t bits = xEventGroupWaitBits(
            rb->events,
            BIT_DATA_AVAILABLE | BIT_ABORT,
            pdTRUE,
            pdFALSE,
            timeout);

        if (bits & BIT_ABORT) {
            return 0;
        }

        /* Re-check available bytes after waking */
        portENTER_CRITICAL(&rb->spinlock);
        avail = available_bytes(rb);
        portEXIT_CRITICAL(&rb->spinlock);

        if (avail == 0) {
            /* Still empty — either timeout or EOF */
            return 0;
        }
    }

    /* Copy data from tail to dest */
    size_t to_copy = (len < avail) ? len : avail;
    size_t tail_pos = rb->tail;
    size_t contig = rb->capacity - tail_pos;

    if (to_copy <= contig) {
        memcpy(dest, rb->buffer + tail_pos, to_copy);
    } else {
        /* Wrap around: copy in two parts */
        memcpy(dest, rb->buffer + tail_pos, contig);
        memcpy(dest + contig, rb->buffer, to_copy - contig);
    }

    /* Advance tail */
    portENTER_CRITICAL(&rb->spinlock);
    rb->tail = (tail_pos + to_copy) % rb->capacity;
    portEXIT_CRITICAL(&rb->spinlock);

    /* Signal space available to reader */
    xEventGroupSetBits(rb->events, BIT_SPACE_AVAILABLE);

    return to_copy;
}

bool audio_ring_buffer_eof(audio_ring_buffer_t rb)
{
    if (!rb) {
        return true;
    }

    portENTER_CRITICAL(&rb->spinlock);
    size_t avail = available_bytes(rb);
    portEXIT_CRITICAL(&rb->spinlock);

    return (rb->eof_flag && avail == 0);
}

size_t audio_ring_buffer_get_file_size(audio_ring_buffer_t rb)
{
    return rb ? rb->file_size : 0;
}

size_t audio_ring_buffer_get_audio_offset(audio_ring_buffer_t rb)
{
    return rb ? rb->audio_data_offset : 0;
}

size_t audio_ring_buffer_get_bytes_read(audio_ring_buffer_t rb)
{
    if (!rb) return 0;
    portENTER_CRITICAL(&rb->spinlock);
    size_t val = rb->bytes_read_total;
    portEXIT_CRITICAL(&rb->spinlock);
    return val;
}

void audio_ring_buffer_seek(audio_ring_buffer_t rb, size_t file_offset)
{
    /* Not used — seek is handled at the audio_player level by stopping
       and restarting with a pre-seeked FILE*. Kept as stub. */
    (void)rb;
    (void)file_offset;
}
