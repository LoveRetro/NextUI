/////////////////////////////////////////////////////////////////////////////////////////

// File: common/generic_bt.c
// Generic implementations of bluetooth functions, to be used by platforms that don't
// provide their own implementations.
// Used by: tg5050
// Library dependencies: pthread
// Tool dependencies: alsa, amixer, bluealsa, bluetoothctl
// Script dependencies: $SYSTEM_PATH//etc/bluetooth/bt_init.sh

// \note This files does not have an acompanying header, as all functions are declared in api.h
// with minimal fallback implementations
// \sa FALLBACK_IMPLEMENTATION

/////////////////////////////////////////////////////////////////////////////////////////

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include <pthread.h>
#include <unistd.h>

bool PLAT_hasBluetooth() { return true; }
bool PLAT_bluetoothEnabled() { return CFG_getBluetooth(); }

#define btlog(fmt, ...) \
    LOG_note(PLAT_bluetoothDiagnosticsEnabled() ? LOG_INFO : LOG_DEBUG, fmt, ##__VA_ARGS__)

// Forward declaration
static int bt_run_cmd(const char *cmd, char *output, size_t output_len);

// Bluetoothctl version detection
static int bluetoothctl_major_version = 0;
static int bluetoothctl_minor_version = 0;

static void bt_detect_version(void) {
	static bool detected = false;
	if (detected) return;
	
	char output[256];
	if (bt_run_cmd("bluetoothctl --version 2>/dev/null | head -1", output, sizeof(output)) == 0) {
		// Parse version like "bluetoothctl: 5.54" or "5.78"
		int major = 0, minor = 0;
		if (sscanf(output, "bluetoothctl: %d.%d", &major, &minor) == 2 ||
		    sscanf(output, "%d.%d", &major, &minor) == 2) {
			bluetoothctl_major_version = major;
			bluetoothctl_minor_version = minor;
			btlog("Detected bluetoothctl version %d.%d\n", major, minor);
		} else {
			// Default to 5.54 if detection fails
			bluetoothctl_major_version = 5;
			bluetoothctl_minor_version = 54;
			btlog("Failed to detect bluetoothctl version, assuming 5.54\n");
		}
	} else {
		// Assume older version if --version doesn't work
		bluetoothctl_major_version = 5;
		bluetoothctl_minor_version = 54;
		btlog("bluetoothctl --version failed, assuming 5.54\n");
	}
	detected = true;
}

// Helper to check if bluetoothctl version is >= specified version
static bool bt_version_gte(int major, int minor) {
	if (bluetoothctl_major_version > major) return true;
	if (bluetoothctl_major_version == major && bluetoothctl_minor_version >= minor) return true;
	return false;
}

// Device class definitions for parsing
#define COD_MAJOR_MASK     0x1F00
#define GET_MAJOR_CLASS(cod) ((cod & COD_MAJOR_MASK) >> 8)
#define BT_CLASS_AUDIO_VIDEO 0x04
#define BT_CLASS_PERIPHERAL  0x05

// Maximum discovered devices to track
#define MAX_DISCOVERED_DEVICES 64

typedef struct bt_dev_node {
	char addr[18];
	char name[249];
	BluetoothDeviceType kind;
	struct bt_dev_node *next;
} bt_dev_node_t;

static bt_dev_node_t *discovered_devices = NULL;
static pthread_mutex_t discovered_devices_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile bool bt_discovering = false;
static volatile bool bt_initialized = false;

// Helper to run a command and capture output
static int bt_run_cmd(const char *cmd, char *output, size_t output_len) {
	btlog("Running command: %s\n", cmd);
	FILE *fp = popen(cmd, "r");
	if (!fp) {
		LOG_error("Failed to run command: %s\n", cmd);
		return -1;
	}
	
	if (output && output_len > 0) {
		output[0] = '\0';
		size_t total = 0;
		char buf[256];
		while (fgets(buf, sizeof(buf), fp) && total < output_len - 1) {
			size_t len = strlen(buf);
			if (total + len < output_len) {
				strcpy(output + total, buf);
				total += len;
			}
		}
	}
	
	int status = pclose(fp);
	return WEXITSTATUS(status);
}

// Helper to add device to discovered list
static void bt_add_discovered_device(const char *addr, const char *name, BluetoothDeviceType kind) {
	pthread_mutex_lock(&discovered_devices_mtx);
	
	// Check if device already exists
	bt_dev_node_t *node = discovered_devices;
	while (node) {
		if (strcmp(node->addr, addr) == 0) {
			// Update name if it changed
			if (name && name[0] && strcmp(node->name, name) != 0) {
				strncpy(node->name, name, sizeof(node->name) - 1);
				node->name[sizeof(node->name) - 1] = '\0';
			}
			// Update kind if we have better info
			if (kind != BLUETOOTH_NONE) {
				node->kind = kind;
			}
			pthread_mutex_unlock(&discovered_devices_mtx);
			return;
		}
		node = node->next;
	}
	
	// Add new device
	bt_dev_node_t *new_node = (bt_dev_node_t *)malloc(sizeof(bt_dev_node_t));
	if (!new_node) {
		pthread_mutex_unlock(&discovered_devices_mtx);
		return;
	}
	
	strncpy(new_node->addr, addr, sizeof(new_node->addr) - 1);
	new_node->addr[sizeof(new_node->addr) - 1] = '\0';
	
	if (name && name[0]) {
		strncpy(new_node->name, name, sizeof(new_node->name) - 1);
		new_node->name[sizeof(new_node->name) - 1] = '\0';
	} else {
		strncpy(new_node->name, addr, sizeof(new_node->name) - 1);
		new_node->name[sizeof(new_node->name) - 1] = '\0';
	}
	
	new_node->kind = kind;
	new_node->next = discovered_devices;
	discovered_devices = new_node;
	
	btlog("Added discovered device: %s (%s) kind=%d\n", new_node->addr, new_node->name, kind);
	pthread_mutex_unlock(&discovered_devices_mtx);
}

// Helper to clear discovered devices list
static void bt_clear_discovered_devices(void) {
	pthread_mutex_lock(&discovered_devices_mtx);
	bt_dev_node_t *node = discovered_devices;
	while (node) {
		bt_dev_node_t *next = node->next;
		free(node);
		node = next;
	}
	discovered_devices = NULL;
	pthread_mutex_unlock(&discovered_devices_mtx);
}

// Helper to remove a device from discovered list (e.g., after pairing)
static void bt_remove_discovered_device(const char *addr) {
	pthread_mutex_lock(&discovered_devices_mtx);
	bt_dev_node_t **pp = &discovered_devices;
	while (*pp) {
		if (strcmp((*pp)->addr, addr) == 0) {
			bt_dev_node_t *to_free = *pp;
			*pp = (*pp)->next;
			free(to_free);
			pthread_mutex_unlock(&discovered_devices_mtx);
			return;
		}
		pp = &(*pp)->next;
	}
	pthread_mutex_unlock(&discovered_devices_mtx);
}

// Parse device class from bluetoothctl info output
static BluetoothDeviceType bt_parse_device_class(const char *info_output) {
	// Look for "Class:" line in bluetoothctl info output
	// Format: Class: 0x240404 (audio-card)
	const char *class_line = strstr(info_output, "Class:");
	if (class_line) {
		unsigned int class_val = 0;
		if (sscanf(class_line, "Class: 0x%x", &class_val) == 1) {
			int major = GET_MAJOR_CLASS(class_val);
			if (major == BT_CLASS_AUDIO_VIDEO) {
				return BLUETOOTH_AUDIO;
			} else if (major == BT_CLASS_PERIPHERAL) {
				return BLUETOOTH_CONTROLLER;
			}
		}
	}
	
	// Also check Icon field as fallback
	const char *icon_line = strstr(info_output, "Icon:");
	if (icon_line) {
		if (strstr(icon_line, "audio") || strstr(icon_line, "headset") || strstr(icon_line, "headphone")) {
			return BLUETOOTH_AUDIO;
		} else if (strstr(icon_line, "input-gaming") || strstr(icon_line, "input-keyboard") || strstr(icon_line, "input-mouse")) {
			return BLUETOOTH_CONTROLLER;
		}
	}
	
	return BLUETOOTH_NONE;
}

// Get device info using bluetoothctl
static BluetoothDeviceType bt_get_device_type(const char *addr) {
	char cmd[256];
	char output[2048];
	
	snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", addr);
	if (bt_run_cmd(cmd, output, sizeof(output)) == 0) {
		return bt_parse_device_class(output);
	}
	return BLUETOOTH_NONE;
}

// Check if bluetooth adapter is powered on
static bool bt_is_powered(void) {
	char output[256];
	if (bt_run_cmd("bluetoothctl show 2>/dev/null | grep 'Powered:' | awk '{print $2}'", output, sizeof(output)) == 0) {
		return strstr(output, "yes") != NULL;
	}
	return false;
}

/////////////////////////////////

void PLAT_bluetoothInit() {
	LOG_info("BT init (generic Linux)\n");

	if (bt_initialized) {
		LOG_error("BT is already initialized.\n");
		return;
	}
	
	// Detect bluetoothctl version
	bt_detect_version();
	
	bt_initialized = true;
	PLAT_bluetoothEnable(CFG_getBluetooth());
}

void PLAT_bluetoothDeinit() {
	if (bt_initialized) {
		bt_clear_discovered_devices();
		bt_initialized = false;
	}
}

void PLAT_bluetoothEnable(bool shouldBeOn) {
	if (shouldBeOn) {
		btlog("Turning BT on...\n");
		system(SYSTEM_PATH "/etc/bluetooth/bt_init.sh start");
	} else {
		btlog("Turning BT off...\n");
		// Stop discovery if active
		if (bt_discovering) {
			//system("bluetoothctl scan off 2>/dev/null");
			bt_discovering = false;
		}
		system(SYSTEM_PATH "/etc/bluetooth/bt_init.sh stop");
	}
	CFG_setBluetooth(shouldBeOn);
}

bool PLAT_bluetoothDiagnosticsEnabled() { 
	return CFG_getBluetoothDiagnostics(); 
}

void PLAT_bluetoothDiagnosticsEnable(bool on) {
	CFG_setBluetoothDiagnostics(on);
}

void PLAT_bluetoothDiscovery(int on) {
	if (on) {
		btlog("Starting BT discovery.\n");
		// Clear old discovered devices
		bt_clear_discovered_devices();
		
		// Start scanning - version-dependent command
		if (bt_version_gte(5, 70)) {
			// In 5.70+, timeout option works differently
			// Start scan in background and schedule auto-stop
			system("sh -c 'bluetoothctl scan on 2>/dev/null & BT_PID=$!; sleep 60; bluetoothctl scan off 2>/dev/null; kill $BT_PID 2>/dev/null' &");
		} else {
			// For 5.54 and similar versions
			system("bluetoothctl --timeout 60 scan on 2>/dev/null &");
		}
		bt_discovering = true;
	} else {
		btlog("Stopping BT discovery.\n");
		system("bluetoothctl scan off 2>/dev/null");
		// Also try to kill any background scan processes
		system("pkill -f 'bluetoothctl scan on' 2>/dev/null");
		bt_discovering = false;
	}
}

bool PLAT_bluetoothDiscovering() {
	return bt_discovering;
}

int PLAT_bluetoothScan(struct BT_device *devices, int max) {
	if (!CFG_getBluetooth()) {
		return 0;
	}
	
	// Get list of discovered devices from bluetoothctl
	char output[8192];
	if (bt_run_cmd("bluetoothctl devices 2>/dev/null", output, sizeof(output)) != 0) {
		btlog("Failed to get device list\n");
		return 0;
	}
	
	// Parse output: "Device XX:XX:XX:XX:XX:XX DeviceName"
	char *line = strtok(output, "\n");
	while (line) {
		char addr[18] = {0};
		char name[249] = {0};
		
		// Parse "Device XX:XX:XX:XX:XX:XX Name"
		if (strncmp(line, "Device ", 7) == 0) {
			if (sscanf(line, "Device %17s", addr) == 1) {
				// Get name (everything after the address)
				char *name_start = line + 7 + 18; // "Device " + "XX:XX:XX:XX:XX:XX "
				if (*name_start) {
					strncpy(name, name_start, sizeof(name) - 1);
				}
				
				// Get device type
				BluetoothDeviceType kind = bt_get_device_type(addr);
				
				// Only add audio and controller devices, skip unknowns for scan results
				if (kind == BLUETOOTH_AUDIO || kind == BLUETOOTH_CONTROLLER) {
					bt_add_discovered_device(addr, name, kind);
				}
			}
		}
		line = strtok(NULL, "\n");
	}
	
	// Copy discovered devices to output array
	int count = 0;
	pthread_mutex_lock(&discovered_devices_mtx);
	bt_dev_node_t *node = discovered_devices;
	while (node && count < max) {
		// Skip devices that are already paired
		char cmd[256];
		char paired_output[256];
		snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null | grep 'Paired: yes'", node->addr);
		if (bt_run_cmd(cmd, paired_output, sizeof(paired_output)) == 0 && strstr(paired_output, "Paired: yes")) {
			node = node->next;
			continue;
		}
		
		struct BT_device *device = &devices[count];
		strncpy(device->addr, node->addr, sizeof(device->addr) - 1);
		device->addr[sizeof(device->addr) - 1] = '\0';
		strncpy(device->name, node->name, sizeof(device->name) - 1);
		device->name[sizeof(device->name) - 1] = '\0';
		device->kind = node->kind;
		
		btlog("Scan result: %s (%s) kind=%d\n", device->addr, device->name, device->kind);
		count++;
		node = node->next;
	}
	pthread_mutex_unlock(&discovered_devices_mtx);
	
	return count;
}

int PLAT_bluetoothPaired(struct BT_devicePaired *paired, int max) {
	if (!CFG_getBluetooth()) {
		return 0;
	}
	
	// Get list of paired devices - try both command formats
	char output[8192];
	int ret = bt_run_cmd("bluetoothctl paired-devices 2>/dev/null", output, sizeof(output));
	
	// If paired-devices doesn't work (5.78+), try alternative command
	if (ret != 0 || strlen(output) == 0) {
		ret = bt_run_cmd("bluetoothctl devices Paired 2>/dev/null", output, sizeof(output));
	}
	
	if (ret != 0) {
		btlog("Failed to get paired device list\n");
		return 0;
	}
	
	int count = 0;
	char *line = strtok(output, "\n");
	while (line && count < max) {
		char addr[18] = {0};
		char name[249] = {0};
		
		// Parse "Device XX:XX:XX:XX:XX:XX Name"
		if (strncmp(line, "Device ", 7) == 0) {
			if (sscanf(line, "Device %17s", addr) == 1) {
				// Get name (everything after the address)
				char *name_start = line + 7 + 18;
				if (*name_start) {
					strncpy(name, name_start, sizeof(name) - 1);
				}
				
				struct BT_devicePaired *device = &paired[count];
				strncpy(device->remote_addr, addr, sizeof(device->remote_addr) - 1);
				device->remote_addr[sizeof(device->remote_addr) - 1] = '\0';
				strncpy(device->remote_name, name, sizeof(device->remote_name) - 1);
				device->remote_name[sizeof(device->remote_name) - 1] = '\0';
				device->is_bonded = true;
				device->rssi = -50; // Default value, actual RSSI requires active connection
				
				// Check if connected
				char cmd[256];
				char info_output[1024];
				snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null | grep 'Connected: yes'", addr);
				device->is_connected = (bt_run_cmd(cmd, info_output, sizeof(info_output)) == 0 && 
				                        strstr(info_output, "Connected: yes") != NULL);
				
				btlog("Paired device: %s (%s) connected=%d\n", device->remote_addr, device->remote_name, device->is_connected);
				count++;
			}
		}
		line = strtok(NULL, "\n");
	}
	
	return count;
}

void PLAT_bluetoothPair(char *addr) {
	btlog("Pairing with %s\n", addr);
	
	char cmd[256];
	
	// Trust the device first (for automatic reconnection)
	snprintf(cmd, sizeof(cmd), "bluetoothctl trust %s 2>/dev/null", addr);
	system(cmd);
	
	// Small delay to ensure trust command completes
	usleep(100000);
	
	// Pair with the device
	snprintf(cmd, sizeof(cmd), "bluetoothctl pair %s 2>/dev/null", addr);
	int ret = system(cmd);
	if (ret != 0) {
		LOG_error("BT pair failed: %d\n", ret);
		// In newer versions, try alternative pairing method
		if (bt_version_gte(5, 70)) {
			snprintf(cmd, sizeof(cmd), "echo 'pair %s' | bluetoothctl 2>/dev/null", addr);
			ret = system(cmd);
			if (ret != 0) {
				LOG_error("BT pair (alternative method) failed: %d\n", ret);
			}
		}
	}
	
	// Remove from discovered list since it's now paired
	bt_remove_discovered_device(addr);
}

void PLAT_bluetoothUnpair(char *addr) {
	btlog("Unpairing %s\n", addr);
	
	char cmd[256];
	
	// Disconnect first if connected
	snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s 2>/dev/null", addr);
	system(cmd);
	
	// Remove the device (this unpairs it)
	snprintf(cmd, sizeof(cmd), "bluetoothctl remove %s 2>/dev/null", addr);
	int ret = system(cmd);
	if (ret != 0) {
		LOG_error("BT unpair failed\n");
	}
}

void PLAT_bluetoothConnect(char *addr) {
	btlog("Connecting to %s\n", addr);
	
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "bluetoothctl connect %s 2>/dev/null", addr);
	int ret = system(cmd);
	if (ret != 0) {
		LOG_error("BT connect failed: %d\n", ret);
	}
	LOG_info("BT connect returned: %d\n", ret);
}

