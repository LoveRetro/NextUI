#include <stdbool.h>
#include <msettings.h>

#include "sdl.h"
#include "defines.h"
#include "api.h"
#include "utils.h"


#define NUM_OPTIONS 4
#define NUM_MAIN_OPTIONS 5
#define MAX_NAME_LEN 255

const char *lightnames[5];
#define NROF_TRIGGERS 14
const char *triggernames[] = {
    "B", "A", "Y", "X", "L", "R", "FN1", "FN2", "MENU", "SELECT", "START", "ALL", "LR", "DPAD"};

const char *effect_names[] = {
    "Linear", "Breathe", "Interval Breathe", "Static",
    "Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
    "Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive"};
const char *topbar_effect_names[] = {
    "Linear", "Breathe", "Interval Breathe", "Static",
    "Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
    "Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive", "Topbar Rainbow", "Topbar night"};
const char *lr_effect_names[] = {
    "Linear", "Breathe", "Interval Breathe", "Static",
    "Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
    "Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive", "LR Rainbow", "LR Reactive"};

// Resolves an effect id to the index the platform's effect list holds it at,
// so left/right walk the list the hardware actually supports. Falls back to the
// first entry for ids that aren't offered here (a settings file carried over
// from another device, or one of the profile-only ids).
static int effect_index_of(int effect_id) {
    int count = PLAT_getLedEffectCount();
    for (int i = 0; i < count; ++i) {
        if (PLAT_getLedEffectId(i) == effect_id) return i;
    }
    return 0;
}

// Platforms that don't name their own effects keep the historical tables below.
static const char *effect_name_for(int light_index, int effect_id) {
    const char *name = PLAT_getLedEffectName(effect_id);
    if (name) return name;

    const char **table = effect_names;
    int count = sizeof(effect_names) / sizeof(effect_names[0]);
    if (light_index == 3) {
        table = lr_effect_names;
        count = sizeof(lr_effect_names) / sizeof(lr_effect_names[0]);
    }
    else if (light_index == 2) {
        table = topbar_effect_names;
        count = sizeof(topbar_effect_names) / sizeof(topbar_effect_names[0]);
    }
    // effect comes from a user-editable ini, so it can be anything
    if (effect_id < 1 || effect_id > count) return "Unknown";
    return table[effect_id - 1];
}

void save_settings() {
    LOG_debug("saving settings plat\n");
    char diskfilename[256];
    int maxlights = LEDS_getCount();
    // TODO: this shouldnt be in shared userdata
    snprintf(diskfilename, sizeof(diskfilename), SHARED_USERDATA_PATH "/%s", PLAT_getLedSettingsFile());

    FILE *file = fopen(diskfilename, "w");

    if (file == NULL)
    {
        perror("Unable to open settings file for writing");
    } else {
        LOG_debug("saving leds!\n");
        for (int i = 0; i < maxlights; ++i)
        {
            fprintf(file, "[%s]\n", lightsDefault[i].name);
            fprintf(file, "effect=%d\n", lightsDefault[i].effect);
            fprintf(file, "color1=0x%06X\n", lightsDefault[i].color1);
            fprintf(file, "color2=0x%06X\n", lightsDefault[i].color2);
            fprintf(file, "speed=%d\n", lightsDefault[i].speed);
            fprintf(file, "brightness=%d\n", lightsDefault[i].brightness);
            fprintf(file, "trigger=%d\n", lightsDefault[i].trigger);
            fprintf(file, "filename=%s\n", lightsDefault[i].filename);
            fprintf(file, "inbrightness=%i\n", lightsDefault[i].inbrightness);
            fprintf(file, "\n"); // added last extra break seperate because everytime i add another option i forget to change it lol
        }

        fclose(file);
    }
}
    
