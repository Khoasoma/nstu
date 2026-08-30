#include "nstu/session.hpp"

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

namespace nstu::client {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

bool launch_agent_in_active_session(const std::wstring& agent_path,
                                    std::string* error) {
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xffffffffu) {
        set_error(error, "no active console session");
        return false;
    }
    HANDLE user_token = nullptr;
    if (!WTSQueryUserToken(session_id, &user_token)) {
        set_error(error, "WTSQueryUserToken failed");
        return false;
    }
    void* environment = nullptr;
    CreateEnvironmentBlock(&environment, user_token, FALSE);
    std::wstring command = L"\"" + agent_path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessAsUserW(
        user_token, agent_path.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, environment, nullptr, &startup, &process);
    if (environment != nullptr) {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(user_token);
    if (!created) {
        set_error(error, "CreateProcessAsUserW failed");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool harden_service_dacl(const std::wstring& service_name, std::string* error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        set_error(error, "OpenSCManagerW failed");
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, service_name.c_str(), WRITE_DAC);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        set_error(error, "OpenServiceW failed");
        return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t sddl[] =
        L"D:P(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)"
        L"(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)"
        L"(A;;LCLORC;;;AU)";
    const bool converted = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
    const bool applied = converted &&
        SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION,
                                 descriptor) != FALSE;
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!applied) {
        set_error(error, "service DACL update failed");
    }
    return applied;
}

} // namespace nstu::client
