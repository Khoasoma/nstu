#include "nstu/client_registry.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/deployment.hpp"
#include "nstu/key_store.hpp"
#include "nstu/secret_store.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_render_target;
ImFont* g_heading_font = nullptr;

constexpr int kMinimumScreenFps = 5;
constexpr int kMaximumScreenFps = 15;

enum class DashboardView : int {
    room_screens,
    selected_client,
};

struct DashboardState {
    DashboardView view = DashboardView::room_screens;
    std::uint64_t selected_client_id = 0;
    int screen_fps = 10;
    bool show_offline = true;
    bool stream_requested = false;
    std::string control_status;
    std::string startup_error;
    std::array<char, 96> client_filter{};
    std::array<char, 512> chat_input{};
};

struct RoomCounts {
    std::size_t online = 0;
    std::size_t attention = 0;
    std::size_t locked = 0;
    std::size_t offline = 0;
};

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
    if (message == WM_GETMINMAXINFO) {
        auto* minimum = reinterpret_cast<MINMAXINFO*>(lparam);
        minimum->ptMinTrackSize.x = 960;
        minimum->ptMinTrackSize.y = 640;
        return 0;
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

void apply_dashboard_style() {
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {20.0f, 16.0f};
    style.FramePadding = {10.0f, 7.0f};
    style.CellPadding = {10.0f, 8.0f};
    style.ItemSpacing = {10.0f, 8.0f};
    style.ItemInnerSpacing = {7.0f, 6.0f};
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = {0.12f, 0.12f, 0.11f, 1.0f};
    colors[ImGuiCol_TextDisabled] = {0.46f, 0.45f, 0.43f, 1.0f};
    colors[ImGuiCol_WindowBg] = {0.975f, 0.971f, 0.958f, 1.0f};
    colors[ImGuiCol_ChildBg] = {1.0f, 1.0f, 1.0f, 1.0f};
    colors[ImGuiCol_PopupBg] = {1.0f, 1.0f, 1.0f, 1.0f};
    colors[ImGuiCol_Border] = {0.90f, 0.89f, 0.87f, 1.0f};
    colors[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_FrameBg] = {0.985f, 0.982f, 0.973f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = {0.95f, 0.945f, 0.93f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = {0.92f, 0.915f, 0.90f, 1.0f};
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_Button] = {0.93f, 0.925f, 0.91f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = {0.88f, 0.875f, 0.86f, 1.0f};
    colors[ImGuiCol_ButtonActive] = {0.83f, 0.825f, 0.81f, 1.0f};
    colors[ImGuiCol_Header] = {0.88f, 0.93f, 0.95f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = {0.82f, 0.90f, 0.94f, 1.0f};
    colors[ImGuiCol_HeaderActive] = {0.76f, 0.87f, 0.92f, 1.0f};
    colors[ImGuiCol_Separator] = {0.90f, 0.89f, 0.87f, 1.0f};
    colors[ImGuiCol_CheckMark] = {0.18f, 0.36f, 0.45f, 1.0f};
    colors[ImGuiCol_SliderGrab] = {0.18f, 0.18f, 0.17f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = {0.30f, 0.30f, 0.29f, 1.0f};
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
    if (GetFileAttributesA(regular.c_str()) != INVALID_FILE_ATTRIBUTES) {
        atlas->AddFontFromFileTTF(regular.c_str(), 17.0f);
    }
    if (GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_heading_font = atlas->AddFontFromFileTTF(bold.c_str(), 22.0f);
    }
}

ImVec4 status_text_color(nstu::server::ClientStatus status) {
    switch (status) {
    case nstu::server::ClientStatus::online:
        return {0.20f, 0.40f, 0.23f, 1.0f};
    case nstu::server::ClientStatus::degraded:
    case nstu::server::ClientStatus::connecting:
        return {0.58f, 0.39f, 0.06f, 1.0f};
    case nstu::server::ClientStatus::locked:
        return {0.62f, 0.18f, 0.17f, 1.0f};
    case nstu::server::ClientStatus::offline:
        return {0.40f, 0.39f, 0.37f, 1.0f};
    }
    return {0.40f, 0.39f, 0.37f, 1.0f};
}

ImVec4 status_background_color(nstu::server::ClientStatus status) {
    switch (status) {
    case nstu::server::ClientStatus::online:
        return {0.93f, 0.96f, 0.92f, 1.0f};
    case nstu::server::ClientStatus::degraded:
    case nstu::server::ClientStatus::connecting:
        return {0.985f, 0.95f, 0.86f, 1.0f};
    case nstu::server::ClientStatus::locked:
        return {0.99f, 0.92f, 0.92f, 1.0f};
    case nstu::server::ClientStatus::offline:
        return {0.94f, 0.935f, 0.925f, 1.0f};
    }
    return {0.94f, 0.935f, 0.925f, 1.0f};
}

RoomCounts count_room_statuses(
    const std::vector<nstu::server::ClientRecord>& clients) {
    RoomCounts counts;
    for (const auto& client : clients) {
        switch (client.status) {
        case nstu::server::ClientStatus::online:
            ++counts.online;
            break;
        case nstu::server::ClientStatus::connecting:
        case nstu::server::ClientStatus::degraded:
            ++counts.attention;
            break;
        case nstu::server::ClientStatus::locked:
            ++counts.locked;
            break;
        case nstu::server::ClientStatus::offline:
            ++counts.offline;
            break;
        }
    }
    return counts;
}

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    });
    return result;
}

bool client_matches_filter(const nstu::server::ClientRecord& client,
                           const DashboardState& state) {
    if (!state.show_offline &&
        client.status == nstu::server::ClientStatus::offline) {
        return false;
    }
    if (state.client_filter[0] == '\0') {
        return true;
    }
    const auto query = lower_ascii(state.client_filter.data());
    return lower_ascii(client.hostname).find(query) != std::string::npos ||
           lower_ascii(client.address).find(query) != std::string::npos;
}

void push_client_id(std::uint64_t id) {
    ImGui::PushID(static_cast<int>(id >> 32u));
    ImGui::PushID(static_cast<int>(id & 0xffffffffu));
}

void pop_client_id() {
    ImGui::PopID();
    ImGui::PopID();
}

void draw_status_badge(nstu::server::ClientStatus status) {
    const char* label = nstu::server::to_string(status);
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 size{text_size.x + 16.0f, text_size.y + 8.0f};
    ImGui::PushID(label);
    ImGui::InvisibleButton("##status", size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(minimum, maximum,
                             ImGui::ColorConvertFloat4ToU32(
                                 status_background_color(status)),
                             4.0f);
    draw_list->AddText({minimum.x + 8.0f, minimum.y + 4.0f},
                       ImGui::ColorConvertFloat4ToU32(
                           status_text_color(status)),
                       label);
    ImGui::PopID();
}

void draw_summary_card(const char* label, std::size_t value,
                       const ImVec4& accent, float width) {
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {1.0f, 1.0f, 1.0f, 1.0f});
    if (ImGui::BeginChild("summary", {width, 62.0f}, true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextColored(accent, "%zu", value);
        ImGui::TextDisabled("%s", label);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

bool draw_mode_button(const char* label, DashboardView value,
                      DashboardState& state) {
    const bool selected = state.view == value;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          selected ? ImVec4{0.12f, 0.12f, 0.11f, 1.0f}
                                   : ImVec4{0.93f, 0.925f, 0.91f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          selected ? ImVec4{0.20f, 0.20f, 0.19f, 1.0f}
                                   : ImVec4{0.88f, 0.875f, 0.86f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          selected ? ImVec4{0.24f, 0.24f, 0.23f, 1.0f}
                                   : ImVec4{0.83f, 0.825f, 0.81f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_Text,
                          selected ? ImVec4{1.0f, 1.0f, 1.0f, 1.0f}
                                   : ImVec4{0.12f, 0.12f, 0.11f, 1.0f});
    const bool pressed = ImGui::Button(label, {142.0f, 34.0f});
    ImGui::PopStyleColor(4);
    if (pressed) {
        state.view = value;
    }
    return pressed;
}

void draw_fps_stepper(DashboardState& state) {
    ImGui::TextUnformatted("Screen refresh");
    ImGui::SameLine();
    ImGui::BeginDisabled(state.screen_fps <= kMinimumScreenFps);
    if (ImGui::Button("-", {30.0f, 30.0f})) {
        --state.screen_fps;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(68.0f);
    ImGui::InputInt("##screen-fps", &state.screen_fps, 0, 0,
                    ImGuiInputTextFlags_ReadOnly);
    state.screen_fps = std::clamp(state.screen_fps, kMinimumScreenFps,
                                  kMaximumScreenFps);
    ImGui::SameLine();
    ImGui::BeginDisabled(state.screen_fps >= kMaximumScreenFps);
    if (ImGui::Button("+", {30.0f, 30.0f})) {
        ++state.screen_fps;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("fps");
}

bool draw_screen_surface(const nstu::server::ClientRecord& client,
                         float height, const char* id) {
    const ImVec2 size{ImGui::GetContentRegionAvail().x, height};
    ImGui::InvisibleButton(id, size);
    const bool clicked = ImGui::IsItemClicked();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool offline = client.status == nstu::server::ClientStatus::offline;
    const ImU32 background = offline ? IM_COL32(230, 228, 223, 255)
                                     : IM_COL32(35, 35, 33, 255);
    const ImU32 foreground = offline ? IM_COL32(100, 98, 94, 255)
                                     : IM_COL32(240, 239, 235, 255);
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(minimum, maximum, background, 5.0f);
    const char* text = offline ? "Offline" : "Awaiting live frame";
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    draw_list->AddText({minimum.x + (size.x - text_size.x) * 0.5f,
                        minimum.y + (height - text_size.y) * 0.5f},
                       foreground, text);
    return clicked;
}

void draw_client_card(const nstu::server::ClientRecord& client, float width,
                      DashboardState& state) {
    push_client_id(client.id);
    const bool selected = state.selected_client_id == client.id;
    ImGui::PushStyleColor(ImGuiCol_Border,
                          selected ? ImVec4{0.31f, 0.56f, 0.68f, 1.0f}
                                   : ImVec4{0.90f, 0.89f, 0.87f, 1.0f});
    if (ImGui::BeginChild("client-card", {width, width * 0.5625f + 92.0f},
                          true, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextUnformatted(client.hostname.c_str());
        const float badge_width =
            ImGui::CalcTextSize(nstu::server::to_string(client.status)).x +
            16.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - badge_width);
        draw_status_badge(client.status);
        if (draw_screen_surface(client, width * 0.5625f, "##screen")) {
            state.selected_client_id = client.id;
        }
        ImGui::TextDisabled("%s", client.address.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u ms", client.latency_ms);
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f%% loss",
                            client.packet_loss_per_mille / 10.0f);
        const float open_width = 56.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - open_width);
        if (ImGui::Button("Open", {open_width, 0.0f})) {
            state.selected_client_id = client.id;
            state.view = DashboardView::selected_client;
            state.stream_requested = false;
        }
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

    ImGui::TextUnformatted("Live room screens");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu shown", visible_clients.size());
    ImGui::Separator();
    if (visible_clients.empty()) {
        const float offset = std::max(36.0f,
                                      ImGui::GetContentRegionAvail().y * 0.34f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
        const char* heading = clients.empty() ? "No clients connected"
                                              : "No clients match this view";
        const ImVec2 heading_size = ImGui::CalcTextSize(heading);
        ImGui::SetCursorPosX(
            std::max(20.0f, (ImGui::GetWindowWidth() - heading_size.x) * 0.5f));
        ImGui::TextDisabled("%s", heading);
        return;
    }

    if (ImGui::BeginChild("screen-wall", {0, 0}, false)) {
        constexpr float minimum_card_width = 270.0f;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = std::clamp(
            static_cast<int>((available + gap) / (minimum_card_width + gap)),
            1, 5);
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
}

void draw_focus_client_list(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    if (ImGui::BeginChild("focus-client-list", {280.0f, 0}, true)) {
        ImGui::TextUnformatted("Clients");
        ImGui::Separator();
        if (ImGui::BeginTable("focus-clients", 2,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                                    82.0f);
            for (const auto& client : clients) {
                if (!client_matches_filter(client, state)) {
                    continue;
                }
                push_client_id(client.id);
                ImGui::TableNextRow(0, 40.0f);
                ImGui::TableSetColumnIndex(0);
                const bool selected = state.selected_client_id == client.id;
                if (ImGui::Selectable(client.hostname.c_str(), selected,
                                      ImGuiSelectableFlags_None,
                                      {0.0f, 36.0f})) {
                    state.selected_client_id = client.id;
                    state.stream_requested = false;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(status_text_color(client.status), "%s",
                                   nstu::server::to_string(client.status));
                pop_client_id();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void draw_telemetry_card(const char* label, const char* value, float width) {
    ImGui::PushID(label);
    if (ImGui::BeginChild("telemetry", {width, 62.0f}, true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextDisabled("%s", label);
        ImGui::TextUnformatted(value);
    }
    ImGui::EndChild();
    ImGui::PopID();
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
        const char* empty = clients.empty() ? "No clients connected"
                                            : "Select a client";
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
    draw_status_badge(selected_client->status);
    ImGui::TextDisabled("%s", selected_client->address.c_str());

    char latency[32]{};
    char packet_loss[32]{};
    sprintf_s(latency, "%u ms", selected_client->latency_ms);
    sprintf_s(packet_loss, "%.1f%%",
              selected_client->packet_loss_per_mille / 10.0f);
    const char* delivery =
        selected_client->delivery == nstu::net::VideoDeliveryMode::multicast
            ? "Multicast"
            : "Unicast";
    const float telemetry_gap = ImGui::GetStyle().ItemSpacing.x;
    const float telemetry_width =
        (ImGui::GetContentRegionAvail().x - telemetry_gap * 2.0f) / 3.0f;
    draw_telemetry_card("Latency", latency, telemetry_width);
    ImGui::SameLine();
    draw_telemetry_card("Packet loss", packet_loss, telemetry_width);
    ImGui::SameLine();
    draw_telemetry_card("Delivery", delivery, telemetry_width);

    ImGui::TextUnformatted("Live screen");
    const float preview_width = ImGui::GetContentRegionAvail().x;
    const float preview_height = std::clamp(
        preview_width * 0.5625f, 220.0f,
        std::max(220.0f, ImGui::GetContentRegionAvail().y - 210.0f));
    draw_screen_surface(*selected_client, preview_height, "##focus-screen");

    ImGui::PushStyleColor(ImGuiCol_Button, {0.12f, 0.12f, 0.11f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          {0.20f, 0.20f, 0.19f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          {0.25f, 0.25f, 0.24f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, 1.0f, 1.0f, 1.0f});
    if (ImGui::Button(state.stream_requested ? "Stop stream" : "Start stream")) {
        const bool enabled = !state.stream_requested;
        std::string error;
        if (control_plane.set_streaming(
                selected_client->id, enabled,
                static_cast<std::uint8_t>(state.screen_fps), &error)) {
            state.stream_requested = enabled;
            state.control_status = enabled ? "Stream command sent."
                                           : "Stop command sent.";
        } else {
            state.control_status = error;
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    if (ImGui::Button("Request keyframe")) {
        std::string error;
        state.control_status =
            control_plane.request_keyframe(selected_client->id, &error)
                ? "Keyframe requested."
                : error;
        ImGui::OpenPopup("control-status");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.99f, 0.92f, 0.92f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          {0.97f, 0.86f, 0.86f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, {0.62f, 0.18f, 0.17f, 1.0f});
    const bool is_locked =
        selected_client->status == nstu::server::ClientStatus::locked;
    if (ImGui::Button(is_locked ? "Unlock client" : "Lock client")) {
        std::string error;
        state.control_status =
            control_plane.set_locked(selected_client->id, !is_locked, &error)
                ? (is_locked ? "Unlock command sent." : "Lock command sent.")
                : error;
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(3);
    if (ImGui::BeginPopup("control-status")) {
        ImGui::TextUnformatted(state.control_status.c_str());
        ImGui::EndPopup();
    }

    if (ImGui::BeginChild("chat-panel", {0, 112.0f}, true)) {
        ImGui::TextUnformatted("Chat");
        ImGui::TextDisabled("No messages in this session.");
        ImGui::SetNextItemWidth(-78.0f);
        const bool submit = ImGui::InputText(
            "##chat-input", state.chat_input.data(), state.chat_input.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((submit || ImGui::Button("Send", {68.0f, 0.0f})) &&
            state.chat_input[0] != '\0') {
            std::string error;
            if (control_plane.send_chat(selected_client->id,
                                        state.chat_input.data(), &error)) {
                state.chat_input[0] = '\0';
                state.control_status = "Message sent.";
            } else {
                state.control_status = error;
            }
            ImGui::OpenPopup("control-status");
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void draw_dashboard_header(const std::vector<nstu::server::ClientRecord>& clients,
                           DashboardState& state) {
    const auto counts = count_room_statuses(clients);
    if (g_heading_font != nullptr) {
        ImGui::PushFont(g_heading_font);
    }
    ImGui::TextUnformatted("NSTU Classroom");
    if (g_heading_font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::TextDisabled("Computer room overview");

    const float control_width = 286.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - control_width);
    draw_fps_stepper(state);
    if (!state.startup_error.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored({0.62f, 0.18f, 0.17f, 1.0f},
                           "Control service unavailable: %s",
                           state.startup_error.c_str());
        ImGui::PopTextWrapPos();
    }

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float summary_width =
        (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f;
    draw_summary_card("Online", counts.online,
                      {0.20f, 0.40f, 0.23f, 1.0f}, summary_width);
    ImGui::SameLine();
    draw_summary_card("Needs attention", counts.attention,
                      {0.58f, 0.39f, 0.06f, 1.0f}, summary_width);
    ImGui::SameLine();
    draw_summary_card("Locked", counts.locked,
                      {0.62f, 0.18f, 0.17f, 1.0f}, summary_width);
    ImGui::SameLine();
    draw_summary_card("Offline", counts.offline,
                      {0.40f, 0.39f, 0.37f, 1.0f}, summary_width);

    draw_mode_button("Room screens", DashboardView::room_screens, state);
    ImGui::SameLine();
    ImGui::BeginDisabled(clients.empty());
    draw_mode_button("Selected client", DashboardView::selected_client, state);
    ImGui::EndDisabled();
    const float filter_width = 220.0f;
    const float checkbox_width = 104.0f;
    const float filter_start = ImGui::GetContentRegionMax().x -
                               filter_width - checkbox_width - gap;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + gap, filter_start));
    ImGui::SetNextItemWidth(filter_width);
    ImGui::InputTextWithHint("##client-filter", "Find client",
                             state.client_filter.data(),
                             state.client_filter.size());
    ImGui::SameLine();
    ImGui::Checkbox("Show offline", &state.show_offline);
    ImGui::Separator();
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
                                CW_USEDEFAULT, 1280, 820, nullptr, nullptr,
                                instance, nullptr);
    if (window == nullptr || !create_device(window)) {
        return 1;
    }
    ShowWindow(window, SW_SHOWDEFAULT);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    apply_dashboard_style();
    load_dashboard_fonts();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    nstu::server::ClientRegistry registry;
    nstu::security::KeyStore key_store;
    nstu::server::ServerControlPlane control_plane(registry, key_store);
    DashboardState dashboard;
    std::string deployment_error;
    const auto data_directory = nstu::deployment::data_root(&deployment_error);
    if (!data_directory.empty() &&
        nstu::deployment::ensure_data_root(data_directory, &deployment_error)) {
        nstu::server::ServerControlPlaneConfig control_config;
        control_config.keyring_path =
            (data_directory / L"server-keyring.bin").wstring();
        constexpr char entropy_text[] = "NSTU-SERVER-KEYRING-V1";
        control_config.keyring_entropy.assign(
            reinterpret_cast<const std::byte*>(entropy_text),
            reinterpret_cast<const std::byte*>(entropy_text) +
                sizeof(entropy_text) - 1);
        control_config.enrollment_secret = nstu::security::load_machine_secret(
            (data_directory / L"server-enrollment.bin").wstring(), {}, nullptr);
        std::string control_error;
        if (!control_plane.start(std::move(control_config), &control_error)) {
            dashboard.startup_error = control_error.empty()
                ? "control listener failed to start"
                : std::move(control_error);
        }
    } else {
        dashboard.startup_error = deployment_error.empty()
            ? "protected data directory is unavailable"
            : std::move(deployment_error);
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

        draw_dashboard_header(clients, dashboard);
        if (dashboard.view == DashboardView::room_screens) {
            draw_room_screen_wall(clients, dashboard);
        } else {
            draw_selected_client(clients, selected, dashboard, control_plane);
        }
        ImGui::End();

        ImGui::Render();
        constexpr float clear_color[4] = {0.975f, 0.971f, 0.958f, 1.0f};
        g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_render_target.Get(), clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    control_plane.stop();
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
