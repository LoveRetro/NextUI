#include "btmenu.hpp"
#include "keyboardprompt.hpp"

#include <unordered_set>
#include <map>

#include <mutex>
#include <shared_mutex>
typedef std::shared_mutex Lock;
typedef std::unique_lock<Lock> WriteLock;
typedef std::shared_lock<Lock> ReadLock;

using namespace Bluetooth;
using namespace std::placeholders;

Menu::Menu(const int &globalQuit, int &globalDirty) : MenuList(MenuItemType::Fixed, "Network", {}), globalQuit(globalQuit), globalDirty(globalDirty)
{
    toggleItem = new MenuItem(ListItemType::Generic, "Bluetooth", "Enable/disable Bluetooth", {false, true}, {"Off", "On"},
                              std::bind(&Menu::getBtToggleState, this),
                              std::bind(&Menu::setBtToggleState, this, std::placeholders::_1),
                              std::bind(&Menu::resetBtToggleState, this));
    diagItem = new MenuItem(ListItemType::Generic, "Bluetooth diagnostics", "Enable/disable Bluetooth logging", {false, true}, {"Off", "On"},
                              std::bind(&Menu::getBtDiagnosticsState, this),
                              std::bind(&Menu::setBtDiagnosticsState, this, std::placeholders::_1),
                              std::bind(&Menu::resetBtDiagnosticsState, this));
    rateItem = new MenuItem(ListItemType::Generic, "Maximum sampling rate", "44100 Hz: better compatibility\n48000 Hz: better quality", {44100, 48000}, {"44100 Hz", "48000 Hz"},
                              std::bind(&Menu::getSamplerateMaximum, this),
                              std::bind(&Menu::setSamplerateMaximum, this, std::placeholders::_1),
                              std::bind(&Menu::resetSamplerateMaximum, this));
    items.push_back(toggleItem);
    items.push_back(diagItem);
    items.push_back(rateItem);

    // best effort layout based on the platform defines, user should really call performLayout manually
    MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
    layout_called = false;

    worker = std::thread{&Menu::updater, this};
}

Menu::~Menu()
{
    quit = true;
    if (worker.joinable())
        worker.join();
}

InputReactionHint Menu::handleInput(int &dirty, int &quit)
{
    auto ret = MenuList::handleInput(dirty, quit);
    if (selectionDirty)
    {
        dirty = true;
        selectionDirty = false; // handled
        //LOG_info("collected selectionDirty\n");
    }
    return ret;
}

std::any Menu::getBtToggleState() const
{
    return BT_enabled();
}

void Menu::setBtToggleState(const std::any &on)
{
    BT_enable(std::any_cast<bool>(on));
}

void Menu::resetBtToggleState()
{
    //
}

std::any Menu::getBtDiagnosticsState() const
{
    return BT_diagnosticsEnabled();
}

void Menu::setBtDiagnosticsState(const std::any &on)
{
    BT_diagnosticsEnable(std::any_cast<bool>(on));
}

void Menu::resetBtDiagnosticsState()
{
    //
}

std::any Menu::getSamplerateMaximum() const
{
    return CFG_getBluetoothSamplingrateLimit();
}

void Menu::setSamplerateMaximum(const std::any &value)
{
    CFG_setBluetoothSamplingrateLimit(std::any_cast<int>(value));
}

void Menu::resetSamplerateMaximum()
{
    CFG_setBluetoothSamplingrateLimit(CFG_DEFAULT_BLUETOOTH_MAXRATE);
}

template <typename Map>
bool key_compare(Map const &lhs, Map const &rhs)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                                                  [](auto a, auto b)
                                                  { return a.first == b.first; });
}

