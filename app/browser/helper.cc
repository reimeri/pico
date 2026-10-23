#include "protocol.h"
#include "include/cef_app.h"
#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_closure_task.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/file.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

namespace {
int events_fd = -1;
bool stopping = false;
std::string profile_root;
class Page;
void Command(PbCommand command);
std::map<uint64_t, CefRefPtr<Page>> pages;

bool Send(uint64_t token, uint32_t type, const void* bytes = nullptr,
          size_t size = 0, int width = 0, int height = 0) {
    if (type != PB_FRAME) size = std::min(size, static_cast<size_t>(PB_URL_CAP - 1));
    PbEvent event{token, type, static_cast<uint32_t>(size),
                  static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    const void* chunks[] = {&event, bytes};
    size_t sizes[] = {sizeof(event), size};
    for (int i = 0; i < 2; ++i) {
        auto p = static_cast<const char*>(chunks[i]);
        while (sizes[i]) {
            ssize_t n = send(events_fd, p, sizes[i], MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n; sizes[i] -= n;
        }
    }
    return true;
}
void Error(uint64_t token, const std::string& text) {
    Send(token, PB_ERROR, text.data(), text.size());
}

class Page : public CefClient, public CefLifeSpanHandler, public CefRenderHandler,
             public CefDisplayHandler, public CefLoadHandler,
             public CefContextMenuHandler, public CefDialogHandler,
             public CefJSDialogHandler {
public:
    explicit Page(uint64_t id) : token(id) {}
    uint64_t token;
    int width = 800, height = 600;
    bool visible = false, close_requested = false;
    CefRefPtr<CefBrowser> browser;
    std::vector<PbCommand> pending;
    CefRefPtr<CefRequestContext> context;
    std::vector<unsigned char> pixels, popup;
    CefRect popup_rect;
    int popup_width = 0, popup_height = 0;
    bool popup_shown = false;
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, width, height);
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> value) override {
        browser = value;
        Send(token, PB_READY);
        if (stopping || close_requested) { browser->GetHost()->CloseBrowser(true); return; }
        auto commands = std::move(pending);
        for (const auto& command : commands) Command(command);
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        browser = nullptr;
        Send(token, PB_CLOSED);
        pages.erase(token);
        if (stopping && pages.empty()) CefQuitMessageLoop();
    }
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
        const CefString&, const CefString&, CefLifeSpanHandler::WindowOpenDisposition, bool,
        const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
        CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override {
        Error(token, "Popup blocked: tabs are not implemented yet.");
        return true; // Never allow CEF to open an external window.
    }
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
        CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> model) override {
        model->Clear();
    }
    bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, const CefString&,
        const CefString&, const std::vector<CefString>&,
        const std::vector<CefString>&, const std::vector<CefString>&,
        CefRefPtr<CefFileDialogCallback> callback) override {
        callback->Cancel(); Error(token, "File selection is not implemented yet."); return true;
    }
    bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString&, JSDialogType,
        const CefString&, const CefString&, CefRefPtr<CefJSDialogCallback>,
        bool& suppress) override {
        suppress = true; Error(token, "JavaScript dialog suppressed in this browser slice."); return false;
    }
    bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString&, bool,
        CefRefPtr<CefJSDialogCallback> callback) override {
        callback->Continue(true, CefString()); return true;
    }
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                         const CefString& url) override {
        if (frame->IsMain()) { auto s = url.ToString(); Send(token, PB_URL, s.data(), s.size()); }
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
        ErrorCode code, const CefString& text, const CefString&) override {
        if (frame->IsMain() && code != ERR_ABORTED) Error(token, text.ToString());
    }
    void Publish() {
        if (!visible || pixels.empty()) return;
        auto result = pixels;
        if (popup_shown && !popup.empty()) {
            for (int y = 0; y < popup_height; ++y)
                for (int x = 0; x < popup_width; ++x) {
                    int dx = popup_rect.x + x, dy = popup_rect.y + y;
                    if (dx >= 0 && dx < width && dy >= 0 && dy < height)
                        memcpy(&result[(dy * width + dx) * 4],
                               &popup[(y * popup_width + x) * 4], 4);
                }
        }
        Send(token, PB_FRAME, result.data(), result.size(), width, height);
    }
    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override {
        popup_shown = show; if (!show) popup.clear(); Publish();
    }
    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override { popup_rect = rect; }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                 const void* buffer, int w, int h) override {
        if (!visible || w < 1 || h < 1 || w > PB_MAX_DIMENSION || h > PB_MAX_DIMENSION) return;
        auto start = static_cast<const unsigned char*>(buffer);
        if (type == PET_VIEW) {
            // A resize may still have an older paint in flight.
            if (w != width || h != height) return;
            pixels.assign(start, start + static_cast<size_t>(w) * h * 4);
        } else {
            popup.assign(start, start + static_cast<size_t>(w) * h * 4);
            popup_width = w; popup_height = h;
        }
        Publish();
    }
