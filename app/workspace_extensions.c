#include "workspace_internal.h"
#include "host_internal.h"
#include "settings.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

void PicoModule_Retain(PicoModuleGeneration *module)
{
    if (!module)
    {
        return;
    }
    assert(module->ref_count >= 0);
    module->ref_count++;
}

void PicoModule_Release(PicoModuleGeneration *module)
{
    if (!module)
    {
        return;
    }
    assert(module->ref_count > 0);
    module->ref_count--;
    if (module->ref_count == 0 && !module->desired)
    {
        if (module->handle)
        {
            dlclose(module->handle);
            module->handle = NULL;
        }
        memset(module, 0, sizeof(*module));
    }
}

static PicoRegistrationGeneration *RegistrationAlloc(const PicoWorkspace *workspace)
{
    PicoRegistrationGeneration *generation;
    if (!workspace)
    {
        return NULL;
    }
    generation = (PicoRegistrationGeneration *)calloc(1, sizeof(*generation));
    if (!generation)
    {
        return NULL;
    }
    generation->id = workspace->registration_generation;
    generation->ref_count = 1; /* the workspace's active-generation reference */
    memcpy(generation->views, workspace->views, sizeof(generation->views));
    memcpy(generation->view_count, workspace->view_count, sizeof(generation->view_count));
    memcpy(generation->empty_views, workspace->empty_views, sizeof(generation->empty_views));
    generation->empty_view_count = workspace->empty_view_count;
    memcpy(generation->hooks, workspace->hooks, sizeof(generation->hooks));
    generation->hook_count = workspace->hook_count;
    memcpy(generation->tool_before_hooks, workspace->tool_before_hooks,
           sizeof(generation->tool_before_hooks));
    generation->tool_before_hook_count = workspace->tool_before_hook_count;
    memcpy(generation->tool_after_hooks, workspace->tool_after_hooks,
           sizeof(generation->tool_after_hooks));
    generation->tool_after_hook_count = workspace->tool_after_hook_count;
    memcpy(generation->llm_hooks, workspace->llm_hooks, sizeof(generation->llm_hooks));
    generation->llm_hook_count = workspace->llm_hook_count;
    memcpy(generation->context_hooks, workspace->context_hooks, sizeof(generation->context_hooks));
    generation->context_hook_count = workspace->context_hook_count;
    memcpy(generation->tool_row_hooks, workspace->tool_row_hooks,
           sizeof(generation->tool_row_hooks));
    generation->tool_row_hook_count = workspace->tool_row_hook_count;
    memcpy(generation->tools, workspace->tools, sizeof(generation->tools));
    generation->tool_count = workspace->tool_count;
    memcpy(generation->commands, workspace->commands, sizeof(generation->commands));
    generation->command_count = workspace->command_count;
    memcpy(generation->completers, workspace->completers, sizeof(generation->completers));
    generation->completer_count = workspace->completer_count;
    memcpy(generation->providers, workspace->providers, sizeof(generation->providers));
    generation->provider_count = workspace->provider_count;
    generation->plugin_count = 0;
    for (int i = 0; i < workspace->workspace_plugin_count; i++)
    {
        if (workspace->workspace_plugins[i].initialized && workspace->workspace_plugins[i].module)
        {
            generation->plugins[generation->plugin_count++] = workspace->workspace_plugins[i];
            PicoModule_Retain(workspace->workspace_plugins[i].module);
        }
    }
    return generation;
}

PicoRegistrationGeneration *PicoWorkspace_RegistrationActive(PicoWorkspace *workspace)
{
    return workspace ? workspace->active_registration : NULL;
}

const PicoRegistrationGeneration *PicoWorkspace_RegistrationActiveConst(const PicoWorkspace *workspace)
{
    return workspace ? workspace->active_registration : NULL;
}

void PicoWorkspace_RegistrationRetain(PicoRegistrationGeneration *generation)
{
    if (!generation)
    {
        return;
    }
    assert(generation->ref_count > 0);
    generation->ref_count++;
}