void handle_light_input(LightSettings *light, SDL_Event *event, int selected_setting)
{
    const uint32_t bright_colors[] = {
        // Blues
        0x000011, 0x000022, 0x000033, 0x000044, 0x000055, 0x000066, 0x000077, 0x000088, 0x000099, 0x0000AA, 0x0000BB, 0x0000CC, 0x3366FF, 0x4D7AFF, 0x6699FF, 0x80B3FF, 0x99CCFF, 0xB3D9FF, 0x0000FF,
        // Cyan
        0x001111, 0x002222, 0x003333, 0x004444, 0x005555, 0x006666, 0x007777, 0x008888, 0x009999, 0x00AAAA, 0x00BBBB, 0x00CCCC, 0x33FFFF, 0x4DFFFF, 0x66FFFF, 0x80FFFF, 0x99FFFF, 0xB3FFFF, 0x00FFFF,
        // Green
        0x001100, 0x002200, 0x003300, 0x004400, 0x005500, 0x006600, 0x007700, 0x008800, 0x009900, 0x00AA00, 0x00BB00, 0x00CC00, 0x33FF33, 0x4DFF4D, 0x66FF66, 0x80FF80, 0x99FF99, 0xB3FFB3, 0x00FF00,
        // Magenta
        0x110011, 0x220022, 0x330033, 0x440044, 0x550055, 0x660066, 0x770077, 0x880088, 0x990099, 0xAA00AA, 0xBB00BB, 0xCC00CC, 0xFF33FF, 0xFF4DFF, 0xFF66FF, 0xFF80FF, 0xFF99FF, 0xFFB3FF, 0xFF00FF,
        // Purple
        0x220044, 0x330066, 0x440088, 0x5500AA, 0x6600CC, 0x7700DD, 0x8800EE, 0x9900FF, 0xAA00FF, 0xBB00FF, 0xCC00FF, 0x8833FF, 0x994DFF, 0xAA66FF, 0xBB80FF, 0xCC99FF, 0xDDB3FF,
        // Red
        0x220000, 0x440000, 0x660000, 0x880000, 0xAA0000, 0xCC0000, 0xFF3333, 0xFF4D4D, 0xFF6666, 0xFF8080, 0xFF9999, 0xFFB3B3, 0xFF0000,
        // Yellow
        0x222200, 0x444400, 0x666600, 0x888800, 0xAAAA00, 0xCCCC00, 0xFFFF33, 0xFFFF4D, 0xFFFF66, 0xFFFF80, 0xFFFF99, 0xFFFFB3, 0xFFFF00,
        // Orange
        0x331100, 0x662200, 0x993300, 0xCC4400, 0xFF5500, 0xFF6600, 0xFF7711, 0xFF8822, 0xFF9933, 0xFFAA44, 0xFFBB55, 0xFFCC66, 0xFFDD77, 0xFFEE88,
        // White to Black Gradient
        0x000000, 0x111111, 0x222222, 0x333333, 0x444444, 0x555555, 0x666666, 0x777777, 0x888888, 0x999999, 0xAAAAAA, 0xBBBBBB, 0xCCCCCC, 0xDDDDDD, 0xFFFFFF
    };
    
    const int num_bright_colors = sizeof(bright_colors) / sizeof(bright_colors[0]);

    switch (selected_setting)
    {
    case 0: // Effect
    {
        int effect_count = PLAT_getLedEffectCount();
        if (effect_count > 0 && PAD_justPressed(BTN_RIGHT))
        {
            int index = (effect_index_of(light->effect) + 1) % effect_count;
            light->effect = PLAT_getLedEffectId(index);
        }
        else if (effect_count > 0 && PAD_justPressed(BTN_LEFT))
        {
            int index = (effect_index_of(light->effect) - 1 + effect_count) % effect_count;
            light->effect = PLAT_getLedEffectId(index);
        }
        break;
    }
    case 1: // Color
    if (PAD_justPressed(BTN_RIGHT))
        {
            int current_index = -1;
            for (int i = 0; i < num_bright_colors; i++)
            {
                if (bright_colors[i] == light->color1)
                {
                    current_index = i;
                    break;
                }
            }
            light->color1 = bright_colors[(current_index + 1) % num_bright_colors];
        }
        else if (PAD_justPressed(BTN_LEFT))
        {
            int current_index = -1;
            for (int i = 0; i < num_bright_colors; i++)
            {
                if (bright_colors[i] == light->color1)
                {
                    current_index = i;
                    break;
                }
            }
            light->color1 = bright_colors[(current_index - 1 + num_bright_colors) % num_bright_colors];
        }
        break;
    // case 2: // Color2
    //     if (PAD_justPressed(BTN_RIGHT))
    //     {
    //         int current_index = -1;
    //         for (int i = 0; i < num_bright_colors; i++)
    //         {
    //             if (bright_colors[i] == light->color2)
    //             {
    //                 current_index = i;
    //                 break;
    //             }
    //         }
    //         SDL_Log("saved settings to disk and shm %d", current_index);
    //         light->color2 = bright_colors[(current_index + 1) % num_bright_colors];
    //     }
    //     else if (PAD_justPressed(BTN_LEFT))
    //     {
    //         int current_index = -1;
    //         for (int i = 0; i < num_bright_colors; i++)
    //         {
    //             if (bright_colors[i] == light->color2)
    //             {
    //                 current_index = i;
    //                 break;
    //             }
    //         }
    //         light->color2 = bright_colors[(current_index - 1 + num_bright_colors) % num_bright_colors];
    //     }
    //     break;
    case 2: // Duration
    if (PAD_justPressed(BTN_RIGHT))
        {
            light->speed = (light->speed + 100) % 5000; // Increase duration
        }
        else if (PAD_justPressed(BTN_LEFT))
        {
            light->speed = (light->speed - 100 + 5000) % 5000; // Decrease duration
        }
        break;
    case 3: // Brightness
    if (PAD_justPressed(BTN_RIGHT))
        {
            light->brightness = (light->brightness + 5) % 105; // Increase duration
        }
        else if (PAD_justPressed(BTN_LEFT))
        {
            light->brightness = (light->brightness - 5 + 105) % 105; // Decrease duration
        }
        break;
    case 4: // Info Brightness
    if (PAD_justPressed(BTN_RIGHT))
        {
            light->inbrightness = (light->inbrightness + 5) % 105; // Increase duration
        }
        else if (PAD_justPressed(BTN_LEFT))
        {
            light->inbrightness = (light->inbrightness - 5 + 105) % 105; // Decrease duration
        }
        break;
    case 5: // trigger
    if (PAD_justPressed(BTN_RIGHT))
        {
            light->trigger = (light->trigger % NROF_TRIGGERS) + 1; // Increase effect (1 to 8)
        }
        else if (PAD_justPressed(BTN_LEFT))
        {
            light->trigger = (light->trigger - 2 + NROF_TRIGGERS) % NROF_TRIGGERS + 1; // Decrease effect (1 to 8)
        }
        break;
    }

    // Save settings after each change
    LEDS_setProfile(LIGHT_PROFILE_DEFAULT);
    LEDS_updateLeds(false);
    save_settings();
}

