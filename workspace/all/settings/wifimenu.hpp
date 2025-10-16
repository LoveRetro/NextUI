#pragma once

#include "menu.hpp"
#include <thread>

namespace Wifi
{
class Menu : public MenuList
{
	const int &globalQuit;
	int &globalDirty;
	// wifi on/off
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
	std::any getWifToggleState() const;
	void setWifiToggleState(const std::any &on);
	void resetWifiToggleState();

	std::any getWifDiagnosticsState() const;
	void setWifiDiagnosticsState(const std::any &on);
	void resetWifiDiagnosticsState();

	void updater();
};

class NetworkItem : public MenuItem
{
	WIFI_network net;
	bool connected;

  public:
	NetworkItem(WIFI_network n, bool connected, MenuList *submenu);

	void drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item,
						bool selected) const override;
};

class ConnectKnownItem : public MenuItem
{
	WIFI_network net;

  public:
	ConnectKnownItem(WIFI_network n, bool &dirty);
};

class ConnectNewItem : public MenuItem
{
	WIFI_network net;

  public:
	ConnectNewItem(WIFI_network n, bool &dirty);
};

class ForgetItem : public MenuItem
{
	WIFI_network net;

  public:
	ForgetItem(WIFI_network n, bool &dirty);
};
} // namespace Wifi