void PicoWorkspace_RegistrationRelease(PicoRegistrationGeneration *generation)
{
    if (!generation)
    {
        return;
    }
    assert(generation->ref_count > 0);
    generation->ref_count--;
    if (generation->ref_count != 0)
    {
        return;
    }
    for (int i = 0; i < generation->plugin_count; i++)
    {
        PicoModule_Release(generation->plugins[i].module);
    }
    free(generation);
}

bool PicoWorkspace_PublishRegistrationGeneration(PicoWorkspace *workspace)
{
    PicoRegistrationGeneration *next;
    PicoRegistrationGeneration *old;
    if (!workspace)
    {
        return false;
    }
    if (++workspace->registration_generation == 0)
    {
        workspace->registration_generation = 1;
    }
    next = RegistrationAlloc(workspace);
    if (!next)
    {
        workspace->registration_generation--;
        return false;
    }
    old = workspace->active_registration;
    workspace->active_registration = next;
    PicoWorkspace_RegistrationRelease(old);
    return true;
}

void PicoWorkspace_RegistrationClear(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return;
    }
    PicoWorkspace_RegistrationRelease(workspace->active_registration);
    workspace->active_registration = NULL;
}

static bool ExtPinned(const char *name)
{
    return name && name[0] && strcmp(name, "extensions") == 0;
}