private:
    IMPLEMENT_REFCOUNTING(Page);
};

void Stop() {
    if (stopping) return;
    stopping = true;
    if (pages.empty()) { CefQuitMessageLoop(); return; }
    auto copy = pages;
    for (auto& [id, page] : copy)
        if (page->browser) page->browser->GetHost()->CloseBrowser(true);
}

void Command(PbCommand cmd) {
    if (cmd.type == PB_STOP) { Stop(); return; }
    if (stopping) return;
    if (cmd.type == PB_OPEN && !pages.count(cmd.token)) {
        // Profile names come only from the trusted host, but never accept paths.
        std::string profile(cmd.profile);
        if (profile.empty() || profile.find_first_not_of("0123456789abcdef-") != std::string::npos) {
            Error(cmd.token, "Invalid session profile."); return;
        }
        CefRefPtr<Page> page = new Page(cmd.token);
        CefRequestContextSettings context_settings;
        CefString(&context_settings.cache_path) = profile_root + "/" + profile;
        context_settings.persist_session_cookies = true;
        page->context = CefRequestContext::CreateContext(context_settings, nullptr);
        CefWindowInfo window;
        window.SetAsWindowless(0);
        CefBrowserSettings settings;
        settings.windowless_frame_rate = 1;
        pages[cmd.token] = page;
        bool created = CefBrowserHost::CreateBrowser(window, page, cmd.text,
                                                        settings, nullptr, page->context);
        if (!created) { pages.erase(cmd.token); Error(cmd.token, "CEF could not create a browser."); }
        return;
    }
    auto it = pages.find(cmd.token);
    if (it == pages.end()) return;
    auto page = it->second;
    if (!page->browser) {
        if (cmd.type == PB_CLOSE) { page->close_requested = true; return; }
        if (page->pending.size() < 256) page->pending.push_back(cmd);
        else Error(cmd.token, "Browser initialization command limit reached.");
        return;
    }
    auto browser = page->browser;
    auto host = browser->GetHost();
    switch (cmd.type) {
    case PB_OPEN: browser->GetMainFrame()->LoadURL(cmd.text); break;
    case PB_CLOSE: host->CloseBrowser(true); break;
    case PB_BACK: browser->GoBack(); break;
    case PB_FORWARD: browser->GoForward(); break;
    case PB_RELOAD: browser->Reload(); break;
    case PB_RESIZE:
        page->width = std::clamp<int>(cmd.x, 1, PB_MAX_DIMENSION);
        page->height = std::clamp<int>(cmd.y, 1, PB_MAX_DIMENSION);
        page->pixels.clear(); page->popup.clear();
        host->WasResized(); break;
    case PB_VISIBLE:
        page->visible = cmd.a;
        // Do not WasHidden(true): background sessions must keep making progress.
        host->SetWindowlessFrameRate(cmd.a ? 30 : 1);
        if (cmd.a) host->Invalidate(PET_VIEW);
        else { page->pixels.clear(); page->pixels.shrink_to_fit(); page->popup.clear(); }
        break;
    case PB_FOCUS: host->SetFocus(cmd.a); break;
    case PB_MOUSE: {
        CefMouseEvent e; e.x = cmd.x; e.y = cmd.y; e.modifiers = cmd.modifiers;
        if (cmd.a < 0) host->SendMouseMoveEvent(e, cmd.b);
        else host->SendMouseClickEvent(e, static_cast<cef_mouse_button_type_t>(cmd.a), cmd.b, 1);
        break;
    }
    case PB_WHEEL: {
        CefMouseEvent e; e.x = cmd.x; e.y = cmd.y; e.modifiers = cmd.modifiers;
        host->SendMouseWheelEvent(e, cmd.a, cmd.b); break;
    }
    case PB_KEY: {
        CefKeyEvent e; e.type = cmd.b ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;
        e.windows_key_code = cmd.a; e.native_key_code = cmd.x;
        e.modifiers = cmd.modifiers;
        host->SendKeyEvent(e); break;
    }
    case PB_TEXT: {
        // Text, unlike physical keys, carries Unicode (including surrogate pairs).
        CefString text(cmd.text);
        for (size_t i = 0; i < text.length(); ++i) {
            CefKeyEvent e; e.type = KEYEVENT_CHAR; e.character = text.c_str()[i];
            e.unmodified_character = text.c_str()[i]; e.windows_key_code = text.c_str()[i];
            e.modifiers = cmd.modifiers; host->SendKeyEvent(e);
        }
        break;
    }
    default: break;
    }
}

