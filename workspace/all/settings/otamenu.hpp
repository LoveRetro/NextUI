#pragma once

#include "menu.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Over-the-air updater. Fetches NextUI releases from GitHub, downloads the chosen
// package and stages it on the SD card root the same way a manual update would
// (MinUI.zip stays zipped, platform boot folders are extracted). The actual
// install happens on the next boot, which is what lets us update Settings.pak
// while it is running.
namespace OTA
{
    // The desktop build has no WiFi status and nothing to reboot into. There the menu
    // behaves like on a device, but the host counts as online and updates are only
    // staged in the fake SD card.
    inline bool desktopBuild() { return std::string(PLATFORM) == "desktop"; }

    struct Asset
    {
        std::string name;
        std::string url;
        uint64_t size = 0;
    };

    struct Release
    {
        std::string tag;
        std::string sha; // commit the tag points to, empty if unknown
        std::string notes; // release notes, cleaned up for display
        std::vector<Asset> assets;
    };

    class Menu : public MenuList
    {
        friend class VersionItem;

        std::vector<Release> releases;
        int selected = 0;      // index into releases, 0 is the newest
        int installed = -1;    // index of the installed release, -1 if unknown
        bool warned = false;   // downgrade warning accepted for this session
        std::string installedName;

        AbstractMenuItem *versionItem = nullptr;

    public:
        Menu();

        // Fetch releases from GitHub, blocks while showing a cancelable overlay.
        // Returns true on success. On failure `error` holds a user facing message,
        // or stays empty if the user cancelled.
        bool refresh(std::string &error);

    private:
        std::string installedLabel() const;
        void updateStatus();
        void showNotes();
        bool confirmDowngrade();
        InputReactionHint startUpdate(bool full);
    };
}