int main(int argc, char *argv[])
{
    char* device = getenv("DEVICE");

	InitSettings();
    PWR_setCPUSpeed(CPU_SPEED_AUTO);

    SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();

    GFX_clearAll();
	GFX_clearLayers(0);
	GFX_flip(screen);

    // GFX_init() is what gets the platform far enough along to answer this
    // (the trimui platforms only learn their model in PLAT_initPlatform)
    int numOfLights = LEDS_getCount();

    if (numOfLights == 0) {
        // devices in this platform's family that simply have no RGB lights
        int quit_msg = 0;
        int msg_dirty = 1;
        while (!quit_msg) {
            GFX_startFrame();
            PAD_poll();
            if (PAD_justPressed(BTN_B)) quit_msg = 1;

            if (msg_dirty) {
                GFX_clear(screen);
                GFX_blitMessage(font.large, "This device has no RGB lights.", screen,
                    &(SDL_Rect){0, 0, screen->w, screen->h});
                GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 1, screen, 1);
                GFX_flip(screen);
                msg_dirty = 0;
            }
            else GFX_sync();
        }
        PWR_quit();
        PAD_quit();
        GFX_quit();
        QuitSettings();
        return 0;
    }

    // brightness is a single global control unless the hardware exposes one
    // per light (the Brick family does, via its per-zone max_scale nodes)
    int combinedBrightness = 1;
    if (exactMatch("brick", device)) {
        const char *brick_names[] = {"F1 key", "F2 key", "Top bar", "L&R triggers"};
        memcpy(lightnames, brick_names, sizeof(brick_names)); // Copy values
        combinedBrightness = 0;
    }
    else if (exactMatch("brickpro", device)) {
        const char *brickpro_names[] = {"F1 key", "F2 key", "Top bar", "Joysticks", "Triggers"};
        memcpy(lightnames, brickpro_names, sizeof(brickpro_names)); // Copy values
        combinedBrightness = 0;
    } else {
        const char *default_names[] = {"Joystick L","Joystick R", "Logo"};
        memcpy(lightnames, default_names, sizeof(default_names)); // Copy values
    }
    // platforms that name their own lights win over the tables above
    for (int i = 0; i < numOfLights && i < 5; ++i) {
        const char *name = PLAT_getLedLabel(i);
        if (name) lightnames[i] = name;
    }

    bool running = true;
    int selected_light = 0;
    int selected_setting = 0;
    SDL_Event event;
    int quit = 0;
	int dirty = 1;
    int show_setting = 0; // 1=brightness,2=volume,3=colortemp
    int was_online = PWR_isOnline();
    int had_bt = PLAT_btIsConnected();

    while (!quit)
    {
        GFX_startFrame();
        uint32_t frame_start = SDL_GetTicks();
		
		PAD_poll();

        PWR_update(&dirty, &show_setting, NULL, NULL);
        
        int is_online = PWR_isOnline();
		if (was_online!=is_online) 
            dirty = 1;
		was_online = is_online;

        int has_bt = PLAT_btIsConnected();
        if (had_bt != has_bt)
            dirty = 1;
        had_bt = has_bt;

        if (PAD_justPressed(BTN_B)) {
            quit = 1;
        }
        else if(PAD_justPressed(BTN_DOWN)) {
            selected_setting = (selected_setting + 1) % NUM_MAIN_OPTIONS;
            dirty = 1;
        }
        else if(PAD_justPressed(BTN_UP)) {
            selected_setting = (selected_setting - 1 + NUM_MAIN_OPTIONS) % NUM_MAIN_OPTIONS;
            dirty = 1;
        }
        else if(PAD_justPressed(BTN_L1)) {
            selected_light = (selected_light - 1 + numOfLights) % numOfLights;
            dirty = 1;
        }
        else if(PAD_justPressed(BTN_R1)) {
            selected_light = (selected_light + 1) % numOfLights;
            dirty = 1;
        }
        else if(PAD_justPressed(BTN_LEFT) || PAD_justPressed(BTN_RIGHT)) {
            handle_light_input(&lightsDefault[selected_light],&event, selected_setting);
            dirty = 1;
        }

        if (dirty) {
            GFX_clear(screen);

            int ow = GFX_blitHardwareGroup(screen, show_setting);

            if (show_setting) GFX_blitHardwareHints(screen, show_setting);

            GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 1, screen, 1);
            GFX_blitButtonGroup((char*[]){ "L/R","Select light", NULL }, 0, screen, 0);


            int max_width = screen->w - SCALE1(PADDING * 2) - ow;
            // Display light name
            char light_name_text[256];
            snprintf(light_name_text, sizeof(light_name_text), "%s", lightnames[selected_light]);

            char title[256];
            int text_width = GFX_truncateText(font.medium, light_name_text, title, max_width, SCALE1(BUTTON_PADDING * 2));
            max_width = MIN(max_width, text_width);

            SDL_Surface *text;
            text = TTF_RenderUTF8_Blended(font.medium, title, COLOR_WHITE);
            GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), max_width, SCALE1(PILL_SIZE)});
            SDL_BlitSurface(text, &(SDL_Rect){0, 0, max_width - SCALE1(BUTTON_PADDING * 2), text->h}, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4)});
            SDL_FreeSurface(text);

            // Display settings
            // const char *settings_labels[6] = {"Effect", "Color", "Color2", "Speed", "Brightness", "Trigger"};
            // this stuff is really not multiplatform at all, need to figure out a way so its not so TrimUI specific
            const char *settings_labels[5]; // Define array with correct size
            if (combinedBrightness) {
                const char *non_brick_labels[] = {"Effect", "Color", "Speed", "Brightness (All Leds)", "Info brightness (All Leds)"};
                memcpy(settings_labels, non_brick_labels, sizeof(non_brick_labels)); // Copy values
            } else {
                const char *brick_labels[] = {"Effect", "Color", "Speed", "Brightness", "Info brightness"};
                memcpy(settings_labels, brick_labels, sizeof(brick_labels)); // Copy values
            }
            int settings_values[5] = {
                lightsDefault[selected_light].effect,
                lightsDefault[selected_light].color1,
                lightsDefault[selected_light].speed,
                lightsDefault[selected_light].brightness,
                lightsDefault[selected_light].inbrightness
            };

            for (int j = 0; j < 5; ++j)
            {
                char setting_text[256];
                bool selected = (j == selected_setting);
                SDL_Color current_color = selected ? COLOR_BLACK : COLOR_WHITE;

                int y = SCALE1(PADDING + PILL_SIZE * (j + 1));

                if (j == 0) { // Display effect name instead of number
                    snprintf(setting_text, sizeof(setting_text), "%s: %s", settings_labels[j], effect_name_for(selected_light, settings_values[j]));
                    SDL_Surface *text = TTF_RenderUTF8_Blended(font.medium, setting_text, current_color);
                    int text_width = text->w + SCALE1(BUTTON_PADDING * 2);
                    GFX_blitPill(selected ? ASSET_WHITE_PILL : ASSET_BLACK_PILL, screen,
                                    &(SDL_Rect){SCALE1(PADDING), y, text_width, SCALE1(PILL_SIZE)});
                    SDL_BlitSurface(text, 
                        &(SDL_Rect){0, 0, text->w, text->h}, screen, 
                        &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4)});
                    SDL_FreeSurface(text);
                } else if (j == 1) { // Display color as hex code
                    snprintf(setting_text, sizeof(setting_text), "%s", settings_labels[j]);
                    SDL_Surface *text = TTF_RenderUTF8_Blended(font.medium, setting_text, current_color);
                    int text_width = text->w + SCALE1(BUTTON_PADDING * 2);
                    GFX_blitPill(selected ? ASSET_WHITE_PILL : ASSET_BLACK_PILL, screen, 
                        &(SDL_Rect){SCALE1(PADDING), y, text_width + SCALE1(BUTTON_MARGIN + BUTTON_SIZE), SCALE1(PILL_SIZE)});
                    SDL_BlitSurface(text, 
                        &(SDL_Rect){0, 0, text->w, text->h}, screen, 
                        &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4)});
                    SDL_FreeSurface(text);

                    // color1 is stored as 0xRRGGBB; GFX_blitAssetColor expects 0xRRGGBBAA
                    GFX_blitAssetColor(ASSET_BUTTON, NULL, screen, &(SDL_Rect){
                        SCALE1(PADDING) + text_width,
                        y + SCALE1(BUTTON_MARGIN)
                    }, ((uint32_t)settings_values[j] << 8) | 0xFF);
                } else  {
                    snprintf(setting_text, sizeof(setting_text), "%s: %d", settings_labels[j], settings_values[j]);
                    SDL_Surface *text = TTF_RenderUTF8_Blended(font.medium, setting_text, current_color);
                    int text_width = text->w + SCALE1(BUTTON_PADDING * 2);
                    GFX_blitPill(selected ? ASSET_WHITE_PILL : ASSET_BLACK_PILL, screen, 
                        &(SDL_Rect){SCALE1(PADDING), y, text_width, SCALE1(PILL_SIZE)});
                    SDL_BlitSurface(text, 
                        &(SDL_Rect){0, 0, text->w, text->h}, screen, 
                        &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4)});
                    SDL_FreeSurface(text);
                }
            }

            GFX_flip(screen);
            dirty = 0;
        }
        else GFX_sync();
    }
	PWR_quit();
	PAD_quit();
	GFX_quit();
    QuitSettings();

    return 0;
}
