/**
 * chd_reader.c - CHD file reader for rcheevos hashing
 * 
 * Provides CD reader callbacks for rcheevos to hash CHD disc images.
 */

#include "chd_reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

/*****************************************************************************
 * Track info structure
 *****************************************************************************/

typedef struct {
    int type;           // CD_TRACK_* type
    int frames;         // Total frames in track
    int pregap_frames;  // Pregap frames
    int postgap_frames; // Postgap frames
    int start_frame;    // Starting frame (cumulative)
} chd_track_info_t;

/*****************************************************************************
 * CHD track handle
 *****************************************************************************/

typedef struct {
    chd_file* chd;
    uint32_t track_num;
    
    // Track info
    chd_track_info_t tracks[CD_MAX_TRACKS];
    int num_tracks;
    
    // Current track info
    int track_start_frame;    // First frame of this track in CHD
    int track_frames;         // Number of frames in track
    int track_type;           // CD_TRACK_* type
    int track_pregap;         // Pregap frames for this track
    
    // Sector format info (determined from track type)
    int sector_header_size;   // Bytes to skip to reach raw data (0, 16, or 24)
    int raw_data_size;        // Size of raw data (2048 or 2352 for audio)
    
    // Hunk info
    uint32_t hunk_bytes;
    uint32_t frame_size;      // Bytes per frame (typically 2448 or 2352)
    int frames_per_hunk;
    
    // Sector buffer
    uint8_t* hunk_buffer;
    uint32_t cached_hunk;
} chd_track_handle_t;

/*****************************************************************************
 * Helper: Parse track type string
 *****************************************************************************/

static int parse_track_type(const char* type_str) {
    if (strcmp(type_str, "MODE1") == 0)
        return CD_TRACK_MODE1;
    if (strcmp(type_str, "MODE1_RAW") == 0 || strcmp(type_str, "MODE1/2352") == 0)
        return CD_TRACK_MODE1_RAW;
    if (strcmp(type_str, "MODE2") == 0)
        return CD_TRACK_MODE2;
    if (strcmp(type_str, "MODE2_FORM1") == 0)
        return CD_TRACK_MODE2_FORM1;
    if (strcmp(type_str, "MODE2_FORM2") == 0)
        return CD_TRACK_MODE2_FORM2;
    if (strcmp(type_str, "MODE2_FORM_MIX") == 0)
        return CD_TRACK_MODE2_FORM_MIX;
    if (strcmp(type_str, "MODE2_RAW") == 0 || strcmp(type_str, "MODE2/2352") == 0)
        return CD_TRACK_MODE2_RAW;
    if (strcmp(type_str, "AUDIO") == 0)
        return CD_TRACK_AUDIO;
    
    return CD_TRACK_MODE1; // Default
}

/*****************************************************************************
 * Helper: Parse CHD track metadata
 *****************************************************************************/