bool PicoWorkspace_ExtensionDisabled(const PicoWorkspace *workspace, const char *name)
{
    if (!workspace || !name || !name[0] || ExtPinned(name))
    {
        return false;
    }
    for (int i = 0; i < workspace->settings.disabled_extension_count; i++)
    {
        if (strcmp(workspace->settings.disabled_extensions[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

bool PicoWorkspaceExtensions_Activate(PicoWorkspace *workspace, PicoModuleGeneration *module)
{
    if (!workspace || !module)
    {
        return false;
    }
    if (!module->ext.workspace_init)
    {
        return true;
    }

    PicoPluginSlot *slot = NULL;
    for (int i = 0; i < workspace->workspace_plugin_count; i++)
    {
        if (strcmp(workspace->workspace_plugins[i].name, module->ext.name) == 0)
        {
            slot = &workspace->workspace_plugins[i];
            break;
        }
    }
    if (!slot && workspace->workspace_plugin_count < PICO_MAX_EXTENSION_SLOTS)
    {
        slot = &workspace->workspace_plugins[workspace->workspace_plugin_count++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->name, sizeof(slot->name), "%s", module->ext.name);
    }
    if (slot && slot->initialized && slot->module == module)
    {
        return true;
    }
    if (slot)
    {
        slot->desired_generation = module->generation;
    }

    bool disabled = PicoWorkspace_ExtensionDisabled(workspace, module->ext.name);
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
    PicoHost *host = workspace->host;
    PicoHost_BeginRegistration(host, PICO_REG_WORKSPACE, workspace);
    int rc = module->ext.workspace_init(workspace, &state);
    if (rc != 0)
    {
        PicoHost_DiscardRegistration(host);
        if (state && module->ext.workspace_shutdown)
        {
            module->ext.workspace_shutdown(workspace, state);
        }
        if (slot)
        {
            snprintf(slot->last_error, sizeof(slot->last_error),
                     "workspace extension init failed (%d)", rc);
            slot->active_generation = 0;
            pico_workspace_status_warn(workspace, slot->last_error);
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

void PicoWorkspaceExtensions_ShutdownModule(PicoWorkspace *workspace, PicoModuleGeneration *module)
{
    if (!workspace || !module)
    {
        return;
    }
    for (int i = 0; i < workspace->workspace_plugin_count; i++)
    {
        PicoPluginSlot *slot = &workspace->workspace_plugins[i];
        if (slot->module == module && slot->initialized)
        {
            if (module->ext.workspace_shutdown)
            {
                module->ext.workspace_shutdown(workspace, slot->state);
            }
            PicoModule_Release(module);
            slot->state = NULL;
            slot->initialized = false;
            slot->active_generation = 0;
            slot->module = NULL;
            break;
        }
    }
}

void PicoWorkspaceExtensions_Shutdown(PicoWorkspace *workspace)
{
    if (!workspace)
    {
        return;
    }
    for (int i = workspace->workspace_plugin_count - 1; i >= 0; i--)
    {
        PicoPluginSlot *slot = &workspace->workspace_plugins[i];
        if (slot->initialized && slot->module)
        {
            if (slot->module->ext.workspace_shutdown)
            {
                slot->module->ext.workspace_shutdown(workspace, slot->state);
            }
            PicoModule_Release(slot->module);
            slot->state = NULL;
            slot->initialized = false;
            slot->active_generation = 0;
            slot->module = NULL;
        }
    }
    workspace->workspace_plugin_count = 0;
    PicoWorkspace_RegistrationClear(workspace);
    memset(workspace->views, 0, sizeof(workspace->views));
    memset(workspace->view_count, 0, sizeof(workspace->view_count));
    memset(workspace->empty_views, 0, sizeof(workspace->empty_views));
    workspace->empty_view_count = 0;
    memset(workspace->hooks, 0, sizeof(workspace->hooks));
    workspace->hook_count = 0;
    memset(workspace->tool_before_hooks, 0, sizeof(workspace->tool_before_hooks));
    workspace->tool_before_hook_count = 0;
    memset(workspace->tool_after_hooks, 0, sizeof(workspace->tool_after_hooks));
    workspace->tool_after_hook_count = 0;
    memset(workspace->llm_hooks, 0, sizeof(workspace->llm_hooks));
    workspace->llm_hook_count = 0;
    memset(workspace->context_hooks, 0, sizeof(workspace->context_hooks));
    workspace->context_hook_count = 0;
    memset(workspace->tool_row_hooks, 0, sizeof(workspace->tool_row_hooks));
    workspace->tool_row_hook_count = 0;
    memset(workspace->tools, 0, sizeof(workspace->tools));
    workspace->tool_count = 0;
    memset(workspace->commands, 0, sizeof(workspace->commands));
    workspace->command_count = 0;
    memset(workspace->completers, 0, sizeof(workspace->completers));
    workspace->completer_count = 0;
    memset(workspace->providers, 0, sizeof(workspace->providers));
    workspace->provider_count = 0;
}

void PicoWorkspaceExtensions_OnFrame(PicoWorkspace *workspace, float dt)
{
    if (!workspace || workspace->state == PICO_WORKSPACE_CLOSING ||
        workspace->state == PICO_WORKSPACE_CLOSED)
    {
        return;
    }
    for (int i = 0; i < workspace->workspace_plugin_count; i++)
    {
        PicoPluginSlot *slot = &workspace->workspace_plugins[i];
        if (slot->initialized && slot->module && slot->module->desired &&
            slot->module->ext.workspace_on_frame)
        {
            slot->module->ext.workspace_on_frame(workspace, slot->state, dt);
        }
    }
}

void *PicoWorkspaceExtensions_State(const PicoWorkspace *workspace, const char *name)
{
    if (!workspace || !name || !name[0])
    {
        return NULL;
    }
    for (int i = 0; i < workspace->workspace_plugin_count; i++)
    {
        if (strcmp(workspace->workspace_plugins[i].name, name) == 0 &&
            workspace->workspace_plugins[i].initialized)
        {
            return workspace->workspace_plugins[i].state;
        }
    }
    return NULL;
}

void PicoWorkspace_RunHooks(PicoWorkspace *workspace, PicoHook hook, PicoAgentId agent_id)
{
    if (!workspace)
    {
        return;
    }
    PicoHookEvent event;
    memset(&event, 0, sizeof(event));
    event.hook = hook;
    event.agent_id = agent_id;

    const PicoRegistrationGeneration *registration = workspace->active_registration;
    if (registration)
    {
        for (int i = 0; i < registration->hook_count; i++)
        {
            if (registration->hooks[i].hook == hook && registration->hooks[i].workspace_fn)
            {
                registration->hooks[i].workspace_fn(workspace, &event, registration->hooks[i].state);
            }
        }
    }
}

bool PicoWorkspace_Reload(PicoWorkspace *workspace)
{
    if (!workspace || (workspace->host && workspace->host->terminal_shutdown) ||
        PicoHost_ProcessRetired() ||
        workspace->state == PICO_WORKSPACE_CLOSING ||
        workspace->state == PICO_WORKSPACE_CLOSED)
    {
        return false;
    }
    PicoHost *host = workspace->host;
    if (!host)
    {
        return false;
    }

    if (PicoWorkspace_BlocksReload(workspace))
    {
        workspace->reload_queued = true;
        return false;
    }
    PicoWorkspace_PrepareReload(workspace);
    if (PicoWorkspace_BlocksReload(workspace))
    {
        workspace->reload_queued = true;
        return false;
    }
    workspace->reload_queued = false;
    PicoWorkspace_SetAcceptingWork(workspace, false);

    PicoWorkspace candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.host = host;
    candidate.id = workspace->id;
    snprintf(candidate.path, sizeof(candidate.path), "%s", workspace->path);
    candidate.state = workspace->state;

    candidate.registration_generation = workspace->registration_generation;

    bool staging_ok = true;
    for (int m = 0; m < host->module_count; m++)
    {
        if (workspace->state == PICO_WORKSPACE_CLOSING || workspace->state == PICO_WORKSPACE_CLOSED)
        {
            staging_ok = false;
            break;
        }
        PicoModuleGeneration *mod = &host->modules[m];
        if (!mod->desired || !mod->ext.workspace_init)
        {
            continue;
        }
        /* Check if module is workspace-local to a different workspace */
        if (!mod->builtin && strstr(mod->source, "/.pico/extensions/"))
        {
            char ws_ext_dir[8192];
            snprintf(ws_ext_dir, sizeof(ws_ext_dir), "%s/.pico/extensions", workspace->path);
            if (strncmp(mod->source, ws_ext_dir, strlen(ws_ext_dir)) != 0)
            {
                continue;
            }
        }
        if (!PicoWorkspaceExtensions_Activate(&candidate, mod))
        {
            staging_ok = false;
            /* Record error in workspace's plugin slot */
            for (int i = 0; i < workspace->workspace_plugin_count; i++)
            {
                if (strcmp(workspace->workspace_plugins[i].name, mod->ext.name) == 0)
                {
                    workspace->workspace_plugins[i].desired_generation = mod->generation;
                    if (candidate.workspace_plugin_count > 0)
                    {
                        snprintf(workspace->workspace_plugins[i].last_error,
                                 sizeof(workspace->workspace_plugins[i].last_error),
                                 "%s", candidate.workspace_plugins[candidate.workspace_plugin_count - 1].last_error);
                    }
                    break;
                }
            }
            break;
        }
    }

    if (workspace->state == PICO_WORKSPACE_CLOSING || workspace->state == PICO_WORKSPACE_CLOSED)
    {
        staging_ok = false;
    }

    if (staging_ok)
    {
        if (!candidate.active_registration)
        {
            PicoWorkspace_PublishRegistrationGeneration(&candidate);
        }

        if (workspace->state == PICO_WORKSPACE_CLOSING || workspace->state == PICO_WORKSPACE_CLOSED)
        {
            PicoWorkspaceExtensions_Shutdown(&candidate);
            PicoWorkspace_RegistrationRelease(candidate.active_registration);
            candidate.active_registration = NULL;
            return false;
        }

        /* Publication succeeded! Atomically apply candidate to workspace */
        PicoPluginSlot old_slots[PICO_MAX_EXTENSION_SLOTS];
        int old_slot_count = workspace->workspace_plugin_count;
        memcpy(old_slots, workspace->workspace_plugins, sizeof(old_slots));

        /* Copy registrations into workspace */
        memcpy(workspace->views, candidate.views, sizeof(workspace->views));
        memcpy(workspace->view_count, candidate.view_count, sizeof(workspace->view_count));
        memcpy(workspace->empty_views, candidate.empty_views, sizeof(workspace->empty_views));
        workspace->empty_view_count = candidate.empty_view_count;
        memcpy(workspace->hooks, candidate.hooks, sizeof(workspace->hooks));
        workspace->hook_count = candidate.hook_count;
        memcpy(workspace->tool_before_hooks, candidate.tool_before_hooks, sizeof(workspace->tool_before_hooks));
        workspace->tool_before_hook_count = candidate.tool_before_hook_count;
        memcpy(workspace->tool_after_hooks, candidate.tool_after_hooks, sizeof(workspace->tool_after_hooks));
        workspace->tool_after_hook_count = candidate.tool_after_hook_count;
        memcpy(workspace->llm_hooks, candidate.llm_hooks, sizeof(workspace->llm_hooks));
        workspace->llm_hook_count = candidate.llm_hook_count;
        memcpy(workspace->context_hooks, candidate.context_hooks, sizeof(workspace->context_hooks));
        workspace->context_hook_count = candidate.context_hook_count;
        memcpy(workspace->tool_row_hooks, candidate.tool_row_hooks, sizeof(workspace->tool_row_hooks));
        workspace->tool_row_hook_count = candidate.tool_row_hook_count;
        memcpy(workspace->tools, candidate.tools, sizeof(workspace->tools));
        workspace->tool_count = candidate.tool_count;
        memcpy(workspace->commands, candidate.commands, sizeof(workspace->commands));
        workspace->command_count = candidate.command_count;
        memcpy(workspace->completers, candidate.completers, sizeof(workspace->completers));
        workspace->completer_count = candidate.completer_count;
        memcpy(workspace->providers, candidate.providers, sizeof(workspace->providers));
        workspace->provider_count = candidate.provider_count;
        memcpy(workspace->workspace_plugins, candidate.workspace_plugins, sizeof(workspace->workspace_plugins));
        workspace->workspace_plugin_count = candidate.workspace_plugin_count;

        PicoRegistrationGeneration *old_reg = workspace->active_registration;
        workspace->active_registration = candidate.active_registration;
        candidate.active_registration = NULL;
        workspace->registration_generation = workspace->active_registration ? workspace->active_registration->id : candidate.registration_generation;
        PicoWorkspace_RegistrationRelease(old_reg);

        /* Shutdown old instances in reverse order */
        for (int i = old_slot_count - 1; i >= 0; i--)
        {
            PicoPluginSlot *slot = &old_slots[i];
            if (slot->initialized && slot->module)
            {
                if (slot->module->ext.workspace_shutdown)
                {
                    slot->module->ext.workspace_shutdown(workspace, slot->state);
                }
                PicoModule_Release(slot->module);
            }
        }

        PicoWorkspace_LoadProfiles(workspace);
        PicoWorkspace_RevalidateToolPolicies(workspace);
        PicoWorkspace_NotifySessions(workspace);
        PicoWorkspace_ReplayToolDetails(workspace);
        if (workspace->state != PICO_WORKSPACE_CLOSING && workspace->state != PICO_WORKSPACE_CLOSED)
        {
            PicoWorkspace_SetAcceptingWork(workspace, true);
        }
        return true;
    }

    /* Staging failed: rollback candidate instances in reverse order */
    for (int i = candidate.workspace_plugin_count - 1; i >= 0; i--)
    {
        PicoPluginSlot *slot = &candidate.workspace_plugins[i];
        if (slot->initialized && slot->module)
        {
            if (slot->module->ext.workspace_shutdown)
            {
                slot->module->ext.workspace_shutdown(&candidate, slot->state);
            }
            PicoModule_Release(slot->module);
        }
    }
    PicoWorkspace_RegistrationClear(&candidate);
    PicoWorkspace_SetAcceptingWork(workspace, true);
    return false;
}