void Menu::updater()
{
    int pollSecs = 15;

    while (!quit && !globalQuit)
    {
        // TODO: pause when menu is not rendered
        // TODO: improve repaint logic in a way that remembers selection
        // Scan
        if (BT_enabled())
        {
            if(!BT_discovering())
                BT_discovery(true);

            std::map<std::string, BT_devicePaired> pairedMap;
            std::vector<BT_devicePaired> kl(SCAN_MAX_RESULTS);
            int known = BT_pairedDevices(kl.data(), SCAN_MAX_RESULTS);
            for (int i = 0; i < known; i++)
                pairedMap.emplace(kl[i].remote_name, kl[i]);

            // grab list and compare it to previous result
            // only relayout the menu if changes happended
            std::map<std::string, BT_device> scanMap;
            std::vector<BT_device> sr(SCAN_MAX_RESULTS);
            int cnt = BT_availableDevices(sr.data(), SCAN_MAX_RESULTS);
            for (int i = 0; i < cnt; i++)
                scanMap.emplace(sr[i].name, sr[i]);

            // dont repopulate if any submenu is open
            bool menuOpen = false;
            for (auto i : items)
            {
                if (i->isDeferred())
                {
                    menuOpen = true;
                    break;
                }
            }

            // something changed?
            if (!menuOpen)
            {
                // remember selection and restore
                std::string selectedName;
                bool selectionApplied = false;

                {
                    WriteLock w(itemLock);
                    selectedName = getSelectedItemName();
                    items.clear();
                    items.push_back(toggleItem);
                    items.push_back(diagItem);
                    items.push_back(rateItem);
                    layout_called = false;

                    for (auto &[s, r] : scanMap)
                    {
                        MenuList *options;
                        options = new MenuList(MenuItemType::List, "Options", {new PairNewItem(r, selectionDirty)});
                        auto itm = new PairableItem{r, options};
                        items.push_back(itm);
                    }

                    for (auto &[s, r] : pairedMap)
                    {
                        MenuList *options;
                        if (r.is_connected)
                        {
                            options = new MenuList(MenuItemType::List, "Options", {
                                                                                    new DisconnectKnownItem(r, selectionDirty),
                                                                                    new UnpairItem(r, selectionDirty),
                                                                                });
                        }
                        else
                        {
                            options = new MenuList(MenuItemType::List, "Options", {
                                                                                    new ConnectKnownItem(r, selectionDirty),
                                                                                    new UnpairItem(r, selectionDirty),
                                                                                });
                        }
                        auto itm = new PairedItem{r, options};
                        itm->setDesc(std::string(r.remote_addr) + " | " + std::to_string(r.rssi));
                        items.push_back(itm);
                    }
                }
                MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});

                // Attempt to restore prev selection
                selectionApplied = selectByName(selectedName);
                globalDirty |= selectionApplied;
                // If selection was restored, we already called performLayout internally
                selectionDirty |= !selectionApplied;
            }
            pollSecs = 2;
        }
        else
        {
            WriteLock w(itemLock);
            items.clear();
            items.push_back(toggleItem);
            items.push_back(diagItem);
            items.push_back(rateItem);
            layout_called = false;
            selectionDirty = true;
            pollSecs = 15;
        }

        // reset selection scope (locks internally)
        if (selectionDirty)
        {
            MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
            selectionDirty = false;
        }

        std::this_thread::sleep_for(std::chrono::seconds(pollSecs));
    }
}

PairNewItem::PairNewItem(BT_device d, bool& dirty)
    : MenuItem(ListItemType::Button, "Pair", "Pair this device.",
        [&](AbstractMenuItem &item) -> InputReactionHint {
            BT_pair(dev.addr);
            dirty = true;
            return Exit;
        }), dev(d)
{}

UnpairItem::UnpairItem(BT_devicePaired d, bool& dirty)
    : MenuItem(ListItemType::Button, "Forget", "Forget this device.",
        [&](AbstractMenuItem &item) -> InputReactionHint {
            BT_unpair(dev.remote_addr);
            dirty = true;
            return Exit;
        }), dev(d)
{}

ConnectKnownItem::ConnectKnownItem(BT_devicePaired d, bool& dirty)
    : MenuItem(ListItemType::Button, "Connect", "Connect this device.",
        [&](AbstractMenuItem &item) -> InputReactionHint {
            BT_connect(dev.remote_addr);
            dirty = true;
            return Exit;
        }), dev(d)
{}

DisconnectKnownItem::DisconnectKnownItem(BT_devicePaired d, bool& dirty)
    : MenuItem(ListItemType::Button, "Disconnect", "Disconnect this device.",
        [&](AbstractMenuItem &item) -> InputReactionHint {
            BT_disconnect(dev.remote_addr);
            dirty = true;
            return Exit;
        }), dev(d)
{}

