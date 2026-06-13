/*
 * NextUI Netplay Helper Module Implementation
 * Extracted UI helpers and orchestration functions for netplay menus
 */

#include "netplay_helper.h"
#include "defines.h"
#include "api.h"
#include "network_common.h"
#ifdef HAS_WIFIMG
#include "wifi_direct.h"
#endif
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

//////////////////////////////////////////////////////////////////////////////
// Minarch accessor functions and types
//////////////////////////////////////////////////////////////////////////////

// Minarch accessor and utility functions
#include "minarch.h"

// Convenience macros for accessor functions
#define screen minarch_getScreen()
#define DEVICE_WIDTH minarch_getDeviceWidth()
#define DEVICE_HEIGHT minarch_getDeviceHeight()

// Create a fake menu struct with just bitmap for compatibility
static struct { SDL_Surface* bitmap; } _menu_accessor;
#define menu (_menu_accessor.bitmap = minarch_getMenuBitmap(), _menu_accessor)

// String utility functions (defined in utils.c)
extern int exactMatch(char* str1, char* str2);
extern int containsString(char* haystack, char* needle);

//////////////////////////////////////////////////////////////////////////////
// Host Discovery State Variables
//////////////////////////////////////////////////////////////////////////////

static NetplayHostInfo netplay_hosts[NETPLAY_MAX_HOSTS];
static int netplay_host_count = 0;
static int netplay_selected_host = 0;
int netplay_force_resume = 0;
int netplay_connected_to_hotspot = 0;

static GBALinkHostInfo gbalink_hosts[GBALINK_MAX_HOSTS];
static int gbalink_host_count = 0;
int gbalink_connected_to_hotspot = 0;
int gbalink_force_resume = 0;

static GBLinkHostInfo gblink_hosts[GBLINK_MAX_HOSTS];
static int gblink_host_count = 0;
int gblink_connected_to_hotspot = 0;
int gblink_force_resume = 0;

// Store the hotspot SSID client connected to (shared - only one game runs at a time)
char connected_hotspot_ssid[33] = {0};

//////////////////////////////////////////////////////////////////////////////
// WiFi/Network Helpers
//////////////////////////////////////////////////////////////////////////////

#include "keyboard.h"

// Launch on-screen keyboard for password input
static char* launchKeyboard(void) {
    return Keyboard_getPassword();
}

// Get signal strength indicator string based on RSSI
// RSSI: typically -30 (excellent) to -90 (poor)
static const char* getSignalStrengthIndicator(int rssi) {
    if (rssi >= -50) return "[####]";      // Excellent
    else if (rssi >= -60) return "[### ]";  // Good
    else if (rssi >= -70) return "[##  ]";  // Fair
    else if (rssi >= -80) return "[#   ]";  // Weak
    else return "[    ]";                    // Very weak
}

// Help dialog entries
typedef struct {
    const char* symbol;
    const char* description;
} WiFiHelpEntry;

static const WiFiHelpEntry wifi_help_entries[] = {
    {"[C]", "Currently connected"},
    {"[*]", "Saved (auto-connect)"},
    {"[L]", "Locked (needs password)"},
    {"[####]", "Excellent signal"},
    {"[### ]", "Good signal"},
    {"[##  ]", "Fair signal"},
    {"[#   ]", "Weak signal"},
    {NULL, NULL}
};

// Render WiFi help dialog overlay
static void renderWiFiHelpDialog(void) {
    int hw = screen->w;
    int hh = screen->h;

    // Count entries
    int entry_count = 0;
    while (wifi_help_entries[entry_count].symbol != NULL) {
        entry_count++;
    }

    // Dialog dimensions
    int line_height = SCALE1(22);
    int box_w = SCALE1(260);
    int box_h = SCALE1(70) + (entry_count * line_height);
    int box_x = (hw - box_w) / 2;
    int box_y = (hh - box_h) / 2;

    // Dark overlay around dialog
    SDL_Rect overlay = {0, 0, hw, hh};
    SDL_FillRect(screen, &overlay, SDL_MapRGB(screen->format, 0, 0, 0));

    // Box background
    SDL_Rect box = {box_x, box_y, box_w, box_h};
    SDL_FillRect(screen, &box, SDL_MapRGB(screen->format, 32, 32, 32));

    // Box border
    SDL_Rect border_top = {box_x, box_y, box_w, SCALE1(2)};
    SDL_Rect border_bot = {box_x, box_y + box_h - SCALE1(2), box_w, SCALE1(2)};
    SDL_Rect border_left = {box_x, box_y, SCALE1(2), box_h};
    SDL_Rect border_right = {box_x + box_w - SCALE1(2), box_y, SCALE1(2), box_h};
    SDL_FillRect(screen, &border_top, SDL_MapRGB(screen->format, 255, 255, 255));
    SDL_FillRect(screen, &border_bot, SDL_MapRGB(screen->format, 255, 255, 255));
    SDL_FillRect(screen, &border_left, SDL_MapRGB(screen->format, 255, 255, 255));
    SDL_FillRect(screen, &border_right, SDL_MapRGB(screen->format, 255, 255, 255));

    int left_margin = box_x + SCALE1(20);
    int right_col = box_x + SCALE1(80);

    // Title
    SDL_Surface* title_surf = TTF_RenderUTF8_Blended(font.medium, "WiFi Symbols", COLOR_WHITE);
    if (title_surf) {
        SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){left_margin, box_y + SCALE1(12)});
        SDL_FreeSurface(title_surf);
    }

    // Entries
    int y_offset = box_y + SCALE1(42);
    for (int i = 0; i < entry_count; i++) {
        // Symbol
        SDL_Surface* sym_surf = TTF_RenderUTF8_Blended(font.small, wifi_help_entries[i].symbol, COLOR_WHITE);
        if (sym_surf) {
            SDL_BlitSurface(sym_surf, NULL, screen, &(SDL_Rect){left_margin, y_offset});
            SDL_FreeSurface(sym_surf);
        }

        // Description
        SDL_Surface* desc_surf = TTF_RenderUTF8_Blended(font.small, wifi_help_entries[i].description, COLOR_GRAY);
        if (desc_surf) {
            SDL_BlitSurface(desc_surf, NULL, screen, &(SDL_Rect){right_col, y_offset});
            SDL_FreeSurface(desc_surf);
        }

        y_offset += line_height;
    }

    // Hint at bottom
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(font.tiny, "Press any button to close", COLOR_GRAY);
    if (hint_surf) {
        int hint_x = box_x + (box_w - hint_surf->w) / 2;
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){hint_x, box_y + box_h - SCALE1(18)});
        SDL_FreeSurface(hint_surf);
    }

    GFX_flip(screen);
}

// Show WiFi help dialog - blocks until user presses any button
static void showWiFiHelpDialog(void) {
    renderWiFiHelpDialog();

    // Wait for any button press
    while (1) {
        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B) ||
            PAD_justPressed(BTN_MENU) || PAD_justPressed(BTN_UP) ||
            PAD_justPressed(BTN_DOWN) || PAD_justPressed(BTN_LEFT) ||
            PAD_justPressed(BTN_RIGHT)) {
            break;
        }

        PWR_update(NULL, NULL, minarch_beforeSleep, minarch_afterSleep);
        minarch_hdmimon();
    }
}

// Render WiFi network selection list
// connected_ssid: SSID of currently connected network (or NULL if not connected)
#ifdef HAS_WIFIMG
static void renderWiFiNetworkList(WIFI_direct_network_t* networks, int count, int selected, const char* connected_ssid) {
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

    SDL_Surface* text;
    int text_w;
    int center_x = screen->w / 2;

    // Title
    int title_y = SCALE1(60);
    text = TTF_RenderUTF8_Blended(font.large, "Select WiFi Network", COLOR_WHITE);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, title_y});
    SDL_FreeSurface(text);

    // Instruction
    int instruction_y = title_y + SCALE1(22);
    text = TTF_RenderUTF8_Blended(font.small, "Choose a network to use", COLOR_GRAY);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, instruction_y});
    SDL_FreeSurface(text);

    int list_start_y = instruction_y + SCALE1(35);  // Extra space for up arrow indicator
    int max_visible = 3;  // Limited to 3 to prevent overlap with button hints

    // Show "Scanning..." message if no networks found yet
    if (count <= 0) {
        int scanning_y = list_start_y + SCALE1(PILL_SIZE * 2);
        text = TTF_RenderUTF8_Blended(font.medium, "Scanning for networks...", COLOR_GRAY);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, scanning_y});
        SDL_FreeSurface(text);

        GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 0, screen, 1);
        GFX_flip(screen);
        return;
    }

    // Calculate visible range for scrolling
    int start_idx = 0;
    if (count > max_visible) {
        // Center selection in the visible area when possible
        start_idx = selected - max_visible / 2;
        if (start_idx < 0) start_idx = 0;
        if (start_idx + max_visible > count) start_idx = count - max_visible;
    }
    int visible_count = (count > max_visible) ? max_visible : count;

    // Network list with pills
    for (int j = 0; j < visible_count; j++) {
        int idx = start_idx + j;

        // Build network label: [status] ssid [signal]
        // Status: [C] = Connected, [*] = Saved credentials, [L] = Locked, [ ] = Open
        char label[128];
        bool is_connected;
        bool has_creds;
        bool is_secured;
        const char* signal;
        const char* ssid;

        WIFI_direct_network_t* net = &networks[idx];
        ssid = net->ssid;
        is_connected = (connected_ssid && strcmp(ssid, connected_ssid) == 0);
        has_creds = net->has_saved_creds;
        is_secured = net->is_secured;
        signal = getSignalStrengthIndicator(net->rssi);

        const char* status;
        if (is_connected) {
            status = "[C]";  // Currently connected
        } else if (has_creds) {
            status = "[*]";  // Has saved credentials
        } else if (is_secured) {
            status = "[L]";  // Locked (needs password)
        } else {
            status = "   ";  // Open network
        }

        snprintf(label, sizeof(label), "%s %s %s", status, ssid, signal);

        SDL_Color text_color = COLOR_WHITE;
        if (idx == selected) {
            text_color = uintToColour(THEME_COLOR5_255);
            int ow;
            TTF_SizeUTF8(font.medium, label, &ow, NULL);
            ow += SCALE1(BUTTON_PADDING * 2);
            // Cap max width
            int max_pill_w = DEVICE_WIDTH - SCALE1(PADDING * 4);
            if (ow > max_pill_w) ow = max_pill_w;
            GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
                center_x - ow/2,
                list_start_y + j * SCALE1(PILL_SIZE),
                ow,
                SCALE1(PILL_SIZE)
            });
        }

        text = TTF_RenderUTF8_Blended(font.medium, label, text_color);
        text_w = text->w;
        // Cap display width
        int max_text_w = DEVICE_WIDTH - SCALE1(PADDING * 4);
        if (text_w > max_text_w) {
            SDL_Rect src_rect = {0, 0, max_text_w, text->h};
            SDL_BlitSurface(text, &src_rect, screen, &(SDL_Rect){center_x - max_text_w/2, list_start_y + j * SCALE1(PILL_SIZE) + SCALE1(4)});
        } else {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, list_start_y + j * SCALE1(PILL_SIZE) + SCALE1(4)});
        }
        SDL_FreeSurface(text);
    }

    // Scroll indicators if needed
    if (count > max_visible) {
        if (start_idx > 0) {
            // Show up arrow indicator - more networks above
            char up_hint[32];
            snprintf(up_hint, sizeof(up_hint), "▲ %d more", start_idx);
            text = TTF_RenderUTF8_Blended(font.tiny, up_hint, COLOR_GRAY);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, list_start_y - SCALE1(15)});
            SDL_FreeSurface(text);
        }
        if (start_idx + max_visible < count) {
            // Show down arrow indicator - more networks below
            int remaining = count - (start_idx + max_visible);
            char down_hint[32];
            snprintf(down_hint, sizeof(down_hint), "▼ %d more", remaining);
            text = TTF_RenderUTF8_Blended(font.tiny, down_hint, COLOR_GRAY);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, list_start_y + visible_count * SCALE1(PILL_SIZE) - SCALE1(2)});
            SDL_FreeSurface(text);
        }
    }

    GFX_blitButtonGroup((char*[]){ "MENU","HELP", NULL }, 0, screen, 0);  // Left aligned
    GFX_blitButtonGroup((char*[]){ "B","BACK", "A","SELECT", NULL }, 1, screen, 1);  // Right aligned
    GFX_flip(screen);
}
#endif // HAS_WIFIMG

