extern "C"
{
#include "msettings.h"

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "ra_auth.h"
#include "ra_sync.h"
#include "i18n.h"
}

#include <csignal>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <regex>
#include <thread>
#include <atomic>
#include <mutex>
#include "wifimenu.hpp"
#include "btmenu.hpp"
#include "keyboardprompt.hpp"
#include "colorpickermenu.hpp"

#define BUSYBOX_STOCK_VERSION "1.27.2"

static int appQuit = false;
static bool appSuspend = false;

static void sigHandler(int sig)
{
    switch (sig)
    {
    case SIGINT:
    case SIGTERM:
        appQuit = true;
        break;
    case SIGSTOP:
        appSuspend = true;
        break;
    case SIGCONT:
        appSuspend = false;
        break;
    default:
        break;
    }
}

struct Context
{
    MenuList *menu;
    SDL_Surface *screen;
    int dirty;
    int show_setting;

    // Either the app manages these, or we account for the space and let the menu draw it
    // We could hardcode the behavior down below, but this should also serve as demo code
    // for how to use menu.cpp in different ways depending on the needs of the app
    bool appManagesTitle = false;
    bool appManagesIndicator = true;
    bool appManagesHints = false;
};

// This is all the MinUiSettings stuff, for now just copied over from the old settings app

static const std::vector<std::any> colors = {
    0x000022U, 0x000044U, 0x000066U, 0x000088U, 0x0000AAU, 0x0000CCU, 0x1e2329U, 0x3366FFU, 0x4D7AFFU, 0x6699FFU, 0x80B3FFU, 0x99CCFFU, 0xB3D9FFU,
    0x002222U, 0x004444U, 0x006666U, 0x008888U, 0x00AAAAU, 0x00CCCCU, 0x33FFFFU, 0x4DFFFFU, 0x66FFFFU, 0x80FFFFU, 0x99FFFFU, 0xB3FFFFU,
    0x002200U, 0x004400U, 0x006600U, 0x008800U, 0x00AA00U, 0x00CC00U, 0x33FF33U, 0x4DFF4DU, 0x66FF66U, 0x80FF80U, 0x99FF99U, 0xB3FFB3U,
    0x220022U, 0x440044U, 0x660066U, 0x880088U, 0x9B2257U, 0xAA00AAU, 0xCC00CCU, 0xFF33FFU, 0xFF4DFFU, 0xFF66FFU, 0xFF80FFU, 0xFF99FFU, 0xFFB3FFU,
    0x110022U, 0x220044U, 0x330066U, 0x440088U, 0x5500AAU, 0x6600CCU, 0x8833FFU, 0x994DFFU, 0xAA66FFU, 0xBB80FFU, 0xCC99FFU, 0xDDB3FFU,
    0x220000U, 0x440000U, 0x660000U, 0x880000U, 0xAA0000U, 0xCC0000U, 0xFF3333U, 0xFF4D4DU, 0xFF6666U, 0xFF8080U, 0xFF9999U, 0xFFB3B3U,
    0x222200U, 0x444400U, 0x666600U, 0x888800U, 0xAAAA00U, 0xCCCC00U, 0xFFFF33U, 0xFFFF4DU, 0xFFFF66U, 0xFFFF80U, 0xFFFF99U, 0xFFFFB3U,
    0x221100U, 0x442200U, 0x663300U, 0x884400U, 0xAA5500U, 0xCC6600U, 0xFF8833U, 0xFF994DU, 0xFFAA66U, 0xFFBB80U, 0xFFCC99U, 0xFFDDB3U,
    0x000000U, 0x141414U, 0x282828U, 0x3C3C3CU, 0x505050U, 0x646464U, 0x8C8C8CU, 0xA0A0A0U, 0xB4B4B4U, 0xC8C8C8U, 0xDCDCDCU, 0xFFFFFFU};
// all colors above but as strings
static const std::vector<std::string> color_strings = {
    "0x000022", "0x000044", "0x000066", "0x000088", "0x0000AA", "0x0000CC", "0x1E2329", "0x3366FF", "0x4D7AFF", "0x6699FF", "0x80B3FF", "0x99CCFF", "0xB3D9FF",
    "0x002222", "0x004444", "0x006666", "0x008888", "0x00AAAA", "0x00CCCC", "0x33FFFF", "0x4DFFFF", "0x66FFFF", "0x80FFFF", "0x99FFFF", "0xB3FFFF",
    "0x002200", "0x004400", "0x006600", "0x008800", "0x00AA00", "0x00CC00", "0x33FF33", "0x4DFF4D", "0x66FF66", "0x80FF80", "0x99FF99", "0xB3FFB3",
    "0x220022", "0x440044", "0x660066", "0x880088", "0x9B2257", "0xAA00AA", "0xCC00CC", "0xFF33FF", "0xFF4DFF", "0xFF66FF", "0xFF80FF", "0xFF99FF", "0xFFB3FF",
    "0x110022", "0x220044", "0x330066", "0x440088", "0x5500AA", "0x6600CC", "0x8833FF", "0x994DFF", "0xAA66FF", "0xBB80FF", "0xCC99FF", "0xDDB3FF",
    "0x220000", "0x440000", "0x660000", "0x880000", "0xAA0000", "0xCC0000", "0xFF3333", "0xFF4D4D", "0xFF6666", "0xFF8080", "0xFF9999", "0xFFB3B3",
    "0x222200", "0x444400", "0x666600", "0x888800", "0xAAAA00", "0xCCCC00", "0xFFFF33", "0xFFFF4D", "0xFFFF66", "0xFFFF80", "0xFFFF99", "0xFFFFB3",
    "0x221100", "0x442200", "0x663300", "0x884400", "0xAA5500", "0xCC6600", "0xFF8833", "0xFF994D", "0xFFAA66", "0xFFBB80", "0xFFCC99", "0xFFDDB3",
    "0x000000", "0x141414", "0x282828", "0x3C3C3C", "0x505050", "0x646464", "0x8C8C8C", "0xA0A0A0", "0xB4B4B4", "0xC8C8C8", "0xDCDCDC", "0xFFFFFF"};

static const std::vector<std::string> font_names = {"OG", "Next"};

// Language support: discovered at startup by scanning .system/res/lang/*.lang.
// Always exposes at least "English" as a fallback.
static std::vector<std::any>    language_values;
static std::vector<std::string> language_labels;
static std::vector<std::string> language_codes;

static std::string languageDisplayName(const std::string &code)
{
    if (code == "en") return "English";
    if (code == "fr") return "Français";
    if (code == "de") return "Deutsch";
    if (code == "es") return "Español";
    if (code == "it") return "Italiano";
    if (code == "pt") return "Português";
    if (code == "nl") return "Nederlands";
    if (code == "ja") return "日本語";
    if (code == "ko") return "한국어";
    if (code == "zh") return "中文";
    return code;
}

static void discoverLanguages()
{
    language_codes.clear();
    language_codes.push_back("en");

    DIR *d = opendir(SDCARD_PATH "/.system/res/lang");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name.size() < 6) continue;
            if (name.substr(name.size() - 5) != ".lang") continue;
            std::string code = name.substr(0, name.size() - 5);
            if (code == "en") continue;
            language_codes.push_back(code);
        }
        closedir(d);
    }

    language_values.clear();
    language_labels.clear();
    for (size_t i = 0; i < language_codes.size(); ++i) {
        language_values.push_back((int)i);
        language_labels.push_back(languageDisplayName(language_codes[i]));
    }
}

static int currentLanguageIndex()
{
    const char *code = CFG_getLanguage();
    for (size_t i = 0; i < language_codes.size(); ++i) {
        if (language_codes[i] == code) return (int)i;
    }
    return 0;
}

