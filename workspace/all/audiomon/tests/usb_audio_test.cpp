#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static char test_dir[] = "/tmp/audiomon-test-XXXXXX";
static char test_audio_file[256];
#define __DEFINES_H__
#define USERDATA_PATH test_dir
#define AUDIO_FILE test_audio_file
#define main audiomon_main
#include "../audiomon.cpp"
#undef main

struct FakeDevice {
    const char* name;
    const char* path;
    const char* subsystem;
};

static std::vector<FakeDevice*> scan_devices;
static int sink = AUDIO_SINK_DEFAULT;

extern "C" {
void InitSettings() {}
int GetAudioSink() { return sink; }
void SetAudioSink(int value) { sink = value; }

const char* udev_device_get_sysname(struct udev_device* dev) {
    return reinterpret_cast<FakeDevice*>(dev)->name;
}
const char* udev_device_get_devpath(struct udev_device* dev) {
    return reinterpret_cast<FakeDevice*>(dev)->path;
}
const char* udev_device_get_subsystem(struct udev_device* dev) {
    return reinterpret_cast<FakeDevice*>(dev)->subsystem;
}
struct udev_enumerate* udev_enumerate_new(struct udev*) {
    return reinterpret_cast<struct udev_enumerate*>(1);
}
struct udev_enumerate* udev_enumerate_unref(struct udev_enumerate*) { return nullptr; }
int udev_enumerate_add_match_subsystem(struct udev_enumerate*, const char*) { return 0; }
int udev_enumerate_scan_devices(struct udev_enumerate*) { return 0; }
struct udev_list_entry* udev_enumerate_get_list_entry(struct udev_enumerate*) {
    return scan_devices.empty() ? nullptr : reinterpret_cast<struct udev_list_entry*>(&scan_devices[0]);
}
struct udev_list_entry* udev_list_entry_get_next(struct udev_list_entry* entry) {
    auto current = reinterpret_cast<FakeDevice**>(entry);
    auto next = current + 1;
    return next == scan_devices.data() + scan_devices.size() ? nullptr : reinterpret_cast<struct udev_list_entry*>(next);
}
const char* udev_list_entry_get_name(struct udev_list_entry* entry) {
    return reinterpret_cast<const char*>(*reinterpret_cast<FakeDevice**>(entry));
}
struct udev_device* udev_device_new_from_syspath(struct udev*, const char* path) {
    return reinterpret_cast<struct udev_device*>(const_cast<char*>(path));
}
struct udev_device* udev_device_unref(struct udev_device*) { return nullptr; }
}

static struct udev_device* device(FakeDevice& fake) {
    return reinterpret_cast<struct udev_device*>(&fake);
}

static std::string audioConfig() {
    std::ifstream file(test_audio_file);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static void add(FakeDevice& fake) {
    if (isUsbAudioDevice(device(fake))) handleUsbAudioConnected(device(fake));
}

static void remove(FakeDevice& fake) {
    if (isUsbAudioDevice(device(fake))) handleUsbAudioDisconnected(device(fake));
}

int main() {
    assert(mkdtemp(test_dir));
    std::snprintf(test_audio_file, sizeof(test_audio_file), "%s/.asoundrc", test_dir);

    FakeDevice midi = {"controlC1", "/devices/usb1/sound/card1/controlC1", "sound"};
    FakeDevice midi_node = {"midiC1D0", "/devices/usb1/sound/card1/midiC1D0", "sound"};
    FakeDevice capture = {"pcmC2D0c", "/devices/usb2/sound/card2/pcmC2D0c", "sound"};
    FakeDevice internal = {"pcmC0D0p", "/devices/platform/codec/sound/card0/pcmC0D0p", "sound"};
    FakeDevice dac = {"pcmC3D1p", "/devices/usb3/sound/card3/pcmC3D1p", "sound"};
    FakeDevice second_pcm = {"pcmC3D2p", "/devices/usb3/sound/card3/pcmC3D2p", "sound"};
    FakeDevice other_dac = {"pcmC4D0p", "/devices/usb4/sound/card4/pcmC4D0p", "sound"};
    FakeDevice malformed = {"pcmC4D0p-extra", "/devices/usb4/sound/card4/pcmC4D0p-extra", "sound"};

    scan_devices = {&midi, &midi_node, &capture, &internal, &malformed};
    scanExistingUsbAudioDevices(nullptr);
    assert(sink == AUDIO_SINK_DEFAULT && access(test_audio_file, F_OK) != 0);

    add(midi);
    add(capture);
    assert(sink == AUDIO_SINK_DEFAULT && access(test_audio_file, F_OK) != 0);

    scan_devices.push_back(&dac);
    scanExistingUsbAudioDevices(nullptr);
    assert(sink == AUDIO_SINK_USBDAC);
    assert(audioConfig().find("card 3\n    device 1\n") != std::string::npos);
    std::string original = audioConfig();
    add(dac);
    add(second_pcm);
    add(midi);
    remove(midi);
    remove(capture);
    assert(sink == AUDIO_SINK_USBDAC && audioConfig() == original);

    remove(dac);
    assert(sink == AUDIO_SINK_USBDAC);
    assert(audioConfig().find("card 3\n    device 2\n") != std::string::npos);
    add(other_dac);
    original = audioConfig();
    remove(second_pcm);
    assert(sink == AUDIO_SINK_USBDAC && audioConfig() == original);

    sink = AUDIO_SINK_BLUETOOTH;
    writeAudioFile("00:11:22:33:44:55", DEVICE_BLUETOOTH);
    original = audioConfig();
    remove(other_dac);
    assert(sink == AUDIO_SINK_BLUETOOTH && audioConfig() == original);

    add(dac);
    assert(sink == AUDIO_SINK_USBDAC);
    remove(dac);
    assert(sink == AUDIO_SINK_DEFAULT && access(test_audio_file, F_OK) != 0);

    assert(rmdir(test_dir) == 0);
    std::puts("USB audio tests passed");
}
