#include "nstu/setup/driver_scan.hpp"
#include "nstu/setup/hardware_scan.hpp"
#include "nstu/setup/policy_audit.hpp"
#include "nstu/wfp_allowlist.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <wrl/client.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <string>
#include <array>
#include <cstdint>
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
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        g_device->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                         &g_render_target);
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
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                            D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected{};
    return SUCCEEDED(D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &desc, &g_swap_chain, &g_device, &selected,
        &g_context)) && (create_target(), true);
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
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
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
        ImGui::Text("Display: %u x %u @ %u Hz", hardware.display.width,
                    hardware.display.height, hardware.display.refresh_hz);
        for (const auto& network : hardware.networks) {
            ImGui::Text("Network: %ls  %llu Mbps", network.adapter_name.c_str(),
                        static_cast<unsigned long long>(network.link_speed_mbps));
        }
        ImGui::Text("Hardware H.264 encoders: %zu", encoders.size());
        if (!encoder_error.empty()) ImGui::Text("Encoder scan: %s", encoder_error.c_str());
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
        g_swap_chain->Present(1, 0);
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