PairableItem::PairableItem(BT_device d, MenuList* submenu)
    : MenuItem(ListItemType::Custom, d.name, d.addr, DeferToSubmenu, submenu), dev(d)
{}

void PairableItem::drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const
{
    SDL_Color text_color = uintToColour(THEME_COLOR4_255);
    SDL_Surface *text = TTF_RenderUTF8_Blended(font.tiny, item.getLabel().c_str(), COLOR_WHITE); // always white

    // hack - this should be correlated to max_width
    int mw = dst.w;

    if (selected)
    {
        // gray pill
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, {dst.x, dst.y, mw, SCALE1(BUTTON_SIZE)});
    }

    // device icon
    if(dev.kind != BLUETOOTH_NONE) {
        auto asset = (dev.kind == BLUETOOTH_AUDIO) ? ASSET_AUDIO : ASSET_CONTROLLER;
        SDL_Rect rect = (dev.kind == BLUETOOTH_AUDIO) ? SDL_Rect{0, 0, 12, 12} : SDL_Rect{0, 0, 12, 12};
        int ix = dst.x + dst.w - SCALE1(OPTION_PADDING + rect.w);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(asset, NULL, surface, &tgt, THEME_COLOR6);
    }

    if (selected)
    {
        // white pill
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);
        GFX_blitPillDarkCPP(ASSET_BUTTON, surface, {dst.x, dst.y, w, SCALE1(BUTTON_SIZE)});
        text_color = uintToColour(THEME_COLOR5_255);
    }

    text = TTF_RenderUTF8_Blended(font.small, item.getName().c_str(), text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(OPTION_PADDING), dst.y + SCALE1(1)});
    SDL_FreeSurface(text);
}

PairedItem::PairedItem(BT_devicePaired d, MenuList* submenu)
    : MenuItem(ListItemType::Custom, d.remote_name, d.remote_addr, DeferToSubmenu, submenu), dev(d)
{}

void PairedItem::drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const
{
    SDL_Color text_color = uintToColour(THEME_COLOR4_255);
    SDL_Surface *text = TTF_RenderUTF8_Blended(font.tiny, item.getLabel().c_str(), COLOR_WHITE); // always white

    // hack - this should be correlated to max_width
    int mw = dst.w;

    if (selected)
    {
        // gray pill
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, {dst.x, dst.y, mw, SCALE1(BUTTON_SIZE)});
    }

    // rssi icon
    auto asset =
        dev.rssi == 0   ? ASSET_WIFI_OFF :
        dev.rssi >= -55 ? ASSET_WIFI :
        dev.rssi >= -67 ? ASSET_WIFI_MED
                        : ASSET_WIFI_LOW;
    SDL_Rect rect = {0, 0, 12, 12};
    int ix = dst.x + dst.w - SCALE1(OPTION_PADDING + rect.w);
    int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
    SDL_Rect tgt{ix, y};
    GFX_blitAssetColor(asset, NULL, surface, &tgt, THEME_COLOR6);

    // connected
    if(dev.is_connected) {
        SDL_Rect rect = {0, 0, 12, 12};
        ix = ix - SCALE1(OPTION_PADDING + rect.w);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(ASSET_CHECKCIRCLE, NULL, surface, &tgt, THEME_COLOR6);
    }
    // bonded
    else if(dev.is_bonded) {
        SDL_Rect rect = {0, 0, 8, 11};
        ix = ix - SCALE1(OPTION_PADDING + rect.w + 2);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(ASSET_LOCK, NULL, surface, &tgt, THEME_COLOR6);
    }

    if (selected)
    {
        // white pill
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);
        GFX_blitPillDarkCPP(ASSET_BUTTON, surface, {dst.x, dst.y, w, SCALE1(BUTTON_SIZE)});
        text_color = uintToColour(THEME_COLOR5_255);
    }

    text = TTF_RenderUTF8_Blended(font.small, item.getName().c_str(), text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(OPTION_PADDING), dst.y + SCALE1(1)});
    SDL_FreeSurface(text);
}
