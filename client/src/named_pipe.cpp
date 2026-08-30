#include "nstu/named_pipe.hpp"

#include <windows.h>
#include <sddl.h>

#include <limits>
#include <utility>

namespace nstu::client {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

NamedPipe::NamedPipe() noexcept = default;

NamedPipe::~NamedPipe() {
    close();
}

NamedPipe::NamedPipe(NamedPipe&& other) noexcept
    : handle_(std::exchange(other.handle_, 0)) {}

NamedPipe& NamedPipe::operator=(NamedPipe&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, 0);
    }
    return *this;
}

bool NamedPipe::create_server(std::wstring_view name, std::string* error) {
    close();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t sddl[] =
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        set_error(error, "pipe SDDL conversion failed");
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const std::wstring pipe_name(name);
    const HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &attributes);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        set_error(error, "CreateNamedPipeW failed");
        return false;
    }
    handle_ = reinterpret_cast<std::uintptr_t>(pipe);
    return true;
}

bool NamedPipe::wait_for_client(std::string* error) const {
    if (!is_open()) {
        set_error(error, "pipe is not open");
        return false;
    }
    if (ConnectNamedPipe(reinterpret_cast<HANDLE>(handle_), nullptr) ||
        GetLastError() == ERROR_PIPE_CONNECTED) {
        return true;
    }
    set_error(error, "ConnectNamedPipe failed");
    return false;
}

bool NamedPipe::connect_client(std::wstring_view name, std::uint32_t timeout_ms,
                               std::string* error) {
    close();
    const std::wstring pipe_name(name);
    if (!WaitNamedPipeW(pipe_name.c_str(), timeout_ms)) {
        set_error(error, "named pipe is unavailable");
        return false;
    }
    const HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        set_error(error, "named pipe client connection failed");
        return false;
    }
    handle_ = reinterpret_cast<std::uintptr_t>(pipe);
    return true;
}

int NamedPipe::write(std::span<const std::byte> bytes, std::string* error) const {
    if (!is_open() || bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        set_error(error, "invalid pipe write");
        return -1;
    }
    DWORD written = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(handle_), bytes.data(),
                   static_cast<DWORD>(bytes.size()), &written, nullptr)) {
        set_error(error, "pipe write failed");
        return -1;
    }
    return static_cast<int>(written);
}

int NamedPipe::read(std::span<std::byte> bytes, std::string* error) const {
    if (!is_open() || bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        set_error(error, "invalid pipe read");
        return -1;
    }
    DWORD read_bytes = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(handle_), bytes.data(),
                  static_cast<DWORD>(bytes.size()), &read_bytes, nullptr)) {
        set_error(error, "pipe read failed");
        return -1;
    }
    return static_cast<int>(read_bytes);
}

bool NamedPipe::available_bytes(std::uint32_t& bytes, std::string* error) const {
    bytes = 0;
    if (!is_open()) {
        set_error(error, "pipe is not open");
        return false;
    }
    DWORD available = 0;
    if (!PeekNamedPipe(reinterpret_cast<HANDLE>(handle_), nullptr, 0, nullptr,
                       &available, nullptr)) {
        set_error(error, "PeekNamedPipe failed");
        return false;
    }
    bytes = available;
    return true;
}

bool NamedPipe::is_open() const noexcept {
    return handle_ != 0 && handle_ !=
           reinterpret_cast<std::uintptr_t>(INVALID_HANDLE_VALUE);
}

void NamedPipe::close() noexcept {
    if (is_open()) {
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
    handle_ = 0;
}

} // namespace nstu::client
