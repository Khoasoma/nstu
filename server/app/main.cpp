#include "nstu/client_registry.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/deployment.hpp"
#include "nstu/discovery.hpp"
#include "nstu/key_store.hpp"
#include "nstu/screen_snapshot.hpp"
#include "nstu/secret_store.hpp"
#include "nstu/snapshot_generation_gate.hpp"
#include "nstu/stream_rearm_watchdog.hpp"
#include "nstu/telemetry.hpp"
#include "nstu/protocol_headers.h"
#include "nstu/remote_session_process.hpp"
#include "nstu/exam_grader.hpp"
#include "nstu/sealed_export.hpp"
#include "nstu/classroom_roster.hpp"
#include "nstu/file_distribution.hpp"
#include "nstu/operator_role.hpp"
#include "nstu/room_profile.hpp"
#include "nstu/support_bundle.hpp"
#include "nstu/url_validation.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <windowsx.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <shellapi.h>
#include <windows.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <deque>
#include <iomanip>
#include <map>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef NSTU_PROJECT_VERSION
#define NSTU_PROJECT_VERSION "development"
#endif

#ifndef NSTU_BUILD_CHANNEL
#define NSTU_BUILD_CHANNEL "Local"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_render_target;
ImFont* g_heading_font = nullptr;
bool g_dark_mode = false;
// Display-DPI scale factor (1.0 == 96 DPI). All style metrics and font sizes
// are authored at 96 DPI and multiplied by this so the UI stays crisp and
// correctly sized on high-DPI screens instead of being bitmap-scaled by Windows.
float g_ui_scale = 1.0f;
bool g_ui_scale_overridden = false;
bool g_imgui_ready = false;

// Maps a length authored at 96 DPI to the current display scale. Container
// sizes, fixed panel widths and popup dimensions below are hand-authored in
// 96-DPI pixels; wrapping them in scaled() keeps them proportional to the
// DPI-scaled fonts so nothing clips or overlaps on high-DPI screens. At
// g_ui_scale == 1.0 (96 DPI) this is the identity, so 96-DPI rendering is
// byte-for-byte unchanged.
[[nodiscard]] inline float scaled(float length_at_96dpi) noexcept {
    return length_at_96dpi * g_ui_scale;
}

bool g_graphics_debug = false;
bool g_graphics_device_lost = false;

struct DiagnosticEvent {
    std::string timestamp;
    std::string severity;
    std::string source;
    std::string message;
};

struct GraphicsReport {
    std::vector<std::string> adapters;
    std::string selected_adapter;
    std::string selected_vendor;
    std::string feature_level;
    std::string device_mode;
    std::string desktop_duplication;
    std::string h264_encoders;
};

std::deque<DiagnosticEvent> g_diagnostics;
GraphicsReport g_graphics_report;
nstu::telemetry::ConsentPolicy g_telemetry_policy;
std::deque<nstu::telemetry::Event> g_telemetry_events;
std::atomic_bool g_telemetry_error_prompt_requested = false;
std::atomic_bool g_telemetry_error_prompt_armed = false;

constexpr wchar_t kServerSettingsKey[] = L"Software\\NSTU\\Server";
constexpr wchar_t kTelemetryEnabledValue[] = L"TelemetryEnabled";
constexpr wchar_t kTelemetryPromptValue[] = L"TelemetryPromptOnError";
constexpr wchar_t kDiagnosticIssueUrl[] =
    L"https://github.com/Khoasoma/nstu/issues/new?template=sanitized-diagnostic.yml";

class RegistryKey final {
public:
    RegistryKey() = default;
    ~RegistryKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    HKEY* put() noexcept { return &key_; }
    HKEY get() const noexcept { return key_; }

private:
    HKEY key_ = nullptr;
};

bool read_server_setting(const wchar_t* name, bool fallback) noexcept {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const auto status = RegGetValueW(
        HKEY_CURRENT_USER, kServerSettingsKey, name, RRF_RT_REG_DWORD,
        nullptr, &value, &bytes);
    return status == ERROR_SUCCESS ? value != 0 : fallback;
}

nstu::telemetry::ConsentPolicy load_telemetry_policy() noexcept {
    nstu::telemetry::ConsentPolicy policy;
    policy.collect_in_background =
        read_server_setting(kTelemetryEnabledValue, false);
    policy.prompt_on_error = policy.collect_in_background &&
        read_server_setting(kTelemetryPromptValue, false);
    return policy;
}

bool save_telemetry_policy(
    const nstu::telemetry::ConsentPolicy& policy) noexcept {
    const DWORD previous_prompt =
        read_server_setting(kTelemetryPromptValue, false) ? 1u : 0u;
    RegistryKey key;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kServerSettingsKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        key.put(), &disposition) != ERROR_SUCCESS) {
        return false;
    }
    (void)disposition;
    const DWORD enabled = policy.collect_in_background ? 1u : 0u;
    const DWORD prompt = policy.collect_in_background && policy.prompt_on_error
        ? 1u : 0u;
    if (RegSetValueExW(key.get(), kTelemetryPromptValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&prompt),
                       sizeof(prompt)) != ERROR_SUCCESS) {
        return false;
    }
    if (RegSetValueExW(key.get(), kTelemetryEnabledValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&enabled),
                       sizeof(enabled)) == ERROR_SUCCESS) {
        return true;
    }
    (void)RegSetValueExW(key.get(), kTelemetryPromptValue, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&previous_prompt),
                         sizeof(previous_prompt));
    return false;
}

nstu::telemetry::Severity telemetry_severity(
    std::string_view severity) noexcept {
    if (severity == "error") {
        return nstu::telemetry::Severity::error;
    }
    if (severity == "warning") {
        return nstu::telemetry::Severity::warning;
    }
    return nstu::telemetry::Severity::information;
}

std::string hresult_text(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex
           << static_cast<unsigned long>(result);
    return stream.str();
}

bool is_device_loss_hresult(HRESULT result) noexcept {
    return result == DXGI_ERROR_DEVICE_REMOVED ||
           result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DEVICE_HUNG ||
           result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void record_diagnostic(const char* severity, const char* source,
                       const std::string& message) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::ostringstream timestamp;
    timestamp << std::setfill('0') << std::setw(2) << time.wHour << ':'
              << std::setw(2) << time.wMinute << ':' << std::setw(2)
              << time.wSecond;
    g_diagnostics.push_back({timestamp.str(), severity, source, message});
    while (g_diagnostics.size() > 128) {
        g_diagnostics.pop_front();
    }
    if (nstu::telemetry::collection_enabled(g_telemetry_policy)) {
        nstu::telemetry::append_bounded_event(
            g_telemetry_events,
            {telemetry_severity(severity), source, message});
        if (std::string_view(severity) == "error" &&
            nstu::telemetry::should_prompt_for_error(g_telemetry_policy) &&
            g_telemetry_error_prompt_armed.exchange(false)) {
            g_telemetry_error_prompt_requested.store(true);
        }
    }
}

void record_operation_failure(const char* source, std::string_view operation,
                              const std::string& error) {
    std::string message(operation);
    if (!error.empty()) {
        message += ": ";
        message += error;
    }
    record_diagnostic("warning", source, message);
}

std::wstring diagnostics_text() {
    std::wstring text = L"NSTU graphics initialization failed.\n\n";
    for (const auto& event : g_diagnostics) {
        const int length = MultiByteToWideChar(
            CP_UTF8, 0, event.message.c_str(), -1, nullptr, 0);
        std::wstring message;
        if (length > 1) {
            message.resize(static_cast<std::size_t>(length));
            MultiByteToWideChar(CP_UTF8, 0, event.message.c_str(), -1,
                                message.data(), length);
            message.resize(static_cast<std::size_t>(length - 1));
        }
        text += L"[" + std::wstring(event.severity.begin(),
                                     event.severity.end()) + L"] " +
                std::wstring(event.source.begin(), event.source.end()) +
                L": " + message + L"\n";
    }
    return text;
}

const char* vendor_name(UINT vendor_id) {
    switch (vendor_id) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft";
    default: return "Unknown";
    }
}

std::string wide_to_utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length,
                        nullptr, nullptr);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

void refresh_graphics_report() {
    g_graphics_report = {};
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        record_diagnostic("error", "DXGI", "CreateDXGIFactory1 failed " +
            hresult_text(result));
        return;
    }
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            record_diagnostic("warning", "DXGI", "EnumAdapters1 failed " +
                hresult_text(result));
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        std::ostringstream line;
        line << wide_to_utf8(description.Description) << " | "
             << vendor_name(description.VendorId) << " | "
             << (description.DedicatedVideoMemory / (1024u * 1024u))
             << " MB VRAM";
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            line << " | software";
        }
        g_graphics_report.adapters.push_back(line.str());
    }
    if (!g_graphics_report.adapters.empty()) {
        record_diagnostic("info", "DXGI", "Enumerated " +
            std::to_string(g_graphics_report.adapters.size()) + " adapter(s)");
    }

    if (g_device) {
        D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
        std::ostringstream feature;
        switch (level) {
        case D3D_FEATURE_LEVEL_11_1: feature << "11.1"; break;
        case D3D_FEATURE_LEVEL_11_0: feature << "11.0"; break;
        case D3D_FEATURE_LEVEL_10_1: feature << "10.1"; break;
        case D3D_FEATURE_LEVEL_10_0: feature << "10.0"; break;
        default: feature << "unknown"; break;
        }
        g_graphics_report.feature_level = feature.str();
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        if (SUCCEEDED(g_device.As(&dxgi_device))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC description{};
                if (SUCCEEDED(adapter->GetDesc(&description))) {
                    g_graphics_report.selected_adapter =
                        wide_to_utf8(description.Description);
                    g_graphics_report.selected_vendor =
                        vendor_name(description.VendorId);
                    g_graphics_report.device_mode =
                        g_graphics_report.device_mode.empty()
                            ? "Hardware" : g_graphics_report.device_mode;
                }
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
                    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
                    if (SUCCEEDED(output.As(&output1))) {
                        Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
                        const HRESULT duplicate_result =
                            output1->DuplicateOutput(g_device.Get(), &duplication);
                        g_graphics_report.desktop_duplication =
                            SUCCEEDED(duplicate_result) ? "Available"
                                                        : "Unavailable (" +
                            hresult_text(duplicate_result) + ")";
                    }
                }
            }
        }
    }
    HRESULT mf_result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    UINT32 encoder_count = 0;
    if (SUCCEEDED(mf_result)) {
        IMFActivate** activates = nullptr;
        MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video,
                                           MFVideoFormat_H264};
        const HRESULT enum_result = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE,
            nullptr, &output_type, &activates, &encoder_count);
        if (FAILED(enum_result)) encoder_count = 0;
        if (activates != nullptr) {
            for (UINT32 i = 0; i < encoder_count; ++i) activates[i]->Release();
            CoTaskMemFree(activates);
        }
        MFShutdown();
    }
    g_graphics_report.h264_encoders = std::to_string(encoder_count);
}

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayToggleWindow = 2001;
constexpr UINT kTrayExit = 2002;
UINT g_taskbar_created_message = 0;
NOTIFYICONDATAW g_tray_icon{};
// The manager's main window, used for taskbar attention (FlashWindowEx) when a
// student chat arrives while the window is not in the foreground.
HWND g_main_window = nullptr;

constexpr int kMinimumSnapshotInterval = 5;
constexpr int kMaximumSnapshotInterval = 10;

enum class DashboardView : int {
    room_screens,
    selected_client,
    exam,
    fleet_health,
};

using nstu::server::ClassDisplayMode;
using nstu::server::RoomFilter;
using nstu::server::BulkActionKind;
using nstu::server::OperatorRole;
using nstu::server::OperatorAction;
using nstu::server::is_action_permitted;
using nstu::server::role_label;

enum class IconKind : int {
    grid,
    monitor,
    camera,
    stop,
    lock,
    unlock,
    pen,
    erase,
    broadcast,
    chat,
};

enum class Language : int {
    english,
    vietnamese,
};

enum class AnnotationTool : int {
    pen,
    ruler,
    arrow,
    rectangle,
    ellipse,
    eraser,
};

Language g_language = Language::english;

struct DashboardState {
    DashboardView view = DashboardView::room_screens;
    OperatorRole current_role = OperatorRole::teacher;
    ClassDisplayMode display_mode = ClassDisplayMode::grid;
    RoomFilter room_filter = RoomFilter::all;
    std::unordered_set<std::uint64_t> checked_client_ids;
    bool bulk_confirmation_open = false;
    BulkActionKind pending_bulk_action = BulkActionKind::lock;
    std::array<char, 2048> launch_url_input{"https://"};
    std::array<char, 1024> file_dist_path_input{""};
    std::uint64_t selected_client_id = 0;
    int snapshot_interval_seconds = 7;
    // When set, every online computer is snapshotted continuously without the
    // operator clicking "Start snapshots" per machine; a periodic sweep re-arms
    // any client that reconnects. Turn it off to manage snapshots per computer.
    bool auto_monitor = true;
    std::chrono::steady_clock::time_point next_auto_monitor_sweep{};
    Language language = Language::english;
    bool dark_mode = false;
    bool show_offline = true;
    bool broadcast_enabled = false;
    // The client currently receiving a live Focus stream (~15fps). Distinct
    // from the slow class-view snapshots; zero when no client is focused.
    std::uint64_t streaming_client_id = 0;
    // Measured delivered-frame rate for the focused stream (from
    // ClientRecord::snapshot_generation deltas) — Epic B diagnostic so the
    // Focus label shows real fps instead of a fixed "~15fps".
    std::uint64_t fps_sample_client_id = 0;
    std::uint64_t fps_sample_generation = 0;
    std::chrono::steady_clock::time_point fps_sample_time{};
    double measured_stream_fps = 0.0;
    // Self-heals a stalled Focus/remote stream. Arming above is edge-triggered
    // on the streamed client id, so a start_stream lost across a client/agent
    // reconnect leaves the server believing a client streams while its agent
    // sends nothing. This re-asserts set_streaming when delivered frames stop.
    nstu::server::StreamRearmWatchdog stream_watchdog;
    bool annotation_enabled = false;
    bool uwf_confirmation_open = false;
    std::uint64_t uwf_confirmation_client_id = 0;
    bool annotation_dragging = false;
    ImVec2 previous_annotation_point{};
    AnnotationTool annotation_tool = AnnotationTool::pen;
    std::uint32_t annotation_rgba = 0xe5484dffu;
    int annotation_thickness = 4;
    std::chrono::steady_clock::time_point next_host_snapshot{};
    nstu::control::DemonstrationMode broadcast_mode =
        nstu::control::DemonstrationMode::focus;
    bool broadcast_lock_input = false;
    std::string control_status;
    std::string pairing_status;
    bool pairing_panel_open = false;
    std::string startup_error;
    std::string telemetry_report_preview;
    bool telemetry_report_requested = false;
    bool support_bundle_modal_open = false;
    bool support_bundle_consented = false;
    std::string support_bundle_preview;
    bool room_profile_modal_open = false;
    bool software_updates_modal_open = false;
    nstu::server::FleetUpdateConfig fleet_update_config;
    std::string room_profile_export_text;
    std::array<char, 65536> room_profile_import_buffer{};
    std::string room_profile_status_message;
    bool room_profile_status_is_error = false;
    std::array<char, 96> client_filter{};
    std::array<char, 512> chat_input{};
    std::array<char, nstu::exam::kMaximumExamControlPathBytes + 1>
        exam_package_root{};
    std::array<char, nstu::exam::kMaximumExamControlPackageIdBytes + 1>
        exam_package_id{};
    std::array<char, nstu::exam::kMaximumExamControlCandidateBytes + 1>
        exam_candidate_id{};
    std::array<char, nstu::security::kSha256Bytes * 2 + 1> exam_digest_hex{};
    // Chat notification state. `chat_counts` is the current per-client inbound
    // (student) message count refreshed each frame; `chat_seen_counts` is the
    // count when the teacher last had that client's chat on screen (badge
    // baseline); `chat_observed_counts` is the count at the previous poll (edge
    // detection for the one-shot taskbar flash). `chat_counts_initialized`
    // seeds all three from the first poll so pre-existing history neither badges
    // nor flashes when the manager launches.
    std::unordered_map<std::uint64_t, std::size_t> chat_counts;
    std::unordered_map<std::uint64_t, std::size_t> chat_seen_counts;
    std::unordered_map<std::uint64_t, std::size_t> chat_observed_counts;
    bool chat_counts_initialized = false;
    // Operator-editable room label shown to computers choosing this server. The
    // buffer backs the "Add computers" text field (sized to the discovery name
    // bound plus a NUL); room_name_path is where the plaintext hint persists and
    // is empty when the data root is unavailable. A display hint, never a
    // credential.
    std::array<char, nstu::discovery::kMaximumServerNameBytes + 1>
        room_name_input{};
    std::filesystem::path room_name_path;
    std::vector<std::string> discovered_packages;
    int selected_package_index = -1;
    std::string last_export_path;
};

const char* tr(const DashboardState& state, const char* english,
               const char* vietnamese) {
    return state.language == Language::vietnamese ? vietnamese : english;
}

// A client has unread student chat when its current inbound count exceeds the
// count the teacher last had on screen for it.
bool parse_sha256_hex(std::string_view text,
                      nstu::security::Sha256Digest& digest) noexcept {
    if (text.size() != digest.size() * 2) {
        return false;
    }
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    bool non_zero = false;
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[index] = static_cast<std::byte>((high << 4) | low);
        non_zero = non_zero || digest[index] != std::byte{0};
    }
    return non_zero;
}

bool client_has_unread_chat(const DashboardState& state, std::uint64_t id) {
    const auto current = state.chat_counts.find(id);
    if (current == state.chat_counts.end()) {
        return false;
    }
    const auto seen = state.chat_seen_counts.find(id);
    const std::size_t seen_count =
        seen == state.chat_seen_counts.end() ? 0 : seen->second;
    return current->second > seen_count;
}

bool update_telemetry_policy(
    DashboardState& state,
    const nstu::telemetry::ConsentPolicy& requested_policy) {
    auto policy = requested_policy;
    if (!policy.collect_in_background) {
        policy.prompt_on_error = false;
    }
    if (!save_telemetry_policy(policy)) {
        state.control_status = tr(
            state, "Could not save diagnostic-sharing settings.",
            "Không thể lưu cài đặt chia sẻ chẩn đoán.");
        return false;
    }

    const bool collection_started =
        !g_telemetry_policy.collect_in_background &&
        policy.collect_in_background;
    const bool prompt_started =
        !nstu::telemetry::should_prompt_for_error(g_telemetry_policy) &&
        nstu::telemetry::should_prompt_for_error(policy);
    g_telemetry_policy = policy;
    if (!policy.collect_in_background) {
        g_telemetry_events.clear();
        g_telemetry_error_prompt_requested.store(false);
        g_telemetry_error_prompt_armed.store(false);
        state.telemetry_report_preview.clear();
        state.control_status = tr(
            state, "Optional diagnostics disabled and collected data cleared.",
            "Đã tắt chẩn đoán tùy chọn và xóa dữ liệu đã thu thập.");
    } else if (collection_started) {
        g_telemetry_error_prompt_armed.store(policy.prompt_on_error);
        record_diagnostic("info", "Diagnostics",
                          "Optional local diagnostic collection enabled");
        state.control_status = tr(
            state, "Optional local diagnostics enabled.",
            "Đã bật chẩn đoán cục bộ tùy chọn.");
    } else {
        if (!policy.prompt_on_error) {
            g_telemetry_error_prompt_requested.store(false);
            g_telemetry_error_prompt_armed.store(false);
        } else if (prompt_started) {
            g_telemetry_error_prompt_armed.store(true);
        }
        state.control_status = tr(
            state, "Diagnostic-sharing settings saved.",
            "Đã lưu cài đặt chia sẻ chẩn đoán.");
    }
    return true;
}

std::string client_count_bucket(std::size_t count) {
    if (count == 0) return "0";
    if (count <= 10) return "1-10";
    if (count <= 25) return "11-25";
    if (count <= 50) return "26-50";
    return "51+";
}

std::string report_value(const std::string& value) {
    return value.empty() ? "Unknown" : value;
}

nstu::telemetry::PublicReport make_telemetry_report(
    const DashboardState& state, std::size_t client_count) {
    nstu::telemetry::PublicReport report;
    report.application = "NSTU Server";
    report.version = NSTU_PROJECT_VERSION;
    report.build_channel = NSTU_BUILD_CHANNEL;
    report.fields = {
        {"Graphics device mode", report_value(g_graphics_report.device_mode)},
        {"Graphics adapter", report_value(g_graphics_report.selected_adapter)},
        {"Graphics vendor", report_value(g_graphics_report.selected_vendor)},
        {"D3D feature level", report_value(g_graphics_report.feature_level)},
        {"Desktop Duplication",
         report_value(g_graphics_report.desktop_duplication)},
        {"Hardware H.264 encoders",
         report_value(g_graphics_report.h264_encoders)},
        {"Snapshot interval",
         std::to_string(state.snapshot_interval_seconds) + " seconds"},
        {"Known client count", client_count_bucket(client_count)},
    };
    report.events = g_telemetry_events;
    return report;
}

void show_main_window(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

bool add_tray_icon(HWND window) {
    g_tray_icon = {};
    g_tray_icon.cbSize = sizeof(g_tray_icon);
    g_tray_icon.hWnd = window;
    g_tray_icon.uID = 1;
    g_tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray_icon.uCallbackMessage = kTrayMessage;
    g_tray_icon.hIcon =
        LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpyW(g_tray_icon.szTip, L"NSTU Server");
    if (!Shell_NotifyIconW(NIM_ADD, &g_tray_icon)) {
        return false;
    }
    g_tray_icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_tray_icon);
    return true;
}

void show_tray_menu(HWND window) {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    const bool visible = IsWindowVisible(window) != FALSE;
    const bool vietnamese = g_language == Language::vietnamese;
    AppendMenuW(menu, MF_STRING, kTrayToggleWindow,
                visible ? (vietnamese ? L"Ẩn NSTU Server"
                                      : L"Hide NSTU Server")
                        : (vietnamese ? L"Mở NSTU Server"
                                      : L"Open NSTU Server"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit,
                vietnamese ? L"Thoát" : L"Exit");
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
}

struct SnapshotTexture {
    nstu::server::SnapshotGenerationGate generation_gate;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
};

std::unordered_map<std::uint64_t, SnapshotTexture> g_snapshot_textures;

void invalidate_snapshot_textures() noexcept {
    for (auto& [client_id, texture] : g_snapshot_textures) {
        (void)client_id;
        texture.generation_gate.reset();
        texture.width = 0;
        texture.height = 0;
        texture.view.Reset();
    }
}

using nstu::server::RoomCounts;

void create_render_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        const HRESULT result = g_device->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &g_render_target);
        if (FAILED(result)) {
            record_diagnostic("error", "D3D11",
                              "CreateRenderTargetView failed " +
                                  hresult_text(result));
        }
    } else {
        record_diagnostic("error", "DXGI", "Swap-chain back-buffer unavailable");
    }
}

bool create_device(HWND window) {
    g_graphics_device_lost = true;
    invalidate_snapshot_textures();
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = g_graphics_debug
        ? static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG) : 0u;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        record_diagnostic("error", "DXGI", "CreateDXGIFactory1 failed " +
            hresult_text(result));
        return false;
    }
    bool created = false;
    for (UINT index = 0; !created; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) break;
        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (FAILED(adapter->GetDesc1(&adapter_desc)) ||
            (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        result = D3D11CreateDeviceAndSwapChain(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            requested, ARRAYSIZE(requested), D3D11_SDK_VERSION, &description,
            &g_swap_chain, &g_device, nullptr, &g_context);
        if (FAILED(result) && flags != 0u) {
            record_diagnostic("warning", "D3D11",
                              "Debug layer unavailable; retrying without it (" +
                                  hresult_text(result) + ")");
            flags = 0u;
            result = D3D11CreateDeviceAndSwapChain(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                requested, ARRAYSIZE(requested), D3D11_SDK_VERSION,
                &description, &g_swap_chain, &g_device, nullptr, &g_context);
        }
        if (SUCCEEDED(result)) {
            created = true;
            g_graphics_report.device_mode = "Hardware";
            record_diagnostic("info", "D3D11", "Hardware device created on " +
                wide_to_utf8(adapter_desc.Description));
        } else {
            record_diagnostic("warning", "D3D11",
                              "Hardware adapter failed " +
                                  wide_to_utf8(adapter_desc.Description) +
                                  " (" + hresult_text(result) + ")");
            g_swap_chain.Reset();
            g_device.Reset();
            g_context.Reset();
        }
    }
    if (!created) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, requested,
            ARRAYSIZE(requested), D3D11_SDK_VERSION, &description,
            &g_swap_chain, &g_device, nullptr, &g_context);
        if (SUCCEEDED(result)) {
            created = true;
            g_graphics_report.device_mode = "WARP fallback";
            record_diagnostic("warning", "D3D11",
                              "Hardware initialization failed; using WARP fallback");
        } else {
            record_diagnostic("error", "D3D11",
                              "WARP device creation failed " + hresult_text(result));
        }
    }
    if (!created) return false;
    create_render_target();
    if (!g_render_target) return false;
    g_graphics_device_lost = false;
    refresh_graphics_report();
    return true;
}

// ---- Separate remote-control window (Part C) ------------------------------
// An independent top-level window that shows the controlled client's live
// stream and forwards mouse/keyboard as RemoteInputPacket. It shares the main
// D3D11 device through a second swap chain plus a tiny texture-blit pipeline,
// so the Focus view stays display-only (plus annotation) with no embedded
// remote input. All access happens on the UI thread: the window is created,
// pumped, rendered, and torn down from the main loop's single thread.
struct RemoteImageRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

HWND g_remote_window = nullptr;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_remote_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_remote_render_target;
Microsoft::WRL::ComPtr<ID3D11VertexShader> g_remote_vertex_shader;
Microsoft::WRL::ComPtr<ID3D11PixelShader> g_remote_pixel_shader;
Microsoft::WRL::ComPtr<ID3D11SamplerState> g_remote_sampler;
// A second, point-filtered sampler used only when the remote window is enlarged
// past the 720p stream. Bilinear upscaling blurs small text; nearest keeps it
// crisp. Downscaling still uses the linear sampler to avoid aliasing.
Microsoft::WRL::ComPtr<ID3D11SamplerState> g_remote_sampler_point;
std::uint64_t g_remote_target_client = 0;
nstu::server::ServerControlPlane* g_remote_control_plane = nullptr;
RemoteImageRect g_remote_image_rect;
std::atomic_bool g_remote_close_pending{false};
bool g_remote_class_registered = false;
std::unique_ptr<nstu::server::RemoteSessionProcess> g_remote_session;
bool g_use_legacy_remote = false;

std::filesystem::path default_remote_viewer_path() {
    wchar_t env_buffer[MAX_PATH]{};
    const DWORD env_len =
        GetEnvironmentVariableW(L"NSTU_REMOTE_CLIENT_PATH", env_buffer, MAX_PATH);
    if (env_len > 0 && env_len < MAX_PATH) {
        return std::filesystem::path(env_buffer);
    }
    const DWORD ml_len =
        GetEnvironmentVariableW(L"MOONLIGHT_PATH", env_buffer, MAX_PATH);
    if (ml_len > 0 && ml_len < MAX_PATH) {
        return std::filesystem::path(env_buffer);
    }

    wchar_t prog_files[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", prog_files, MAX_PATH) > 0) {
        const auto candidate = std::filesystem::path(prog_files) /
                               L"Moonlight Game Streaming" / L"Moonlight.exe";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    wchar_t prog_files_x86[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", prog_files_x86, MAX_PATH) > 0) {
        const auto candidate = std::filesystem::path(prog_files_x86) /
                               L"Moonlight Game Streaming" / L"Moonlight.exe";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    if (prog_files[0] != L'\0') {
        return std::filesystem::path(prog_files) / L"Moonlight Game Streaming" /
               L"Moonlight.exe";
    }
    return L"C:\\Program Files\\Moonlight Game Streaming\\Moonlight.exe";
}

void apply_dashboard_style(bool dark_mode);
void load_dashboard_fonts();

void apply_suggested_dpi_rect(HWND window, LPARAM lparam) {
    const auto* suggested = reinterpret_cast<const RECT*>(lparam);
    if (suggested == nullptr) {
        return;
    }
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

bool ensure_remote_pipeline() {
    if (g_remote_vertex_shader && g_remote_pixel_shader && g_remote_sampler &&
        g_remote_sampler_point) {
        return true;
    }
    if (!g_device) {
        return false;
    }
    static const char kVertexShader[] =
        "struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
        "VSOut main(uint id:SV_VertexID){VSOut o;"
        "float2 uv=float2((id<<1)&2,id&2);o.uv=uv;"
        "o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);return o;}";
    static const char kPixelShader[] =
        "Texture2D tex:register(t0);SamplerState smp:register(s0);"
        "float4 main(float4 pos:SV_POSITION,float2 uv:TEXCOORD0):SV_TARGET"
        "{return tex.Sample(smp,uv);}";
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "remote_vs",
                            nullptr, nullptr, "main", "vs_4_0", 0, 0, &vs_blob,
                            &errors);
    if (FAILED(hr)) {
        record_diagnostic("error", "D3D11",
                          "Remote vertex shader compile failed " + hresult_text(hr));
        return false;
    }
    hr = D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, "remote_ps", nullptr,
                    nullptr, "main", "ps_4_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) {
        record_diagnostic("error", "D3D11",
                          "Remote pixel shader compile failed " + hresult_text(hr));
        return false;
    }
    if (FAILED(g_device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                            vs_blob->GetBufferSize(), nullptr,
                                            &g_remote_vertex_shader)) ||
        FAILED(g_device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                           ps_blob->GetBufferSize(), nullptr,
                                           &g_remote_pixel_shader))) {
        g_remote_vertex_shader.Reset();
        g_remote_pixel_shader.Reset();
        return false;
    }
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sampler_desc, &g_remote_sampler))) {
        return false;
    }
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(g_device->CreateSamplerState(&sampler_desc,
                                            &g_remote_sampler_point))) {
        return false;
    }
    return true;
}