static const std::vector<std::any>    screen_timeout_secs = {0U, 5U, 10U, 15U, 30U, 45U, 60U, 90U, 120U, 240U, 360U, 600U};
static std::vector<std::string>       screen_timeout_labels = {"Never", "5s", "10s", "15s", "30s", "45s", "60s", "90s", "2m", "4m", "6m", "10m"};

static const std::vector<std::any>    sleep_timeout_secs = {5U, 10U, 15U, 30U, 45U, 60U, 90U, 120U, 240U, 360U, 600U};
static const std::vector<std::string> sleep_timeout_labels = {"5s", "10s", "15s", "30s", "45s", "60s", "90s", "2m", "4m", "6m", "10m"};

static std::vector<std::string> on_off = {"Off", "On"};

static std::vector<std::string> scaling_strings = {"Fullscreen", "Fit", "Fill"};
static const std::vector<std::any> scaling = {(int)GFX_SCALE_FULLSCREEN, (int)GFX_SCALE_FIT, (int)GFX_SCALE_FILL};

// Notification duration options (in seconds)
static const std::vector<std::any> notify_duration_values = {1, 2, 3, 4, 5};
static const std::vector<std::string> notify_duration_labels = {"1s", "2s", "3s", "4s", "5s"};

// Progress notification duration options (in seconds, 0 = disabled)
static const std::vector<std::any> progress_duration_values = {0, 1, 2, 3, 4, 5};
static std::vector<std::string>    progress_duration_labels = {"Off", "1s", "2s", "3s", "4s", "5s"};

// Menu transition mode options
static const std::vector<std::any>    transition_mode_values = {(int)TRANSITION_OFF, (int)TRANSITION_SNAPPY, (int)TRANSITION_COMFY};
static std::vector<std::string>       transition_mode_labels = {"Off", "Snappy", "Comfy"};

// RetroAchievements sort order options
static const std::vector<std::any> ra_sort_values = {
    (int)RA_SORT_UNLOCKED_FIRST,
    (int)RA_SORT_DISPLAY_ORDER_FIRST,
    (int)RA_SORT_DISPLAY_ORDER_LAST,
    (int)RA_SORT_WON_BY_MOST,
    (int)RA_SORT_WON_BY_LEAST,
    (int)RA_SORT_POINTS_MOST,
    (int)RA_SORT_POINTS_LEAST,
    (int)RA_SORT_TITLE_AZ,
    (int)RA_SORT_TITLE_ZA,
    (int)RA_SORT_TYPE_ASC,
    (int)RA_SORT_TYPE_DESC
};
static std::vector<std::string> ra_sort_labels = {
    "Unlocked First",
    "Display Order (First)",
    "Display Order (Last)",
    "Won By (Most)",
    "Won By (Least)",
    "Points (Most)",
    "Points (Least)",
    "Title (A-Z)",
    "Title (Z-A)",
    "Type (Asc)",
    "Type (Desc)"
};

// Called once after I18N is loaded — refresh every globally-cached label vector.
// Numeric labels ("5s", "5%", etc.) and proper nouns (font names) are kept as-is.
static void initLocalizedLabels()
{
    on_off = { "val.off", "val.on" };
    screen_timeout_labels[0] = "val.never";
    scaling_strings = { "val.fullscreen", "val.fit", "val.fill" };
    progress_duration_labels[0] = "val.off";
    transition_mode_labels = { "val.off", "val.snappy", "val.comfy" };
    ra_sort_labels = {
        "ra.sort.unlocked_first",
        "ra.sort.display_first",
        "ra.sort.display_last",
        "ra.sort.won_most",
        "ra.sort.won_least",
        "ra.sort.points_most",
        "ra.sort.points_least",
        "ra.sort.title_az",
        "ra.sort.title_za",
        "ra.sort.type_asc",
        "ra.sort.type_desc",
    };
}

namespace {
    struct ColorDef { int id; const char *name_key; const char *desc_key; uint32_t defaultColor; };
    static const ColorDef g_colorDefs[] = {
        {1, "settings.color.main",                "settings.color.main_desc",                CFG_DEFAULT_COLOR1},
        {2, "settings.color.primary",             "settings.color.primary_desc",             CFG_DEFAULT_COLOR2},
        {3, "settings.color.secondary",           "settings.color.secondary_desc",           CFG_DEFAULT_COLOR3},
        {6, "settings.color.hint",                "settings.color.hint_desc",                CFG_DEFAULT_COLOR6},
        {4, "settings.color.list_text",           "settings.color.list_text_desc",           CFG_DEFAULT_COLOR4},
        {5, "settings.color.list_text_selected",  "settings.color.list_text_selected_desc",  CFG_DEFAULT_COLOR5},
        {7, "settings.color.background",          "settings.color.background_desc",          CFG_DEFAULT_COLOR7},
    };

    static std::vector<ColorPreset> buildColorPresets(int excludeId)
    {
        std::vector<ColorPreset> result;
        for (const auto &def : g_colorDefs)
        {
            if (def.id != excludeId)
                result.push_back({CFG_getColor(def.id), T(def.name_key)});
        }
        return result;
    }

