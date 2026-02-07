#include <dlfcn.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "manual.h"

typedef void (*manual_set_host_fn)(const ManualHost *host);
typedef void (*manual_open_fn)(const char *rom_path);

static ManualHost g_manual_host;
static int g_host_initialized = 0;
static int g_plugin_load_attempted = 0;
static void *g_plugin_handle = NULL;
static manual_set_host_fn g_plugin_set_host = NULL;
static manual_open_fn g_plugin_open = NULL;

static void Manual_tryLoadPlugin(void) {
  if (g_plugin_load_attempted)
    return;
  g_plugin_load_attempted = 1;

  const char *paths[] = {
      "/mnt/SDCARD/.system/tg5040/lib/manual.so",
      SYSTEM_PATH "/lib/manual.so",
      "/mnt/SDCARD/.system/tg5040/lib/libmanual.so",
      SYSTEM_PATH "/lib/libmanual.so",
      "./manual.so",
      "./libmanual.so",
      "manual.so",
      "libmanual.so",
      NULL};
  for (int i = 0; paths[i]; i++) {
    g_plugin_handle = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
    if (g_plugin_handle)
      break;
  }

  if (!g_plugin_handle) {
    const char *err = dlerror();
    LOG_error("manual: failed to load manual.so (%s)\n",
              err ? err : "unknown error");
    return;
  }

  g_plugin_set_host = (manual_set_host_fn)dlsym(g_plugin_handle, "Manual_setHost");
  g_plugin_open = (manual_open_fn)dlsym(g_plugin_handle, "Manual_open");

  if (!g_plugin_set_host || !g_plugin_open) {
    LOG_error("manual: missing required symbols in manual.so\n");
    dlclose(g_plugin_handle);
    g_plugin_handle = NULL;
    g_plugin_set_host = NULL;
    g_plugin_open = NULL;
    return;
  }

  if (g_host_initialized)
    g_plugin_set_host(&g_manual_host);
}

void Manual_setHost(const ManualHost *host) {
  if (host) {
    g_manual_host = *host;
    g_host_initialized = 1;
  } else {
    memset(&g_manual_host, 0, sizeof(g_manual_host));
    g_host_initialized = 0;
  }

  Manual_tryLoadPlugin();
  if (g_plugin_set_host && g_host_initialized)
    g_plugin_set_host(&g_manual_host);
}

void Manual_open(const char *rom_path) {
  if (!rom_path || !rom_path[0])
    return;

  Manual_tryLoadPlugin();
  if (!g_plugin_open) {
    LOG_error("manual: plugin unavailable, cannot open manual for %s\n", rom_path);
    return;
  }

  if (g_plugin_set_host && g_host_initialized)
    g_plugin_set_host(&g_manual_host);
  g_plugin_open(rom_path);
}
