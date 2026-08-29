#include "host_internal.h"
#include "settings.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static bool ExtPinned(const char *name)
{
    return name && name[0] && strcmp(name, "extensions") == 0;
}

bool PicoHost_ExtensionDisabled(const PicoHost *host, const char *name)
{
    if (!host || !name || !name[0] || ExtPinned(name))
    {
        return false;
    }
    for (int i = 0; i < host->preferences.disabled_host_extension_count; i++)
    {
        if (strcmp(host->preferences.disabled_host_extensions[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool HostSlotMatchesModule(const PicoPluginSlot *slot,
                                  const PicoModuleGeneration *module)
{
    if (!slot || !module)
    {
        return false;
    }
    if (module->source[0])
    {
        return slot->source && strcmp(slot->source, module->source) == 0;
    }
    return !slot->source && slot->name[0] && module->ext.name &&
           strcmp(slot->name, module->ext.name) == 0;
}

static PicoPluginSlot *FindOrAllocHostSlot(PicoHost *host,
                                           PicoModuleGeneration *module)
{
    if (!host || !module || !module->ext.name || !module->ext.name[0])
    {
        return NULL;
    }
    for (int i = 0; i < host->host_plugin_count; i++)
    {
        if (HostSlotMatchesModule(&host->host_plugins[i], module))
        {
            return &host->host_plugins[i];
        }
    }
    if (host->host_plugin_count < PICO_MAX_EXTENSION_SLOTS)
    {
        PicoPluginSlot *slot = &host->host_plugins[host->host_plugin_count++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->name, sizeof(slot->name), "%s", module->ext.name);
        slot->source = module->source[0] ? module->source : NULL;
        return slot;
    }
    return NULL;
}

bool PicoHostExtensions_Activate(PicoHost *host, PicoModuleGeneration *module)
{
    if (!host || !module)
    {
        return false;
    }
    if (!module->ext.host_init)
    {
        return true;
    }

    PicoPluginSlot *slot = FindOrAllocHostSlot(host, module);
    if (slot && slot->initialized && slot->module == module)
    {
        return true;
    }
    if (slot)
    {
        slot->desired_generation = module->generation;
    }

    bool disabled = PicoHost_ExtensionDisabled(host, module->ext.name);
    if (disabled)
    {
        if (slot)
        {
            slot->initialized = false;
            slot->active_generation = 0;
            slot->last_error[0] = '\0';
        }
        return true;
    }

    void *state = NULL;
    PicoHost_BeginRegistration(host, PICO_REG_HOST, NULL);
    int rc = module->ext.host_init(host, &state);
    if (rc != 0)
    {
        PicoHost_DiscardRegistration(host);
        if (state && module->ext.host_shutdown)
        {
            module->ext.host_shutdown(host, state);
        }
        if (slot)
        {
            snprintf(slot->last_error, sizeof(slot->last_error),
                     "host extension init failed (%d)", rc);
            slot->active_generation = 0;
            pico_status_warn(host, slot->last_error);
        }
        return false;
    }
    if (slot)
    {
        slot->state = state;
        slot->module = module;
        slot->initialized = true;
        slot->active_generation = module->generation;
        slot->last_error[0] = '\0';
    }
    PicoModule_Retain(module);
    PicoHost_PublishRegistration(host, state);
    return true;
}

void PicoHostExtensions_ShutdownModule(PicoHost *host, PicoModuleGeneration *module)
{
    if (!host || !module)
    {
        return;
    }
    for (int i = 0; i < host->host_plugin_count; i++)
    {
        PicoPluginSlot *slot = &host->host_plugins[i];
        if (slot->initialized && slot->module == module)
        {
            if (module->ext.host_shutdown)
            {
                module->ext.host_shutdown(host, slot->state);
            }
            PicoModule_Release(module);
            slot->state = NULL;
            slot->initialized = false;
            slot->active_generation = 0;
            slot->module = NULL;
        }
    }
}

void PicoHostExtensions_Shutdown(PicoHost *host)
{
    if (!host)
    {
        return;
    }
    for (int i = host->host_plugin_count - 1; i >= 0; i--)
    {
        PicoPluginSlot *slot = &host->host_plugins[i];
        if (slot->initialized && slot->module)
        {
            if (slot->module->ext.host_shutdown)
            {
                slot->module->ext.host_shutdown(host, slot->state);
            }
            PicoModule_Release(slot->module);
            slot->state = NULL;
            slot->initialized = false;
            slot->active_generation = 0;
            slot->module = NULL;
        }
    }
    host->host_plugin_count = 0;
    memset(host->views, 0, sizeof(host->views));
    memset(host->view_count, 0, sizeof(host->view_count));
    memset(host->hooks, 0, sizeof(host->hooks));
    host->hook_count = 0;
    memset(host->commands, 0, sizeof(host->commands));
    host->command_count = 0;
    memset(host->completers, 0, sizeof(host->completers));
    host->completer_count = 0;
    memset(host->auths, 0, sizeof(host->auths));
    host->auth_count = 0;
    memset(&host->staging, 0, sizeof(host->staging));
}

void PicoHostExtensions_OnFrame(PicoHost *host, float dt)
{
    if (!host)
    {
        return;
    }
    for (int i = 0; i < host->host_plugin_count; i++)
    {
        PicoPluginSlot *slot = &host->host_plugins[i];
        if (slot->initialized && slot->module && slot->module->ext.host_on_frame)
        {
            slot->module->ext.host_on_frame(host, slot->state, dt);
        }
    }
}

void *PicoHostExtensions_State(const PicoHost *host, const char *name)
{
    if (!host || !name || !name[0])
    {
        return NULL;
    }
    for (int i = 0; i < host->host_plugin_count; i++)
    {
        if (strcmp(host->host_plugins[i].name, name) == 0 && host->host_plugins[i].initialized)
        {
            return host->host_plugins[i].state;
        }
    }
    return NULL;
}

typedef struct PicoHostExtensionSet {
    PicoSlotView views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int view_count[PICO_SLOT_COUNT];
    PicoHookEntry hooks[PICO_MAX_HOOKS];
    int hook_count;
    PicoCommand commands[PICO_MAX_COMMANDS];
    int command_count;
    PicoCompleter completers[PICO_MAX_COMPLETERS];
    int completer_count;
    PicoAuth auths[PICO_MAX_AUTH];
    int auth_count;
    PicoPluginSlot plugins[PICO_MAX_EXTENSION_SLOTS];
    int plugin_count;
} PicoHostExtensionSet;

static void CaptureHostExtensionSet(const PicoHost *host, PicoHostExtensionSet *set)
{
    memset(set, 0, sizeof(*set));
    memcpy(set->views, host->views, sizeof(set->views));
    memcpy(set->view_count, host->view_count, sizeof(set->view_count));
    memcpy(set->hooks, host->hooks, sizeof(set->hooks));
    set->hook_count = host->hook_count;
    memcpy(set->commands, host->commands, sizeof(set->commands));
    set->command_count = host->command_count;
    memcpy(set->completers, host->completers, sizeof(set->completers));
    set->completer_count = host->completer_count;
    memcpy(set->auths, host->auths, sizeof(set->auths));
    set->auth_count = host->auth_count;
    memcpy(set->plugins, host->host_plugins, sizeof(set->plugins));
    set->plugin_count = host->host_plugin_count;
}

static void InstallHostExtensionSet(PicoHost *host, const PicoHostExtensionSet *set)
{
    memcpy(host->views, set->views, sizeof(host->views));
    memcpy(host->view_count, set->view_count, sizeof(host->view_count));
    memcpy(host->hooks, set->hooks, sizeof(host->hooks));
    host->hook_count = set->hook_count;
    memcpy(host->commands, set->commands, sizeof(host->commands));
    host->command_count = set->command_count;
    memcpy(host->completers, set->completers, sizeof(host->completers));
    host->completer_count = set->completer_count;
    memcpy(host->auths, set->auths, sizeof(host->auths));
    host->auth_count = set->auth_count;
    memcpy(host->host_plugins, set->plugins, sizeof(host->host_plugins));
    host->host_plugin_count = set->plugin_count;
}

static void ClearHostExtensionSet(PicoHost *host)
{
    memset(host->views, 0, sizeof(host->views));
    memset(host->view_count, 0, sizeof(host->view_count));
    memset(host->hooks, 0, sizeof(host->hooks));
    host->hook_count = 0;
    memset(host->commands, 0, sizeof(host->commands));
    host->command_count = 0;
    memset(host->completers, 0, sizeof(host->completers));
    host->completer_count = 0;
    memset(host->auths, 0, sizeof(host->auths));
    host->auth_count = 0;
    memset(host->host_plugins, 0, sizeof(host->host_plugins));
    host->host_plugin_count = 0;
}

static void ShutdownHostSlots(PicoHost *host, PicoPluginSlot *slots, int count)
{
    for (int i = count - 1; i >= 0; i--)
    {
        PicoPluginSlot *slot = &slots[i];
        if (slot->initialized && slot->module)
        {
            if (slot->module->ext.host_shutdown)
            {
                slot->module->ext.host_shutdown(host, slot->state);
            }
            PicoModule_Release(slot->module);
            slot->state = NULL;
            slot->initialized = false;
            slot->module = NULL;
        }
    }
}

bool PicoHostExtensions_Reload(PicoHost *host)
{
    if (!host)
    {
        return false;
    }

    PicoHostExtensionSet old;
    CaptureHostExtensionSet(host, &old);
    ClearHostExtensionSet(host);

    bool ok = true;
    for (int m = 0; m < host->module_count; m++)
    {
        PicoModuleGeneration *mod = &host->modules[m];
        if (!mod->desired || !mod->ext.host_init)
        {
            continue;
        }
        if (!PicoHostExtensions_Activate(host, mod))
        {
            ok = false;
            break;
        }
    }

    if (ok)
    {
        ShutdownHostSlots(host, old.plugins, old.plugin_count);
        return true;
    }

    /* Candidate instances are isolated in the cleared live slots. Roll back only
     * those instances, then restore the untouched active set. */
    ShutdownHostSlots(host, host->host_plugins, host->host_plugin_count);
    ClearHostExtensionSet(host);
    InstallHostExtensionSet(host, &old);
    PicoHost_DiscardRegistration(host);
    return false;
}
