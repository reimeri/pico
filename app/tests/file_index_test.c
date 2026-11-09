#define _GNU_SOURCE
#include "file_index.h"
#include "test_wait.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char root[4096], cache[4096];
static int failed;
#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", message); failed = 1; return; } } while (0)
static void Write(const char *rel, const char *text)
{
    char path[8192]; snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(2); } fputs(text, f); fclose(f);
}
static void Directory(const char *rel)
{ char path[8192]; snprintf(path, sizeof(path), "%s/%s", root, rel); if (mkdir(path, 0700)) { perror(path); exit(2); } }
static void Git(const char *arg, const char *value)
{
    pid_t p = fork();
    if (!p) { execlp("git", "git", "-C", root, arg, value, (char *)NULL); _exit(127); }
    int status; waitpid(p, &status, 0); if (status) { fprintf(stderr, "git fixture failed\n"); exit(2); }
}
static PicoFileIndexResult Query(PicoFileIndex *i, uint64_t token, const char *q)
{
    PicoFileIndexResult r;
    PicoFileIndex_Query(i, token, q, &r);
    PICO_TEST_WAIT(r.pending) { PicoFileIndex_Query(i, token, q, &r); }
    return r;
}
static bool Has(PicoFileIndexResult r, const char *path)
{ for (int n = 0; n < r.count; n++) if (!strcmp(r.paths[n], path)) return true; return false; }
static PicoFileIndexInfo Ready(PicoFileIndex *i, uint64_t after)
{
    PicoFileIndexInfo info;
    PicoFileIndex_Info(i, &info);
    PICO_TEST_WAIT(info.generation <= after || info.refreshing || info.cached) { PicoFileIndex_Info(i, &info); }
    return info;
}
static void GitTrack(const char *path, bool force)
{
    pid_t p = fork();
    if (!p)
    {
        if (force) execlp("git", "git", "-C", root, "add", "-f", "--", path, (char *)NULL);
        else execlp("git", "git", "-C", root, "add", "--", path, (char *)NULL);
        _exit(127);
    }
    int status; waitpid(p, &status, 0); if (status) exit(2);
}
static void TestCorpus(void)
{
    Git("init", "-q");
    Directory("Assets"); Directory("notes"); Directory("ignored"); Directory(".pico");
    Write("ignored/tracked.md", "tracked"); GitTrack("ignored/tracked.md", true);
    Write(".gitignore", "ignored/\n"); Write("ignored/secret.md", "hidden");
    Write("notes/design notes.md", "wanted"); Write(".pico/internal.md", "private");
    Write("deleted.md", "deleted"); GitTrack("deleted.md", false);
    char path[8192]; snprintf(path, sizeof(path), "%s/deleted.md", root); unlink(path);
    for (int n = 0; n < 12000; n++) { char p[80]; snprintf(p, sizeof(p), "Assets/asset_%05d.png", n); Write(p, ""); }
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i, "open index");
    PicoFileIndexInfo info = Ready(i, 0);
    CHECK(info.files > 12000, "complete searchable corpus exceeds previous cutoff");
    CHECK(Has(Query(i, 1, "design notes"), "notes/design notes.md"), "late-directory file is searchable by filename");
    CHECK(!Has(Query(i, 1, "secret.md"), "ignored/secret.md"), "ignored untracked paths stay out of normal discovery");
    CHECK(Has(Query(i, 1, "tracked.md"), "ignored/tracked.md"), "tracked ignored files remain discoverable");
    CHECK(!Has(Query(i, 1, "deleted.md"), "deleted.md"), "deleted tracked paths are not offered");
    CHECK(!Has(Query(i, 1, "internal.md"), ".pico/internal.md"), "internal session files are not suggested");
    /* Directory has a tracked file too: explicit scope must discover its ignored siblings. */
    CHECK(Has(Query(i, 1, "ignored/secret"), "ignored/secret.md"), "explicit ignored-directory query discovers ignored files");
    PicoFileIndex_Close(i);
}
static void TestSnapshots(void)
{
    Git("init", "-q"); Write("one.md", "one");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i, "open index"); Ready(i, 0);
    PicoFileIndexResult old = Query(i, 1, "one"); CHECK(Has(old, "one.md"), "initial snapshot");
    Write("two.md", "two"); PicoFileIndex_Refresh(i); Ready(i, old.generation);
    CHECK(!Has(Query(i, 1, "two"), "two.md"), "active token retains its old generation during refresh");
    CHECK(Has(Query(i, 2, "two"), "two.md"), "new token adopts refreshed generation");
    CHECK(Has(Query(i, 2, "one"), "one.md"), "backspacing/broadening does not search a narrowed result set");
    for (int n = 0; n < 1000; n++) { PicoFileIndexResult r; PicoFileIndex_Query(i, 2, n%2 ? "missing" : "two", &r); }
    CHECK(Has(Query(i, 2, "one"), "one.md"), "superseded queries cannot publish stale suggestions");
    PicoFileIndex_Close(i);
}
static void TestWatch(void)
{
    Git("init", "-q"); Write("one.md", "one");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i, "open index");
    PicoFileIndexInfo info = Ready(i, 0); Write("new.md", "new");
    Ready(i, info.generation);
    CHECK(Has(Query(i, 1, "new.md"), "new.md"), "filesystem changes refresh later token suggestions without manual rescan");
    char a[8192], b[8192]; snprintf(a, sizeof(a), "%s/new.md", root); snprintf(b, sizeof(b), "%s/renamed.md", root); rename(a,b);
    info = Ready(i, info.generation+1);
    CHECK(Has(Query(i, 2, "renamed.md"), "renamed.md"), "rename is reflected in a later generation");
    PicoFileIndex_Close(i);
}
static void TestCache(void)
{
    Git("init", "-q"); Write("persisted.md", "one");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i, "open index"); Ready(i,0); PicoFileIndex_Close(i);
    i = PicoFileIndex_Open(root); CHECK(i, "reopen cached index");
    CHECK(Has(Query(i, 1, "persisted.md"), "persisted.md"), "persisted paths are usable after reopening");
    Ready(i,0); PicoFileIndex_Close(i);
    /* Corrupt caches are disposable, not fatal and never trusted as paths. */
    char dir[8192]; snprintf(dir, sizeof(dir), "%s/pico/files", cache); DIR *d = opendir(dir); CHECK(d, "cache directory exists");
    struct dirent *e;
    while ((e = readdir(d))) if (strstr(e->d_name, ".idx"))
    { char path[16384]; snprintf(path, sizeof(path), "%s/%s", dir,e->d_name); FILE *f = fopen(path,"w"); CHECK(f,"open cache"); fputs("corrupt",f); fclose(f); }
    closedir(d);
    i = PicoFileIndex_Open(root); CHECK(i,"open after cache corruption");
    CHECK(Has(Query(i, 1, "persisted.md"), "persisted.md"), "bad cache triggers successful rediscovery");
    PicoFileIndex_Close(i);
}
static void TestSymlinks(void)
{
    Directory("docs"); Write("docs/real.md", "one");
    char path[8192]; snprintf(path,sizeof(path),"%s/docs/loop",root); CHECK(!symlink(root,path),"create loop symlink");
    snprintf(path,sizeof(path),"%s/alias",root); CHECK(!symlink("docs",path),"create alias symlink");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open index"); Ready(i,0);
    CHECK(Has(Query(i,1,"real.md"),"docs/real.md"),"regular file survives symlink cycle");
    CHECK(!Has(Query(i,1,"alias/real"),"alias/real.md"),"explicit scopes do not recurse directory symlinks");
    PicoFileIndex_Close(i);
}
static void TestDeepNativeTree(void)
{
    setenv("PATH","/nonexistent",1);
    char dir[2048] = "";
    for (int n = 0; n < 512; n++) { strcat(dir,n ? "/d" : "d"); Directory(dir); }
    char path[2200]; snprintf(path,sizeof(path),"%s/deep.md",dir); Write(path,"deep");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open deep native tree");
    /* Long labels are intentionally not insertable; discovery must nonetheless
     * complete, without depth truncation or recursive stack exhaustion. */
    PicoFileIndexInfo info = Ready(i,0);
    CHECK(info.files == 1,"deep native traversal discovers its regular file");
    PicoFileIndex_Close(i);
}
static void TestSeparateProject(void)
{
    Git("init","-q"); Directory("nested");
    char parent[4096]; snprintf(parent,sizeof(parent),"%s",root);
    CHECK(strlen(parent)+8 < sizeof(root),"nested fixture fits path"); strcpy(root,parent); strcat(root,"/nested"); Git("init","-q"); Write("nested-only.md","one");
    snprintf(root,sizeof(root),"%s",parent); Write("parent.md","two");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open parent project"); Ready(i,0);
    CHECK(!Has(Query(i,1,"nested-only"),"nested/nested-only.md"),"nested Git projects are not indexed by their parent");
    CHECK(!Has(Query(i,1,"nested/nested-only"),"nested/nested-only.md"),"scoped discovery respects separate-project boundaries");
    PicoFileIndex_Close(i);
}
static void TestCacheInsideWorkspace(void)
{
    Git("init","-q"); Write("only.md","one");
    char local[8192]; snprintf(local,sizeof(local),"%s/local-cache",root); setenv("XDG_CACHE_HOME",local,1);
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open with workspace-local cache");
    PicoFileIndexInfo info = Ready(i,0);
    PicoFileIndex_Refresh(i); info = Ready(i,info.generation);
    CHECK(info.files == 1 && Query(i,1,".idx").count == 0,"service cache is excluded even when stored inside the indexed workspace");
    PicoFileIndex_Close(i);
}
static void TestFallback(void)
{
    setenv("PATH", "/nonexistent", 1); Write("fallback.md", "one");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open native fallback");
    PicoFileIndexResult r = Query(i,1,"fallback.md");
    CHECK(Has(r,"fallback.md") && strstr(r.status,"without ignore rules"), "missing tools produce native results with an explicit warning");
    PicoFileIndex_Close(i);
}
static void MakeExecutable(const char *rel, const char *text)
{
    Write(rel,text); char path[8192]; snprintf(path,sizeof(path),"%s/%s",root,rel);
    if (chmod(path,0700)) exit(2);
}
static pid_t WaitChildPid(void)
{
    char path[8192]; snprintf(path,sizeof(path),"%s/child.pid",root);
    long pid = 0;
    PICO_TEST_WAIT(pid <= 0)
    {
        FILE *f = fopen(path,"r");
        if (f) { if (fscanf(f,"%ld",&pid) != 1) pid = 0; fclose(f); }
    }
    return (pid_t)pid;
}
static void BlockingGit(void)
{
    Directory("bin");
    char script[16384];
    snprintf(script,sizeof(script),"#!/bin/sh\necho $$ > '%s/child.pid'\nexec /bin/sh -c 'while :; do :; done'\n",root);
    MakeExecutable("bin/git",script);
    char bin[8192]; snprintf(bin,sizeof(bin),"%s/bin",root); setenv("PATH",bin,1);
}
static void TestCloseReapsChild(void)
{
    BlockingGit();
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open blocked enumerator");
    pid_t child = WaitChildPid();
    PicoFileIndex_Close(i);
    PICO_TEST_WAIT(kill(child,0) == 0) {}
    CHECK(errno == ESRCH,"closing the index kills and reaps its enumerator without waiting on the UI thread");
}
static void TestCachedSnapshot(void)
{
    Git("init","-q"); Write("cached.md","one");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open cached fixture"); Ready(i,0); PicoFileIndex_Close(i);
    Write("new.md","two");
    BlockingGit();
    i = PicoFileIndex_Open(root); CHECK(i,"reopen while reconciliation is blocked");
    pid_t child = WaitChildPid();
    PicoFileIndexResult result = Query(i,1,"cached.md");
    PicoFileIndexInfo info; PicoFileIndex_Info(i,&info);
    CHECK(Has(result,"cached.md") && info.cached,"persisted cache serves results before blocked reconciliation completes");
    CHECK(!result.status[0],"usable cached results do not add an informational popup row");
    CHECK(!Has(Query(i,1,"new.md"),"new.md"),"cached token does not pretend to be freshly reconciled");
    PicoFileIndex_Close(i);
    PICO_TEST_WAIT(kill(child,0) == 0) {}
}
static void TestSupersededScope(void)
{
    Git("init","-q"); Directory("docs"); Write("docs/found.md","one"); Write("outside.md","two");
    PicoFileIndex *i = PicoFileIndex_Open(root); CHECK(i,"open scope fixture"); Ready(i,0);
    /* Replace git only after normal discovery; a scoped ignore check must be
     * cancellable independently from workspace-wide discovery. */
    BlockingGit();
    PicoFileIndexResult r; PicoFileIndex_Query(i,1,"docs/",&r);
    pid_t child = WaitChildPid();
    CHECK(Has(Query(i,1,"outside.md"),"outside.md"),"editing away from a blocked scope cancels it and searches the workspace");
    PICO_TEST_WAIT(kill(child,0) == 0) {}
    PicoFileIndex_Close(i);
}
static int Remove(const char *path, const struct stat *st, int type, struct FTW *ftw)
{ (void)st;(void)type;(void)ftw;return remove(path); }
int main(void)
{
    void (*tests[])(void) = {TestCorpus, TestSnapshots, TestWatch, TestCache, TestSymlinks, TestDeepNativeTree, TestSeparateProject, TestCacheInsideWorkspace, TestFallback, TestCloseReapsChild, TestCachedSnapshot, TestSupersededScope};
    for (size_t n = 0; n < sizeof(tests)/sizeof(tests[0]); n++)
    {
        char tmp[] = "/tmp/pico-index-test-XXXXXX"; if (!mkdtemp(tmp)) return 2;
        snprintf(root,sizeof(root),"%s/project",tmp); mkdir(root,0700);
        snprintf(cache,sizeof(cache),"%s/cache",tmp); mkdir(cache,0700);
        pid_t p = fork();
        if (!p) { setenv("XDG_CACHE_HOME",cache,1); tests[n](); _exit(failed ? 1 : 0); }
        int status; waitpid(p,&status,0); nftw(tmp,Remove,32,FTW_DEPTH|FTW_PHYS);
        if (status) return 1;
    }
    puts("file index contracts passed"); return 0;
}
