/*
 * On-Screen Keyboard for minarch
 * Ported from settings/keyboardprompt.cpp (MIT License)
 * Original: https://github.com/josegonzalez/minui-keyboard
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "keyboard.h"
#include "defines.h"
#include "api.h"

// Minarch accessor and utility functions
#include "minarch.h"
#define screen minarch_getScreen()

// External globals from minarch.c
extern struct GFX_Fonts font;

#define KB_ROWS 5
#define KB_COLS 14
#define KB_MAX_INPUT 128

// Keyboard layouts
static const char* kb_layout_lower[KB_ROWS][KB_COLS] = {
    {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", NULL},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\", NULL},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", NULL, NULL, NULL},
    {"z", "x", "c", "v", "b", "n", "m", ",", ".", "/", NULL, NULL, NULL, NULL},
    {"SHIFT", "SPACE", "DONE", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};

static const char* kb_layout_upper[KB_ROWS][KB_COLS] = {
    {"~", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "+", NULL},
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}", "|", NULL},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L", ":", "\"", NULL, NULL, NULL},
    {"Z", "X", "C", "V", "B", "N", "M", "<", ">", "?", NULL, NULL, NULL, NULL},
    {"SHIFT", "SPACE", "DONE", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};

// Count non-null keys in a row
static int kb_row_length(const char* layout[KB_ROWS][KB_COLS], int row) {
    int len = 0;
    for (int i = 0; i < KB_COLS; i++) {
        if (layout[row][i] != NULL) len++;
    }
    return len;
}

// Draw the on-screen keyboard
static void kb_draw(const char* title, const char* input_text, int cur_row, int cur_col, int shift) {
    const char* (*layout)[KB_COLS] = shift ? kb_layout_upper : kb_layout_lower;

    GFX_clear(screen);

    int center_x = screen->w / 2;
    int center_y = screen->h / 2;
    SDL_Surface* text;
    int text_w;

    // Keyboard dimensions (smaller with padding)
    int key_size = SCALE1(18);
    int key_spacing = SCALE1(3);
    int special_key_w = SCALE1(50);
    int special_spacing = SCALE1(58);

    // Calculate total keyboard height to center it
    int kb_height = (KB_ROWS * key_size) + ((KB_ROWS - 1) * key_spacing);
    int title_h = SCALE1(30);
    int input_h = SCALE1(24);
    int gap = SCALE1(12);
    int total_h = title_h + gap + input_h + gap + kb_height;

    int content_start_y = center_y - total_h / 2;

    // Title
    text = TTF_RenderUTF8_Blended(font.medium, title, COLOR_WHITE);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, content_start_y});
    SDL_FreeSurface(text);

    // Input field background
    int input_y = content_start_y + title_h + gap;
    int input_w = screen->w - SCALE1(100);  // More padding on sides
    SDL_Rect input_bg = {center_x - input_w/2, input_y, input_w, input_h};
    SDL_FillRect(screen, &input_bg, SDL_MapRGB(screen->format, 40, 40, 40));

    // Input text (show actual characters so user can verify)
    int len = strlen(input_text);

    if (len > 0) {
        text = TTF_RenderUTF8_Blended(font.small, input_text, COLOR_WHITE);
        text_w = text->w;
        // Clamp to fit in input field
        if (text_w > input_bg.w - SCALE1(10)) {
            SDL_Rect src = {text_w - (input_bg.w - SCALE1(10)), 0, input_bg.w - SCALE1(10), text->h};
            SDL_BlitSurface(text, &src, screen, &(SDL_Rect){input_bg.x + SCALE1(5), input_y + SCALE1(3)});
        } else {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, input_y + SCALE1(3)});
        }
        SDL_FreeSurface(text);
    }

    // Keyboard
    int kb_start_y = input_y + input_h + gap;

    for (int row = 0; row < KB_ROWS; row++) {
        int row_len = kb_row_length(layout, row);
        int row_width;

        if (row == KB_ROWS - 1) {
            // Special row with SHIFT, SPACE, DONE
            row_width = (3 * special_key_w) + (2 * key_spacing);
        } else {
            row_width = (row_len * key_size) + ((row_len - 1) * key_spacing);
        }

        int start_x = center_x - row_width / 2;

        for (int col = 0; col < KB_COLS; col++) {
            const char* key = layout[row][col];
            if (key == NULL) continue;

            bool selected = (row == cur_row && col == cur_col);
            int key_w = key_size;

            // Special keys are wider
            if (strcmp(key, "SHIFT") == 0 || strcmp(key, "SPACE") == 0 || strcmp(key, "DONE") == 0) {
                key_w = special_key_w;
            }

            int key_x;
            if (row == KB_ROWS - 1) {
                // Position special keys
                if (col == 0) key_x = start_x;
                else if (col == 1) key_x = start_x + special_key_w + key_spacing;
                else key_x = start_x + 2 * (special_key_w + key_spacing);
            } else {
                key_x = start_x + col * (key_size + key_spacing);
            }

            int key_y = kb_start_y + row * (key_size + key_spacing);

            // Key background
            SDL_Rect key_rect = {key_x, key_y, key_w, key_size};
            Uint32 bg_color = selected ?
                SDL_MapRGB(screen->format, 255, 255, 255) :
                SDL_MapRGB(screen->format, 60, 60, 60);
            SDL_FillRect(screen, &key_rect, bg_color);

            // Key text - use tiny font for special keys, small for regular keys
            SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
            bool is_special = (strcmp(key, "SHIFT") == 0 || strcmp(key, "SPACE") == 0 || strcmp(key, "DONE") == 0);
            text = TTF_RenderUTF8_Blended(is_special ? font.tiny : font.small, key, text_color);
            int tx = key_x + (key_w - text->w) / 2;
            int ty = key_y + (key_size - text->h) / 2;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){tx, ty});
            SDL_FreeSurface(text);
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){ "B","DELETE", "Y","CANCEL", "A","TYPE", NULL }, 1, screen, 1);

    GFX_flip(screen);
}

char* Keyboard_show(const char* title) {
    char input[KB_MAX_INPUT + 1] = {0};
    int input_len = 0;
    int cur_row = 0;
    int cur_col = 0;
    int shift = 0;  // 0 = lowercase, 1 = uppercase
    int dirty = 1;

    const char* (*layout)[KB_COLS];

    while (1) {
        layout = shift ? kb_layout_upper : kb_layout_lower;

        GFX_startFrame();
        PAD_poll();

        // Cancel
        if (PAD_justPressed(BTN_Y) || PAD_justPressed(BTN_MENU)) {
            return NULL;
        }

        // Navigation
        if (PAD_justRepeated(BTN_UP)) {
            cur_row--;
            if (cur_row < 0) cur_row = KB_ROWS - 1;
            // Clamp column to row length
            int row_len = kb_row_length(layout, cur_row);
            if (cur_col >= row_len) cur_col = row_len - 1;
            dirty = 1;
        }
        else if (PAD_justRepeated(BTN_DOWN)) {
            cur_row++;
            if (cur_row >= KB_ROWS) cur_row = 0;
            int row_len = kb_row_length(layout, cur_row);
            if (cur_col >= row_len) cur_col = row_len - 1;
            dirty = 1;
        }
        else if (PAD_justRepeated(BTN_LEFT)) {
            cur_col--;
            if (cur_col < 0) {
                int row_len = kb_row_length(layout, cur_row);
                cur_col = row_len - 1;
            }
            dirty = 1;
        }
        else if (PAD_justRepeated(BTN_RIGHT)) {
            cur_col++;
            int row_len = kb_row_length(layout, cur_row);
            if (cur_col >= row_len) cur_col = 0;
            dirty = 1;
        }
        // Delete character
        else if (PAD_justPressed(BTN_B)) {
            if (input_len > 0) {
                input[--input_len] = '\0';
                dirty = 1;
            }
        }
        // Type character / action
        else if (PAD_justPressed(BTN_A)) {
            const char* key = layout[cur_row][cur_col];
            if (key != NULL) {
                if (strcmp(key, "SHIFT") == 0) {
                    shift = !shift;
                    dirty = 1;
                }
                else if (strcmp(key, "SPACE") == 0) {
                    if (input_len < KB_MAX_INPUT) {
                        input[input_len++] = ' ';
                        input[input_len] = '\0';
                        dirty = 1;
                    }
                }
                else if (strcmp(key, "DONE") == 0) {
                    // Return the input
                    if (input_len > 0) {
                        char* result = malloc(input_len + 1);
                        if (result) {
                            strcpy(result, input);
                        }
                        return result;
                    }
                    return NULL;  // Empty input = cancel
                }
                else {
                    // Regular character
                    if (input_len < KB_MAX_INPUT) {
                        input[input_len++] = key[0];
                        input[input_len] = '\0';
                        dirty = 1;
                    }
                }
            }
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            kb_draw(title, input, cur_row, cur_col, shift);
            dirty = 0;
        }

        minarch_hdmimon();
    }
}

char* Keyboard_getPassword(void) {
    return Keyboard_show("Enter WiFi Password");
}
