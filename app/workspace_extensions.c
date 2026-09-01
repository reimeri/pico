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

static bool ModuleAppliesToWorkspace(const PicoHost *host,
                                     const PicoWorkspace *workspace,
                                     const PicoModuleGeneration *module)
{
    if (!host || !workspace || !module || module->builtin)
    {
        return host && workspace && module;
    }
    PicoWorkspace *owner = PicoHost_SourceWorkspace(host, module->source);
    return !owner || owner == workspace;
}

static bool WorkspaceSlotMatchesModule(const PicoPluginSlot *slot,
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

static bool ActivateWorkspaceModule(PicoWorkspace *workspace, PicoWorkspace *target,
                                    PicoModuleGeneration *module)
{
    if (!workspace || !target || !module)
    {
        return false;
    }
    if (!module->ext.workspace_init)
    {
        return true;
    }

    PicoPluginSlot *slot = NULL;
    for (int i = 0; i < target->workspace_plugin_count; i++)
    {
        if (WorkspaceSlotMatchesModule(&target->workspace_plugins[i], module))
        {
            slot = &target->workspace_plugins[i];
            break;
        }
    }
    if (!slot && target->workspace_plugin_count < PICO_MAX_EXTENSION_SLOTS)
    {
        slot = &target->workspace_plugins[target->workspace_plugin_count++];
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->name, sizeof(slot->name), "%s", module->ext.name);
        slot->source = module->source[0] ? module->source : NULL;
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
    PicoHost_BeginWorkspaceRegistration(host, workspace, target);
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

bool PicoWorkspaceExtensions_Activate(PicoWorkspace *workspace, PicoModuleGeneration *module)
{
    return ActivateWorkspaceModule(workspace, workspace, module);
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
        if (slot->initialized && slot->module &&
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

typedef struct PicoWorkspaceExtensionSet {
    uint64_t registration_generation;
    PicoRegistrationGeneration *active_registration;
    PicoSlotView views[PICO_SLOT_COUNT][PICO_MAX_SLOT_VIEWS];
    int view_count[PICO_SLOT_COUNT];
    PicoEmptyView empty_views[PICO_MAX_EMPTY_VIEWS];
    int empty_view_count;
    PicoHookEntry hooks[PICO_MAX_HOOKS];
    int hook_count;
    PicoToolBeforeEntry tool_before_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_before_hook_count;
    PicoToolAfterEntry tool_after_hooks[PICO_MAX_TOOL_HOOKS];
    int tool_after_hook_count;
    PicoLlmHookEntry llm_hooks[PICO_MAX_LLM_HOOKS];
    int llm_hook_count;
    PicoContextHookEntry context_hooks[PICO_MAX_CONTEXT_HOOKS];
    int context_hook_count;
    PicoToolRowEntry tool_row_hooks[PICO_MAX_TOOL_ROW_HOOKS];
    int tool_row_hook_count;
    PicoTool tools[PICO_MAX_TOOLS];
    int tool_count;
    PicoCommand commands[PICO_MAX_COMMANDS];
    int command_count;
    PicoCompleter completers[PICO_MAX_COMPLETERS];
    int completer_count;
    PicoProvider providers[PICO_MAX_PROVIDERS];
    int provider_count;
    PicoPluginSlot plugins[PICO_MAX_EXTENSION_SLOTS];
    int plugin_count;
} PicoWorkspaceExtensionSet;

static void CaptureWorkspaceExtensionSet(const PicoWorkspace *workspace,
                                         PicoWorkspaceExtensionSet *set)
{
    memset(set, 0, sizeof(*set));
    set->registration_generation = workspace->registration_generation;
    set->active_registration = workspace->active_registration;
    memcpy(set->views, workspace->views, sizeof(set->views));
    memcpy(set->view_count, workspace->view_count, sizeof(set->view_count));
    memcpy(set->empty_views, workspace->empty_views, sizeof(set->empty_views));
    set->empty_view_count = workspace->empty_view_count;
    memcpy(set->hooks, workspace->hooks, sizeof(set->hooks));
    set->hook_count = workspace->hook_count;
    memcpy(set->tool_before_hooks, workspace->tool_before_hooks, sizeof(set->tool_before_hooks));
    set->tool_before_hook_count = workspace->tool_before_hook_count;
    memcpy(set->tool_after_hooks, workspace->tool_after_hooks, sizeof(set->tool_after_hooks));
    set->tool_after_hook_count = workspace->tool_after_hook_count;
    memcpy(set->llm_hooks, workspace->llm_hooks, sizeof(set->llm_hooks));
    set->llm_hook_count = workspace->llm_hook_count;
    memcpy(set->context_hooks, workspace->context_hooks, sizeof(set->context_hooks));
    set->context_hook_count = workspace->context_hook_count;
    memcpy(set->tool_row_hooks, workspace->tool_row_hooks, sizeof(set->tool_row_hooks));
    set->tool_row_hook_count = workspace->tool_row_hook_count;
    memcpy(set->tools, workspace->tools, sizeof(set->tools));
    set->tool_count = workspace->tool_count;
    memcpy(set->commands, workspace->commands, sizeof(set->commands));
    set->command_count = workspace->command_count;
    memcpy(set->completers, workspace->completers, sizeof(set->completers));
    set->completer_count = workspace->completer_count;
    memcpy(set->providers, workspace->providers, sizeof(set->providers));
    set->provider_count = workspace->provider_count;
    memcpy(set->plugins, workspace->workspace_plugins, sizeof(set->plugins));
    set->plugin_count = workspace->workspace_plugin_count;
}

static void InstallWorkspaceExtensionSet(PicoWorkspace *workspace,
                                         const PicoWorkspaceExtensionSet *set)
{
    memcpy(workspace->views, set->views, sizeof(workspace->views));
    memcpy(workspace->view_count, set->view_count, sizeof(workspace->view_count));
    memcpy(workspace->empty_views, set->empty_views, sizeof(workspace->empty_views));
    workspace->empty_view_count = set->empty_view_count;
    memcpy(workspace->hooks, set->hooks, sizeof(workspace->hooks));
    workspace->hook_count = set->hook_count;
    memcpy(workspace->tool_before_hooks, set->tool_before_hooks, sizeof(workspace->tool_before_hooks));
    workspace->tool_before_hook_count = set->tool_before_hook_count;
    memcpy(workspace->tool_after_hooks, set->tool_after_hooks, sizeof(workspace->tool_after_hooks));
    workspace->tool_after_hook_count = set->tool_after_hook_count;
    memcpy(workspace->llm_hooks, set->llm_hooks, sizeof(workspace->llm_hooks));
    workspace->llm_hook_count = set->llm_hook_count;
    memcpy(workspace->context_hooks, set->context_hooks, sizeof(workspace->context_hooks));
    workspace->context_hook_count = set->context_hook_count;
    memcpy(workspace->tool_row_hooks, set->tool_row_hooks, sizeof(workspace->tool_row_hooks));
    workspace->tool_row_hook_count = set->tool_row_hook_count;
    memcpy(workspace->tools, set->tools, sizeof(workspace->tools));
    workspace->tool_count = set->tool_count;
    memcpy(workspace->commands, set->commands, sizeof(workspace->commands));
    workspace->command_count = set->command_count;
    memcpy(workspace->completers, set->completers, sizeof(workspace->completers));
    workspace->completer_count = set->completer_count;
    memcpy(workspace->providers, set->providers, sizeof(workspace->providers));
    workspace->provider_count = set->provider_count;
    memcpy(workspace->workspace_plugins, set->plugins, sizeof(workspace->workspace_plugins));
    workspace->workspace_plugin_count = set->plugin_count;
    workspace->registration_generation = set->registration_generation;
    workspace->active_registration = set->active_registration;
}

static void ShutdownWorkspaceSlots(PicoWorkspace *workspace, PicoPluginSlot *slots, int count)
{
    for (int i = count - 1; i >= 0; i--)
    {
        PicoPluginSlot *slot = &slots[i];
        if (slot->initialized && slot->module)
        {
            if (slot->module->ext.workspace_shutdown)
            {
                slot->module->ext.workspace_shutdown(workspace, slot->state);
            }
            PicoModule_Release(slot->module);
            slot->state = NULL;
            slot->initialized = false;
            slot->module = NULL;
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
    if (workspace->state == PICO_WORKSPACE_OPEN)
    {
        workspace->reload_retry_compile_failures = true;
    }

    if (PicoWorkspace_BlocksReload(workspace))
    {
        workspace->reload_queued = true;
        if (workspace->state == PICO_WORKSPACE_OPEN)
        {
            workspace->state = PICO_WORKSPACE_RELOADING;
        }
        PicoWorkspace_SetAcceptingWork(workspace, false);
        return false;
    }
    PicoWorkspace_PrepareReload(workspace);
    if (PicoWorkspace_BlocksReload(workspace))
    {
        workspace->reload_queued = true;
        if (workspace->state == PICO_WORKSPACE_OPEN)
        {
            workspace->state = PICO_WORKSPACE_RELOADING;
        }
        PicoWorkspace_SetAcceptingWork(workspace, false);
        return false;
    }
    workspace->reload_queued = false;
    PicoWorkspace_SetAcceptingWork(workspace, false);

    PicoPlugins_LoadWorkspaceSources(host, workspace);
    workspace->reload_retry_compile_failures = false;

    PicoWorkspaceExtensionSet old;
    CaptureWorkspaceExtensionSet(workspace, &old);

    /* Registrations and instances are built in separate storage, while every
     * init and shutdown callback receives the live owning workspace. */
    PicoWorkspace candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.host = host;
    candidate.id = workspace->id;
    candidate.state = workspace->state;
    candidate.registration_generation = workspace->registration_generation;

    bool staging_ok = true;
    PicoModuleGeneration *failed_module = NULL;
    char failed_error[1024] = {0};
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
        if (!ModuleAppliesToWorkspace(host, workspace, mod))
        {
            continue;
        }
        if (!ActivateWorkspaceModule(workspace, &candidate, mod))
        {
            staging_ok = false;
            failed_module = mod;
            for (int i = 0; i < candidate.workspace_plugin_count; i++)
            {
                if (WorkspaceSlotMatchesModule(&candidate.workspace_plugins[i], mod))
                {
                    snprintf(failed_error, sizeof(failed_error), "%s",
                             candidate.workspace_plugins[i].last_error);
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
    if (staging_ok && !candidate.active_registration)
    {
        staging_ok = PicoWorkspace_PublishRegistrationGeneration(&candidate);
    }

    if (staging_ok)
    {
        PicoWorkspaceExtensionSet next;
        CaptureWorkspaceExtensionSet(&candidate, &next);
        InstallWorkspaceExtensionSet(workspace, &next);
        candidate.active_registration = NULL;

        ShutdownWorkspaceSlots(workspace, old.plugins, old.plugin_count);
        PicoWorkspace_RegistrationRelease(old.active_registration);

        PicoWorkspace_LoadProfiles(workspace);
        PicoWorkspace_RevalidateToolPolicies(workspace);
        PicoWorkspace_NotifySessions(workspace);
        PicoWorkspace_ReplayToolDetails(workspace);
        if (workspace->state != PICO_WORKSPACE_CLOSING && workspace->state != PICO_WORKSPACE_CLOSED)
        {
            workspace->state = PICO_WORKSPACE_OPEN;
            PicoWorkspace_SetAcceptingWork(workspace, true);
        }
        return true;
    }

    ShutdownWorkspaceSlots(workspace, candidate.workspace_plugins,
                           candidate.workspace_plugin_count);
    PicoWorkspace_RegistrationClear(&candidate);

    if (failed_module)
    {
        PicoPluginSlot *slot = NULL;
        for (int i = 0; i < workspace->workspace_plugin_count; i++)
        {
            if (WorkspaceSlotMatchesModule(&workspace->workspace_plugins[i], failed_module))
            {
                slot = &workspace->workspace_plugins[i];
                break;
            }
        }
        if (!slot && workspace->workspace_plugin_count < PICO_MAX_EXTENSION_SLOTS)
        {
            slot = &workspace->workspace_plugins[workspace->workspace_plugin_count++];
            memset(slot, 0, sizeof(*slot));
            snprintf(slot->name, sizeof(slot->name), "%s", failed_module->ext.name);
            slot->source = failed_module->source[0] ? failed_module->source : NULL;
        }
        if (slot)
        {
            slot->desired_generation = failed_module->generation;
            snprintf(slot->last_error, sizeof(slot->last_error), "%s", failed_error);
        }
    }

    if (workspace->state != PICO_WORKSPACE_CLOSING &&
        workspace->state != PICO_WORKSPACE_CLOSED)
    {
        workspace->state = PICO_WORKSPACE_OPEN;
        PicoWorkspace_SetAcceptingWork(workspace, true);
    }
    return false;
}

