#include "nstu/named_pipe.hpp"
#include "nstu/session.hpp"

#include <windows.h>
#include <wtsapi32.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace {

constexpr wchar_t kServiceName[] = L"nstu-service";
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop_event = nullptr;
std::filesystem::path g_agent_path;
std::mutex g_agent_path_mutex;

void launch_agent() {
    std::filesystem::path agent_path;
    {
        std::scoped_lock lock(g_agent_path_mutex);
        agent_path = g_agent_path;
    }
    if (agent_path.empty()) {
        return;
    }
    std::string ignored_error;
    const bool agent_started = nstu::client::launch_agent_in_active_session(
        agent_path.wstring(), &ignored_error);
    (void)agent_started;
}

void report_status(DWORD state, DWORD error = NO_ERROR) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    status.dwControlsAccepted = state == SERVICE_RUNNING
                                    ? SERVICE_ACCEPT_STOP |
                                          SERVICE_ACCEPT_SESSIONCHANGE
                                    : 0;
    g_status_handle && SetServiceStatus(g_status_handle, &status);
}

DWORD WINAPI control_handler(DWORD control, DWORD event_type, void*, void*) {
    if (control == SERVICE_CONTROL_STOP && g_stop_event != nullptr) {
        report_status(SERVICE_STOP_PENDING);
        SetEvent(g_stop_event);
    } else if (control == SERVICE_CONTROL_SESSIONCHANGE &&
               (event_type == WTS_SESSION_LOGON ||
                event_type == WTS_SESSION_UNLOCK ||
                event_type == WTS_CONSOLE_CONNECT)) {
        launch_agent();
    }
    return NO_ERROR;
}

void WINAPI service_main(DWORD, wchar_t**) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName,
                                                     control_handler, nullptr);
    if (g_status_handle == nullptr) {
        return;
    }
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        report_status(SERVICE_STOPPED, GetLastError());
        return;
    }
    report_status(SERVICE_START_PENDING);
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    {
        std::scoped_lock lock(g_agent_path_mutex);
        g_agent_path = std::filesystem::path(executable).parent_path() /
                       L"nstu-agent.exe";
    }
    std::string ignored_error;
    const bool dacl_hardened =
        nstu::client::harden_service_dacl(kServiceName, &ignored_error);
    (void)dacl_hardened;
    launch_agent();
    report_status(SERVICE_RUNNING);
    WaitForSingleObject(g_stop_event, INFINITE);
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    {
        std::scoped_lock lock(g_agent_path_mutex);
        g_agent_path.clear();
    }
    report_status(SERVICE_STOPPED);
}

} // namespace

int main() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table) &&
        GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        return 2;
    }
    return 0;
}