void PLAT_bluetoothDisconnect(char *addr) {
	btlog("Disconnecting from %s\n", addr);
	
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s 2>/dev/null", addr);
	int ret = system(cmd);
	if (ret != 0) {
		LOG_error("BT disconnect failed: %d\n", ret);
	}
}

bool PLAT_bluetoothConnected() {
	// Check for any active ACL connections using hcitool
	FILE *fp;
	char buffer[256];
	bool connected = false;

	fp = popen("hcitool con 2>/dev/null", "r");
	if (fp == NULL) {
		// Fallback: check bluetoothctl
		char output[2048];
		if (bt_run_cmd("bluetoothctl info 2>/dev/null | grep 'Connected: yes'", output, sizeof(output)) == 0) {
			return strstr(output, "Connected: yes") != NULL;
		}
		return false;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strstr(buffer, "ACL")) {
			connected = true;
			break;
		}
	}

	pclose(fp);
	return connected;
}

int PLAT_bluetoothVolume() {
	// Try to get volume from ALSA mixer for bluealsa
	char output[256];
	int vol = 100; // Default to 100%
	
	// Try bluealsa-aplay volume or amixer
	if (bt_run_cmd("amixer -D bluealsa get 'A2DP' 2>/dev/null | grep -o '[0-9]*%' | head -1 | tr -d '%'", output, sizeof(output)) == 0) {
		int parsed_vol;
		if (sscanf(output, "%d", &parsed_vol) == 1) {
			vol = parsed_vol;
		}
	}
	
	btlog("BT volume: %d\n", vol);
	return vol;
}

