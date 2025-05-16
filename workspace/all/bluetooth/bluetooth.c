#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>  // For 'bool' type

#include <pthread.h>
#include <fcntl.h>
//#include <bluetooth/bluetooth.h>
//#include <bluetooth/hci.h>
//#include <bluetooth/hci_lib.h>
//#include <bluetooth/rfcomm.h> 
//#include <sys/ioctl.h>       // Required for ioctl()
//#define LOG_INFO(fmt, ...) printf(fmt, ##__VA_ARGS__)

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "msettings.h"

#define MAX_DEVICES 10  // Maximum number of Bluetooth devices to store

#define MAX_DEVICES 10

// Structure to hold device info
typedef struct {
    char name[248];
    char address[18];
    int connected;
    int paired;
} BluetoothDevice;

// Array to store discovered Bluetooth devices
BluetoothDevice devices[MAX_DEVICES];
int num_devices = 0;
static SDL_Surface *screen;
static bool quit = false;

void pair_device(const char *device_address) {
    char command[256];

    printf("Pairing with device %s...\n", device_address);
    

    // this needs to go in one line otherwise it will not be registerd under the agent and not reconnect automaticaly
    snprintf(command, sizeof(command), "echo -e \"agent on\ndefault-agent\npair %s\ntrust %s\n\" | bluetoothctl",device_address);

    int result = system(command);
    snprintf(command, sizeof(command), "bluetoothctl connect %s",device_address);

    result = system(command);

    if (result == 0) {
        printf("Device %s paired successfully!\n", device_address);
    } else {
        printf("Failed to pair with device %s\n", device_address);
    }

}
void remove_device(const char *device_address) {
    char command[256];

    printf("Pairing with device %s...\n", device_address);
    

    // this needs to go in one line otherwise it will not be registerd under the agent and not reconnect automaticaly
    snprintf(command, sizeof(command), "echo -e \"remove %s\n\" | bluetoothctl",device_address);

    int result = system(command);

    if (result == 0) {
        printf("Device %s remove successfully!\n", device_address);
    } else {
        printf("Failed to remove device %s\n", device_address);
    }

}

// Function to scan for Bluetooth devices
void scan_bluetooth_devices() {
    FILE *fp;
    char path[1035];
    num_devices = 0;
    LOG_info("test test\n");
    // Run bluetoothctl to list paired devices
    fp = popen("bluetoothctl devices", "r");
    if (fp == NULL) {
        perror("Failed to run bluetoothctl devices");
        return;
    }
LOG_info("hiii test\n");
    // Process paired devices
    printf("all devices:\n");
    while (fgets(path, sizeof(path)-1, fp) != NULL) {
        // Each line will be something like: "Device XX:XX:XX:XX:XX:XX Device Name"
        char *address = strtok(path, " "); // Get the "Device" token (we can discard this)
        char *addr = strtok(NULL, " ");    // Get the address token (XX:XX:XX:XX:XX:XX)
        char *name = strtok(NULL, "\n");   // Get the remaining part as the name
        
        if (addr && name) {
            // Add the device address and name to the devices array
            strncpy(devices[num_devices].address, addr, sizeof(devices[num_devices].address) - 1);
            strncpy(devices[num_devices].name, name, sizeof(devices[num_devices].name) - 1);
            num_devices++;
        }
    }
    fclose(fp);
    fp = popen("bluetoothctl paired-devices", "r");
    if (fp == NULL) {
        perror("Failed to run bluetoothctl paired-devices");
        return;
    }
    
    // Process paired devices
    printf("paired devices:\n");
    while (fgets(path, sizeof(path)-1, fp) != NULL) {
        // Each line will be something like: "Device XX:XX:XX:XX:XX:XX Device Name"
        char *address = strtok(path, " "); // Get the "Device" token (we can discard this)
        char *addr = strtok(NULL, " ");    // Get the address token (XX:XX:XX:XX:XX:XX)
        char *name = strtok(NULL, "\n");   // Get the remaining part as the name
    
        if (addr && name) {
            // Search for the device in the existing devices array and set paired = 1
            for (int i = 0; i < num_devices; i++) {
                if (strcmp(devices[i].address, addr) == 0) {
                    // If the device is found, mark it as paired
                    devices[i].paired = 1;
                    break;
                }
            }
        }
    }
    fclose(fp);

    // Run bluetoothctl to list connected devices
    fp = popen("bluetoothctl connected", "r");
    if (fp == NULL) {
        perror("Failed to run bluetoothctl connected");
        return;
    }

    // Process connected devices
    printf("\nConnected devices:\n");
    while (fgets(path, sizeof(path)-1, fp) != NULL) {
        // Each line will be something like: "XX:XX:XX:XX:XX:XX"
        char *address = strtok(path, " \n");

        if (address) {
            // Find the device in the paired devices list and mark it as connected
            for (int i = 0; i < num_devices; i++) {
                if (strcmp(devices[i].address, address) == 0) {
                    printf("Connected: %s %s\n", devices[i].address, devices[i].name);
                    break;
                }
            }
        }
    }
    fclose(fp);
}

