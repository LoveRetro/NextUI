/**
 * chd_reader.h - CHD file reader for rcheevos hashing
 * 
 * Provides CD reader callbacks for rcheevos to hash CHD disc images.
 * This allows RetroAchievements to work with PlayStation (and other CD-based)
 * games stored in CHD format.
 */

#ifndef CHD_READER_H
#define CHD_READER_H

#include <stddef.h>
#include <stdint.h>

/**
 * Open a track from a CHD file.
 * 
 * @param path Path to the CHD file
 * @param track Track number (1-based) or RC_HASH_CDTRACK_* special value
 * @return Track handle, or NULL if not a CHD file or on error
 */
void* chd_open_track(const char* path, uint32_t track);

/**
 * Open a track from a CHD file (iterator variant).
 * 
 * @param path Path to the CHD file
 * @param track Track number (1-based) or RC_HASH_CDTRACK_* special value
 * @param iterator The hash iterator (unused, for API compatibility)
 * @return Track handle, or NULL if not a CHD file or on error
 */
void* chd_open_track_iterator(const char* path, uint32_t track, const void* iterator);

/**
 * Read a sector from an open CHD track.
 * 
 * @param track_handle Handle returned by chd_open_track
 * @param sector Sector number relative to track start
 * @param buffer Buffer to read data into
 * @param requested_bytes Number of bytes to read
 * @return Number of bytes actually read
 */
size_t chd_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes);

/**
 * Close a CHD track handle.
 * 
 * @param track_handle Handle returned by chd_open_track
 */
void chd_close_track(void* track_handle);

/**
 * Get the first sector number of a track.
 * 
 * @param track_handle Handle returned by chd_open_track
 * @return First sector number (always 0 since we handle offset internally)
 */
uint32_t chd_first_track_sector(void* track_handle);

/**
 * Check if a file path points to a CHD file.
 * 
 * @param path File path to check
 * @return Non-zero if the file has a .chd extension
 */
int chd_reader_is_chd(const char* path);

#endif /* CHD_READER_H */