static int parse_chd_tracks(chd_file* chd, chd_track_info_t* tracks, int* num_tracks) {
    char metadata[256];
    uint32_t metadata_size;
    int track_idx = 0;
    int cumulative_frames = 0;
    
    // Try CDROM_TRACK_METADATA2 first (newer format with pregap info)
    for (int i = 0; i < CD_MAX_TRACKS; i++) {
        chd_error err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, i, 
                                         metadata, sizeof(metadata), 
                                         &metadata_size, NULL, NULL);
        if (err != CHDERR_NONE) {
            // Try older format
            err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, i,
                                   metadata, sizeof(metadata),
                                   &metadata_size, NULL, NULL);
            if (err != CHDERR_NONE) {
                // Try GD-ROM format
                err = chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, i,
                                       metadata, sizeof(metadata),
                                       &metadata_size, NULL, NULL);
                if (err != CHDERR_NONE)
                    break;
            }
        }
        
        metadata[metadata_size] = '\0';
        
        // Parse the metadata string
        int track_num, frames, pregap = 0, postgap = 0;
        char type_str[32] = {0};
        char subtype_str[32] = {0};
        char pgtype_str[32] = {0};
        char pgsub_str[32] = {0};
        
        // Full format 2: "TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
        int parsed = sscanf(metadata, "TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d",
                           &track_num, type_str, subtype_str, &frames, &pregap, pgtype_str, pgsub_str, &postgap);
        
        if (parsed < 4) {
            // Try format 1 (no pregap info)
            parsed = sscanf(metadata, "TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d",
                           &track_num, type_str, subtype_str, &frames);
            pregap = 0;
        }
        
        if (parsed >= 4) {
            tracks[track_idx].type = parse_track_type(type_str);
            tracks[track_idx].frames = frames;
            tracks[track_idx].pregap_frames = pregap;
            tracks[track_idx].postgap_frames = postgap;
            
            // CHD format: 'frames' is the actual data frames, NOT including pregap.
            // The cumulative frame_offset points to the first frame of this track's allocation.
            tracks[track_idx].start_frame = cumulative_frames;
            
            // Calculate padding for 4-frame alignment (per CHD spec)
            int padding = ((frames + 3) & ~3) - frames;
            cumulative_frames += frames + padding;
            track_idx++;
        }
    }
    
    *num_tracks = track_idx;
    return track_idx > 0 ? 0 : -1;
}

/*****************************************************************************
 * Helper: Check if track is data track
 *****************************************************************************/

static int is_data_track(int type) {
    return type != CD_TRACK_AUDIO;
}

/*****************************************************************************
 * Helper: Determine sector header size from track type
 * 
 * Based on rcheevos cdreader.c logic:
 * - MODE1/2352 (MODE1_RAW): 16 bytes (12 sync + 4 header)
 * - MODE2/2352 (MODE2_RAW): 24 bytes (12 sync + 4 header + 8 subheader)
 * - MODE1/2048, MODE2/2048: 0 bytes (cooked)
 * - AUDIO: 0 bytes (raw audio)
 *****************************************************************************/

static void get_sector_format(int track_type, int frame_size, int* header_size, int* data_size) {
    // Default: cooked 2048-byte sectors
    *data_size = 2048;
    *header_size = 0;
    
    switch (track_type) {
        case CD_TRACK_MODE1_RAW:
            // MODE1/2352: sync(12) + header(4) + data(2048) + ecc(288)
            if (frame_size >= 2352) {
                *header_size = 16;
                *data_size = 2048;
            }
            break;
            
        case CD_TRACK_MODE2_RAW:
        case CD_TRACK_MODE2_FORM_MIX:
            // MODE2/2352: sync(12) + header(4) + subheader(8) + data(2048) + ecc(280)
            if (frame_size >= 2352) {
                *header_size = 24;
                *data_size = 2048;
            }
            break;
            
        case CD_TRACK_MODE2_FORM1:
        case CD_TRACK_MODE2_FORM2:
            // MODE2 without sync: subheader(8) + data
            if (frame_size == 2336) {
                *header_size = 8;
                *data_size = 2048;
            }
            break;
            
        case CD_TRACK_MODE1:
        case CD_TRACK_MODE2:
            // Cooked sectors, no header
            *header_size = 0;
            *data_size = 2048;
            break;
            
        case CD_TRACK_AUDIO:
            // Audio tracks have no header, all 2352 bytes are data
            *header_size = 0;
            *data_size = 2352;
            break;
            
        default:
            // Unknown type, try to determine from frame size
            if (frame_size >= 2352) {
                // Assume raw with MODE1 header
                *header_size = 16;
                *data_size = 2048;
            }
            break;
    }
}

/*****************************************************************************
 * Helper: Find requested track
 *****************************************************************************/

