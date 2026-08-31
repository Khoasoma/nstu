#include "nstu/client_registry.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/deployment.hpp"
#include "nstu/key_store.hpp"
#include "nstu/screen_snapshot.hpp"
#include "nstu/secret_store.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <windows.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_render_target;
ImFont* g_heading_font = nullptr;
bool g_dark_mode = false;

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayToggleWindow = 2001;
constexpr UINT kTrayExit = 2002;
UINT g_taskbar_created_message = 0;
NOTIFYICONDATAW g_tray_icon{};

constexpr int kMinimumSnapshotInterval = 5;
constexpr int kMaximumSnapshotInterval = 10;

enum class DashboardView : int {
    room_screens,
    selected_client,
};

enum class Language : int {
    english,
    vietnamese,
};

Language g_language = Language::english;

struct DashboardState {
    DashboardView view = DashboardView::room_screens;
    std::uint64_t selected_client_id = 0;
    int snapshot_interval_seconds = 7;
    Language language = Language::english;
    bool dark_mode = false;
    bool show_offline = true;
    bool broadcast_enabled = false;
    bool annotation_enabled = false;
    bool annotation_dragging = false;
    ImVec2 previous_annotation_point{};
    std::uint32_t annotation_rgba = 0xe5484dffu;
    int annotation_thickness = 4;
    std::chrono::steady_clock::time_point next_host_snapshot{};
    std::string control_status;
    std::string startup_error;
    std::array<char, 96> client_filter{};
    std::array<char, 512> chat_input{};
};

const char* tr(const DashboardState& state, const char* english,
               const char* vietnamese) {
    return state.language == Language::vietnamese ? vietnamese : english;
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
    std::uint64_t generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
};