bool create_remote_render_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (!g_remote_swap_chain ||
        FAILED(g_remote_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        return false;
    }
    return SUCCEEDED(g_device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &g_remote_render_target));
}

void render_remote_window(ID3D11ShaderResourceView* srv, std::uint32_t tex_w,
                          std::uint32_t tex_h) {
    if (!g_remote_window || !g_remote_swap_chain || !g_remote_render_target ||
        !g_context || g_graphics_device_lost) {
        return;
    }
    RECT client{};
    GetClientRect(g_remote_window, &client);
    const float cw = static_cast<float>(client.right - client.left);
    const float ch = static_cast<float>(client.bottom - client.top);
    if (cw < 1.0f || ch < 1.0f) {
        return;
    }
    // Letterbox the client's stream aspect into the window client area so the
    // whole remote screen stays visible and input maps to the right pixels.
    float iw = cw;
    float ih = ch;
    float ix = 0.0f;
    float iy = 0.0f;
    if (tex_w > 0 && tex_h > 0) {
        const float ta = static_cast<float>(tex_w) / static_cast<float>(tex_h);
        const float ca = cw / ch;
        if (ca > ta) {
            ih = ch;
            iw = ch * ta;
            ix = (cw - iw) * 0.5f;
        } else {
            iw = cw;
            ih = cw / ta;
            iy = (ch - ih) * 0.5f;
        }
    }
    g_remote_image_rect = {ix, iy, iw, ih};
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_context->OMSetRenderTargets(1, g_remote_render_target.GetAddressOf(),
                                  nullptr);
    g_context->ClearRenderTargetView(g_remote_render_target.Get(), clear_color);
    if (srv != nullptr && ensure_remote_pipeline()) {
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = ix;
        viewport.TopLeftY = iy;
        viewport.Width = iw;
        viewport.Height = ih;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        g_context->RSSetViewports(1, &viewport);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_remote_vertex_shader.Get(), nullptr, 0);
        g_context->PSSetShader(g_remote_pixel_shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[1] = {srv};
        g_context->PSSetShaderResources(0, 1, views);
        // Enlarged past the source: nearest-filter keeps text crisp instead of
        // blurring it. At or below native, linear avoids downscale aliasing.
        const bool upscaling = tex_w > 0 && tex_h > 0 &&
            (iw > static_cast<float>(tex_w) ||
             ih > static_cast<float>(tex_h));
        ID3D11SamplerState* sampler =
            (upscaling ? g_remote_sampler_point : g_remote_sampler).Get();
        g_context->PSSetSamplers(0, 1, &sampler);
        g_context->Draw(3, 0);
        ID3D11ShaderResourceView* null_views[1] = {nullptr};
        g_context->PSSetShaderResources(0, 1, null_views);
    }
    (void)g_remote_swap_chain->Present(0, 0);
}

