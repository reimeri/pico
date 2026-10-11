#define _POSIX_C_SOURCE 200809L
#include "pico/plugin.h"
#include "host_internal.h"
#include "workspace_internal.h"
#include "agent_internal.h"
#include "session.h"
#include "complete_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Completion rows carry short opaque handles; absolute paths need not fit the
 * public completion item's insert buffer. Worker jobs own copied identities. */
typedef struct SessionChoice {
    bool project;
    union {
        PicoCatalogProjectMatch group;
        PicoCatalogReferenceMatch session;
    } data;
} SessionChoice;

typedef struct SessionQueryKey {
    PicoWorkspaceId workspace;
    PicoAgentId agent;
    uint64_t token;
    char project[4096];
    char checkout[4096];
    char exclude_id[40];
    char prefix[512];
    char pinned_project[4096];
    int qualifier_length;
} SessionQueryKey;

typedef struct SessionCompletion {
    uint64_t generation;
    uint64_t results;
    bool pending;
    bool valid;
    SessionQueryKey requested;
    SessionQueryKey cached;
    SessionChoice *choices;
    int count;
    char scope_project[4096];
    char scope_prefix[512];
    uint64_t scope_token;
    bool scope_pending;
    PicoWorkspaceId scope_workspace;
    PicoAgentId scope_agent;
} SessionCompletion;

typedef struct SessionQueryTask {
    uint64_t generation;
    SessionCompletion *owner; /* compared only after checking current builtin */
    SessionQueryKey key;
    SessionChoice *choices;
    int count;
    bool success;
} SessionQueryTask;

static uint64_t s_generation;

static int QualifierLength(const SessionQueryKey *key)
{
    if (key->qualifier_length > 0) return key->qualifier_length;
    const char *colon = strchr(key->prefix, ':');
    return colon ? (int)(colon-key->prefix)+1 : 0;
}

/* Only the query after a qualifier can vary within a cached search scope.
 * A different token, conversation or project must not inherit old rows. */
static bool SameScope(const SessionQueryKey *a, const SessionQueryKey *b)
{
    int qualifier = QualifierLength(a);
    return a->workspace == b->workspace && a->agent == b->agent && a->token == b->token &&
        !strcmp(a->project,b->project) && !strcmp(a->checkout,b->checkout) &&
        !strcmp(a->exclude_id,b->exclude_id) && !strcmp(a->pinned_project,b->pinned_project) &&
        a->qualifier_length == b->qualifier_length && qualifier == QualifierLength(b) &&
        !strncmp(a->prefix,b->prefix,(size_t)qualifier);
}

static bool ContainsFold(const char *text, const char *query)
{
    size_t length = strlen(query);
    if (!length) return true;
    for (; *text; text++)
        if (!strncasecmp(text,query,length)) return true;
    return false;
}

static bool ChoiceMatches(const SessionChoice *choice, const SessionQueryKey *key)
{
    int qualifier = QualifierLength(key);
    if (choice->project)
    {
        /* Unresolved/ambiguous qualified searches still offer project choices,
         * matched against the name before ':', not the session query. */
        char name[sizeof(key->prefix)];
        snprintf(name,sizeof(name),"%s",key->prefix);
        if (qualifier) name[qualifier-1] = '\0';
        return ContainsFold(choice->data.group.name,name);
    }
    const char *query = key->prefix+qualifier;
    return ContainsFold(choice->data.session.ref.title,query) || ContainsFold(choice->data.session.ref.id,query);
}

