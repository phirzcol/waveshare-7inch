#pragma once

#include "lvgl.h"

/**
 * @brief Show the text editor with an empty untitled document.
 */
void text_editor_show(void);

/**
 * @brief Show the text editor and open a specific file.
 * @param filepath Full path to file on SD card.
 */
void text_editor_open(const char *filepath);