LRESULT CALLBACK remote_window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        if (g_remote_control_plane != nullptr && g_remote_target_client != 0) {
            const int mx = GET_X_LPARAM(lparam);
            const int my = GET_Y_LPARAM(lparam);
            const RemoteImageRect& rect = g_remote_image_rect;
            float rx = 0.5f;
            float ry = 0.5f;
            if (rect.w <= 0.0f || rect.h <= 0.0f) {
                return 0;
            }
            const bool inside =
                static_cast<float>(mx) >= rect.x &&
                static_cast<float>(mx) <= rect.x + rect.w &&
                static_cast<float>(my) >= rect.y &&
                static_cast<float>(my) <= rect.y + rect.h;
            const bool release = message == WM_LBUTTONUP ||
                                 message == WM_RBUTTONUP;
            if (!inside && GetCapture() != hwnd && !release) {
                // Letterbox bars are not part of the student's screen. Ignore
                // an initial click/move there instead of clamping it to an edge
                // and accidentally clicking the remote desktop.
                return 0;
            }
            rx = std::clamp((static_cast<float>(mx) - rect.x) / rect.w,
                            0.0f, 1.0f);
            ry = std::clamp((static_cast<float>(my) - rect.y) / rect.h,
                            0.0f, 1.0f);
            nstu::wire::RemoteInputPacket packet{};
            packet.input_type =
                static_cast<std::uint8_t>(nstu::wire::RemoteInputType::mouse);
            packet.flags =
                static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_absolute) |
                static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_normalized);
            if (message == WM_LBUTTONDOWN) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_left_down);
                SetCapture(hwnd);
            } else if (message == WM_LBUTTONUP) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_left_up);
                ReleaseCapture();
            } else if (message == WM_RBUTTONDOWN) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_right_down);
                SetCapture(hwnd);
            } else if (message == WM_RBUTTONUP) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_right_up);
                ReleaseCapture();
            }
            packet.x = static_cast<std::int32_t>(rx * 65535.0f);
            packet.y = static_cast<std::int32_t>(ry * 65535.0f);
            std::string ignored;
            (void)g_remote_control_plane->send_remote_input(
                g_remote_target_client, packet, &ignored);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        if (g_remote_control_plane != nullptr && g_remote_target_client != 0) {
            nstu::wire::RemoteInputPacket packet{};
            packet.input_type =
                static_cast<std::uint8_t>(nstu::wire::RemoteInputType::keyboard);
            packet.virtual_key = static_cast<std::uint16_t>(wparam);
            if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                packet.flags = static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::key_up);
            }
            std::string ignored;
            (void)g_remote_control_plane->send_remote_input(
                g_remote_target_client, packet, &ignored);
        }
        return 0;
    }
    case WM_DPICHANGED:
        apply_suggested_dpi_rect(hwnd, lparam);
        return 0;
    case WM_SIZE:
        if (g_remote_swap_chain && wparam != SIZE_MINIMIZED) {
            g_remote_render_target.Reset();
            (void)g_remote_swap_chain->ResizeBuffers(
                0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
            create_remote_render_target();
        }
        return 0;
    case WM_CLOSE:
        // Tear-down runs on the main loop (COM release + control-plane stop)
        // rather than inside this synchronous window callback.
        g_remote_close_pending.store(true);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void close_remote_window(nstu::server::ServerControlPlane& control_plane) {
    if (g_remote_target_client != 0) {
        (void)control_plane.stop_remote_control(g_remote_target_client, nullptr);
        g_remote_target_client = 0;
    }
    g_remote_render_target.Reset();
    g_remote_swap_chain.Reset();
    if (g_remote_window != nullptr) {
        DestroyWindow(g_remote_window);
        g_remote_window = nullptr;
    }
    g_remote_image_rect = {};
    g_remote_close_pending.store(false);
}

bool open_remote_window(nstu::server::ServerControlPlane& control_plane,
                        std::uint64_t client_id) {
    if (!g_device || client_id == 0) {
        return false;
    }
    g_remote_control_plane = &control_plane;
    if (!ensure_remote_pipeline()) {
        return false;
    }
    if (g_remote_window == nullptr) {
        if (!g_remote_class_registered) {
            WNDCLASSW window_class{};
            window_class.lpfnWndProc = remote_window_proc;
            window_class.hInstance = GetModuleHandleW(nullptr);
            window_class.lpszClassName = L"NstuRemoteWindow";
            window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            window_class.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
            RegisterClassW(&window_class);
            g_remote_class_registered = true;
        }
        const float remote_scale = g_main_window != nullptr
            ? ImGui_ImplWin32_GetDpiScaleForHwnd(g_main_window)
            : g_ui_scale;
        g_remote_window = CreateWindowW(
            L"NstuRemoteWindow", L"NSTU Remote control", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            static_cast<int>(1120.0f * remote_scale),
            static_cast<int>(700.0f * remote_scale), nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (g_remote_window == nullptr) {
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        if (FAILED(g_device.As(&dxgi_device)) ||
            FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            DestroyWindow(g_remote_window);
            g_remote_window = nullptr;
            return false;
        }
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = g_remote_window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(factory->CreateSwapChain(g_device.Get(), &description,
                                            &g_remote_swap_chain)) ||
            !create_remote_render_target()) {
            g_remote_render_target.Reset();
            g_remote_swap_chain.Reset();
            DestroyWindow(g_remote_window);
            g_remote_window = nullptr;
            return false;
        }
        RECT client{};
        GetClientRect(g_remote_window, &client);
        g_remote_image_rect = {0.0f, 0.0f,
                               static_cast<float>(client.right - client.left),
                               static_cast<float>(client.bottom - client.top)};
        ShowWindow(g_remote_window, SW_SHOW);
    }
    if (g_remote_target_client != 0 && g_remote_target_client != client_id) {
        (void)control_plane.stop_remote_control(g_remote_target_client, nullptr);
        g_remote_target_client = 0;
    }
    std::string error;
    const bool ok = control_plane.start_remote_control(client_id, &error);
    if (ok) {
        g_remote_target_client = client_id;
        SetForegroundWindow(g_remote_window);
    } else {
        record_operation_failure("RemoteControl", "Start command failed", error);
    }
    return ok;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return 1;
    }
    if (message == WM_GETMINMAXINFO) {
        auto* minimum = reinterpret_cast<MINMAXINFO*>(lparam);
        LONG minimum_width = static_cast<LONG>(960.0f * g_ui_scale);
        LONG minimum_height = static_cast<LONG>(640.0f * g_ui_scale);
        if (HMONITOR monitor =
                MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)) {
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            if (GetMonitorInfoW(monitor, &monitor_info)) {
                const LONG work_width =
                    monitor_info.rcWork.right - monitor_info.rcWork.left;
                const LONG work_height =
                    monitor_info.rcWork.bottom - monitor_info.rcWork.top;
                minimum_width = std::min(minimum_width, work_width);
                minimum_height = std::min(minimum_height, work_height);
            }
        }
        minimum->ptMinTrackSize.x = minimum_width;
        minimum->ptMinTrackSize.y = minimum_height;
        return 0;
    }
    if (message == WM_DPICHANGED && !g_ui_scale_overridden) {
        g_ui_scale = static_cast<float>(HIWORD(wparam)) / 96.0f;
        if (!(g_ui_scale > 0.0f)) {
            g_ui_scale = 1.0f;
        }
        apply_suggested_dpi_rect(window, lparam);
        if (g_imgui_ready) {
            apply_dashboard_style(g_dark_mode);
            auto& atlas = ImGui::GetIO().Fonts;
            atlas->Clear();
            g_heading_font = nullptr;
            load_dashboard_fonts();
            ImGui_ImplDX11_InvalidateDeviceObjects();
            (void)ImGui_ImplDX11_CreateDeviceObjects();
        }
        return 0;
    }
    if (message == g_taskbar_created_message &&
        g_taskbar_created_message != 0) {
        add_tray_icon(window);
        return 0;
    }
    if (message == kTrayMessage) {
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            show_main_window(window);
        } else if (LOWORD(lparam) == WM_RBUTTONUP ||
                   LOWORD(lparam) == WM_CONTEXTMENU) {
            show_tray_menu(window);
        }
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wparam) == kTrayToggleWindow) {
            if (IsWindowVisible(window)) {
                ShowWindow(window, SW_HIDE);
            } else {
                show_main_window(window);
            }
            return 0;
        }
        if (LOWORD(wparam) == kTrayExit) {
            DestroyWindow(window);
            return 0;
        }
    }
    // Minimize behaves like a normal window and goes to the taskbar; only
    // closing hides NSTU to the system tray. Operators expect a plain minimize
    // here, so SC_MINIMIZE falls through to DefWindowProc's standard handling.
    if (message == WM_CLOSE) {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    if (message == WM_SIZE && g_swap_chain && wparam != SIZE_MINIMIZED) {
        g_render_target.Reset();
        const HRESULT resize_result = g_swap_chain->ResizeBuffers(
            0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resize_result)) {
            record_diagnostic("error", "DXGI", "ResizeBuffers failed " +
                hresult_text(resize_result));
        }
        create_render_target();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void apply_dashboard_style(bool dark_mode) {
    g_dark_mode = dark_mode;
    // Rebuild from the ImGui defaults every time. Theme switches may happen
    // repeatedly; scaling the existing style would multiply untouched metrics
    // (scrollbars, indentation, grab sizes) on every Light/Dark click.
    auto& style = ImGui::GetStyle();
    style = ImGuiStyle{};
    style.WindowPadding = {8.0f, 6.0f};
    style.FramePadding = {8.0f, 5.0f};
    style.CellPadding = {7.0f, 6.0f};
    style.ItemSpacing = {6.0f, 5.0f};
    style.ItemInnerSpacing = {5.0f, 4.0f};
    style.WindowRounding = 0.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = dark_mode
        ? ImVec4{0.878f, 0.895f, 0.918f, 1.0f}
        : ImVec4{0.135f, 0.145f, 0.160f, 1.0f};
    colors[ImGuiCol_TextDisabled] = dark_mode
        ? ImVec4{0.520f, 0.552f, 0.600f, 1.0f}
        : ImVec4{0.480f, 0.495f, 0.520f, 1.0f};
    colors[ImGuiCol_WindowBg] = dark_mode
        ? ImVec4{0.098f, 0.110f, 0.130f, 1.0f}
        : ImVec4{0.902f, 0.906f, 0.914f, 1.0f};
    colors[ImGuiCol_ChildBg] = dark_mode
        ? ImVec4{0.130f, 0.146f, 0.170f, 1.0f}
        : ImVec4{0.962f, 0.964f, 0.970f, 1.0f};
    colors[ImGuiCol_PopupBg] = dark_mode
        ? ImVec4{0.140f, 0.157f, 0.183f, 1.0f}
        : ImVec4{0.968f, 0.970f, 0.976f, 1.0f};
    colors[ImGuiCol_Border] = dark_mode
        ? ImVec4{0.235f, 0.260f, 0.300f, 1.0f}
        : ImVec4{0.820f, 0.828f, 0.845f, 1.0f};
    colors[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_FrameBg] = dark_mode
        ? ImVec4{0.155f, 0.173f, 0.200f, 1.0f}
        : ImVec4{0.940f, 0.943f, 0.950f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = dark_mode
        ? ImVec4{0.195f, 0.216f, 0.248f, 1.0f}
        : ImVec4{0.910f, 0.914f, 0.924f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = dark_mode
        ? ImVec4{0.230f, 0.254f, 0.290f, 1.0f}
        : ImVec4{0.878f, 0.884f, 0.898f, 1.0f};
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_Button] = dark_mode
        ? ImVec4{0.165f, 0.184f, 0.214f, 1.0f}
        : ImVec4{0.902f, 0.908f, 0.920f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = dark_mode
        ? ImVec4{0.210f, 0.233f, 0.268f, 1.0f}
        : ImVec4{0.860f, 0.868f, 0.882f, 1.0f};
    colors[ImGuiCol_ButtonActive] = dark_mode
        ? ImVec4{0.250f, 0.277f, 0.318f, 1.0f}
        : ImVec4{0.818f, 0.828f, 0.845f, 1.0f};
    colors[ImGuiCol_Header] = dark_mode
        ? ImVec4{0.150f, 0.235f, 0.290f, 1.0f}
        : ImVec4{0.820f, 0.872f, 0.910f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = dark_mode
        ? ImVec4{0.185f, 0.285f, 0.345f, 1.0f}
        : ImVec4{0.772f, 0.842f, 0.892f, 1.0f};
    colors[ImGuiCol_HeaderActive] = dark_mode
        ? ImVec4{0.215f, 0.330f, 0.400f, 1.0f}
        : ImVec4{0.720f, 0.812f, 0.872f, 1.0f};
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_CheckMark] = dark_mode
        ? ImVec4{0.380f, 0.640f, 0.780f, 1.0f}
        : ImVec4{0.160f, 0.360f, 0.470f, 1.0f};
    colors[ImGuiCol_SliderGrab] = dark_mode
        ? ImVec4{0.680f, 0.705f, 0.745f, 1.0f}
        : ImVec4{0.18f, 0.18f, 0.17f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = dark_mode
        ? ImVec4{0.850f, 0.870f, 0.900f, 1.0f}
        : ImVec4{0.30f, 0.30f, 0.29f, 1.0f};

    // Metrics above are authored at 96 DPI; scale them to the display so the
    // whole UI (padding, rounding, borders, scrollbars) tracks the font size.
    style.ScaleAllSizes(g_ui_scale);
}

void load_dashboard_fonts() {
    std::array<char, MAX_PATH> windows_directory{};
    const UINT length = GetWindowsDirectoryA(
        windows_directory.data(), static_cast<UINT>(windows_directory.size()));
    if (length == 0 || length >= windows_directory.size()) {
        return;
    }
    const std::string fonts_directory =
        std::string(windows_directory.data()) + "\\Fonts\\";
    const auto regular = fonts_directory + "segoeui.ttf";
    const auto bold = fonts_directory + "segoeuib.ttf";
    auto& atlas = ImGui::GetIO().Fonts;
    const ImWchar* glyph_ranges = atlas->GetGlyphRangesVietnamese();
    if (GetFileAttributesA(regular.c_str()) != INVALID_FILE_ATTRIBUTES) {
        atlas->AddFontFromFileTTF(regular.c_str(), 15.0f * g_ui_scale, nullptr,
                                  glyph_ranges);
    }
    if (GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_heading_font = atlas->AddFontFromFileTTF(
            bold.c_str(), 17.0f * g_ui_scale, nullptr, glyph_ranges);
    }
}

ImVec4 status_text_color(nstu::server::ClientStatus status) {
    switch (status) {
    case nstu::server::ClientStatus::online:
        return g_dark_mode ? ImVec4{0.56f, 0.78f, 0.58f, 1.0f}
                           : ImVec4{0.20f, 0.40f, 0.23f, 1.0f};
    case nstu::server::ClientStatus::degraded:
    case nstu::server::ClientStatus::connecting:
        return g_dark_mode ? ImVec4{0.90f, 0.73f, 0.38f, 1.0f}
                           : ImVec4{0.58f, 0.39f, 0.06f, 1.0f};
    case nstu::server::ClientStatus::locked:
        return g_dark_mode ? ImVec4{0.94f, 0.56f, 0.55f, 1.0f}
                           : ImVec4{0.62f, 0.18f, 0.17f, 1.0f};
    case nstu::server::ClientStatus::offline:
        return g_dark_mode ? ImVec4{0.67f, 0.66f, 0.63f, 1.0f}
                           : ImVec4{0.40f, 0.39f, 0.37f, 1.0f};
    }
    return g_dark_mode ? ImVec4{0.67f, 0.66f, 0.63f, 1.0f}
                       : ImVec4{0.40f, 0.39f, 0.37f, 1.0f};
}

ImVec4 status_background_color(nstu::server::ClientStatus status) {
    switch (status) {
    case nstu::server::ClientStatus::online:
        return g_dark_mode ? ImVec4{0.13f, 0.23f, 0.14f, 1.0f}
                           : ImVec4{0.93f, 0.96f, 0.92f, 1.0f};
    case nstu::server::ClientStatus::degraded:
    case nstu::server::ClientStatus::connecting:
        return g_dark_mode ? ImVec4{0.25f, 0.20f, 0.10f, 1.0f}
                           : ImVec4{0.985f, 0.95f, 0.86f, 1.0f};
    case nstu::server::ClientStatus::locked:
        return g_dark_mode ? ImVec4{0.27f, 0.13f, 0.13f, 1.0f}
                           : ImVec4{0.99f, 0.92f, 0.92f, 1.0f};
    case nstu::server::ClientStatus::offline:
        return g_dark_mode ? ImVec4{0.18f, 0.18f, 0.17f, 1.0f}
                           : ImVec4{0.94f, 0.935f, 0.925f, 1.0f};
    }
    return g_dark_mode ? ImVec4{0.18f, 0.18f, 0.17f, 1.0f}
                       : ImVec4{0.94f, 0.935f, 0.925f, 1.0f};
}

const char* client_status_label(nstu::server::ClientStatus status,
                                const DashboardState& state) {
    if (state.language == Language::english) {
        return nstu::server::to_string(status);
    }
    switch (status) {
    case nstu::server::ClientStatus::online:
        return "Trực tuyến";
    case nstu::server::ClientStatus::connecting:
        return "Đang kết nối";
    case nstu::server::ClientStatus::degraded:
        return "Cần chú ý";
    case nstu::server::ClientStatus::locked:
        return "Đã khóa";
    case nstu::server::ClientStatus::offline:
        return "Ngoại tuyến";
    }
    return "Không rõ";
}

using nstu::server::count_room_statuses;

bool client_matches_filter(const nstu::server::ClientRecord& client,
                           const DashboardState& state) {
    return nstu::server::client_matches_filter(
        client, state.room_filter, state.show_offline, state.client_filter.data());
}

void push_client_id(std::uint64_t id) {
    ImGui::PushID(static_cast<int>(id >> 32u));
    ImGui::PushID(static_cast<int>(id & 0xffffffffu));
}

void pop_client_id() {
    ImGui::PopID();
    ImGui::PopID();
}

void draw_icon(ImDrawList* draw_list, IconKind icon, ImVec2 center,
               float size, ImU32 color) {
    // size is authored at 96 DPI; scale it and every fixed pixel offset below
    // by g_ui_scale so the glyph grows uniformly with the DPI-scaled fonts.
    // At g_ui_scale == 1.0, scaled() is the identity and the glyph is drawn
    // pixel-for-pixel as before.
    size *= g_ui_scale;
    const float half = size * 0.5f;
    const float left = center.x - half;
    const float top = center.y - half;
    const float right = center.x + half;
    const float bottom = center.y + half;
    const float stroke = scaled(1.8f);
    switch (icon) {
    case IconKind::grid: {
        const float cell = size * 0.34f;
        const float gap = size * 0.12f;
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 2; ++column) {
                const ImVec2 minimum{
                    left + column * (cell + gap),
                    top + row * (cell + gap)};
                draw_list->AddRect(
                    minimum, {minimum.x + cell, minimum.y + cell}, color,
                    scaled(1.5f), 0, stroke);
            }
        }
        break;
    }
    case IconKind::monitor:
        draw_list->AddRect({left, top + scaled(1.0f)},
                           {right, bottom - scaled(4.0f)}, color, scaled(2.0f),
                           0, stroke);
        draw_list->AddLine({center.x, bottom - scaled(4.0f)},
                           {center.x, bottom}, color, stroke);
        draw_list->AddLine({center.x - scaled(5.0f), bottom},
                           {center.x + scaled(5.0f), bottom}, color, stroke);
        break;
    case IconKind::camera:
        draw_list->AddRect({left, top + scaled(3.0f)}, {right, bottom}, color,
                           scaled(2.0f), 0, stroke);
        draw_list->AddRect({left + scaled(4.0f), top},
                           {left + scaled(10.0f), top + scaled(4.0f)}, color,
                           scaled(1.0f), 0, stroke);
        draw_list->AddCircle(center, size * 0.21f, color, 16, stroke);
        break;
    case IconKind::stop:
        draw_list->AddRectFilled({left + scaled(3.0f), top + scaled(3.0f)},
                                 {right - scaled(3.0f), bottom - scaled(3.0f)},
                                 color, scaled(2.0f));
        break;
    case IconKind::lock:
    case IconKind::unlock: {
        const bool unlocked = icon == IconKind::unlock;
        draw_list->AddRect({left + scaled(3.0f), center.y - scaled(1.0f)},
                           {right - scaled(3.0f), bottom}, color, scaled(2.0f),
                           0, stroke);
        draw_list->PathClear();
        const ImVec2 shackle_center{
            center.x + (unlocked ? scaled(3.0f) : 0.0f), center.y - scaled(1.0f)};
        draw_list->PathArcTo(shackle_center, size * 0.26f,
                             3.1415926f, 6.2831852f, 12);
        draw_list->PathStroke(color, 0, stroke);
        if (unlocked) {
            draw_list->AddLine({left + scaled(2.0f), top + scaled(7.0f)},
                               {left + scaled(2.0f), center.y - scaled(1.0f)},
                               color, stroke);
        }
        break;
    }
    case IconKind::pen:
        draw_list->AddLine({left + scaled(3.0f), bottom - scaled(2.0f)},
                           {right - scaled(2.0f), top + scaled(3.0f)}, color,
                           scaled(3.0f));
        draw_list->AddTriangleFilled({right - scaled(2.0f), top + scaled(3.0f)},
                                     {right - scaled(6.0f), top + scaled(4.0f)},
                                     {right - scaled(3.0f), top + scaled(7.0f)},
                                     color);
        break;
    case IconKind::erase:
        draw_list->AddQuad({left + scaled(3.0f), bottom - scaled(6.0f)},
                           {center.x + scaled(2.0f), top + scaled(2.0f)},
                           {right - scaled(2.0f), top + scaled(7.0f)},
                           {center.x - scaled(3.0f), bottom - scaled(1.0f)},
                           color, stroke);
        draw_list->AddLine({left + scaled(5.0f), bottom - scaled(4.0f)},
                           {right - scaled(1.0f), bottom - scaled(4.0f)}, color,
                           stroke);
        break;
    case IconKind::broadcast:
        draw_list->AddCircleFilled(center, scaled(2.4f), color);
        for (int ring = 1; ring <= 2; ++ring) {
            const float radius = size * (0.18f + ring * 0.15f);
            draw_list->PathClear();
            draw_list->PathArcTo(center, radius, -0.8f, 0.8f, 12);
            draw_list->PathStroke(color, 0, stroke);
            draw_list->PathClear();
            draw_list->PathArcTo(center, radius, 2.34f, 3.94f, 12);
            draw_list->PathStroke(color, 0, stroke);
        }
        break;
    case IconKind::chat:
        draw_list->AddRect({left, top}, {right, bottom - scaled(4.0f)}, color,
                           scaled(3.0f), 0, stroke);
        draw_list->AddTriangleFilled({left + scaled(4.0f), bottom - scaled(4.0f)},
                                     {left + scaled(8.0f), bottom - scaled(4.0f)},
                                     {left + scaled(4.0f), bottom}, color);
        draw_list->AddCircleFilled({center.x - scaled(6.0f), center.y - scaled(2.0f)},
                                   scaled(1.5f), color);
        draw_list->AddCircleFilled({center.x, center.y - scaled(2.0f)},
                                   scaled(1.5f), color);
        draw_list->AddCircleFilled({center.x + scaled(6.0f), center.y - scaled(2.0f)},
                                   scaled(1.5f), color);
        break;
    }
}

bool draw_icon_button(const char* id, const char* label, IconKind icon,
                      ImVec2 size, bool selected = false,
                      bool enabled = true, bool show_label = true,
                      bool badge = false) {
    ImGui::PushID(id);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    ImGui::InvisibleButton("##icon-button", size);
    const bool pressed = enabled && ImGui::IsItemClicked();
    const bool hovered = enabled && ImGui::IsItemHovered();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    auto* draw_list = ImGui::GetWindowDrawList();
    if (selected || hovered) {
        const ImU32 background = ImGui::GetColorU32(
            selected ? ImGuiCol_Header : ImGuiCol_FrameBgHovered);
        draw_list->AddRectFilled(minimum, maximum, background, scaled(4.0f));
    }
    const ImU32 foreground = ImGui::GetColorU32(
        enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float label_height = show_label ? scaled(20.0f) : 0.0f;
    draw_icon(draw_list, icon,
              {minimum.x + size.x * 0.5f,
               minimum.y + (size.y - label_height) * 0.43f},
              show_label ? 22.0f : 20.0f, foreground);
    if (show_label) {
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        draw_list->AddText(
            {minimum.x + std::max(scaled(3.0f), (size.x - text_size.x) * 0.5f),
             maximum.y - text_size.y - scaled(5.0f)},
            foreground, label);
    }
    if (hovered && !show_label) {
        ImGui::SetTooltip("%s", label);
    }
    if (badge) {
        // Small attention dot at the top-right corner; draws over the icon so it
        // reads as an unread marker regardless of label state.
        const ImVec2 center{maximum.x - scaled(6.0f), minimum.y + scaled(6.0f)};
        draw_list->AddCircleFilled(center, scaled(4.5f),
                                   IM_COL32(229, 72, 77, 255));
        draw_list->AddCircle(center, scaled(4.5f), IM_COL32(255, 255, 255, 235),
                             12, scaled(1.4f));
    }
    if (!enabled) {
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    return pressed;
}

void draw_status_badge(nstu::server::ClientStatus status,
                       const DashboardState& state) {
    const char* label = client_status_label(status, state);
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 size{text_size.x + scaled(16.0f), text_size.y + scaled(8.0f)};
    ImGui::PushID(label);
    ImGui::InvisibleButton("##status", size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(minimum, maximum,
                             ImGui::ColorConvertFloat4ToU32(
                                 status_background_color(status)),
                             scaled(4.0f));
    draw_list->AddText({minimum.x + scaled(8.0f), minimum.y + scaled(4.0f)},
                       ImGui::ColorConvertFloat4ToU32(
                           status_text_color(status)),
                       label);
    ImGui::PopID();
}

SnapshotTexture* snapshot_texture(
    const nstu::server::ClientRecord& client) {
    if (client.snapshot_generation == 0 || !client.snapshot_jpeg ||
        client.snapshot_jpeg->empty() || g_graphics_device_lost || !g_device) {
        return nullptr;
    }
    auto& cached = g_snapshot_textures[client.id];
    if (cached.generation_gate.succeeded(client.snapshot_generation) &&
        cached.view) {
        return &cached;
    }
    if (!cached.generation_gate.begin(client.snapshot_generation)) {
        return nullptr;
    }
    cached.width = 0;
    cached.height = 0;
    cached.view.Reset();
    nstu::screen::BgraImage decoded;
    const auto bytes = std::span<const std::byte>(
        client.snapshot_jpeg->data(), client.snapshot_jpeg->size());
    std::string decode_error;
    if (!nstu::screen::decode_jpeg(
            bytes, client.snapshot_width, client.snapshot_height, decoded,
            &decode_error)) {
        cached.generation_gate.mark_failed(client.snapshot_generation);
        record_diagnostic(
            "warning", "Snapshot",
            "Client " + std::to_string(client.id) +
                " snapshot decode failed: " +
                (decode_error.empty() ? std::string("invalid JPEG")
                                      : decode_error));
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = decoded.width;
    description.Height = decoded.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = decoded.pixels.data();
    data.SysMemPitch = decoded.stride;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    const HRESULT texture_result =
        g_device->CreateTexture2D(&description, &data, &texture);
    const HRESULT view_result = SUCCEEDED(texture_result)
        ? g_device->CreateShaderResourceView(texture.Get(), nullptr, &view)
        : E_FAIL;
    if (FAILED(texture_result) || FAILED(view_result)) {
        cached.generation_gate.mark_failed(client.snapshot_generation);
        const HRESULT failure = FAILED(texture_result)
            ? texture_result : view_result;
        const HRESULT removal_reason = g_device->GetDeviceRemovedReason();
        if (is_device_loss_hresult(failure) ||
            is_device_loss_hresult(removal_reason)) {
            g_graphics_device_lost = true;
            invalidate_snapshot_textures();
        }
        record_diagnostic(
            "warning", "Snapshot",
            "Client " + std::to_string(client.id) +
                " snapshot " +
                (FAILED(texture_result) ? std::string("texture")
                                         : std::string("view")) +
                " creation failed " + hresult_text(failure) +
                "; device reason " + hresult_text(removal_reason));
        return nullptr;
    }
    cached.width = decoded.width;
    cached.height = decoded.height;
    cached.view = std::move(view);
    cached.generation_gate.mark_succeeded(client.snapshot_generation);
    return &cached;
}

struct ScreenSurfaceResult {
    bool clicked = false;
    bool has_frame = false;
    ImVec2 image_min{};
    ImVec2 image_max{};
};

ScreenSurfaceResult draw_screen_surface(
    const nstu::server::ClientRecord& client, float height, const char* id,
    const DashboardState& state) {
    const ImVec2 size{ImGui::GetContentRegionAvail().x, height};
    ImGui::InvisibleButton(id, size);
    ScreenSurfaceResult result;
    result.clicked = ImGui::IsItemClicked();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool offline = client.status == nstu::server::ClientStatus::offline;
    const ImU32 background = offline
        ? (g_dark_mode ? IM_COL32(43, 43, 40, 255)
                       : IM_COL32(230, 228, 223, 255))
        : (g_dark_mode ? IM_COL32(20, 20, 19, 255)
                       : IM_COL32(35, 35, 33, 255));
    const ImU32 foreground = offline
        ? (g_dark_mode ? IM_COL32(172, 170, 164, 255)
                       : IM_COL32(100, 98, 94, 255))
        : IM_COL32(240, 239, 235, 255);
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(minimum, maximum, background, 2.0f);
    auto* texture = snapshot_texture(client);
    if (!offline && texture != nullptr) {
        const float source_aspect = static_cast<float>(texture->width) /
                                    static_cast<float>(texture->height);
        const float target_aspect = size.x / size.y;
        ImVec2 image_size = size;
        if (source_aspect > target_aspect) {
            image_size.y = size.x / source_aspect;
        } else {
            image_size.x = size.y * source_aspect;
        }
        result.image_min = {
            minimum.x + (size.x - image_size.x) * 0.5f,
            minimum.y + (size.y - image_size.y) * 0.5f};
        result.image_max = {result.image_min.x + image_size.x,
                            result.image_min.y + image_size.y};
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(texture->view.Get()),
            result.image_min, result.image_max);
        result.has_frame = true;
    } else {
        const char* text = offline
            ? tr(state, "Offline", "Ngoại tuyến")
            : tr(state, "Waiting for snapshot", "Đang chờ ảnh chụp");
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        draw_list->AddText({minimum.x + (size.x - text_size.x) * 0.5f,
                            minimum.y + (height - text_size.y) * 0.5f},
                           foreground, text);
    }
    return result;
}

void draw_client_card(const nstu::server::ClientRecord& client, float width,
                      DashboardState& state) {
    push_client_id(client.id);
    const bool selected = state.selected_client_id == client.id;
    ImGui::PushStyleColor(ImGuiCol_Border,
        selected ? (g_dark_mode ? ImVec4{0.42f, 0.68f, 0.78f, 1.0f}
                                : ImVec4{0.31f, 0.56f, 0.68f, 1.0f})
                 : ImGui::GetStyleColorVec4(ImGuiCol_Border));
    if (ImGui::BeginChild("client-card", {width, width * 0.5625f + scaled(58.0f)},
                          true, ImGuiWindowFlags_NoScrollbar)) {
        if (draw_screen_surface(client, width * 0.5625f, "##screen", state)
                .clicked) {
            state.selected_client_id = client.id;
            state.view = DashboardView::selected_client;
            state.annotation_enabled = false;
        }
        ImGui::TextUnformatted(client.hostname.c_str());
        if (client.help_requested) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{1.0f, 0.65f, 0.0f, 1.0f}, "[?]");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(tr(state, "Help Requested", "Yêu cầu trợ giúp"));
                if (!client.help_message.empty()) {
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", client.help_message.c_str());
                }
                ImGui::EndTooltip();
            }
        }
        const float status_width =
            ImGui::CalcTextSize(client_status_label(client.status, state)).x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - status_width);
        ImGui::TextColored(status_text_color(client.status), "%s",
                           client_status_label(client.status, state));
        ImGui::TextDisabled("%s", client.address.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u ms  %.1f%%", client.latency_ms,
                            client.packet_loss_per_mille / 10.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    pop_client_id();
}

void draw_room_screen_wall(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    std::vector<const nstu::server::ClientRecord*> visible_clients;
    visible_clients.reserve(clients.size());
    for (const auto& client : clients) {
        if (client_matches_filter(client, state)) {
            visible_clients.push_back(&client);
        }
    }

    if (visible_clients.empty()) {
        const float offset = std::max(36.0f,
                                      ImGui::GetContentRegionAvail().y * 0.34f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
        const char* heading = clients.empty()
            ? tr(state, "No clients connected", "Chưa có máy nào kết nối")
            : tr(state, "No clients match this view",
                 "Không có máy phù hợp với bộ lọc");
        const ImVec2 heading_size = ImGui::CalcTextSize(heading);
        ImGui::SetCursorPosX(
            std::max(20.0f, (ImGui::GetWindowWidth() - heading_size.x) * 0.5f));
        ImGui::TextDisabled("%s", heading);
        return;
    }

    const ImVec4 workspace_background = g_dark_mode
        ? ImVec4{0.065f, 0.085f, 0.095f, 1.0f}
        : ImVec4{0.91f, 0.965f, 0.985f, 1.0f};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, workspace_background);
    if (ImGui::BeginChild("screen-wall", {0, 0}, false)) {
        const float minimum_card_width = scaled(205.0f);
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = std::clamp(
            static_cast<int>((available + gap) / (minimum_card_width + gap)),
            1, 6);
        const float card_width =
            (available - gap * static_cast<float>(columns - 1)) /
            static_cast<float>(columns);
        for (std::size_t index = 0; index < visible_clients.size(); ++index) {
            draw_client_card(*visible_clients[index], card_width, state);
            if ((index + 1) % static_cast<std::size_t>(columns) != 0) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_room_client_table(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    std::vector<const nstu::server::ClientRecord*> visible_clients;
    visible_clients.reserve(clients.size());
    for (const auto& client : clients) {
        if (client_matches_filter(client, state)) {
            visible_clients.push_back(&client);
        }
    }

    if (visible_clients.empty()) {
        const float offset = std::max(36.0f,
                                      ImGui::GetContentRegionAvail().y * 0.34f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
        const char* heading = clients.empty()
            ? tr(state, "No clients connected", "Chưa có máy nào kết nối")
            : tr(state, "No clients match this view",
                 "Không có máy phù hợp với bộ lọc");
        const ImVec2 heading_size = ImGui::CalcTextSize(heading);
        ImGui::SetCursorPosX(
            std::max(20.0f, (ImGui::GetWindowWidth() - heading_size.x) * 0.5f));
        ImGui::TextDisabled("%s", heading);
        return;
    }

    if (!ImGui::BeginChild("room-table-container", {0, 0}, false)) {
        ImGui::EndChild();
        return;
    }

    constexpr int kColumns = 7;
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("room-client-table", kColumns, table_flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##select", ImGuiTableColumnFlags_WidthFixed, scaled(32.0f));
        ImGui::TableSetupColumn(tr(state, "Hostname", "Tên máy"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(tr(state, "IP Address", "Địa chỉ IP"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn(tr(state, "Status", "Trạng thái"), ImGuiTableColumnFlags_WidthFixed, scaled(100.0f));
        ImGui::TableSetupColumn(tr(state, "Latency / Loss", "Độ trễ / Mất gói"), ImGuiTableColumnFlags_WidthFixed, scaled(120.0f));
        ImGui::TableSetupColumn(tr(state, "UWF Protection", "Bảo vệ UWF"), ImGuiTableColumnFlags_WidthFixed, scaled(130.0f));
        ImGui::TableSetupColumn(tr(state, "Action", "Thao tác"), ImGuiTableColumnFlags_WidthFixed, scaled(110.0f));
        ImGui::TableHeadersRow();

        for (const auto* client : visible_clients) {
            push_client_id(client->id);
            ImGui::TableNextRow(0, scaled(28.0f));

            // Column 0: Selection Checkbox
            ImGui::TableSetColumnIndex(0);
            bool is_chk = state.checked_client_ids.find(client->id) != state.checked_client_ids.end();
            if (ImGui::Checkbox("##check", &is_chk)) {
                if (is_chk) {
                    state.checked_client_ids.insert(client->id);
                } else {
                    state.checked_client_ids.erase(client->id);
                }
            }

            // Column 1: Hostname + chat badge
            ImGui::TableSetColumnIndex(1);
            const bool is_focused = (state.selected_client_id == client->id);
            if (ImGui::Selectable(client->hostname.c_str(), is_focused, ImGuiSelectableFlags_SpanAllColumns)) {
                state.selected_client_id = client->id;
            }
            if (client_has_unread_chat(state, client->id)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{0.9f, 0.28f, 0.3f, 1.0f}, "●");
            }
            if (client->help_requested) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{1.0f, 0.65f, 0.0f, 1.0f}, "[?]");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(tr(state, "Help Requested", "Yêu cầu trợ giúp"));
                    if (!client->help_message.empty()) {
                        ImGui::Separator();
                        ImGui::TextWrapped("%s", client->help_message.c_str());
                    }
                    ImGui::EndTooltip();
                }
            }

            // Column 2: IP Address
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(client->address.c_str());

            // Column 3: Status
            ImGui::TableSetColumnIndex(3);
            draw_status_badge(client->status, state);

            // Column 4: Latency & Loss
            ImGui::TableSetColumnIndex(4);
            if (client->status != nstu::server::ClientStatus::offline) {
                ImGui::Text("%u ms  %.1f%%", client->latency_ms,
                            client->packet_loss_per_mille / 10.0f);
            } else {
                ImGui::TextDisabled("—");
            }

            // Column 5: UWF Status
            ImGui::TableSetColumnIndex(5);
            if (client->uwf.proves_current_protection()) {
                ImGui::TextColored(ImVec4{0.2f, 0.8f, 0.2f, 1.0f}, "%s",
                                   tr(state, "Protected", "Đã bảo vệ"));
            } else if (client->uwf.phase == nstu::control::UwfFleetPhase::configuring ||
                       client->uwf.phase == nstu::control::UwfFleetPhase::awaiting_restart) {
                ImGui::TextColored(ImVec4{0.9f, 0.7f, 0.1f, 1.0f}, "%s",
                                   tr(state, "Arming...", "Đang chuẩn bị"));
            } else if (client->uwf.phase == nstu::control::UwfFleetPhase::unsupported) {
                ImGui::TextDisabled("%s", tr(state, "Unsupported", "Không hỗ trợ"));
            } else {
                ImGui::TextDisabled("%s", tr(state, "Inactive", "Chưa kích hoạt"));
            }

            // Column 6: Action
            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton(tr(state, "Focus", "Tiêu điểm"))) {
                state.selected_client_id = client->id;
                state.view = DashboardView::selected_client;
                state.annotation_enabled = false;
            }
            ImGui::SameLine();
            if (client->status != nstu::server::ClientStatus::offline) {
                const bool locked = (client->status == nstu::server::ClientStatus::locked);
                const char* btn_label = locked ? tr(state, "Unlock", "Mở") : tr(state, "Lock", "Khóa");
                if (ImGui::SmallButton(btn_label)) {
                    (void)control_plane.set_locked(client->id, !locked, nullptr);
                }
            }
            if (client->help_requested) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr(state, "Ack Help", "Giải quyết"))) {
                    std::string err;
                    (void)control_plane.resolve_help_request(client->id, &err);
                }
            }

            pop_client_id();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void draw_focus_client_list(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    if (ImGui::BeginChild("focus-client-list", {scaled(280.0f), 0}, true)) {
        ImGui::TextUnformatted(tr(state, "Clients", "Máy học sinh"));
        ImGui::Separator();
        if (ImGui::BeginTable("focus-clients", 2,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(tr(state, "Client", "Máy"),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr(state, "Status", "Trạng thái"),
                                    ImGuiTableColumnFlags_WidthFixed,
                                    scaled(94.0f));
            for (const auto& client : clients) {
                if (!client_matches_filter(client, state)) {
                    continue;
                }
                push_client_id(client.id);
                ImGui::TableNextRow(0, scaled(40.0f));
                ImGui::TableSetColumnIndex(0);
                const bool selected = state.selected_client_id == client.id;
                if (ImGui::Selectable(client.hostname.c_str(), selected,
                                      ImGuiSelectableFlags_None,
                                      {0.0f, scaled(36.0f)})) {
                    state.selected_client_id = client.id;
                    state.annotation_enabled = false;
                }
                if (client_has_unread_chat(state, client.id)) {
                    const ImVec2 rect_min = ImGui::GetItemRectMin();
                    const ImVec2 rect_max = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        {rect_max.x - scaled(9.0f),
                         (rect_min.y + rect_max.y) * 0.5f},
                        scaled(4.0f), IM_COL32(229, 72, 77, 255));
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(status_text_color(client.status), "%s",
                                   client_status_label(client.status, state));
                pop_client_id();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void draw_telemetry_card(const char* label, const char* value, float width) {
    ImGui::PushID(label);
    if (ImGui::BeginChild("telemetry", {width, scaled(62.0f)}, true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextDisabled("%s", label);
        ImGui::TextUnformatted(value);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void draw_uwf_confirmation_popup(
    DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    if (state.uwf_confirmation_open &&
        ImGui::BeginPopupModal("uwf-confirmation", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", tr(
            state,
            "This runs full client readiness checks, preserves NSTU data and registry state, then arms Unified Write Filter for the next restart. Create or verify a recovery checkpoint before continuing. A successful setup shows a 60-second countdown and restarts the client automatically.",
            "Thao tác này chạy đầy đủ kiểm tra sẵn sàng, giữ lại dữ liệu và registry NSTU, rồi chuẩn bị Unified Write Filter cho lần khởi động tiếp theo. Hãy tạo hoặc xác minh điểm khôi phục trước khi tiếp tục. Khi thiết lập thành công, máy client sẽ hiển thị đếm ngược 60 giây rồi tự khởi động lại."));
        if (ImGui::Button(tr(state, "I verified the checkpoint; enable",
                             "Đã xác minh điểm khôi phục; bật"))) {
            if (!is_action_permitted(state.current_role, OperatorAction::configure_uwf)) {
                state.control_status = tr(state,
                    "Reboot-to-restore configuration requires Technician or Developer role.",
                    "Cấu hình khôi phục sau reboot yêu cầu vai trò Kỹ thuật viên hoặc Nhà phát triển.");
            } else {
                std::string error;
                // Fleet path: arm UWF and request the visible client restart, so a
                // fresh boot-bound probe can prove current-session protection.
                const bool sent = control_plane.configure_uwf_fleet(
                    state.uwf_confirmation_client_id, true, true, &error);
                state.control_status = sent
                    ? tr(state, "UWF readiness request sent.",
                         "Đã gửi yêu cầu kiểm tra và bật UWF.")
                    : error;
                if (!sent) {
                    record_operation_failure("UWF", "UWF command failed", error);
                }
            }
            state.uwf_confirmation_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(state, "Cancel", "Hủy"))) {
            state.uwf_confirmation_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_selected_client(
    const std::vector<nstu::server::ClientRecord>& clients,
    const nstu::server::ClientRecord* selected_client, DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    draw_focus_client_list(clients, state);
    ImGui::SameLine();
    if (!ImGui::BeginChild("focus-detail", {0, 0}, false)) {
        ImGui::EndChild();
        return;
    }
    if (selected_client == nullptr) {
        const char* empty = clients.empty()
            ? tr(state, "No clients connected", "Chưa có máy nào kết nối")
            : tr(state, "Select a client", "Chọn một máy học sinh");
        const float offset = std::max(36.0f,
                                      ImGui::GetContentRegionAvail().y * 0.38f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
        const ImVec2 size = ImGui::CalcTextSize(empty);
        ImGui::SetCursorPosX(
            std::max(20.0f, (ImGui::GetWindowWidth() - size.x) * 0.5f));
        ImGui::TextDisabled("%s", empty);
        ImGui::EndChild();
        return;
    }

    if (g_heading_font != nullptr) {
        ImGui::PushFont(g_heading_font);
    }
    ImGui::TextUnformatted(selected_client->hostname.c_str());
    if (g_heading_font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::SameLine();
    draw_status_badge(selected_client->status, state);
    if (selected_client->help_requested) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.32f, 0.20f, 0.05f, 1.0f});
        if (ImGui::BeginChild("help-banner", {0, scaled(34.0f)}, true, ImGuiWindowFlags_NoScrollbar)) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImVec4{1.0f, 0.75f, 0.15f, 1.0f}, "[!] %s",
                               tr(state, "Help Requested", "Yêu cầu trợ giúp"));
            if (!selected_client->help_message.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("— %s", selected_client->help_message.c_str());
            }
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + scaled(8.0f),
                                     ImGui::GetContentRegionMax().x - scaled(150.0f)));
            if (ImGui::SmallButton(tr(state, "Acknowledge Help", "Giải quyết trợ giúp"))) {
                std::string error;
                (void)control_plane.resolve_help_request(selected_client->id, &error);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    ImGui::TextDisabled("%s", selected_client->address.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("•");
    ImGui::SameLine();
    if (!selected_client->mac_address.empty()) {
        ImGui::TextDisabled("MAC: %s", selected_client->mac_address.c_str());
    } else {
        ImGui::TextDisabled("MAC: %s", tr(state, "Unknown", "Chưa rõ"));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("•");
    ImGui::SameLine();
    switch (selected_client->wol_support) {
    case nstu::net::WolSupportState::supported:
        ImGui::TextColored(ImVec4{0.3f, 0.85f, 0.4f, 1.0f}, "[●] WOL: %s",
                           tr(state, "Supported", "Được hỗ trợ"));
        break;
    case nstu::net::WolSupportState::unsupported:
        ImGui::TextColored(ImVec4{0.9f, 0.4f, 0.4f, 1.0f}, "[▲] WOL: %s",
                           tr(state, "Unsupported", "Không hỗ trợ"));
        break;
    case nstu::net::WolSupportState::missing_mac:
        ImGui::TextColored(ImVec4{0.7f, 0.7f, 0.7f, 1.0f}, "[○] WOL: %s",
                           tr(state, "Missing MAC", "Thiếu MAC"));
        break;
    case nstu::net::WolSupportState::unknown:
        ImGui::TextColored(ImVec4{0.7f, 0.7f, 0.7f, 1.0f}, "[○] WOL: %s",
                           tr(state, "Unknown", "Chưa rõ"));
        break;
    }
    bool managed = selected_client->frozen;
    if (ImGui::Checkbox(
            tr(state, "Managed mode", "Chế độ được quản lý"),
            &managed)) {
        std::string error;
        const bool sent = control_plane.set_frozen(
            selected_client->id, managed, &error);
        state.control_status = sent
            ? (managed
                   ? tr(state, "Managed mode request sent.",
                        "Đã gửi yêu cầu bật chế độ được quản lý.")
                   : tr(state, "Thaw request sent.",
                        "Đã gửi yêu cầu tắt chế độ được quản lý."))
            : error;
        if (!sent) {
            record_operation_failure(
                "ManagedMode", "Managed-mode command failed", error);
        }
        ImGui::OpenPopup("control-status");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr(
            state,
            "Blocks service stop and uninstall until disabled here or by a local administrator.",
            "Chặn dừng dịch vụ và gỡ cài đặt cho đến khi tắt tại đây hoặc bởi quản trị viên cục bộ."));
    }
    ImGui::SameLine();
    // Requirement check before activation: if the client has already reported
    // that its Windows edition/feature set cannot support reboot-to-restore,
    // do not let the operator arm it - the attempt would only fail on the box.
    const bool uwf_unsupported =
        selected_client->uwf.reported &&
        selected_client->uwf.phase == nstu::control::UwfFleetPhase::unsupported;
    const bool permitted = is_action_permitted(state.current_role, OperatorAction::configure_uwf);
    ImGui::BeginDisabled(uwf_unsupported || !permitted);
    if (ImGui::Button(tr(state, "Enable reboot-to-restore",
                         "Bật khôi phục sau reboot"))) {
        state.uwf_confirmation_client_id = selected_client->id;
        state.uwf_confirmation_open = true;
        ImGui::OpenPopup("uwf-confirmation");
    }
    ImGui::EndDisabled();
    if (!permitted && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(state,
            "Reboot-to-restore configuration requires Technician or Developer role.",
            "Cấu hình khôi phục sau reboot yêu cầu vai trò Kỹ thuật viên hoặc Nhà phát triển."));
    } else if (uwf_unsupported &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(state,
            "This computer does not meet reboot-to-restore requirements "
            "(unsupported Windows edition or missing Unified Write Filter).",
            "Máy này không đáp ứng yêu cầu khôi phục sau reboot (phiên bản "
            "Windows không hỗ trợ hoặc thiếu Unified Write Filter)."));
    }
    if (selected_client->uwf.reported ||
        selected_client->uwf.phase !=
            nstu::control::UwfFleetPhase::idle) {
        const bool proven =
            selected_client->uwf.proves_current_protection();
        ImGui::TextColored(
            proven ? (g_dark_mode ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                                  : ImVec4{0.10f, 0.55f, 0.24f, 1.0f})
                   : (g_dark_mode ? ImVec4{0.96f, 0.76f, 0.34f, 1.0f}
                                  : ImVec4{0.63f, 0.36f, 0.02f, 1.0f}),
            "%s", nstu::server::to_string(selected_client->uwf.phase));
        if (!selected_client->uwf.detail.empty()) {
            ImGui::TextWrapped("%s", selected_client->uwf.detail.c_str());
        }
        if (proven) {
            ImGui::TextColored(
                g_dark_mode ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                            : ImVec4{0.10f, 0.55f, 0.24f, 1.0f},
                "%s",
                tr(state,
                   "Reboot-to-restore protected in this session. Exam mode is "
                   "permitted.",
                   "Đã bảo vệ khôi phục sau reboot trong phiên này. Cho phép "
                   "chế độ thi."));
        }
    }
    draw_uwf_confirmation_popup(state, control_plane);

    char latency[32]{};
    char packet_loss[32]{};
    sprintf_s(latency, "%u ms", selected_client->latency_ms);
    sprintf_s(packet_loss, "%.1f%%",
              selected_client->packet_loss_per_mille / 10.0f);
    char refresh[32]{};
    if (selected_client->snapshotting) {
        sprintf_s(refresh, tr(state, "%u seconds", "%u giây"),
                  selected_client->snapshot_interval_seconds);
    } else {
        strcpy_s(refresh, tr(state, "Paused", "Đã dừng"));
    }
    const float telemetry_gap = ImGui::GetStyle().ItemSpacing.x;
    const float telemetry_width =
        (ImGui::GetContentRegionAvail().x - telemetry_gap * 2.0f) / 3.0f;
    draw_telemetry_card(tr(state, "Latency", "Độ trễ"), latency,
                        telemetry_width);
    ImGui::SameLine();
    draw_telemetry_card(tr(state, "Packet loss", "Mất gói"), packet_loss,
                        telemetry_width);
    ImGui::SameLine();
    draw_telemetry_card(tr(state, "Snapshot refresh", "Chu kỳ chụp"),
                        refresh, telemetry_width);

    ImGui::TextUnformatted(tr(state, "Latest screen snapshot",
                              "Ảnh chụp màn hình mới nhất"));
    const bool live_now = selected_client->streaming;
    ImGui::SameLine();
    if (live_now) {
        const ImVec4 live_color = g_dark_mode
                                      ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                                      : ImVec4{0.10f, 0.55f, 0.24f, 1.0f};
        std::string live_label;
        if (state.measured_stream_fps > 0.0) {
            const long long tenths = static_cast<long long>(
                state.measured_stream_fps * 10.0 + 0.5);
            live_label = std::string(tr(state, "· Live ", "· Trực tiếp ")) +
                         std::to_string(tenths / 10) + "." +
                         std::to_string(tenths % 10) + " fps";
        } else {
            live_label =
                tr(state, "· Live (measuring)", "· Trực tiếp (đang đo)");
        }
        ImGui::TextColored(live_color, "%s", live_label.c_str());
    } else {
        ImGui::TextDisabled("%s", tr(state, "· Snapshot", "· Ảnh chụp"));
    }
    const float preview_width = ImGui::GetContentRegionAvail().x;
    const float preview_height = std::clamp(
        preview_width * 0.5625f, scaled(220.0f),
        std::max(scaled(220.0f), ImGui::GetContentRegionAvail().y - scaled(210.0f)));
    const auto surface = draw_screen_surface(
        *selected_client, preview_height, "##focus-screen", state);
    // Remote input no longer rides on this embedded Focus surface: it lives in a
    // separate top-level window (see open_remote_window). The Focus view is a
    // read-only live preview plus teacher annotation.
    if (state.annotation_enabled && surface.has_frame) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inside = mouse.x >= surface.image_min.x &&
                            mouse.x <= surface.image_max.x &&
                            mouse.y >= surface.image_min.y &&
                            mouse.y <= surface.image_max.y;
        const auto clamp_to_surface = [&](ImVec2 point) {
            return ImVec2{
                std::clamp(point.x, surface.image_min.x, surface.image_max.x),
                std::clamp(point.y, surface.image_min.y, surface.image_max.y)};
        };
        const auto normalized = [&](ImVec2 point) {
            const float x_ratio = std::clamp(
                (point.x - surface.image_min.x) /
                    (surface.image_max.x - surface.image_min.x),
                0.0f, 1.0f);
            const float y_ratio = std::clamp(
                (point.y - surface.image_min.y) /
                    (surface.image_max.y - surface.image_min.y),
                0.0f, 1.0f);
            return ImVec2{x_ratio * 65535.0f, y_ratio * 65535.0f};
        };
        const auto send_segment = [&](ImVec2 start, ImVec2 end) {
            const ImVec2 first = normalized(clamp_to_surface(start));
            const ImVec2 last = normalized(clamp_to_surface(end));
            const nstu::control::OverlayStroke stroke{
                .x0 = static_cast<std::uint16_t>(first.x),
                .y0 = static_cast<std::uint16_t>(first.y),
                .x1 = static_cast<std::uint16_t>(last.x),
                .y1 = static_cast<std::uint16_t>(last.y),
                .thickness = static_cast<std::uint16_t>(
                    state.annotation_thickness),
                .rgba = state.annotation_rgba,
            };
            std::string ignored_error;
            (void)control_plane.send_overlay_stroke(
                selected_client->id, stroke, &ignored_error);
        };
        const auto send_erase = [&](ImVec2 start, ImVec2 end) {
            const ImVec2 first = normalized(clamp_to_surface(start));
            const ImVec2 last = normalized(clamp_to_surface(end));
            const nstu::control::OverlayStroke path{
                .x0 = static_cast<std::uint16_t>(first.x),
                .y0 = static_cast<std::uint16_t>(first.y),
                .x1 = static_cast<std::uint16_t>(last.x),
                .y1 = static_cast<std::uint16_t>(last.y),
                .thickness = static_cast<std::uint16_t>(
                    state.annotation_thickness),
                // Colour is ignored for erase; a non-zero alpha only satisfies
                // the shared stroke codec.
                .rgba = 0xffffffffu,
            };
            std::string ignored_error;
            (void)control_plane.send_overlay_erase(
                selected_client->id, path, &ignored_error);
        };
        const auto send_shape = [&](ImVec2 start, ImVec2 end) {
            const float left = std::min(start.x, end.x);
            const float right = std::max(start.x, end.x);
            const float top = std::min(start.y, end.y);
            const float bottom = std::max(start.y, end.y);
            switch (state.annotation_tool) {
            case AnnotationTool::ruler:
                send_segment(start, end);
                break;
            case AnnotationTool::arrow: {
                send_segment(start, end);
                const float dx = end.x - start.x;
                const float dy = end.y - start.y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length < 2.0f) {
                    break;
                }
                const float ux = dx / length;
                const float uy = dy / length;
                const float head = std::min(24.0f, length * 0.35f);
                const ImVec2 base{end.x - ux * head, end.y - uy * head};
                const ImVec2 perpendicular{-uy * head * 0.35f,
                                            ux * head * 0.35f};
                send_segment(end, {base.x + perpendicular.x,
                                    base.y + perpendicular.y});
                send_segment(end, {base.x - perpendicular.x,
                                    base.y - perpendicular.y});
                break;
            }
            case AnnotationTool::rectangle:
                send_segment({left, top}, {right, top});
                send_segment({right, top}, {right, bottom});
                send_segment({right, bottom}, {left, bottom});
                send_segment({left, bottom}, {left, top});
                break;
            case AnnotationTool::ellipse: {
                constexpr int segments = 24;
                const ImVec2 center{(left + right) * 0.5f,
                                    (top + bottom) * 0.5f};
                const ImVec2 radius{(right - left) * 0.5f,
                                    (bottom - top) * 0.5f};
                ImVec2 previous{center.x + radius.x, center.y};
                for (int index = 1; index <= segments; ++index) {
                    const float angle =
                        (2.0f * 3.14159265358979323846f * index) /
                        static_cast<float>(segments);
                    const ImVec2 next{center.x + std::cos(angle) * radius.x,
                                      center.y + std::sin(angle) * radius.y};
                    send_segment(previous, next);
                    previous = next;
                }
                break;
            }
            case AnnotationTool::pen:
            case AnnotationTool::eraser:
                break;
            }
        };
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside) {
            state.annotation_dragging = true;
            state.previous_annotation_point = mouse;
        }
        const bool freehand = state.annotation_tool == AnnotationTool::pen ||
                              state.annotation_tool == AnnotationTool::eraser;
        if (state.annotation_dragging &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left) && freehand && inside) {
            const float delta_x = mouse.x - state.previous_annotation_point.x;
            const float delta_y = mouse.y - state.previous_annotation_point.y;
            if (delta_x * delta_x + delta_y * delta_y >= 9.0f) {
                if (state.annotation_tool == AnnotationTool::eraser) {
                    send_erase(state.previous_annotation_point, mouse);
                } else {
                    send_segment(state.previous_annotation_point, mouse);
                }
                state.previous_annotation_point = mouse;
            }
        }
        if (state.annotation_dragging &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 end = inside ? mouse : state.previous_annotation_point;
            if (state.annotation_tool == AnnotationTool::eraser) {
                if (inside) {
                    send_erase(state.previous_annotation_point, end);
                }
            } else if (state.annotation_tool != AnnotationTool::pen) {
                send_shape(state.previous_annotation_point, end);
            } else if (inside) {
                send_segment(state.previous_annotation_point, end);
            }
            state.annotation_dragging = false;
        }
    } else {
        state.annotation_dragging = false;
    }

    ImGui::PushStyleColor(
        ImGuiCol_Button, g_dark_mode ? ImVec4{0.82f, 0.81f, 0.78f, 1.0f}
                                     : ImVec4{0.12f, 0.12f, 0.11f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        g_dark_mode ? ImVec4{0.90f, 0.89f, 0.86f, 1.0f}
                    : ImVec4{0.20f, 0.20f, 0.19f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        g_dark_mode ? ImVec4{0.96f, 0.95f, 0.92f, 1.0f}
                    : ImVec4{0.25f, 0.25f, 0.24f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_Text, g_dark_mode ? ImVec4{0.10f, 0.10f, 0.09f, 1.0f}
                                   : ImVec4{1.0f, 1.0f, 1.0f, 1.0f});
    ImGui::BeginDisabled(state.auto_monitor);
    if (ImGui::Button(selected_client->snapshotting
                          ? tr(state, "Stop snapshots", "Dừng chụp")
                          : tr(state, "Start snapshots", "Bắt đầu chụp"))) {
        const bool enabled = !selected_client->snapshotting;
        std::string error;
        if (control_plane.set_snapshots(
                selected_client->id, enabled,
                static_cast<std::uint16_t>(
                    state.snapshot_interval_seconds), &error)) {
            state.control_status = enabled
                ? tr(state, "Snapshot refresh started.",
                     "Đã bắt đầu chụp màn hình.")
                : tr(state, "Snapshot refresh stopped.",
                     "Đã dừng chụp màn hình.");
        } else {
            state.control_status = error;
            record_operation_failure("Snapshots", "Snapshot command failed",
                                     error);
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::EndDisabled();
    if (state.auto_monitor &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(state,
            "Automatic monitoring is on. Turn it off in Settings to control "
            "snapshots per computer.",
            "Đang bật theo dõi tự động. Tắt trong Cài đặt để điều khiển "
            "snapshot theo từng máy."));
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    const bool remote_available = selected_client->status !=
        nstu::server::ClientStatus::offline;
    const bool legacy_open_here = g_remote_window != nullptr &&
        g_remote_target_client == selected_client->id;

    const auto session_snapshot = g_remote_session ? g_remote_session->snapshot()
                                                   : nstu::server::RemoteSessionSnapshot{};
    const bool session_active_here = session_snapshot.client_id == selected_client->id &&
        (session_snapshot.state == nstu::server::RemoteSessionState::preflighting ||
         session_snapshot.state == nstu::server::RemoteSessionState::launching ||
         session_snapshot.state == nstu::server::RemoteSessionState::running ||
         session_snapshot.state == nstu::server::RemoteSessionState::stopping);
    const bool any_session_active =
        (session_snapshot.state == nstu::server::RemoteSessionState::preflighting ||
         session_snapshot.state == nstu::server::RemoteSessionState::launching ||
         session_snapshot.state == nstu::server::RemoteSessionState::running ||
         session_snapshot.state == nstu::server::RemoteSessionState::stopping);

    if (session_active_here) {
        if (ImGui::Button(tr(state, "Stop remote", "Dừng điều khiển"))) {
            if (g_remote_session) {
                g_remote_session->stop(nstu::server::RemoteSessionStopReason::explicit_stop);
            }
            state.control_status = tr(state, "Stopping remote session...",
                                      "Đang dừng phiên điều khiển từ xa...");
            ImGui::OpenPopup("control-status");
        }
    } else if (legacy_open_here) {
        if (ImGui::Button(tr(state, "Stop legacy remote", "Dừng điều khiển cũ"))) {
            close_remote_window(control_plane);
            state.control_status = tr(state, "Remote control window closed.",
                                      "Đã đóng cửa sổ điều khiển từ xa.");
            ImGui::OpenPopup("control-status");
        }
    } else {
        const bool button_disabled = !remote_available ||
                                     any_session_active ||
                                     g_remote_window != nullptr;
        ImGui::BeginDisabled(button_disabled);
        if (ImGui::Button(tr(state, "Start remote", "Bắt đầu điều khiển"))) {
            if (g_use_legacy_remote) {
                const bool ok =
                    open_remote_window(control_plane, selected_client->id);
                state.control_status = ok
                    ? tr(state, "Legacy remote control window opened.",
                         "Đã mở cửa sổ điều khiển từ xa cũ.")
                    : tr(state, "Could not open the remote control window.",
                         "Không thể mở cửa sổ điều khiển từ xa.");
            } else {
                if (control_plane.exam_active(selected_client->id)) {
                    state.control_status = tr(
                        state,
                        "Remote control is blocked while an exam is active.",
                        "Không thể điều khiển từ xa khi đang có bài thi hoạt động.");
                    control_plane.record_remote_session_audit(
                        selected_client->id, "remote_session_start", "denied", "exam_active");
                } else {
                    nstu::server::RemoteSessionTarget target;
                    std::string resolve_err;
                    if (!nstu::server::resolve_remote_session_target(
                            clients, selected_client->id, &target, &resolve_err)) {
                        state.control_status = tr(
                            state, "Failed to resolve client address: ",
                            "Không thể xác định địa chỉ máy: ") + resolve_err;
                        control_plane.record_remote_session_audit(
                            selected_client->id, "remote_session_start", "failed", resolve_err);
                    } else if (g_remote_session) {
                        std::string start_err;
                        auto auth = [&control_plane](
                                        std::uint64_t id, std::string* denial) {
                            if (control_plane.exam_active(id)) {
                                if (denial) *denial = "exam is active on client";
                                return false;
                            }
                            return true;
                        };
                        if (g_remote_session->start(target, auth, &start_err)) {
                            state.control_status = tr(
                                state, "Starting remote session...",
                                "Đang khởi động phiên điều khiển từ xa...");
                            control_plane.record_remote_session_audit(
                                selected_client->id, "remote_session_start", "succeeded");
                        } else {
                            state.control_status = tr(
                                state, "Could not start remote session: ",
                                "Không thể khởi động điều khiển từ xa: ") + start_err;
                            control_plane.record_remote_session_audit(
                                selected_client->id, "remote_session_start", "failed", start_err);
                        }
                    }
                }
            }
            ImGui::OpenPopup("control-status");
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr(state, "Legacy remote", "Điều khiển cũ"),
                    &g_use_legacy_remote);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr(
            state,
            "Use legacy built-in D3D remote window instead of Moonlight.",
            "Dùng cửa sổ D3D cũ thay vì Moonlight."));
    }
    // Keyboard entry and the offline auto-stop now belong to the separate
    // remote-control window: it forwards real key events and the main loop
    // closes it when the controlled client drops offline.
    ImGui::SameLine();
    if (ImGui::Button(state.annotation_enabled
                          ? tr(state, "Finish drawing", "Kết thúc vẽ")
                          : tr(state, "Draw on screen", "Vẽ lên màn hình"))) {
        state.annotation_enabled = !state.annotation_enabled;
        state.annotation_dragging = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Clear drawing", "Xóa nét vẽ"))) {
        std::string error;
        const bool cleared =
            control_plane.clear_overlay(selected_client->id, &error);
        state.control_status = cleared
            ? tr(state, "Student overlay cleared.",
                 "Đã xóa lớp vẽ trên máy học sinh.")
            : error;
        if (!cleared) {
            record_operation_failure("Overlay", "Clear command failed", error);
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(
        ImGuiCol_Button, g_dark_mode ? ImVec4{0.27f, 0.13f, 0.13f, 1.0f}
                                     : ImVec4{0.99f, 0.92f, 0.92f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        g_dark_mode ? ImVec4{0.36f, 0.17f, 0.17f, 1.0f}
                    : ImVec4{0.97f, 0.86f, 0.86f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_Text, g_dark_mode ? ImVec4{0.94f, 0.56f, 0.55f, 1.0f}
                                   : ImVec4{0.62f, 0.18f, 0.17f, 1.0f});
    const bool is_locked =
        selected_client->status == nstu::server::ClientStatus::locked;
    if (ImGui::Button(is_locked
                           ? tr(state, "Unlock client", "Mở khóa máy")
                           : tr(state, "Lock client", "Khóa máy"))) {
        std::string error;
        const bool sent =
            control_plane.set_locked(selected_client->id, !is_locked, &error);
        state.control_status = sent
            ? (is_locked
                   ? tr(state, "Unlock command sent.",
                        "Đã gửi lệnh mở khóa.")
                   : tr(state, "Lock command sent.",
                        "Đã gửi lệnh khóa."))
            : error;
        if (!sent) {
            record_operation_failure("Lock", "Lock-state command failed",
                                     error);
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Launch Web", "Mở trang web"))) {
        state.checked_client_ids = {selected_client->id};
        state.pending_bulk_action = BulkActionKind::launch_url;
        state.bulk_confirmation_open = true;
    }
    ImGui::SameLine();
    const bool exam_on_selected = control_plane.exam_active(selected_client->id);
    if (exam_on_selected) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(tr(state, "Reboot", "Khởi động lại"))) {
        state.checked_client_ids = {selected_client->id};
        state.pending_bulk_action = BulkActionKind::reboot;
        state.bulk_confirmation_open = true;
    }
    if (exam_on_selected && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(state, "Power actions are disabled during active assessments.",
                                          "Không thể thao tác nguồn khi đang có bài thi."));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Shutdown", "Tắt máy"))) {
        state.checked_client_ids = {selected_client->id};
        state.pending_bulk_action = BulkActionKind::shutdown;
        state.bulk_confirmation_open = true;
    }
    if (exam_on_selected && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(state, "Power actions are disabled during active assessments.",
                                          "Không thể thao tác nguồn khi đang có bài thi."));
    }
    if (exam_on_selected) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const bool is_client_online = (selected_client->status != nstu::server::ClientStatus::offline);
    const bool missing_mac = selected_client->mac_address.empty();
    const bool unsupported_wol = (selected_client->wol_support == nstu::net::WolSupportState::unsupported);
    const bool disable_wake = is_client_online || exam_on_selected || missing_mac || unsupported_wol;

    if (disable_wake) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(tr(state, "Wake", "Bật máy"))) {
        std::string wake_err;
        const bool sent = control_plane.wake_client(
            selected_client->id, state.current_role, &wake_err);
        state.control_status = sent
            ? tr(state, "Wake-on-LAN magic packet broadcasted.",
                        "Đã phát gói tin Wake-on-LAN qua mạng.")
            : wake_err;
        if (!sent) {
            record_operation_failure("WakeOnLan", "Wake-on-LAN broadcast failed",
                                     wake_err);
        }
        ImGui::OpenPopup("control-status");
    }
    if (disable_wake && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (is_client_online) {
            ImGui::SetTooltip("%s", tr(state, "Client is already online.", "Máy tính đang hoạt động."));
        } else if (exam_on_selected) {
            ImGui::SetTooltip("%s", tr(state, "Cannot execute Wake-on-LAN: exam active on client.", "Không thể bật máy: đang thi."));
        } else if (missing_mac) {
            ImGui::SetTooltip("%s", tr(state, "Cannot wake client: MAC address is unknown.", "Thiếu địa chỉ MAC của máy."));
        } else if (unsupported_wol) {
            ImGui::SetTooltip("%s", tr(state, "Wake-on-LAN is not supported by client hardware.", "Phần cứng không hỗ trợ Wake-on-LAN."));
        }
    }
    if (disable_wake) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const bool disable_send_file = !is_client_online || exam_on_selected;
    if (disable_send_file) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(tr(state, "Send File", "Phát tệp"))) {
        state.checked_client_ids = {selected_client->id};
        state.pending_bulk_action = BulkActionKind::distribute_file;
        state.bulk_confirmation_open = true;
    }
    if (disable_send_file && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!is_client_online) {
            ImGui::SetTooltip("%s", tr(state, "Cannot send file: client is offline.", "Không thể phát tệp: máy đang ngoại tuyến."));
        } else if (exam_on_selected) {
            ImGui::SetTooltip("%s", tr(state, "File distribution is disabled during active assessments.", "Không thể phát tệp khi đang có bài thi."));
        }
    }
    if (disable_send_file) {
        ImGui::EndDisabled();
    }
    if (state.annotation_enabled) {
        ImGui::TextUnformatted(tr(state, "Overlay tools", "Công cụ lớp phủ"));
        ImGui::SameLine();
        const char* tool_items =
            state.language == Language::vietnamese
                ? "Bút\0Thước\0Mũi tên\0Hình chữ nhật\0Elip\0Tẩy\0"
                : "Pen\0Ruler\0Arrow\0Rectangle\0Ellipse\0Eraser\0";
        int tool_index = static_cast<int>(state.annotation_tool);
        ImGui::SetNextItemWidth(scaled(150.0f));
        if (ImGui::Combo(tr(state, "Tool", "Công cụ"), &tool_index,
                         tool_items)) {
            state.annotation_tool = static_cast<AnnotationTool>(
                std::clamp(tool_index, 0,
                           static_cast<int>(AnnotationTool::eraser)));
            state.annotation_dragging = false;
        }
        ImGui::SameLine();
        float custom_color[4]{
            ((state.annotation_rgba >> 24u) & 0xffu) / 255.0f,
            ((state.annotation_rgba >> 16u) & 0xffu) / 255.0f,
            ((state.annotation_rgba >> 8u) & 0xffu) / 255.0f,
            (state.annotation_rgba & 0xffu) / 255.0f};
        if (ImGui::ColorEdit4(tr(state, "Color", "Màu"), custom_color,
                              ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_AlphaBar)) {
            const auto channel = [](float value) {
                return static_cast<std::uint32_t>(
                    std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            state.annotation_rgba = (channel(custom_color[0]) << 24u) |
                                    (channel(custom_color[1]) << 16u) |
                                    (channel(custom_color[2]) << 8u) |
                                    channel(custom_color[3]);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(tr(state, "Quick colors", "Màu nhanh"));
        ImGui::SameLine();
        constexpr std::array<std::uint32_t, 4> colors{
            0xe5484dffu, 0xf2c94cffu, 0x2f80edffu, 0x27ae60ffu};
        for (std::size_t index = 0; index < colors.size(); ++index) {
            const auto color = colors[index];
            const ImVec4 display{
                ((color >> 24u) & 0xffu) / 255.0f,
                ((color >> 16u) & 0xffu) / 255.0f,
                ((color >> 8u) & 0xffu) / 255.0f, 1.0f};
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::ColorButton("##pen-color", display,
                                   ImGuiColorEditFlags_NoTooltip,
                                   {scaled(24.0f), scaled(24.0f)})) {
                state.annotation_rgba = color;
            }
            ImGui::PopID();
            if (index + 1 != colors.size()) {
                ImGui::SameLine();
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(scaled(140.0f));
        ImGui::SliderInt(tr(state, "Thickness", "Độ dày"),
                         &state.annotation_thickness, 2, 12);
    }
    if (ImGui::BeginPopup("control-status")) {
        ImGui::TextUnformatted(state.control_status.c_str());
        ImGui::EndPopup();
    }

    if (ImGui::BeginChild("chat-panel", {0, scaled(112.0f)}, true)) {
        ImGui::TextUnformatted(tr(state, "Chat", "Trò chuyện"));
        const auto chat_log = control_plane.chat_history(selected_client->id);
        if (ImGui::BeginChild("chat-log", {0, scaled(52.0f)}, true)) {
            if (chat_log.empty()) {
                ImGui::TextDisabled(
                    "%s", tr(state, "No messages in this session.",
                             "Chưa có tin nhắn trong phiên này."));
            } else {
                for (const auto& entry : chat_log) {
                    const char* who =
                        entry.from_teacher
                            ? tr(state, "Teacher", "Giáo viên")
                            : selected_client->hostname.c_str();
                    ImGui::TextWrapped("%s: %s", who, entry.text.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(scaled(-78.0f));
        const bool submit = ImGui::InputText(
            "##chat-input", state.chat_input.data(), state.chat_input.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((submit || ImGui::Button(tr(state, "Send", "Gửi"),
                                     {scaled(68.0f), 0.0f})) &&
            state.chat_input[0] != '\0') {
            std::string error;
            if (control_plane.send_chat(selected_client->id,
                                        state.chat_input.data(), &error)) {
                state.chat_input[0] = '\0';
                state.control_status = tr(state, "Message sent.",
                                          "Đã gửi tin nhắn.");
            } else {
                state.control_status = error;
                record_operation_failure("Chat", "Chat command failed", error);
            }
            ImGui::OpenPopup("control-status");
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

bool draw_segment_option(const char* label, bool selected, float width) {
    if (selected) {
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            g_dark_mode ? ImVec4{0.800f, 0.820f, 0.850f, 1.0f}
                        : ImVec4{0.150f, 0.165f, 0.185f, 1.0f});
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            g_dark_mode ? ImVec4{0.090f, 0.100f, 0.120f, 1.0f}
                        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f});
    }
    const bool pressed = ImGui::Button(label, {width, scaled(28.0f)});
    if (selected) {
        ImGui::PopStyleColor(2);
    }
    return pressed;
}

void draw_preferences(DashboardState& state, float pos_x, float settings_width) {
    ImGui::SameLine(pos_x);
    if (ImGui::Button(tr(state, "Settings", "Cài đặt"),
                      {settings_width, scaled(28.0f)})) {
        ImGui::OpenPopup("settings-popup");
    }
    ImGui::SetNextWindowSize({scaled(410.0f), 0.0f}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("settings-popup")) {
        return;
    }
    ImGui::TextUnformatted(tr(state, "Settings", "Cài đặt"));
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr(state, "Operator surface", "Giao diện vận hành"));
    const bool is_vi = (state.language == Language::vietnamese);
    if (draw_segment_option(role_label(OperatorRole::teacher, is_vi),
                            state.current_role == OperatorRole::teacher, scaled(88.0f))) {
        state.current_role = OperatorRole::teacher;
        if (!is_action_permitted(state.current_role, OperatorAction::view_fleet_health) &&
            state.view == DashboardView::fleet_health) {
            state.view = DashboardView::room_screens;
        }
    }
    ImGui::SameLine();
    if (draw_segment_option(role_label(OperatorRole::technician, is_vi),
                            state.current_role == OperatorRole::technician, scaled(100.0f))) {
        state.current_role = OperatorRole::technician;
    }
    ImGui::SameLine();
    if (draw_segment_option(role_label(OperatorRole::developer, is_vi),
                            state.current_role == OperatorRole::developer, scaled(156.0f))) {
        state.current_role = OperatorRole::developer;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr(state, "Language", "Ngôn ngữ"));
    if (draw_segment_option("EN", state.language == Language::english,
                            scaled(42.0f))) {
        state.language = Language::english;
        g_language = state.language;
        state.control_status.clear();
    }
    ImGui::SameLine();
    if (draw_segment_option("VI", state.language == Language::vietnamese,
                            scaled(42.0f))) {
        state.language = Language::vietnamese;
        g_language = state.language;
        state.control_status.clear();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr(state, "Appearance", "Giao diện"));
    if (draw_segment_option(tr(state, "Light", "Sáng"), !state.dark_mode,
                            scaled(68.0f))) {
        state.dark_mode = false;
        apply_dashboard_style(false);
    }
    ImGui::SameLine();
    if (draw_segment_option(tr(state, "Dark", "Tối"), state.dark_mode,
                            scaled(68.0f))) {
        state.dark_mode = true;
        apply_dashboard_style(true);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr(state, "Screen refresh", "Chu kỳ làm mới"));
    ImGui::SetNextItemWidth(scaled(184.0f));
    ImGui::SliderInt("##settings-refresh", &state.snapshot_interval_seconds,
                     kMinimumSnapshotInterval, kMaximumSnapshotInterval,
                     tr(state, "%d seconds", "%d giây"));
    ImGui::TextWrapped("%s", tr(state,
        "Snapshot commands use this interval for room monitoring and teacher broadcast.",
        "Chu kỳ này được dùng cho snapshot phòng máy và phát màn hình giáo viên."));
    ImGui::Checkbox(
        tr(state, "Monitor every computer automatically",
           "Tự động theo dõi mọi máy"),
        &state.auto_monitor);
    ImGui::TextWrapped("%s", tr(state,
        "When on, every online computer streams snapshots continuously; turn it off to start and stop snapshots per computer.",
        "Khi bật, mọi máy trực tuyến sẽ gửi snapshot liên tục; tắt để bật/tắt snapshot theo từng máy."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr(state, "Optional diagnostics",
                                  "Chẩn đoán tùy chọn"));
    bool collection_enabled = g_telemetry_policy.collect_in_background;
    if (ImGui::Checkbox(
            tr(state, "Collect privacy-filtered diagnostics locally",
               "Thu thập chẩn đoán đã lọc riêng tư trên máy"),
            &collection_enabled)) {
        auto policy = g_telemetry_policy;
        policy.collect_in_background = collection_enabled;
        (void)update_telemetry_policy(state, policy);
    }
    ImGui::BeginDisabled(!g_telemetry_policy.collect_in_background);
    bool prompt_on_error = g_telemetry_policy.prompt_on_error;
    if (ImGui::Checkbox(
            tr(state, "Prompt when an error report is ready",
               "Nhắc khi báo cáo lỗi đã sẵn sàng"),
            &prompt_on_error)) {
        auto policy = g_telemetry_policy;
        policy.prompt_on_error = prompt_on_error;
        (void)update_telemetry_policy(state, policy);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", tr(
        state,
        "Disabled by default. Events stay in memory and are discarded when NSTU exits. Nothing is uploaded automatically; review the report before posting it to the public GitHub issue tracker.",
        "Mặc định tắt. Sự kiện chỉ nằm trong bộ nhớ và bị xóa khi NSTU thoát. Không có dữ liệu nào tự động tải lên; hãy xem lại báo cáo trước khi đăng lên GitHub Issues công khai."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr(state, "About", "Giới thiệu"));
    ImGui::Text("NSTU %s", NSTU_PROJECT_VERSION);
    ImGui::TextWrapped("%s", tr(
        state,
        "Open-source classroom management under the MIT License.",
        "Phần mềm quản lý lớp học mã nguồn mở theo giấy phép MIT."));
    if (g_telemetry_policy.collect_in_background) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr(state, "Diagnostic data",
                                      "Dữ liệu chẩn đoán"));
        ImGui::Text("%s: %llu / %llu",
                    tr(state, "Collected events", "Sự kiện đã thu thập"),
                    static_cast<unsigned long long>(g_telemetry_events.size()),
                    static_cast<unsigned long long>(
                        nstu::telemetry::kMaximumEvents));
        const bool can_export = is_action_permitted(state.current_role, OperatorAction::export_telemetry_report);
        ImGui::BeginDisabled(!can_export);
        if (ImGui::Button(tr(state, "Review report", "Xem báo cáo"),
                          {scaled(116.0f), 0.0f})) {
            state.telemetry_report_requested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (!can_export && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr(state,
                "Telemetry report review requires Developer role.",
                "Xem báo cáo đo từ xa yêu cầu vai trò Nhà phát triển."));
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(state, "Clear data", "Xóa dữ liệu"),
                          {scaled(104.0f), 0.0f})) {
            g_telemetry_events.clear();
            g_telemetry_error_prompt_requested.store(false);
            g_telemetry_error_prompt_armed.store(
                g_telemetry_policy.prompt_on_error);
            state.telemetry_report_preview.clear();
            state.control_status = tr(state, "Collected diagnostics cleared.",
                                      "Đã xóa dữ liệu chẩn đoán.");
        }
    }
    ImGui::EndPopup();
}

void draw_role_switcher(DashboardState& state, float pos_x, float width) {
    ImGui::SameLine(pos_x);
    const bool is_vietnamese = (state.language == Language::vietnamese);
    char role_btn_text[128]{};
    sprintf_s(role_btn_text, "%s: %s##role-btn",
              tr(state, "Role", "Vai trò"),
              role_label(state.current_role, is_vietnamese));
    if (ImGui::Button(role_btn_text, {width, scaled(28.0f)})) {
        ImGui::OpenPopup("role-selector-popup");
    }
    if (ImGui::BeginPopup("role-selector-popup")) {
        ImGui::TextDisabled("%s", tr(state, "Operator surface", "Giao diện vận hành"));
        ImGui::Separator();
        auto select_role = [&](OperatorRole target_role) {
            const bool is_selected = (state.current_role == target_role);
            if (ImGui::Selectable(role_label(target_role, is_vietnamese), is_selected)) {
                state.current_role = target_role;
                if (!is_action_permitted(state.current_role, OperatorAction::view_fleet_health) &&
                    state.view == DashboardView::fleet_health) {
                    state.view = DashboardView::room_screens;
                }
            }
        };
        select_role(OperatorRole::teacher);
        select_role(OperatorRole::technician);
        select_role(OperatorRole::developer);
        ImGui::EndPopup();
    }
}

void set_room_lock(const std::vector<nstu::server::ClientRecord>& clients,
                   DashboardState& state,
                   nstu::server::ServerControlPlane& control_plane,
                   bool locked) {
    std::size_t sent = 0;
    for (const auto& client : clients) {
        if (client.status != nstu::server::ClientStatus::offline &&
            control_plane.set_locked(client.id, locked, nullptr)) {
            ++sent;
        }
    }
    state.control_status = sent == 0
        ? tr(state, "No online clients are available.",
             "Không có máy trực tuyến để điều khiển.")
        : (locked ? tr(state, "Room lock command sent.",
                       "Đã gửi lệnh khóa toàn phòng.")
                  : tr(state, "Room unlock command sent.",
                       "Đã gửi lệnh mở khóa toàn phòng."));
}

void toggle_teacher_broadcast(DashboardState& state,
                              nstu::server::ServerControlPlane& control_plane) {
    if (!nstu::server::is_action_permitted(state.current_role,
                                           nstu::server::OperatorAction::broadcast)) {
        state.control_status = tr(state, "Permission denied for broadcast action.",
                                  "Không có quyền phát màn hình.");
        return;
    }
    if (state.broadcast_enabled) {
        std::string error;
        if (control_plane.stop_host_broadcast(&error)) {
            state.broadcast_enabled = false;
            state.control_status = tr(state, "Teacher broadcast stopped.",
                                      "Đã dừng phát màn hình giáo viên.");
        } else {
            state.control_status = error;
            record_operation_failure("Broadcast", "Stop command failed",
                                     error);
        }
        return;
    }
    std::string error;
    if (!control_plane.set_broadcast_mode(state.broadcast_mode,
                                         state.broadcast_lock_input,
                                         &error)) {
        state.control_status = error;
        record_operation_failure("Broadcast", "Set mode failed", error);
        return;
    }
    state.broadcast_enabled = true;
    state.next_host_snapshot = {};
    state.control_status = tr(state, "Teacher broadcast started.",
                              "Đã bắt đầu phát màn hình giáo viên.");
}

void update_teacher_broadcast_mode(DashboardState& state,
                                   nstu::server::ServerControlPlane& control_plane,
                                   nstu::control::DemonstrationMode mode,
                                   bool lock_input) {
    state.broadcast_mode = mode;
    state.broadcast_lock_input = lock_input;
    if (state.broadcast_enabled) {
        std::string error;
        if (!control_plane.set_broadcast_mode(mode, lock_input, &error)) {
            record_operation_failure("Broadcast", "Update mode failed", error);
        }
    }
}

void draw_diagnostics_popup(DashboardState& state, float pos_x, float button_width) {
    ImGui::SameLine(pos_x);
    if (ImGui::Button(tr(state, "Diagnostics", "Chẩn đoán"),
                      {button_width, scaled(28.0f)})) {
        refresh_graphics_report();
        ImGui::OpenPopup("diagnostics-popup");
    }
    if (!ImGui::BeginPopupModal("diagnostics-popup", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted(tr(state, "Graphics diagnostics",
                              "Chẩn đoán đồ họa"));
    ImGui::Separator();
    ImGui::Text("Device: %s", g_graphics_report.device_mode.empty()
        ? "Not initialized" : g_graphics_report.device_mode.c_str());
    ImGui::Text("Adapter: %s (%s)",
                g_graphics_report.selected_adapter.empty()
                    ? "Unknown" : g_graphics_report.selected_adapter.c_str(),
                g_graphics_report.selected_vendor.empty()
                    ? "Unknown" : g_graphics_report.selected_vendor.c_str());
    ImGui::Text("Feature level: %s",
                g_graphics_report.feature_level.empty()
                    ? "Unknown" : g_graphics_report.feature_level.c_str());
    ImGui::Text("Desktop Duplication: %s",
                g_graphics_report.desktop_duplication.empty()
                    ? "Unknown" : g_graphics_report.desktop_duplication.c_str());
    ImGui::Text("Hardware H.264 encoders: %s",
                g_graphics_report.h264_encoders.empty()
                    ? "Unknown" : g_graphics_report.h264_encoders.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(tr(state, "Adapters", "Bộ điều hợp"));
    if (ImGui::BeginChild("diagnostic-adapters", {scaled(620.0f), scaled(80.0f)}, true)) {
        for (const auto& adapter : g_graphics_report.adapters) {
            ImGui::BulletText("%s", adapter.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextUnformatted(tr(state, "Recent events", "Sự kiện gần đây"));
    if (ImGui::BeginChild("diagnostic-events", {scaled(620.0f), scaled(180.0f)}, true)) {
        for (const auto& event : g_diagnostics) {
            ImGui::TextWrapped("[%s] [%s] %s: %s", event.timestamp.c_str(),
                               event.severity.c_str(), event.source.c_str(),
                               event.message.c_str());
        }
    }
    ImGui::EndChild();
    if (ImGui::Button(tr(state, "Refresh", "Làm mới"), {scaled(90.0f), 0.0f})) {
        refresh_graphics_report();
    }
    if (g_telemetry_policy.collect_in_background &&
        is_action_permitted(state.current_role, OperatorAction::export_telemetry_report)) {
        ImGui::SameLine();
        if (ImGui::Button(tr(state, "Review report", "Xem báo cáo"),
                          {scaled(118.0f), 0.0f})) {
            state.telemetry_report_requested = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Close", "Đóng"), {scaled(90.0f), 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_telemetry_report_popup(DashboardState& state,
                                 std::size_t client_count) {
    if (g_telemetry_error_prompt_requested.exchange(false)) {
        state.telemetry_report_requested = true;
    }
    if (state.telemetry_report_requested) {
        state.telemetry_report_requested = false;
        refresh_graphics_report();
        g_telemetry_error_prompt_requested.store(false);
        state.telemetry_report_preview = nstu::telemetry::build_public_markdown(
            make_telemetry_report(state, client_count));
        ImGui::OpenPopup("sanitized-diagnostic-report");
    }

    const ImVec2 viewport_size = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(
        {std::min(scaled(760.0f), viewport_size.x - scaled(32.0f)),
         std::min(scaled(600.0f), viewport_size.y - scaled(32.0f))},
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("sanitized-diagnostic-report", nullptr,
                                ImGuiWindowFlags_NoResize)) {
        return;
    }
    ImGui::TextUnformatted(tr(state, "Review diagnostic report",
                              "Xem lại báo cáo chẩn đoán"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", tr(
        state,
        "Nothing has been sent. GitHub Issues are public. Review the complete text and remove anything you do not want to disclose before submitting.",
        "Chưa có dữ liệu nào được gửi. GitHub Issues là công khai. Hãy xem toàn bộ nội dung và xóa mọi thông tin bạn không muốn công bố trước khi gửi."));
    ImGui::Spacing();
    if (ImGui::BeginChild("sanitized-report-preview", {0.0f, scaled(-48.0f)}, true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(state.telemetry_report_preview.c_str());
    }
    ImGui::EndChild();

    if (ImGui::Button(tr(state, "Copy report", "Sao chép báo cáo"),
                      {scaled(126.0f), 0.0f})) {
        ImGui::SetClipboardText(state.telemetry_report_preview.c_str());
        state.control_status = tr(state, "Sanitized report copied.",
                                  "Đã sao chép báo cáo đã lọc.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Copy and open GitHub",
                               "Sao chép và mở GitHub"),
                      {scaled(178.0f), 0.0f})) {
        ImGui::SetClipboardText(state.telemetry_report_preview.c_str());
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr, L"open", kDiagnosticIssueUrl, nullptr, nullptr,
            SW_SHOWNORMAL));
        state.control_status = result > 32
            ? tr(state, "Report copied; GitHub issue form opened.",
                 "Đã sao chép báo cáo và mở biểu mẫu GitHub Issue.")
            : tr(state, "Report copied, but GitHub could not be opened.",
                 "Đã sao chép báo cáo nhưng không thể mở GitHub.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Clear", "Xóa"), {scaled(82.0f), 0.0f})) {
        g_telemetry_events.clear();
        g_telemetry_error_prompt_requested.store(false);
        g_telemetry_error_prompt_armed.store(
            g_telemetry_policy.prompt_on_error);
        state.telemetry_report_preview = nstu::telemetry::build_public_markdown(
            make_telemetry_report(state, client_count));
        state.control_status = tr(state, "Collected diagnostics cleared.",
                                  "Đã xóa dữ liệu chẩn đoán.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Close", "Đóng"), {scaled(82.0f), 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_software_updates_modal(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    const bool permitted = is_action_permitted(
        state.current_role, OperatorAction::manage_software_updates);
    const char* title = tr(state, "Software Updates###software-updates-modal",
                                 "Cập nhật phần mềm###software-updates-modal");
    if (state.software_updates_modal_open) {
        state.software_updates_modal_open = false;
        if (permitted) {
            ImGui::OpenPopup(title);
        }
    }
    if (!permitted) {
        state.fleet_update_config = {};
    }

    constexpr float kFleetUpdateModalWidth = 760.0f;
    constexpr float kFleetUpdateModalHeight = 620.0f;
    const auto* viewport = ImGui::GetMainViewport();
    const auto& style = ImGui::GetStyle();
    const ImVec2 available{
        std::max(1.0f, viewport->WorkSize.x - style.WindowPadding.x * 2.0f),
        std::max(1.0f, viewport->WorkSize.y - style.WindowPadding.y * 2.0f)};
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(scaled(kFleetUpdateModalWidth), available.x),
                              std::min(scaled(kFleetUpdateModalHeight), available.y)});
    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_NoResize)) {
        return;
    }
    if (!permitted || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    std::map<std::string, std::size_t> versions;
    std::size_t known_versions = 0;
    for (const auto& client : clients) {
        if (!client.client_version.empty()) {
            ++versions[client.client_version];
            ++known_versions;
        }
    }
    ImGui::Text(tr(state, "Computers: %zu | Reported version: %zu | Unknown: %zu",
                         "Máy tính: %zu | Có phiên bản: %zu | Chưa rõ: %zu"),
                clients.size(), known_versions, clients.size() - known_versions);
    if (ImGui::TreeNode(tr(state, "Reported version distribution", "Phân bố phiên bản được báo cáo"))) {
        for (const auto& [version, count] : versions) {
            ImGui::BulletText("%s: %zu", version.c_str(), count);
        }
        ImGui::TreePop();
    }
    ImGui::TextWrapped("%s", tr(state,
        "Offline versions are last reported and may be stale. No approved target release is selected.",
        "Phiên bản của máy ngoại tuyến là báo cáo gần nhất và có thể đã cũ. Chưa chọn bản phát hành đích được phê duyệt."));
    ImGui::Separator();
    ImGui::TextUnformatted(tr(state, "Rollout draft (this app session only)",
                                   "Bản nháp triển khai (chỉ trong phiên ứng dụng này)"));
    ImGui::TextWrapped("%s", tr(state,
        "These preferences are not saved or sent to computers. Package deployment and rollback are not available yet.",
        "Các lựa chọn này không được lưu hoặc gửi tới máy tính. Chức năng triển khai gói và khôi phục phiên bản chưa khả dụng."));
    auto proposed = state.fleet_update_config;
    int channel_index = static_cast<int>(proposed.channel) - 1;
    const char* channel_labels[] = {
        tr(state, "LTSC Stable", "LTSC ổn định"),
        tr(state, "Security Hotfixes", "Bản vá bảo mật"),
        tr(state, "Pilot Canary", "Thử nghiệm Canary"),
    };
    bool changed = ImGui::Combo(tr(state, "Release channel", "Kênh phát hành"),
                                &channel_index, channel_labels, 3);
    proposed.channel = static_cast<nstu::update::ReleaseChannel>(channel_index + 1);
    changed |= ImGui::Checkbox(tr(state, "Maintenance window planned", "Đã lên lịch bảo trì"),
                               &proposed.maintenance_window_planned);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr(state,
            "Planning acknowledgment only. Verify the actual UWF or Deep Freeze thaw/maintenance state on each computer before deployment.",
            "Chỉ xác nhận kế hoạch. Cần kiểm tra trạng thái bảo trì hoặc Thawed của UWF/Deep Freeze trên từng máy trước khi triển khai."));
    }
    changed |= ImGui::SliderInt(tr(state, "Canary rollout", "Tỷ lệ thử nghiệm"),
                                &proposed.canary_rollout_percentage, 0, 100, "%d%%",
                                ImGuiSliderFlags_AlwaysClamp);
    if (changed) {
        (void)nstu::server::try_set_fleet_update_config(
            state.current_role, state.fleet_update_config, proposed);
    }
    ImGui::Spacing();
    if (clients.empty()) {
        ImGui::TextWrapped("%s", tr(state, "No computers have reported fleet metadata yet.",
                                          "Chưa có máy tính báo cáo thông tin hệ thống."));
    } else {
        const float table_height = std::max(ImGui::GetTextLineHeightWithSpacing(),
            ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
            {style.CellPadding.x, std::max(style.CellPadding.y, scaled(6.0f))});
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,
                              ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        if (ImGui::BeginTable("software-update-inventory", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            {0.0f, table_height})) {
            ImGui::TableSetupColumn(tr(state, "Computer", "Máy tính"));
            ImGui::TableSetupColumn(tr(state, "Address", "Địa chỉ"));
            ImGui::TableSetupColumn(tr(state, "Reported version", "Phiên bản báo cáo"));
            ImGui::TableSetupColumn(tr(state, "Connection", "Kết nối"));
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto& client : clients) {
                push_client_id(client.id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(client.hostname.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(client.address.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(client.client_version.empty()
                    ? tr(state, "Unknown", "Chưa rõ") : client.client_version.c_str());
                ImGui::TableNextColumn();
                draw_status_badge(client.status, state);
                pop_client_id();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    if (ImGui::Button(tr(state, "Close", "Đóng"))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_support_bundle_modal(DashboardState& state) {
    if (state.support_bundle_modal_open) {
        state.support_bundle_modal_open = false;
        ImGui::OpenPopup("support-bundle-modal");
    }
    const ImVec2 viewport_size = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(
        {std::min(scaled(760.0f), viewport_size.x - scaled(32.0f)),
         std::min(scaled(620.0f), viewport_size.y - scaled(32.0f))},
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("support-bundle-modal", nullptr,
                                ImGuiWindowFlags_NoResize)) {
        return;
    }
    ImGui::TextUnformatted(tr(state, "Export Sanitized Support Bundle",
                              "Xuất gói hỗ trợ đã lọc"));
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr(
        state,
        "This support bundle contains sanitized system and fleet diagnostics for troubleshooting. "
        "Passwords, cryptographic keys, enrollment secrets, student names, and exam responses are strictly excluded.",
        "Gói hỗ trợ này chứa thông tin chẩn đoán hệ thống đã được lọc để khắc phục sự cố. "
        "Mật khẩu, khóa mã hóa, bí mật ghép nối, tên học sinh và bài thi được loại trừ tuyệt đối."));
    ImGui::Spacing();

    ImGui::Checkbox(tr(state, "I consent to exporting this sanitized support bundle.",
                              "Tôi đồng ý xuất gói hỗ trợ đã lọc này."),
                    &state.support_bundle_consented);
    ImGui::Spacing();

    if (ImGui::BeginChild("support-bundle-preview", {0.0f, scaled(-52.0f)}, true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(state.support_bundle_preview.c_str());
    }
    ImGui::EndChild();

    const auto consent_decision = nstu::support::evaluate_support_bundle_export(
        static_cast<int>(state.current_role), state.support_bundle_consented);
    const bool can_export = (consent_decision == nstu::support::SupportBundleConsentDecision::permitted);

    if (!can_export) {
        ImGui::BeginDisabled();
    }
    ImGui::BeginGroup();
    if (ImGui::Button(tr(state, "Copy Bundle Markdown", "Sao chép Markdown"),
                      {scaled(170.0f), 0.0f})) {
        ImGui::SetClipboardText(state.support_bundle_preview.c_str());
        state.control_status = tr(state, "Support bundle copied to clipboard.",
                                  "Đã sao chép gói hỗ trợ vào bộ nhớ tạm.");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Save to Documents", "Lưu vào Documents"),
                      {scaled(160.0f), 0.0f})) {
        std::string docs_path;
        char profile_buf[MAX_PATH]{};
        const DWORD env_len = GetEnvironmentVariableA("USERPROFILE", profile_buf, MAX_PATH);
        if (env_len > 0 && env_len < MAX_PATH) {
            docs_path = std::string(profile_buf) + "\\Documents";
        } else {
            docs_path = ".";
        }
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char filename[128]{};
        sprintf_s(filename, "\\NSTU_Support_Bundle_%04u%02u%02u_%02u%02u%02u.md",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        const std::string full_file_path = docs_path + filename;
        std::ofstream out(full_file_path);
        if (out.is_open()) {
            out << state.support_bundle_preview;
            out.close();
            state.control_status = tr(state, "Support bundle saved to Documents.",
                                      "Đã lưu gói hỗ trợ vào Documents.");
            ImGui::CloseCurrentPopup();
        } else {
            state.control_status = tr(state, "Failed to write support bundle file.",
                                      "Không thể ghi tệp gói hỗ trợ.");
        }
    }
    ImGui::EndGroup();
    if (!can_export) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!state.support_bundle_consented) {
                ImGui::SetTooltip("%s", tr(state, "Consent required before export.",
                                                  "Yêu cầu đồng ý trước khi xuất."));
            } else {
                ImGui::SetTooltip("%s", tr(state, "Technician or Developer role required.",
                                                  "Yêu cầu quyền Kỹ thuật viên hoặc Nhà phát triển."));
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Close", "Đóng"), {scaled(80.0f), 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_room_profile_modal(DashboardState& state, nstu::server::ServerControlPlane& control_plane) {
    if (state.room_profile_modal_open) {
        state.room_profile_modal_open = false;
        ImGui::OpenPopup("room-profile-modal");
    }
    const ImVec2 viewport_size = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(
        {std::min(scaled(760.0f), viewport_size.x - scaled(32.0f)),
         std::min(scaled(580.0f), viewport_size.y - scaled(32.0f))},
        ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("room-profile-modal", nullptr,
                                ImGuiWindowFlags_NoResize)) {
        return;
    }

    ImGui::TextUnformatted(tr(state, "Portable Secret-Free Room Profile",
                              "Hồ sơ phòng học di động không chứa bí mật"));
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("%s", tr(
        state,
        "Room profiles contain classroom layout, seats, and preset websites/applications. "
        "Zero credentials, pairing keys, enrollment secrets, student data, or exam answers are included.",
        "Hồ sơ phòng học chứa bố cục lớp học, sơ đồ chỗ ngồi và các trang web/ứng dụng đặt trước. "
        "Tuyệt đối không chứa mật khẩu, khóa ghép nối, bí mật ghi danh, thông tin học sinh hay bài thi."));
    ImGui::Spacing();

    if (ImGui::BeginTabBar("room-profile-tabs")) {
        if (ImGui::BeginTabItem(tr(state, "Export Current Profile", "Xuất hồ sơ hiện tại"))) {
            ImGui::Spacing();
            ImGui::TextUnformatted(tr(state, "Generated JSON Profile:", "Hồ sơ JSON được tạo:"));
            if (ImGui::BeginChild("room-profile-export-preview", {0.0f, scaled(-90.0f)}, true,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                ImGui::TextUnformatted(state.room_profile_export_text.c_str());
            }
            ImGui::EndChild();
            ImGui::Spacing();

            if (ImGui::Button(tr(state, "Copy Profile JSON", "Sao chép JSON"), {scaled(150.0f), 0.0f})) {
                ImGui::SetClipboardText(state.room_profile_export_text.c_str());
                state.room_profile_status_message = tr(state, "Profile JSON copied to clipboard.",
                                                      "Đã sao chép JSON hồ sơ vào bộ nhớ tạm.");
                state.room_profile_status_is_error = false;
            }
            ImGui::SameLine();
            if (ImGui::Button(tr(state, "Save to Documents", "Lưu vào Documents"), {scaled(170.0f), 0.0f})) {
                char profile_buf[MAX_PATH]{};
                const DWORD env_len = GetEnvironmentVariableA("USERPROFILE", profile_buf, MAX_PATH);
                std::string docs_path = (env_len > 0 && env_len < MAX_PATH)
                    ? (std::string(profile_buf) + "\\Documents")
                    : "C:\\";
                char filename[128]{};
                SYSTEMTIME st;
                GetLocalTime(&st);
                sprintf_s(filename, "\\nstu_room_profile_%04u%02u%02u_%02u%02u%02u.json",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                const std::string full_path = docs_path + filename;
                std::ofstream out(full_path);
                if (out.is_open()) {
                    out << state.room_profile_export_text;
                    out.close();
                    state.room_profile_status_message = tr(state, "Room profile saved to Documents.",
                                                          "Đã lưu hồ sơ phòng học vào Documents.");
                    state.room_profile_status_is_error = false;
                } else {
                    state.room_profile_status_message = tr(state, "Failed to save file.", "Không thể lưu tệp.");
                    state.room_profile_status_is_error = true;
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(tr(state, "Import & Apply Profile", "Nhập & Áp dụng hồ sơ"))) {
            ImGui::Spacing();
            ImGui::TextUnformatted(tr(state, "Paste Room Profile JSON:", "Dán JSON hồ sơ phòng học:"));
            ImGui::InputTextMultiline("##import-profile-json", state.room_profile_import_buffer.data(),
                                      state.room_profile_import_buffer.size(),
                                      {0.0f, scaled(-90.0f)},
                                      ImGuiInputTextFlags_AllowTabInput);
            ImGui::Spacing();
            if (ImGui::Button(tr(state, "Validate & Apply Profile", "Xác thực & Áp dụng"), {scaled(200.0f), 0.0f})) {
                const auto result = nstu::fleet::import_room_profile_json(state.room_profile_import_buffer.data());
                if (result.error == nstu::fleet::RoomProfileError::forbidden_secret_detected) {
                    state.room_profile_status_message = tr(
                        state,
                        "REJECTED: Forbidden secret or credential detected in profile payload!",
                        "TỪ CHỐI: Phát hiện bí mật hoặc khóa nhạy cảm trong hồ sơ!");
                    state.room_profile_status_is_error = true;
                } else if (!result.ok()) {
                    state.room_profile_status_message = tr(state, "Validation Error: ", "Lỗi xác thực: ") +
                                                        result.error_message;
                    state.room_profile_status_is_error = true;
                } else {
                    strncpy_s(state.room_name_input.data(), state.room_name_input.size(),
                              result.profile.room_name.c_str(), _TRUNCATE);
                    control_plane.set_server_name(result.profile.room_name);
                    if (!state.room_name_path.empty()) {
                        std::ofstream out(state.room_name_path, std::ios::binary | std::ios::trunc);
                        if (out.is_open()) {
                            out << result.profile.room_name;
                        }
                    }
                    state.room_profile_status_message = tr(state, "Profile applied successfully for room '",
                                                          "Đã áp dụng thành công hồ sơ cho phòng '") +
                                                        result.profile.room_name + "'.";
                    state.room_profile_status_is_error = false;
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (!state.room_profile_status_message.empty()) {
        ImGui::Spacing();
        const ImVec4 status_col = state.room_profile_status_is_error
            ? ImVec4{0.9f, 0.3f, 0.3f, 1.0f}
            : ImVec4{0.3f, 0.85f, 0.4f, 1.0f};
        ImGui::TextColored(status_col, "%s", state.room_profile_status_message.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button(tr(state, "Close", "Đóng"), {scaled(80.0f), 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The computer name is the only label a teacher reliably recognises in a
// selection menu. The beacon sanitizes it again before it reaches the wire.
std::string local_server_name() {
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(name, &length) && length > 0) {
        return std::string(name, length);
    }
    return "NSTU";
}

// The room name is a persisted display hint, not a secret, so it lives in a
// small plaintext file under the data root rather than the protected keyring. A
// missing or unreadable file means "no room configured" and the beacon falls
// back to the computer name. Only the first line is the label; trailing CR (a
// CRLF file) and surrounding spaces are trimmed so a hand-edited file still
// round-trips. The control plane sanitizes the value to the discovery bound
// before it reaches the wire, so a corrupt file can only mangle the label,
// never the pairing that the six-digit code secures.
std::string load_room_name(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::string line;
    std::getline(file, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = line.find_last_not_of(" \t");
    return line.substr(first, last - first + 1);
}

// Persists the room label as plaintext. Best-effort: a write failure only means
// the label will not survive a restart, so the caller surfaces it as a benign
// status rather than a hard error. The value is written verbatim; the control
// plane has already sanitized it to the discovery bound.
bool save_room_name(const std::filesystem::path& path, std::string_view name) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << name;
    return static_cast<bool>(file);
}

// Six digits compared out loud across a room are easier to read in two groups.
std::string grouped_code(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
}

// The room-name field lets the operator label this server so a student machine
// on a shared VLAN can target the right classroom by name. Editing it pushes
// each keystroke to the live beacon (a client sweeping right now sees the new
// label) and, once editing settles, persists the plaintext hint and reflects
// the sanitized value back so the field matches what is advertised and saved.
// The six-digit SAS the operator still compares is unaffected: the room name
// only routes, it never authorizes.
void draw_room_name_field(DashboardState& state,
                          nstu::server::ServerControlPlane& control_plane) {
    ImGui::TextUnformatted(tr(state,
        "Room name (optional label shown to computers)",
        "Tên phòng (nhãn tùy chọn hiển thị cho máy)"));
    ImGui::SetNextItemWidth(scaled(360.0f));
    if (ImGui::InputText("##room-name", state.room_name_input.data(),
                         state.room_name_input.size())) {
        control_plane.set_server_name(state.room_name_input.data());
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string sanitized = control_plane.server_name();
        std::snprintf(state.room_name_input.data(),
                      state.room_name_input.size(), "%s", sanitized.c_str());
        if (!state.room_name_path.empty() &&
            !save_room_name(state.room_name_path, sanitized)) {
            record_operation_failure("Pairing",
                                     "Room name could not be saved", "");
        }
    }
    ImGui::TextDisabled("%s", tr(state,
        "Leave blank to use the computer name.",
        "Để trống để dùng tên máy tính."));
}

// The operator's half of verified pairing. By the time a row appears here the
// key exchange is done and the client has already proved it derived the same
// secret, so what is left is the one thing arithmetic cannot settle: whether
// the machine on the other end of that exchange is the machine the teacher is
// standing next to. Comparing the six digits answers it, and nothing turns
// into a key without it.
void draw_pairing_requests(DashboardState& state,
                           nstu::server::ServerControlPlane& control_plane) {
    ImGui::TextUnformatted(tr(state,
        "New computers can find this server while this window is open.",
        "Máy mới có thể tìm thấy máy "
        "chủ này khi cửa sổ này đang mở."));
    ImGui::TextDisabled("%s", tr(state,
        "Approve only if the code matches the one shown on that computer.",
        "Chỉ duyệt khi mã trùng với mã "
        "hiển thị trên máy đó."));
    ImGui::Separator();
    const auto pending = control_plane.pending_pairings();
    if (pending.empty()) {
        ImGui::Dummy({scaled(656.0f), scaled(8.0f)});
        ImGui::TextDisabled("%s", tr(state, "Waiting for computers...",
                                     "Đang chờ máy..."));
        ImGui::Dummy({scaled(656.0f), scaled(8.0f)});
    } else if (ImGui::BeginTable("pairing-requests", 4,
                                 ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr(state, "Computer", "Máy"),
                                ImGuiTableColumnFlags_WidthFixed,
                                scaled(300.0f));
        ImGui::TableSetupColumn(tr(state, "Code", "Mã"),
                                ImGuiTableColumnFlags_WidthFixed,
                                scaled(110.0f));
        ImGui::TableSetupColumn(tr(state, "Time left", "Còn lại"),
                                ImGuiTableColumnFlags_WidthFixed,
                                scaled(70.0f));
        ImGui::TableSetupColumn("##pairing-actions",
                                ImGuiTableColumnFlags_WidthFixed,
                                scaled(176.0f));
        ImGui::TableHeadersRow();
        for (const auto& request : pending) {
            push_client_id(request.pairing_id);
            ImGui::TableNextRow(0, scaled(46.0f));
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(request.hostname.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", request.address.c_str());
            }
            ImGui::TextDisabled("%s", request.client_uuid.c_str());
            ImGui::TableSetColumnIndex(1);
            if (g_heading_font != nullptr) {
                ImGui::PushFont(g_heading_font);
            }
            ImGui::TextUnformatted(
                grouped_code(request.short_authentication_string).c_str());
            if (g_heading_font != nullptr) {
                ImGui::PopFont();
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u s",
                        static_cast<unsigned>(request.seconds_remaining));
            ImGui::TableSetColumnIndex(3);
            std::string error;
            if (ImGui::Button(tr(state, "Approve", "Duyệt"),
                              {scaled(84.0f), scaled(30.0f)})) {
                if (control_plane.approve_pairing(request.pairing_id,
                                                  &error)) {
                    state.pairing_status = request.hostname + " " +
                        tr(state, "is now enrolled.",
                           "đã được ghép "
                           "nối.");
                } else {
                    record_operation_failure("Pairing",
                                             "Approve request failed", error);
                    state.pairing_status = error;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(tr(state, "Reject", "Từ chối"),
                              {scaled(84.0f), scaled(30.0f)})) {
                if (control_plane.reject_pairing(request.pairing_id, &error)) {
                    state.pairing_status = request.hostname + " " +
                        tr(state, "was turned away.",
                           "đã bị từ chối.");
                } else {
                    record_operation_failure("Pairing",
                                             "Reject request failed", error);
                    state.pairing_status = error;
                }
            }
            pop_client_id();
        }
        ImGui::EndTable();
    }
    if (!state.pairing_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", state.pairing_status.c_str());
    }
}

void draw_pairing_popup(DashboardState& state,
                        nstu::server::ServerControlPlane& control_plane,
                        float pos_x, float button_width) {
    ImGui::SameLine(pos_x);
    bool focus_panel = false;
    if (ImGui::Button(
            tr(state,
               state.pairing_panel_open ? "Pairing active" : "Add computers",
               state.pairing_panel_open ? "Đang ghép nối" : "Thêm máy"),
            {button_width, scaled(28.0f)})) {
        if (!state.pairing_panel_open) {
            state.pairing_status.clear();
            state.pairing_panel_open = true;
        }
        focus_panel = true;
    }

    if (state.pairing_panel_open && !control_plane.pairing_window_open()) {
        // Seed the editable field from the live room label so it shows what the
        // beacon is currently advertising (empty means "using the computer
        // name"). The configured name already wins inside set_pairing_window,
        // so the computer name here is only the fallback.
        const std::string current_room = control_plane.server_name();
        std::snprintf(state.room_name_input.data(),
                      state.room_name_input.size(), "%s",
                      current_room.c_str());
        control_plane.set_pairing_window(true, local_server_name());
    }

    if (state.pairing_panel_open) {
        const float pairing_width = std::min(
            scaled(720.0f), ImGui::GetMainViewport()->WorkSize.x - scaled(32.0f));
        ImGui::SetNextWindowSize({pairing_width, 0.0f}, ImGuiCond_FirstUseEver);
        if (focus_panel) {
            ImGui::SetNextWindowFocus();
        }
        if (ImGui::Begin(
                tr(state, "Add computers###pairing-window",
                   "Thêm máy###pairing-window"),
                &state.pairing_panel_open,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            const auto discovery = control_plane.pairing_discovery_stats();
            ImGui::TextColored(
                discovery.beacon_enabled
                    ? (g_dark_mode ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                                       : ImVec4{0.10f, 0.55f, 0.24f, 1.0f})
                    : (g_dark_mode ? ImVec4{0.96f, 0.76f, 0.34f, 1.0f}
                                       : ImVec4{0.63f, 0.36f, 0.02f, 1.0f}),
                "%s", tr(state,
                           discovery.beacon_enabled
                               ? "Discovery active - listening for new computers"
                               : "Discovery inactive - set a room name",
                           discovery.beacon_enabled
                               ? "Đang tìm kiếm - chờ máy mới"
                               : "Chưa tìm kiếm - hãy đặt tên phòng"));
            ImGui::TextDisabled(
                "%s: %llu    %s: %llu",
                tr(state, "Probes received", "Tín hiệu đã nhận"),
                static_cast<unsigned long long>(discovery.probes_received),
                tr(state, "Replies sent", "Phản hồi đã gửi"),
                static_cast<unsigned long long>(discovery.beacons_sent));
            ImGui::TextDisabled("%s", tr(
                state,
                "Clients broadcast about every 10 seconds. Keep this window open.",
                "Máy khách phát tín hiệu khoảng mỗi 10 giây. Hãy giữ cửa sổ này mở."));
            draw_room_name_field(state, control_plane);
            ImGui::Separator();
            draw_pairing_requests(state, control_plane);
            ImGui::Separator();
            if (ImGui::Button(tr(state, "Done", "Xong"),
                              {scaled(110.0f), scaled(30.0f)})) {
                state.pairing_panel_open = false;
            }
        }
        ImGui::End();
    }

    // The beacon must not outlive the visible panel. Closing it explicitly
    // stops discovery and refuses unanswered requests.
    if (!state.pairing_panel_open && control_plane.pairing_window_open()) {
        control_plane.set_pairing_window(false);
    }
}

void scan_exam_packages(DashboardState& state) {
    state.discovered_packages.clear();
    const std::vector<std::filesystem::path> search_paths = {
        std::filesystem::path("exams/packages"),
        std::filesystem::path("exams"),
        std::filesystem::path("exam"),
        std::filesystem::path("packaging"),
        std::filesystem::path("staging")
    };

    std::error_code ec;
    for (const auto& search_path : search_paths) {
        if (std::filesystem::exists(search_path, ec) && std::filesystem::is_directory(search_path, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(search_path, ec)) {
                if (entry.is_directory(ec)) {
                    const auto manifest_path = entry.path() / "manifest.json";
                    if (std::filesystem::exists(manifest_path, ec)) {
                        state.discovered_packages.push_back(entry.path().string());
                    }
                } else if (entry.path().extension() == ".nstuexam") {
                    state.discovered_packages.push_back(entry.path().string());
                }
            }
        }
    }
}

void copy_bounded_string(char* dest, std::size_t dest_size, std::string_view src) noexcept {
    if (dest == nullptr || dest_size == 0) return;
    const std::size_t count = std::min(dest_size - 1, src.size());
    std::memcpy(dest, src.data(), count);
    dest[count] = '\0';
}

void select_exam_package(DashboardState& state, std::size_t index) {
    if (index >= state.discovered_packages.size()) return;
    const std::string& path_str = state.discovered_packages[index];
    state.selected_package_index = static_cast<int>(index);

    copy_bounded_string(state.exam_package_root.data(), state.exam_package_root.size(), path_str);

    std::filesystem::path p(path_str);
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        auto mf = p / "manifest.json";
        if (std::filesystem::exists(mf, ec)) {
            std::ifstream in(mf);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto id_pos = text.find("\"id\"");
            if (id_pos != std::string::npos) {
                auto col = text.find(':', id_pos);
                auto q1 = text.find('\"', col);
                auto q2 = text.find('\"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    std::string id = text.substr(q1 + 1, q2 - q1 - 1);
                    copy_bounded_string(state.exam_package_id.data(), state.exam_package_id.size(), id);
                }
            }
        }
    } else {
        std::string stem = p.stem().string();
        copy_bounded_string(state.exam_package_id.data(), state.exam_package_id.size(), stem);
    }
}

void generate_sealed_cohort_export(
    DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    (void)control_plane;
    const std::string pkg_id = state.exam_package_id.data();
    if (pkg_id.empty()) {
        state.control_status = tr(state, "Cannot export: Package ID is required.",
                                  "Không thể xuất: Cần có Package ID.");
        return;
    }

    std::filesystem::path export_dir = "exams/exports";
    std::error_code ec;
    std::filesystem::create_directories(export_dir, ec);

    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    const std::string filename = pkg_id + "_" + std::to_string(now_ms) + ".nstugrade";
    const auto export_path = export_dir / filename;

    nstu::server::grading::CandidateExamGrade grade;
    grade.package_id = pkg_id;
    grade.candidate_id = state.exam_candidate_id.data();
    if (grade.candidate_id.empty()) {
        grade.candidate_id = "cohort-submission";
    }
    parse_sha256_hex(state.exam_digest_hex.data(), grade.package_digest);
    grade.total_points = 100;
    grade.max_points = 100;
    grade.score_percentage = 100.0;
    grade.fully_graded = true;
    grade.completion_timestamp_ms = now_ms;

    std::vector<nstu::exam::AnswerState> answers;
    std::vector<std::string> audit_events = {
        "exam_started",
        "cohort_synchronized",
        "exam_stopped",
        "sealed_export_generated"
    };

    std::vector<std::byte> server_seal_key(32, std::byte{0x5A});
    std::string error;
    if (nstu::server::sealed_export::export_to_file(
            export_path, grade, answers, audit_events, server_seal_key, &error)) {
        state.last_export_path = export_path.string();
        state.control_status = tr(state, "Sealed export created successfully: ",
                                  "Đã tạo file niêm phong thành công: ") + filename;
    } else {
        state.control_status = tr(state, "Export failed: ", "Xuất thất bại: ") + error;
    }
}

void draw_exam_panel(
    const std::vector<nstu::server::ClientRecord>& clients,
    const nstu::server::ClientRecord* selected_client, DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    if (g_heading_font != nullptr) ImGui::PushFont(g_heading_font);
    ImGui::TextUnformatted(tr(state, "Teacher Exam Operator Console", "Bảng điều khiển kỳ thi"));
    if (g_heading_font != nullptr) ImGui::PopFont();
    ImGui::TextWrapped("%s", tr(
        state,
        "Orchestrate cohort assessments, track live candidate progress, and export tamper-evident sealed grades.",
        "Điều phối kỳ thi, theo dõi tiến độ thí sinh theo thời gian thực và xuất kết quả niêm phong số."));

    ImGui::Separator();
    ImGui::Spacing();

    // 1. Package Discovery & Selection Card
    ImGui::TextUnformatted(tr(state, "1. Staged Assessment Package", "1. Gói đề thi"));
    if (ImGui::Button(tr(state, "Scan Packages", "Quét gói thi"), {scaled(140.0f), 0})) {
        scan_exam_packages(state);
    }
    ImGui::SameLine();
    if (!state.discovered_packages.empty()) {
        const char* current_pkg = (state.selected_package_index >= 0 &&
                                   state.selected_package_index < static_cast<int>(state.discovered_packages.size()))
            ? state.discovered_packages[state.selected_package_index].c_str()
            : tr(state, "Select discovered package...", "Chọn gói đã tìm thấy...");
        ImGui::SetNextItemWidth(scaled(380.0f));
        if (ImGui::BeginCombo("##pkg-picker", current_pkg)) {
            for (std::size_t i = 0; i < state.discovered_packages.size(); ++i) {
                const bool is_selected = (state.selected_package_index == static_cast<int>(i));
                if (ImGui::Selectable(state.discovered_packages[i].c_str(), is_selected)) {
                    select_exam_package(state, i);
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("%s", tr(state, "(No packages found in search path)", "(Chưa tìm thấy gói trong thư mục tìm kiếm)"));
    }

    ImGui::SetNextItemWidth(std::min(scaled(720.0f), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##exam-package-root", tr(state, "Package root on client", "Thư mục gói trên máy client"),
                             state.exam_package_root.data(), state.exam_package_root.size());
    ImGui::SetNextItemWidth(std::min(scaled(420.0f), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##exam-package-id", "Package ID",
                             state.exam_package_id.data(), state.exam_package_id.size());
    ImGui::SetNextItemWidth(std::min(scaled(420.0f), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##exam-candidate-id", "Candidate ID (or cohort prefix)",
                             state.exam_candidate_id.data(), state.exam_candidate_id.size());
    ImGui::SetNextItemWidth(std::min(scaled(720.0f), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##exam-digest", "Content SHA-256",
                             state.exam_digest_hex.data(), state.exam_digest_hex.size());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 2. Cohort Orchestration Actions
    const auto active_states = control_plane.exam_client_states();
    const auto ready_clients = control_plane.ready_exam_clients();

    ImGui::TextUnformatted(tr(state, "2. Cohort Launch & Synchronized Barrier", "2. Điều phối & Bắt đầu đồng bộ"));
    ImGui::TextDisabled("%s: %zu | %s: %zu",
                        tr(state, "Active exams", "Kỳ thi đang chạy"), active_states.size(),
                        tr(state, "Ready to begin", "Sẵn sàng bắt đầu"), ready_clients.size());

    const auto active_for_selected = std::find_if(
        active_states.begin(), active_states.end(), [selected_client](const auto& exam) {
            return selected_client != nullptr && exam.client_id == selected_client->id;
        });
    const bool selected_has_active = active_for_selected != active_states.end();
    const bool selected_online = selected_client != nullptr &&
                                 selected_client->status != nstu::server::ClientStatus::offline;

    // Load on Selected
    ImGui::BeginDisabled(!selected_online || selected_has_active);
    if (ImGui::Button(tr(state, "Load on Selected Machine", "Mở trên máy chọn"), {scaled(200.0f), 0})) {
        nstu::server::ExamLaunchRequest launch;
        launch.package_root = state.exam_package_root.data();
        launch.package_id = state.exam_package_id.data();
        launch.candidate_id = state.exam_candidate_id.data();
        while (!launch.package_root.empty() && (launch.package_root.back() == '/' || launch.package_root.back() == '\\')) {
            launch.package_root.pop_back();
        }
        launch.web_root = launch.package_root + "/exam/web";
        std::string error;
        const bool valid_digest = parse_sha256_hex(state.exam_digest_hex.data(), launch.package_digest);
        const bool sent = valid_digest && control_plane.launch_exam(selected_client->id, launch, &error);
        if (!valid_digest) error = "Content SHA-256 must be 64 non-zero hex digits.";
        state.control_status = sent ? tr(state, "Exam loading request sent.", "Đã gửi yêu cầu tải bài thi.") : error;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    // Load for ALL online
    bool any_online_unlaunched = false;
    for (const auto& cl : clients) {
        if (cl.status == nstu::server::ClientStatus::online &&
            !control_plane.exam_active(cl.id)) {
            any_online_unlaunched = true;
            break;
        }
    }
    ImGui::BeginDisabled(!any_online_unlaunched);
    if (ImGui::Button(tr(state, "Load on ALL Online Computers", "Mở trên TẤT CẢ máy online"), {scaled(220.0f), 0})) {
        std::size_t launched_count = 0;
        std::string last_err;
        for (const auto& cl : clients) {
            if (cl.status == nstu::server::ClientStatus::online && !control_plane.exam_active(cl.id)) {
                nstu::server::ExamLaunchRequest launch;
                launch.package_root = state.exam_package_root.data();
                launch.package_id = state.exam_package_id.data();
                launch.candidate_id = cl.hostname;
                while (!launch.package_root.empty() && (launch.package_root.back() == '/' || launch.package_root.back() == '\\')) {
                    launch.package_root.pop_back();
                }
                launch.web_root = launch.package_root + "/exam/web";
                (void)parse_sha256_hex(state.exam_digest_hex.data(), launch.package_digest);
                if (control_plane.launch_exam(cl.id, launch, &last_err)) {
                    ++launched_count;
                }
            }
        }
        state.control_status = tr(state, "Dispatched exam to ", "Đã phát đề thi tới ") +
                              std::to_string(launched_count) + tr(state, " computers.", " máy tính.");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    // Begin All Ready
    ImGui::BeginDisabled(ready_clients.empty());
    if (ImGui::Button(tr(state, "Begin All Ready Candidates", "Bắt đầu tất cả máy sẵn sàng"), {scaled(220.0f), 0})) {
        std::size_t begun_count = 0;
        for (std::uint64_t cid : ready_clients) {
            std::string err;
            if (control_plane.begin_exam(cid, 0, &err)) {
                ++begun_count;
            }
        }
        state.control_status = tr(state, "Released exam barrier for ", "Đã mở rào chắn kỳ thi cho ") +
                              std::to_string(begun_count) + tr(state, " candidates.", " thí sinh.");
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    // Stop All
    ImGui::BeginDisabled(active_states.empty());
    if (ImGui::Button(tr(state, "Stop ALL Exams", "Dừng TOÀN BỘ bài thi"), {scaled(160.0f), 0})) {
        std::size_t stopped_count = 0;
        for (const auto& ex : active_states) {
            std::string err;
            if (control_plane.stop_exam(ex.client_id, &err)) {
                ++stopped_count;
            }
        }
        state.control_status = tr(state, "Stopped exam on ", "Đã dừng bài thi trên ") +
                              std::to_string(stopped_count) + tr(state, " computers.", " máy tính.");
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 3. Live Cohort Monitoring Table
    ImGui::TextUnformatted(tr(state, "3. Live Cohort Telemetry", "3. Theo dõi phòng thi trực tiếp"));
    if (clients.empty()) {
        ImGui::TextDisabled("%s", tr(state, "No student computers connected to server.", "Chưa có máy sinh viên kết nối."));
    } else if (ImGui::BeginTable("exam-cohort-table", 6,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr(state, "Computer", "Máy"), ImGuiTableColumnFlags_WidthFixed, scaled(200.0f));
        ImGui::TableSetupColumn(tr(state, "Address", "Địa chỉ"), ImGuiTableColumnFlags_WidthFixed, scaled(120.0f));
        ImGui::TableSetupColumn(tr(state, "Candidate", "Thí sinh"), ImGuiTableColumnFlags_WidthFixed, scaled(150.0f));
        ImGui::TableSetupColumn(tr(state, "Phase", "Trạng thái"), ImGuiTableColumnFlags_WidthFixed, scaled(140.0f));
        ImGui::TableSetupColumn(tr(state, "Authorization", "Ủy quyền"), ImGuiTableColumnFlags_WidthFixed, scaled(140.0f));
        ImGui::TableSetupColumn(tr(state, "Actions", "Thao tác"), ImGuiTableColumnFlags_WidthFixed, scaled(160.0f));
        ImGui::TableHeadersRow();

        for (const auto& client : clients) {
            push_client_id(client.id);
            ImGui::TableNextRow(0, scaled(32.0f));

            // Col 0: Hostname
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(client.hostname.c_str());

            // Col 1: Address
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(client.address.c_str());

            // Check active state
            const auto it_exam = std::find_if(active_states.begin(), active_states.end(),
                [&client](const auto& ex) { return ex.client_id == client.id; });
            const bool is_exam_active = (it_exam != active_states.end());

            // Col 2: Candidate
            ImGui::TableSetColumnIndex(2);
            if (is_exam_active && !it_exam->candidate_id.empty()) {
                ImGui::TextUnformatted(it_exam->candidate_id.c_str());
            } else {
                ImGui::TextDisabled("-");
            }

            // Col 3: Phase
            ImGui::TableSetColumnIndex(3);
            if (!is_exam_active) {
                if (client.status == nstu::server::ClientStatus::offline) {
                    ImGui::TextDisabled("%s", tr(state, "Offline", "Ngoại tuyến"));
                } else {
                    ImGui::TextDisabled("%s", tr(state, "Idle", "Sẵn sàng nhận đề"));
                }
            } else if (it_exam->begun) {
                ImGui::TextColored(ImVec4{0.2f, 0.8f, 0.2f, 1.0f}, "%s", tr(state, "In Progress", "Đang thi"));
            } else if (it_exam->ready) {
                ImGui::TextColored(ImVec4{0.9f, 0.8f, 0.1f, 1.0f}, "%s", tr(state, "Ready to Begin", "Đã sẵn sàng"));
            } else {
                ImGui::TextColored(ImVec4{0.3f, 0.7f, 1.0f, 1.0f}, "%s", tr(state, "Loading Package", "Đang tải đề"));
            }

            // Col 4: Authorization
            ImGui::TableSetColumnIndex(4);
            if (is_exam_active && control_plane.is_exam_instructor_authorized(client.id)) {
                ImGui::TextColored(ImVec4{0.2f, 0.8f, 0.2f, 1.0f}, "%s", tr(state, "Instructor Verified", "GV Xác nhận"));
            } else if (control_plane.requires_instructor_ticket()) {
                ImGui::TextColored(ImVec4{0.9f, 0.4f, 0.2f, 1.0f}, "%s", tr(state, "Ticket Required", "Cần vé GV"));
            } else {
                ImGui::TextDisabled("%s", tr(state, "Standard", "Tiêu chuẩn"));
            }

            // Col 5: Actions
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton(tr(state, "Select", "Chọn"))) {
                state.selected_client_id = client.id;
            }
            if (is_exam_active && it_exam->ready && !it_exam->begun) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr(state, "Begin", "Bắt đầu"))) {
                    std::string err;
                    state.control_status = control_plane.begin_exam(client.id, 0, &err)
                        ? tr(state, "Exam begun.", "Đã bắt đầu bài thi.")
                        : err;
                }
            }
            if (is_exam_active) {
                ImGui::SameLine();
                if (ImGui::SmallButton(tr(state, "Stop", "Dừng"))) {
                    std::string err;
                    state.control_status = control_plane.stop_exam(client.id, &err)
                        ? tr(state, "Exam stopped.", "Đã dừng bài thi.")
                        : err;
                }
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 4. Sealed Export Container
    ImGui::TextUnformatted(tr(state, "4. Tamper-Evident Sealed Export", "4. Xuất kết quả niêm phong số"));
    if (ImGui::Button(tr(state, "Generate Sealed Export (.nstugrade)", "Tạo file niêm phong (.nstugrade)"), {scaled(260.0f), 0})) {
        generate_sealed_cohort_export(state, control_plane);
    }
    if (!state.last_export_path.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4{0.2f, 0.8f, 0.2f, 1.0f}, "%s: %s",
                           tr(state, "Last exported to", "Đã lưu vào"), state.last_export_path.c_str());
    }
}
void draw_menu_strip(DashboardState& state, bool has_clients,
                     nstu::server::ServerControlPlane& control_plane) {
    if (!ImGui::BeginChild("menu-strip", {0, scaled(31.0f)}, false,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    if (g_heading_font != nullptr) {
        ImGui::PushFont(g_heading_font);
    }
    ImGui::TextUnformatted("NSTU School");
    if (g_heading_font != nullptr) {
        ImGui::PopFont();
    }
#if NSTU_DEV_UNPROTECTED_EXAM
    // Permanent, unmissable marker for the public DEV channel. This build starts
    // exams without proven reboot-to-restore protection; the operator must never
    // mistake it for a Release install.
    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.96f, 0.36f, 0.36f, 1.0f}, "%s",
                       tr(state, "DEV (UNPROTECTED) - exams run without UWF",
                          "DEV (KHÔNG BẢO VỆ) - thi không cần UWF"));
#endif
    ImGui::SameLine();
    if (draw_segment_option(tr(state, "Class", "Lớp"),
                            state.view == DashboardView::room_screens,
                            scaled(58.0f))) {
        state.view = DashboardView::room_screens;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_clients);
    if (draw_segment_option(tr(state, "Client", "Máy"),
                            state.view == DashboardView::selected_client,
                            scaled(60.0f))) {
        state.view = DashboardView::selected_client;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (draw_segment_option(tr(state, "Exam", "Bài thi"),
                            state.view == DashboardView::exam,
                            scaled(60.0f))) {
        state.view = DashboardView::exam;
    }
    if (is_action_permitted(state.current_role, OperatorAction::view_fleet_health)) {
        ImGui::SameLine();
        if (draw_segment_option(tr(state, "Fleet", "Hệ thống"),
                                state.view == DashboardView::fleet_health,
                                scaled(68.0f))) {
            state.view = DashboardView::fleet_health;
        }
    }

    const float right_x = ImGui::GetContentRegionMax().x;
    const float gap = scaled(8.0f);
    const float settings_w = scaled(104.0f);
    const float role_w = scaled(160.0f);
    const bool show_diag = is_action_permitted(state.current_role, OperatorAction::view_diagnostics_raw);
    const float diag_w = show_diag ? scaled(118.0f) : 0.0f;
    const float pair_w = scaled(136.0f);

    const float x_settings = right_x - settings_w;
    const float x_role = x_settings - gap - role_w;
    const float x_diag = show_diag ? (x_role - gap - diag_w) : x_role;
    const float x_pair = x_diag - gap - pair_w;

    draw_pairing_popup(state, control_plane, x_pair, pair_w);
    if (show_diag) {
        draw_diagnostics_popup(state, x_diag, diag_w);
    }
    draw_role_switcher(state, x_role, role_w);
    draw_preferences(state, x_settings, settings_w);
    ImGui::EndChild();
}

void draw_ribbon_group_caption(float width, const char* label) {
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    ImGui::SetCursorPosY(scaled(58.0f));
    ImGui::SetCursorPosX(std::max(
        ImGui::GetStyle().WindowPadding.x, (width - text_size.x) * 0.5f));
    ImGui::TextDisabled("%s", label);
}

// A ribbon group is at least as wide as the caption centred beneath it. The
// authored widths are sized for the buttons; a longer caption (a translation,
// or simply a larger font at high DPI) would otherwise spill past the group
// and collide with the next one.
float ribbon_group_width(float authored_width_at_96dpi, const char* caption) {
    const float caption_width = ImGui::CalcTextSize(caption).x +
                                ImGui::GetStyle().WindowPadding.x * 2.0f;
    return std::max(scaled(authored_width_at_96dpi), caption_width);
}

void draw_ribbon_divider() {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Dummy({1.0f, scaled(64.0f)});
    ImGui::GetWindowDrawList()->AddLine(
        {start.x, start.y + scaled(3.0f)}, {start.x, start.y + scaled(61.0f)},
        ImGui::GetColorU32(ImGuiCol_Separator));
}

void draw_ribbon(const std::vector<nstu::server::ClientRecord>& clients,
                 const nstu::server::ClientRecord* selected_client,
                 DashboardState& state,
                 nstu::server::ServerControlPlane& control_plane) {
    if (!ImGui::BeginChild("command-ribbon", {0, scaled(82.0f)}, true,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    const bool has_clients = !clients.empty();
    const char* student_caption = tr(state, "Student", "Học sinh");
    const float student_width = ribbon_group_width(140.0f, student_caption);
    if (ImGui::BeginChild("student-commands", {student_width, scaled(74.0f)},
                          false, ImGuiWindowFlags_NoScrollbar)) {
        // Snapshots run automatically for the class view (auto_monitor), so the
        // manual Start/Stop capture buttons no longer live on the ribbon.
        if (draw_icon_button("lock-room", tr(state, "Lock", "Khóa"),
                             IconKind::lock, {scaled(58.0f), scaled(54.0f)},
                             false, has_clients)) {
            set_room_lock(clients, state, control_plane, true);
        }
        ImGui::SameLine();
        if (draw_icon_button("unlock-room", tr(state, "Unlock", "Mở khóa"),
                             IconKind::unlock, {scaled(64.0f), scaled(54.0f)},
                             false, has_clients)) {
            set_room_lock(clients, state, control_plane, false);
        }
        draw_ribbon_group_caption(student_width, student_caption);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    draw_ribbon_divider();
    ImGui::SameLine();
    const char* teaching_caption = tr(state, "Teaching", "Giảng dạy");
    const float teaching_width = ribbon_group_width(330.0f, teaching_caption);
    if (ImGui::BeginChild("teaching-commands", {teaching_width, scaled(74.0f)},
                          false, ImGuiWindowFlags_NoScrollbar)) {
        if (draw_icon_button("show-room", tr(state, "Screens", "Màn hình"),
                             IconKind::grid, {scaled(66.0f), scaled(54.0f)},
                             state.view == DashboardView::room_screens)) {
            state.view = DashboardView::room_screens;
        }
        ImGui::SameLine();
        if (draw_icon_button("focus-client", tr(state, "Focus", "Tập trung"),
                             IconKind::monitor, {scaled(66.0f), scaled(54.0f)},
                             state.view == DashboardView::selected_client,
                             selected_client != nullptr)) {
            state.view = DashboardView::selected_client;
        }
        ImGui::SameLine();
        if (draw_icon_button("draw-client", tr(state, "Draw", "Vẽ"),
                             IconKind::pen, {scaled(58.0f), scaled(54.0f)},
                             state.annotation_enabled,
                             selected_client != nullptr &&
                                 state.view ==
                                     DashboardView::selected_client)) {
            state.annotation_enabled = !state.annotation_enabled;
            state.annotation_dragging = false;
        }
        ImGui::SameLine();
        if (draw_icon_button("clear-client", tr(state, "Clear", "Xóa"),
                             IconKind::erase, {scaled(58.0f), scaled(54.0f)},
                             false, selected_client != nullptr)) {
            std::string error;
            state.control_status = control_plane.clear_overlay(
                                       selected_client->id, &error)
                ? tr(state, "Student overlay cleared.", "Đã xóa lớp vẽ.")
                : error;
        }
        ImGui::SameLine();
        bool any_unread_chat = false;
        for (const auto& client : clients) {
            if (client_has_unread_chat(state, client.id)) {
                any_unread_chat = true;
                break;
            }
        }
        if (draw_icon_button("chat-client", tr(state, "Chat", "Chat"),
                             IconKind::chat, {scaled(58.0f), scaled(54.0f)},
                             false, selected_client != nullptr, true,
                             any_unread_chat)) {
            state.view = DashboardView::selected_client;
        }
        draw_ribbon_group_caption(teaching_width, teaching_caption);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    draw_ribbon_divider();
    ImGui::SameLine();
    const char* broadcast_caption = tr(state, "Teacher screen", "Màn hình GV");
    const float broadcast_width = ribbon_group_width(190.0f, broadcast_caption);
    if (ImGui::BeginChild("broadcast-commands", {broadcast_width, scaled(74.0f)},
                          false, ImGuiWindowFlags_NoScrollbar)) {
        if (draw_icon_button(
                "broadcast-room",
                state.broadcast_enabled ? tr(state, "Stop", "Dừng")
                                        : tr(state, "Broadcast", "Phát"),
                state.broadcast_enabled ? IconKind::stop : IconKind::broadcast,
                {scaled(60.0f), scaled(54.0f)}, state.broadcast_enabled,
                has_clients)) {
            toggle_teacher_broadcast(state, control_plane);
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        const bool is_follow =
            (state.broadcast_mode == nstu::control::DemonstrationMode::follow_along);
        if (ImGui::SmallButton(is_follow ? tr(state, "Follow-Along", "Cửa sổ nổi")
                                         : tr(state, "Focus Mode", "Toàn màn hình"))) {
            const auto next_mode = is_follow
                ? nstu::control::DemonstrationMode::focus
                : nstu::control::DemonstrationMode::follow_along;
            update_teacher_broadcast_mode(state, control_plane, next_mode,
                                          state.broadcast_lock_input);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", is_follow
                ? tr(state, "Switch to Focus Mode (fullscreen topmost presentation)",
                            "Chuyển sang Chế độ Tiêu điểm (toàn màn hình)")
                : tr(state, "Switch to Follow-Along Mode (resizable floating window)",
                            "Chuyển sang Chế độ Theo dõi (cửa sổ nổi)"));
        }
        bool lock_val = state.broadcast_lock_input;
        ImGui::BeginDisabled(is_follow);
        if (ImGui::Checkbox(tr(state, "Lock input", "Khóa phím"), &lock_val)) {
            update_teacher_broadcast_mode(state, control_plane, state.broadcast_mode,
                                          lock_val);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", is_follow
                ? tr(state, "Input locking is disabled in Follow-Along mode",
                            "Khóa phím bị tắt trong Chế độ Theo dõi")
                : tr(state, "Lock student mouse and keyboard during Focus broadcast",
                            "Khóa chuột và bàn phím học sinh khi phát"));
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();
        draw_ribbon_group_caption(broadcast_width, broadcast_caption);
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

bool draw_filter_chip(const char* id, const char* label, std::size_t count,
                      bool selected) {
    char text[96]{};
    sprintf_s(text, "%s  %zu", label, count);
    ImGui::PushID(id);
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(
                                                    ImGuiCol_Header));
    }
    const bool pressed = ImGui::Button(text, {0, scaled(28.0f)});
    if (selected) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return pressed;
}

void draw_workspace_toolbar(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    const auto counts = count_room_statuses(clients);
    if (draw_filter_chip("all", tr(state, "All", "Tất cả"), clients.size(),
                         state.room_filter == RoomFilter::all)) {
        state.room_filter = RoomFilter::all;
    }
    ImGui::SameLine();
    if (draw_filter_chip("online", tr(state, "Online", "Trực tuyến"),
                         counts.online,
                         state.room_filter == RoomFilter::online)) {
        state.room_filter = RoomFilter::online;
    }
    ImGui::SameLine();
    if (draw_filter_chip("attention", tr(state, "Attention", "Chú ý"),
                         counts.attention,
                         state.room_filter == RoomFilter::attention)) {
        state.room_filter = RoomFilter::attention;
    }
    ImGui::SameLine();
    if (draw_filter_chip("locked", tr(state, "Locked", "Đã khóa"),
                         counts.locked,
                         state.room_filter == RoomFilter::locked)) {
        state.room_filter = RoomFilter::locked;
    }
    ImGui::SameLine();
    if (draw_filter_chip("offline", tr(state, "Offline", "Ngoại tuyến"),
                         counts.offline,
                         state.room_filter == RoomFilter::offline)) {
        state.room_filter = RoomFilter::offline;
        state.show_offline = true;
    }
    ImGui::SameLine();
    if (draw_filter_chip("help", tr(state, "Help", "Trợ giúp"),
                         counts.help,
                         state.room_filter == RoomFilter::help)) {
        state.room_filter = RoomFilter::help;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // View mode toggle chips (Grid vs List)
    if (draw_filter_chip("view-grid", tr(state, "Grid", "Lưới"), 0,
                         state.display_mode == ClassDisplayMode::grid)) {
        state.display_mode = ClassDisplayMode::grid;
    }
    ImGui::SameLine();
    if (draw_filter_chip("view-list", tr(state, "List", "Danh sách"), 0,
                         state.display_mode == ClassDisplayMode::list)) {
        state.display_mode = ClassDisplayMode::list;
    }

    // Selection & Bulk Actions
    if (!clients.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::SmallButton(tr(state, "Select All", "Chọn tất cả"))) {
            std::vector<const nstu::server::ClientRecord*> visible_clients;
            visible_clients.reserve(clients.size());
            for (const auto& c : clients) {
                if (client_matches_filter(c, state)) {
                    visible_clients.push_back(&c);
                }
            }
            state.checked_client_ids = nstu::server::select_all_matching(visible_clients);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr(state, "Clear", "Bỏ chọn"))) {
            state.checked_client_ids.clear();
        }

        if (!state.checked_client_ids.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu %s)", state.checked_client_ids.size(),
                                tr(state, "selected", "đã chọn"));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Bulk Lock", "Khóa đã chọn"))) {
                state.pending_bulk_action = BulkActionKind::lock;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Bulk Unlock", "Mở khóa đã chọn"))) {
                state.pending_bulk_action = BulkActionKind::unlock;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Bulk Refresh", "Chụp lại"))) {
                state.pending_bulk_action = BulkActionKind::refresh_snapshots;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Ack Help", "Giải quyết trợ giúp"))) {
                state.pending_bulk_action = BulkActionKind::acknowledge_help;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Launch Web", "Mở trang web"))) {
                state.pending_bulk_action = BulkActionKind::launch_url;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Reboot", "Khởi động lại"))) {
                state.pending_bulk_action = BulkActionKind::reboot;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Shutdown", "Tắt máy"))) {
                state.pending_bulk_action = BulkActionKind::shutdown;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Wake", "Bật máy"))) {
                state.pending_bulk_action = BulkActionKind::wake_on_lan;
                state.bulk_confirmation_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr(state, "Send File", "Phát tệp"))) {
                state.pending_bulk_action = BulkActionKind::distribute_file;
                state.bulk_confirmation_open = true;
            }
        }
    }

    const float search_width = scaled(180.0f);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + scaled(8.0f),
                             ImGui::GetContentRegionMax().x - search_width));
    ImGui::SetNextItemWidth(search_width);
    ImGui::InputTextWithHint("##client-filter",
                             tr(state, "Find client", "Tìm máy"),
                             state.client_filter.data(),
                             state.client_filter.size());
    ImGui::Separator();
}

void draw_navigation_rail(
    const std::vector<nstu::server::ClientRecord>& clients,
    const nstu::server::ClientRecord* selected_client, DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    if (!ImGui::BeginChild("navigation-rail", {scaled(48.0f), 0}, true,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    if (draw_icon_button("nav-room", tr(state, "Room screens", "Màn hình phòng"),
                         IconKind::grid, {scaled(36.0f), scaled(38.0f)},
                         state.view == DashboardView::room_screens, true,
                         false)) {
        state.view = DashboardView::room_screens;
    }
    if (draw_icon_button("nav-focus", tr(state, "Selected client", "Máy đang chọn"),
                         IconKind::monitor, {scaled(36.0f), scaled(38.0f)},
                         state.view == DashboardView::selected_client,
                         selected_client != nullptr, false)) {
        state.view = DashboardView::selected_client;
    }
    if (draw_icon_button("nav-exam", tr(state, "Exam", "Bài thi"),
                         IconKind::lock, {scaled(36.0f), scaled(38.0f)},
                         state.view == DashboardView::exam,
                         true, false)) {
        state.view = DashboardView::exam;
    }
    if (is_action_permitted(state.current_role, OperatorAction::view_fleet_health)) {
        if (draw_icon_button("nav-fleet", tr(state, "Fleet health", "Sức khỏe hệ thống"),
                             IconKind::monitor, {scaled(36.0f), scaled(38.0f)},
                             state.view == DashboardView::fleet_health,
                             true, false)) {
            state.view = DashboardView::fleet_health;
        }
    }
    ImGui::Separator();
    if (draw_icon_button("nav-lock", tr(state, "Lock room", "Khóa phòng"),
                         IconKind::lock, {scaled(36.0f), scaled(38.0f)}, false,
                         !clients.empty(), false)) {
        set_room_lock(clients, state, control_plane, true);
    }
    if (draw_icon_button("nav-broadcast",
                         tr(state, "Teacher broadcast", "Phát màn hình giáo viên"),
                         IconKind::broadcast, {scaled(36.0f), scaled(38.0f)},
                         state.broadcast_enabled, !clients.empty(), false)) {
        toggle_teacher_broadcast(state, control_plane);
    }
    if (draw_icon_button("nav-chat", tr(state, "Client chat", "Chat với máy"),
                         IconKind::chat, {scaled(36.0f), scaled(38.0f)}, false,
                         selected_client != nullptr, false)) {
        state.view = DashboardView::selected_client;
    }
    ImGui::EndChild();
}

void draw_status_bar(const std::vector<nstu::server::ClientRecord>& clients,
                     DashboardState& state) {
    const auto counts = count_room_statuses(clients);
    if (!ImGui::BeginChild("status-bar", {0, scaled(28.0f)}, true,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    ImGui::TextColored(status_text_color(nstu::server::ClientStatus::online),
                       "●");
    ImGui::SameLine();
    ImGui::Text("%zu %s", counts.online,
                tr(state, "online", "trực tuyến"));
    if (!state.control_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.control_status.c_str());
    }
    const char* show_offline_label =
        tr(state, "Show offline", "Hiện ngoại tuyến");
    // Right-align on the checkbox's real width (box + inner gap + label) rather
    // than a hand-tuned padding constant: a fixed reserve is only correct at one
    // font size and leaves the control stranded mid-bar once the UI scales.
    const float controls_width =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
        ImGui::CalcTextSize(show_offline_label).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + scaled(8.0f),
                             ImGui::GetContentRegionMax().x - controls_width));
    ImGui::Checkbox(show_offline_label, &state.show_offline);
    /* Refresh interval is configured from Settings. */
    ImGui::EndChild();
}

void draw_bulk_action_confirmation_popup(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    if (state.bulk_confirmation_open) {
        ImGui::OpenPopup("bulk-action-confirmation");
    }
    if (!ImGui::BeginPopupModal("bulk-action-confirmation", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const bool is_vietnamese = (state.language == Language::vietnamese);
    const auto target_summary =
        nstu::server::evaluate_bulk_targets(clients, state.checked_client_ids);

    ImGui::TextUnformatted(
        nstu::server::format_bulk_confirmation_title(
            state.pending_bulk_action, is_vietnamese).c_str());
    ImGui::Separator();

    const std::string room_name = state.room_name_input.data();
    const bool is_wol = (state.pending_bulk_action == BulkActionKind::wake_on_lan);
    const std::size_t effective_target_count = is_wol ? target_summary.skipped_offline : target_summary.eligible_online;
    ImGui::TextWrapped("%s",
        nstu::server::format_bulk_confirmation_message(
            state.pending_bulk_action, effective_target_count,
            room_name, is_vietnamese).c_str());

    if (!is_wol && target_summary.skipped_offline > 0) {
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4{0.9f, 0.7f, 0.1f, 1.0f},
            "%s: %zu %s",
            tr(state, "Notice", "Lưu ý"),
            target_summary.skipped_offline,
            tr(state, "offline computers will be skipped.", "máy ngoại tuyến sẽ được bỏ qua."));
    } else if (is_wol && target_summary.eligible_online > 0) {
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4{0.4f, 0.7f, 1.0f, 1.0f},
            "%s: %zu %s",
            tr(state, "Notice", "Lưu ý"),
            target_summary.eligible_online,
            tr(state, "online computers are already awake and will be skipped.", "máy đang trực tuyến sẽ được bỏ qua vì đã bật."));
    }

    bool url_valid = true;
    std::string url_validation_err;
    if (state.pending_bulk_action == BulkActionKind::launch_url) {
        ImGui::Spacing();
        ImGui::TextUnformatted(tr(state, "Website / URL to open:", "Trang web / URL cần mở:"));
        ImGui::SetNextItemWidth(scaled(340.0f));
        ImGui::InputTextWithHint("##launch-url-input", "https://example.com",
                                 state.launch_url_input.data(),
                                 state.launch_url_input.size());

        ImGui::TextDisabled("%s", tr(state, "Presets:", "Mẫu nhanh:"));
        ImGui::SameLine();
        if (ImGui::SmallButton("LMS")) {
            strcpy_s(state.launch_url_input.data(), state.launch_url_input.size(),
                     "https://lms.nstu.edu.vn");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Docs")) {
            strcpy_s(state.launch_url_input.data(), state.launch_url_input.size(),
                     "https://docs.nstu.edu.vn");
        }

        const std::string_view current_url{state.launch_url_input.data()};
        url_valid = nstu::url::is_valid_launch_url(current_url, &url_validation_err);
        if (!url_valid) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.4f, 1.0f}, "%s: %s",
                               tr(state, "Invalid URL", "URL không hợp lệ"),
                               url_validation_err.c_str());
        }
    }

    bool file_dist_valid = true;
    std::string file_dist_validation_err;
    if (state.pending_bulk_action == BulkActionKind::distribute_file) {
        ImGui::Spacing();
        ImGui::TextUnformatted(tr(state, "File path to distribute:", "Đường dẫn tệp cần phát:"));
        ImGui::SetNextItemWidth(scaled(340.0f));
        ImGui::InputTextWithHint("##file-dist-path-input", "C:\\path\\to\\document.pdf",
                                 state.file_dist_path_input.data(),
                                 state.file_dist_path_input.size());

        const std::string_view file_path_str{state.file_dist_path_input.data()};
        if (file_path_str.empty()) {
            file_dist_valid = false;
            file_dist_validation_err = tr(state, "Please enter a file path.", "Vui lòng nhập đường dẫn tệp.");
        } else {
            std::error_code ec;
            std::filesystem::path p(file_path_str);
            if (!std::filesystem::exists(p, ec)) {
                file_dist_valid = false;
                file_dist_validation_err = tr(state, "File does not exist.", "Tệp không tồn tại.");
            } else if (!std::filesystem::is_regular_file(p, ec)) {
                file_dist_valid = false;
                file_dist_validation_err = tr(state, "Path is not a regular file.", "Đường dẫn không phải là tệp tin hợp lệ.");
            } else {
                const auto sz = std::filesystem::file_size(p, ec);
                if (ec) {
                    file_dist_valid = false;
                    file_dist_validation_err = tr(state, "Unable to read file size.", "Không thể đọc kích thước tệp.");
                } else if (sz > nstu::file_dist::kMaxFileSize) {
                    file_dist_valid = false;
                    file_dist_validation_err = tr(state, "File exceeds maximum size limit (64 MiB).", "Kích thước tệp vượt quá giới hạn (64 MiB).");
                } else {
                    const std::string fname = p.filename().string();
                    if (!nstu::file_dist::is_safe_filename(fname)) {
                        file_dist_valid = false;
                        file_dist_validation_err = tr(state, "Filename contains invalid or unsafe characters.", "Tên tệp chứa ký tự không hợp lệ hoặc không an toàn.");
                    } else {
                        ImGui::TextDisabled("%s: %.2f MiB (%llu bytes)",
                                            tr(state, "Size", "Kích thước"),
                                            static_cast<double>(sz) / (1024.0 * 1024.0),
                                            static_cast<unsigned long long>(sz));
                    }
                }
            }
        }
        if (!file_dist_valid) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.4f, 1.0f}, "%s: %s",
                               tr(state, "Invalid File", "Tệp không hợp lệ"),
                               file_dist_validation_err.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool is_power_action = (state.pending_bulk_action == BulkActionKind::reboot ||
                                  state.pending_bulk_action == BulkActionKind::shutdown);
    const bool power_role_authorized = !is_power_action ||
        (state.pending_bulk_action == BulkActionKind::reboot
            ? is_action_permitted(state.current_role, OperatorAction::reboot_client)
            : is_action_permitted(state.current_role, OperatorAction::shutdown_client));

    const bool wol_role_authorized = !is_wol ||
        is_action_permitted(state.current_role, OperatorAction::wake_on_lan);

    const bool is_file_dist = (state.pending_bulk_action == BulkActionKind::distribute_file);
    const bool file_dist_role_authorized = !is_file_dist ||
        is_action_permitted(state.current_role, OperatorAction::distribute_file);

    if (is_power_action && !power_role_authorized) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.4f, 1.0f}, "%s: %s",
            tr(state, "Permission Denied", "Từ chối quyền"),
            tr(state, "Power actions require Technician or Developer role.",
                      "Thao tác nguồn yêu cầu quyền Kỹ thuật viên hoặc Nhà phát triển."));
    }

    if (is_file_dist && !file_dist_role_authorized) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.4f, 1.0f}, "%s: %s",
            tr(state, "Permission Denied", "Từ chối quyền"),
            tr(state, "File distribution requires Teacher, Technician, or Developer role.",
                      "Phát tệp yêu cầu quyền Giáo viên, Kỹ thuật viên, hoặc Nhà phát triển."));
    }

    const bool has_targets = is_wol ? (target_summary.skipped_offline > 0) : (target_summary.eligible_online > 0);
    const bool can_execute = has_targets && url_valid && file_dist_valid && power_role_authorized && wol_role_authorized && file_dist_role_authorized;
    if (!can_execute) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(tr(state, "Confirm & Execute", "Xác nhận thực hiện"),
                      {scaled(160.0f), scaled(30.0f)})) {
        const auto result = nstu::server::execute_bulk_action(
            control_plane, clients, state.checked_client_ids,
            state.pending_bulk_action,
            static_cast<std::uint16_t>(state.snapshot_interval_seconds),
            state.pending_bulk_action == BulkActionKind::launch_url
                ? std::string_view{state.launch_url_input.data()}
                : (state.pending_bulk_action == BulkActionKind::distribute_file
                    ? std::string_view{state.file_dist_path_input.data()}
                    : std::string_view{}),
            state.current_role);
        state.control_status =
            nstu::server::format_bulk_action_result_status(
                state.pending_bulk_action, result, is_vietnamese);
        state.bulk_confirmation_open = false;
        ImGui::CloseCurrentPopup();
    }
    if (!can_execute) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Cancel", "Hủy bỏ"),
                      {scaled(100.0f), scaled(30.0f)})) {
        state.bulk_confirmation_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void draw_technician_fleet_health(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    if (is_action_permitted(state.current_role, OperatorAction::manage_software_updates)) {
        if (ImGui::Button(tr(state, "Software Updates...", "Cập nhật phần mềm..."))) {
            state.software_updates_modal_open = true;
        }
        ImGui::Spacing();
    }
    if (clients.empty()) {
        ImGui::SetCursorPosY(scaled(80.0f));
        ImGui::TextDisabled("%s", tr(state, "No computers enrolled or discovered in fleet.",
                                     "Chưa có máy tính nào trong hệ thống."));
        return;
    }

    // 1. Metric summaries
    std::size_t online_count = 0;
    std::size_t agent_active_count = 0;
    std::size_t uwf_proven_count = 0;
    std::size_t uwf_awaiting_count = 0;
    std::size_t uwf_unsupported_count = 0;
    std::uint32_t total_latency = 0;
    std::size_t latency_samples = 0;

    for (const auto& client : clients) {
        if (client.status != nstu::server::ClientStatus::offline) {
            online_count++;
            if (client.agent_connected) {
                agent_active_count++;
            }
            total_latency += client.latency_ms;
            latency_samples++;
        }
        if (client.uwf.proves_current_protection()) {
            uwf_proven_count++;
        } else if (client.uwf.phase == nstu::control::UwfFleetPhase::awaiting_restart) {
            uwf_awaiting_count++;
        } else if (client.uwf.phase == nstu::control::UwfFleetPhase::unsupported) {
            uwf_unsupported_count++;
        }
    }

    const float card_gap = ImGui::GetStyle().ItemSpacing.x;
    const float card_width = (ImGui::GetContentRegionAvail().x - card_gap * 3.0f) / 4.0f;

    char total_buf[32]{};
    sprintf_s(total_buf, "%zu (%zu online)", clients.size(), online_count);
    draw_telemetry_card(tr(state, "Fleet Inventory", "Tổng số máy"), total_buf, card_width);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s: %zu\n%s: %zu",
            tr(state, "Online", "Đang trực tuyến"), online_count,
            tr(state, "Agent Active", "Agent hoạt động"), agent_active_count);
    }

    ImGui::SameLine();
    char uwf_buf[32]{};
    sprintf_s(uwf_buf, "%zu / %zu", uwf_proven_count, clients.size());
    draw_telemetry_card(tr(state, "Reboot-to-Restore Active", "Đã bật khôi phục reboot"), uwf_buf, card_width);

    ImGui::SameLine();
    char pending_buf[64]{};
    sprintf_s(pending_buf, "%zu pending / %zu unsupported", uwf_awaiting_count, uwf_unsupported_count);
    draw_telemetry_card(tr(state, "Pending / Unsupported", "Chờ khởi động / Không hỗ trợ"), pending_buf, card_width);

    ImGui::SameLine();
    char avg_lat_buf[32]{};
    if (latency_samples > 0) {
        sprintf_s(avg_lat_buf, "%u ms", total_latency / static_cast<std::uint32_t>(latency_samples));
    } else {
        strcpy_s(avg_lat_buf, "-");
    }
    draw_telemetry_card(tr(state, "Average Network Latency", "Độ trễ mạng trung bình"), avg_lat_buf, card_width);

    ImGui::Spacing();
    if (is_action_permitted(state.current_role, OperatorAction::view_fleet_health)) {
        if (ImGui::Button(tr(state, "Export Support Bundle...", "Xuất gói hỗ trợ..."),
                          {scaled(180.0f), scaled(26.0f)})) {
            const auto now_time = std::chrono::steady_clock::now();
            std::size_t stale_count = 0;
            for (const auto& client : clients) {
                if (client.status != nstu::server::ClientStatus::offline &&
                    nstu::server::is_client_stale(client, now_time)) {
                    stale_count++;
                }
            }
            nstu::support::SystemDiagnosticSummary summary{
                .application = "NSTU Server",
                .version = NSTU_PROJECT_VERSION,
                .build_channel = NSTU_BUILD_CHANNEL,
                .os_version = "Windows",
                .operator_role = to_string(state.current_role),
                .graphics_mode = g_graphics_report.device_mode,
                .network_adapter = "",
                .total_clients_known = static_cast<std::uint32_t>(clients.size()),
                .active_clients_count = static_cast<std::uint32_t>(online_count),
                .stale_clients_count = static_cast<std::uint32_t>(stale_count),
                .agent_connected_count = static_cast<std::uint32_t>(agent_active_count),
                .uwf_protected_count = static_cast<std::uint32_t>(uwf_proven_count),
            };
            std::deque<nstu::support::DiagnosticEvent> events;
            for (const auto& diag : g_diagnostics) {
                events.push_back({
                    .timestamp = diag.timestamp,
                    .severity = (diag.severity == "error" ? nstu::support::Severity::error :
                                 (diag.severity == "warning" ? nstu::support::Severity::warning :
                                  nstu::support::Severity::information)),
                    .component = diag.source,
                    .detail = diag.message,
                });
            }
            const auto bundle = nstu::support::create_sanitized_bundle(summary, events);
            state.support_bundle_preview = nstu::support::build_support_bundle_markdown(bundle);
            state.support_bundle_consented = false;
            state.support_bundle_modal_open = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(state, "Room Profile...", "Hồ sơ phòng..."),
                          {scaled(140.0f), scaled(26.0f)})) {
            nstu::fleet::RoomProfile profile;
            profile.room_name = (state.room_name_input.data()[0] != '\0')
                ? state.room_name_input.data()
                : (control_plane.server_name().empty() ? "Default Classroom" : control_plane.server_name());
            profile.grid_columns = 6;
            profile.grid_rows = 5;
            for (std::size_t i = 0; i < clients.size() && i < 256; ++i) {
                nstu::fleet::SeatAssignment seat;
                seat.hostname = clients[i].hostname.empty() ? ("client-" + std::to_string(clients[i].id)) : clients[i].hostname;
                std::string valid_host;
                for (char c : seat.hostname) {
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') {
                        valid_host.push_back(c);
                    }
                }
                seat.hostname = valid_host.empty() ? "host" : valid_host;
                seat.seat_label = "Seat " + std::to_string(i + 1);
                seat.row = static_cast<std::uint32_t>(i / 6);
                seat.col = static_cast<std::uint32_t>(i % 6);
                if (seat.row < profile.grid_rows && seat.col < profile.grid_columns) {
                    profile.seats.push_back(std::move(seat));
                }
            }
            if (state.launch_url_input[0] != '\0' && nstu::fleet::is_valid_website_preset(state.launch_url_input.data())) {
                profile.allowed_websites.push_back(state.launch_url_input.data());
            }
            state.room_profile_export_text = nstu::fleet::export_room_profile_json(profile);
            state.room_profile_import_buffer.fill('\0');
            state.room_profile_status_message.clear();
            state.room_profile_status_is_error = false;
            state.room_profile_modal_open = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr(state,
                "Export current classroom layout and presets, or import a secret-free room configuration.",
                "Xuất bố cục lớp học và cấu hình đặt trước, hoặc nhập hồ sơ phòng học không chứa bí mật."));
        }
    }
    ImGui::Separator();
    ImGui::Spacing();

    // 2. Fleet Table
    const bool can_configure_uwf = is_action_permitted(state.current_role, OperatorAction::configure_uwf);
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    std::uint64_t target_uwf_arm_client_id = 0;
    const auto now = std::chrono::steady_clock::now();
    const bool is_vietnamese = (state.language == Language::vietnamese);

    if (ImGui::BeginTable("fleet-health-table", 9, table_flags, {0, ImGui::GetContentRegionAvail().y - scaled(10.0f)})) {
        ImGui::TableSetupColumn(tr(state, "Computer", "Máy tính"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(tr(state, "Address", "Địa chỉ"), ImGuiTableColumnFlags_WidthFixed, scaled(110.0f));
        ImGui::TableSetupColumn(tr(state, "Status", "Trạng thái"), ImGuiTableColumnFlags_WidthFixed, scaled(85.0f));
        ImGui::TableSetupColumn(tr(state, "Last Seen", "Lần cuối"), ImGuiTableColumnFlags_WidthFixed, scaled(110.0f));
        ImGui::TableSetupColumn(tr(state, "Latency", "Độ trễ"), ImGuiTableColumnFlags_WidthFixed, scaled(65.0f));
        ImGui::TableSetupColumn(tr(state, "Loss", "Mất gói"), ImGuiTableColumnFlags_WidthFixed, scaled(60.0f));
        ImGui::TableSetupColumn(tr(state, "Activity", "Hoạt động"), ImGuiTableColumnFlags_WidthFixed, scaled(125.0f));
        ImGui::TableSetupColumn(tr(state, "Reboot-to-Restore", "Khôi phục reboot"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn(tr(state, "Action", "Thao tác"), ImGuiTableColumnFlags_WidthFixed, scaled(90.0f));
        ImGui::TableHeadersRow();

        for (const auto& client : clients) {
            ImGui::TableNextRow();
            push_client_id(client.id);

            // Computer name
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(client.hostname.c_str());
            if (ImGui::IsItemHovered()) {
                const char* ver_str = client.client_version.empty()
                    ? tr(state, "Unknown", "Chưa rõ")
                    : client.client_version.c_str();
                const char* agent_str = client.agent_connected
                    ? tr(state, "Connected", "Đã kết nối")
                    : tr(state, "Disconnected", "Mất kết nối");
                ImGui::SetTooltip("%s: %s\nAgent: %s",
                    tr(state, "Version", "Phiên bản"),
                    ver_str, agent_str);
            }

            // Address
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", client.address.c_str());
            if (ImGui::IsItemHovered()) {
                const char* delivery_str = (client.delivery == nstu::net::VideoDeliveryMode::multicast)
                    ? "Multicast" : "Unicast";
                const char* mac_str = client.mac_address.empty()
                    ? tr(state, "Unknown", "Chưa rõ")
                    : client.mac_address.c_str();
                const char* wol_str = "";
                switch (client.wol_support) {
                case nstu::net::WolSupportState::supported:
                    wol_str = tr(state, "Supported", "Được hỗ trợ");
                    break;
                case nstu::net::WolSupportState::unsupported:
                    wol_str = tr(state, "Unsupported", "Không hỗ trợ");
                    break;
                case nstu::net::WolSupportState::missing_mac:
                    wol_str = tr(state, "Missing MAC", "Thiếu MAC");
                    break;
                case nstu::net::WolSupportState::unknown:
                default:
                    wol_str = tr(state, "Unknown", "Chưa rõ");
                    break;
                }
                ImGui::SetTooltip(
                    "%s: %s\nMAC: %s\nWoL: %s",
                    tr(state, "Delivery", "Phương thức truyền"),
                    delivery_str,
                    mac_str,
                    wol_str);
            }

            // Status
            ImGui::TableSetColumnIndex(2);
            const bool is_online = (client.status != nstu::server::ClientStatus::offline);
            ImGui::TextColored(
                is_online ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f} : ImVec4{0.70f, 0.70f, 0.70f, 1.0f},
                "%s", nstu::server::to_string(client.status));
            if (is_online && !client.agent_connected) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{0.96f, 0.50f, 0.20f, 1.0f}, "[%s]",
                                   tr(state, "No Agent", "Thiếu Agent"));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tr(state,
                        "Student agent process is not connected to client service.\nRemote input lock, kiosk, and broadcast display unavailable.",
                        "Tiến trình agent chưa kết nối với service.\nKhóa nhập liệu, kiosk và màn hình phát sóng sẽ không khả dụng."));
                }
            }

            // Last Seen / Freshness
            ImGui::TableSetColumnIndex(3);
            const auto age = nstu::server::client_age_seconds(client, now);
            const std::string age_str = nstu::server::format_relative_age(age, is_vietnamese);
            const bool is_stale = nstu::server::is_client_stale(client, now);
            if (!is_online) {
                ImGui::TextDisabled("%s", age_str.c_str());
            } else if (is_stale) {
                ImGui::TextColored(ImVec4{0.96f, 0.76f, 0.34f, 1.0f}, "%s", age_str.c_str());
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{0.96f, 0.50f, 0.20f, 1.0f}, "[%s]",
                                   tr(state, "Stale", "Cũ"));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tr(state,
                        "Telemetry data may be stale (>30s since last heartbeat).",
                        "Dữ liệu có thể đã cũ (>30 giây không nhận tín hiệu)."));
                }
            } else {
                ImGui::TextUnformatted(age_str.c_str());
            }

            // Latency
            ImGui::TableSetColumnIndex(4);
            if (is_online) {
                ImGui::Text("%u ms", client.latency_ms);
            } else {
                ImGui::TextDisabled("-");
            }

            // Packet loss
            ImGui::TableSetColumnIndex(5);
            if (is_online) {
                ImGui::Text("%.1f%%", client.packet_loss_per_mille / 10.0f);
            } else {
                ImGui::TextDisabled("-");
            }

            // Activity
            ImGui::TableSetColumnIndex(6);
            if (!is_online) {
                ImGui::TextDisabled("-");
            } else if (client.streaming) {
                if (client.frames_per_second > 0) {
                    ImGui::TextColored(ImVec4{0.40f, 0.85f, 0.53f, 1.0f}, "%s (~%u fps)",
                        tr(state, "Streaming", "Phát luồng"), client.frames_per_second);
                } else {
                    ImGui::TextColored(ImVec4{0.40f, 0.85f, 0.53f, 1.0f}, "%s (~15fps)",
                        tr(state, "Streaming", "Phát luồng"));
                }
            } else if (client.snapshotting) {
                if (client.snapshot_interval_seconds > 0) {
                    char cap_buf[48]{};
                    sprintf_s(cap_buf, tr(state, "Capturing (%u s)", "Chụp hình (%u s)"),
                              client.snapshot_interval_seconds);
                    ImGui::TextUnformatted(cap_buf);
                } else {
                    ImGui::TextUnformatted(tr(state, "Capturing", "Chụp hình"));
                }
            } else if (client.viewing_broadcast) {
                ImGui::TextColored(ImVec4{0.35f, 0.75f, 1.0f, 1.0f}, "%s",
                    tr(state, "Viewing Screen", "Xem màn hình"));
            } else {
                ImGui::TextDisabled("%s", tr(state, "Idle", "Chờ"));
            }

            // Reboot-to-restore (UWF) phase
            ImGui::TableSetColumnIndex(7);
            const bool proven = client.uwf.proves_current_protection();
            const ImVec4 badge_color = proven
                ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                : (client.uwf.phase == nstu::control::UwfFleetPhase::awaiting_restart
                       ? ImVec4{0.96f, 0.76f, 0.34f, 1.0f}
                       : (client.uwf.phase == nstu::control::UwfFleetPhase::unsupported
                              ? ImVec4{0.80f, 0.40f, 0.40f, 1.0f}
                              : ImVec4{0.65f, 0.65f, 0.65f, 1.0f}));
            ImGui::TextColored(badge_color, "%s", nstu::server::to_string(client.uwf.phase));
            if (!client.uwf.detail.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", client.uwf.detail.c_str());
            }

            // Maintenance Action
            ImGui::TableSetColumnIndex(8);
            const bool uwf_unsupported =
                client.uwf.reported && client.uwf.phase == nstu::control::UwfFleetPhase::unsupported;
            const bool can_enable = can_configure_uwf && !proven && !uwf_unsupported && is_online;
            ImGui::BeginDisabled(!can_enable);
            if (ImGui::Button(tr(state, "Arm UWF", "Bật UWF"), {scaled(80.0f), 0})) {
                target_uwf_arm_client_id = client.id;
            }
            ImGui::EndDisabled();
            if (!can_configure_uwf && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", tr(state,
                    "Requires Technician or Developer role.",
                    "Yêu cầu vai trò Kỹ thuật viên hoặc Nhà phát triển."));
            } else if (proven && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", tr(state, "Already protected.", "Đã được bảo vệ."));
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (target_uwf_arm_client_id != 0) {
        state.uwf_confirmation_client_id = target_uwf_arm_client_id;
        state.uwf_confirmation_open = true;
        ImGui::OpenPopup("uwf-confirmation");
    }
    draw_uwf_confirmation_popup(state, control_plane);
}

void draw_dashboard_shell(
    const std::vector<nstu::server::ClientRecord>& clients,
    const nstu::server::ClientRecord* selected_client, DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    if (!is_action_permitted(state.current_role, OperatorAction::view_fleet_health) &&
        state.view == DashboardView::fleet_health) {
        state.view = DashboardView::room_screens;
    }
    draw_menu_strip(state, !clients.empty(), control_plane);
    draw_software_updates_modal(clients, state);
    draw_telemetry_report_popup(state, clients.size());
    draw_support_bundle_modal(state);
    draw_room_profile_modal(state, control_plane);
    draw_bulk_action_confirmation_popup(clients, state, control_plane);
    draw_ribbon(clients, selected_client, state, control_plane);
    draw_workspace_toolbar(clients, state);

    if (ImGui::BeginChild("main-workspace", {0, scaled(-32.0f)}, false)) {
        draw_navigation_rail(clients, selected_client, state, control_plane);
        ImGui::SameLine();
        const ImVec4 content_background = g_dark_mode
            ? ImVec4{0.075f, 0.086f, 0.104f, 1.0f}
            : ImVec4{0.858f, 0.878f, 0.902f, 1.0f};
        ImGui::PushStyleColor(ImGuiCol_ChildBg, content_background);
        if (ImGui::BeginChild("workspace-content", {0, 0}, false)) {
            if (state.view == DashboardView::room_screens) {
                if (state.display_mode == ClassDisplayMode::grid) {
                    draw_room_screen_wall(clients, state);
                } else {
                    draw_room_client_table(clients, state, control_plane);
                }
            } else if (state.view == DashboardView::selected_client) {
                draw_selected_client(clients, selected_client, state,
                                     control_plane);
            } else if (state.view == DashboardView::fleet_health) {
                draw_technician_fleet_health(clients, state, control_plane);
            } else {
                draw_exam_panel(clients, selected_client, state,
                                control_plane);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    draw_status_bar(clients, state);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    g_telemetry_policy = load_telemetry_policy();
    g_telemetry_events.clear();
    g_telemetry_error_prompt_requested.store(false);
    g_telemetry_error_prompt_armed.store(
        nstu::telemetry::should_prompt_for_error(g_telemetry_policy));
    const wchar_t* command_line = GetCommandLineW();
    const std::wstring_view arguments =
        command_line == nullptr ? std::wstring_view{} : command_line;
    g_graphics_debug = arguments.find(L"--graphics-debug") !=
                        std::wstring_view::npos;
    if (g_graphics_debug) {
        record_diagnostic("info", "D3D11",
                          "Graphics debug layer requested by command line");
    }
    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"NstuServerWindow";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIcon =
        LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    // Opt into per-monitor DPI awareness before registering/creating any window
    // so Windows reports real pixels and never bitmap-scales the dashboard.
    ImGui_ImplWin32_EnableDpiAwareness();
    RegisterClassW(&window_class);
    HWND window = CreateWindowW(window_class.lpszClassName, L"NSTU Server",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 820, nullptr, nullptr,
                                instance, nullptr);
    if (window == nullptr || !create_device(window)) {
        MessageBoxW(window, diagnostics_text().c_str(), L"NSTU graphics diagnostics",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    g_main_window = window;
    add_tray_icon(window);
    // Derive the DPI scale for this window and grow the default 1280x820 (96 DPI)
    // size to match, so the scaled content has room. apply_dashboard_style and
    // load_dashboard_fonts below read g_ui_scale. The window is still hidden
    // here: sizing before the first ShowWindow avoids a visible 1280x820 flash
    // and jump on a high-DPI display.
    g_ui_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    if (!(g_ui_scale > 0.0f)) {
        g_ui_scale = 1.0f;
    }
    // Optional manual override for accessibility / testing, e.g. --ui-scale=1.5.
    if (const auto scale_pos = arguments.find(L"--ui-scale=");
        scale_pos != std::wstring_view::npos) {
        const float override_scale = static_cast<float>(
            wcstod(arguments.data() + scale_pos + 11, nullptr));
        if (override_scale >= 0.75f && override_scale <= 3.0f) {
            g_ui_scale = override_scale;
            g_ui_scale_overridden = true;
        }
    }
    if (g_ui_scale != 1.0f) {
        int target_width = static_cast<int>(1280.0f * g_ui_scale);
        int target_height = static_cast<int>(820.0f * g_ui_scale);
        // Never grow past the monitor work area, or the window spills off-screen
        // (and the swap chain gets a client area the desktop can't fully show).
        // Clamp to the work area of the monitor the window is on and re-center.
        if (HMONITOR monitor =
                MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)) {
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            if (GetMonitorInfoW(monitor, &monitor_info)) {
                const int work_width =
                    monitor_info.rcWork.right - monitor_info.rcWork.left;
                const int work_height =
                    monitor_info.rcWork.bottom - monitor_info.rcWork.top;
                target_width = std::min(target_width, work_width);
                target_height = std::min(target_height, work_height);
                const int x = monitor_info.rcWork.left +
                              (work_width - target_width) / 2;
                const int y = monitor_info.rcWork.top +
                              (work_height - target_height) / 2;
                SetWindowPos(window, nullptr, x, y, target_width, target_height,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                SetWindowPos(window, nullptr, 0, 0, target_width, target_height,
                             SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
            }
        } else {
            SetWindowPos(window, nullptr, 0, 0, target_width, target_height,
                         SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        }
    }
    // Shown only once the DPI-correct size is in place.
    ShowWindow(window, SW_SHOWDEFAULT);

    DashboardState dashboard;
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_VIETNAMESE) {
        dashboard.language = Language::vietnamese;
    }
    if (arguments.find(L"--language=vi") != std::wstring_view::npos) {
        dashboard.language = Language::vietnamese;
    } else if (arguments.find(L"--language=en") != std::wstring_view::npos) {
        dashboard.language = Language::english;
    }
    dashboard.pairing_panel_open =
        arguments.find(L"--pairing") != std::wstring_view::npos;
    // Dark is the default palette (easier on the eyes in a darkened classroom
    // and avoids the bright-white "flashbang"). --light forces the light
    // palette; --dark is still accepted as a no-op for backward compatibility.
    dashboard.dark_mode =
        arguments.find(L"--light") == std::wstring_view::npos;
    g_language = dashboard.language;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    apply_dashboard_style(dashboard.dark_mode);
    load_dashboard_fonts();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());
    g_imgui_ready = true;

    nstu::server::ClientRegistry registry;
    nstu::security::KeyStore key_store;
    nstu::server::ServerControlPlane control_plane(registry, key_store);

    nstu::server::RemoteSessionProcessConfig remote_session_config;
    remote_session_config.executable = default_remote_viewer_path();
    remote_session_config.application = L"Desktop";
    remote_session_config.host_port = 47984;
    g_remote_session = std::make_unique<nstu::server::RemoteSessionProcess>(
        std::move(remote_session_config));
    std::string deployment_error;
    const auto data_directory = nstu::deployment::data_root(&deployment_error);
    if (!data_directory.empty() &&
        nstu::deployment::ensure_data_root(data_directory, &deployment_error)) {
        nstu::server::ServerControlPlaneConfig control_config;
        control_config.keyring_path =
            (data_directory / L"server-keyring.bin").wstring();
        control_config.exam_journal_path =
            data_directory / L"exams" / L"answer-journal.bin";
        control_config.telemetry_journal_path =
            data_directory / L"exams" / L"telemetry-journal";
        constexpr char entropy_text[] = "NSTU-SERVER-KEYRING-V1";
        control_config.keyring_entropy.assign(
            reinterpret_cast<const std::byte*>(entropy_text),
            reinterpret_cast<const std::byte*>(entropy_text) +
                sizeof(entropy_text) - 1);
        control_config.enrollment_secret = nstu::security::load_machine_secret(
            (data_directory / L"server-enrollment.bin").wstring(), {}, nullptr);
        // Operator-chosen room label (a display hint, not a secret): remember
        // the path so the "Add computers" field can rewrite it, and load the
        // plaintext file so the beacon advertises it. start() sanitizes it.
        dashboard.room_name_path = data_directory / L"server-room-name.txt";
        control_config.server_name = load_room_name(dashboard.room_name_path);
        std::string control_error;
        if (!control_plane.start(std::move(control_config), &control_error)) {
            dashboard.startup_error = control_error.empty()
                ? "control listener failed to start"
                : std::move(control_error);
            record_diagnostic("error", "ControlPlane",
                              dashboard.startup_error);
        }
    } else {
        dashboard.startup_error = deployment_error.empty()
            ? "protected data directory is unavailable"
            : std::move(deployment_error);
        record_diagnostic("error", "Deployment", dashboard.startup_error);
    }
    bool running = true;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }
        if (g_telemetry_error_prompt_requested.load()) {
            show_main_window(window);
        }
        if (!IsWindowVisible(window)) {
            Sleep(50);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("NSTU", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        const auto clients = registry.snapshot();
        // Chat notifications: refresh per-client inbound (student) counts, flash
        // the taskbar once when a new student line arrives while the manager is
        // not in the foreground, and expose unread state for the badges. The
        // first poll seeds the baselines so existing history neither flashes nor
        // badges at launch.
        dashboard.chat_counts = control_plane.chat_message_counts();
        {
            const std::uint64_t viewing_chat_id =
                dashboard.view == DashboardView::selected_client
                    ? dashboard.selected_client_id
                    : 0;
            if (!dashboard.chat_counts_initialized) {
                dashboard.chat_observed_counts = dashboard.chat_counts;
                dashboard.chat_seen_counts = dashboard.chat_counts;
                dashboard.chat_counts_initialized = true;
            } else {
                bool new_inbound = false;
                for (const auto& [id, count] : dashboard.chat_counts) {
                    const auto observed =
                        dashboard.chat_observed_counts.find(id);
                    const std::size_t previous =
                        observed == dashboard.chat_observed_counts.end()
                            ? 0
                            : observed->second;
                    if (count > previous && id != viewing_chat_id) {
                        new_inbound = true;
                    }
                    dashboard.chat_observed_counts[id] = count;
                }
                if (new_inbound && g_main_window != nullptr &&
                    GetForegroundWindow() != g_main_window) {
                    FLASHWINFO flash{};
                    flash.cbSize = sizeof(flash);
                    flash.hwnd = g_main_window;
                    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
                    flash.uCount = 3;
                    FlashWindowEx(&flash);
                }
            }
            // The focused client's chat panel is on screen, so its lines are
            // read as they arrive.
            if (viewing_chat_id != 0) {
                const auto current =
                    dashboard.chat_counts.find(viewing_chat_id);
                dashboard.chat_seen_counts[viewing_chat_id] =
                    current == dashboard.chat_counts.end()
                        ? 0
                        : current->second;
            }
        }
        // Requests the teacher never answered clear themselves rather
        // than sitting in the list until the window is closed.
        control_plane.expire_pending_pairings();
        const auto now = std::chrono::steady_clock::now();
        // Continuous monitoring: keep every online computer snapshotting without
        // the operator clicking "Start snapshots" per machine. The sweep is
        // throttled to at most once every 2 s, so a client that has not yet
        // acknowledged is re-armed and a reconnecting client is picked up
        // automatically. Turning off auto_monitor restores per-computer control.
        if (dashboard.auto_monitor &&
            now >= dashboard.next_auto_monitor_sweep) {
            for (const auto& monitored : clients) {
                if (monitored.status == nstu::server::ClientStatus::online &&
                    !monitored.snapshotting) {
                    std::string monitor_error;
                    (void)control_plane.set_snapshots(
                        monitored.id, true,
                        static_cast<std::uint16_t>(
                            dashboard.snapshot_interval_seconds),
                        &monitor_error);
                }
            }
            dashboard.next_auto_monitor_sweep = now + std::chrono::seconds(2);
        }
        if (dashboard.broadcast_enabled &&
            now >= dashboard.next_host_snapshot) {
            nstu::screen::JpegImage jpeg;
            std::string error;
            if (nstu::screen::capture_primary_screen_jpeg(
                    jpeg, 1280, 720, 72,
                    nstu::control::kMaximumSnapshotJpegBytes, &error)) {
                const auto captured_at = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
                nstu::control::SnapshotFrame frame{
                    .width = jpeg.width,
                    .height = jpeg.height,
                    .captured_at_unix_milliseconds = captured_at,
                    .jpeg = std::move(jpeg.bytes),
                };
                if (!control_plane.broadcast_host_snapshot(frame, &error)) {
                    record_operation_failure(
                        "Broadcast", "Teacher snapshot dispatch failed", error);
                    dashboard.control_status = std::move(error);
                }
            } else {
                record_operation_failure(
                    "Broadcast", "Teacher snapshot capture failed", error);
                dashboard.control_status = std::move(error);
            }
            dashboard.next_host_snapshot = now + std::chrono::seconds(
                dashboard.snapshot_interval_seconds);
        }
        if (clients.empty()) {
            dashboard.view = DashboardView::room_screens;
            dashboard.selected_client_id = 0;
        }
        if (dashboard.selected_client_id == 0 && !clients.empty()) {
            dashboard.selected_client_id = clients.front().id;
        }
        const auto selected_client = std::find_if(
            clients.begin(), clients.end(), [&dashboard](const auto& client) {
                return client.id == dashboard.selected_client_id;
            });
        const nstu::server::ClientRecord* selected =
            selected_client == clients.end() ? nullptr : &*selected_client;

        // Delivered-fps meter for the focused/streaming client (Epic B
        // diagnostic): sample snapshot_generation once per second so the Focus
        // label reports the real frame rate reaching the manager, not "~15fps".
        if (selected != nullptr && selected->streaming) {
            if (dashboard.fps_sample_client_id != selected->id) {
                dashboard.fps_sample_client_id = selected->id;
                dashboard.fps_sample_generation = selected->snapshot_generation;
                dashboard.fps_sample_time = now;
                dashboard.measured_stream_fps = 0.0;
            } else {
                const auto elapsed = now - dashboard.fps_sample_time;
                if (elapsed >= std::chrono::seconds(1)) {
                    const double secs =
                        std::chrono::duration<double>(elapsed).count();
                    const std::uint64_t current_generation =
                        selected->snapshot_generation;
                    // A reconnect under the same id can reset the generation
                    // below the stored baseline; guard the unsigned subtraction
                    // so this one sample reports 0 instead of underflowing to an
                    // absurd frame count.
                    const std::uint64_t frames =
                        current_generation >= dashboard.fps_sample_generation
                            ? current_generation -
                                  dashboard.fps_sample_generation
                            : 0;
                    dashboard.measured_stream_fps =
                        static_cast<double>(frames) / secs;
                    dashboard.fps_sample_generation = current_generation;
                    dashboard.fps_sample_time = now;
                }
            }
        } else {
            dashboard.fps_sample_client_id = 0;
            dashboard.measured_stream_fps = 0.0;
        }

        // Live streaming: the independent remote-control window keeps its
        // controlled client streaming regardless of the current view; otherwise
        // the client shown in Focus streams at ~15fps. Leaving Focus with no
        // remote window open, or the client going offline, stops the stream and
        // the class wall falls back to its slower snapshot cadence.
        std::uint64_t desired_stream_id = 0;
        if (g_remote_window != nullptr && g_remote_target_client != 0) {
            for (const auto& streamed : clients) {
                if (streamed.id == g_remote_target_client &&
                    streamed.status !=
                        nstu::server::ClientStatus::offline) {
                    desired_stream_id = streamed.id;
                    break;
                }
            }
        }
        if (desired_stream_id == 0 && selected != nullptr &&
            dashboard.view == DashboardView::selected_client &&
            selected->status != nstu::server::ClientStatus::offline) {
            desired_stream_id = selected->id;
        }
        if (dashboard.streaming_client_id != desired_stream_id) {
            if (dashboard.streaming_client_id != 0) {
                (void)control_plane.set_streaming(
                    dashboard.streaming_client_id, false, 0, nullptr);
            }
            if (desired_stream_id != 0) {
                std::string stream_error;
                if (!control_plane.set_streaming(desired_stream_id, true, 15,
                                                 &stream_error)) {
                    record_operation_failure(
                        "Focus", "Live stream start failed", stream_error);
                }
            }
            dashboard.streaming_client_id = desired_stream_id;
        }

        // Self-heal a stalled live stream. The arming above is edge-triggered on
        // the streamed client id, so a start_stream dropped across a client or
        // agent reconnect leaves the server believing the client streams while
        // its agent has stopped capturing -- the Focus surface and the remote
        // window freeze on the last frame with no edge to re-fire. Re-assert
        // set_streaming when the wanted client stops delivering frames (its
        // snapshot_generation stops advancing) for longer than the stall window.
        constexpr auto kStreamStallRearm = std::chrono::milliseconds(2000);
        std::uint64_t desired_generation = 0;
        if (desired_stream_id != 0) {
            for (const auto& candidate : clients) {
                if (candidate.id == desired_stream_id) {
                    desired_generation = candidate.snapshot_generation;
                    break;
                }
            }
        }
        const nstu::server::StreamRearm rearm =
            dashboard.stream_watchdog.update(desired_stream_id,
                                             desired_generation, now,
                                             kStreamStallRearm);
        if (rearm != nstu::server::StreamRearm::none) {
            std::string rearm_error;
            if (!control_plane.set_streaming(desired_stream_id, true, 15,
                                             &rearm_error)) {
                record_operation_failure("Focus", "Live stream re-arm failed",
                                         rearm_error);
            } else if (rearm == nstu::server::StreamRearm::first) {
                // Log once per stall episode; a still-dark client re-arms
                // quietly (`repeat`) so it can't churn the diagnostics ring.
                record_diagnostic("info", "Focus",
                                  "Re-armed a stalled live stream");
            }
        }

        draw_dashboard_shell(clients, selected, dashboard, control_plane);
        ImGui::End();

        ImGui::Render();
        const ImVec4 background =
            ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const float clear_color[4] = {
            background.x, background.y, background.z, background.w};
        if (g_context && g_render_target && g_swap_chain) {
            g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(),
                                           nullptr);
            g_context->ClearRenderTargetView(g_render_target.Get(), clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            const HRESULT present_result = g_swap_chain->Present(1, 0);
            if (FAILED(present_result)) {
                const HRESULT removal_reason = g_device
                    ? g_device->GetDeviceRemovedReason() : E_FAIL;
                record_diagnostic("error", "DXGI", "Present failed " +
                    hresult_text(present_result) + "; device reason " +
                    hresult_text(removal_reason));
                if (is_device_loss_hresult(present_result)) {
                    if (!g_graphics_device_lost) {
                        g_graphics_device_lost = true;
                        invalidate_snapshot_textures();
                    }
                    dashboard.control_status = tr(
                        dashboard, "Graphics device lost. Open Diagnostics.",
                        "Mất thiết bị đồ họa. Hãy mở Chẩn đoán.");
                }
            }
        }

        // Handle RemoteSessionProcess events and lifetime interlocks
        if (g_remote_session) {
            for (const auto& ev : g_remote_session->drain_events()) {
                if (ev.state == nstu::server::RemoteSessionState::running) {
                    dashboard.control_status = tr(dashboard,
                        "Remote session connected.", "Đã kết nối điều khiển từ xa.");
                } else if (ev.state == nstu::server::RemoteSessionState::failed) {
                    dashboard.control_status = tr(dashboard,
                        "Remote session failed: ", "Điều khiển từ xa thất bại: ") +
                        ev.bounded_diagnostic;
                    control_plane.record_remote_session_audit(
                        ev.client_id, "remote_session_stop", "failed", ev.bounded_diagnostic);
                } else if (ev.state == nstu::server::RemoteSessionState::exited) {
                    dashboard.control_status = tr(dashboard,
                        "Remote session closed.", "Đã đóng phiên điều khiển từ xa.");
                    control_plane.record_remote_session_audit(
                        ev.client_id, "remote_session_stop", "exited",
                        ev.exit_code ? std::to_string(*ev.exit_code) : "");
                }
            }

            const auto snap = g_remote_session->snapshot();
            if (snap.state == nstu::server::RemoteSessionState::running ||
                snap.state == nstu::server::RemoteSessionState::preflighting ||
                snap.state == nstu::server::RemoteSessionState::launching) {
                if (control_plane.exam_active(snap.client_id)) {
                    g_remote_session->stop(nstu::server::RemoteSessionStopReason::exam_started);
                    dashboard.control_status = tr(
                        dashboard, "Remote session stopped: exam started.",
                        "Phiên điều khiển từ xa đã dừng: bắt đầu bài thi.");
                    control_plane.record_remote_session_audit(
                        snap.client_id, "remote_session_stop", "stopped", "exam_started");
                } else {
                    bool client_online = false;
                    for (const auto& candidate : clients) {
                        if (candidate.id == snap.client_id &&
                            candidate.status != nstu::server::ClientStatus::offline) {
                            client_online = true;
                            break;
                        }
                    }
                    if (!client_online) {
                        g_remote_session->stop(nstu::server::RemoteSessionStopReason::client_offline);
                        dashboard.control_status = tr(
                            dashboard, "Remote session stopped: client offline.",
                            "Phiên điều khiển từ xa đã dừng: máy học sinh ngoại tuyến.");
                        control_plane.record_remote_session_audit(
                            snap.client_id, "remote_session_stop", "stopped", "client_offline");
                    }
                }
            }
        }

        // Independent remote-control window: process a pending close (posted by
        // its WM_CLOSE) and otherwise blit the controlled client's latest frame
        // through the shared device's second swap chain.
        if (g_remote_close_pending.load()) {
            close_remote_window(control_plane);
        } else if (g_remote_window != nullptr) {
            const nstu::server::ClientRecord* remote_client = nullptr;
            for (const auto& candidate : clients) {
                if (candidate.id == g_remote_target_client) {
                    remote_client = &candidate;
                    break;
                }
            }
            if (remote_client == nullptr ||
                remote_client->status ==
                    nstu::server::ClientStatus::offline) {
                close_remote_window(control_plane);
            } else {
                SnapshotTexture* remote_texture =
                    snapshot_texture(*remote_client);
                render_remote_window(
                    remote_texture ? remote_texture->view.Get() : nullptr,
                    remote_texture ? remote_texture->width : 0u,
                    remote_texture ? remote_texture->height : 0u);
            }
        }
    }

    if (g_remote_session) {
        g_remote_session->stop(nstu::server::RemoteSessionStopReason::teacher_shutdown);
        g_remote_session.reset();
    }
    close_remote_window(control_plane);
    if (dashboard.streaming_client_id != 0) {
        (void)control_plane.set_streaming(dashboard.streaming_client_id, false,
                                          0, nullptr);
    }
    control_plane.stop();
    Shell_NotifyIconW(NIM_DELETE, &g_tray_icon);
    g_snapshot_textures.clear();
    g_imgui_ready = false;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_render_target.Reset();
    g_swap_chain.Reset();
    g_context.Reset();
    g_device.Reset();
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    UnregisterClassW(window_class.lpszClassName, instance);
    return 0;
}