static int find_track(chd_track_handle_t* handle, uint32_t track_request) {
    if (handle->num_tracks == 0)
        return -1;
    
    // Handle special track requests
    if (track_request == (uint32_t)-1) {
        // RC_HASH_CDTRACK_FIRST_DATA - first data track
        for (int i = 0; i < handle->num_tracks; i++) {
            if (is_data_track(handle->tracks[i].type))
                return i;
        }
        return -1;
    }
    else if (track_request == (uint32_t)-2) {
        // RC_HASH_CDTRACK_LAST - last track
        return handle->num_tracks - 1;
    }
    else if (track_request == (uint32_t)-3) {
        // RC_HASH_CDTRACK_LARGEST - largest track
        int largest_idx = 0;
        int largest_frames = 0;
        for (int i = 0; i < handle->num_tracks; i++) {
            if (handle->tracks[i].frames > largest_frames) {
                largest_frames = handle->tracks[i].frames;
                largest_idx = i;
            }
        }
        return largest_idx;
    }
    else if (track_request > 0 && track_request <= (uint32_t)handle->num_tracks) {
        // Specific track number (1-based)
        return track_request - 1;
    }
    
    return -1;
}

/*****************************************************************************
 * CD Reader callbacks
 *****************************************************************************/

void* chd_open_track(const char* path, uint32_t track) {
    return chd_open_track_iterator(path, track, NULL);
}

void* chd_open_track_iterator(const char* path, uint32_t track, const void* iterator) {
    (void)iterator; // Unused
    
    // Check if this is a CHD file
    const char* ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".chd") != 0) {
        return NULL; // Not a CHD file, let default reader handle it
    }
    
    chd_file* chd = NULL;
    chd_error err = chd_open(path, CHD_OPEN_READ, NULL, &chd);
    if (err != CHDERR_NONE) {
        return NULL;
    }
    
    // Allocate handle
    chd_track_handle_t* handle = (chd_track_handle_t*)calloc(1, sizeof(chd_track_handle_t));
    if (!handle) {
        chd_close(chd);
        return NULL;
    }
    
    handle->chd = chd;
    handle->track_num = track;
    handle->cached_hunk = (uint32_t)-1;
    
    // Get hunk info
    const chd_header* header = chd_get_header(chd);
    handle->hunk_bytes = header->hunkbytes;
    
    // CD frames are typically 2448 bytes (2352 sector + 96 subcode) or 2352 bytes
    // Check unit bytes if available, otherwise assume CD_FRAME_SIZE
    handle->frame_size = header->unitbytes ? header->unitbytes : CD_FRAME_SIZE;
    handle->frames_per_hunk = handle->hunk_bytes / handle->frame_size;
    
    // Allocate hunk buffer
    handle->hunk_buffer = (uint8_t*)malloc(handle->hunk_bytes);
    if (!handle->hunk_buffer) {
        chd_close(chd);
        free(handle);
        return NULL;
    }
    
    // Parse track metadata
    if (parse_chd_tracks(chd, handle->tracks, &handle->num_tracks) != 0) {
        free(handle->hunk_buffer);
        chd_close(chd);
        free(handle);
        return NULL;
    }
    
    // Find requested track
    int track_idx = find_track(handle, track);
    if (track_idx < 0) {
        free(handle->hunk_buffer);
        chd_close(chd);
        free(handle);
        return NULL;
    }
    
    handle->track_num = track_idx + 1; // Convert to 1-based
    handle->track_start_frame = handle->tracks[track_idx].start_frame;
    handle->track_frames = handle->tracks[track_idx].frames;
    handle->track_type = handle->tracks[track_idx].type;
    handle->track_pregap = handle->tracks[track_idx].pregap_frames;
    
    // Determine sector format based on track type
    get_sector_format(handle->track_type, handle->frame_size, 
                      &handle->sector_header_size, &handle->raw_data_size);
    
    return handle;
}