    std::string execCommand(const char* cmd) {
        std::array<char, 128> buffer;
        std::string result;

        // Redirect stderr to stdout using 2>&1
        std::string fullCmd = std::string(cmd) + " 2>&1";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(fullCmd.c_str(), "r"), pclose);
        if (!pipe) throw std::runtime_error("popen() failed!");

        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }

        return result;
    }

    std::string extractBusyBoxVersion(const std::string& output) {
        std::regex versionRegex(R"(BusyBox\s+(v[\d.]+))");
        std::smatch match;
        if (std::regex_search(output, match, versionRegex)) {
            return match.str(1);
        }
        return "";
    }

    class DeviceInfo {
    public:
        enum Vendor {
            Unknown,
            Trimui,
            Miyoo
        };

        enum Model {
            UnknownModel,
            Brick,
            SmartPro,
            SmartProS,
            Flip
        };

        enum Platform {
            UnknownPlatform,
            tg5040,
            tg5050,
            my355
        };

        DeviceInfo() {
            char* device = getenv("DEVICE");
            if (device) {
                if(exactMatch("brick", device)) {
                    m_vendor = Trimui;
                    m_model = Brick;
                    m_platform = tg5040;
                } else if(exactMatch("smartpro", device)) {
                    m_vendor = Trimui;
                    m_model = SmartPro;
                    m_platform = tg5040;
                } else if(exactMatch("smartpros", device)) {
                    m_vendor = Trimui;
                    m_model = SmartProS;
                    m_platform = tg5050;
                } else if(exactMatch("my355", device)) {
                    m_vendor = Trimui;
                    m_model = Flip;
                    m_platform = my355;
                }
            }
        }

        Vendor getVendor() const { return m_vendor; }
        Model getModel() const { return m_model; }
        Platform getPlatform() const { return m_platform; }

        bool hasColorTemperature() const {
            return m_platform == tg5040;
        }

        bool hasContrastSaturation() const {
            return m_platform == my355 || m_platform == tg5040;
        }

        bool hasExposure() const {
            return m_platform == tg5040;
        }

        bool hasActiveCooling() const {
            return m_platform == tg5050;
        }

        bool hasMuteToggle() const {
            return m_platform == tg5050 || m_platform == tg5040;
        }

        bool hasAnalogSticks() const {
            return m_model == SmartPro || m_model == SmartProS;
        }

        bool hasWifi() const {
            return m_platform == tg5050 || m_platform == tg5040 || m_platform == my355;
        }

        bool hasBluetooth() const {
            return m_platform == tg5050 || m_platform == tg5040 || m_platform == my355;
        }

    private:
        Vendor m_vendor = Unknown;
        Model m_model = UnknownModel;
        Platform m_platform = UnknownPlatform;
    };
}
int main(int argc, char *argv[])
{
    try
    {
        DeviceInfo deviceInfo;

        char version[128];
        PLAT_getOsVersionInfo(version, 128);
        LOG_info("This is stock OS version %s\n", version);
        InitSettings();

        PWR_setCPUSpeed(CPU_SPEED_AUTO);

        Context ctx = {0};
        ctx.dirty = 1;
        ctx.show_setting = 0;
        ctx.screen = GFX_init(MODE_MAIN);
        // GFX_init has loaded the language; refresh cached label vectors before
        // any MenuItem is constructed below.
        initLocalizedLabels();
        PAD_init();
        PWR_init();
        TIME_init();

        signal(SIGINT, sigHandler);
        signal(SIGTERM, sigHandler);

        char timezones[MAX_TIMEZONES][MAX_TZ_LENGTH];
        int tz_count = 0;
        TIME_getTimezones(timezones, &tz_count);

        int was_online = PWR_isOnline();
        int had_bt = PLAT_btIsConnected();

        std::vector<std::any> tz_values;
        std::vector<std::string> tz_labels;
        for (int i = 0; i < tz_count; ++i) {
            //LOG_info("Timezone: %s\n", timezones[i]);
            tz_values.push_back(std::string(timezones[i]));
            // Todo: beautify, remove underscores and so on
            tz_labels.push_back(std::string(timezones[i]));
        }

        // Factory helpers to avoid repeating identical lambda boilerplate for each picker
        auto makeColorSetter = [](int id) -> ValueSetCallback {
            return [id](const std::any &v){ CFG_setColor(id, std::any_cast<uint32_t>(v)); };
        };
        auto makeColorOpener = [](ColorPickerMenu *picker, int id, std::string name) -> MenuListCallback {
            return [picker, id, name](AbstractMenuItem &item) -> InputReactionHint {
                picker->reset(CFG_getColor(id), buildColorPresets(id), name);
                return DeferToSubmenu(item);
            };
        };
        auto makeColorGetter = [](int id) -> ValueGetCallback {
            return [id]() -> std::any { return CFG_getColor(id); };
        };
        auto makeColorResetter = [](int id, uint32_t defaultColor) -> ValueResetCallback {
            return [id, defaultColor]() { CFG_setColor(id, defaultColor); };
        };

        // Pre-create one RGB picker per color setting (reused across opens)
        std::vector<std::unique_ptr<ColorPickerMenu>> pickers;
        pickers.reserve(std::size(g_colorDefs));
        for (const auto &def : g_colorDefs)
            pickers.push_back(std::make_unique<ColorPickerMenu>(
                CFG_getColor(def.id), makeColorSetter(def.id), buildColorPresets(def.id), T(def.name_key)));

        // Build color MenuItems (loop order = g_colorDefs order = display order)
        std::vector<AbstractMenuItem *> colorMenuItems;
        colorMenuItems.reserve(std::size(g_colorDefs));
        for (int i = 0; i < (int)std::size(g_colorDefs); i++)
        {
            const auto &def = g_colorDefs[i];
            ColorPickerMenu *picker = pickers[i].get();
            colorMenuItems.push_back(new MenuItem{
                ListItemType::Color, T(def.name_key), T(def.desc_key), colors, color_strings,
                makeColorGetter(def.id),
                makeColorSetter(def.id),
                makeColorResetter(def.id, def.defaultColor),
                makeColorOpener(picker, def.id, T(def.name_key)), picker});
        }

        std::vector<AbstractMenuItem *> appearanceItems;
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.font", "settings.appearance.font_desc", {0, 1}, font_names,
            []() -> std::any{ return CFG_getFontId(); },
            [](const std::any &value){ CFG_setFontId(std::any_cast<int>(value)); },
            []() { CFG_setFontId(CFG_DEFAULT_FONT_ID);}});
        for (auto *item : colorMenuItems)
            appearanceItems.push_back(item);
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.show_battery_pct", "settings.appearance.show_battery_pct_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getShowBatteryPercent(); },
            [](const std::any &value) { CFG_setShowBatteryPercent(std::any_cast<bool>(value)); },
            []() { CFG_setShowBatteryPercent(CFG_DEFAULT_SHOWBATTERYPERCENT);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.show_anim", "settings.appearance.show_anim_desc", {false, true}, on_off,
            []() -> std::any{ return CFG_getMenuAnimations(); },
            [](const std::any &value) { CFG_setMenuAnimations(std::any_cast<bool>(value)); },
            []() { CFG_setMenuAnimations(CFG_DEFAULT_SHOWMENUANIMATIONS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.transitions", "settings.appearance.transitions_desc", transition_mode_values, transition_mode_labels,
            []() -> std::any { return CFG_getMenuTransitions(); },
            [](const std::any &value) { CFG_setMenuTransitions(std::any_cast<int>(value)); },
            []() { CFG_setMenuTransitions(CFG_DEFAULT_SHOWMENUTRANSITIONS); }});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.art_radius", "settings.appearance.art_radius_desc", 0, 24, "px",
            []() -> std::any{ return CFG_getThumbnailRadius(); },
            [](const std::any &value) { CFG_setThumbnailRadius(std::any_cast<int>(value)); },
            []() { CFG_setThumbnailRadius(CFG_DEFAULT_THUMBRADIUS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.art_width", "settings.appearance.art_width_desc",
            5, 100, "%",
            []() -> std::any{ return (int)(CFG_getGameArtWidth() * 100); },
            [](const std::any &value) { CFG_setGameArtWidth((double)std::any_cast<int>(value) / 100.0); },
            []() { CFG_setGameArtWidth(CFG_DEFAULT_GAMEARTWIDTH);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.show_folder_names", "settings.appearance.show_folder_names_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getShowFolderNamesAtRoot(); },
            [](const std::any &value) { CFG_setShowFolderNamesAtRoot(std::any_cast<bool>(value)); },
            []() { CFG_setShowFolderNamesAtRoot(CFG_DEFAULT_SHOWFOLDERNAMESATROOT);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.show_recents", "settings.appearance.show_recents_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getShowRecents(); },
            [](const std::any &value) { CFG_setShowRecents(std::any_cast<bool>(value)); },
            []() { CFG_setShowRecents(CFG_DEFAULT_SHOWRECENTS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.show_tools", "settings.appearance.show_tools_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getShowTools(); },
            [](const std::any &value) { CFG_setShowTools(std::any_cast<bool>(value)); },
            []() { CFG_setShowTools(CFG_DEFAULT_SHOWTOOLS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.show_art", "settings.appearance.show_art_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getShowGameArt(); },
            [](const std::any &value) { CFG_setShowGameArt(std::any_cast<bool>(value)); },
            []() { CFG_setShowGameArt(CFG_DEFAULT_SHOWGAMEART);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.folder_bg", "settings.appearance.folder_bg_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getRomsUseFolderBackground(); },
            [](const std::any &value) { CFG_setRomsUseFolderBackground(std::any_cast<bool>(value)); },
            []() { CFG_setRomsUseFolderBackground(CFG_DEFAULT_ROMSUSEFOLDERBACKGROUND);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, "settings.appearance.qs_ui", "settings.appearance.qs_ui_desc", {false, true}, on_off,
            []() -> std::any{ return CFG_getShowQuickswitcherUI(); },
            [](const std::any &value){ CFG_setShowQuickswitcherUI(std::any_cast<bool>(value)); },
            []() { CFG_setShowQuickswitcherUI(CFG_DEFAULT_SHOWQUICKWITCHERUI);}});
        // not needed anymore
        // new MenuItem{ListItemType::Generic, "Game switcher scaling", "The scaling algorithm used to display the savegame image.", scaling, scaling_strings, []() -> std::any
        // { return CFG_getGameSwitcherScaling(); },
        // [](const std::any &value)
        // { CFG_setGameSwitcherScaling(std::any_cast<int>(value)); },
        // []() { CFG_setGameSwitcherScaling(CFG_DEFAULT_GAMESWITCHERSCALING);}},
        appearanceItems.push_back(new MenuItem{ListItemType::Button, "settings.reset_defaults", "settings.reset_defaults_desc", ResetCurrentMenu});
        auto *appearanceMenu = new MenuList(MenuItemType::Fixed, "settings.section.appearance", std::move(appearanceItems));

        std::vector<AbstractMenuItem*> displayItems = {
            new MenuItem{ListItemType::Generic, "settings.display.brightness", "settings.display.brightness_desc", 0, 10, "",[]() -> std::any
            { return GetBrightness(); }, [](const std::any &value)
            { SetBrightness(std::any_cast<int>(value)); },
            []() { SetBrightness(SETTINGS_DEFAULT_BRIGHTNESS);}},

        };

        if(deviceInfo.hasColorTemperature())
        {
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, "settings.display.colortemp", "settings.display.colortemp_desc", 0, 40, "",[]() -> std::any
                { return GetColortemp(); }, [](const std::any &value)
                { SetColortemp(std::any_cast<int>(value)); },
                []() { SetColortemp(SETTINGS_DEFAULT_COLORTEMP);}});
        }

        if(deviceInfo.hasContrastSaturation())
        {
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, "settings.display.contrast", "settings.display.contrast_desc", -4, 5, "",[]() -> std::any
                { return GetContrast(); }, [](const std::any &value)
                { SetContrast(std::any_cast<int>(value)); },
                []() { SetContrast(SETTINGS_DEFAULT_CONTRAST);}});
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, "settings.display.saturation", "settings.display.saturation_desc", -5, 5, "",[]() -> std::any
                { return GetSaturation(); }, [](const std::any &value)
                { SetSaturation(std::any_cast<int>(value)); },
                []() { SetSaturation(SETTINGS_DEFAULT_SATURATION);}});
        }

        if(deviceInfo.hasExposure())
        {
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, "settings.display.exposure", "settings.display.exposure_desc", -4, 5, "",[]() -> std::any
                { return GetExposure(); }, [](const std::any &value)
                { SetExposure(std::any_cast<int>(value)); },
                []() { SetExposure(SETTINGS_DEFAULT_EXPOSURE);}});
        }
        displayItems.push_back(
            new MenuItem{ListItemType::Button, "settings.reset_defaults", "settings.reset_defaults_desc", ResetCurrentMenu});

        auto displayMenu = new MenuList(MenuItemType::Fixed, "settings.section.display", displayItems);

        std::vector<AbstractMenuItem*> systemItems = {
            new MenuItem{ListItemType::Generic, "settings.audio.volume", "settings.audio.volume_desc",
            {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20},
            {"val.muted", "5%","10%","15%","20%","25%","30%","35%","40%","45%","50%","55%","60%","65%","70%","75%","80%","85%","90%","95%","100%"},
            []() -> std::any{ return GetVolume(); }, [](const std::any &value)
            { SetVolume(std::any_cast<int>(value)); },
            []() { SetVolume(SETTINGS_DEFAULT_VOLUME);}},
            new MenuItem{ListItemType::Generic, "settings.system.screen_timeout", "settings.system.screen_timeout_desc", screen_timeout_secs, screen_timeout_labels, []() -> std::any
            { return CFG_getScreenTimeoutSecs(); }, [](const std::any &value)
            { CFG_setScreenTimeoutSecs(std::any_cast<uint32_t>(value)); },
            []() { CFG_setScreenTimeoutSecs(CFG_DEFAULT_SCREENTIMEOUTSECS);}},
            new MenuItem{ListItemType::Generic, "settings.system.suspend_timeout", "settings.system.suspend_timeout_desc", sleep_timeout_secs, sleep_timeout_labels, []() -> std::any
            { return CFG_getSuspendTimeoutSecs(); }, [](const std::any &value)
            { CFG_setSuspendTimeoutSecs(std::any_cast<uint32_t>(value)); },
            []() { CFG_setSuspendTimeoutSecs(CFG_DEFAULT_SUSPENDTIMEOUTSECS);}},
            new MenuItem{ListItemType::Generic, "settings.system.haptic", "settings.system.haptic_desc", {false, true}, on_off, []() -> std::any
            { return CFG_getHaptics(); }, [](const std::any &value)
            { CFG_setHaptics(std::any_cast<bool>(value)); },
            []() { CFG_setHaptics(CFG_DEFAULT_HAPTICS);}},
            new MenuItem{ListItemType::Generic, "settings.system.default_view", "settings.system.default_view_desc",
            {(int)SCREEN_GAMELIST, (int)SCREEN_GAMESWITCHER, (int)SCREEN_QUICKMENU},
            {"settings.system.view.content_list","settings.system.view.game_switcher","settings.system.view.quick_menu"},
            []() -> std::any { return CFG_getDefaultView(); },
            [](const std::any &value){ CFG_setDefaultView(std::any_cast<int>(value)); },
            []() { CFG_setDefaultView(CFG_DEFAULT_VIEW);}},
            new MenuItem{ListItemType::Generic, "settings.system.clock_24h", "settings.system.clock_24h_desc", {false, true}, on_off, []() -> std::any
            { return CFG_getClock24H(); },
            [](const std::any &value)
            { CFG_setClock24H(std::any_cast<bool>(value)); },
            []() { CFG_setClock24H(CFG_DEFAULT_CLOCK24H);}},
            new MenuItem{ListItemType::Generic, "settings.system.show_clock", "settings.system.show_clock_desc", {false, true}, on_off, []() -> std::any
            { return CFG_getShowClock(); },
            [](const std::any &value)
            { CFG_setShowClock(std::any_cast<bool>(value)); },
            []() { CFG_setShowClock(CFG_DEFAULT_SHOWCLOCK);}},
            new MenuItem{ListItemType::Generic, "settings.system.auto_time", "settings.system.auto_time_desc", {false, true}, on_off, []() -> std::any
            { return TIME_getNetworkTimeSync(); }, [](const std::any &value)
            { TIME_setNetworkTimeSync(std::any_cast<bool>(value)); },
            []() { TIME_setNetworkTimeSync(false);}}, // default from stock
            new MenuItem{ListItemType::Generic, "settings.system.timezone", "settings.system.timezone_desc", tz_values, tz_labels, []() -> std::any
            { return std::string(TIME_getCurrentTimezone()); }, [](const std::any &value)
            { TIME_setCurrentTimezone(std::any_cast<std::string>(value).c_str()); },
            []() { TIME_setCurrentTimezone("Asia/Shanghai");}}, // default from Stock
            new MenuItem{ListItemType::Generic, "settings.system.save_format", "settings.system.save_format_desc",
            {(int)SAVE_FORMAT_SAV, (int)SAVE_FORMAT_SRM, (int)SAVE_FORMAT_SRM_UNCOMPRESSED, (int)SAVE_FORMAT_GEN},
            {"val.fmt.minui", "val.fmt.retroarch_compressed", "val.fmt.retroarch_uncompressed", "val.fmt.generic"}, []() -> std::any
            { return CFG_getSaveFormat(); }, [](const std::any &value)
            { CFG_setSaveFormat(std::any_cast<int>(value)); },
            []() { CFG_setSaveFormat(CFG_DEFAULT_SAVEFORMAT);}},
            new MenuItem{ListItemType::Generic, "settings.system.save_state_format", "settings.system.save_state_format_desc",
            {(int)STATE_FORMAT_SAV, (int)STATE_FORMAT_SRM_EXTRADOT, (int)STATE_FORMAT_SRM_UNCOMRESSED_EXTRADOT, (int)STATE_FORMAT_SRM, (int)STATE_FORMAT_SRM_UNCOMRESSED},
            {"val.fmt.minui", "val.fmt.retroarch_ish_compressed", "val.fmt.retroarch_ish_uncompressed", "val.fmt.retroarch_compressed", "val.fmt.retroarch_uncompressed"}, []() -> std::any
            { return CFG_getStateFormat(); }, [](const std::any &value)
            { CFG_setStateFormat(std::any_cast<int>(value)); },
            []() { CFG_setStateFormat(CFG_DEFAULT_STATEFORMAT);}},
            new MenuItem{ListItemType::Generic, "settings.system.extracted_name", "settings.system.extracted_name_desc", {false, true}, on_off,
            []() -> std::any{ return CFG_getUseExtractedFileName(); },
            [](const std::any &value){ CFG_setUseExtractedFileName(std::any_cast<bool>(value)); },
            []() { CFG_setUseExtractedFileName(CFG_DEFAULT_EXTRACTEDFILENAME);}}
        };

        if(deviceInfo.getPlatform() == DeviceInfo::tg5040)
        {
            systemItems.push_back(
                new MenuItem{ListItemType::Generic, "settings.system.safe_poweroff", "settings.system.safe_poweroff_desc", {false, true}, on_off,
                []() -> std::any { return CFG_getPowerOffProtection(); },
                [](const std::any &value) { CFG_setPowerOffProtection(std::any_cast<bool>(value)); },
                []() { CFG_setPowerOffProtection(CFG_DEFAULT_POWEROFFPROTECTION); }}
            );
        }

        if(deviceInfo.hasActiveCooling())
        {
            systemItems.push_back(
                new MenuItem{ListItemType::Generic, "settings.system.fan_speed", "settings.system.fan_speed_desc",
                {-3,-2,-1,0,10,20,30,40,50,60,70,80,90,100}, {"settings.system.fan.performance","settings.system.fan.normal","settings.system.fan.quiet","0%","10%","20%","30%","40%","50%","60%","70%","80%","90%","100%"},
                []() -> std::any { return GetFanSpeed(); },
                [](const std::any &value){ SetFanSpeed(std::any_cast<int>(value)); },
                []() { SetFanSpeed(SETTINGS_DEFAULT_FAN_SPEED); }}
            );
        }

        discoverLanguages();
        systemItems.push_back(
            new MenuItem{ListItemType::Generic,
                         (char *)T("settings.language.title"),
                         (char *)T("settings.language.desc"),
                         language_values, language_labels,
                         []() -> std::any { return currentLanguageIndex(); },
                         [](const std::any &value) {
                             int idx = std::any_cast<int>(value);
                             if (idx < 0 || (size_t)idx >= language_codes.size()) return;
                             const std::string &code = language_codes[idx];
                             CFG_setLanguage(code.c_str());
                             I18N_reload(code.c_str());
                         },
                         []() {
                             CFG_setLanguage("en");
                             I18N_reload("en");
                         }});

        systemItems.push_back(
            new MenuItem{ListItemType::Button, "settings.reset_defaults", "settings.reset_defaults_desc", ResetCurrentMenu});

        auto systemMenu = new MenuList(MenuItemType::Fixed, "settings.section.system", systemItems);

        std::vector<AbstractMenuItem*> muteItems =
        {
            new MenuItem{ListItemType::Generic, "settings.mute.volume_toggled", "settings.mute.volume_toggled_desc",
            {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20},
            {"val.unchanged", "val.muted", "5%","10%","15%","20%","25%","30%","35%","40%","45%","50%","55%","60%","65%","70%","75%","80%","85%","90%","95%","100%"},
            []() -> std::any { return GetMutedVolume(); },
            [](const std::any &value) { SetMutedVolume(std::any_cast<int>(value)); },
            []() { SetMutedVolume(0); }},
            new MenuItem{ListItemType::Generic, "settings.mute.fn_disables_led", "settings.mute.fn_disables_led_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getMuteLEDs(); },
            [](const std::any &value) { CFG_setMuteLEDs(std::any_cast<bool>(value)); },
            []() { CFG_setMuteLEDs(CFG_DEFAULT_MUTELEDS); }},
            new MenuItem{ListItemType::Generic, "settings.mute.brightness_toggled", "settings.mute.brightness_toggled_desc",
            {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, 0,1,2,3,4,5,6,7,8,9,10},
            {"val.unchanged","0","1","2","3","4","5","6","7","8","9","10"},
            []() -> std::any { return GetMutedBrightness(); }, [](const std::any &value)
            { SetMutedBrightness(std::any_cast<int>(value)); },
            []() { SetMutedBrightness(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}},
        };

        if(deviceInfo.hasMuteToggle())
        {
            if(deviceInfo.hasColorTemperature()) {
                muteItems.push_back(
                    new MenuItem{ListItemType::Generic, "settings.mute.colortemp_toggled", "settings.mute.colortemp_toggled_desc",
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40},
                    {"val.unchanged","0","1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17","18","19","20","21","22","23","24","25","26","27","28","29","30","31","32","33","34","35","36","37","38","39","40"},
                    []() -> std::any{ return GetMutedColortemp(); }, [](const std::any &value)
                    { SetMutedColortemp(std::any_cast<int>(value)); },
                    []() { SetMutedColortemp(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}}
                );
            }
            if(deviceInfo.hasContrastSaturation()) {
                muteItems.insert(muteItems.end(), {
                    new MenuItem{ListItemType::Generic, "settings.mute.contrast_toggled", "settings.mute.contrast_toggled_desc",
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, -4,-3,-2,-1,0,1,2,3,4,5},
                    {"val.unchanged","-4","-3","-2","-1","0","1","2","3","4","5"},
                    []() -> std::any  { return GetMutedContrast(); }, [](const std::any &value)
                    { SetMutedContrast(std::any_cast<int>(value)); },
                    []() { SetMutedContrast(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}},
                    new MenuItem{ListItemType::Generic, "settings.mute.saturation_toggled", "settings.mute.saturation_toggled_desc",
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, -5,-4,-3,-2,-1,0,1,2,3,4,5},
                    {"val.unchanged","-5","-4","-3","-2","-1","0","1","2","3","4","5"},
                    []() -> std::any{ return GetMutedSaturation(); }, [](const std::any &value)
                    { SetMutedSaturation(std::any_cast<int>(value)); },
                    []() { SetMutedSaturation(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}}}
                );
            }
            if(deviceInfo.hasExposure()) {
                muteItems.push_back(
                    new MenuItem{ListItemType::Generic, "settings.mute.exposure_toggled", "settings.mute.exposure_toggled_desc",
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, -4,-3,-2,-1,0,1,2,3,4,5},
                    {"val.unchanged","-4","-3","-2","-1","0","1","2","3","4","5"},
                    []() -> std::any  { return GetMutedExposure(); }, [](const std::any &value)
                    { SetMutedExposure(std::any_cast<int>(value)); },
                    []() { SetMutedExposure(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}}
                );
            }

            muteItems.insert(muteItems.end(), {
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.a", "settings.mute.turbo.a_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboA(); },
                [](const std::any &value) { SetMuteTurboA(std::any_cast<int>(value));},
                []() { SetMuteTurboA(0);}},
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.b", "settings.mute.turbo.b_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboB(); },
                [](const std::any &value) { SetMuteTurboB(std::any_cast<int>(value));},
                []() { SetMuteTurboB(0);}},
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.x", "settings.mute.turbo.x_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboX(); },
                [](const std::any &value) { SetMuteTurboX(std::any_cast<int>(value));},
                []() { SetMuteTurboX(0);}},
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.y", "settings.mute.turbo.y_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboY(); },
                [](const std::any &value) { SetMuteTurboY(std::any_cast<int>(value));},
                []() { SetMuteTurboY(0);}},
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.l1", "settings.mute.turbo.l1_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboL1(); },
                [](const std::any &value) { SetMuteTurboL1(std::any_cast<int>(value));},
                []() { SetMuteTurboL1(0);}},
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.l2", "settings.mute.turbo.l2_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboL2(); },
                [](const std::any &value) { SetMuteTurboL2(std::any_cast<int>(value));},
                []() { SetMuteTurboL2(0);}},
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.r1", "settings.mute.turbo.r1_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboR1(); },
                [](const std::any &value) { SetMuteTurboR1(std::any_cast<int>(value));},
                []() { SetMuteTurboR1(0);}},
                new MenuItem{ListItemType::Generic, "settings.mute.turbo.r2", "settings.mute.turbo.r2_desc", {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboR2(); },
                [](const std::any &value) { SetMuteTurboR2(std::any_cast<int>(value));},
                []() { SetMuteTurboR2(0);}}
            });
        }

        if(deviceInfo.hasMuteToggle() && !deviceInfo.hasAnalogSticks()){
            muteItems.push_back(
                new MenuItem{ListItemType::Generic, "settings.mute.dpad_mode", "settings.mute.dpad_mode_desc", {0, 1, 2}, {"val.dpad", "val.joystick", "val.both"}, []() -> std::any
                {
                    if(!GetMuteDisablesDpad() && !GetMuteEmulatesJoystick()) return 0;
                    if(GetMuteDisablesDpad() && GetMuteEmulatesJoystick()) return 1;
                    return 2;
                },
                [](const std::any &value)
                {
                    int v = std::any_cast<int>(value);
                    SetMuteDisablesDpad((v == 1));
                    SetMuteEmulatesJoystick((v > 0));
                },
                []()
                {
                    SetMuteDisablesDpad(0);
                    SetMuteEmulatesJoystick(0);
                }});
        }
        muteItems.push_back(new MenuItem{ListItemType::Button, "settings.reset_defaults", "settings.reset_defaults_desc", ResetCurrentMenu});

        auto notificationsMenu = new MenuList(MenuItemType::Fixed, "settings.notif.title",
        {
            new MenuItem{ListItemType::Generic, "settings.notif.save_states", "settings.notif.save_states_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getNotifyManualSave(); },
            [](const std::any &value) { CFG_setNotifyManualSave(std::any_cast<bool>(value)); },
            []() { CFG_setNotifyManualSave(CFG_DEFAULT_NOTIFY_MANUAL_SAVE);}},
            new MenuItem{ListItemType::Generic, "settings.notif.load_states", "settings.notif.load_states_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getNotifyLoad(); },
            [](const std::any &value) { CFG_setNotifyLoad(std::any_cast<bool>(value)); },
            []() { CFG_setNotifyLoad(CFG_DEFAULT_NOTIFY_LOAD);}},
            new MenuItem{ListItemType::Generic, "settings.notif.screenshots", "settings.notif.screenshots_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getNotifyScreenshot(); },
            [](const std::any &value) { CFG_setNotifyScreenshot(std::any_cast<bool>(value)); },
            []() { CFG_setNotifyScreenshot(CFG_DEFAULT_NOTIFY_SCREENSHOT);}},
            new MenuItem{ListItemType::Generic, "settings.notif.vol_display", "settings.notif.vol_display_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getNotifyAdjustments(); },
            [](const std::any &value) { CFG_setNotifyAdjustments(std::any_cast<bool>(value)); },
            []() { CFG_setNotifyAdjustments(CFG_DEFAULT_NOTIFY_ADJUSTMENTS);}},
            new MenuItem{ListItemType::Generic, "settings.notif.duration", "settings.notif.duration_desc", notify_duration_values, notify_duration_labels,
            []() -> std::any { return CFG_getNotifyDuration(); },
            [](const std::any &value) { CFG_setNotifyDuration(std::any_cast<int>(value)); },
            []() { CFG_setNotifyDuration(CFG_DEFAULT_NOTIFY_DURATION);}},
            new MenuItem{ListItemType::Button, "settings.reset_defaults", "settings.reset_defaults_desc", ResetCurrentMenu},
        });

        // RetroAchievements keyboard prompts
        auto raUsernamePrompt = new KeyboardPrompt("settings.ra.username", [](AbstractMenuItem &item) -> InputReactionHint {
            CFG_setRAUsername(item.getName().c_str());
            return Exit;
        });

        auto raPasswordPrompt = new KeyboardPrompt("settings.ra.password", [](AbstractMenuItem &item) -> InputReactionHint {
            CFG_setRAPassword(item.getName().c_str());
            return Exit;
        });

        auto retroAchievementsMenu = new MenuList(MenuItemType::Fixed, "settings.ra.title",
        {
            new MenuItem{ListItemType::Generic, "settings.ra.enable", "settings.ra.enable_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getRAEnable(); },
            [](const std::any &value) { CFG_setRAEnable(std::any_cast<bool>(value)); },
            []() { CFG_setRAEnable(CFG_DEFAULT_RA_ENABLE);}},
            new TextInputMenuItem{"settings.ra.username", "settings.ra.username_desc",
            []() -> std::any {
                std::string username = CFG_getRAUsername();
                return username.empty() ? std::string("settings.ra.not_set") : username;
            },
            [raUsernamePrompt](AbstractMenuItem &item) -> InputReactionHint {
                raUsernamePrompt->setInitialText(CFG_getRAUsername());
                item.defer(true);
                return NoOp;
            }, raUsernamePrompt},
            new TextInputMenuItem{"settings.ra.password", "settings.ra.password_desc",
            []() -> std::any {
                std::string password = CFG_getRAPassword();
                return password.empty() ? std::string("settings.ra.not_set") : std::string("********");
            },
            [raPasswordPrompt](AbstractMenuItem &item) -> InputReactionHint {
                raPasswordPrompt->setInitialText(CFG_getRAPassword());
                item.defer(true);
                return NoOp;
            }, raPasswordPrompt},
            new MenuItem{ListItemType::Button, "settings.ra.authenticate", "settings.ra.authenticate_desc",
            [](AbstractMenuItem &item) -> InputReactionHint {
                const char* username = CFG_getRAUsername();
                const char* password = CFG_getRAPassword();

                if (!username || strlen(username) == 0 || !password || strlen(password) == 0) {
                    item.setDesc("settings.ra.err_credentials");
                    return NoOp;
                }

                item.setDesc("settings.ra.authenticating");

                RA_AuthResponse response;
                RA_AuthResult result = RA_authenticateSync(username, password, &response);

                if (result == RA_AUTH_SUCCESS) {
                    CFG_setRAToken(response.token);
                    CFG_setRAAuthenticated(true);
                    char buf[256];
                    snprintf(buf, sizeof(buf), "settings.ra.authenticated_as", response.display_name);
                    item.setDesc(buf);
                } else {
                    CFG_setRAToken("");
                    CFG_setRAAuthenticated(false);
                    char buf[256];
                    snprintf(buf, sizeof(buf), "settings.ra.err_prefix", response.error_message);
                    item.setDesc(buf);
                }
                return NoOp;
            }},
            new StaticMenuItem{ListItemType::Generic, "settings.ra.status", "settings.ra.status_desc",
            []() -> std::any {
                if (CFG_getRAAuthenticated() && strlen(CFG_getRAToken()) > 0) {
                    return std::string("settings.ra.status_yes");
                }
                return std::string("settings.ra.status_no");
            }},
            // TODO: Hardcore mode hidden until feature is fully implemented and ready for the emulator approval process done by the RetroAchievements team
            // new MenuItem{ListItemType::Generic, "Hardcore Mode", "Disable save states and cheats for achievements", {false, true}, on_off,
            // []() -> std::any { return CFG_getRAHardcoreMode(); },
            // [](const std::any &value) { CFG_setRAHardcoreMode(std::any_cast<bool>(value)); },
            // []() { CFG_setRAHardcoreMode(CFG_DEFAULT_RA_HARDCOREMODE);}},
            new MenuItem{ListItemType::Generic, "settings.ra.show_notif", "settings.ra.show_notif_desc", {false, true}, on_off,
            []() -> std::any { return CFG_getRAShowNotifications(); },
            [](const std::any &value) { CFG_setRAShowNotifications(std::any_cast<bool>(value)); },
            []() { CFG_setRAShowNotifications(CFG_DEFAULT_RA_SHOW_NOTIFICATIONS);}},
            new MenuItem{ListItemType::Generic, "settings.ra.notif_duration", "settings.ra.notif_duration_desc", notify_duration_values, notify_duration_labels,
            []() -> std::any { return CFG_getRANotificationDuration(); },
            [](const std::any &value) { CFG_setRANotificationDuration(std::any_cast<int>(value)); },
            []() { CFG_setRANotificationDuration(CFG_DEFAULT_RA_NOTIFICATION_DURATION);}},
            new MenuItem{ListItemType::Generic, "settings.ra.progress_duration", "settings.ra.progress_duration_desc", progress_duration_values, progress_duration_labels,
            []() -> std::any { return CFG_getRAProgressNotificationDuration(); },
            [](const std::any &value) { CFG_setRAProgressNotificationDuration(std::any_cast<int>(value)); },
            []() { CFG_setRAProgressNotificationDuration(CFG_DEFAULT_RA_PROGRESS_NOTIFICATION_DURATION);}},
            new MenuItem{ListItemType::Generic, "settings.ra.sort", "settings.ra.sort_desc", ra_sort_values, ra_sort_labels,
            []() -> std::any { return CFG_getRAAchievementSortOrder(); },
            [](const std::any &value) { CFG_setRAAchievementSortOrder(std::any_cast<int>(value)); },
            []() { CFG_setRAAchievementSortOrder(CFG_DEFAULT_RA_ACHIEVEMENT_SORT_ORDER);}},
            new MenuItem{ListItemType::Button, "settings.ra.sync",
            []() -> std::string {
                uint32_t count = 0;
                RA_Sync_hasPendingUnlocks(&count);
                if (count > 0) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "settings.ra.sync_pending_fmt", count);
                    return std::string(buf);
                }
                return std::string("settings.ra.sync_no_pending");
            }(),
            [](AbstractMenuItem &item) -> InputReactionHint {
                // Check authentication
                if (!CFG_getRAAuthenticated() || strlen(CFG_getRAToken()) == 0) {
                    item.setDesc("settings.ra.sync_not_auth");
                    return NoOp;
                }

                // Check for pending unlocks
                uint32_t pending = 0;
                if (!RA_Sync_hasPendingUnlocks(&pending) || pending == 0) {
                    item.setDesc("settings.ra.sync_no_pending");
                    return NoOp;
                }

                // Show initial overlay with cancel hint
                char msg[256];
                snprintf(msg, sizeof(msg), "settings.ra.sync_inprogress_fmt",
                         pending, pending == 1 ? "" : "s", pending);
                MenuList::showOverlay(msg, OverlayDismissMode::None);

                // Shared state between sync thread and main thread
                SDL_atomic_t cancel;
                SDL_AtomicSet(&cancel, 0);
                std::atomic<bool> done{false};
                std::mutex progress_mutex;
                std::string progress_msg;
                std::atomic<bool> progress_dirty{false};
                RA_SyncResult sync_result = {0, 0, 0, 0};

                // Progress callback updates shared message string
                struct ProgressCtx {
                    std::mutex* mutex;
                    std::atomic<bool>* dirty;
                    std::string* msg;
                    uint32_t total;
                };
                ProgressCtx pctx = {&progress_mutex, &progress_dirty, &progress_msg, pending};

                // Launch sync on background thread (game_id=0 for all games, NULL config for interactive defaults)
                std::thread sync_thread([&]() {
                    sync_result = RA_Sync_syncAll(0, NULL, &cancel,
                        [](uint32_t current, uint32_t total, bool success, void* userdata) {
                            auto* ctx = static_cast<ProgressCtx*>(userdata);
                            char buf[256];
                            snprintf(buf, sizeof(buf), "settings.ra.sync_progress_fmt",
                                     current, ctx->total);
                            {
                                std::lock_guard<std::mutex> lock(*ctx->mutex);
                                *ctx->msg = buf;
                            }
                            ctx->dirty->store(true);
                        }, &pctx);
                    done.store(true);
                });

                // Main thread: poll for B-button cancel and update overlay
                while (!done.load()) {
                    GFX_startFrame();
                    PAD_poll();

                    if (PAD_justPressed(BTN_B)) {
                        SDL_AtomicSet(&cancel, 1);
                        MenuList::showOverlay("settings.ra.sync_cancelling", OverlayDismissMode::None);
                    }

                    // Update overlay if progress changed
                    if (progress_dirty.exchange(false)) {
                        std::string current_msg;
                        {
                            std::lock_guard<std::mutex> lock(progress_mutex);
                            current_msg = progress_msg;
                        }
                        if (!SDL_AtomicGet(&cancel)) {
                            MenuList::showOverlay(current_msg, OverlayDismissMode::None);
                        }
                    }

                    GFX_sync();
                }

                sync_thread.join();
                MenuList::hideOverlay();

                // Update button description with result
                if (SDL_AtomicGet(&cancel) && sync_result.synced == 0) {
                    item.setDesc("settings.ra.sync_cancelled");
                } else if (SDL_AtomicGet(&cancel) && sync_result.synced > 0) {
                    snprintf(msg, sizeof(msg), "settings.ra.sync_cancelled_partial_fmt",
                             sync_result.synced, sync_result.total);
                    item.setDesc(msg);
                } else if (sync_result.failed > 0) {
                    snprintf(msg, sizeof(msg), "settings.ra.sync_incomplete_fmt",
                             sync_result.synced);
                    item.setDesc(msg);
                } else if (sync_result.synced > 0) {
                    snprintf(msg, sizeof(msg), "settings.ra.sync_synced_fmt",
                             sync_result.synced, sync_result.synced == 1 ? "" : "s");
                    item.setDesc(msg);
                } else {
                    item.setDesc("settings.ra.sync_no_pending");
                }

                return NoOp;
            }},
            new MenuItem{ListItemType::Button, "settings.reset_defaults", "settings.reset_defaults_desc", ResetCurrentMenu},
        });

        auto minarchMenu = new MenuList(MenuItemType::List, "settings.ingame.title",
        {
            new MenuItem{ListItemType::Generic, "settings.ingame.notifications", "settings.ingame.notifications_desc", {}, {}, nullptr, nullptr, DeferToSubmenu, notificationsMenu},
            new MenuItem{ListItemType::Generic, "settings.ra.in_game", "settings.ra.in_game_desc", {}, {}, nullptr, nullptr, DeferToSubmenu, retroAchievementsMenu},
        });

        // We need to alert the user about potential issues if the
        // stock OS was modified in way that are known to cause issues
        std::string bbver = extractBusyBoxVersion(execCommand("cat --help"));
        if (bbver.empty())
            bbver = "settings.about.busybox_missing";
        else if(deviceInfo.getPlatform() == DeviceInfo::tg5040 && bbver.find(BUSYBOX_STOCK_VERSION) == std::string::npos)
            ctx.menu->showOverlay(
                "settings.stock_warning",
                OverlayDismissMode::DismissOnA);

        auto aboutMenu = new MenuList(MenuItemType::Fixed, "settings.about.title",
        {
            new StaticMenuItem{ListItemType::Generic, "settings.about.version", "",
            []() -> std::any {
                std::ifstream t(ROOT_SYSTEM_PATH "/version.txt");
                std::stringstream buffer;
                buffer << t.rdbuf();
                return buffer.str();
            }},
            new StaticMenuItem{ListItemType::Generic, "settings.about.platform", "",
            []() -> std::any {
                return std::string(PLAT_getModel()); }
            },
            new StaticMenuItem{ListItemType::Generic, "settings.about.os_version", "",
            []() -> std::any {
                char osver[128];
                PLAT_getOsVersionInfo(osver, 128);
                return std::string(osver); }
            },
            new StaticMenuItem{ListItemType::Generic, "settings.about.busybox", "",
            [&]() -> std::any { return bbver; }
            },
        });

        std::vector<AbstractMenuItem*> mainItems = {
            new MenuItem{ListItemType::Generic, "settings.main.appearance", "settings.main.appearance_desc", {}, {}, nullptr, nullptr, DeferToSubmenu, appearanceMenu},
            new MenuItem{ListItemType::Generic, "settings.main.display", "", {}, {}, nullptr, nullptr, DeferToSubmenu, displayMenu},
            new MenuItem{ListItemType::Generic, "settings.main.system", "", {}, {}, nullptr, nullptr, DeferToSubmenu, systemMenu},
        };

        if(deviceInfo.hasMuteToggle())
            mainItems.push_back(new MenuItem{ListItemType::Generic, "settings.fn.main", "settings.fn.main_desc", {}, {}, nullptr, nullptr, DeferToSubmenu,
                new MenuList(MenuItemType::Fixed, "settings.fn.title", muteItems)});

        mainItems.push_back(new MenuItem{ListItemType::Generic, "settings.ingame.main", "settings.ingame.main_desc", {}, {}, nullptr, nullptr, DeferToSubmenu, minarchMenu});

        if(deviceInfo.hasWifi())
            mainItems.push_back(new MenuItem{ListItemType::Generic, "settings.main.network", "", {}, {}, nullptr, nullptr, DeferToSubmenu, new Wifi::Menu(appQuit, ctx.dirty)});

        if(deviceInfo.hasBluetooth())
            mainItems.push_back(new MenuItem{ListItemType::Generic, "settings.main.bluetooth", "", {}, {}, nullptr, nullptr, DeferToSubmenu, new Bluetooth::Menu(appQuit, ctx.dirty)});

        mainItems.push_back(new MenuItem{ListItemType::Generic, "settings.about.main", "", {}, {}, nullptr, nullptr, DeferToSubmenu, aboutMenu});

        ctx.menu = new MenuList(MenuItemType::List, "Main", mainItems);

        SDL_Surface* bgbmp = IMG_Load(SDCARD_PATH "/bg.png");
        SDL_Surface* convertedbg = SDL_ConvertSurfaceFormat(bgbmp, SDL_PIXELFORMAT_RGB565, 0);
        if (convertedbg) {
            SDL_FreeSurface(bgbmp);
            SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, ctx.screen->w, ctx.screen->h, 32, SDL_PIXELFORMAT_RGB565);
            GFX_blitScaleToFill(convertedbg, scaled);
            bgbmp = scaled;
        }

        // main content (list)
        // PADDING all around
        SDL_Rect listRect = {SCALE1(PADDING), SCALE1(PADDING), ctx.screen->w - SCALE1(PADDING * 2), ctx.screen->h - SCALE1(PADDING * 2)};
        SDL_Rect titleRect = {0, 0, 0, 0};
        // PILL_SIZE above (if showing title)
        if (ctx.appManagesTitle || ctx.appManagesIndicator)
            listRect = dy(listRect, SCALE1(PILL_SIZE));
        // BUTTON_SIZE below (if showing hints)
        if (ctx.appManagesHints)
            listRect.h -= SCALE1(BUTTON_SIZE);
        ctx.menu->performLayout(listRect);

        while (!appQuit)
        {
            GFX_startFrame();
            PAD_poll();

            ctx.menu->handleInput(ctx.dirty, appQuit);

            PWR_update(&ctx.dirty, &ctx.show_setting, nullptr, nullptr);

            int is_online = PWR_isOnline();
            if (was_online!=is_online)
                ctx.dirty = 1;
            was_online = is_online;

            int has_bt = PLAT_btIsConnected();
            if (had_bt != has_bt)
                ctx.dirty = 1;
            had_bt = has_bt;

            if (ctx.dirty)
            {
                GFX_clear(ctx.screen);
                if(bgbmp) {
                    SDL_Rect image_rect = {0, 0, ctx.screen->w, ctx.screen->h};
                    SDL_BlitSurface(bgbmp, NULL, ctx.screen, &image_rect);
                } else {
                    uint32_t bgc = CFG_getColor(COLOR_BACKGROUND);
                    SDL_FillRect(ctx.screen, NULL, SDL_MapRGB(ctx.screen->format, (bgc >> 16) & 0xFF, (bgc >> 8) & 0xFF, bgc & 0xFF));
                }

                int ow = 0;

                // indicator area top right
                if (ctx.appManagesIndicator)
                {
                    ow = GFX_blitHardwareGroup(ctx.screen, ctx.show_setting);
                }
                int max_width = ctx.screen->w - SCALE1(PADDING * 2) - ow;

                // title pill
                if (ctx.appManagesTitle)
                {
                    char display_name[256];
                    int text_width = GFX_truncateText(font.large, "Some title", display_name, max_width, SCALE1(BUTTON_PADDING * 2));
                    max_width = MIN(max_width, text_width);

                    SDL_Surface *text;
                    text = TTF_RenderUTF8_Blended(font.large, display_name, COLOR_WHITE);
                    SDL_Rect target = {SCALE1(PADDING), SCALE1(PADDING), max_width, SCALE1(PILL_SIZE)};
                    GFX_blitPillLight(ASSET_WHITE_PILL, ctx.screen, &target);
                    SDL_BlitSurfaceCPP(text, {0, 0, max_width - SCALE1(BUTTON_PADDING * 2), text->h}, ctx.screen, {SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4)});
                    SDL_FreeSurface(text);
                }
                else {
                    // just set the titleRect and we will pass it on to the list to populate as needed
                    titleRect = {SCALE1(PADDING), SCALE1(PADDING), max_width, SCALE1(PILL_SIZE)};
                }

                // bottom area, button hints
                if (ctx.appManagesHints)
                {
                    if (ctx.show_setting && !GetHDMI())
                        GFX_blitHardwareHints(ctx.screen, ctx.show_setting);
                    else
                    {
                        char *hints[] = {(char *)T("btn.menu"), (char *)T("btn.sleep"), NULL};
                        GFX_blitButtonGroup(hints, 0, ctx.screen, 0);
                    }
                    char *hints[] = {(char *)"B", (char *)T("btn.back"), (char *)"A", (char *)T("btn.okay"), NULL};
                    GFX_blitButtonGroup(hints, 1, ctx.screen, 1);
                }

                ctx.menu->draw(ctx.screen, listRect, titleRect);

                // present
                GFX_flip(ctx.screen);
                ctx.dirty = false;

                // hdmimon();
            }
            else
                GFX_sync();
        }

        delete ctx.menu;
        delete appearanceMenu;
        delete systemMenu;
        ctx.menu = NULL;

        // Color pickers are owned by unique_ptrs above; destroyed automatically here.

        QuitSettings();
        PWR_quit();
        PAD_quit();
        BT_quit();
        GFX_quit();

        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_error("%s", e.what());
        QuitSettings();
        PWR_quit();
        PAD_quit();
        BT_quit();
        GFX_quit();

        return EXIT_FAILURE;
    }
}