class App : public CefApp, public CefBrowserProcessHandler {
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine>, const CefString&) override {
        return true; // Never let CEF's default relaunch open a Chrome window.
    }
    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command) override {
        // OSR owns no native child window. Keep the rendering path portable;
        // native Wayland/X11 presentation belongs to Pico's existing window.
        // This embedded Alloy browser has no Chrome tab/reader-mode UI.
        // Chromium 151's reader-mode soft-navigation observer assumes such a
        // tab exists and crashes after SPA link clicks (CEF issue #4234).
        // Disable only that unused feature, retaining any other feature flags.
        if (process_type.empty()) {
            std::string disabled = command->GetSwitchValue("disable-features").ToString();
            if (!disabled.empty()) disabled += ",";
            command->AppendSwitchWithValue("disable-features", disabled + "ImmersiveReadAnything");
        }
        command->AppendSwitchWithValue("ozone-platform", "headless");
        command->AppendSwitch("disable-background-timer-throttling");
        command->AppendSwitch("disable-renderer-backgrounding");
    }
    IMPLEMENT_REFCOUNTING(App);
};
}
int main(int argc, char** argv) {
    CefMainArgs args(argc, argv);
    CefRefPtr<App> app = new App;
    int subprocess = CefExecuteProcess(args, app, nullptr);
    if (subprocess >= 0) return subprocess;
    if (argc != 4) return 2;
    int command_fd = atoi(argv[1]); events_fd = atoi(argv[2]); profile_root = argv[3];
    fcntl(command_fd, F_SETFD, FD_CLOEXEC);
    fcntl(events_fd, F_SETFD, FD_CLOEXEC);
    int owner = open((profile_root + "/.pico-owner.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (owner < 0 || flock(owner, LOCK_EX | LOCK_NB)) {
        Error(0, "Browser storage is in use by another Pico process. Close that process's browser runtime first.");
        if (owner >= 0) close(owner);
        return 1;
    }
    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    CefString(&settings.root_cache_path) = profile_root;
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) return 1;
    executable[length] = 0;
    std::string assets(executable);
    assets = assets.substr(0, assets.find_last_of('/'));
    CefString(&settings.resources_dir_path) = assets;
    CefString(&settings.locales_dir_path) = assets + "/locales";
    CefString(&settings.log_file) = profile_root + "/cef.log";
    // The sandbox is intentionally enabled. Never retry with --no-sandbox.
    if (!CefInitialize(args, settings, app, nullptr)) {
        Error(0, "CEF initialization failed; check browser/cef.log and profile ownership."); return 1;
    }
    std::thread reader([command_fd] {
        PbCommand cmd;
        for (;;) {
            ssize_t n = recv(command_fd, &cmd, sizeof(cmd), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n != sizeof(cmd)) break;
            cmd.text[sizeof(cmd.text)-1] = 0; cmd.profile[sizeof(cmd.profile)-1] = 0;
            if (!CefPostTask(TID_UI, base::BindOnce(Command, cmd))) break;
            if (cmd.type == PB_STOP) return;
        }
        CefPostTask(TID_UI, base::BindOnce(Stop));
    });
    CefRunMessageLoop();
    shutdown(command_fd, SHUT_RDWR);
    reader.join();
    CefShutdown();
    close(owner); close(command_fd); close(events_fd);
    return 0;
}
