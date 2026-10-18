#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t raw_sd_test_run(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