// Show WiFi network selection UI
// Returns true if user successfully connects (or confirms current connection), false if cancelled/failed
static bool showWiFiNetworkSelection(void) {
#ifdef HAS_WIFIMG
    // Use WIFI_direct functions for more reliable WiFi operations

    // Ensure WiFi hardware is ready
    if (!WIFI_direct_ensureReady()) {
        minarch_menuMessage("Failed to initialize WiFi.\n\nPlease try again.",
                     (char*[]){ "A","OKAY", NULL });
        return false;
    }

    // Check if already connected to a WiFi network
    char connected_ssid_buf[WIFI_DIRECT_SSID_MAX] = {0};
    const char* connected_ssid = NULL;

    if (WIFI_direct_isConnected()) {
        if (WIFI_direct_getCurrentSSID(connected_ssid_buf, sizeof(connected_ssid_buf)) == 0) {
            connected_ssid = connected_ssid_buf;
        }
    }

    // Network list - continuously updated
    WIFI_direct_network_t networks[16];
    int count = 0;
    int selected = 0;
    int dirty = 1;
    bool first_selection_done = false;

    // Scan timing - separate trigger from reading to avoid blocking UI
    uint32_t last_scan_trigger_time = 0;
    uint32_t scan_trigger_interval_ms = 4000;  // Trigger new scan every 4 seconds
    uint32_t scan_read_delay_ms = 1500;        // Wait 1.5s after trigger before reading
    bool scan_pending = false;

    // Overall timeout to prevent hanging
    uint32_t start_time = SDL_GetTicks();
    uint32_t max_duration_ms = 120000;  // 120 seconds (2 minutes) max on this screen

    // Trigger initial scan immediately (non-blocking)
    WIFI_direct_triggerScan();
    last_scan_trigger_time = SDL_GetTicks();
    scan_pending = true;

    while (1) {
        uint32_t now = SDL_GetTicks();

        // Check for overall timeout
        if (now - start_time > max_duration_ms) {
            minarch_menuMessage("WiFi selection timed out.\n\nPlease try again.",
                         (char*[]){ "A","OKAY", NULL });
            return false;
        }

        // Read scan results after delay (non-blocking read of cached results)
        if (scan_pending && (now - last_scan_trigger_time >= scan_read_delay_ms)) {
            scan_pending = false;

            int new_count = WIFI_direct_scanNetworks(networks, 16);

            if (new_count != count || new_count > 0) {
                count = new_count;
                dirty = 1;

                // Auto-select best network on first successful scan
                if (count > 0 && !first_selection_done) {
                    first_selection_done = true;

                    // Find the best network to pre-select
                    int preselect_idx = -1;
                    int best_saved_idx = -1;
                    int best_rssi = -999;

                    for (int i = 0; i < count; i++) {
                        if (connected_ssid && strcmp(networks[i].ssid, connected_ssid) == 0) {
                            preselect_idx = i;
                        }
                        if (networks[i].has_saved_creds && networks[i].rssi > best_rssi) {
                            best_rssi = networks[i].rssi;
                            best_saved_idx = i;
                        }
                    }

                    if (preselect_idx >= 0) {
                        selected = preselect_idx;
                    } else if (best_saved_idx >= 0) {
                        selected = best_saved_idx;
                    } else {
                        selected = 0;
                    }
                }

                // Keep selection in bounds
                if (selected >= count && count > 0) {
                    selected = count - 1;
                }
            }
        }

        // Trigger periodic rescan (non-blocking)
        if (!scan_pending && (now - last_scan_trigger_time >= scan_trigger_interval_ms)) {
            WIFI_direct_triggerScan();
            last_scan_trigger_time = now;
            scan_pending = true;
        }

        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_B)) {
            return false;  // Cancel
        }

        // Show help dialog when MENU is pressed
        if (PAD_justPressed(BTN_MENU)) {
            showWiFiHelpDialog();
            dirty = 1;  // Redraw after dialog closes
        }

        // Navigation (only if we have networks)
        if (count > 0) {
            if (PAD_justRepeated(BTN_UP)) {
                selected--;
                if (selected < 0) selected = count - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                selected++;
                if (selected >= count) selected = 0;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A)) {
                // Selected network
                WIFI_direct_network_t* net = &networks[selected];

                // Check if user selected the already-connected network
                if (connected_ssid && strcmp(net->ssid, connected_ssid) == 0) {
                    // Already connected to this network
                    // Verify we have an IP, request DHCP if needed
                    showOverlayMessage("Verifying connection...");

                    // Check if we already have an IP
                    char ip[16] = {0};
                    WIFI_direct_getIP(ip, sizeof(ip));

                    if (ip[0] == '\0' || strcmp(ip, "0.0.0.0") == 0) {
                        // No IP yet, request DHCP and wait
                        system("udhcpc -i wlan0 -q -t 5 >/dev/null 2>&1");

                        // Wait for IP with timeout
                        for (int i = 0; i < 10; i++) {
                            SDL_Delay(500);
                            WIFI_direct_getIP(ip, sizeof(ip));
                            if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
                                break;
                            }
                        }
                    }

                    return true;
                }

                if (net->has_saved_creds || !net->is_secured) {
                    // Connect with saved credentials or open network
                    showOverlayMessage("Connecting...");
                    int ret = WIFI_direct_connect(net->ssid, NULL);  // NULL = use saved creds

                    if (ret == 0) {
                        // Wait for DHCP to assign IP
                        showOverlayMessage("Getting IP address...");
                        char ip[16] = {0};
                        bool got_ip = false;

                        // First check if we already have IP
                        WIFI_direct_getIP(ip, sizeof(ip));
                        if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
                            got_ip = true;
                        } else {
                            // Request DHCP
                            system("udhcpc -i wlan0 -q -t 5 >/dev/null 2>&1");
                            // Wait for IP with timeout
                            for (int i = 0; i < 20; i++) {  // 10 second timeout
                                SDL_Delay(500);
                                WIFI_direct_getIP(ip, sizeof(ip));
                                if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
                                    got_ip = true;
                                    break;
                                }
                            }
                        }

                        if (got_ip) {
                            return true;
                        }
                        minarch_menuMessage("Connected but no IP.\n\nPlease try again.",
                                     (char*[]){ "A","OKAY", NULL });
                    } else {
                        minarch_menuMessage("Connection failed.\n\nPlease check the network\nand try again.",
                                     (char*[]){ "A","OKAY", NULL });
                    }
                    dirty = 1;
                    // Force rescan
                    WIFI_direct_triggerScan();
                    last_scan_trigger_time = SDL_GetTicks();
                    scan_pending = true;
                } else {
                    // Need password - launch keyboard
                    char* password = launchKeyboard();
                    if (password) {
                        showOverlayMessage("Connecting...");
                        int ret = WIFI_direct_connect(net->ssid, password);
                        free(password);

                        if (ret == 0) {
                            // Wait for DHCP to assign IP
                            showOverlayMessage("Getting IP address...");
                            char ip[16] = {0};
                            bool got_ip = false;

                            // Request DHCP
                            system("udhcpc -i wlan0 -q -t 5 >/dev/null 2>&1");
                            // Wait for IP with timeout
                            for (int i = 0; i < 20; i++) {  // 10 second timeout
                                SDL_Delay(500);
                                WIFI_direct_getIP(ip, sizeof(ip));
                                if (ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0) {
                                    got_ip = true;
                                    break;
                                }
                            }

                            if (got_ip) {
                                return true;
                            }
                            minarch_menuMessage("Connected but no IP.\n\nPlease try again.",
                                         (char*[]){ "A","OKAY", NULL });
                        } else {
                            minarch_menuMessage("Connection failed.\n\nIncorrect password or\nnetwork unavailable.",
                                         (char*[]){ "A","OKAY", NULL });
                        }
                    }
                    dirty = 1;
                    // Force rescan
                    WIFI_direct_triggerScan();
                    last_scan_trigger_time = SDL_GetTicks();
                    scan_pending = true;
                }
            }
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            renderWiFiNetworkList(networks, count, selected, connected_ssid);
            dirty = 0;
        }

        minarch_hdmimon();
    }

#else
    // WiFi network selection not available on this platform
    minarch_menuMessage("WiFi not available\non this platform.",
                 (char*[]){ "A","OKAY", NULL });
    return false;
#endif
}

bool ensureWifiEnabled(void) {
#ifdef HAS_WIFIMG
    // Check if WiFi is already ready (avoid showing message if already enabled)
    if (WIFI_direct_isConnected()) {
        return true;  // Already connected, no need to enable
    }

    // Check if wpa_supplicant is running (quick check without full ensureReady)
    int ret = system("pidof wpa_supplicant > /dev/null 2>&1");
    if (ret == 0) {
        // wpa_supplicant running, WiFi is likely ready
        return true;
    }

    // Show enabling message only when we actually need to enable WiFi
    GFX_setMode(MODE_MAIN);
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
    {
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, "Enabling WiFi...", COLOR_WHITE);
        int text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
        SDL_FreeSurface(text);
    }
    GFX_flip(screen);

    // Use WIFI_direct to ensure WiFi is ready
    bool ready = WIFI_direct_ensureReady();

    GFX_setMode(MODE_MENU);

    if (!ready) {
        minarch_menuMessage("Failed to enable WiFi.\nPlease try again.", (char*[]){ "A","OKAY", NULL });
        return false;
    }

    return true;
#else
    minarch_menuMessage("WiFi not available\non this platform.", (char*[]){ "A","OKAY", NULL });
    return false;
#endif
}

bool ensureNetworkConnected(LinkType type, const char* action) {
    (void)action;  // Currently unused after adding WiFi selection

    // Always show WiFi selection so user can confirm or change network
    // If already connected, that network will be pre-selected
    // User can either confirm current connection or switch to another
    if (!showWiFiNetworkSelection()) {
        return false;  // User cancelled
    }

    // Verify connection after user selection
    bool connected = false;
    switch (type) {
        case LINK_TYPE_NETPLAY: connected = Netplay_hasNetworkConnection(); break;
        case LINK_TYPE_GBALINK: connected = GBALink_hasNetworkConnection(); break;
        case LINK_TYPE_GBLINK: connected = GBLink_hasNetworkConnection(); break;
    }
    return connected;
}

// Structure for async hotspot stop + WiFi restore
typedef struct {
    bool stop_hotspot;  // true if host (need to call WIFI_direct_stopHotspot)
    char hotspot_ssid[33];
} HotspotStopArgs;

static void* hotspot_stop_thread(void* arg) {
    HotspotStopArgs* args = (HotspotStopArgs*)arg;

#ifdef HAS_WIFIMG
    if (args->stop_hotspot) {
        WIFI_direct_stopHotspot();
    }

    // Forget every saved hotspot network (this session's plus any left behind by
    // earlier sessions that didn't tear down cleanly), and re-enable other networks
    // so wpa_supplicant.conf doesn't accumulate stale NextUI-* entries over time.
    WIFI_direct_forgetAllHotspots();

    // Restore previous WiFi connection
    WIFI_direct_restorePreviousConnection();
#endif

    free(args);
    return NULL;
}

void stopHotspotAndRestoreWiFiAsync(bool is_host) {
    HotspotStopArgs* args = malloc(sizeof(HotspotStopArgs));
    if (!args) {
        LOG_error("stopHotspotAndRestoreWiFiAsync: failed to allocate args\n");
        return;
    }

    args->stop_hotspot = is_host;

    // Copy and clear the connected hotspot SSID
    strncpy(args->hotspot_ssid, connected_hotspot_ssid, sizeof(args->hotspot_ssid) - 1);
    args->hotspot_ssid[sizeof(args->hotspot_ssid) - 1] = '\0';
    connected_hotspot_ssid[0] = '\0';

    // Spawn detached thread
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, hotspot_stop_thread, args) != 0) {
        LOG_error("stopHotspotAndRestoreWiFiAsync: failed to create thread\n");
        free(args);
    }

    pthread_attr_destroy(&attr);
}

