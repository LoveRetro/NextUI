#pragma once

#include "menu.hpp"
#include <thread>

namespace Bluetooth
{
    class Menu : public MenuList
    {
        const int &globalQuit;
        int &globalDirty;
        // bt on/off
        MenuItem *toggleItem;
        // diagnostics on/off
        MenuItem *diagItem;

        std::thread worker;
        bool quit = false;
        bool selectionDirty = false;

    public:
        Menu(const int &globalQuit, int &globalDirty);
        ~Menu();

        InputReactionHint handleInput(int &dirty, int &quit) override;

    private:
        std::any getBtToggleState() const;
        void setBtToggleState(const std::any &on);
        void resetBtToggleState();

        std::any getBtDiagnosticsState() const;
        void setBtDiagnosticsState(const std::any &on);
        void resetBtDiagnosticsState();

        void updater();
    };

    class PairableItem : public MenuItem
    {
        BT_device dev;

    public:
        PairableItem(BT_device d, MenuList *submenu);

        void drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const override;
    };

    class PairedItem : public MenuItem
    {
        BT_devicePaired dev;

    public:
        PairedItem(BT_devicePaired d, MenuList *submenu);

        void drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const override;
    };

    class ConnectKnownItem : public MenuItem
    {
        BT_devicePaired dev;
    public:
        ConnectKnownItem(BT_devicePaired n, bool& dirty);
    };

    class DisconnectKnownItem : public MenuItem
    {
        BT_devicePaired dev;
    public:
        DisconnectKnownItem(BT_devicePaired n, bool& dirty);
    };

    class PairNewItem : public MenuItem
    {
        BT_device dev;

    public:
        PairNewItem(BT_device n, bool& dirty);
    };

    class UnpairItem : public MenuItem
    {
        BT_devicePaired dev;

    public:
        UnpairItem(BT_devicePaired n, bool& dirty);
    };
}