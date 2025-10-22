// bt_audio_gamepad_daemon.cpp
// Monitors Bluetooth device connections and USB-C DAC connections, updating .asoundrc for audio sinks

#include <dbus/dbus.h>
#include <libudev.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <syslog.h>
#include <errno.h>

extern "C"
{
#include "msettings.h"
}

#define AUDIO_FILE "/mnt/SDCARD/.userdata/tg5040/.asoundrc"
#define UUID_A2DP "0000110b-0000-1000-8000-00805f9b34fb"

enum DeviceType {
    DEVICE_BLUETOOTH,
    DEVICE_USB_AUDIO
};

bool use_syslog = false;
bool running = true;

void log(const std::string& msg) {
    if (use_syslog) syslog(LOG_INFO, "%s", msg.c_str());
    else std::cout << msg << std::endl;
}

void ensureDirExists(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

void writeAudioFile(const std::string& device_identifier, DeviceType type) {
    ensureDirExists("/mnt/SDCARD/.userdata/tg5040");
    std::ofstream f(AUDIO_FILE);
    if (!f) {
        log("Failed to write audio config file");
        return;
    }

    if (type == DEVICE_BLUETOOTH) {
        // Bluetooth A2DP configuration
        f << "defaults.bluealsa.device \"" << device_identifier << "\"\n\n"
          << "pcm.!default {\n"
          << "    type plug\n"
          << "    slave.pcm {\n"
          << "        type bluealsa\n"
          << "        device \"" << device_identifier << "\"\n"
          << "        profile \"a2dp\"\n"
          << "        delay 0\n"
          << "    }\n"
          << "}\n"
          << "ctl.!default {\n"
          << "    type bluealsa\n"
          << "}\n";
        log("Updated .asoundrc with Bluetooth device: " + device_identifier);
    } else if (type == DEVICE_USB_AUDIO) {
        // USB Audio configuration using ALSA card
        f << "pcm.!default {\n"
          << "    type hw\n"
          << "    card " << device_identifier << "\n"
          << "}\n"
          << "ctl.!default {\n"
          << "    type hw\n"
          << "    card " << device_identifier << "\n"
          << "}\n";
        log("Updated .asoundrc with USB audio device: " + device_identifier);
    }

    f.flush(); // flush C++ stream buffer

    // Ensure it's flushed to disk
    int fd = ::open(AUDIO_FILE, O_WRONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

void clearAudioFile() {
    if (unlink(AUDIO_FILE) == 0) {
        log("Removed audio config");
        // Ensure it's flushed to disk
        int dfd = open("/mnt/SDCARD/.userdata/tg5040", O_DIRECTORY);
        fsync(dfd); // sync the directory entry removal
        close(dfd);
    } else {
        log("Audio config file not present or failed to remove");
    }
}

std::string pathToMac(const std::string& path) {
    auto pos = path.find("dev_");
    if (pos == std::string::npos) return "";
    std::string mac = path.substr(pos + 4);
    for (auto& c : mac) if (c == '_') c = ':';
    return mac;
}

std::string getUsbAudioCardNumber(struct udev_device* dev) {
    // Look for the card number in the device path or properties
    const char* devnode = udev_device_get_devnode(dev);
    if (!devnode) return "";

    // Extract card number from device node like /dev/snd/controlC1
    std::string devnode_str(devnode);
    auto pos = devnode_str.find("controlC");
    if (pos != std::string::npos) {
        return devnode_str.substr(pos + 8); // Extract number after "controlC"
    }

    // Alternative: check ALSA card property
    const char* card = udev_device_get_property_value(dev, "SOUND_CARD");
    if (card) return std::string(card);

    return "";
}

bool isUsbAudioDevice(struct udev_device* dev) {
    const char* subsystem = udev_device_get_subsystem(dev);
    const char* devnode = udev_device_get_devnode(dev);
    const char* devpath = udev_device_get_devpath(dev);

    if (!subsystem || strcmp(subsystem, "sound") != 0) {
        return false;
    }

    if (!devnode) {
        return false;
    }

    // Check if it's a control device (indicates audio capability)
    std::string devnode_str(devnode);
    if (devnode_str.find("controlC") == std::string::npos) {
        return false;
    }

    // Check if it's USB-connected by looking at the device path
    if (devpath && strstr(devpath, "usb")) {
        return true;
    }

    return false;
}

bool hasUUID(DBusConnection* conn, const std::string& path, const std::string& uuid) {
    DBusMessage* msg = dbus_message_new_method_call("org.bluez", path.c_str(), "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return false;

    const char* iface = "org.bluez.Device1";
    const char* prop = "UUIDs";

    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, nullptr);
    dbus_message_unref(msg);
    if (!reply) return false;

    DBusMessageIter iter;
    dbus_message_iter_init(reply, &iter);
    DBusMessageIter variant;
    dbus_message_iter_recurse(&iter, &variant);

    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return false;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse(&variant, &array);

    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
        const char* val;
        dbus_message_iter_get_basic(&array, &val);
        if (uuid == val) {
            dbus_message_unref(reply);
            return true;
        }
        dbus_message_iter_next(&array);
    }

    dbus_message_unref(reply);
    return false;
}

void handleDeviceConnected(DBusConnection* conn, const std::string& path) {
    std::string mac = pathToMac(path);
    if (hasUUID(conn, path, UUID_A2DP)) {
        log("Audio device connected: " + mac);
        writeAudioFile(mac, DEVICE_BLUETOOTH);
        SetAudioSink(AUDIO_SINK_BLUETOOTH);
    } else {
        log("Non-audio device connected: " + mac);
    }
}

void handleDeviceDisconnected(DBusConnection* conn, const std::string& path) {
    std::string mac = pathToMac(path);
    if (hasUUID(conn, path, UUID_A2DP)) {
        log("Audio device disconnected: " + mac);
        clearAudioFile();
        // TODO: we could maintain a stack here, if USBC was connected before and restore that instead
        SetAudioSink(AUDIO_SINK_DEFAULT);
    }
}

void handleUsbAudioConnected(struct udev_device* dev) {
    std::string card = getUsbAudioCardNumber(dev);
    if (!card.empty()) {
        log("USB audio device connected: card " + card);
        writeAudioFile(card, DEVICE_USB_AUDIO);
        SetAudioSink(AUDIO_SINK_USBDAC);
    }
}

void handleUsbAudioDisconnected(struct udev_device* dev) {
    std::string card = getUsbAudioCardNumber(dev);
    if (!card.empty()) {
        log("USB audio device disconnected: card " + card);
        clearAudioFile();
        // TODO: we could maintain a stack here, if BT was connected before and restore that instead
        SetAudioSink(AUDIO_SINK_DEFAULT);
    }
}

void signalHandler(int sig) {
    running = false;
}

void scanExistingUsbAudioDevices(struct udev* udev) {
    log("Scanning for existing USB audio devices...");

    struct udev_enumerate* enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
        log("Failed to create udev enumerator");
        return;
    }

    // Filter for sound subsystem
    udev_enumerate_add_match_subsystem(enumerate, "sound");
    udev_enumerate_scan_devices(enumerate);

    struct udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry* entry;

    udev_list_entry_foreach(entry, devices) {
        const char* path = udev_list_entry_get_name(entry);
        struct udev_device* dev = udev_device_new_from_syspath(udev, path);

        if (dev) {
            if (isUsbAudioDevice(dev)) {
                log("Found existing USB audio device at startup");
                handleUsbAudioConnected(dev);
            }
            udev_device_unref(dev);
        }
    }

    udev_enumerate_unref(enumerate);
    log("Finished scanning for existing USB audio devices");
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "-s") {
        use_syslog = true;
        openlog("audiomon", LOG_PID | LOG_CONS, LOG_USER);
    }

    InitSettings();
    // This will be updated as soon as something connects
    SetAudioSink(AUDIO_SINK_DEFAULT);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Initialize D-Bus
    DBusError err;
    dbus_error_init(&err);

    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        log("Failed to connect to system D-Bus");
        return 1;
    }
    log("Connected to system D-Bus");

    dbus_bus_add_match(conn,
        "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
        nullptr);
    dbus_connection_flush(conn);

    // Initialize udev
    struct udev* udev = udev_new();
    if (!udev) {
        log("Failed to create udev context");
        return 1;
    }

    // Try both kernel and udev events
    struct udev_monitor* mon = udev_monitor_new_from_netlink(udev, "kernel");
    if (!mon) {
        log("Failed to create kernel udev monitor, trying udev monitor");
        mon = udev_monitor_new_from_netlink(udev, "udev");
        if (!mon) {
            log("Failed to create udev monitor");
            udev_unref(udev);
            return 1;
        }
    }

    // Don't filter initially - let's see all events
    // udev_monitor_filter_add_match_subsystem_devtype(mon, "sound", NULL);
    udev_monitor_enable_receiving(mon);

    // Scan for existing USB audio devices before starting event monitoring
    scanExistingUsbAudioDevices(udev);

    int udev_fd = udev_monitor_get_fd(mon);
    int dbus_fd = -1;

    // Try to get D-Bus file descriptor
    if (!dbus_connection_get_unix_fd(conn, &dbus_fd)) {
        log("Warning: Could not get D-Bus file descriptor, will use polling");
        dbus_fd = -1;
    }

    log("Monitoring for Bluetooth and USB audio device events");

    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);

        // Add D-Bus file descriptor
        if (dbus_fd >= 0) {
            FD_SET(dbus_fd, &readfds);
        }

        // Add udev file descriptor
        FD_SET(udev_fd, &readfds);

        int max_fd = (dbus_fd > udev_fd) ? dbus_fd : udev_fd;

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);

        if (ret < 0) {
            if (errno == EINTR) continue;
            log("select() error");
            break;
        }

        // Handle D-Bus events
        if (dbus_fd >= 0 && FD_ISSET(dbus_fd, &readfds)) {
            dbus_connection_read_write(conn, 0);
            DBusMessage* msg = dbus_connection_pop_message(conn);
            if (msg) {
                if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
                    const char* path = dbus_message_get_path(msg);
                    if (path && std::string(path).find("dev_") != std::string::npos) {

                        DBusMessageIter args;
                        dbus_message_iter_init(msg, &args);

                        const char* iface = nullptr;
                        dbus_message_iter_get_basic(&args, &iface);
                        if (iface && std::string(iface) == "org.bluez.Device1") {

                            dbus_message_iter_next(&args);
                            if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_ARRAY) {
                                DBusMessageIter changed;
                                dbus_message_iter_recurse(&args, &changed);

                                while (dbus_message_iter_get_arg_type(&changed) == DBUS_TYPE_DICT_ENTRY) {
                                    DBusMessageIter dict;
                                    dbus_message_iter_recurse(&changed, &dict);

                                    const char* key;
                                    dbus_message_iter_get_basic(&dict, &key);

                                    if (std::string(key) == "Connected") {
                                        dbus_message_iter_next(&dict);
                                        DBusMessageIter variant;
                                        dbus_message_iter_recurse(&dict, &variant);
                                        dbus_bool_t connected;
                                        dbus_message_iter_get_basic(&variant, &connected);

                                        if (connected)
                                            handleDeviceConnected(conn, path);
                                        else
                                            handleDeviceDisconnected(conn, path);
                                    }

                                    dbus_message_iter_next(&changed);
                                }
                            }
                        }
                    }
                }
                dbus_message_unref(msg);
            }
        }

        // Handle udev events
        if (FD_ISSET(udev_fd, &readfds)) {
            struct udev_device* dev = udev_monitor_receive_device(mon);
            if (dev) {
                const char* action = udev_device_get_action(dev);
                const char* subsystem = udev_device_get_subsystem(dev);

                // Only process sound events
                if (subsystem && strcmp(subsystem, "sound") == 0) {
                    if (isUsbAudioDevice(dev)) {
                        if (action) {
                            if (strcmp(action, "add") == 0) {
                                handleUsbAudioConnected(dev);
                            } else if (strcmp(action, "remove") == 0) {
                                handleUsbAudioDisconnected(dev);
                            }
                        }
                    }
                }
                udev_device_unref(dev);
            }
        }
    }

    // Cleanup
    udev_monitor_unref(mon);
    udev_unref(udev);

    if (use_syslog) closelog();
    return 0;
}