static void *SessionQueryRead(void *arg)
{
    SessionQueryTask *task = arg;
    PicoCatalogProjectMatch *groups = NULL;
    PicoCatalogReferenceMatch *sessions = NULL;
    int group_count = PicoCatalog_SearchProjects(NULL,false,&groups,0);
    int session_count = 0;
    if (group_count < 0) goto done;

    /* Prefix edits never change the searchable snapshot. Only a qualifier
     * changes which project's sessions are loaded. Names are resolved from
     * the complete project metadata, not the popup-sized displayed list. */
    int qualifier = QualifierLength(&task->key);
    const char *project = task->key.project;
    bool offer_projects = qualifier == 0;
    bool load_sessions = true;
    if (qualifier)
    {
        if (task->key.pinned_project[0]) project = task->key.pinned_project;
        else
        {
            char name[sizeof(task->key.prefix)];
            memcpy(name,task->key.prefix,(size_t)qualifier-1);
            name[qualifier-1] = '\0';
            int exact = -1, matches = 0;
            for (int i=0; i<group_count; i++)
                if (!strcasecmp(groups[i].name,name)) { exact=i; matches++; }
            if (matches == 1) project=groups[exact].path;
            else { offer_projects=true; load_sessions=false; }
        }
    }
    if (load_sessions)
    {
        session_count = PicoCatalog_SearchReferences(project,NULL,task->key.checkout,
            task->key.exclude_id,&sessions,0);
        if (session_count < 0) goto done;
    }
    int shown_groups = offer_projects ? group_count : 0;
    if (session_count > INT_MAX-shown_groups) goto done;
    int count = session_count+shown_groups;
    if (count)
    {
        if ((size_t)count > SIZE_MAX/sizeof(*task->choices)) goto done;
        task->choices = calloc((size_t)count,sizeof(*task->choices));
        if (!task->choices) goto done;
        for (int i=0; i<session_count; i++) task->choices[i].data.session=sessions[i];
        for (int i=0; i<shown_groups; i++)
        {
            SessionChoice *row=&task->choices[session_count+i];
            row->project=true;
            row->data.group=groups[i];
        }
    }
    task->count=count;
    task->success=true;
done:
    free(sessions); free(groups);
    return NULL;
}

static void SessionQueryDestroy(void *arg)
{
    SessionQueryTask *task = arg;
    free(task->choices); free(task);
}

static void SessionQueryDone(PicoHost *host, void *arg)
{
    SessionQueryTask *task = arg;
    SessionCompletion *s = PicoPlugins_HostState(host, "sessions");
    if (!s || s != task->owner || s->generation != task->generation) return;
    s->pending = false;
    if (task->success && SameScope(&s->requested, &task->key))
    {
        free(s->choices); s->choices = task->choices; task->choices = NULL;
        s->count = task->count; s->cached = task->key; s->valid = true;
        s->results++;
    }
    PicoComplete_Refresh(host);
    pico_host_request_redraw(host);
}

