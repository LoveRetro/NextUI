#include "btagent.hpp"
#include <iostream>
#include <cstring>

static constexpr const char* AGENT_PATH = "/com/nextui/agent";

static void agent_method_call(GDBusConnection*,
                                const gchar*,
                                const gchar*,
                                const gchar*,
                                const gchar* method_name,
                                GVariant*,
                                GDBusMethodInvocation* invocation,
                                gpointer);

static void properties_changed(GDBusConnection*,
                                const gchar*,
                                const gchar*,
                                const gchar* object_path,
                                const gchar* interface,
                                GVariant* params,
                                gpointer user_data);

using namespace Bluetooth;

PairingAgent::PairingAgent()
{
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
}

void PairingAgent::startPairingWindow()
{
    if (agent_registered)
        return;

    registerAgent();
    subscribeDeviceSignals();
    setAdapterPairable(true);

    std::cout << "Pairing window opened\n";
}

void PairingAgent::registerAgent()
{
    static const GDBusInterfaceVTable vtable = {
        agent_method_call, nullptr, nullptr
    };

    static const gchar* xml = R"XML(
<node>
 <interface name="org.bluez.Agent1">
  <method name="Release"/>
  <method name="RequestPinCode">
   <arg type="o" direction="in"/>
   <arg type="s" direction="out"/>
  </method>
  <method name="RequestPasskey">
   <arg type="o" direction="in"/>
   <arg type="u" direction="out"/>
  </method>
  <method name="RequestConfirmation">
   <arg type="o" direction="in"/>
   <arg type="u" direction="in"/>
  </method>
  <method name="RequestAuthorization">
   <arg type="o" direction="in"/>
  </method>
  <method name="Cancel"/>
 </interface>
</node>
)XML";

    introspection = g_dbus_node_info_new_for_xml(xml, nullptr);

    agent_reg_id = g_dbus_connection_register_object(
        bus,
        AGENT_PATH,
        introspection->interfaces[0],
        &vtable,
        nullptr, nullptr, nullptr);

    std::cout << "Agent registered at " << AGENT_PATH << "\n";

    g_dbus_connection_call_sync(
        bus, "org.bluez", "/org/bluez",
        "org.bluez.AgentManager1",
        "RegisterAgent",
        g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

    std::cout << "Agent registered with BlueZ\n";

    g_dbus_connection_call_sync(
        bus, "org.bluez", "/org/bluez",
        "org.bluez.AgentManager1",
        "RequestDefaultAgent",
        g_variant_new("(o)", AGENT_PATH),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    
    std::cout << "Agent set as default\n";

    agent_registered = true;
}

void PairingAgent::subscribeDeviceSignals()
{
    signal_id = g_dbus_connection_signal_subscribe(
        bus,
        "org.bluez",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        nullptr,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        properties_changed,
        this,
        nullptr);
    
    std::cout << "Subscribed to device property changes\n";
}

void PairingAgent::stopPairingWindow()
{
    if (!agent_registered)
        return;

    g_dbus_connection_call_sync(
        bus, "org.bluez", "/org/bluez",
        "org.bluez.AgentManager1",
        "UnregisterAgent",
        g_variant_new("(o)", AGENT_PATH),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

    std::cout << "Agent unregistered from BlueZ\n";

    setAdapterPairable(false);

    std::cout << "Adapter set to non-pairable\n";

    g_dbus_connection_signal_unsubscribe(bus, signal_id);
    g_dbus_connection_unregister_object(bus, agent_reg_id);

    agent_registered = false;

    std::cout << "Pairing window closed\n";
}

void PairingAgent::setAdapterPairable(bool on)
{
    g_dbus_connection_call_sync(
        bus,
        "org.bluez",
        "/org/bluez/hci0",
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", "org.bluez.Adapter1",
                      "Discoverable",
                      g_variant_new_boolean(on)),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

    std::cout << "Adapter Discoverable set to " << (on ? "true" : "false") << "\n";

    g_dbus_connection_call_sync(
        bus,
        "org.bluez",
        "/org/bluez/hci0",
        "org.freedesktop.DBus.Properties",
        "Set",
        g_variant_new("(ssv)", "org.bluez.Adapter1",
                        "Pairable",
                        g_variant_new_boolean(on)),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    
    std::cout << "Adapter Pairable set to " << (on ? "true" : "false") << "\n";
}

static void agent_method_call(GDBusConnection*,
                                const gchar*,
                                const gchar*,
                                const gchar*,
                                const gchar* method_name,
                                GVariant*,
                                GDBusMethodInvocation* invocation,
                                gpointer)
{
    if (strcmp(method_name, "RequestPinCode") == 0) {
        std::cout << "RequestPinCode called\n";
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(s)", "0000"));
        return;
    }

    if (strcmp(method_name, "RequestPasskey") == 0) {
        std::cout << "RequestPasskey called\n";
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(u)", 0));
        return;
    }

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static void properties_changed(GDBusConnection*,
                                const gchar*,
                                const gchar*,
                                const gchar* object_path,
                                const gchar* interface,
                                GVariant* params,
                                gpointer user_data)
{
    auto* self = static_cast<PairingAgent*>(user_data);

    const gchar* iface = nullptr;
    GVariantIter* iter = nullptr;
    GVariant* val = nullptr;
    const gchar* key = nullptr;

    /* params = (sa{sv}as) */
    g_variant_get(params, "(&sa{sv}as)", &iface, &iter, nullptr);

    /* We only care about Device1 changes */
    if (strcmp(iface, "org.bluez.Device1") != 0) {
        g_variant_iter_free(iter);
        return;
    }

    while (g_variant_iter_next(iter, "{sv}", &key, &val)) {
        if (strcmp(key, "Paired") == 0 &&
            g_variant_get_boolean(val)) {

            std::cout << "Device paired: " << object_path << "\n";
            self->stopPairingWindow();
        }
        g_variant_unref(val);
    }

    g_variant_iter_free(iter);
}