void PLAT_bluetoothSetVolume(int vol) {
	if (vol > 100) vol = 100;
	if (vol < 0) vol = 0;
	
	char cmd[256];
	// Try to set bluealsa volume
	snprintf(cmd, sizeof(cmd), "amixer -D bluealsa set 'A2DP' %d%% 2>/dev/null", vol);
	system(cmd);
	
	btlog("Set BT volume: %d\n", vol);
}

// bt_device_watcher.c

#include <sys/inotify.h>

#define WATCHED_DIR_FMT "%s"
#define WATCHED_FILE ".asoundrc"
#define EVENT_BUF_LEN (1024 * (sizeof(struct inotify_event) + NAME_MAX + 1))

static pthread_t watcher_thread;
static int inotify_fd = -1;
static int dir_watch_fd = -1;
static int file_watch_fd = -1;
static volatile int running = 0;
static void (*callback_fn)(int device, int watch_event) = NULL;
static char watched_dir[MAX_PATH];
static char watched_file_path[MAX_PATH];

// Function to detect audio device type from .asoundrc content
static int detect_audio_device_type() {
    FILE *file = fopen(watched_file_path, "r");
    if (!file) {
		//LOG_info("detect_audio_device_type: .asoundrc not found, defaulting to AUDIO_SINK_DEFAULT\n");
        return AUDIO_SINK_DEFAULT;
    }
    
    char line[256];
    int is_bluetooth = 0;
    int is_usb_dac = 0;
    
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "type bluealsa") || strstr(line, "defaults.bluealsa.device")) {
            //LOG_info("detect_audio_device_type: found bluealsa\n");
            is_bluetooth = 1;
            break;
        }
        if (strstr(line, "type hw")) {
			//LOG_info("detect_audio_device_type: found hw card\n");
            is_usb_dac = 1;
            break;
        }
    }
    
    fclose(file);
    
    if (is_bluetooth) {
        return AUDIO_SINK_BLUETOOTH;
    } else if (is_usb_dac) {
        return AUDIO_SINK_USBDAC;
    } else {
        return AUDIO_SINK_DEFAULT;
    }
}

