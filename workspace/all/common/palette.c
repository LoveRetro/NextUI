#include "palette.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "defines.h"
#include "config.h"

static const uint32_t palette_default_colors[PALETTE_COLOR_COUNT] = {
    CFG_DEFAULT_COLOR1, CFG_DEFAULT_COLOR2, CFG_DEFAULT_COLOR3,
    CFG_DEFAULT_COLOR4, CFG_DEFAULT_COLOR5, CFG_DEFAULT_COLOR6,
    CFG_DEFAULT_COLOR7,
};

// Derive a display name from a filename: strip .txt, replace underscores with spaces.
static void palette_nameFromFilename(const char *filename, char *out, size_t out_size)
{
    size_t len = strlen(filename);
    if (len > 4 && strcasecmp(filename + len - 4, ".txt") == 0)
        len -= 4;
    if (len >= out_size)
        len = out_size - 1;
    for (size_t i = 0; i < len; i++)
        out[i] = (filename[i] == '_') ? ' ' : filename[i];
    out[len] = '\0';
}

// Parse a single palette file. Returns true on success and a supported version.
static bool palette_loadFile(const char *path, const char *filename, bool builtin, ColorPalette *out)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return false;

    memset(out, 0, sizeof(*out));
    out->version = 1; // assumed when version= is omitted
    out->builtin = builtin;
    strncpy(out->path, path, sizeof(out->path) - 1);
    for (int i = 0; i < PALETTE_COLOR_COUNT; i++)
        out->colors[i] = palette_default_colors[i];

    bool haveName = false;
    char line[256];
    while (fgets(line, sizeof(line), file))
    {
        int temp_value;
        if (sscanf(line, "version=%i", &temp_value) == 1)
        {
            out->version = temp_value;
            continue;
        }
        if (strncmp(line, "name=", 5) == 0)
        {
            char *value = line + 5;
            value[strcspn(value, "\r\n")] = 0;
            strncpy(out->name, value, sizeof(out->name) - 1);
            haveName = out->name[0] != '\0';
            continue;
        }
        for (int i = 0; i < PALETTE_COLOR_COUNT; i++)
        {
            char key[8];
            snprintf(key, sizeof(key), "color%d=", i + 1);
            size_t keylen = strlen(key);
            if (strncmp(line, key, keylen) == 0)
            {
                char *value = line + keylen;
                value[strcspn(value, "\r\n")] = 0;
                out->colors[i] = CFG_parseHexColor(value);
                break;
            }
        }
    }
    fclose(file);

    // Skip palettes authored for a newer format than we understand.
    if (out->version > PALETTE_VERSION_MAX)
        return false;

    if (!haveName)
        palette_nameFromFilename(filename, out->name, sizeof(out->name));

    return true;
}

static int palette_scanDir(const char *dirpath, bool builtin, ColorPalette *out, int max, int count)
{
    DIR *dir = opendir(dirpath);
    if (!dir)
        return count;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max)
    {
        if (ent->d_name[0] == '.')
            continue;
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".txt") != 0)
            continue;

        char path[PALETTE_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);
        if (palette_loadFile(path, ent->d_name, builtin, &out[count]))
            count++;
    }
    closedir(dir);
    return count;
}

int PALETTE_enumerate(ColorPalette *out, int max)
{
    if (!out || max <= 0)
        return 0;

    int count = 0;
    // Built-ins first (read-only), then user/community drop-ins.
    count = palette_scanDir(RES_PATH "/palettes", true, out, max, count);
    count = palette_scanDir(SDCARD_PATH "/Palettes", false, out, max, count);
    return count;
}

void PALETTE_apply(const ColorPalette *palette)
{
    if (!palette)
        return;
    // CFG_applyPalette sets the 7 colors and the palette name as one atomic step,
    // so the persisted "current palette" can never point at a color set that
    // doesn't match it.
    CFG_applyPalette(palette->name, palette->colors);
}
