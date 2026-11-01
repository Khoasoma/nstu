#include "nstu/exam_app_suppression.hpp"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace nstu::client {

std::vector<ProcessSnapshotEntry> default_process_enumerator() {
    std::vector<ProcessSnapshotEntry> entries;
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return entries;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &pe)) {
        do {
            DWORD session_id = 0;
            if (ProcessIdToSessionId(pe.th32ProcessID, &session_id)) {
                int size_needed = WideCharToMultiByte(
                    CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
                std::string exe_name;
                if (size_needed > 1) {
                    exe_name.resize(static_cast<std::size_t>(size_needed - 1));
                    WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                                        exe_name.data(), size_needed, nullptr, nullptr);
                }
                if (!exe_name.empty()) {
                    entries.push_back(ProcessSnapshotEntry{
                        .pid = static_cast<std::uint32_t>(pe.th32ProcessID),
                        .session_id = static_cast<std::uint32_t>(session_id),
                        .image_name = std::move(exe_name),
                    });
                }
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
#endif
    return entries;
}

bool default_process_terminator(std::uint32_t pid,
                                std::string_view /*image_name*/) {
#ifdef _WIN32
    if (pid <= 4) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    BOOL success = TerminateProcess(process, 1);
    CloseHandle(process);
    return success != FALSE;
#else
    (void)pid;
    return false;
#endif
}

ExamAppSuppressionExecutor::ExamAppSuppressionExecutor(
    ProcessEnumeratorFn enumerator,
    ProcessTerminatorFn terminator,
    SuppressionAuditFn audit_cb)
    : enumerator_(enumerator ? std::move(enumerator) : default_process_enumerator),
      terminator_(terminator ? std::move(terminator) : default_process_terminator),
      audit_cb_(std::move(audit_cb)) {}

void ExamAppSuppressionExecutor::arm(
    std::vector<std::string> allowed_images) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    classifier_.set_allowed_images(std::move(allowed_images));
    classifier_.arm();
}

void ExamAppSuppressionExecutor::disarm() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    classifier_.disarm();
}

bool ExamAppSuppressionExecutor::active() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return classifier_.active();
}

std::size_t ExamAppSuppressionExecutor::scan(std::uint32_t target_session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!classifier_.active() || target_session_id == 0 || !enumerator_ ||
        !terminator_) {
        return 0;
    }

    const auto processes = enumerator_();
    std::size_t suppressed = 0;

    for (const auto& proc : processes) {
        if (proc.session_id == 0 || proc.session_id != target_session_id) {
            continue;
        }

        if (classifier_.should_suppress(proc.image_name)) {
            if (terminator_(proc.pid, proc.image_name)) {
                ++suppressed;
                ++total_suppressions_;
                if (audit_cb_) {
                    audit_cb_(proc.pid, proc.image_name, proc.session_id);
                }
            }
        }
    }
    return suppressed;
}

std::size_t ExamAppSuppressionExecutor::total_suppressions() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_suppressions_;
}

const ExamAppSuppression& ExamAppSuppressionExecutor::classifier() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return classifier_;
}

} // namespace nstu::client
