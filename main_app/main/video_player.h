#pragma once

/**
 * @brief Show the video player using /sdcard/video as the default library.
 */
void video_player_show(void);

/**
 * @brief Open a specific MJPEG stream file in the video player.
 * @param filepath Full path to an .mjpg or .mjpeg file.
 */
void video_player_open(const char *filepath);

/**
 * @brief Stop any active video playback task.
 */
void video_player_stop(void);
