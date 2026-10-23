#ifndef PICO_BROWSER_INSPECTION_H
#define PICO_BROWSER_INSPECTION_H
#include "protocol.h"
#include "include/cef_browser.h"
#include "include/cef_parser.h"
#include "include/cef_resource_request_handler.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace browser_inspection {
inline std::string Hex(uint64_t value) {
    char text[32]; snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value)); return text;
}
inline std::string Json(CefRefPtr<CefDictionaryValue> dict) {
    auto value = CefValue::Create(); value->SetDictionary(dict); return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}
inline CefRefPtr<CefDictionaryValue> Parse(const void* data, size_t size) {
    auto value = CefParseJSON(data, size, JSON_PARSER_RFC);
    return value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
}
inline bool Write(int fd, const void* bytes, size_t size) {
    auto data = static_cast<const char*>(bytes);
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n; size -= n;
    }
    return true;
}
inline int Temporary(std::string& path, const char* stem) {
    path = std::string("/tmp/pico-browser-") + stem + "-XXXXXX";
    int fd = mkstemp(path.data());
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    else path.clear();
    return fd;
}

/* No ring buffer and no per-record text truncation. Resource callbacks append
 * on CEF's IO thread; DevTools callbacks append on its UI thread. Only this
 * journal and its error state are shared. No Page/UI pointers cross threads. */
class DiagnosticLog {
public:
    explicit DiagnosticLog(uint64_t token) : token_(token) {
        fd_ = Temporary(path_, "diagnostics");
        if (fd_ < 0) error_ = "Could not create diagnostic journal.";
    }
    ~DiagnosticLog() { if (fd_ >= 0) close(fd_); }
    std::atomic<uint64_t> navigation{1};
    void Fail(const std::string& error) { std::lock_guard lock(mu_); if (error_.empty()) error_ = error; }
    void Append(const char* kind, uint64_t nav, CefRefPtr<CefDictionaryValue> data) {
        auto record = CefDictionaryValue::Create();
        record->SetString("kind", kind); record->SetString("browser_id", Hex(token_));
        record->SetString("navigation", Hex(nav));
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        record->SetString("time_unix_ms", std::to_string(ms)); record->SetDictionary("data", data);
        std::string text = Json(record) + "\n";
        std::lock_guard lock(mu_);
        if (fd_ < 0 || !error_.empty()) return; // Subsequent reads explicitly report the collection failure.
        if (!Write(fd_, text.data(), text.size())) {
            // Do not leave a partial JSON record looking like a complete one.
            if (ftruncate(fd_, size_) != 0) error_ = "Diagnostic storage failed; journal contains an incomplete trailing record.";
            lseek(fd_, size_, SEEK_SET);
            if (error_.empty()) error_ = "Diagnostic storage failed; collection is incomplete.";
        } else size_ += text.size();
    }
    bool Snapshot(int output, const std::string& cursor, std::string& failure, bool& incomplete, const std::atomic<bool>& cancelled) {
        std::unique_lock lock(mu_);
        const uint64_t end_offset = size_;
        uint64_t offset = 0;
        if (!cursor.empty()) {
            std::string prefix = Hex(token_) + ":";
            if (cursor.compare(0, prefix.size(), prefix) != 0) { failure = "Diagnostic cursor belongs to a different browser."; return false; }
            std::string number = cursor.substr(prefix.size()); char* end = nullptr;
            errno = 0; offset = strtoull(number.c_str(), &end, 10);
            if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos || errno || *end || offset > size_) {
                failure = "Invalid diagnostic cursor."; return false;
            }
            char boundary = 0;
            if (offset && (pread(fd_, &boundary, 1, offset - 1) != 1 || boundary != '\n')) {
                failure = "Diagnostic cursor is not at a record boundary."; return false;
            }
        }
        incomplete = !error_.empty();
        auto meta = CefDictionaryValue::Create(); meta->SetBool("untrusted", true);
        meta->SetString("kind", "diagnostics"); meta->SetString("browser_id", Hex(token_));
        meta->SetString("cursor", Hex(token_) + ":" + std::to_string(size_));
        meta->SetString("source_path", path_); meta->SetBool("complete", !incomplete);
        if (incomplete) meta->SetString("collection_error", error_);
        std::string header = Json(meta) + "\n";
        lock.unlock(); // The immutable prefix stays readable while new records append.
        if (!Write(output, header.data(), header.size())) { failure = "Could not write diagnostic snapshot."; return false; }
        char buffer[16384];
        while (offset < end_offset) {
            if (cancelled.load()) { failure = "Diagnostic snapshot cancelled."; return false; }
            ssize_t n = pread(fd_, buffer, std::min<uint64_t>(sizeof(buffer), end_offset - offset), offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || !Write(output, buffer, n)) { failure = "Could not copy complete diagnostic snapshot."; return false; }
            offset += n;
        }
        return true;
    }