//////////////////////////////////////////////////////////////////////////////
// UI Helpers
//////////////////////////////////////////////////////////////////////////////

void showOverlayMessage(const char* msg) {
    GFX_setMode(MODE_MAIN);
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
    SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
    int text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
    SDL_FreeSurface(text);
    GFX_flip(screen);
    GFX_setMode(MODE_MENU);
}

void showConnectedSuccess(uint32_t timeout_ms) {
    uint32_t start_time = SDL_GetTicks();
    GFX_setMode(MODE_MAIN);
    while (SDL_GetTicks() - start_time < timeout_ms) {
        GFX_startFrame();
        PAD_poll();
        if (PAD_justPressed(BTN_A)) break;

        GFX_clear(screen);
        GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

        SDL_Surface* text;
        int text_w;
        int center_x = screen->w / 2;
        int center_y = screen->h / 2;

        text = TTF_RenderUTF8_Blended(font.large, "Connected!", COLOR_WHITE);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y - SCALE1(20)});
        SDL_FreeSurface(text);

        text = TTF_RenderUTF8_Blended(font.medium, "Starting game...", COLOR_WHITE);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y + SCALE1(20)});
        SDL_FreeSurface(text);

        GFX_flip(screen);
        minarch_hdmimon();
    }
    GFX_setMode(MODE_MENU);
}

int Menu_selectConnectionMode(const char* title) {
    int selected = 0;
    int dirty = 1;
    const char* modes[] = { "Hotspot", "WiFi" };
    int mode_count = 2;

    while (1) {
        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_B)) {
            return -1;  // Cancelled
        }

        if (PAD_justRepeated(BTN_UP)) {
            selected--;
            if (selected < 0) selected = mode_count - 1;
            dirty = 1;
        }
        else if (PAD_justRepeated(BTN_DOWN)) {
            selected++;
            if (selected >= mode_count) selected = 0;
            dirty = 1;
        }
        else if (PAD_justPressed(BTN_A)) {
            return selected;  // 0 = Hotspot, 1 = WiFi
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            GFX_clear(screen);
            GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

            SDL_Surface* text;
            int text_w;
            int center_x = screen->w / 2;

            // Title
            int title_y = SCALE1(60);
            text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, title_y});
            SDL_FreeSurface(text);

            // Instruction
            int instruction_y = title_y + SCALE1(30);
            text = TTF_RenderUTF8_Blended(font.medium, "Select connection mode:", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, instruction_y});
            SDL_FreeSurface(text);

            // Subtitle hint
            int subtitle_y = instruction_y + SCALE1(20);
            text = TTF_RenderUTF8_Blended(font.small, "Use hotspot for better gameplay", COLOR_GRAY);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, subtitle_y});
            SDL_FreeSurface(text);

            // Mode list with pills
            int list_start_y = subtitle_y + SCALE1(25);
            for (int j = 0; j < mode_count; j++) {
                SDL_Color text_color = COLOR_WHITE;
                if (j == selected) {
                    text_color = uintToColour(THEME_COLOR5_255);
                    int ow;
                    TTF_SizeUTF8(font.large, modes[j], &ow, NULL);
                    ow += SCALE1(BUTTON_PADDING * 2);
                    GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
                        center_x - ow/2,
                        list_start_y + j * SCALE1(PILL_SIZE),
                        ow,
                        SCALE1(PILL_SIZE)
                    });
                }

                text = TTF_RenderUTF8_Blended(font.large, modes[j], text_color);
                text_w = text->w;
                SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, list_start_y + j * SCALE1(PILL_SIZE) + SCALE1(4)});
                SDL_FreeSurface(text);
            }

            GFX_blitButtonGroup((char*[]){ "B","BACK", "A","SELECT", NULL }, 1, screen, 1);
            GFX_flip(screen);
            dirty = 0;
        }

        minarch_hdmimon();
    }
}

//////////////////////////////////////////////////////////////////////////////
// Host Discovery State Accessors
//////////////////////////////////////////////////////////////////////////////

const char* getHostGameName(LinkType type, int index) {
    switch (type) {
        case LINK_TYPE_NETPLAY: return netplay_hosts[index].game_name;
        case LINK_TYPE_GBALINK: return gbalink_hosts[index].game_name;
        case LINK_TYPE_GBLINK:  return gblink_hosts[index].game_name;
    }
    return "";
}

const char* getHostIP(LinkType type, int index) {
    switch (type) {
        case LINK_TYPE_NETPLAY: return netplay_hosts[index].host_ip;
        case LINK_TYPE_GBALINK: return gbalink_hosts[index].host_ip;
        case LINK_TYPE_GBLINK:  return gblink_hosts[index].host_ip;
    }
    return "";
}

int getHostPort(LinkType type, int index) {
    switch (type) {
        case LINK_TYPE_NETPLAY: return netplay_hosts[index].port;
        case LINK_TYPE_GBALINK: return gbalink_hosts[index].port;
        case LINK_TYPE_GBLINK:  return gblink_hosts[index].port;
    }
    return 0;
}

const char* getHostLinkMode(LinkType type, int index) {
    switch (type) {
        case LINK_TYPE_GBALINK: return gbalink_hosts[index].link_mode;
        default: return "";  // Only GBALink uses link_mode
    }
}

int getHostCount(LinkType type) {
    switch (type) {
        case LINK_TYPE_NETPLAY: return netplay_host_count;
        case LINK_TYPE_GBALINK: return gbalink_host_count;
        case LINK_TYPE_GBLINK:  return gblink_host_count;
    }
    return 0;
}

void setHostCount(LinkType type, int count) {
    switch (type) {
        case LINK_TYPE_NETPLAY: netplay_host_count = count; break;
        case LINK_TYPE_GBALINK: gbalink_host_count = count; break;
        case LINK_TYPE_GBLINK:  gblink_host_count = count; break;
    }
}

int isLinkConnected(LinkType type) {
    switch (type) {
        case LINK_TYPE_NETPLAY: return Netplay_getMode() != NETPLAY_OFF;
        case LINK_TYPE_GBALINK: return GBALink_getMode() != GBALINK_OFF;
        case LINK_TYPE_GBLINK:  return GBLink_getMode() != GBLINK_OFF;
    }
    return 0;
}

int* getForceResumeFlag(LinkType type) {
    switch (type) {
        case LINK_TYPE_NETPLAY: return &netplay_force_resume;
        case LINK_TYPE_GBALINK: return &gbalink_force_resume;
        case LINK_TYPE_GBLINK:  return &gblink_force_resume;
    }
    return NULL;
}

int Multiplayer_isActive(void) {
    return GBALink_isConnected() || GBLink_isConnected() || Netplay_isConnected();
}

CoreLinkSupport checkCoreLinkSupport(const char* core_name) {
    CoreLinkSupport support = {false, false, false};

    if (Netplay_checkCoreSupport(core_name)) {
        support.show_netplay = true;
    }
    if (GBALink_checkCoreSupport(core_name)) {
        support.has_netpacket = true;
        support.show_netplay = true;
    }
    if (GBLink_checkCoreSupport(core_name)) {
        support.has_gblink = true;
        support.show_netplay = true;
    }

    return support;
}

//////////////////////////////////////////////////////////////////////////////
// Rendering Helpers
//////////////////////////////////////////////////////////////////////////////

void showSearchingScreen(void) {
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
    SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, "Searching for hosts...", COLOR_WHITE);
    int text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
    SDL_FreeSurface(text);
    GFX_blitButtonGroup((char*[]){ "B","CANCEL", NULL }, 0, screen, 1);
    GFX_flip(screen);
}

void showConnectingScreen(const char* host_ip) {
    char msg[256];
    snprintf(msg, sizeof(msg), "Connecting to %s...", host_ip);
    GFX_setMode(MODE_MAIN);
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
    SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
    int text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
    SDL_FreeSurface(text);
    GFX_flip(screen);
    GFX_setMode(MODE_MENU);
}

void renderHostSelectionList(LinkType type, int selected, int host_count) {
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

    SDL_Surface* text;
    int text_w;
    int center_x = screen->w / 2;

    // Title
    int title_y = SCALE1(60);
    text = TTF_RenderUTF8_Blended(font.large, "Select Host", COLOR_WHITE);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, title_y});
    SDL_FreeSurface(text);

    // Host list with pills
    int list_start_y = title_y + SCALE1(40);
    for (int j = 0; j < host_count; j++) {
        // Format: "Game Name (IP)"
        char host_label[128];
        snprintf(host_label, sizeof(host_label), "%s (%s)",
                 getHostGameName(type, j), getHostIP(type, j));

        SDL_Color text_color = COLOR_WHITE;
        if (j == selected) {
            text_color = uintToColour(THEME_COLOR5_255);
            int ow;
            TTF_SizeUTF8(font.medium, host_label, &ow, NULL);
            ow += SCALE1(BUTTON_PADDING * 2);
            GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
                center_x - ow/2,
                list_start_y + j * SCALE1(PILL_SIZE),
                ow,
                SCALE1(PILL_SIZE)
            });
        }

        text = TTF_RenderUTF8_Blended(font.medium, host_label, text_color);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, list_start_y + j * SCALE1(PILL_SIZE) + SCALE1(4)});
        SDL_FreeSurface(text);
    }

    GFX_blitButtonGroup((char*[]){ "B","BACK", "A","SELECT", NULL }, 1, screen, 1);
    GFX_flip(screen);
}

void renderHotspotWaitingScreen(const char* code) {
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

    SDL_Surface* text;
    int text_w, text_h;
    int center_x = screen->w / 2;
    int center_y = screen->h / 2;

    // Large code in pill (centered, prominent)
    TTF_SizeUTF8(font.large, code, &text_w, &text_h);
    int pill_w = text_w + SCALE1(BUTTON_PADDING * 2);
    int pill_y = center_y - text_h - SCALE1(4);
    GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
        center_x - pill_w/2,
        pill_y,
        pill_w,
        SCALE1(PILL_SIZE)
    });
    text = TTF_RenderUTF8_Blended(font.large, code, uintToColour(THEME_COLOR5_255));
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, pill_y + SCALE1(4)});
    SDL_FreeSurface(text);

    // Medium instruction
    text = TTF_RenderUTF8_Blended(font.medium, "Select this code on the other device", COLOR_WHITE);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y + SCALE1(5)});
    SDL_FreeSurface(text);

    // Small status
    text = TTF_RenderUTF8_Blended(font.small, "Waiting for connection...", COLOR_WHITE);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y + SCALE1(28)});
    SDL_FreeSurface(text);
}

void renderWiFiWaitingScreen(const char* ip) {
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

    SDL_Surface* text;
    int text_w, text_h;
    int center_x = screen->w / 2;
    int center_y = screen->h / 2;

    // Large IP (centered, prominent)
    text = TTF_RenderUTF8_Blended(font.large, ip, COLOR_WHITE);
    text_w = text->w;
    text_h = text->h;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y - text_h});
    SDL_FreeSurface(text);

    // Medium instruction
    text = TTF_RenderUTF8_Blended(font.medium, "Waiting for player to join...", COLOR_WHITE);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y + SCALE1(5)});
    SDL_FreeSurface(text);

    // Small hint
    text = TTF_RenderUTF8_Blended(font.small, "Other device must be on the same WiFi network", COLOR_WHITE);
    text_w = text->w;
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y + SCALE1(28)});
    SDL_FreeSurface(text);
}

void showConnectionSuccessScreen(void) {
    uint32_t start_time = SDL_GetTicks();
    GFX_setMode(MODE_MAIN);
    while (SDL_GetTicks() - start_time < 3000) {
        GFX_startFrame();
        PAD_poll();
        if (PAD_justPressed(BTN_A)) break;

        GFX_clear(screen);
        GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

        SDL_Surface* text;
        int text_w;
        int center_x = screen->w / 2;
        int center_y = screen->h / 2;

        text = TTF_RenderUTF8_Blended(font.large, "Connected!", COLOR_WHITE);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y - SCALE1(20)});
        SDL_FreeSurface(text);

        text = TTF_RenderUTF8_Blended(font.medium, "Starting game...", COLOR_WHITE);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y + SCALE1(20)});
        SDL_FreeSurface(text);

        GFX_flip(screen);
        minarch_hdmimon();
    }
    GFX_setMode(MODE_MENU);
}