std::unordered_map<std::uint64_t, SnapshotTexture> g_snapshot_textures;

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
    if (message == WM_SYSCOMMAND &&
        (wparam & 0xfff0u) == SC_MINIMIZE) {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    if (message == WM_CLOSE) {
        ShowWindow(window, SW_HIDE);
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

void apply_dashboard_style(bool dark_mode) {
    g_dark_mode = dark_mode;
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
    colors[ImGuiCol_Text] = dark_mode
        ? ImVec4{0.91f, 0.90f, 0.87f, 1.0f}
        : ImVec4{0.12f, 0.12f, 0.11f, 1.0f};
    colors[ImGuiCol_TextDisabled] = dark_mode
        ? ImVec4{0.61f, 0.60f, 0.57f, 1.0f}
        : ImVec4{0.46f, 0.45f, 0.43f, 1.0f};
    colors[ImGuiCol_WindowBg] = dark_mode
        ? ImVec4{0.075f, 0.075f, 0.070f, 1.0f}
        : ImVec4{0.975f, 0.971f, 0.958f, 1.0f};
    colors[ImGuiCol_ChildBg] = dark_mode
        ? ImVec4{0.105f, 0.105f, 0.098f, 1.0f}
        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f};
    colors[ImGuiCol_PopupBg] = dark_mode
        ? ImVec4{0.12f, 0.12f, 0.11f, 1.0f}
        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f};
    colors[ImGuiCol_Border] = dark_mode
        ? ImVec4{0.22f, 0.22f, 0.20f, 1.0f}
        : ImVec4{0.90f, 0.89f, 0.87f, 1.0f};
    colors[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_FrameBg] = dark_mode
        ? ImVec4{0.14f, 0.14f, 0.13f, 1.0f}
        : ImVec4{0.985f, 0.982f, 0.973f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = dark_mode
        ? ImVec4{0.20f, 0.20f, 0.18f, 1.0f}
        : ImVec4{0.95f, 0.945f, 0.93f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = dark_mode
        ? ImVec4{0.25f, 0.25f, 0.23f, 1.0f}
        : ImVec4{0.92f, 0.915f, 0.90f, 1.0f};
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_Button] = dark_mode
        ? ImVec4{0.17f, 0.17f, 0.16f, 1.0f}
        : ImVec4{0.93f, 0.925f, 0.91f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = dark_mode
        ? ImVec4{0.24f, 0.24f, 0.22f, 1.0f}
        : ImVec4{0.88f, 0.875f, 0.86f, 1.0f};
    colors[ImGuiCol_ButtonActive] = dark_mode
        ? ImVec4{0.30f, 0.30f, 0.28f, 1.0f}
        : ImVec4{0.83f, 0.825f, 0.81f, 1.0f};
    colors[ImGuiCol_Header] = dark_mode
        ? ImVec4{0.17f, 0.24f, 0.27f, 1.0f}
        : ImVec4{0.88f, 0.93f, 0.95f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = dark_mode
        ? ImVec4{0.21f, 0.31f, 0.35f, 1.0f}
        : ImVec4{0.82f, 0.90f, 0.94f, 1.0f};
    colors[ImGuiCol_HeaderActive] = dark_mode
        ? ImVec4{0.24f, 0.36f, 0.41f, 1.0f}
        : ImVec4{0.76f, 0.87f, 0.92f, 1.0f};
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_CheckMark] = dark_mode
        ? ImVec4{0.52f, 0.74f, 0.82f, 1.0f}
        : ImVec4{0.18f, 0.36f, 0.45f, 1.0f};
    colors[ImGuiCol_SliderGrab] = dark_mode
        ? ImVec4{0.72f, 0.71f, 0.68f, 1.0f}
        : ImVec4{0.18f, 0.18f, 0.17f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = dark_mode
        ? ImVec4{0.90f, 0.89f, 0.86f, 1.0f}
        : ImVec4{0.30f, 0.30f, 0.29f, 1.0f};
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
        atlas->AddFontFromFileTTF(regular.c_str(), 17.0f, nullptr,
                                  glyph_ranges);
    }
    if (GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_heading_font = atlas->AddFontFromFileTTF(
            bold.c_str(), 22.0f, nullptr, glyph_ranges);
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

void draw_status_badge(nstu::server::ClientStatus status,
                       const DashboardState& state) {
    const char* label = client_status_label(status, state);
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
    if (ImGui::BeginChild("summary", {width, 62.0f}, true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextColored(accent, "%zu", value);
        ImGui::TextDisabled("%s", label);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

bool draw_mode_button(const char* label, DashboardView value,
                      DashboardState& state) {
    const bool selected = state.view == value;
    ImGui::PushStyleColor(ImGuiCol_Button,
        selected ? (g_dark_mode ? ImVec4{0.82f, 0.81f, 0.78f, 1.0f}
                                : ImVec4{0.12f, 0.12f, 0.11f, 1.0f})
                 : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        selected ? (g_dark_mode ? ImVec4{0.90f, 0.89f, 0.86f, 1.0f}
                                : ImVec4{0.20f, 0.20f, 0.19f, 1.0f})
                 : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        selected ? (g_dark_mode ? ImVec4{0.96f, 0.95f, 0.92f, 1.0f}
                                : ImVec4{0.24f, 0.24f, 0.23f, 1.0f})
                 : ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleColor(ImGuiCol_Text,
        selected ? (g_dark_mode ? ImVec4{0.10f, 0.10f, 0.09f, 1.0f}
                                : ImVec4{1.0f, 1.0f, 1.0f, 1.0f})
                 : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    const bool pressed = ImGui::Button(label, {142.0f, 34.0f});
    ImGui::PopStyleColor(4);
    if (pressed) {
        state.view = value;
    }
    return pressed;
}

void draw_snapshot_stepper(DashboardState& state) {
    ImGui::TextUnformatted(tr(state, "Snapshot interval", "Chu kỳ chụp"));
    ImGui::SameLine();
    ImGui::BeginDisabled(state.snapshot_interval_seconds <=
                         kMinimumSnapshotInterval);
    if (ImGui::Button("-", {30.0f, 30.0f})) {
        --state.snapshot_interval_seconds;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(68.0f);
    ImGui::InputInt("##snapshot-interval", &state.snapshot_interval_seconds,
                    0, 0,
                    ImGuiInputTextFlags_ReadOnly);
    state.snapshot_interval_seconds = std::clamp(
        state.snapshot_interval_seconds, kMinimumSnapshotInterval,
        kMaximumSnapshotInterval);
    ImGui::SameLine();
    ImGui::BeginDisabled(state.snapshot_interval_seconds >=
                         kMaximumSnapshotInterval);
    if (ImGui::Button("+", {30.0f, 30.0f})) {
        ++state.snapshot_interval_seconds;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr(state, "seconds", "giây"));
}

SnapshotTexture* snapshot_texture(
    const nstu::server::ClientRecord& client) {
    if (client.snapshot_generation == 0 || client.snapshot_jpeg.empty()) {
        return nullptr;
    }
    auto& cached = g_snapshot_textures[client.id];
    if (cached.generation == client.snapshot_generation && cached.view) {
        return &cached;
    }
    nstu::screen::JpegImage jpeg{
        client.snapshot_width, client.snapshot_height, client.snapshot_jpeg};
    nstu::screen::BgraImage decoded;
    if (!nstu::screen::decode_jpeg(jpeg, decoded, nullptr)) {
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
    if (FAILED(g_device->CreateTexture2D(&description, &data, &texture)) ||
        FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr,
                                                  &view))) {
        return nullptr;
    }
    cached.generation = client.snapshot_generation;
    cached.width = decoded.width;
    cached.height = decoded.height;
    cached.view = std::move(view);
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
    draw_list->AddRectFilled(minimum, maximum, background, 5.0f);
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
    if (ImGui::BeginChild("client-card", {width, width * 0.5625f + 92.0f},
                          true, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextUnformatted(client.hostname.c_str());
        const float badge_width =
            ImGui::CalcTextSize(client_status_label(client.status, state)).x +
            16.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - badge_width);
        draw_status_badge(client.status, state);
        if (draw_screen_surface(client, width * 0.5625f, "##screen", state)
                .clicked) {
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
        if (ImGui::Button(tr(state, "Open", "Mở"), {open_width, 0.0f})) {
            state.selected_client_id = client.id;
            state.view = DashboardView::selected_client;
            state.annotation_enabled = false;
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

    ImGui::TextUnformatted(tr(state, "Latest room snapshots",
                              "Ảnh chụp mới nhất trong phòng"));
    ImGui::SameLine();
    ImGui::TextDisabled(tr(state, "%zu shown", "Đang hiển thị %zu"),
                        visible_clients.size());
    ImGui::Separator();
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
        ImGui::TextUnformatted(tr(state, "Clients", "Máy học sinh"));
        ImGui::Separator();
        if (ImGui::BeginTable("focus-clients", 2,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(tr(state, "Client", "Máy"),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr(state, "Status", "Trạng thái"),
                                    ImGuiTableColumnFlags_WidthFixed, 94.0f);
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
                    state.annotation_enabled = false;
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
    ImGui::TextDisabled("%s", selected_client->address.c_str());

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
    const float preview_width = ImGui::GetContentRegionAvail().x;
    const float preview_height = std::clamp(
        preview_width * 0.5625f, 220.0f,
        std::max(220.0f, ImGui::GetContentRegionAvail().y - 210.0f));
    const auto surface = draw_screen_surface(
        *selected_client, preview_height, "##focus-screen", state);
    if (state.annotation_enabled && surface.has_frame) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inside = mouse.x >= surface.image_min.x &&
                            mouse.x <= surface.image_max.x &&
                            mouse.y >= surface.image_min.y &&
                            mouse.y <= surface.image_max.y;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && inside) {
            if (state.annotation_dragging) {
                const float delta_x = mouse.x - state.previous_annotation_point.x;
                const float delta_y = mouse.y - state.previous_annotation_point.y;
                if (delta_x * delta_x + delta_y * delta_y >= 9.0f) {
                    const auto normalized = [&](float value, float minimum,
                                                float maximum) {
                        const float ratio = std::clamp(
                            (value - minimum) / (maximum - minimum), 0.0f,
                            1.0f);
                        return static_cast<std::uint16_t>(ratio * 65535.0f);
                    };
                    const nstu::control::OverlayStroke stroke{
                        .x0 = normalized(state.previous_annotation_point.x,
                                         surface.image_min.x,
                                         surface.image_max.x),
                        .y0 = normalized(state.previous_annotation_point.y,
                                         surface.image_min.y,
                                         surface.image_max.y),
                        .x1 = normalized(mouse.x, surface.image_min.x,
                                         surface.image_max.x),
                        .y1 = normalized(mouse.y, surface.image_min.y,
                                         surface.image_max.y),
                        .thickness = static_cast<std::uint16_t>(
                            state.annotation_thickness),
                        .rgba = state.annotation_rgba,
                    };
                    std::string ignored_error;
                    (void)control_plane.send_overlay_stroke(
                        selected_client->id, stroke, &ignored_error);
                    state.previous_annotation_point = mouse;
                }
            } else {
                state.annotation_dragging = true;
                state.previous_annotation_point = mouse;
            }
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
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
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(4);
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
        state.control_status =
            control_plane.clear_overlay(selected_client->id, &error)
                ? tr(state, "Student overlay cleared.",
                     "Đã xóa lớp vẽ trên máy học sinh.")
                : error;
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
        state.control_status =
            control_plane.set_locked(selected_client->id, !is_locked, &error)
                ? (is_locked
                       ? tr(state, "Unlock command sent.",
                            "Đã gửi lệnh mở khóa.")
                       : tr(state, "Lock command sent.",
                            "Đã gửi lệnh khóa."))
                : error;
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(3);
    if (state.annotation_enabled) {
        ImGui::TextUnformatted(tr(state, "Pen", "Bút"));
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
                                   {24.0f, 24.0f})) {
                state.annotation_rgba = color;
            }
            ImGui::PopID();
            if (index + 1 != colors.size()) {
                ImGui::SameLine();
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderInt(tr(state, "Thickness", "Độ dày"),
                         &state.annotation_thickness, 2, 12);
    }
    if (ImGui::BeginPopup("control-status")) {
        ImGui::TextUnformatted(state.control_status.c_str());
        ImGui::EndPopup();
    }

    if (ImGui::BeginChild("chat-panel", {0, 112.0f}, true)) {
        ImGui::TextUnformatted(tr(state, "Chat", "Trò chuyện"));
        ImGui::TextDisabled("%s", tr(state, "No messages in this session.",
                                      "Chưa có tin nhắn trong phiên này."));
        ImGui::SetNextItemWidth(-78.0f);
        const bool submit = ImGui::InputText(
            "##chat-input", state.chat_input.data(), state.chat_input.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((submit || ImGui::Button(tr(state, "Send", "Gửi"),
                                     {68.0f, 0.0f})) &&
            state.chat_input[0] != '\0') {
            std::string error;
            if (control_plane.send_chat(selected_client->id,
                                        state.chat_input.data(), &error)) {
                state.chat_input[0] = '\0';
                state.control_status = tr(state, "Message sent.",
                                          "Đã gửi tin nhắn.");
            } else {
                state.control_status = error;
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
            g_dark_mode ? ImVec4{0.82f, 0.81f, 0.78f, 1.0f}
                        : ImVec4{0.12f, 0.12f, 0.11f, 1.0f});
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            g_dark_mode ? ImVec4{0.10f, 0.10f, 0.09f, 1.0f}
                        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f});
    }
    const bool pressed = ImGui::Button(label, {width, 28.0f});
    if (selected) {
        ImGui::PopStyleColor(2);
    }
    return pressed;
}

void draw_preferences(DashboardState& state) {
    constexpr float preference_width = 242.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - preference_width);
    if (draw_segment_option("EN", state.language == Language::english,
                            38.0f)) {
        state.language = Language::english;
        g_language = state.language;
        state.control_status.clear();
    }
    ImGui::SameLine();
    if (draw_segment_option("VI", state.language == Language::vietnamese,
                            38.0f)) {
        state.language = Language::vietnamese;
        g_language = state.language;
        state.control_status.clear();
    }
    ImGui::SameLine();
    if (draw_segment_option(tr(state, "Light", "Sáng"), !state.dark_mode,
                            64.0f)) {
        state.dark_mode = false;
        apply_dashboard_style(false);
    }
    ImGui::SameLine();
    if (draw_segment_option(tr(state, "Dark", "Tối"), state.dark_mode,
                            64.0f)) {
        state.dark_mode = true;
        apply_dashboard_style(true);
    }
}

void draw_dashboard_header(const std::vector<nstu::server::ClientRecord>& clients,
                           DashboardState& state,
                           nstu::server::ServerControlPlane& control_plane) {
    const auto counts = count_room_statuses(clients);
    if (g_heading_font != nullptr) {
        ImGui::PushFont(g_heading_font);
    }
    ImGui::TextUnformatted(tr(state, "NSTU Classroom", "Lớp học NSTU"));
    if (g_heading_font != nullptr) {
        ImGui::PopFont();
    }
    draw_preferences(state);
    ImGui::TextDisabled("%s", tr(state, "Computer room overview",
                                  "Tổng quan phòng máy"));

    const float control_width = 334.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - control_width);
    draw_snapshot_stepper(state);
    if (!state.startup_error.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(
            status_text_color(nstu::server::ClientStatus::locked), "%s: %s",
            tr(state, "Control service unavailable",
               "Dịch vụ điều khiển không khả dụng"),
            state.startup_error.c_str());
        ImGui::PopTextWrapPos();
    }

    const auto send_room_snapshots = [&](bool enabled) {
        std::size_t sent = 0;
        std::string last_error;
        for (const auto& client : clients) {
            if (client.status == nstu::server::ClientStatus::offline) {
                continue;
            }
            std::string error;
            if (control_plane.set_snapshots(
                    client.id, enabled,
                    static_cast<std::uint16_t>(
                        state.snapshot_interval_seconds), &error)) {
                ++sent;
            } else {
                last_error = std::move(error);
            }
        }
        if (sent == 0) {
            state.control_status = last_error.empty()
                ? tr(state, "No online clients are available.",
                     "Không có máy trực tuyến để điều khiển.")
                : std::move(last_error);
        } else {
            state.control_status = enabled
                ? tr(state, "Room snapshot refresh started.",
                     "Đã bắt đầu chụp màn hình toàn phòng.")
                : tr(state, "Room snapshot refresh stopped.",
                     "Đã dừng chụp màn hình toàn phòng.");
        }
    };
    const auto set_room_lock = [&](bool locked) {
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
    };

    ImGui::BeginDisabled(clients.empty());
    if (ImGui::Button(tr(state, "Start room snapshots",
                         "Bắt đầu chụp toàn phòng"))) {
        send_room_snapshots(true);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Stop snapshots", "Dừng chụp"))) {
        send_room_snapshots(false);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Lock room", "Khóa phòng"))) {
        set_room_lock(true);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Unlock room", "Mở khóa phòng"))) {
        set_room_lock(false);
    }
    ImGui::SameLine();
    if (ImGui::Button(
            state.broadcast_enabled
                ? tr(state, "Stop teacher broadcast", "Dừng phát màn hình")
                : tr(state, "Broadcast teacher screen",
                     "Phát màn hình giáo viên"))) {
        if (state.broadcast_enabled) {
            std::string error;
            if (control_plane.stop_host_broadcast(&error)) {
                state.broadcast_enabled = false;
                state.control_status = tr(state, "Teacher broadcast stopped.",
                                          "Đã dừng phát màn hình giáo viên.");
            } else {
                state.control_status = error;
            }
        } else {
            state.broadcast_enabled = true;
            state.next_host_snapshot = {};
            state.control_status = tr(state, "Teacher broadcast started.",
                                      "Đã bắt đầu phát màn hình giáo viên.");
        }
    }
    ImGui::EndDisabled();
    if (!state.control_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.control_status.c_str());
    }

    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float summary_width =
        (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f;
    draw_summary_card(tr(state, "Online", "Trực tuyến"), counts.online,
                      status_text_color(nstu::server::ClientStatus::online),
                      summary_width);
    ImGui::SameLine();
    draw_summary_card(tr(state, "Needs attention", "Cần chú ý"),
                      counts.attention,
                      status_text_color(nstu::server::ClientStatus::degraded),
                      summary_width);
    ImGui::SameLine();
    draw_summary_card(tr(state, "Locked", "Đã khóa"), counts.locked,
                      status_text_color(nstu::server::ClientStatus::locked),
                      summary_width);
    ImGui::SameLine();
    draw_summary_card(tr(state, "Offline", "Ngoại tuyến"), counts.offline,
                      status_text_color(nstu::server::ClientStatus::offline),
                      summary_width);

    draw_mode_button(tr(state, "Room screens", "Màn hình phòng"),
                     DashboardView::room_screens, state);
    ImGui::SameLine();
    ImGui::BeginDisabled(clients.empty());
    draw_mode_button(tr(state, "Selected client", "Máy đang chọn"),
                     DashboardView::selected_client, state);
    ImGui::EndDisabled();
    const float filter_width = 220.0f;
    const char* show_offline_label =
        tr(state, "Show offline", "Hiện ngoại tuyến");
    const float checkbox_width =
        ImGui::CalcTextSize(show_offline_label).x + 34.0f;
    const float filter_start = ImGui::GetContentRegionMax().x -
                               filter_width - checkbox_width - gap;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + gap, filter_start));
    ImGui::SetNextItemWidth(filter_width);
    ImGui::InputTextWithHint("##client-filter",
                             tr(state, "Find client", "Tìm máy"),
                             state.client_filter.data(),
                             state.client_filter.size());
    ImGui::SameLine();
    ImGui::Checkbox(show_offline_label, &state.show_offline);
    ImGui::Separator();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"NstuServerWindow";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIcon =
        LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&window_class);
    HWND window = CreateWindowW(window_class.lpszClassName, L"NSTU Server",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 820, nullptr, nullptr,
                                instance, nullptr);
    if (window == nullptr || !create_device(window)) {
        return 1;
    }
    ShowWindow(window, SW_SHOWDEFAULT);
    add_tray_icon(window);

    DashboardState dashboard;
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_VIETNAMESE) {
        dashboard.language = Language::vietnamese;
    }
    const wchar_t* command_line = GetCommandLineW();
    const std::wstring_view arguments =
        command_line == nullptr ? std::wstring_view{} : command_line;
    if (arguments.find(L"--language=vi") != std::wstring_view::npos) {
        dashboard.language = Language::vietnamese;
    } else if (arguments.find(L"--language=en") != std::wstring_view::npos) {
        dashboard.language = Language::english;
    }
    dashboard.dark_mode =
        arguments.find(L"--dark") != std::wstring_view::npos;
    g_language = dashboard.language;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    apply_dashboard_style(dashboard.dark_mode);
    load_dashboard_fonts();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    nstu::server::ClientRegistry registry;
    nstu::security::KeyStore key_store;
    nstu::server::ServerControlPlane control_plane(registry, key_store);
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
        const auto now = std::chrono::steady_clock::now();
        if (dashboard.broadcast_enabled &&
            now >= dashboard.next_host_snapshot) {
            nstu::screen::JpegImage jpeg;
            std::string error;
            if (nstu::screen::capture_primary_screen_jpeg(
                    jpeg, 480, 270, 52,
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
                    dashboard.control_status = std::move(error);
                }
            } else {
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

        draw_dashboard_header(clients, dashboard, control_plane);
        if (dashboard.view == DashboardView::room_screens) {
            draw_room_screen_wall(clients, dashboard);
        } else {
            draw_selected_client(clients, selected, dashboard, control_plane);
        }
        ImGui::End();

        ImGui::Render();
        const ImVec4 background =
            ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const float clear_color[4] = {
            background.x, background.y, background.z, background.w};
        g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_render_target.Get(), clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);
    }

    control_plane.stop();
    Shell_NotifyIconW(NIM_DELETE, &g_tray_icon);
    g_snapshot_textures.clear();
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