private:
    uint64_t token_, size_ = 0;
    int fd_ = -1;
    std::string path_, error_;
    std::mutex mu_;
};

/* CEF supplies request metadata directly, avoiding an unbounded map of active
 * DevTools request IDs. No headers, cookies, or bodies are collected. */
class ResourceDiagnostics : public CefResourceRequestHandler {
public:
    explicit ResourceDiagnostics(std::shared_ptr<DiagnosticLog> log) : log_(std::move(log)), navigation_(log_->navigation.load()) {}
    void OnResourceLoadComplete(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
                               CefRefPtr<CefResponse> response, URLRequestStatus status, int64_t received) override {
        int code = response ? response->GetStatus() : 0;
        if (status == UR_SUCCESS && code < 400) return;
        auto data = CefDictionaryValue::Create();
        data->SetString("url", request->GetURL()); data->SetString("method", request->GetMethod());
        data->SetInt("status", code); data->SetInt("net_error", response ? response->GetError() : 0);
        data->SetString("status_text", response ? response->GetStatusText() : CefString());
        data->SetString("outcome", status == UR_CANCELED ? "cancelled" : status == UR_SUCCESS ? "http_error" : "failed");
        data->SetString("received_bytes", std::to_string(received));
        log_->Append("network_failure", navigation_, data);
    }
private:
    std::shared_ptr<DiagnosticLog> log_;
    uint64_t navigation_;
    IMPLEMENT_REFCOUNTING(ResourceDiagnostics);
};

