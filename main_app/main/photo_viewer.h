#pragma once

#include "lvgl.h"

/**
 * @brief Show photo viewer with a random photo from /sdcard/photos.
 */
void photo_viewer_show(void);

/**
 * @brief Open a specific photo in the viewer.
 * Scans the parent directory for other JPEGs for navigation.
 * @param filepath Full path to the JPEG file.
 */
void photo_viewer_open(const char *filepath);
