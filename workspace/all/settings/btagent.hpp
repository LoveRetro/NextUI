#pragma once 
#include <gio/gio.h>
#include <glib.h>

namespace Bluetooth
{
class PairingAgent {
public:
    PairingAgent();
    void startPairingWindow();
    void stopPairingWindow();

private:
    void registerAgent();
    void subscribeDeviceSignals();
    void setAdapterPairable(bool on);

private:
    GDBusConnection* bus = nullptr;
    GDBusNodeInfo* introspection = nullptr;
    guint agent_reg_id = 0;
    guint signal_id = 0;
    bool agent_registered = false;
};
} // namespace Bluetooth