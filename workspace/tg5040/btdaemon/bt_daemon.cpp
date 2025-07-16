// bt_audio_gamepad_daemon.cpp
// Monitors Bluetooth device connections and updates .asoundrc for audio sinks

#include <dbus/dbus.h>
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
#include <syslog.h>

#define AUDIO_FILE "/mnt/SDCARD/.userdata/tg5040/.asoundrc"
#define UUID_A2DP "0000110b-0000-1000-8000-00805f9b34fb"

bool use_syslog = false;
bool running = true;

void log(const std::string& msg) {
    if (use_syslog) syslog(LOG_INFO, "%s", msg.c_str());
    else std::cout << msg << std::endl;
}

void ensureDirExists(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

void writeAudioFile(const std::string& mac) {
    ensureDirExists("/mnt/SDCARD/.userdata/tg5040");
    std::ofstream f(AUDIO_FILE);
    if (!f) {
        log("Failed to write audio config file");
        return;
    }
    f << "defaults.bluealsa.device \"" << mac << "\"\n\n"
      << "pcm.!default {\n"
      << "    type plug\n"
      << "    slave.pcm {\n"
      << "        type bluealsa\n"
      //<< "        interface \"hci0\"\n"
      << "        device \"" << mac << "\"\n"
      << "        profile \"a2dp\"\n"
      << "        delay 0\n"
      //<< "        delay 1000\n"
      << "    }\n"
      << "}\n"
      << "ctl.!default {\n"
      << "    type bluealsa\n"
      //<< "    interface \"hci0\"\n"
      << "}\n";

    f.flush(); // flush C++ stream buffer

    // Ensure it's flushed to disk
    int fd = ::open(AUDIO_FILE, O_WRONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }

    log("Updated .asoundrc with device: " + mac);
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
        writeAudioFile(mac);
    } else {
        log("Non-audio device connected: " + mac);
    }
}

void handleDeviceDisconnected(DBusConnection* conn, const std::string& path) {
    std::string mac = pathToMac(path);
    if (hasUUID(conn, path, UUID_A2DP)) {
        log("Audio device disconnected: " + mac);
        clearAudioFile();
    }
}

void signalHandler(int sig) {
    running = false;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "-s") {
        use_syslog = true;
        openlog("bt_daemon", LOG_PID | LOG_CONS, LOG_USER);
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

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

    while (running) {
        dbus_connection_read_write(conn, 1000);
        DBusMessage* msg = dbus_connection_pop_message(conn);
        if (!msg) continue;

        if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
            const char* path = dbus_message_get_path(msg);
            if (!path || std::string(path).find("dev_") == std::string::npos) {
                dbus_message_unref(msg);
                continue;
            }

            DBusMessageIter args;
            dbus_message_iter_init(msg, &args);

            const char* iface = nullptr;
            dbus_message_iter_get_basic(&args, &iface);
            if (!iface || std::string(iface) != "org.bluez.Device1") {
                dbus_message_unref(msg);
                continue;
            }

            dbus_message_iter_next(&args);
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
                dbus_message_unref(msg);
                continue;
            }

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

        dbus_message_unref(msg);
    }

    if (use_syslog) closelog();
    return 0;
}
