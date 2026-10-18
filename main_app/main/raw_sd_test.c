#include "raw_sd_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"

extern const uint8_t raw_crab_seed_bin_start[] asm("_binary_raw_crab_seed_bin_start");
extern const uint8_t raw_crab_seed_bin_end[] asm("_binary_raw_crab_seed_bin_end");

static const char *TAG = "RAW_SD";

#define RAW_ALIGN_SECTORS       2048u    /* 1 MiB at 512 bytes/sector */
#define RAW_GUARD_SECTORS       2048u
#define RAW_MIN_FREE_SECTORS    (256u * 1024u * 1024u / 512u)
#define RAW_CHUNK_SECTORS       128u     /* 64 KiB */
#define RAW_BENCH_BYTES         (32u * 1024u * 1024u)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t align_up_u32(uint32_t value, uint32_t align)
{
    return (value + align - 1u) / align * align;
}

static uint32_t fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static esp_err_t find_raw_area(uint32_t *raw_start, uint32_t *raw_count, char *out, size_t out_len)
{
    uint8_t *mbr = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!mbr) {
        snprintf(out, out_len, "Raw SD: no DMA buffer for MBR");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sdmmc_read_sectors(bsp_sdcard, mbr, 0, 1);
    if (err != ESP_OK) {
        snprintf(out, out_len, "Raw SD: MBR read failed: %s", esp_err_to_name(err));
        heap_caps_free(mbr);
        return err;
    }

    if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
        snprintf(out, out_len, "Raw SD: sector 0 has no MBR signature");
        heap_caps_free(mbr);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t max_end = 0;
    char part_text[160] = {0};
    for (int i = 0; i < 4; ++i) {
        const uint8_t *entry = mbr + 446 + i * 16;
        uint8_t type = entry[4];
        uint32_t start = le32(entry + 8);
        uint32_t count = le32(entry + 12);
        if (type == 0 || count == 0) {
            continue;
        }
        uint32_t end = start + count;
        if (end > max_end) {
            max_end = end;
        }
        size_t used = strlen(part_text);
        snprintf(part_text + used, sizeof(part_text) - used,
                 "p%d type=0x%02x start=%lu count=%lu; ",
                 i + 1, type, (unsigned long)start, (unsigned long)count);
    }
    heap_caps_free(mbr);

    if (max_end == 0) {
        snprintf(out, out_len, "Raw SD: no MBR partitions found");
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t capacity = (uint32_t)bsp_sdcard->csd.capacity;
    uint32_t start = align_up_u32(max_end + RAW_GUARD_SECTORS, RAW_ALIGN_SECTORS);
    if (start >= capacity) {
        snprintf(out, out_len, "Raw SD: no free raw area. %s card=%lu sectors",
                 part_text, (unsigned long)capacity);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t count = capacity - start;
    if (count < RAW_MIN_FREE_SECTORS) {
        snprintf(out, out_len,
                 "Raw SD: free area too small. %s raw_start=%lu raw=%lu MiB",
                 part_text, (unsigned long)start, (unsigned long)(count / 2048u));
        return ESP_ERR_INVALID_SIZE;
    }

    *raw_start = start;
    *raw_count = count;
    ESP_LOGI(TAG, "Detected raw area: %s raw_start=%lu raw=%lu MiB",
             part_text, (unsigned long)start, (unsigned long)(count / 2048u));
    return ESP_OK;
}

static esp_err_t write_seed(uint32_t start_sector, uint8_t *buf, size_t buf_bytes)
{
    const uint8_t *seed = raw_crab_seed_bin_start;
    size_t seed_len = (size_t)(raw_crab_seed_bin_end - raw_crab_seed_bin_start);
    size_t written = 0;
    uint32_t sector = start_sector;

    while (written < seed_len) {
        size_t todo = seed_len - written;
        if (todo > buf_bytes) todo = buf_bytes;
        size_t padded = (todo + 511u) & ~511u;
        memset(buf, 0, padded);
        memcpy(buf, seed + written, todo);
        esp_err_t err = sdmmc_write_sectors(bsp_sdcard, buf, sector, padded / 512u);
        if (err != ESP_OK) return err;
        written += todo;
        sector += padded / 512u;
    }
    return ESP_OK;
}

static esp_err_t verify_seed(uint32_t start_sector, uint8_t *buf, size_t buf_bytes, uint32_t *hash_out)
{
    const uint8_t *seed = raw_crab_seed_bin_start;
    size_t seed_len = (size_t)(raw_crab_seed_bin_end - raw_crab_seed_bin_start);
    size_t checked = 0;
    uint32_t sector = start_sector;
    uint32_t hash = 2166136261u;

    while (checked < seed_len) {
        size_t todo = seed_len - checked;
        if (todo > buf_bytes) todo = buf_bytes;
        size_t padded = (todo + 511u) & ~511u;
        esp_err_t err = sdmmc_read_sectors(bsp_sdcard, buf, sector, padded / 512u);
        if (err != ESP_OK) return err;
        if (memcmp(buf, seed + checked, todo) != 0) {
            return ESP_ERR_INVALID_CRC;
        }
        for (size_t i = 0; i < todo; ++i) {
            hash ^= buf[i];
            hash *= 16777619u;
        }
        checked += todo;
        sector += padded / 512u;
    }

    *hash_out = hash;
    return ESP_OK;
}

esp_err_t raw_sd_test_run(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    if (!bsp_sdcard) {
        snprintf(out, out_len, "Raw SD: SD card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    if (bsp_sdcard->csd.sector_size != 512) {
        snprintf(out, out_len, "Raw SD: unsupported sector size %d", bsp_sdcard->csd.sector_size);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t raw_start = 0;
    uint32_t raw_count = 0;
    esp_err_t err = find_raw_area(&raw_start, &raw_count, out, out_len);
    if (err != ESP_OK) return err;

    const size_t seed_len = (size_t)(raw_crab_seed_bin_end - raw_crab_seed_bin_start);
    const uint32_t seed_sectors = (uint32_t)((seed_len + 511u) / 512u);
    if (seed_sectors + RAW_GUARD_SECTORS >= raw_count) {
        snprintf(out, out_len, "Raw SD: seed does not fit in raw area");
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t chunk_bytes = RAW_CHUNK_SECTORS * 512u;
    uint8_t *buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) {
        snprintf(out, out_len, "Raw SD: no %u byte DMA buffer", (unsigned)chunk_bytes);
        return ESP_ERR_NO_MEM;
    }

    int64_t write_start_us = esp_timer_get_time();
    err = write_seed(raw_start, buf, chunk_bytes);
    int64_t write_us = esp_timer_get_time() - write_start_us;
    if (err != ESP_OK) {
        snprintf(out, out_len, "Raw SD: seed write failed at sector %lu: %s",
                 (unsigned long)raw_start, esp_err_to_name(err));
        heap_caps_free(buf);
        return err;
    }

    uint32_t read_hash = 0;
    int64_t verify_start_us = esp_timer_get_time();
    err = verify_seed(raw_start, buf, chunk_bytes, &read_hash);
    int64_t verify_us = esp_timer_get_time() - verify_start_us;
    if (err != ESP_OK) {
        snprintf(out, out_len, "Raw SD: verify failed at sector %lu: %s",
                 (unsigned long)raw_start, esp_err_to_name(err));
        heap_caps_free(buf);
        return err;
    }

    const uint32_t expected_hash = fnv1a32(raw_crab_seed_bin_start, seed_len);
    if (read_hash != expected_hash) {
        snprintf(out, out_len, "Raw SD: hash mismatch %08lx != %08lx",
                 (unsigned long)read_hash, (unsigned long)expected_hash);
        heap_caps_free(buf);
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t bench_sectors = RAW_BENCH_BYTES / 512u;
    if (bench_sectors > raw_count - RAW_GUARD_SECTORS) {
        bench_sectors = raw_count - RAW_GUARD_SECTORS;
    }
    bench_sectors -= bench_sectors % RAW_CHUNK_SECTORS;
    if (bench_sectors == 0) {
        bench_sectors = seed_sectors;
    }

    int64_t bench_start_us = esp_timer_get_time();
    for (uint32_t done = 0; done < bench_sectors; done += RAW_CHUNK_SECTORS) {
        uint32_t sectors = RAW_CHUNK_SECTORS;
        if (sectors > bench_sectors - done) sectors = bench_sectors - done;
        err = sdmmc_read_sectors(bsp_sdcard, buf, raw_start + done, sectors);
        if (err != ESP_OK) {
            snprintf(out, out_len, "Raw SD: bench read failed: %s", esp_err_to_name(err));
            heap_caps_free(buf);
            return err;
        }
    }
    int64_t bench_us = esp_timer_get_time() - bench_start_us;

    heap_caps_free(buf);

    float write_mbps = (float)seed_len * 1000000.0f / (float)write_us / (1024.0f * 1024.0f);
    float verify_mbps = (float)seed_len * 1000000.0f / (float)verify_us / (1024.0f * 1024.0f);
    float bench_mbps = (float)(bench_sectors * 512u) * 1000000.0f / (float)bench_us / (1024.0f * 1024.0f);

    snprintf(out, out_len,
             "Raw SD OK\n"
             "raw_start=%lu raw=%lu MiB\n"
             "seed=%u bytes sectors=%lu hash=%08lx\n"
             "write=%.2f MB/s verify=%.2f MB/s\n"
             "raw read %.2f MB/s over %lu MiB",
             (unsigned long)raw_start,
             (unsigned long)(raw_count / 2048u),
             (unsigned)seed_len,
             (unsigned long)seed_sectors,
             (unsigned long)read_hash,
             write_mbps,
             verify_mbps,
             bench_mbps,
             (unsigned long)(bench_sectors / 2048u));

    ESP_LOGI(TAG, "%s", out);
    return ESP_OK;
}