//////////////////////////////////////////////////////////////////////////////
// Utility Functions
//////////////////////////////////////////////////////////////////////////////

uint32_t calculateGameCRC(void) {
    uint32_t crc = 0;
    void* game_data = minarch_getGameData();
    size_t game_size = minarch_getGameSize();
    if (game_data && game_size > 0) {
        const uint8_t* data = (const uint8_t*)game_data;
        for (size_t j = 0; j < game_size && j < 1024; j++) {
            crc = (crc << 1) ^ data[j];
        }
    }
    return crc;
}

void getGameName(char* buf, size_t buf_size) {
    const char* game_name = minarch_getGameName();
    if (game_name[0]) {
        strncpy(buf, game_name, buf_size - 1);
        buf[buf_size - 1] = '\0';
        // Remove extension if present
        char* dot = strrchr(buf, '.');
        if (dot) {
            *dot = '\0';
        }
    } else {
        strncpy(buf, "Unknown Game", buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

void showTransitionMessage(const char* message) {
    GFX_setMode(MODE_MAIN);
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
    {
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, message, COLOR_WHITE);
        int text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
        SDL_FreeSurface(text);
    }
    GFX_flip(screen);
}

void showTimedConfirmation(const char* message, int duration_ms) {
    uint32_t start_time = SDL_GetTicks();
    GFX_setMode(MODE_MAIN);
    while (SDL_GetTicks() - start_time < (uint32_t)duration_ms) {
        GFX_startFrame();
        PAD_poll();
        if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B)) break;

        GFX_clear(screen);
        GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

        SDL_Surface* text;
        int text_w;
        int center_x = screen->w / 2;
        int center_y = screen->h / 2;

        text = TTF_RenderUTF8_Blended(font.large, message, COLOR_WHITE);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y});
        SDL_FreeSurface(text);

        GFX_flip(screen);
        minarch_hdmimon();
    }
    GFX_setMode(MODE_MENU);
}

// Show a confirmation dialog that requires A to confirm or B to cancel
// Returns true if user pressed A, false if user pressed B
static bool showConfirmDialog(const char* title, const char* message) {
    GFX_setMode(MODE_MAIN);
    while (1) {
        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_A)) {
            GFX_setMode(MODE_MENU);
            return true;
        }
        if (PAD_justPressed(BTN_B)) {
            GFX_setMode(MODE_MENU);
            return false;
        }

        GFX_clear(screen);
        GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

        SDL_Surface* text;
        int text_w;
        int center_x = screen->w / 2;
        int y = screen->h / 3;

        // Title
        text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
        SDL_FreeSurface(text);
        y += 70;

        // Message (can contain newlines - render line by line)
        char msg_copy[512];
        strncpy(msg_copy, message, sizeof(msg_copy) - 1);
        msg_copy[sizeof(msg_copy) - 1] = '\0';
        char* line = strtok(msg_copy, "\n");
        while (line) {
            text = TTF_RenderUTF8_Blended(font.medium, line, COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
            y += 40;
            line = strtok(NULL, "\n");
        }

        // Button hints at bottom
        y = screen->h - 60;
        GFX_blitButtonGroup((char*[]){ "B","CANCEL", "A","CONTINUE", NULL }, 0, screen, 1);

        GFX_flip(screen);
        minarch_hdmimon();
    }
}

// Show link mode restart dialog with specific layout
// is_host: true for host changing mode, false for client syncing to host
// Returns true if user pressed A (confirm), false if user pressed B (cancel)
static bool showLinkModeRestartDialog(const char* mode_name, bool is_host) {
    GFX_setMode(MODE_MAIN);
    while (1) {
        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_A)) {
            GFX_setMode(MODE_MENU);
            return true;
        }
        if (PAD_justPressed(BTN_B)) {
            GFX_setMode(MODE_MENU);
            return false;
        }

        GFX_clear(screen);
        GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

        SDL_Surface* text;
        int text_w;
        int center_x = screen->w / 2;
        int y = SCALE1(60);

        text = TTF_RenderUTF8_Blended(font.large, "Restart Required", COLOR_WHITE);
        text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
        SDL_FreeSurface(text);
        y += SCALE1(30);

        // Main message - different for host vs client
        if (is_host) {
            text = TTF_RenderUTF8_Blended(font.medium, "Changing connectivity mode to", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
            y += SCALE1(20);

            char mode_msg[128];
            snprintf(mode_msg, sizeof(mode_msg), "%s", mode_name);
            text = TTF_RenderUTF8_Blended(font.medium, mode_msg, COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
            y += SCALE1(20);

            text = TTF_RenderUTF8_Blended(font.medium, "requires a restart for", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
            y += SCALE1(20);

            text = TTF_RenderUTF8_Blended(font.medium, "the changes to take effect.", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
            y += SCALE1(20);

            text = TTF_RenderUTF8_Blended(font.medium, "Please rehost after restarting to connect.", COLOR_GRAY);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
        } else {
            text = TTF_RenderUTF8_Blended(font.medium, "Your connectivity mode doesn't match the host.", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
            y += SCALE1(20);

            text = TTF_RenderUTF8_Blended(font.medium, "A restart is needed to sync settings.", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
            y += SCALE1(20);

            text = TTF_RenderUTF8_Blended(font.medium, "Please rejoin after restarting to connect.", COLOR_GRAY);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, y});
            SDL_FreeSurface(text);
        }

        // Button hints at bottom
        GFX_blitButtonGroup((char*[]){ "B","CANCEL", "A","RESTART", NULL }, 0, screen, 1);

        GFX_flip(screen);
        minarch_hdmimon();
    }
}

void showLinkStatusMessage(
    const char* title,
    const char* mode_str,
    const char* conn_str,
    const char* state_str,
    const char* code,
    const char* local_ip,
    const char* status_msg
) {
    char msg[512];

    if (code) {
        // Hotspot host mode - show code
        snprintf(msg, sizeof(msg), "%s\n\nMode: %s (%s)\nState: %s\nCode: %s\nIP: %s\n\n%s",
                 title, mode_str, conn_str, state_str, code, local_ip, status_msg);
    } else if (conn_str[0]) {
        // Connected with connection type
        snprintf(msg, sizeof(msg), "%s\n\nMode: %s (%s)\nState: %s\nLocal IP: %s\n\n%s",
                 title, mode_str, conn_str, state_str, local_ip, status_msg);
    } else {
        // Not connected
        snprintf(msg, sizeof(msg), "%s\n\nMode: %s\nState: %s\nLocal IP: %s\n\n%s",
                 title, mode_str, state_str, local_ip, status_msg);
    }

    minarch_menuMessage(msg, (char*[]){ "A","OKAY", NULL });
}

void renderLinkMenuUI(
    const char* title,
    char** items,
    int item_count,
    int selected,
    const char* (*getHint)(void)
) {
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.25f, 1, 0);
    GFX_blitHardwareGroup(screen, 0);

    // Title
    SDL_Surface* text;
    text = TTF_RenderUTF8_Blended(font.large, title, uintToColour(THEME_COLOR6_255));
    int title_w = text->w + SCALE1(BUTTON_PADDING * 2);
    GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){
        SCALE1(PADDING),
        SCALE1(PADDING),
        title_w,
        SCALE1(PILL_SIZE)
    });
    SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
        SCALE1(PADDING + BUTTON_PADDING),
        SCALE1(PADDING + 4)
    });
    SDL_FreeSurface(text);

    // Button hints
    GFX_blitButtonGroup((char*[]){ "B","BACK", "A","OKAY", NULL }, 1, screen, 1);

    // Menu items - centered vertically, shifted up by one pill size
    int oy = (((DEVICE_HEIGHT / FIXED_SCALE) - PADDING * 2) - (item_count * PILL_SIZE)) / 2 - PILL_SIZE;
    for (int i = 0; i < item_count; i++) {
        char* item = items[i];
        SDL_Color text_color = COLOR_WHITE;

        if (i == selected) {
            text_color = uintToColour(THEME_COLOR5_255);

            int ow;
            TTF_SizeUTF8(font.large, item, &ow, NULL);
            ow += SCALE1(BUTTON_PADDING * 2);

            // Selected pill background
            GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
                SCALE1(PADDING),
                SCALE1(oy + PADDING + (i * PILL_SIZE)),
                ow,
                SCALE1(PILL_SIZE)
            });
        }

        // Text
        text = TTF_RenderUTF8_Blended(font.large, item, text_color);
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
            SCALE1(PADDING + BUTTON_PADDING),
            SCALE1(oy + PADDING + (i * PILL_SIZE) + 4)
        });
        SDL_FreeSurface(text);
    }

    // Render contextual hint below menu items (left-aligned, multi-line support)
    if (getHint) {
        const char* hint = getHint();
        if (hint) {
            int leading = SCALE1(14);
            int y = SCALE1(oy + PADDING + (item_count * PILL_SIZE) + PILL_SIZE / 2);
            char line[256];
            const char* start = hint;
            const char* end;

            // Render each line left-aligned (with BUTTON_PADDING to align with text inside pills)
            while (*start) {
                end = strchr(start, '\n');
                int len = end ? (end - start) : (int)strlen(start);
                if (len > 0 && len < (int)sizeof(line)) {
                    strncpy(line, start, len);
                    line[len] = '\0';

                    SDL_Surface* hint_text = TTF_RenderUTF8_Blended(font.tiny, line, COLOR_WHITE);
                    if (hint_text) {
                        SDL_Rect dst = { SCALE1(PADDING + BUTTON_PADDING), y, hint_text->w, hint_text->h };
                        SDL_BlitSurface(hint_text, NULL, screen, &dst);
                        SDL_FreeSurface(hint_text);
                    }
                }
                y += leading;
                if (!end) break;
                start = end + 1;
            }
        }
    }

    GFX_flip(screen);
}

//////////////////////////////////////////////////////////////////////////////
// Auto-Configuration Helpers
//////////////////////////////////////////////////////////////////////////////

// Convert gpsp_serial mode code to human-readable name
static const char* getGBALinkModeName(const char* mode) {
    if (!mode) return "Unknown";
    if (strcmp(mode, "auto") == 0) return "Automatic";
    if (strcmp(mode, "disabled") == 0) return "Disabled";
    if (strcmp(mode, "rfu") == 0) return "GBA Wireless Adapter";
    if (strcmp(mode, "mul_poke") == 0) return "Pokemon Gen3 Link Cable";
    if (strcmp(mode, "mul_aw1") == 0) return "Advance Wars 1";
    if (strcmp(mode, "mul_aw2") == 0) return "Advance Wars 2";
    return mode;  // Return original if unknown
}