static void add_file_watch() {
    if (file_watch_fd >= 0) return; // already watching

    file_watch_fd = inotify_add_watch(inotify_fd, watched_file_path,
                                      IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF);
    if (file_watch_fd < 0) {
        if (errno != ENOENT) // ENOENT means file doesn't exist yet - no error needed
            LOG_error("PLAT_audioDeviceWatchRegister: failed to add file watch: %s\n", strerror(errno));
    } else {
        LOG_info("Watching file: %s\n", watched_file_path);
    }
}

static void remove_file_watch() {
    if (file_watch_fd >= 0) {
        inotify_rm_watch(inotify_fd, file_watch_fd);
        file_watch_fd = -1;
        LOG_info("Stopped watching file: %s\n", watched_file_path);
    }
}

static void *watcher_thread_func(void *arg) {
    char buffer[EVENT_BUF_LEN];

    // At start try to watch file if exists
    add_file_watch();

    while (running) {
        int length = read(inotify_fd, buffer, EVENT_BUF_LEN);
        if (length < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                sleep(1);
                continue;
            }
            LOG_error("inotify read error: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < length;) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            if (event->wd == dir_watch_fd) {
                if (event->len > 0 && strcmp(event->name, WATCHED_FILE) == 0) {
                    if (event->mask & IN_CREATE) {
                        add_file_watch();
                        int device_type = detect_audio_device_type();
                        if (callback_fn) callback_fn(device_type, DIRWATCH_CREATE);
                    }
					// No need to react to this, we handle it via file watch
                    //else if (event->mask & IN_DELETE) {
                    //    remove_file_watch();
                    //    if (callback_fn) callback_fn(AUDIO_SINK_DEFAULT, DIRWATCH_DELETE);
                    //}
                }
            }
            else if (event->wd == file_watch_fd) {
                if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF)) {
                    if (event->mask & IN_DELETE_SELF) {
                        remove_file_watch();
						if (callback_fn) callback_fn(AUDIO_SINK_DEFAULT, FILEWATCH_DELETE);
                    }
					// No need to react to this, it usually comes paired with FILEWATCH_MODIFY
					//else if (event->mask & IN_CLOSE_WRITE) {
					//	if (callback_fn) callback_fn(AUDIO_SINK_BLUETOOTH, FILEWATCH_CLOSE_WRITE);
					//}
					else if (event->mask & IN_MODIFY) {
						int device_type = detect_audio_device_type();
						if (callback_fn) callback_fn(device_type, FILEWATCH_MODIFY);
					}
                }
            }

            i += sizeof(struct inotify_event) + event->len;
        }
    }

    return NULL;
}

