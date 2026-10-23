#include "protocol.h"
#include "inspection.h"
#include "include/cef_app.h"
#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_image.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"
#include "include/cef_command_line.h"
#include "include/wrapper/cef_closure_task.h"
#include <algorithm>
#include <cerrno>
#include <csignal>
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
    if (type != PB_FRAME && type != PB_OBSERVATION && type != PB_TEXT_RESULT) size = std::min(size, static_cast<size_t>(PB_URL_CAP - 1));
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
             public CefJSDialogHandler, public CefRequestHandler, public CefDevToolsMessageObserver {
public:
    explicit Page(uint64_t id) : token(id), inspector(id, next_message, [id](const PbTextResult& result) {
        Send(id, PB_TEXT_RESULT, &result, sizeof(result));
    }, [id](const PbTextResult& result) {
        auto found = pages.find(id);
        if (found != pages.end()) found->second->inspector.SnapshotReady(result);
        else if (result.path[0]) unlink(result.path);
    }) {}
    uint64_t token;
    int width = 800, height = 600;
    bool visible = false, close_requested = false, failed = false;
    CefRefPtr<CefBrowser> browser;
    std::vector<PbCommand> pending;
    CefRefPtr<CefRequestContext> context;
    std::vector<unsigned char> pixels, popup;
    CefRect popup_rect;
    int popup_width = 0, popup_height = 0;
    bool popup_shown = false;
    bool agent_control = false, capture_ready = false, priming_capture = false, capturing = false, fresh_view = false, fresh_popup = false;
    uint64_t navigation = 1, viewport = 1, observed_navigation = 0, observed_viewport = 0;
    uint64_t request = 0, action_navigation = 0, action_viewport = 0;
    std::string url, action_error;
    bool interrupted = false, releasing = false, uncertain = false;
    unsigned delivered_events = 0, events_in_action = 0;
    int capture_message = 0;
    uint64_t capture_navigation = 0, capture_viewport = 0;
    std::vector<PbCommand> steps;
    size_t step_index = 0;
    unsigned completed = 0, held_buttons = 0;
    std::map<int, bool> held_keys;
    int mouse_x = 0, mouse_y = 0, next_message = 1, input_message = 0;
    CefRefPtr<CefRegistration> devtools;
    browser_inspection::Inspector inspector;
    int runtime_message = 0;
    bool initialized = false;
    std::string initial_url;
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
        CefRefPtr<CefRequest>, bool, bool, const CefString&, bool&) override {
        return new browser_inspection::ResourceDiagnostics(inspector.log);
    }
    void OnDevToolsEvent(CefRefPtr<CefBrowser> browser, const CefString& method, const void* params, size_t size) override {
        inspector.Event(method, params, size, navigation, browser);
    }
    void Initialize(bool success) {
        if (!success) inspector.log->Fail("Chromium runtime diagnostics could not be enabled.");
        initialized = true; runtime_message = 0;
        Send(token, PB_READY);
        if (stopping || close_requested) { browser->GetHost()->CloseBrowser(true); return; }
        browser->GetMainFrame()->LoadURL(initial_url);
        initial_url.clear();
        auto commands = std::move(pending);
        for (const auto& command : commands) Command(command);
    }
    void Inspect(const PbCommand& command) {
        if (request || inspector.request || (command.type == PB_EVALUATE && !agent_control)) {
            PbTextResult result{}; result.request = command.request; result.error = 1;
            snprintf(result.failure, sizeof(result.failure), "%s", !agent_control && command.type == PB_EVALUATE ?
                     "Browser is under User control." : "Browser is busy.");
            Send(token, PB_TEXT_RESULT, &result, sizeof(result)); return;
        }
        inspector.Begin(command, browser, navigation, viewport, url);
        CefPostDelayedTask(TID_UI, base::BindOnce([](uint64_t token, uint64_t id) {
            auto found = pages.find(token);
            if (found != pages.end() && found->second->inspector.request == id)
                found->second->inspector.Cancel("Browser inspection timed out.");
        }, token, command.request), 30000);
    }
    void Resize(int w, int h) {
        w = std::clamp<int>(w, 1, PB_MAX_DIMENSION); h = std::clamp<int>(h, 1, PB_MAX_DIMENSION);
        if (width == w && height == h) return;
        ++viewport; Changed(); width = w; height = h;
        pixels.clear(); popup.clear(); browser->GetHost()->WasResized();
        if (request && capture_ready && !input_message) Capture();
    }
    void Viewport(const PbCommand& command) {
        PbTextResult result{}; result.request = command.request;
        if (!agent_control) {
            result.error = 1; snprintf(result.failure, sizeof(result.failure), "Browser is under User control.");
        } else {
            Resize(command.x, command.y);
            auto data = CefDictionaryValue::Create();
            data->SetString("mode", command.a ? "fixed" : "dock"); data->SetInt("width", width); data->SetInt("height", height);
            data->SetString("viewport", browser_inspection::Hex(viewport));
            data->SetString("coordinates", "image pixels; 1:1 logical viewport units; dock is scaled down and letterboxed");
            std::string path, text = browser_inspection::Json(data);
            int fd = browser_inspection::Temporary(path, "output");
            bool ok = fd >= 0 && browser_inspection::Write(fd, text.data(), text.size());
            if (fd >= 0 && close(fd)) ok = false;
            if (ok) { snprintf(result.path, sizeof(result.path), "%s", path.c_str()); result.size = text.size(); }
            else { if (!path.empty()) unlink(path.c_str()); result.error = 1; snprintf(result.failure, sizeof(result.failure), "Viewport changed, but could not save the result."); }
        }
        Send(token, PB_TEXT_RESULT, &result, sizeof(result));
    }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    void ReleaseInput() {
        if (browser) {
            auto host = browser->GetHost();
            CefMouseEvent mouse; mouse.x = mouse_x; mouse.y = mouse_y;
            for (int i = 0; i < 3; ++i)
                if (held_buttons & (1u << i)) host->SendMouseClickEvent(mouse, static_cast<cef_mouse_button_type_t>(i), true, 1);
            for (const auto& [key, held] : held_keys) if (held) {
                CefKeyEvent event; event.type = KEYEVENT_KEYUP; event.windows_key_code = key;
                host->SendKeyEvent(event);
            }
        }
        held_buttons = 0; held_keys.clear();
    }
    void Finish(const std::string& error, const std::vector<unsigned char>& png = {}) {
        if (!request) return;
        PbObservation info{};
        info.request = request; info.navigation = navigation; info.viewport = viewport;
        info.completed = completed; info.delivered_events = delivered_events;
        info.partial_action = events_in_action != 0; info.uncertain = uncertain; info.png_size = static_cast<uint32_t>(png.size());
        snprintf(info.url, sizeof(info.url), "%s", url.c_str());
        snprintf(info.error, sizeof(info.error), "%s", error.c_str());
        std::vector<unsigned char> packet(sizeof(info) + png.size());
        memcpy(packet.data(), &info, sizeof(info));
        if (!png.empty()) memcpy(packet.data() + sizeof(info), png.data(), png.size());
        ReleaseInput();
        Send(token, PB_OBSERVATION, packet.data(), packet.size(), width, height);
        request = 0; input_message = capture_message = 0; priming_capture = capturing = capture_ready = false; steps.clear();
        if (!visible) { pixels.clear(); popup.clear(); }
    }
    void Interrupt(const std::string& error) {
        if (!request) return;
        action_error = error; interrupted = true; priming_capture = capturing = capture_ready = false;
        capture_message = 0;
        if (!input_message) Cleanup();
    }
    void Cleanup() {
        steps.clear(); step_index = 0; releasing = true;
        for (int i = 0; i < 3; ++i) if (held_buttons & (1u << i)) {
            PbCommand step{}; step.type = PB_MOUSE; step.a = i; step.b = 1;
            step.x = mouse_x; step.y = mouse_y; steps.push_back(step);
        }
        for (const auto& [key, held] : held_keys) if (held) {
            PbCommand step{}; step.type = PB_KEY; step.a = key; step.b = 1; steps.push_back(step);
        }
        Step();
    }
    void Capture() {
        if (!request || !browser || !capture_ready || browser->IsLoading() || capture_message) return;
        capturing = false; fresh_view = false; fresh_popup = !popup_shown;
        priming_capture = true;
        browser->GetHost()->Invalidate(PET_VIEW);
    }
    void CaptureFence() {
        if (!request || !capture_ready || !priming_capture || browser->IsLoading()) return;
        priming_capture = false;
        capture_navigation = navigation; capture_viewport = viewport;
        /* A compositor snapshot is a render fence, not the returned image.
         * Only afterwards invalidate the OSR surfaces (including popups).
         * This prevents an already queued pre-action paint satisfying capture. */
        auto params = CefDictionaryValue::Create(); params->SetString("format", "png");
        params->SetBool("fromSurface", true);
        capture_message = next_message++;
        if (!browser->GetHost()->ExecuteDevToolsMethod(capture_message, "Page.captureScreenshot", params))
            Finish("Could not synchronize the browser compositor.");
    }
    void Captured() {
        if (!request || !capturing || !fresh_view || !fresh_popup || browser->IsLoading()) return;
        auto bitmap = Composite();
        if (bitmap.empty()) return;
        auto image = CefImage::CreateImage();
        if (!image->AddBitmap(1, width, height, CEF_COLOR_TYPE_BGRA_8888, CEF_ALPHA_TYPE_OPAQUE,
                              bitmap.data(), bitmap.size())) { Finish("Could not encode observation."); return; }
        int w = 0, h = 0;
        auto data = image->GetAsPNG(1, false, w, h);
        if (!data || data->GetSize() > PB_MAX_PNG) { Finish("Observation image exceeds the size limit."); return; }
        std::vector<unsigned char> png(data->GetSize()); data->GetData(png.data(), png.size(), 0);
        observed_navigation = navigation; observed_viewport = viewport;
        Finish(action_error, png);
    }
    void Begin(const PbCommand& command, bool actions) {
        if (request || inspector.request) { // Host serializes by session; reject a protocol violation explicitly.
            PbObservation info{}; info.request = command.request;
            snprintf(info.error, sizeof(info.error), "Browser is busy.");
            Send(token, PB_OBSERVATION, &info, sizeof(info), width, height); return;
        }
        request = command.request; capture_ready = !actions; interrupted = releasing = uncertain = false;
        delivered_events = events_in_action = 0; completed = 0; step_index = 0; steps.clear(); action_error.clear();
        action_navigation = command.navigation; action_viewport = command.viewport;
        if (actions && (!agent_control || navigation != command.navigation || viewport != command.viewport ||
                        observed_navigation != navigation || observed_viewport != viewport || command.x != width || command.y != height)) {
            Finish(!agent_control ? "Browser is under User control." : "Stale observation: observe again before acting."); return;
        }
        CefPostDelayedTask(TID_UI, base::BindOnce([](uint64_t token, uint64_t id) {
            auto found = pages.find(token);
            if (found == pages.end()) return;
            auto self = found->second;
            if (self->request == id) {
                bool unacknowledged = self->input_message != 0;
                bool input_active = unacknowledged || self->held_buttons || self->events_in_action;
                for (const auto& [key, held] : self->held_keys) input_active = input_active || held;
                self->uncertain = unacknowledged;
                self->Finish(input_active ? "Input operation timed out. Browser closed to prevent late input; check delivery uncertainty." :
                                             "Browser observation timed out; no application readiness is implied.");
                if (input_active && self->browser) self->browser->GetHost()->CloseBrowser(true);
            }
        }, token, request), 30000);
        if (!actions) Capture();
    }
    void Changed() {
        fresh_view = fresh_popup = capturing = false;
        inspector.log->navigation.store(navigation);
    }
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefRequest>, bool, bool) override {
        if (frame->IsMain()) { ++navigation; Changed(); }
        return false;
    }
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool, bool) override {
        if (loading) { fresh_view = fresh_popup = false; }
        else if (request && capture_ready && !input_message) Capture();
    }
    void Step() {
        if (!request || input_message || !browser) return;
        if (interrupted && !releasing) { Cleanup(); return; }
        if (releasing && step_index >= steps.size()) { Finish(action_error); return; }
        if (!releasing && !agent_control) { Interrupt("Browser control was taken by the user."); return; }
        if (!releasing && (step_index >= steps.size() || !action_error.empty() ||
            navigation != action_navigation || viewport != action_viewport)) {
            if (step_index < steps.size() && action_error.empty()) action_error = "Stale observation: remaining actions were stopped.";
            bool held = held_buttons != 0;
            for (const auto& [key, down] : held_keys) held = held || down;
            if (held) { interrupted = true; Cleanup(); return; }
            capture_ready = true; Capture(); return;
        }
        const auto& command = steps[step_index];
        auto params = CefDictionaryValue::Create();
        int modifiers = ((command.modifiers & EVENTFLAG_ALT_DOWN) ? 1 : 0) |
                        ((command.modifiers & EVENTFLAG_CONTROL_DOWN) ? 2 : 0) |
                        ((command.modifiers & EVENTFLAG_COMMAND_DOWN) ? 4 : 0) |
                        ((command.modifiers & EVENTFLAG_SHIFT_DOWN) ? 8 : 0);
        params->SetInt("modifiers", modifiers);
        std::string method;
        if (command.type == PB_MOUSE || command.type == PB_WHEEL) {
            mouse_x = command.x; mouse_y = command.y;
            params->SetInt("x", mouse_x); params->SetInt("y", mouse_y);
            if (command.type == PB_WHEEL) {
                params->SetString("type", "mouseWheel");
                params->SetInt("deltaX", command.a); params->SetInt("deltaY", command.b);
            } else {
                params->SetString("type", command.a < 0 ? "mouseMoved" : command.b ? "mouseReleased" : "mousePressed");
                if (command.a >= 0) {
                    params->SetString("button", command.a == 0 ? "left" : command.a == 1 ? "middle" : "right");
                    params->SetInt("clickCount", command.text[0] ? command.text[0] : 1);
                    if (!command.b) held_buttons |= 1u << command.a;
                } else if (held_buttons) params->SetString("button", held_buttons & 1 ? "left" : held_buttons & 2 ? "middle" : "right");
            }
            unsigned sent_buttons = held_buttons;
            if (command.type == PB_MOUSE && command.a >= 0 && command.b) sent_buttons &= ~(1u << command.a);
            int buttons = ((sent_buttons & 1) ? 1 : 0) | ((sent_buttons & 2) ? 4 : 0) | ((sent_buttons & 4) ? 2 : 0);
            params->SetInt("buttons", buttons); method = "Input.dispatchMouseEvent";
        } else if (command.type == PB_KEY) {
            params->SetString("type", command.b ? "keyUp" : "keyDown");
            params->SetInt("windowsVirtualKeyCode", command.a);
            params->SetString("key", command.text);
            params->SetString("code", command.profile);
            if (!command.b && !(command.modifiers & (EVENTFLAG_CONTROL_DOWN | EVENTFLAG_ALT_DOWN))) {
                if (command.a == 13) params->SetString("text", "\r");
                if (command.a == 32) params->SetString("text", " ");
            }
            if (!command.b) held_keys[command.a] = true;
            method = "Input.dispatchKeyEvent";
        } else if (command.type == PB_TEXT) {
            params->SetString("text", command.text); method = "Input.insertText";
        } else { Finish("Invalid native input command."); return; }
        browser->GetHost()->SetFocus(true);
        input_message = next_message++;
        if (!browser->GetHost()->ExecuteDevToolsMethod(input_message, method, params)) {
            input_message = 0;
            if (releasing) { uncertain = true; Finish("Input release dispatch failed; browser closed."); CloseUnresponsive(); }
            else Interrupt("Native browser input dispatch failed.");
        }
    }
    void CloseUnresponsive() { if (browser) browser->GetHost()->CloseBrowser(true); }
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool success, const void* result, size_t result_size) override {
        if (id == runtime_message) { Initialize(success); return; }
        if (inspector.Result(id, success, result, result_size)) return;
        if (!request) return;
        if (id == capture_message) {
            capture_message = 0;
            if (!success) { Finish("Browser compositor synchronization failed: " + std::string(static_cast<const char*>(result), result_size)); return; }
            if (navigation != capture_navigation || viewport != capture_viewport || browser->IsLoading()) { Capture(); return; }
            pixels.clear(); capturing = true; fresh_view = false; fresh_popup = !popup_shown;
            if (popup_shown) { popup.clear(); browser->GetHost()->Invalidate(PET_POPUP); }
            browser->GetHost()->Invalidate(PET_VIEW); return;
        }
        if (id != input_message) return;
        input_message = 0;
        if (!success) {
            uncertain = true;
            if (releasing) { Finish("Input release failed; delivery uncertain. Browser closed."); CloseUnresponsive(); }
            else Interrupt("Native browser input failed; delivery uncertain, do not retry blindly.");
            return;
        }
        const auto& acknowledged = steps[step_index];
        if (acknowledged.type == PB_MOUSE && acknowledged.a >= 0 && acknowledged.b) held_buttons &= ~(1u << acknowledged.a);
        if (acknowledged.type == PB_KEY && acknowledged.b) held_keys[acknowledged.a] = false;
        if (!releasing) {
            ++delivered_events; ++events_in_action;
            if (steps[step_index].end_action) { ++completed; events_in_action = 0; }
        }
        ++step_index;
        CefRefPtr<Page> self(this);
        CefPostTask(TID_UI, base::BindOnce(&Page::Step, self));
    }
    void OnDevToolsAgentDetached(CefRefPtr<CefBrowser>) override {
        if (failed || close_requested || stopping) return;
        /* Termination often detaches DevTools first. Allow its precise cause to
         * win before completing pending work; bound the non-termination case. */
        CefRefPtr<Page> self(this);
        CefPostDelayedTask(TID_UI, base::BindOnce([](CefRefPtr<Page> page) {
            if (page->failed || page->close_requested || !page->browser) return;
            page->inspector.Fail("Browser renderer disconnected; evaluation effects may be uncertain.");
            page->inspector.log->Fail("Browser diagnostic channel disconnected; collection is incomplete.");
            if (page->request) { page->uncertain = page->input_message != 0; page->Finish("Browser renderer disconnected; any in-flight input delivery is uncertain."); }
        }, self), 100);
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus status,
                                   int error_code, const CefString& error_string) override {
        if (failed || close_requested || stopping) return;
        failed = true;
        const char* cause = "abnormal termination";
        switch (status) {
        case TS_PROCESS_WAS_KILLED: cause = "killed"; break;
        case TS_PROCESS_CRASHED: cause = "crashed"; break;
        case TS_PROCESS_OOM: cause = "out of memory"; break;
        case TS_LAUNCH_FAILED: cause = "launch failed"; break;
        case TS_INTEGRITY_FAILURE: cause = "integrity failure"; break;
        default: break;
        }
        std::string error = "Browser renderer " + std::string(cause) + " (code " + std::to_string(error_code) + ").";
        if (!error_string.empty()) error += " " + error_string.ToString();
        /* Publish failure before results/close. The host retains a stopped dock
         * instead of mistaking a crash for an explicit human close. */
        Send(token, PB_STOPPED, error.data(), error.size());
        inspector.Fail(error + " Evaluation effects may be uncertain; close and reopen the browser.");
        inspector.log->Fail(error + " Diagnostic collection is incomplete.");
        if (request) { uncertain = input_message != 0; Finish(error + " In-flight input may be uncertain; close and reopen the browser."); }
        close_requested = true;
        if (browser) browser->GetHost()->CloseBrowser(true);
    }
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
        devtools = browser->GetHost()->AddDevToolsMessageObserver(this);
        if (stopping || close_requested) { browser->GetHost()->CloseBrowser(true); return; }
        runtime_message = next_message++;
        if (!browser->GetHost()->ExecuteDevToolsMethod(runtime_message, "Runtime.enable", nullptr)) Initialize(false);
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        inspector.Fail("Browser closed; evaluation effects may be uncertain.");
        uncertain = input_message != 0; Finish("Browser closed; in-flight input may be uncertain."); devtools = nullptr;
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
        if (frame->IsMain()) {
            this->url = url.ToString(); ++navigation; Changed();
            Send(token, PB_URL, this->url.data(), this->url.size());
            if (request && capture_ready) {
                CefRefPtr<Page> self(this); CefPostTask(TID_UI, base::BindOnce(&Page::Capture, self));
            }
        }
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
        ErrorCode code, const CefString& text, const CefString&) override {
        if (frame->IsMain() && code != ERR_ABORTED) {
            Error(token, text.ToString());
            if (request && !steps.empty()) Interrupt(text.ToString());
            else Finish(text.ToString());
        }
    }
    std::vector<unsigned char> Composite() {
        if (pixels.empty()) return {};
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
        return result;
    }
    void Publish() {
        if (!visible || pixels.empty()) return;
        auto result = Composite();
        Send(token, PB_FRAME, result.data(), result.size(), width, height);
    }
    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override {
        popup_shown = show; if (!show) { popup.clear(); fresh_popup = true; } else fresh_popup = false; Publish(); Captured();
    }
    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override { popup_rect = rect; }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                 const void* buffer, int w, int h) override {
        if ((!visible && !request) || w < 1 || h < 1 || w > PB_MAX_DIMENSION || h > PB_MAX_DIMENSION) return;
        auto start = static_cast<const unsigned char*>(buffer);
        if (type == PET_VIEW) {
            // A resize may still have an older paint in flight.
            if (w != width || h != height) return;
            pixels.assign(start, start + static_cast<size_t>(w) * h * 4);
            if (capturing) fresh_view = true;
        } else {
            popup.assign(start, start + static_cast<size_t>(w) * h * 4);
            popup_width = w; popup_height = h;
            if (capturing) fresh_popup = true;
        }
        if (type == PET_VIEW && priming_capture) CaptureFence();
        Publish(); Captured();
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
        page->initial_url = cmd.text;
        CefRequestContextSettings context_settings;
        CefString(&context_settings.cache_path) = profile_root + "/" + profile;
        context_settings.persist_session_cookies = true;
        page->context = CefRequestContext::CreateContext(context_settings, nullptr);
        CefWindowInfo window;
        window.SetAsWindowless(0);
        CefBrowserSettings settings;
        settings.windowless_frame_rate = 1;
        pages[cmd.token] = page;
        bool created = CefBrowserHost::CreateBrowser(window, page, "about:blank",
                                                        settings, nullptr, page->context);
        if (!created) { pages.erase(cmd.token); const std::string error = "CEF could not create a browser."; Send(cmd.token, PB_STOPPED, error.data(), error.size()); }
        return;
    }
    auto it = pages.find(cmd.token);
    if (it == pages.end()) return;
    auto page = it->second;
    if (page->failed) return;
    if (cmd.type == PB_CLOSE) {
        page->close_requested = true;
        page->pending.clear();
        if (page->browser) page->browser->GetHost()->CloseBrowser(true);
        return;
    }
    if (!page->browser || !page->initialized) {
        if (page->pending.size() < 256) page->pending.push_back(cmd);
        else Error(cmd.token, "Browser initialization command limit reached.");
        return;
    }
    auto browser = page->browser;
    auto host = browser->GetHost();
    if (cmd.request && (cmd.type == PB_MOUSE || cmd.type == PB_WHEEL || cmd.type == PB_KEY || cmd.type == PB_TEXT)) {
        if (page->request == cmd.request) {
            if (page->steps.size() >= PB_MAX_STEPS) page->Finish("Action batch exceeds native input limit.");
            else page->steps.push_back(cmd);
        }
        return;
    }
    switch (cmd.type) {
    case PB_DIAGNOSTICS: case PB_INSPECT: case PB_EVALUATE: page->Inspect(cmd); break;
    case PB_VIEWPORT: page->Viewport(cmd); break;
    case PB_OBSERVE: page->Begin(cmd, false); break;
    case PB_BEGIN: page->Begin(cmd, true); break;
    case PB_COMMIT:
        if (page->request == cmd.request) {
            bool valid = !page->steps.empty();
            for (const auto& step : page->steps)
                if ((step.type == PB_MOUSE || step.type == PB_WHEEL) &&
                    (step.x < 0 || step.y < 0 || step.x >= page->width || step.y >= page->height)) valid = false;
            if (valid) page->Step(); else page->Finish("Invalid input coordinates.");
        }
        break;
    case PB_CANCEL:
        if (page->inspector.request == cmd.request) page->inspector.Cancel("Browser operation cancelled.");
        if (page->request == cmd.request) page->Interrupt("Browser operation cancelled; completed input was not undone.");
        break;
    case PB_CONTROL:
        page->agent_control = cmd.a;
        if (!cmd.a && page->inspector.Evaluating()) page->inspector.Cancel("Browser control was taken by the user.");
        if (!cmd.a) { if (!page->steps.empty()) page->Interrupt("Browser control was taken by the user."); if (!page->request) { page->ReleaseInput(); host->SetFocus(false); } }
        break;
    case PB_OPEN: ++page->navigation; page->Changed(); browser->GetMainFrame()->LoadURL(cmd.text); break;
    case PB_CLOSE: host->CloseBrowser(true); break;
    case PB_BACK: browser->GoBack(); break;
    case PB_FORWARD: browser->GoForward(); break;
    case PB_RELOAD: browser->Reload(); break;
    case PB_RESIZE:
        page->Resize(cmd.x, cmd.y);
        break;
    case PB_VISIBLE:
        page->visible = cmd.a;
        // Do not WasHidden(true): background sessions must keep making progress.
        host->SetWindowlessFrameRate(cmd.a ? 30 : 1);
        if (cmd.a) host->Invalidate(PET_VIEW);
        else if (!page->request) { page->pixels.clear(); page->pixels.shrink_to_fit(); page->popup.clear(); }
        break;
    case PB_FOCUS: if (page->steps.empty()) host->SetFocus(cmd.a); break;
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
    // File-size quotas must produce explicit output/collection errors, not
    // terminate every session's browser helper with SIGXFSZ.
    signal(SIGXFSZ, SIG_IGN);
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
    Send(0, PB_RUNTIME_READY);
    std::thread reader([command_fd] {
        PbCommand cmd;
        for (;;) {
            ssize_t n = recv(command_fd, &cmd, sizeof(cmd), MSG_TRUNC);
            if (n < 0 && errno == EINTR) continue;
            if (n != sizeof(cmd) || cmd.type < PB_OPEN || cmd.type > PB_VIEWPORT ||
                !memchr(cmd.text, 0, sizeof(cmd.text)) || !memchr(cmd.profile, 0, sizeof(cmd.profile))) {
                if (n) {
                    std::string error = n < 0 ? "Browser command transport failed: " + std::string(strerror(errno)) :
                                               "Browser command protocol error: malformed command.";
                    CefPostTask(TID_UI, base::BindOnce([](std::string message) { Error(0, message); Stop(); }, error));
                    return;
                }
                break;
            }
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
