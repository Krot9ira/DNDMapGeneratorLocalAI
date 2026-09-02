#include "process_runner.h"

#include <windows.h>
#include <vector>

namespace dnd {

bool ProcessRunner::RunCapture(
    const std::string& cmd,
    const std::string& workingDir,
    std::function<void(const std::string&)> onLine,
    std::atomic<bool>* cancelFlag)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    std::wstring wcmd(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd.data(), n);

    std::wstring wwd;
    if (!workingDir.empty()) {
        int nwd = MultiByteToWideChar(CP_UTF8, 0, workingDir.c_str(), -1, nullptr, 0);
        wwd.resize(nwd);
        MultiByteToWideChar(CP_UTF8, 0, workingDir.c_str(), -1, wwd.data(), nwd);
    }

    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, wwd.empty() ? nullptr : wwd.c_str(),
                        &si, &pi)) {
        CloseHandle(hWrite);
        CloseHandle(hRead);
        return false;
    }
    CloseHandle(hWrite);

    char buf[512];
    std::string lineAcc;
    bool cancelled = false;

    for (;;) {
        if (cancelFlag && cancelFlag->load()) {
            TerminateProcess(pi.hProcess, 1);
            cancelled = true;
            break;
        }

        DWORD avail = 0;
        if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr)) break;

        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 50) != WAIT_OBJECT_0) continue;
            if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        }

        DWORD toRead = avail < (DWORD)(sizeof(buf) - 1) ? avail : (DWORD)(sizeof(buf) - 1);
        DWORD bytesRead = 0;
        if (!ReadFile(hRead, buf, toRead, &bytesRead, nullptr) || bytesRead == 0) break;

        for (DWORD i = 0; i < bytesRead; ++i) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                if (onLine) onLine(lineAcc);
                lineAcc.clear();
            } else {
                lineAcc.push_back(c);
            }
        }
    }
    if (!lineAcc.empty() && onLine) onLine(lineAcc);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return !cancelled && exitCode == 0;
}

bool ProcessRunner::RunDetached(
    const std::string& cmd,
    const std::string& workingDir)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    std::wstring wcmd(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd.data(), n);

    std::wstring wwd;
    if (!workingDir.empty()) {
        int nwd = MultiByteToWideChar(CP_UTF8, 0, workingDir.c_str(), -1, nullptr, 0);
        wwd.resize(nwd);
        MultiByteToWideChar(CP_UTF8, 0, workingDir.c_str(), -1, wwd.data(), nwd);
    }

    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, wwd.empty() ? nullptr : wwd.c_str(),
                        &si, &pi)) {
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

} // namespace dnd