int main(int argc, char *argv[]) {
    InitSettings();

    PWR_setCPUSpeed(CPU_SPEED_MENU);

    screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();

    scan_bluetooth_devices();

    int dirty = 1;
    int show_setting = 0;
    int selected_device = 0;
    while (!quit) {
        uint32_t frame_start = SDL_GetTicks();

        PAD_poll();

        // Check for quit or navigation inputs
        if (PAD_justPressed(BTN_B)) {
            quit = 1;
        } else if (PAD_justPressed(BTN_UP)) {
            selected_device = (selected_device - 1 + num_devices) % num_devices;
        } else if (PAD_justPressed(BTN_DOWN)) {
            selected_device = (selected_device + 1) % num_devices;
        } else if (PAD_justPressed(BTN_A)) {
            pair_device(devices[selected_device].address);
        } else if (PAD_justPressed(BTN_X)) {
            remove_device(devices[selected_device].address);
        }

        PWR_update(&dirty, &show_setting, NULL, NULL);

    
            GFX_clear(screen);

            int max_width = screen->w - SCALE1(PADDING * 2);

            char display_name[256];
            sprintf(display_name, "%s", "Bluetooth :D");
            char title[256];
            int text_width = GFX_truncateText(font.medium, display_name, title, max_width, SCALE1(BUTTON_PADDING * 2));
            max_width = MIN(max_width, text_width);

            SDL_Surface *text;
            text = TTF_RenderUTF8_Blended(font.medium, title, COLOR_WHITE);
            GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), max_width, SCALE1(PILL_SIZE)});
            SDL_BlitSurface(text, &(SDL_Rect){0, 0, max_width - SCALE1(BUTTON_PADDING * 2), text->h}, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4)});
            SDL_FreeSurface(text);

            
            for (int i = 0; i < num_devices; ++i) {
                char device_text[256];
                snprintf(device_text, sizeof(device_text), "%s: %s", devices[i].name, devices[i].address);
                
                bool selected = (i == selected_device);
                SDL_Color current_color = selected ? COLOR_BLACK : COLOR_WHITE;
                
                int y = SCALE1(PADDING + PILL_SIZE * (i + 1));
                
                SDL_Surface *text = TTF_RenderUTF8_Blended(font.medium, device_text, current_color);
                int text_width = text->w + SCALE1(BUTTON_PADDING * 2);
                
                GFX_blitPill(selected ? ASSET_WHITE_PILL : ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), y, text_width, SCALE1(PILL_SIZE)});
                SDL_BlitSurface(text, &(SDL_Rect){0, 0, text->w, text->h}, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4)});
                SDL_FreeSurface(text);
            }
            
            if (show_setting)
                GFX_blitHardwareHints(screen, show_setting);
            else
                GFX_blitButtonGroup((char*[]){ "B","BACK", NULL }, 1, screen, 0);
            
            GFX_blitButtonGroup((char*[]){ "A","PAIR","X","UNPAIR", NULL }, 1, screen, 1);

            GFX_flip(screen);
            dirty = 0;
        
      
    }

    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();

    return EXIT_SUCCESS;
}