static int SessionQuery(PicoHost *host, const char *prefix, PicoCompleteItem *out, int max, void *state)
{
    SessionCompletion *s = state;
    PicoWorkspace *ws = PicoHost_SelectedWorkspace(host);
    PicoAgent *agent = PicoHost_SelectedAgent(host);
    if (!s || !ws || !agent) return 0;
    SessionQueryKey key = {0};
    key.workspace = ws->id; key.agent = agent->id; key.token = PicoComplete_TokenId();
    snprintf(key.project, sizeof(key.project), "%s", ws->project_path[0] ? ws->project_path : ws->path);
    snprintf(key.checkout, sizeof(key.checkout), "%s", ws->path);
    snprintf(key.exclude_id, sizeof(key.exclude_id), "%s", agent->session_id);
    snprintf(key.prefix, sizeof(key.prefix), "%s", prefix);
    if (s->scope_project[0])
    {
        size_t n = strlen(s->scope_prefix);
        if (ws->id == s->scope_workspace && agent->id == s->scope_agent &&
            !strncmp(prefix, s->scope_prefix, n) && (s->scope_pending || s->scope_token == key.token))
        {
            snprintf(key.pinned_project, sizeof(key.pinned_project), "%s", s->scope_project);
            key.qualifier_length=(int)n;
            s->scope_token = key.token; s->scope_pending = false;
        }
        else { s->scope_project[0] = '\0'; s->scope_pending = false; }
    }
    s->requested = key;
    if (!s->pending && (!s->valid || !SameScope(&key,&s->cached)))
    {
        SessionQueryTask *task = calloc(1,sizeof(*task));
        if (task)
        {
            task->owner = s; task->generation = s->generation; task->key = key;
            if (PicoHost_StartTaskCompleted(host, SessionQueryRead, task, NULL, SessionQueryDone, SessionQueryDestroy))
                s->pending = true;
            else free(task);
        }
    }
    if (!s->valid || !SameScope(&key,&s->cached)) return 0;
    int project_rows = 0;
    for (int i=0; i<s->count; i++)
        if (s->choices[i].project && ChoiceMatches(&s->choices[i],&key)) project_rows++;
    int project_budget = QualifierLength(&key) ? max : max/3;
    if (project_budget > project_rows) project_budget = project_rows;
    int n = 0;
    for (int pass=0; pass<2; pass++)
    {
        int limit = pass == 0 ? max-project_budget : max;
        for (int i=0; i<s->count && n<limit; i++)
        {
            const SessionChoice *row = &s->choices[i];
            if (row->project != (pass == 1) || !ChoiceMatches(row,&key)) continue;
            PicoCompleteItem *item = &out[n++];
            memset(item,0,sizeof(*item));
            if (row->project)
            {
                snprintf(item->label,sizeof(item->label),"%s:",row->data.group.name);
                snprintf(item->detail,sizeof(item->detail),"Project · %.100s",row->data.group.path);
            }
            else
            {
                snprintf(item->label,sizeof(item->label),"%s",row->data.session.ref.title);
                snprintf(item->detail,sizeof(item->detail),"%.65s%s · %.39s",row->data.session.checkout_name,
                    row->data.session.worktree ? " (worktree)" : "",row->data.session.ref.id);
            }
            snprintf(item->insert,sizeof(item->insert),"session-choice:%llu:%d",(unsigned long long)s->results,i);
        }
    }
    return n;
}

static bool SessionAccept(PicoHost *host, const PicoCompleteItem *item, void *state)
{
    SessionCompletion *s = state;
    unsigned long long results = 0; int i = -1, start = 0, end = 0;
    if (sscanf(item->insert,"session-choice:%llu:%d",&results,&i) != 2 ||
        results != s->results || i < 0 || i >= s->count ||
        !s->valid || !SameScope(&s->requested,&s->cached) ||
        !ChoiceMatches(&s->choices[i],&s->requested) || !PicoComplete_TokenRange(host,&start,&end)) return true;
    SessionChoice *row = &s->choices[i];
    char *text;
    if (row->project)
    {
        text = malloc(strlen(row->data.group.name)+3);
        if (!text) return true;
        sprintf(text,"#%s:",row->data.group.name);
        snprintf(s->scope_project,sizeof(s->scope_project),"%s",row->data.group.path);
        snprintf(s->scope_prefix,sizeof(s->scope_prefix),"%s:",row->data.group.name);
        s->scope_pending = true;
        s->scope_workspace = s->requested.workspace; s->scope_agent = s->requested.agent;
    }
    else
    {
        text = PicoSessionRef_Serialize(&row->data.session.ref);
        s->scope_project[0] = '\0'; s->scope_pending = false;
    }
    if (text)
    {
        PicoComposer_ReplaceRange(host,start,end,text);
        if (row->project) PicoComplete_SetScope(host,start,text);
        free(text);
    }
    return true;
}

static int SessionsInit(PicoHost *host, void **state_out)
{
    SessionCompletion *s = calloc(1,sizeof(*s));
    if (!s) return -1;
    s->generation = ++s_generation;
    *state_out = s;
    pico_host_add_completer(host,'#',false,SessionQuery,SessionAccept);
    return 0;
}

static void SessionsShutdown(PicoHost *host, void *state)
{
    (void)host;
    SessionCompletion *s = state;
    if (s) { free(s->choices); free(s); }
}

PicoExt pico_ext_sessions(void)
{
    return (PicoExt){.abi=PICO_EXT_ABI,.name="sessions",.description="Session references",
        .host_init=SessionsInit,.host_shutdown=SessionsShutdown};
}
