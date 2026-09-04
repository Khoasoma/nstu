#include "nstu/setup/driver_scan.hpp"
#include "nstu/setup/hardware_scan.hpp"
#include "nstu/setup/policy_audit.hpp"
#include "nstu/wfp_allowlist.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <string>
#include <array>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <sstream>
#include <vector>
#include <ws2tcpip.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_render_target;
UINT g_width = 0;
UINT g_height = 0;
bool g_running = true;
bool g_graphics_debug = false;

enum class SetupTarget : int { choose, client, server, both };

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
};

std::deque<DiagnosticEvent> g_diagnostics;
GraphicsReport g_graphics_report;

std::string hresult_text(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex
           << static_cast<unsigned long>(result);
    return stream.str();
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
    while (g_diagnostics.size() > 128) g_diagnostics.pop_front();
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

std::wstring diagnostics_text() {
    std::wstring text = L"NSTU setup graphics initialization failed.\n\n";
    for (const auto& event : g_diagnostics) {
        text += L"[" + std::wstring(event.severity.begin(), event.severity.end()) +
                L"] " + std::wstring(event.source.begin(), event.source.end()) +
                L": " + std::wstring(event.message.begin(), event.message.end()) +
                L"\n";
    }
    return text;
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
    if (g_device) {
        const D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
        switch (level) {
        case D3D_FEATURE_LEVEL_11_1: g_graphics_report.feature_level = "11.1"; break;
        case D3D_FEATURE_LEVEL_11_0: g_graphics_report.feature_level = "11.0"; break;
        case D3D_FEATURE_LEVEL_10_1: g_graphics_report.feature_level = "10.1"; break;
        case D3D_FEATURE_LEVEL_10_0: g_graphics_report.feature_level = "10.0"; break;
        default: g_graphics_report.feature_level = "unknown"; break;
        }
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(g_device.As(&dxgi_device)) &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC description{};
            if (SUCCEEDED(adapter->GetDesc(&description))) {
                g_graphics_report.selected_adapter =
                    wide_to_utf8(description.Description);
                g_graphics_report.selected_vendor =
                    vendor_name(description.VendorId);
            }
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
            Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
            const HRESULT duplicate_result =
                SUCCEEDED(adapter->EnumOutputs(0, &output)) &&
                SUCCEEDED(output.As(&output1))
                    ? output1->DuplicateOutput(g_device.Get(), &duplication)
                    : E_NOINTERFACE;
            g_graphics_report.desktop_duplication =
                SUCCEEDED(duplicate_result) ? "Available" :
                "Unavailable (" + hresult_text(duplicate_result) + ")";
        }
    }
}

std::vector<std::uint32_t> parse_ipv4_list(const char* text,
                                           std::string* error) {
    std::vector<std::uint32_t> addresses;
    std::string value = text == nullptr ? std::string{} : text;
    std::size_t start = 0;
    while (start < value.size()) {
        const std::size_t end = value.find(',', start);
        std::string token = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const auto first = token.find_first_not_of(" \t\r\n");
        const auto last = token.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            token = token.substr(first, last - first + 1);
            IN_ADDR address{};
            if (InetPtonA(AF_INET, token.c_str(), &address) != 1) {
                if (error != nullptr) {
                    *error = "Invalid IPv4 address: " + token;
                }
                return {};
            }
            addresses.push_back(address.S_un.S_addr);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (addresses.empty() && error != nullptr) {
        *error = "Enter at least one IPv4 address.";
    }
    return addresses;
}

void create_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    const HRESULT buffer_result =
        g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (SUCCEEDED(buffer_result)) {
        const HRESULT view_result = g_device->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &g_render_target);
        if (FAILED(view_result)) {
            record_diagnostic("error", "D3D11",
                              "CreateRenderTargetView failed " +
                                  hresult_text(view_result));
        }
    } else {
        record_diagnostic("error", "DXGI", "GetBuffer failed " +
            hresult_text(buffer_result));
    }
}

void resize_target(UINT width, UINT height) {
    if (!g_swap_chain || width == 0 || height == 0) return;
    g_render_target.Reset();
    if (SUCCEEDED(g_swap_chain->ResizeBuffers(0, width, height,
                                              DXGI_FORMAT_UNKNOWN, 0))) {
        create_target();
    }
}

