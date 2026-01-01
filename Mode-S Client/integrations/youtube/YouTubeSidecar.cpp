#include "YouTubeSidecar.h"
#include <vector>

YouTubeSidecar::~YouTubeSidecar() { stop(); }

bool YouTubeSidecar::start(const std::wstring& pythonExe,
    const std::wstring& scriptPath,
    EventHandler onEvent)
{
    // Backward-compatible overload: no explicit config path.
    return start(pythonExe, scriptPath, L"", std::move(onEvent));
}

bool YouTubeSidecar::start(const std::wstring& pythonExe,
    const std::wstring& scriptPath,
    const std::wstring& configPath,
    EventHandler onEvent)
{
    stop();
    onEvent_ = std::move(onEvent);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hStdOutRd_, &hStdOutWr_, &sa, 0)) return false;
    SetHandleInformation(hStdOutRd_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hStdOutWr_;
    si.hStdError = hStdOutWr_;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = L"\"" + pythonExe + L"\" \"" + scriptPath + L"\"";
    if (!configPath.empty()) {
        cmd += L" \"" + configPath + L"\"";
    }
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    ZeroMemory(&pi_, sizeof(pi_));
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi_))
    {
        // IMPORTANT: GetLastError() is only meaningful immediately after CreateProcessW fails.
        // Do not call it on the success path (it may be 0 or stale).
        const DWORD err = GetLastError();
        wchar_t buf[1024];
        swprintf_s(
            buf,
            L"CreateProcessW failed. GetLastError=%lu\n  pythonExe=%s\n  scriptPath=%s\n  cmd=%s\n",
            err,
            pythonExe.c_str(),
            scriptPath.c_str(),
            cmd.c_str());
        OutputDebugStringW(buf);

        CloseHandle(hStdOutRd_); hStdOutRd_ = nullptr;
        CloseHandle(hStdOutWr_); hStdOutWr_ = nullptr;
        return false;
    }

    // Success: do not call GetLastError() here.

    CloseHandle(hStdOutWr_);
    hStdOutWr_ = nullptr;

    running_ = true;
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

void YouTubeSidecar::stop()
{
    if (!running_) return;
    running_ = false;

    if (pi_.hProcess) {
        TerminateProcess(pi_.hProcess, 0);
        CloseHandle(pi_.hProcess);
        pi_.hProcess = nullptr;
    }
    if (pi_.hThread) {
        CloseHandle(pi_.hThread);
        pi_.hThread = nullptr;
    }
    if (hStdOutRd_) { CloseHandle(hStdOutRd_); hStdOutRd_ = nullptr; }
    if (reader_.joinable()) reader_.join();
}

void YouTubeSidecar::reader_loop()
{
    std::string buffer;
    buffer.reserve(8192);

    char tmp[4096];
    DWORD read = 0;

    while (running_) {
        if (!ReadFile(hStdOutRd_, tmp, sizeof(tmp), &read, nullptr) || read == 0) {
            Sleep(10);
            continue;
        }
        buffer.append(tmp, tmp + read);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            try {
                auto j = nlohmann::json::parse(line);
                if (onEvent_) onEvent_(j);
            }
            catch (...) {
                // ignore bad lines
            }
        }
    }
}
