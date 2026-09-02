#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace dnd {

class ProcessRunner {
public:
    static bool RunCapture(
        const std::string& cmd,
        const std::string& workingDir,
        std::function<void(const std::string&)> onLine,
        std::atomic<bool>* cancelFlag = nullptr);

    static bool RunDetached(
        const std::string& cmd,
        const std::string& workingDir = "");
};

inline bool RunProcessCapture(
    const std::string& cmd,
    const std::string& workingDir,
    std::function<void(const std::string&)> onLine,
    std::atomic<bool>* cancelFlag = nullptr) {
    return ProcessRunner::RunCapture(cmd, workingDir, onLine, cancelFlag);
}

inline bool RunProcessDetached(
    const std::string& cmd,
    const std::string& workingDir = "") {
    return ProcessRunner::RunDetached(cmd, workingDir);
}

} // namespace dnd
