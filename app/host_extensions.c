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

static PicoPluginSlot *FindOrAllocHostSlot(PicoHost *host, const char *name)
{
    if (!host || !name || !name[0])
    {
        return NULL;
    }
    for (int i = 0; i < host->host_plugin_count; i++)
    {
        if (strcmp(host->host_plugins[i].name, name) == 0)
        {
            return &host->host_plugins[i];
        }
    }
    if (host->host_plugin_count < PICO_MAX_EXTENSION_SLOTS)
    {
        PicoPluginSlot *slot = &host->host_plugins[host->host_plugin_count++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->name, sizeof(slot->name), "%s", name);
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

    PicoPluginSlot *slot = FindOrAllocHostSlot(host, module->ext.name);
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
        if (slot->initialized && slot->module && slot->module->desired &&
            slot->module->ext.host_on_frame)
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

static void SaveHostRegistrations(const PicoHost *host, PicoHostStaging *saved)
{
    memset(saved, 0, sizeof(*saved));
    memcpy(saved->host_views, host->views, sizeof(saved->host_views));
    memcpy(saved->host_view_count, host->view_count, sizeof(saved->host_view_count));
    memcpy(saved->host_hooks, host->hooks, sizeof(saved->host_hooks));
    saved->host_hook_count = host->hook_count;
    memcpy(saved->host_commands, host->commands, sizeof(saved->host_commands));
    saved->host_command_count = host->command_count;
    memcpy(saved->host_completers, host->completers, sizeof(saved->host_completers));
    saved->host_completer_count = host->completer_count;
    memcpy(saved->host_auths, host->auths, sizeof(saved->host_auths));
    saved->host_auth_count = host->auth_count;
}

static void RestoreHostRegistrations(PicoHost *host, const PicoHostStaging *saved)
{
    memcpy(host->views, saved->host_views, sizeof(host->views));
    memcpy(host->view_count, saved->host_view_count, sizeof(host->view_count));
    memcpy(host->hooks, saved->host_hooks, sizeof(host->hooks));
    host->hook_count = saved->host_hook_count;
    memcpy(host->commands, saved->host_commands, sizeof(host->commands));
    host->command_count = saved->host_command_count;
    memcpy(host->completers, saved->host_completers, sizeof(host->completers));
    host->completer_count = saved->host_completer_count;
    memcpy(host->auths, saved->host_auths, sizeof(host->auths));
    host->auth_count = saved->host_auth_count;
}

bool PicoHostExtensions_Reload(PicoHost *host)
{
    if (!host)
    {
        return false;
    }
    PicoHostStaging saved;
    SaveHostRegistrations(host, &saved);
    PicoPluginSlot old_slots[PICO_MAX_EXTENSION_SLOTS];
    int old_slot_count = host->host_plugin_count;
    memcpy(old_slots, host->host_plugins, sizeof(old_slots));

    pico_clear_registrations(host);

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
        for (int i = old_slot_count - 1; i >= 0; i--)
        {
            PicoPluginSlot *slot = &old_slots[i];
            if (slot->initialized && slot->module)
            {
                if (slot->module->ext.host_shutdown)
                {
                    slot->module->ext.host_shutdown(host, slot->state);
                }
                PicoModule_Release(slot->module);
            }
        }
        return true;
    }

    /* Rollback */
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
        }
    }
    RestoreHostRegistrations(host, &saved);
    memcpy(host->host_plugins, old_slots, sizeof(host->host_plugins));
    host->host_plugin_count = old_slot_count;
    return false;
}