bool create_device(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Windowed = TRUE;
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = static_cast<UINT>(D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    if (g_graphics_debug) flags |= static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
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
            (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        result = D3D11CreateDeviceAndSwapChain(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
            ARRAYSIZE(levels), D3D11_SDK_VERSION, &desc, &g_swap_chain,
            &g_device, nullptr, &g_context);
        if (FAILED(result) && g_graphics_debug) {
            record_diagnostic("warning", "D3D11",
                              "Debug layer unavailable; retrying without it (" +
                                  hresult_text(result) + ")");
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            g_swap_chain.Reset();
            g_device.Reset();
            g_context.Reset();
            result = D3D11CreateDeviceAndSwapChain(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                ARRAYSIZE(levels), D3D11_SDK_VERSION, &desc, &g_swap_chain,
                &g_device, nullptr, &g_context);
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
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
            ARRAYSIZE(levels), D3D11_SDK_VERSION, &desc, &g_swap_chain,
            &g_device, nullptr, &g_context);
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
    create_target();
    refresh_graphics_report();
    return g_render_target != nullptr;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return 1;
    }
    switch (message) {
    case WM_SIZE:
        g_width = LOWORD(lparam);
        g_height = HIWORD(lparam);
        if (wparam != SIZE_MINIMIZED) resize_target(g_width, g_height);
        return 0;
    case WM_CLOSE:
        g_running = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

const char* target_name(SetupTarget target) {
    switch (target) {
    case SetupTarget::client: return "Client setup";
    case SetupTarget::server: return "Server setup";
    case SetupTarget::both: return "Client + Server audit";
    default: return "Choose setup target";
    }
}

void draw_diagnostics_popup() {
    if (!ImGui::Button("Diagnostics")) {
        // The popup remains available after the first activation.
    } else {
        refresh_graphics_report();
        ImGui::OpenPopup("setup-diagnostics");
    }
    if (!ImGui::BeginPopupModal("setup-diagnostics", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Setup diagnostics");
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
    if (ImGui::BeginChild("setup-diagnostic-adapters", {620.0f, 100.0f}, true)) {
        for (const auto& adapter : g_graphics_report.adapters) {
            ImGui::BulletText("%s", adapter.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::TextUnformatted("Recent events");
    if (ImGui::BeginChild("setup-diagnostic-events", {620.0f, 170.0f}, true)) {
        for (const auto& event : g_diagnostics) {
            ImGui::TextWrapped("[%s] [%s] %s: %s", event.timestamp.c_str(),
                               event.severity.c_str(), event.source.c_str(),
                               event.message.c_str());
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Refresh", {90.0f, 0.0f})) refresh_graphics_report();
    ImGui::SameLine();
    if (ImGui::Button("Close", {90.0f, 0.0f})) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
    const wchar_t* command_line = GetCommandLineW();
    const std::wstring command_line_text = command_line == nullptr
        ? std::wstring{} : command_line;
    g_graphics_debug = command_line_text.find(L"--graphics-debug") !=
                       std::wstring::npos;
    SetupTarget setup_target = SetupTarget::choose;
    if (command_line_text.find(L"--target=client") != std::wstring::npos) {
        setup_target = SetupTarget::client;
    } else if (command_line_text.find(L"--target=server") != std::wstring::npos) {
        setup_target = SetupTarget::server;
    } else if (command_line_text.find(L"--target=both") != std::wstring::npos) {
        setup_target = SetupTarget::both;
    }
    if (g_graphics_debug) {
        record_diagnostic("info", "D3D11",
                          "Graphics debug layer requested by command line");
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com);
    const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mf)) return 1;

    WNDCLASSW klass{};
    klass.hInstance = instance;
    klass.lpfnWndProc = window_proc;
    klass.lpszClassName = L"NstuSetupWindow";
    klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&klass);
    HWND window = CreateWindowW(klass.lpszClassName, L"NSTU Setup",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 900, 620, nullptr, nullptr,
                                instance, nullptr);
    if (window == nullptr || !create_device(window)) {
        MessageBoxW(window, diagnostics_text().c_str(),
                    L"NSTU setup diagnostics", MB_OK | MB_ICONERROR);
        MFShutdown();
        if (uninitialize_com) CoUninitialize();
        return 1;
    }
    ShowWindow(window, show);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    const auto hardware = nstu::setup::scan_hardware();
    std::string encoder_error;
    const auto encoders = nstu::setup::scan_hardware_h264_encoders(&encoder_error);
    auto policy = nstu::setup::read_policy();
    std::string status;
    nstu::net::WfpWebsiteAllowlist wfp_allowlist;
    std::array<char, 1024> allowlist_input{};

    while (g_running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("NSTU Administrator Bootstrapper");
        if (setup_target == SetupTarget::choose) {
            ImGui::TextUnformatted("Choose what this machine will host");
            ImGui::TextWrapped(
                "Selecting a target keeps the setup workflow and checks clear "
                "for technicians. You can run both roles on one machine when "
                "required.");
            ImGui::Separator();
            if (ImGui::Button("Set up Client", {180.0f, 42.0f})) {
                setup_target = SetupTarget::client;
            }
            ImGui::SameLine();
            if (ImGui::Button("Set up Server", {180.0f, 42.0f})) {
                setup_target = SetupTarget::server;
            }
            ImGui::SameLine();
            if (ImGui::Button("Audit Both Roles", {180.0f, 42.0f})) {
                setup_target = SetupTarget::both;
            }
            ImGui::End();
            ImGui::Render();
            const float clear[] = {0.04f, 0.05f, 0.06f, 1.0f};
            g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(), nullptr);
            g_context->ClearRenderTargetView(g_render_target.Get(), clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            const HRESULT present_result = g_swap_chain->Present(1, 0);
            if (FAILED(present_result)) {
                record_diagnostic("error", "DXGI", "Present failed " +
                    hresult_text(present_result));
            }
            continue;
        }
        ImGui::Text("Target: %s", target_name(setup_target));
        ImGui::SameLine();
        draw_diagnostics_popup();
        ImGui::Separator();
        const bool configure_client = setup_target == SetupTarget::client ||
                                      setup_target == SetupTarget::both;
        const bool configure_server = setup_target == SetupTarget::server ||
                                      setup_target == SetupTarget::both;
        if (configure_server) {
            ImGui::TextUnformatted("Server prerequisites");
            ImGui::Text("Display: %u x %u @ %u Hz", hardware.display.width,
                        hardware.display.height, hardware.display.refresh_hz);
            ImGui::Text("Hardware H.264 encoders: %zu", encoders.size());
            if (!encoder_error.empty()) ImGui::Text("Encoder scan: %s", encoder_error.c_str());
            ImGui::Separator();
        }
        if (configure_client) {
            ImGui::TextUnformatted("Client prerequisites");
            ImGui::Text("The client requires an active network adapter and a display session.");
            ImGui::Separator();
        }
        for (const auto& network : hardware.networks) {
            ImGui::Text("Network: %ls  %llu Mbps", network.adapter_name.c_str(),
                        static_cast<unsigned long long>(network.link_speed_mbps));
        }
        if (!configure_server) {
            ImGui::Text("Network adapters detected: %zu", hardware.networks.size());
        }
        ImGui::Separator();
        ImGui::Text("Policy audit");
        ImGui::Text("Task Manager: %s", policy.task_manager_disabled ? "disabled" : "enabled");
        ImGui::Text("Command Prompt: %s", policy.command_prompt_disabled ? "disabled" : "enabled");
        ImGui::Text("Control Panel: %s", policy.control_panel_disabled ? "disabled" : "enabled");
        ImGui::Text("Drive C visibility: %s", policy.drive_c_hidden ? "hidden" : "visible");
        if (ImGui::Button("Apply lockdown")) {
            status = nstu::setup::apply_lockdown(&status)
                ? "Lockdown applied to the current user profile."
                : "Lockdown failed: " + status;
            policy = nstu::setup::read_policy();
        }
        ImGui::Separator();
        ImGui::Text("Website allowlist (IPv4)");
        ImGui::TextDisabled("Comma-separated addresses; applies only when you click Apply.");
        ImGui::InputText("##wfp-allowlist", allowlist_input.data(),
                         allowlist_input.size());
        if (ImGui::Button("Apply website allowlist")) {
            std::string parse_error;
            const auto addresses =
                parse_ipv4_list(allowlist_input.data(), &parse_error);
            if (addresses.empty()) {
                status = parse_error;
            } else {
                nstu::net::WfpAllowlistConfig config;
                config.allowed_ipv4 = addresses;
                status = wfp_allowlist.apply(config, &parse_error)
                    ? "Website allowlist applied to outbound HTTP/HTTPS."
                    : "Website allowlist failed: " + parse_error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear website allowlist")) {
            std::string clear_error;
            status = wfp_allowlist.clear(&clear_error)
                ? "NSTU website allowlist cleared."
                : "Website allowlist clear failed: " + clear_error;
        }
        if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();
        ImGui::Render();
        const float clear[] = {0.04f, 0.05f, 0.06f, 1.0f};
        g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_render_target.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT present_result = g_swap_chain->Present(1, 0);
        if (FAILED(present_result)) {
            const HRESULT reason = g_device
                ? g_device->GetDeviceRemovedReason() : E_FAIL;
            record_diagnostic("error", "DXGI", "Present failed " +
                hresult_text(present_result) + "; device reason " +
                hresult_text(reason));
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_render_target.Reset();
    g_swap_chain.Reset();
    g_context.Reset();
    g_device.Reset();
    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, instance);
    MFShutdown();
    if (uninitialize_com) CoUninitialize();
    return 0;
}