class Inspector {
public:
    Inspector(uint64_t token, int& next_message, std::function<void(const PbTextResult&)> reply,
              std::function<void(const PbTextResult&)> async_ready)
        : log(std::make_shared<DiagnosticLog>(token)), token_(token), next_message_(next_message), reply_(std::move(reply)), async_ready_(std::move(async_ready)) {}
    ~Inspector() { if (snapshot_cancelled_) snapshot_cancelled_->store(true); }
    void SnapshotReady(const PbTextResult& result) {
        if (request != result.request) { if (result.path[0]) unlink(result.path); return; }
        request = 0; browser_ = nullptr; meta_ = nullptr; snapshot_cancelled_.reset(); reply_(result);
    }
    std::shared_ptr<DiagnosticLog> log;
    uint64_t request = 0;
    bool Evaluating() const { return request && kind_ == PB_EVALUATE; }
    void Event(const CefString& method, const void* data, size_t size, uint64_t navigation, CefRefPtr<CefBrowser> browser) {
        if (method != "Runtime.consoleAPICalled" && method != "Runtime.exceptionThrown") return;
        auto value = Parse(data, size);
        if (!value) { log->Fail("Could not decode a Chromium diagnostic event."); return; }
        log->Append(method == "Runtime.consoleAPICalled" ? "console" : "exception", navigation, value->Copy(false));
        // Remote handles are not a snapshot API, and must not retain arbitrary
        // page objects merely because diagnostics are being recorded.
        auto release = [&](CefRefPtr<CefDictionaryValue> object) {
            if (!object || !object->HasKey("objectId")) return;
            auto params = CefDictionaryValue::Create(); params->SetString("objectId", object->GetString("objectId"));
            browser->GetHost()->ExecuteDevToolsMethod(next_message_++, "Runtime.releaseObject", params);
        };
        auto args = value->GetList("args");
        if (args) for (size_t i = 0; i < args->GetSize(); ++i) release(args->GetDictionary(i));
        auto exception = value->GetDictionary("exceptionDetails");
        if (exception) release(exception->GetDictionary("exception"));
    }
    void Begin(const PbCommand& cmd, CefRefPtr<CefBrowser> browser, uint64_t navigation, uint64_t viewport, const std::string& url) {
        request = cmd.request; kind_ = cmd.type; browser_ = browser; message_ = 0;
        meta_ = CefDictionaryValue::Create(); meta_->SetBool("untrusted", true);
        meta_->SetString("browser_id", Hex(token_)); meta_->SetString("navigation", Hex(navigation));
        meta_->SetString("viewport", Hex(viewport)); meta_->SetString("url", url);
        int fd = open(cmd.text, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 2 || st.st_size > 65536) {
            if (fd >= 0) close(fd);
            Fail("Cannot read browser tool arguments."); return;
        }
        std::string text(st.st_size, '\0'); size_t offset = 0;
        while (offset < text.size()) {
            ssize_t n = read(fd, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            offset += n;
        }
        close(fd);
        auto args = offset == text.size() ? Parse(text.data(), text.size()) : nullptr;
        if (!args) { Fail("Invalid browser tool arguments."); return; }
        if (kind_ == PB_DIAGNOSTICS) {
            snapshot_cancelled_ = std::make_shared<std::atomic<bool>>(false);
            auto cancelled = snapshot_cancelled_; auto journal = log;
            auto ready = async_ready_; uint64_t id = request;
            std::string cursor = args->GetString("cursor");
            bool posted = CefPostTask(TID_FILE_USER_VISIBLE, base::BindOnce([](std::shared_ptr<DiagnosticLog> journal, std::shared_ptr<std::atomic<bool>> cancelled, std::function<void(const PbTextResult&)> ready, uint64_t id, std::string cursor) {
                PbTextResult result{}; result.request = id;
                std::string path, failure; bool incomplete = false;
                int output = Temporary(path, "output");
                bool ok = output >= 0 && journal->Snapshot(output, cursor, failure, incomplete, *cancelled);
                struct stat st{};
                if (output >= 0) { if (fstat(output, &st)) ok = false; if (close(output)) ok = false; }
                if (cancelled->load()) { if (!path.empty()) unlink(path.c_str()); return; }
                if (ok) { result.error = incomplete; result.size = st.st_size; snprintf(result.path, sizeof(result.path), "%s", path.c_str()); }
                else {
                    if (!path.empty()) unlink(path.c_str());
                    result.error = 1;
                    snprintf(result.failure, sizeof(result.failure), "%s", failure.empty() ? "Cannot save diagnostic snapshot." : failure.c_str());
                }
                if (!CefPostTask(TID_UI, base::BindOnce([](std::function<void(const PbTextResult&)> ready, PbTextResult result, std::shared_ptr<std::atomic<bool>> cancelled) {
                    if (cancelled->load()) { if (result.path[0]) unlink(result.path); }
                    else ready(result);
                }, ready, result, cancelled)) && result.path[0]) unlink(result.path);
            }, journal, cancelled, ready, id, cursor));
            if (!posted) Fail("Could not start diagnostic snapshot worker.");
            return;
        }
        if (kind_ == PB_INSPECT) {
            if (args->GetString("mode") == "accessibility") {
                meta_->SetString("kind", "accessibility"); stage_ = 0;
                Call("Accessibility.getFullAXTree", CefDictionaryValue::Create());
            } else {
                meta_->SetString("kind", "dom"); selector_ = args->GetString("selector");
                meta_->SetString("selector", selector_); stage_ = 1;
                auto params = CefDictionaryValue::Create(); params->SetInt("depth", 0);
                Call("DOM.getDocument", params);
            }
        } else {
            meta_->SetString("kind", "evaluation"); stage_ = 0;
            auto params = CefDictionaryValue::Create(); params->SetString("expression", args->GetString("expression"));
            params->SetBool("returnByValue", true); params->SetBool("awaitPromise", true);
            params->SetBool("userGesture", false); params->SetBool("includeCommandLineAPI", false);
            params->SetString("objectGroup", "pico-evaluate"); params->SetDouble("timeout", 10000);
            Call("Runtime.evaluate", params);
        }
    }
    bool Result(int id, bool success, const void* data, size_t size) {
        if (!request || id != message_) return false;
        message_ = 0;
        auto value = Parse(data, size);
        if (success && kind_ == PB_INSPECT && stage_) {
            if (!value) { Fail("Could not decode DOM inspection response."); return true; }
            auto params = CefDictionaryValue::Create();
            if (stage_ == 1) {
                auto root = value->GetDictionary("root");
                if (!root) { Fail("Document is unavailable."); return true; }
                params->SetInt("nodeId", root->GetInt("nodeId")); params->SetString("selector", selector_);
                stage_ = 2; Call("DOM.querySelector", params); return true;
            }
            int node = value->GetInt("nodeId");
            if (!node) { Fail("DOM selector matched no element in the main document."); return true; }
            params->SetInt("nodeId", node); stage_ = 0; Call("DOM.getOuterHTML", params); return true;
        }
        bool error = !success || (kind_ == PB_EVALUATE && value && value->HasKey("exceptionDetails"));
        if (kind_ == PB_EVALUATE) {
            auto params = CefDictionaryValue::Create(); params->SetString("objectGroup", "pico-evaluate");
            browser_->GetHost()->ExecuteDevToolsMethod(next_message_++, "Runtime.releaseObjectGroup", params);
        }
        meta_->SetBool("success", !error);
        std::string prefix = Json(meta_); prefix.pop_back(); prefix += ",\"data\":";
        std::string path; int output = Temporary(path, "output");
        bool ok = output >= 0 && Write(output, prefix.data(), prefix.size()) &&
            Write(output, size ? data : "null", size ? size : 4) && Write(output, "}\n", 2);
        CompleteFile(output, path, ok, error, "Could not save complete inspection result.");
        return true;
    }
    void Cancel(const std::string& reason) {
        if (!request) return;
        bool evaluating = Evaluating(); auto browser = browser_;
        Fail(reason + (evaluating ? " Evaluation effects were not undone; page closed to prevent late execution." : ""));
        if (evaluating && browser) browser->GetHost()->CloseBrowser(true);
    }
    void Fail(const std::string& reason) {
        if (!request) return;
        PbTextResult result{}; result.request = request; result.error = 1;
        snprintf(result.failure, sizeof(result.failure), "%s", reason.c_str());
        if (snapshot_cancelled_) { snapshot_cancelled_->store(true); snapshot_cancelled_.reset(); }
        request = 0; message_ = 0; browser_ = nullptr; meta_ = nullptr; reply_(result);
    }
private:
    void Call(const char* method, CefRefPtr<CefDictionaryValue> params) {
        message_ = next_message_++;
        if (!browser_->GetHost()->ExecuteDevToolsMethod(message_, method, params)) Fail("Could not dispatch browser inspection.");
    }
    void CompleteFile(int fd, const std::string& path, bool ok, bool error, const std::string& failure) {
        PbTextResult result{}; result.request = request; result.error = error;
        struct stat st{};
        if (fd >= 0) { if (fstat(fd, &st)) ok = false; if (close(fd)) ok = false; }
        if (!ok) { if (!path.empty()) unlink(path.c_str()); Fail(failure); return; }
        result.size = st.st_size; snprintf(result.path, sizeof(result.path), "%s", path.c_str());
        request = 0; message_ = 0; browser_ = nullptr; meta_ = nullptr; reply_(result);
    }
    uint64_t token_;
    int& next_message_;
    std::function<void(const PbTextResult&)> reply_, async_ready_;
    std::shared_ptr<std::atomic<bool>> snapshot_cancelled_;
    uint32_t kind_ = 0;
    int message_ = 0, stage_ = 0;
    std::string selector_;
    CefRefPtr<CefBrowser> browser_;
    CefRefPtr<CefDictionaryValue> meta_;
};
}
#endif