void PLAT_audioDeviceWatchRegister(void (*cb)(int device, int event)) {
    if (running) return; // Already running

    callback_fn = cb;

    const char *home = getenv("HOME");
    if (!home) {
        LOG_error("PLAT_audioDeviceWatchRegister: HOME environment variable not set\n");
        return;
    }

    snprintf(watched_dir, MAX_PATH, WATCHED_DIR_FMT, home);
    snprintf(watched_file_path, MAX_PATH, "%s/%s", watched_dir, WATCHED_FILE);

    LOG_info("PLAT_audioDeviceWatchRegister: Watching directory %s\n", watched_dir);
    LOG_info("PLAT_audioDeviceWatchRegister: Watching file %s\n", watched_file_path);

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        LOG_error("PLAT_audioDeviceWatchRegister: failed to initialize inotify\n");
        return;
    }

    dir_watch_fd = inotify_add_watch(inotify_fd, watched_dir, IN_CREATE | IN_DELETE);
    if (dir_watch_fd < 0) {
        LOG_error("PLAT_audioDeviceWatchRegister: failed to add directory watch\n");
        close(inotify_fd);
        inotify_fd = -1;
        return;
    }

    file_watch_fd = -1;

    running = 1;
    if (pthread_create(&watcher_thread, NULL, watcher_thread_func, NULL) != 0) {
        LOG_error("PLAT_audioDeviceWatchRegister: failed to create thread\n");
        inotify_rm_watch(inotify_fd, dir_watch_fd);
        close(inotify_fd);
        inotify_fd = -1;
        dir_watch_fd = -1;
        running = 0;
    }
}

void PLAT_audioDeviceWatchUnregister(void) {
    if (!running) return;

    running = 0;
    pthread_join(watcher_thread, NULL);

    if (file_watch_fd >= 0)
        inotify_rm_watch(inotify_fd, file_watch_fd);
    if (dir_watch_fd >= 0)
        inotify_rm_watch(inotify_fd, dir_watch_fd);
    if (inotify_fd >= 0)
        close(inotify_fd);

    inotify_fd = -1;
    dir_watch_fd = -1;
    file_watch_fd = -1;
    callback_fn = NULL;
}