// Auto-configure gpSP serial mode for GBA multiplayer
// Ensures a working mode is set since "auto" and "disable" don't support multiplayer
// Force gpSP's GBA link/serial mode to "auto" so the core auto-detects the correct
// adapter from the ROM (game code at 0xAC -> gba_over.h: RFU / Pokemon cable / Adv Wars).
// Returns true if it had to change the option and trigger a game reload; the caller must
// then bail with MENU_CALLBACK_EXIT, since gpSP only re-reads the serial mode on game load.
static bool forceGBALinkAutoNeedsReload(void) {
    // Only for GBA core (gpSP)
    if (!exactMatch((char*)minarch_getCoreTag(), "GBA")) return false;

    const char* current = minarch_getCoreOptionValue("gpsp_serial");
    if (current && strcmp(current, "auto") == 0) {
        // Already auto-detecting (resolved at the last game load) - nothing to do.
        return false;
    }

    minarch_setCoreOptionValue("gpsp_serial", "auto");
    minarch_saveConfig();
    minarch_reloadGame();        // deferred; re-runs load_gamepak -> auto-detect
    gbalink_force_resume = 1;    // close menus and resume into the reloaded game
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Orchestration Functions
//////////////////////////////////////////////////////////////////////////////

int hostGame_common(LinkType type, void* list, int i) {
    (void)list; (void)i;

    // Check if already in a session
    bool already_connected = false;
    const char* session_name = "Netplay";
    switch (type) {
        case LINK_TYPE_NETPLAY: already_connected = Netplay_getMode() != NETPLAY_OFF; break;
        case LINK_TYPE_GBALINK: already_connected = GBALink_getMode() != GBALINK_OFF; break;
        case LINK_TYPE_GBLINK:  already_connected = GBLink_getMode() != GBLINK_OFF; break;
    }

    if (already_connected) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Already in %s session.\nDisconnect first.", session_name);
        minarch_menuMessage(msg, (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    // Auto-enable WiFi if needed
    if (!ensureWifiEnabled()) {
        return MENU_CALLBACK_NOP;
    }

    // Auto-configure link cable mode for GBA games (gpSP auto-detects via gba_over.h)
    if (type == LINK_TYPE_GBALINK) {
        if (forceGBALinkAutoNeedsReload()) return MENU_CALLBACK_EXIT;
    }

    // Show mode selection using shared pill-style UI
    int selected = Menu_selectConnectionMode("Host Game");
    if (selected < 0) {
        return MENU_CALLBACK_NOP;  // Cancelled
    }

    // Get game name from current ROM
    char game_name[64];
    getGameName(game_name, sizeof(game_name));

    uint32_t crc = calculateGameCRC();

    // Execute selected mode
    if (selected == 0) {
        return hostGameHotspot_common(type, game_name, crc);
    } else {
        return hostGameWiFi_common(type, game_name, crc);
    }
}

int hostGameHotspot_common(LinkType type, const char* game_name, uint32_t crc) {
#ifndef HAS_WIFIMG
    minarch_menuMessage("WiFi not available\non this platform.", (char*[]){ "A","OKAY", NULL });
    return MENU_CALLBACK_NOP;
#endif

    // Show initial message
    GFX_setMode(MODE_MAIN);
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
    {
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, "Starting hotspot...", COLOR_WHITE);
        int text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
        SDL_FreeSurface(text);
    }
    GFX_flip(screen);
    GFX_setMode(MODE_MENU);

    // Generate SSID with random 4-character code
    char ssid[33];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    NET_HotspotConfig hotspot_cfg = {
        .prefix = LINK_HOTSPOT_SSID_PREFIX,
        .seed = (unsigned int)(tv.tv_usec ^ tv.tv_sec ^ crc)
    };
    NET_generateHotspotSSID(ssid, sizeof(ssid), &hotspot_cfg);

    const char* pass = WIFI_direct_getHotspotPassword();

    // Purge stale hotspot networks while wpa_supplicant is still up (startHotspot
    // kills it to hand wlan0 to hostapd). Clears entries this device accumulated
    // from past client joins.
    WIFI_direct_forgetAllHotspots();

    if (WIFI_direct_startHotspot(ssid, pass) != 0) {
        minarch_menuMessage("Failed to start hotspot.\nCheck device capabilities.", (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    // Get hotspot IP
    const char* hotspot_ip = WIFI_direct_getHotspotIP();

    // Start host with hotspot IP
    int start_result = -1;
    switch (type) {
        case LINK_TYPE_NETPLAY: start_result = Netplay_startHost(game_name, crc, hotspot_ip); break;
        case LINK_TYPE_GBALINK: {
            // Get current link mode to sync with client
            const char* link_mode = minarch_getCoreOptionValue("gpsp_serial");
            start_result = GBALink_startHost(game_name, crc, hotspot_ip, link_mode);
            break;
        }
        case LINK_TYPE_GBLINK:  start_result = GBLink_startHost(game_name, crc, hotspot_ip); break;
    }

    if (start_result != 0) {
        WIFI_direct_stopHotspot();
        minarch_menuMessage("Failed to start host.\nCheck device capabilities.", (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    // Extract code from SSID for display
    size_t prefix_len = strlen(LINK_HOTSPOT_SSID_PREFIX);
    const char* code = (strlen(ssid) > prefix_len) ? ssid + prefix_len : "????";

    // Wait loop
    int dirty = 1;
    int connected = 0;
    int cancelled = 0;

    GFX_setMode(MODE_MAIN);
    while (1) {
        GFX_startFrame();
        PAD_poll();

        // Check for cancellation
        if (PAD_justPressed(BTN_B)) {
            cancelled = 1;
            break;
        }

        // For GBLink: run a few core frames so gambatte can accept/process the
        // TCP connection; GBLink_isConnected() then polls the kernel socket table.
        if (type == LINK_TYPE_GBLINK) {
            for (int i = 0; i < 5; i++) {
                minarch_forceCoreOptionUpdate();
                if (GBLink_isConnected()) {
                    connected = 1;
                    break;
                }
            }
            if (connected) break;
        }

        // Check connection state for other types
        switch (type) {
            case LINK_TYPE_NETPLAY:
                if (Netplay_getState() == NETPLAY_STATE_SYNCING || Netplay_isConnected()) {
                    connected = 1;
                }
                break;
            case LINK_TYPE_GBALINK:
                if (GBALink_getState() == GBALINK_STATE_CONNECTED) {
                    connected = 1;
                }
                break;
            default:
                break;
        }
        if (connected) break;

        // For GBLink: skip the render delay to poll more frequently
        if (type == LINK_TYPE_GBLINK) {
            dirty = 1;  // Always redraw to keep loop fast
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            renderHotspotWaitingScreen(code);
            GFX_blitButtonGroup((char*[]){ "B","CANCEL", NULL }, 0, screen, 1);
            GFX_flip(screen);
            dirty = 0;
        }

        minarch_hdmimon();
    }

    // Handle cancellation
    if (cancelled) {
        GFX_clear(screen);
        GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, "Cancelling...", COLOR_WHITE);
        int text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
        SDL_FreeSurface(text);
        GFX_flip(screen);
        GFX_setMode(MODE_MENU);

        // Stop host
        switch (type) {
            case LINK_TYPE_NETPLAY: Netplay_stopHost(); break;
            case LINK_TYPE_GBALINK: GBALink_stopHost(); break;
            case LINK_TYPE_GBLINK:  GBLink_stopHost(); break;
        }
        return MENU_CALLBACK_NOP;
    }

    // Handle connection success - show confirmation screen
    if (connected) {
        uint32_t start_time = SDL_GetTicks();
        while (SDL_GetTicks() - start_time < 3000) {
            GFX_startFrame();
            PAD_poll();
            if (PAD_justPressed(BTN_A)) break;

            GFX_clear(screen);
            GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

            SDL_Surface* text;
            int text_w;
            int center_x = screen->w / 2;
            int center_y = screen->h / 2;

            text = TTF_RenderUTF8_Blended(font.large, "Connected!", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y - SCALE1(20)});
            SDL_FreeSurface(text);

            text = TTF_RenderUTF8_Blended(font.medium, "Starting game...", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, center_y + SCALE1(20)});
            SDL_FreeSurface(text);

            GFX_flip(screen);
            minarch_hdmimon();
        }
        GFX_setMode(MODE_MENU);

        // Stop UDP broadcast - no longer needed after connection
        switch (type) {
            case LINK_TYPE_NETPLAY: Netplay_stopBroadcast(); break;
            case LINK_TYPE_GBLINK:  GBLink_stopBroadcast(); break;
            default: break;
        }

        // Set force resume flag
        switch (type) {
            case LINK_TYPE_NETPLAY: netplay_force_resume = 1; break;
            case LINK_TYPE_GBALINK: gbalink_force_resume = 1; break;
            case LINK_TYPE_GBLINK:  gblink_force_resume = 1; break;
        }
        return MENU_CALLBACK_EXIT;
    }

    GFX_setMode(MODE_MENU);
    return MENU_CALLBACK_NOP;
}

int hostGameWiFi_common(LinkType type, const char* game_name, uint32_t crc) {
    // Ensure wlan1 is down in case device previously hosted via hotspot
    system("ip link set wlan1 down 2>/dev/null");

    // Check for network connectivity (required for WiFi mode)
    if (!ensureNetworkConnected(type, "hosting")) {
        return MENU_CALLBACK_NOP;
    }

    // Show initial message
    GFX_setMode(MODE_MAIN);
    GFX_clear(screen);
    GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
    {
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, "Starting host...", COLOR_WHITE);
        int text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
        SDL_FreeSurface(text);
    }
    GFX_flip(screen);
    GFX_setMode(MODE_MENU);

    // Start host based on type
    int start_result = -1;
    switch (type) {
        case LINK_TYPE_NETPLAY: start_result = Netplay_startHost(game_name, crc, NULL); break;
        case LINK_TYPE_GBALINK: {
            // Get current link mode to sync with client
            const char* link_mode = minarch_getCoreOptionValue("gpsp_serial");
            start_result = GBALink_startHost(game_name, crc, NULL, link_mode);
            break;
        }
        case LINK_TYPE_GBLINK:  start_result = GBLink_startHost(game_name, crc, NULL); break;
    }

    if (start_result != 0) {
        minarch_menuMessage("Failed to start host.\nCheck WiFi connection.", (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    // Get IP based on type
    const char* ip = NULL;
    switch (type) {
        case LINK_TYPE_NETPLAY: ip = Netplay_getLocalIP(); break;
        case LINK_TYPE_GBALINK: ip = GBALink_getLocalIP(); break;
        case LINK_TYPE_GBLINK:  ip = GBLink_getLocalIP(); break;
    }

    // Wait loop
    int dirty = 1;
    int connected = 0;
    int cancelled = 0;

    GFX_setMode(MODE_MAIN);
    while (1) {
        GFX_startFrame();
        PAD_poll();

        // Check for cancellation
        if (PAD_justPressed(BTN_B)) {
            cancelled = 1;
            break;
        }

        // For GBLink: run a few core frames so gambatte can accept/process the
        // TCP connection; GBLink_isConnected() then polls the kernel socket table.
        if (type == LINK_TYPE_GBLINK) {
            for (int i = 0; i < 5; i++) {
                minarch_forceCoreOptionUpdate();
                if (GBLink_isConnected()) {
                    connected = 1;
                    break;
                }
            }
            if (connected) break;
        }

        // Check connection state for other types
        switch (type) {
            case LINK_TYPE_NETPLAY:
                if (Netplay_getState() == NETPLAY_STATE_SYNCING || Netplay_isConnected()) {
                    connected = 1;
                }
                break;
            case LINK_TYPE_GBALINK:
                if (GBALink_getState() == GBALINK_STATE_CONNECTED) {
                    connected = 1;
                }
                break;
            default:
                break;
        }
        if (connected) break;

        // For GBLink: skip the render delay to poll more frequently
        if (type == LINK_TYPE_GBLINK) {
            dirty = 1;  // Always redraw to keep loop fast
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            renderWiFiWaitingScreen(ip);
            GFX_blitButtonGroup((char*[]){ "B","CANCEL", NULL }, 0, screen, 1);
            GFX_flip(screen);
            dirty = 0;
        }

        minarch_hdmimon();
    }

    // Handle cancellation
    if (cancelled) {
        GFX_clear(screen);
        GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, "Cancelling...", COLOR_WHITE);
        int text_w = text->w;
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){screen->w/2 - text_w/2, screen->h/2});
        SDL_FreeSurface(text);
        GFX_flip(screen);
        GFX_setMode(MODE_MENU);

        // Stop host
        switch (type) {
            case LINK_TYPE_NETPLAY: Netplay_stopHost(); break;
            case LINK_TYPE_GBALINK: GBALink_stopHost(); break;
            case LINK_TYPE_GBLINK:  GBLink_stopHost(); break;
        }
        return MENU_CALLBACK_NOP;
    }

    // Handle connection success - show confirmation screen
    if (connected) {
        showConnectionSuccessScreen();

        // Stop UDP broadcast - no longer needed after connection
        switch (type) {
            case LINK_TYPE_NETPLAY: Netplay_stopBroadcast(); break;
            case LINK_TYPE_GBLINK:  GBLink_stopBroadcast(); break;
            default: break;
        }

        // Set force resume flag
        switch (type) {
            case LINK_TYPE_NETPLAY: netplay_force_resume = 1; break;
            case LINK_TYPE_GBALINK: gbalink_force_resume = 1; break;
            case LINK_TYPE_GBLINK:  gblink_force_resume = 1; break;
        }
        return MENU_CALLBACK_EXIT;
    }

    GFX_setMode(MODE_MENU);
    return MENU_CALLBACK_NOP;
}

int joinGameWiFi_common(LinkType type) {
    // Ensure wlan1 is down in case device previously hosted via hotspot
    system("ip link set wlan1 down 2>/dev/null");

    // Check for network connectivity (required for WiFi mode)
    if (!ensureNetworkConnected(type, "joining")) {
        return MENU_CALLBACK_NOP;
    }

    // Start discovery based on type
    int start_result = -1;
    switch (type) {
        case LINK_TYPE_NETPLAY: start_result = Netplay_startDiscovery(); break;
        case LINK_TYPE_GBALINK: start_result = GBALink_startDiscovery(); break;
        case LINK_TYPE_GBLINK:  start_result = GBLink_startDiscovery(); break;
    }
    if (start_result != 0) {
        minarch_menuMessage("Failed to start discovery.\nCheck WiFi connection.", (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    int dirty = 1;
    int cancelled = 0;
    uint32_t last_poll = SDL_GetTicks();
    setHostCount(type, 0);

    // Show scanning message with dark overlay
    GFX_setMode(MODE_MAIN);
    showSearchingScreen();

    // Auto-refresh discovery loop - wait for hosts
    while (1) {
        GFX_startFrame();
        PAD_poll();

        // Check for cancellation
        if (PAD_justPressed(BTN_B)) {
            cancelled = 1;
            break;
        }

        // Poll for hosts periodically (every 500ms)
        uint32_t now = SDL_GetTicks();
        if (now - last_poll >= 500) {
            last_poll = now;
            int new_count = 0;
            switch (type) {
                case LINK_TYPE_NETPLAY: new_count = Netplay_getDiscoveredHosts(netplay_hosts, NETPLAY_MAX_HOSTS); break;
                case LINK_TYPE_GBALINK: new_count = GBALink_getDiscoveredHosts(gbalink_hosts, GBALINK_MAX_HOSTS); break;
                case LINK_TYPE_GBLINK:  new_count = GBLink_getDiscoveredHosts(gblink_hosts, GBLINK_MAX_HOSTS); break;
            }
            if (new_count != getHostCount(type)) {
                setHostCount(type, new_count);
                dirty = 1;
            }

            // Found hosts - exit loop
            if (getHostCount(type) > 0) {
                break;
            }
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        // Keep showing scanning message
        if (dirty) {
            showSearchingScreen();
            dirty = 0;
        }

        minarch_hdmimon();
    }
    GFX_setMode(MODE_MENU);

    if (cancelled || getHostCount(type) == 0) {
        // Stop discovery before returning
        switch (type) {
            case LINK_TYPE_NETPLAY: Netplay_stopDiscovery(); break;
            case LINK_TYPE_GBALINK: GBALink_stopDiscovery(); break;
            case LINK_TYPE_GBLINK:  GBLink_stopDiscovery(); break;
        }
        if (!cancelled) {
            minarch_menuMessage("No hosts found.\n\nMake sure:\n1. Both devices on same WiFi\n2. Host started first",
                         (char*[]){ "A","OKAY", NULL });
        }
        return MENU_CALLBACK_NOP;
    }

    // Show host selection with pills (continue polling for new hosts)
    int selected = 0;
    dirty = 1;
    last_poll = SDL_GetTicks();

    while (1) {
        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_B)) {
            // Stop discovery before returning
            switch (type) {
                case LINK_TYPE_NETPLAY: Netplay_stopDiscovery(); break;
                case LINK_TYPE_GBALINK: GBALink_stopDiscovery(); break;
                case LINK_TYPE_GBLINK:  GBLink_stopDiscovery(); break;
            }
            return MENU_CALLBACK_NOP;
        }

        if (PAD_justRepeated(BTN_UP)) {
            selected--;
            if (selected < 0) selected = getHostCount(type) - 1;
            dirty = 1;
        }
        else if (PAD_justRepeated(BTN_DOWN)) {
            selected++;
            if (selected >= getHostCount(type)) selected = 0;
            dirty = 1;
        }
        else if (PAD_justPressed(BTN_A)) {
            // Host selected - proceed to connect
            break;
        }

        // Continue polling for new hosts in the background
        uint32_t now = SDL_GetTicks();
        if (now - last_poll >= 500) {
            last_poll = now;
            int new_count = 0;
            switch (type) {
                case LINK_TYPE_NETPLAY: new_count = Netplay_getDiscoveredHosts(netplay_hosts, NETPLAY_MAX_HOSTS); break;
                case LINK_TYPE_GBALINK: new_count = GBALink_getDiscoveredHosts(gbalink_hosts, GBALINK_MAX_HOSTS); break;
                case LINK_TYPE_GBLINK:  new_count = GBLink_getDiscoveredHosts(gblink_hosts, GBLINK_MAX_HOSTS); break;
            }
            if (new_count != getHostCount(type)) {
                setHostCount(type, new_count);
                if (selected >= getHostCount(type)) selected = getHostCount(type) - 1;
                dirty = 1;
            }
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            renderHostSelectionList(type, selected, getHostCount(type));
            dirty = 0;
        }

        minarch_hdmimon();
    }

    // Stop discovery AFTER selection is made
    switch (type) {
        case LINK_TYPE_NETPLAY: Netplay_stopDiscovery(); break;
        case LINK_TYPE_GBALINK: GBALink_stopDiscovery(); break;
        case LINK_TYPE_GBLINK:  GBLink_stopDiscovery(); break;
    }

    // For GBALink, check link mode compatibility BEFORE establishing TCP connection
    // This allows showing the sync dialog without making a network connection
    if (type == LINK_TYPE_GBALINK) {
        const char* host_mode = getHostLinkMode(type, selected);
        const char* client_mode = minarch_getCoreOptionValue("gpsp_serial");

        // Check if modes differ (need reload for gpsp to pick up new mode)
        if (host_mode && host_mode[0] && (!client_mode || strcmp(client_mode, host_mode) != 0)) {
            if (showLinkModeRestartDialog(getGBALinkModeName(host_mode), false)) {
                // User confirmed - apply mode, save config, reload (no TCP connection was made)
                minarch_setCoreOptionValue("gpsp_serial", host_mode);
                minarch_saveConfig();
                minarch_reloadGame();  // Deferred to avoid segfault
                gbalink_force_resume = 1;
                return MENU_CALLBACK_EXIT;
            } else {
                // User cancelled - return without connecting
                return MENU_CALLBACK_NOP;
            }
        }
    }

    // Show connecting screen
    const char* host_ip = getHostIP(type, selected);
    int host_port = getHostPort(type, selected);
    showConnectingScreen(host_ip);

    // Connect to selected host (modes already match for GBALink)
    int connect_result = -1;
    switch (type) {
        case LINK_TYPE_NETPLAY: connect_result = Netplay_connectToHost(host_ip, host_port); break;
        case LINK_TYPE_GBALINK: connect_result = GBALink_connectToHost(host_ip, host_port); break;
        case LINK_TYPE_GBLINK:  connect_result = GBLink_connectToHost(host_ip, host_port); break;
    }

    if (connect_result == GBALINK_CONNECT_ERROR) {
        minarch_menuMessage("Connection failed.", (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    // Handle GBALink needing reload for link mode sync (fallback for protocol changes)
    // This path is kept as a safety fallback in case host doesn't broadcast link_mode
    if (type == LINK_TYPE_GBALINK && connect_result == GBALINK_CONNECT_NEEDS_RELOAD) {
        const char* host_mode = GBALink_getPendingLinkMode();

        if (showLinkModeRestartDialog(getGBALinkModeName(host_mode), false)) {
            // User confirmed - apply mode, save config, disconnect, reload, return to game
            GBALink_applyPendingLinkMode();
            minarch_saveConfig();
            GBALink_disconnect();
            minarch_reloadGame();  // Deferred to avoid segfault
            // Set force_resume to close all menus
            gbalink_force_resume = 1;
            return MENU_CALLBACK_EXIT;
        } else {
            // User cancelled - clear state and disconnect
            GBALink_clearPendingReload();
            GBALink_disconnect();
            return MENU_CALLBACK_NOP;
        }
    }

    // Show success screen
    showConnectionSuccessScreen();

    // Set force resume flag
    switch (type) {
        case LINK_TYPE_NETPLAY: netplay_force_resume = 1; break;
        case LINK_TYPE_GBALINK: gbalink_force_resume = 1; break;
        case LINK_TYPE_GBLINK:  gblink_force_resume = 1; break;
    }
    return MENU_CALLBACK_EXIT;
}

int joinGame_Hotspot_common(LinkType type) {
    // Note: ensureWifiEnabled() already called by joinGame_common()

    // Link-type specific setup
    const char* display_name = "Netplay";
    int* connected_to_hotspot_flag = NULL;
    int* force_resume_flag = NULL;
    int default_port = 0;

    switch (type) {
        case LINK_TYPE_NETPLAY:
            connected_to_hotspot_flag = &netplay_connected_to_hotspot;
            force_resume_flag = &netplay_force_resume;
            default_port = NETPLAY_DEFAULT_PORT;
            break;
        case LINK_TYPE_GBALINK:
            connected_to_hotspot_flag = &gbalink_connected_to_hotspot;
            force_resume_flag = &gbalink_force_resume;
            default_port = GBALINK_DEFAULT_PORT;
            break;
        case LINK_TYPE_GBLINK:
            connected_to_hotspot_flag = &gblink_connected_to_hotspot;
            force_resume_flag = &gblink_force_resume;
            default_port = GBLINK_DEFAULT_PORT;
            break;
    }

    *connected_to_hotspot_flag = 0;

    // Purge stale hotspot networks left by previous sessions before joining a new
    // one. Done up front (not only on teardown) so a backlog from aborted joins
    // can't grow unbounded or let wpa_supplicant prefer an old saved hotspot.
    WIFI_direct_forgetAllHotspots();

    // Show scanning message
    char scan_msg[64];
    snprintf(scan_msg, sizeof(scan_msg), "Scanning for %s hosts...", display_name);
    showOverlayMessage(scan_msg);

    // Scan for hotspots (all types use unified prefix)
    char hotspots[8][33];
    memset(hotspots, 0, sizeof(hotspots));  // Zero-initialize to prevent garbage

    // Use wifi_direct for more reliable scanning (bypasses wifi_daemon)
    int hotspot_count = WIFI_direct_scanForHotspots(LINK_HOTSPOT_SSID_PREFIX, hotspots, 8);

    if (hotspot_count == 0) {
        char no_host_msg[128];
        snprintf(no_host_msg, sizeof(no_host_msg),
            "No %s host found.\n\nMake sure the host has\nstarted %s first.",
            display_name, type == LINK_TYPE_NETPLAY ? "hosting" : "a link session");
        minarch_menuMessage(no_host_msg, (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    // Show list of available hosts for user to select
    char selected_ssid[33];
    int selected = 0;
    int show_selection = 1;
    int dirty = 1;
    size_t prefix_len = strlen(LINK_HOTSPOT_SSID_PREFIX);

    while (show_selection) {
        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_B)) {
            return MENU_CALLBACK_NOP;  // Cancel
        }

        if (PAD_justRepeated(BTN_UP)) {
            selected--;
            if (selected < 0) selected = hotspot_count - 1;
            dirty = 1;
        }
        else if (PAD_justRepeated(BTN_DOWN)) {
            selected++;
            if (selected >= hotspot_count) selected = 0;
            dirty = 1;
        }
        else if (PAD_justPressed(BTN_A)) {
            strncpy(selected_ssid, hotspots[selected], sizeof(selected_ssid) - 1);
            selected_ssid[32] = '\0';
            show_selection = 0;
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            GFX_clear(screen);
            GFX_drawOnLayer(menu.bitmap, 0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, 0.15f, 1, 0);

            SDL_Surface* text;
            int text_w;
            int center_x = screen->w / 2;

            // Calculate vertical layout
            int title_y = SCALE1(60);
            int instruction_y = title_y + SCALE1(30);
            int list_start_y = instruction_y + SCALE1(35);

            // Large title "Join Game"
            text = TTF_RenderUTF8_Blended(font.large, "Join Game", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, title_y});
            SDL_FreeSurface(text);

            // Medium instruction
            text = TTF_RenderUTF8_Blended(font.medium, "Select code displayed on the host device", COLOR_WHITE);
            text_w = text->w;
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, instruction_y});
            SDL_FreeSurface(text);

            // Render code list with pills
            for (int j = 0; j < hotspot_count; j++) {
                const char* code = (strlen(hotspots[j]) > prefix_len) ? hotspots[j] + prefix_len : "????";
                const char* display_code = code[0] ? code : "????";

                SDL_Color text_color = COLOR_WHITE;
                if (j == selected) {
                    text_color = uintToColour(THEME_COLOR5_255);
                    int ow;
                    TTF_SizeUTF8(font.large, display_code, &ow, NULL);
                    ow += SCALE1(BUTTON_PADDING * 2);
                    GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
                        center_x - ow/2,
                        list_start_y + j * SCALE1(PILL_SIZE),
                        ow,
                        SCALE1(PILL_SIZE)
                    });
                }

                text = TTF_RenderUTF8_Blended(font.large, display_code, text_color);
                text_w = text->w;
                SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){center_x - text_w/2, list_start_y + j * SCALE1(PILL_SIZE) + SCALE1(4)});
                SDL_FreeSurface(text);
            }

            GFX_blitButtonGroup((char*[]){ "B","BACK", "A","SELECT", NULL }, 1, screen, 1);
            GFX_flip(screen);
            dirty = 0;
        }

        minarch_hdmimon();
    }

    // Connect to selected hotspot
    const char* selected_code = (strlen(selected_ssid) > prefix_len) ? selected_ssid + prefix_len : "????";
    char connect_msg[64];
    snprintf(connect_msg, sizeof(connect_msg), "Connecting to %s...", selected_code[0] ? selected_code : "????");
    showOverlayMessage(connect_msg);

    // Save current connection before switching to hotspot (so we can restore later)
    WIFI_direct_saveCurrentConnection();

    // IMPORTANT: Ensure wlan1 is completely down before joining another hotspot
    // This fixes an issue where a device that previously hosted still has wlan1
    // up with 10.0.0.1, causing routing conflicts when trying to join a new hotspot
    system("killall hostapd 2>/dev/null");
    system("killall udhcpd 2>/dev/null");
    system("ip addr flush dev wlan1 2>/dev/null");
    system("ip link set wlan1 down 2>/dev/null");

    // Disconnect from current WiFi first to ensure clean switch to hotspot
    WIFI_direct_disconnect();
    // Flush any stale network state (old routes, IP addresses)
    system("ip addr flush dev wlan0 2>/dev/null");
    system("ip route flush dev wlan0 2>/dev/null");
    SDL_Delay(1000);  // Wait for cleanup to complete

    // Use wifi_direct for more reliable connection (bypasses wifi_daemon)
    const char* hotspot_pass = WIFI_direct_getHotspotPassword();
    int wifi_connect_result = WIFI_direct_connect(selected_ssid, hotspot_pass);

    if (wifi_connect_result != 0) {
        WIFI_direct_restorePreviousConnection();  // Restore WiFi so next scan works
        minarch_menuMessage("Failed to connect to host.", (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    *connected_to_hotspot_flag = 1;

    // Store the hotspot SSID for cleanup
    strncpy(connected_hotspot_ssid, selected_ssid, 32);
    connected_hotspot_ssid[32] = '\0';

    // GBLink needs IP refresh after connecting to hotspot
    if (type == LINK_TYPE_GBLINK) {
        SDL_Delay(500);
        GBLink_hasNetworkConnection();  // Refreshes IP as side effect
    }

    // On hotspot network, host is always at fixed IP
    const char* host_ip = WIFI_direct_getHotspotIP();

    // For GBALink, query host's link_mode directly BEFORE TCP connection
    // This allows showing the sync dialog without making a TCP connection
    if (type == LINK_TYPE_GBALINK) {
        showOverlayMessage("Checking compatibility...");

        // Direct UDP query to known host IP (more reliable than broadcasts on hotspot)
        char host_mode[32] = {0};
        int query_result = GBALink_queryHostLinkMode(host_ip, host_mode, sizeof(host_mode));

        if (query_result == 0 && host_mode[0]) {
            const char* client_mode = minarch_getCoreOptionValue("gpsp_serial");

            // Check if modes differ
            if (!client_mode || strcmp(client_mode, host_mode) != 0) {
                if (showLinkModeRestartDialog(getGBALinkModeName(host_mode), false)) {
                    // User confirmed - apply mode, save config, disconnect from hotspot, reload
                    minarch_setCoreOptionValue("gpsp_serial", host_mode);
                    minarch_saveConfig();
                    WIFI_direct_restorePreviousConnection();
                    *connected_to_hotspot_flag = 0;
                    minarch_reloadGame();  // Deferred to avoid segfault
                    gbalink_force_resume = 1;
                    return MENU_CALLBACK_EXIT;
                } else {
                    // User cancelled - disconnect from hotspot
                    WIFI_direct_restorePreviousConnection();
                    *connected_to_hotspot_flag = 0;
                    return MENU_CALLBACK_NOP;
                }
            }
        }
    }

    // Verify client has valid IP before attempting TCP connect
    char client_ip[16] = {0};
    WIFI_direct_getIP(client_ip, sizeof(client_ip));
    if (client_ip[0] == '\0' || strcmp(client_ip, "0.0.0.0") == 0) {
        showOverlayMessage("Waiting for network...");
        // Wait up to 10 seconds for DHCP to assign IP
        for (int i = 0; i < 20; i++) {
            SDL_Delay(500);
            WIFI_direct_getIP(client_ip, sizeof(client_ip));
            if (client_ip[0] != '\0' && strcmp(client_ip, "0.0.0.0") != 0) {
                break;
            }
        }
        if (client_ip[0] == '\0' || strcmp(client_ip, "0.0.0.0") == 0) {
            minarch_menuMessage("Failed to get IP address.\n\nPlease try again.",
                               (char*[]){ "A","OKAY", NULL });
            WIFI_direct_restorePreviousConnection();
            *connected_to_hotspot_flag = 0;
            return MENU_CALLBACK_NOP;
        }
    }

    showOverlayMessage("Establishing link...");

    // Network warmup: ping host to populate ARP cache and verify connectivity
    // This helps with intermittent connection failures on hotspot
    {
        char ping_cmd[64];
        snprintf(ping_cmd, sizeof(ping_cmd), "ping -c 1 -W 2 %s >/dev/null 2>&1", host_ip);
        int ping_result = system(ping_cmd);
        if (ping_result != 0) {
            // First ping failed, wait and retry - ARP may need time
            SDL_Delay(500);
            system(ping_cmd);
        }
        // Even if ping fails, still try TCP - the ping may be blocked but TCP allowed
    }

    // Connect to host with retry logic (modes already match for GBALink if we got here)
    int connect_result = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            char retry_msg[64];
            snprintf(retry_msg, sizeof(retry_msg), "Retrying connection... (%d/3)", attempt + 1);
            showOverlayMessage(retry_msg);
            SDL_Delay(1500);  // Increased delay before retry for network stabilization
        }

        switch (type) {
            case LINK_TYPE_NETPLAY:
                connect_result = Netplay_connectToHost(host_ip, default_port);
                break;
            case LINK_TYPE_GBALINK:
                connect_result = GBALink_connectToHost(host_ip, default_port);
                break;
            case LINK_TYPE_GBLINK:
                connect_result = GBLink_connectToHost(host_ip, default_port);
                break;
        }

        // Success or needs reload - stop retrying
        if (connect_result == 0 || connect_result == GBALINK_CONNECT_NEEDS_RELOAD) {
            break;
        }
    }

    if (connect_result == GBALINK_CONNECT_ERROR || (connect_result != 0 && connect_result != GBALINK_CONNECT_NEEDS_RELOAD)) {
        minarch_menuMessage("Failed to connect to host.\n\nConnection timed out.", (char*[]){ "A","OKAY", NULL });
        WIFI_direct_restorePreviousConnection();
        *connected_to_hotspot_flag = 0;
        return MENU_CALLBACK_NOP;
    }

    // Handle GBALink needing reload for link mode sync (fallback for protocol changes)
    // This path is kept as a safety fallback in case UDP discovery didn't get link_mode
    if (type == LINK_TYPE_GBALINK && connect_result == GBALINK_CONNECT_NEEDS_RELOAD) {
        const char* host_mode = GBALink_getPendingLinkMode();

        if (showLinkModeRestartDialog(getGBALinkModeName(host_mode), false)) {
            // User confirmed - apply mode, save config, disconnect, reload, return to game
            GBALink_applyPendingLinkMode();
            minarch_saveConfig();
            GBALink_disconnect();
            minarch_reloadGame();  // Deferred to avoid segfault
            // Set force_resume to close all menus
            gbalink_force_resume = 1;
            return MENU_CALLBACK_EXIT;
        } else {
            // User cancelled - clear state and disconnect
            GBALink_clearPendingReload();
            GBALink_disconnect();
            WIFI_direct_restorePreviousConnection();
            *connected_to_hotspot_flag = 0;
            return MENU_CALLBACK_NOP;
        }
    }

    // Show success
    showConnectedSuccess(type == LINK_TYPE_GBLINK ? 2000 : 3000);

    *force_resume_flag = 1;
    return MENU_CALLBACK_EXIT;
}

int joinGame_common(LinkType type, void* list, int i) {
    (void)list; (void)i;

    // Check if already connected
    bool connected = false;
    const char* session_name = "Netplay";
    switch (type) {
        case LINK_TYPE_NETPLAY: connected = Netplay_getMode() != NETPLAY_OFF; break;
        case LINK_TYPE_GBALINK: connected = GBALink_getMode() != GBALINK_OFF; break;
        case LINK_TYPE_GBLINK:  connected = GBLink_getMode() != GBLINK_OFF; break;
    }

    if (connected) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Already in %s session.\nDisconnect first.", session_name);
        minarch_menuMessage(msg, (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    if (!ensureWifiEnabled()) {
        return MENU_CALLBACK_NOP;
    }

    // Auto-configure link cable mode for GBA games (gpSP auto-detects via gba_over.h)
    if (type == LINK_TYPE_GBALINK) {
        if (forceGBALinkAutoNeedsReload()) return MENU_CALLBACK_EXIT;
    }

    int selected = Menu_selectConnectionMode("Join Game");
    if (selected < 0) return MENU_CALLBACK_NOP;

    if (selected == 0) {
        return joinGame_Hotspot_common(type);
    }

    // WiFi mode
    return joinGameWiFi_common(type);
}

int disconnect_common(LinkType type, void* list, int i) {
    (void)list; (void)i;

    // Check if not connected
    bool disconnected = false;
    const char* session_name = "Netplay";
    switch (type) {
        case LINK_TYPE_NETPLAY: disconnected = Netplay_getMode() == NETPLAY_OFF; break;
        case LINK_TYPE_GBALINK: disconnected = GBALink_getMode() == GBALINK_OFF; break;
        case LINK_TYPE_GBLINK:  disconnected = GBLink_getMode() == GBLINK_OFF; break;
    }

    if (disconnected) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Not in a %s session.", session_name);
        minarch_menuMessage(msg, (char*[]){ "A","OKAY", NULL });
        return MENU_CALLBACK_NOP;
    }

    showTransitionMessage("Disconnecting...");

    // Capture hotspot state, disconnect, and stop host in one switch
    bool was_host = false;
    bool needs_hotspot_cleanup = false;

    switch (type) {
        case LINK_TYPE_NETPLAY:
            was_host = (Netplay_getMode() == NETPLAY_HOST);
            needs_hotspot_cleanup = Netplay_isUsingHotspot() || netplay_connected_to_hotspot;
            Netplay_disconnect();
            if (was_host) Netplay_stopHostFast();
            netplay_connected_to_hotspot = 0;
            break;
        case LINK_TYPE_GBALINK:
            was_host = (GBALink_getMode() == GBALINK_HOST);
            needs_hotspot_cleanup = GBALink_isUsingHotspot() || gbalink_connected_to_hotspot;
            GBALink_disconnect();
            if (was_host) GBALink_stopHostFast();
            gbalink_connected_to_hotspot = 0;
            break;
        case LINK_TYPE_GBLINK:
            was_host = (GBLink_getMode() == GBLINK_HOST);
            needs_hotspot_cleanup = GBLink_isUsingHotspot() || gblink_connected_to_hotspot;
            GBLink_stopAllFast();
            gblink_connected_to_hotspot = 0;
            break;
    }

    // Do hotspot cleanup asynchronously to avoid 5-10 second delay
    if (needs_hotspot_cleanup) {
        stopHotspotAndRestoreWiFiAsync(was_host);
    }
#ifdef HAS_WIFIMG
    else {
        // WiFi-mode session (no hotspot to tear down): a WIFI_direct_connect that
        // switched networks may have left other saved networks disabled via
        // select_network. Re-enable them now so they're usable again immediately
        // instead of only after the next reboot/WiFi toggle.
        WIFI_enableAll();
    }
#endif

    showTimedConfirmation("Disconnected", 1500);
    return MENU_CALLBACK_NOP;
}

//////////////////////////////////////////////////////////////////////////////
// Status & Menu Functions
//////////////////////////////////////////////////////////////////////////////

int status_common(LinkType type) {
    const char* mode_str = "Off";
    const char* state_str = "Idle";
    const char* conn_str = "";
    const char* code = NULL;
    const char* local_ip = NULL;
    const char* status_msg = NULL;

    // Type-specific variables
    int mode_off = 0;
    int mode_host = 0;
    int is_using_hotspot = 0;
    int connected_to_hotspot = 0;

    switch (type) {
        case LINK_TYPE_NETPLAY:
            mode_off = (Netplay_getMode() == NETPLAY_OFF);
            mode_host = (Netplay_getMode() == NETPLAY_HOST);
            is_using_hotspot = Netplay_isUsingHotspot();
            connected_to_hotspot = netplay_connected_to_hotspot;
            local_ip = Netplay_getLocalIP();
            status_msg = Netplay_getStatusMessage();

            switch (Netplay_getMode()) {
                case NETPLAY_HOST: mode_str = "Host"; break;
                case NETPLAY_CLIENT: mode_str = "Client"; break;
                default: mode_str = "Off"; break;
            }
            switch (Netplay_getState()) {
                case NETPLAY_STATE_WAITING: state_str = "Waiting for player"; break;
                case NETPLAY_STATE_CONNECTING: state_str = "Connecting"; break;
                case NETPLAY_STATE_SYNCING: state_str = "Connected"; break;
                case NETPLAY_STATE_PLAYING: state_str = "Playing"; break;
                case NETPLAY_STATE_STALLED: state_str = "Playing (stalled)"; break;
                case NETPLAY_STATE_DISCONNECTED: state_str = "Disconnected"; break;
                case NETPLAY_STATE_ERROR: state_str = "Error"; break;
                default: state_str = "Idle"; break;
            }
            break;

        case LINK_TYPE_GBALINK:
            mode_off = (GBALink_getMode() == GBALINK_OFF);
            mode_host = (GBALink_getMode() == GBALINK_HOST);
            is_using_hotspot = GBALink_isUsingHotspot();
            connected_to_hotspot = gbalink_connected_to_hotspot;
            local_ip = GBALink_getLocalIP();
            status_msg = GBALink_getStatusMessage();

            switch (GBALink_getMode()) {
                case GBALINK_HOST: mode_str = "Host"; break;
                case GBALINK_CLIENT: mode_str = "Client"; break;
                default: mode_str = "Off"; break;
            }
            switch (GBALink_getState()) {
                case GBALINK_STATE_WAITING: state_str = "Waiting for link"; break;
                case GBALINK_STATE_CONNECTING: state_str = "Connecting"; break;
                case GBALINK_STATE_CONNECTED: state_str = "Connected"; break;
                case GBALINK_STATE_DISCONNECTED: state_str = "Disconnected"; break;
                case GBALINK_STATE_ERROR: state_str = "Error"; break;
                default: state_str = "Idle"; break;
            }
            break;

        case LINK_TYPE_GBLINK:
            mode_off = (GBLink_getMode() == GBLINK_OFF);
            mode_host = (GBLink_getMode() == GBLINK_HOST);
            is_using_hotspot = GBLink_isUsingHotspot();
            connected_to_hotspot = gblink_connected_to_hotspot;
            local_ip = GBLink_getLocalIP();
            status_msg = GBLink_getStatusMessage();

            switch (GBLink_getMode()) {
                case GBLINK_HOST: mode_str = "Host"; break;
                case GBLINK_CLIENT: mode_str = "Client"; break;
                default: mode_str = "Off"; break;
            }
            switch (GBLink_getState()) {
                case GBLINK_STATE_WAITING: state_str = "Waiting for link"; break;
                case GBLINK_STATE_CONNECTING: state_str = "Connecting"; break;
                case GBLINK_STATE_CONNECTED: state_str = "Connected"; break;
                case GBLINK_STATE_DISCONNECTED: state_str = "Disconnected"; break;
                case GBLINK_STATE_ERROR: state_str = "Error"; break;
                default: state_str = "Idle"; break;
            }
            break;
    }

    // Connection type string
    if (!mode_off) {
        if (is_using_hotspot || connected_to_hotspot) {
            conn_str = "Hotspot";
        } else {
            conn_str = "WiFi";
        }
    }

    // Get hotspot code if hosting with hotspot
    if (is_using_hotspot && mode_host) {
        const char* ssid = NULL;
#ifdef HAS_WIFIMG
        ssid = WIFI_direct_getHotspotSSID();
#endif
        size_t ssid_len = ssid ? strlen(ssid) : 0;
        size_t prefix_len = strlen(LINK_HOTSPOT_SSID_PREFIX);
        code = (ssid && ssid_len > prefix_len) ? ssid + prefix_len : "????";
    }

    showLinkStatusMessage("Netplay Status", mode_str, conn_str, state_str, code, local_ip, status_msg);
    return MENU_CALLBACK_NOP;
}

// Menu option handlers
int OptionNetplay_hostGame(void* list, int i) {
    return hostGame_common(LINK_TYPE_NETPLAY, list, i);
}

int OptionNetplay_joinGame(void* list, int i) {
    return joinGame_common(LINK_TYPE_NETPLAY, list, i);
}

int OptionNetplay_disconnect(void* list, int i) {
    return disconnect_common(LINK_TYPE_NETPLAY, list, i);
}

int OptionNetplay_status(void* list, int i) {
    (void)list; (void)i;
    return status_common(LINK_TYPE_NETPLAY);
}

int OptionGBALink_hostGame(void* list, int i) {
    return hostGame_common(LINK_TYPE_GBALINK, list, i);
}

int OptionGBALink_joinGame(void* list, int i) {
    return joinGame_common(LINK_TYPE_GBALINK, list, i);
}

int OptionGBALink_disconnect(void* list, int i) {
    return disconnect_common(LINK_TYPE_GBALINK, list, i);
}

int OptionGBALink_status(void* list, int i) {
    (void)list; (void)i;
    return status_common(LINK_TYPE_GBALINK);
}

int OptionGBLink_hostGame(void* list, int i) {
    return hostGame_common(LINK_TYPE_GBLINK, list, i);
}

int OptionGBLink_joinGame(void* list, int i) {
    return joinGame_common(LINK_TYPE_GBLINK, list, i);
}

int OptionGBLink_disconnect(void* list, int i) {
    return disconnect_common(LINK_TYPE_GBLINK, list, i);
}

int OptionGBLink_status(void* list, int i) {
    (void)list; (void)i;
    return status_common(LINK_TYPE_GBLINK);
}

const char* getNetplayMenuHint(void) {
    // GBA link adapter mode is now auto-detected by gpSP (gpsp_serial="auto"),
    // so there is no manual setting to hint about.
    return NULL;
}

const char* (*getLinkMenuHint(LinkType type))(void) {
    switch (type) {
        case LINK_TYPE_NETPLAY: return getNetplayMenuHint;
        case LINK_TYPE_GBALINK: return getNetplayMenuHint;
        case LINK_TYPE_GBLINK:  return NULL;
    }
    return NULL;
}

// Internal callback types for menu
typedef int (*NetplayMenuCallback)(void*, int);

typedef struct {
    NetplayMenuCallback host;
    NetplayMenuCallback join;
    NetplayMenuCallback disconnect;
    NetplayMenuCallback status;
} NetplayLinkCallbacks;

static NetplayLinkCallbacks getNetplayLinkCallbacks(LinkType type) {
    switch (type) {
        case LINK_TYPE_NETPLAY:
            return (NetplayLinkCallbacks){OptionNetplay_hostGame, OptionNetplay_joinGame,
                                          OptionNetplay_disconnect, OptionNetplay_status};
        case LINK_TYPE_GBALINK:
            return (NetplayLinkCallbacks){OptionGBALink_hostGame, OptionGBALink_joinGame,
                                          OptionGBALink_disconnect, OptionGBALink_status};
        case LINK_TYPE_GBLINK:
            return (NetplayLinkCallbacks){OptionGBLink_hostGame, OptionGBLink_joinGame,
                                          OptionGBLink_disconnect, OptionGBLink_status};
    }
    return (NetplayLinkCallbacks){NULL, NULL, NULL, NULL};
}

int Netplay_menu_link(LinkType type) {
    int* force_resume = getForceResumeFlag(type);
    *force_resume = 0;

    NetplayLinkCallbacks callbacks = getNetplayLinkCallbacks(type);
    const char* (*getHint)(void) = getLinkMenuHint(type);

    int dirty = 1;
    int show_menu = 1;
    int selected = 0;

    while (show_menu) {
        int is_connected = isLinkConnected(type);

        char* items[5];
        NetplayMenuCallback item_callbacks[5];
        int item_count = 0;

        if (!is_connected) {
            items[item_count] = "Host Game";
            item_callbacks[item_count] = callbacks.host;
            item_count++;
            items[item_count] = "Join Game";
            item_callbacks[item_count] = callbacks.join;
            item_count++;
        } else {
            items[item_count] = "Disconnect";
            item_callbacks[item_count] = callbacks.disconnect;
            item_count++;
        }
        items[item_count] = "Status";
        item_callbacks[item_count] = callbacks.status;
        item_count++;

        if (selected >= item_count) selected = item_count - 1;

        GFX_startFrame();
        PAD_poll();

        if (PAD_justRepeated(BTN_UP)) {
            selected--;
            if (selected < 0) selected = item_count - 1;
            dirty = 1;
        }
        else if (PAD_justRepeated(BTN_DOWN)) {
            selected++;
            if (selected >= item_count) selected = 0;
            dirty = 1;
        }
        else if (PAD_justPressed(BTN_B)) {
            show_menu = 0;
        }
        else if (PAD_justPressed(BTN_A)) {
            int result = item_callbacks[selected](NULL, selected);
            if (result == MENU_CALLBACK_EXIT || *force_resume) {
                show_menu = 0;
            }
            dirty = 1;
        }

        if (*force_resume) {
            show_menu = 0;
        }

        PWR_update(&dirty, NULL, minarch_beforeSleep, minarch_afterSleep);

        if (dirty) {
            renderLinkMenuUI("Netplay", items, item_count, selected, getHint);
            dirty = 0;
        }

        minarch_hdmimon();
    }

    return *force_resume;
}

//////////////////////////////////////////////////////////////////////////////
// Link Cleanup
//////////////////////////////////////////////////////////////////////////////

void Netplay_quitAll(void) {
    GBLink_quit();
    GBALink_quit();
    Netplay_quit();
}