size_t chd_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes) {
    chd_track_handle_t* handle = (chd_track_handle_t*)track_handle;
    if (!handle || !handle->chd)
        return 0;
    
    // Convert relative sector number to CHD frame number
    // rcheevos calls: read_sector(first_track_sector() + offset)
    // Since first_track_sector() returns 0, sector IS the relative offset
    // We add track_start_frame to get the CHD frame number
    //
    // IMPORTANT: CHD allocates frames sequentially including pregap frames.
    // Even if PGTYPE='V' (virtual/silence), the frames are still allocated.
    // The FRAMES metadata field is the actual data frames AFTER pregap.
    // So we must always skip over pregap frames to reach the actual data.
    uint32_t frame = handle->track_start_frame + sector;
    
    // Always skip pregap for data tracks - the pregap frames are allocated
    // in the CHD regardless of whether they contain real data or silence.
    if (is_data_track(handle->track_type) && handle->track_pregap > 0) {
        frame += handle->track_pregap;
    }
    
    // Calculate which hunk contains this frame
    uint32_t hunk_num = frame / handle->frames_per_hunk;
    uint32_t frame_in_hunk = frame % handle->frames_per_hunk;
    
    // Read hunk if not cached
    if (hunk_num != handle->cached_hunk) {
        chd_error err = chd_read(handle->chd, hunk_num, handle->hunk_buffer);
        if (err != CHDERR_NONE) {
            return 0;
        }
        handle->cached_hunk = hunk_num;
    }
    
    // Calculate offset into hunk
    uint32_t offset = frame_in_hunk * handle->frame_size;
    uint8_t* src = handle->hunk_buffer + offset;
    
    // Use pre-calculated sector format from track type
    size_t header_skip = handle->sector_header_size;
    size_t data_size = handle->raw_data_size;
    
    // For raw sectors, verify sync pattern and allow per-sector mode override
    // This handles discs with mixed mode sectors
    if (handle->frame_size >= 2352 && header_skip > 0) {
        // Check for sync pattern: 00 FF FF FF FF FF FF FF FF FF FF 00
        if (src[0] == 0x00 && src[1] == 0xFF && src[11] == 0x00) {
            // Sync pattern found at offset 0 - standard layout
            int mode = src[15];
            if (mode == 1) {
                header_skip = 16; // Sync(12) + Header(4)
                data_size = 2048;
            } else if (mode == 2) {
                header_skip = 24; // Sync(12) + Header(4) + Subheader(8)
                data_size = 2048;
            }
        } else if (handle->frame_size == 2448 && 
                   src[96] == 0x00 && src[97] == 0xFF && src[107] == 0x00) {
            // Sync pattern found at offset 96 - subcode-first layout
            int mode = src[96 + 15];
            src += 96;  // Adjust source pointer to skip subcode
            
            if (mode == 1) {
                header_skip = 16;
                data_size = 2048;
            } else if (mode == 2) {
                header_skip = 24;
                data_size = 2048;
            }
        }
    }
    
    // Copy the data
    size_t to_copy = requested_bytes;
    if (to_copy > data_size)
        to_copy = data_size;
    
    memcpy(buffer, src + header_skip, to_copy);
    
    return to_copy;
}

void chd_close_track(void* track_handle) {
    chd_track_handle_t* handle = (chd_track_handle_t*)track_handle;
    if (!handle)
        return;
    
    if (handle->hunk_buffer)
        free(handle->hunk_buffer);
    
    if (handle->chd)
        chd_close(handle->chd);
    
    free(handle);
}

uint32_t chd_first_track_sector(void* track_handle) {
    chd_track_handle_t* handle = (chd_track_handle_t*)track_handle;
    if (!handle)
        return 0;
    
    // Return 0 to indicate the track starts at relative sector 0.
    // The read_sector callback will add track_start_frame to convert 
    // to CHD frame numbers.
    return 0;
}

/*****************************************************************************
 * Integration helper
 *****************************************************************************/

int chd_reader_is_chd(const char* path) {
    const char* ext = strrchr(path, '.');
    return ext && strcasecmp(ext, ".chd") == 0;
}
