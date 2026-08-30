#include "nstu/client_registry.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_render_target;

void create_render_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        g_device->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                         &g_render_target);
    }
}

bool create_device(HWND window) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested, 1,
            D3D11_SDK_VERSION, &description, &g_swap_chain, &g_device,
            &selected, &g_context))) {
        return false;
    }
    create_render_target();
    return g_render_target != nullptr;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return 1;
    }
    if (message == WM_SIZE && g_swap_chain && wparam != SIZE_MINIMIZED) {
        g_render_target.Reset();
        g_swap_chain->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam),
                                    DXGI_FORMAT_UNKNOWN, 0);
        create_render_target();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"NstuServerWindow";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&window_class);
    HWND window = CreateWindowW(window_class.lpszClassName, L"NSTU Server",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1100, 700, nullptr, nullptr,
                                instance, nullptr);
    if (window == nullptr || !create_device(window)) {
        return 1;
    }
    ShowWindow(window, SW_SHOWDEFAULT);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsLight();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    nstu::server::ClientRegistry registry;
    std::uint64_t selected_client_id = 0;
    bool stream_requested = false;
    std::array<char, 512> chat_input{};
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
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("NSTU", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize);
        ImGui::Text("Connected clients: %zu", registry.size());
        ImGui::Separator();
        const auto clients = registry.snapshot();
        const auto selected_client = std::find_if(
            clients.begin(), clients.end(),
            [selected_client_id](const auto& client) {
                return client.id == selected_client_id;
            });
        const bool has_selected_client = selected_client != clients.end();
        const float left_width = ImGui::GetContentRegionAvail().x * 0.58f;
        if (ImGui::BeginChild("client-list", {left_width, 0}, true)) {
            ImGui::TextUnformatted("Clients");
            ImGui::Separator();
            if (clients.empty()) {
                ImGui::TextDisabled("No authenticated clients connected.");
            } else if (ImGui::BeginTable(
                           "clients", 6,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Host");
                ImGui::TableSetupColumn("Address");
                ImGui::TableSetupColumn("Status");
                ImGui::TableSetupColumn("Path");
                ImGui::TableSetupColumn("Latency");
                ImGui::TableSetupColumn("Loss");
                ImGui::TableHeadersRow();
                for (const auto& client : clients) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const bool selected = selected_client_id == client.id;
                    if (ImGui::Selectable(client.hostname.c_str(), selected,
                                         ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_client_id = client.id;
                        stream_requested = false;
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(client.address.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(nstu::server::to_string(client.status));
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(
                        client.delivery == nstu::net::VideoDeliveryMode::multicast
                            ? "Multicast"
                            : "Unicast");
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%u ms", client.latency_ms);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.1f%%", client.packet_loss_per_mille / 10.0f);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("client-detail", {0, 0}, true)) {
            if (!has_selected_client) {
                ImGui::TextUnformatted(clients.empty()
                                           ? "Waiting for an authenticated client"
                                           : "Select a client");
            } else {
                ImGui::Text("%s", selected_client->hostname.c_str());
                ImGui::TextUnformatted(selected_client->address.c_str());
                ImGui::Separator();
                ImGui::Text("Status: %s",
                            nstu::server::to_string(selected_client->status));
                ImGui::Text("Latency: %u ms", selected_client->latency_ms);
                ImGui::Text("Packet loss: %.1f%%",
                            selected_client->packet_loss_per_mille / 10.0f);
                ImGui::Text("Delivery: %s",
                            selected_client->delivery ==
                                    nstu::net::VideoDeliveryMode::multicast
                                ? "Multicast"
                                : "Unicast");
                ImGui::Separator();
                ImGui::TextUnformatted("Screen stream");
                const float preview_width = ImGui::GetContentRegionAvail().x;
                const float preview_height =
                    std::max(120.0f, std::min(220.0f, preview_width * 9.0f / 16.0f));
                if (ImGui::BeginChild("stream-preview",
                                      {0, preview_height}, true,
                                      ImGuiWindowFlags_NoScrollbar)) {
                    const auto top_left = ImGui::GetCursorScreenPos();
                    const auto bottom_right = ImVec2(
                        top_left.x + ImGui::GetContentRegionAvail().x,
                        top_left.y + preview_height - 12.0f);
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        top_left, bottom_right, IM_COL32(28, 31, 35, 255));
                    ImGui::SetCursorPos({12.0f, preview_height * 0.42f});
                    ImGui::TextUnformatted(stream_requested
                                               ? "Waiting for authenticated video frames"
                                               : "No live stream requested");
                }
                ImGui::EndChild();
                ImGui::TextUnformatted("Target: 15 FPS");
                if (ImGui::Button(stream_requested ? "Stop stream" : "Start stream")) {
                    stream_requested = !stream_requested;
                }
                ImGui::SameLine();
                if (ImGui::Button("Request keyframe")) {
                    ImGui::OpenPopup("control-status");
                }
                ImGui::SameLine();
                if (ImGui::Button("Lock")) {
                    ImGui::OpenPopup("control-status");
                }
                if (ImGui::BeginPopup("control-status")) {
                    ImGui::TextUnformatted("Control-plane routing is pending.");
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::BeginChild("chat-panel", {0, 116}, true)) {
            ImGui::TextUnformatted("Chat");
            ImGui::TextDisabled(has_selected_client
                                    ? "No messages in this local UI session."
                                    : "Select an authenticated client to chat.");
            ImGui::BeginDisabled(!has_selected_client);
            ImGui::SetNextItemWidth(-92.0f);
            const bool submit = ImGui::InputText("##chat-input", chat_input.data(),
                                                  chat_input.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((submit || ImGui::Button("Send")) && chat_input[0] != '\0') {
                chat_input[0] = '\0';
            }
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        constexpr float clear_color[4] = {0.94f, 0.94f, 0.93f, 1.0f};
        g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_render_target.Get(), clear_color);
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
    UnregisterClassW(window_class.lpszClassName, instance);
    return 0;
}
