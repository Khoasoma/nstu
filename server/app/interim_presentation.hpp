#pragma once

#include "imgui.h"

#include <array>
#include <cstddef>
#include <cstdint>

// Presentation tokens from the approved NSTU interim UI handoff. This layer
// accepts only ImGui style and appearance; it cannot change operational state.
namespace nstu::ui {

enum class ColorRole : std::size_t {
    canvas, window, surface, raised, control, hover, pressed, border,
    input_border, text, muted, disabled, accent, accent_hover, on_accent,
    selected, selected_text, focus, success, success_bg, warning, warning_bg,
    danger, danger_bg, offline, offline_bg, info, info_bg, media, count,
};

using Palette =
    std::array<std::uint32_t, static_cast<std::size_t>(ColorRole::count)>;

inline constexpr Palette kLightPalette{
    0xF5F6F8, 0xEFF1F4, 0xFFFFFF, 0xFFFFFF, 0xF1F3F6, 0xE8EBF0,
    0xDDE2E9, 0xD6DAE0, 0x808994, 0x22262B, 0x5F6670, 0x858C96,
    0x23659C, 0x1E5687, 0xFFFFFF, 0xE4EEF7, 0x1C598D, 0x2779B7,
    0x237547, 0xEBF4ED, 0x86611D, 0xFFF4DB, 0xAE3941, 0xFBEFF0,
    0x5F6974, 0xEEF1F4, 0x235D88, 0xECF3F9, 0xE9EDF2,
};

inline constexpr Palette kDarkPalette{
    0x1D1E21, 0x222327, 0x25272B, 0x2C2E33, 0x2C2E33, 0x34363C,
    0x3B3E45, 0x41444C, 0x737B87, 0xE4E7EC, 0xAFB5BF, 0x777D88,
    0x355E80, 0x406D92, 0xF3F8FC, 0x2C3A47, 0xA2CBE9, 0x87B9E0,
    0x8FCAA5, 0x28382E, 0xE0BD7B, 0x3D3426, 0xE3A0A6, 0x402D32,
    0xB3BAC5, 0x30343B, 0xA2CBE9, 0x2B3A49, 0x17191D,
};

[[nodiscard]] inline ImVec4 interim_color(ColorRole role, bool dark) noexcept {
    const auto rgb = (dark ? kDarkPalette : kLightPalette)
        [static_cast<std::size_t>(role)];
    constexpr float channel_max = 255.0f;
    return {
        static_cast<float>((rgb >> 16u) & 0xFFu) / channel_max,
        static_cast<float>((rgb >> 8u) & 0xFFu) / channel_max,
        static_cast<float>(rgb & 0xFFu) / channel_max,
        1.0f,
    };
}

namespace spacing {
inline constexpr float xs = 4.0f;
inline constexpr float sm = 8.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 16.0f;
inline constexpr float xl = 24.0f;
inline constexpr float xxl = 32.0f;
} // namespace spacing

namespace radius {
inline constexpr float window = 0.0f;
inline constexpr float control = 8.0f;
inline constexpr float card = 10.0f;
inline constexpr float dialog = 12.0f;
inline constexpr float badge = 6.0f;
} // namespace radius

namespace geometry {
inline constexpr float reference_dpi = 96.0f;
inline constexpr float outline = 1.0f;
inline constexpr float shell_compact_breakpoint = 1100.0f;
inline constexpr float sidebar = 200.0f;
inline constexpr float sidebar_compact = 176.0f;
inline constexpr float header = 64.0f;
inline constexpr float toolbar_min = 44.0f;
inline constexpr float statusbar = 28.0f;
inline constexpr float content_inset = 24.0f;
inline constexpr float content_inset_compact = 16.0f;
inline constexpr float table_row_min = 40.0f;
inline constexpr float table_header = 36.0f;
inline constexpr float classroom_table_min = 920.0f;
inline constexpr float classroom_search_min = 220.0f;
inline constexpr float classroom_filter = 136.0f;
inline constexpr float card_min_width = 208.0f;
inline constexpr float grid_gap = 12.0f;
inline constexpr float snapshot_aspect = 16.0f / 9.0f;
inline constexpr float focus_targets = 220.0f;
inline constexpr float focus_targets_compact = 180.0f;
inline constexpr float focus_viewport_min = 250.0f;
inline constexpr float dialog_width = 640.0f;
inline constexpr float dialog_max_width = 860.0f;
inline constexpr float dialog_inset = 24.0f;
} // namespace geometry

namespace typography {
inline constexpr float page_title = 22.0f;
inline constexpr float section_title = 16.0f;
inline constexpr float body = 14.0f;
inline constexpr float caption = 12.0f;
inline constexpr float candidate_body = 16.0f;
} // namespace typography

inline void apply_interim_style(ImGuiStyle& style, bool dark, float scale) {
    // Rebuild all fields, including those a local widget may have customized.
    // This preserves the existing absolute-DPI behavior across theme swaps.
    style = ImGuiStyle{};
    style.WindowPadding = {spacing::xl, spacing::xl};
    style.FramePadding = {spacing::md, spacing::sm};
    style.CellPadding = {spacing::md, spacing::sm};
    style.ItemSpacing = {spacing::md, spacing::sm};
    style.ItemInnerSpacing = {spacing::sm, spacing::xs};
    style.IndentSpacing = spacing::xl;
    style.ScrollbarSize = spacing::lg;
    style.GrabMinSize = spacing::md;
    style.WindowRounding = radius::window;
    style.ChildRounding = radius::card;
    style.FrameRounding = radius::control;
    style.PopupRounding = radius::dialog;
    style.ScrollbarRounding = radius::control;
    style.GrabRounding = radius::control;
    style.TabRounding = radius::control;
    style.WindowBorderSize = radius::window;
    style.ChildBorderSize = geometry::outline;
    style.PopupBorderSize = geometry::outline;
    style.FrameBorderSize = geometry::outline;
    style.TabBorderSize = radius::window;

    const auto color = [dark](ColorRole role) {
        return interim_color(role, dark);
    };
    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = color(ColorRole::text);
    // Existing renderers use TextDisabled for readable secondary metadata.
    // Actual disabled controls retain their separate disabled presentation.
    colors[ImGuiCol_TextDisabled] = color(ColorRole::muted);
    colors[ImGuiCol_WindowBg] = color(ColorRole::canvas);
    colors[ImGuiCol_ChildBg] = color(ColorRole::surface);
    colors[ImGuiCol_PopupBg] = color(ColorRole::raised);
    colors[ImGuiCol_Border] = color(ColorRole::border);
    colors[ImGuiCol_BorderShadow] = {};
    colors[ImGuiCol_FrameBg] = color(ColorRole::control);
    colors[ImGuiCol_FrameBgHovered] = color(ColorRole::hover);
    colors[ImGuiCol_FrameBgActive] = color(ColorRole::pressed);
    colors[ImGuiCol_TitleBg] = color(ColorRole::window);
    colors[ImGuiCol_TitleBgActive] = color(ColorRole::window);
    colors[ImGuiCol_TitleBgCollapsed] = color(ColorRole::window);
    colors[ImGuiCol_MenuBarBg] = color(ColorRole::window);
    colors[ImGuiCol_ScrollbarBg] = color(ColorRole::window);
    colors[ImGuiCol_ScrollbarGrab] = color(ColorRole::border);
    colors[ImGuiCol_ScrollbarGrabHovered] = color(ColorRole::muted);
    colors[ImGuiCol_ScrollbarGrabActive] = color(ColorRole::focus);
    colors[ImGuiCol_CheckMark] = color(ColorRole::focus);
    colors[ImGuiCol_SliderGrab] = color(ColorRole::accent);
    colors[ImGuiCol_SliderGrabActive] = color(ColorRole::focus);
    colors[ImGuiCol_Button] = color(ColorRole::control);
    colors[ImGuiCol_ButtonHovered] = color(ColorRole::hover);
    colors[ImGuiCol_ButtonActive] = color(ColorRole::pressed);
    colors[ImGuiCol_Header] = color(ColorRole::selected);
    colors[ImGuiCol_HeaderHovered] = color(ColorRole::hover);
    colors[ImGuiCol_HeaderActive] = color(ColorRole::pressed);
    colors[ImGuiCol_Separator] = color(ColorRole::border);
    colors[ImGuiCol_SeparatorHovered] = color(ColorRole::focus);
    colors[ImGuiCol_SeparatorActive] = color(ColorRole::focus);
    colors[ImGuiCol_ResizeGrip] = color(ColorRole::border);
    colors[ImGuiCol_ResizeGripHovered] = color(ColorRole::muted);
    colors[ImGuiCol_ResizeGripActive] = color(ColorRole::focus);
    colors[ImGuiCol_Tab] = color(ColorRole::control);
    colors[ImGuiCol_TabHovered] = color(ColorRole::hover);
    colors[ImGuiCol_TabSelected] = color(ColorRole::selected);
    colors[ImGuiCol_TabSelectedOverline] = color(ColorRole::focus);
    colors[ImGuiCol_TabDimmed] = color(ColorRole::window);
    colors[ImGuiCol_TabDimmedSelected] = color(ColorRole::control);
    colors[ImGuiCol_TabDimmedSelectedOverline] = color(ColorRole::border);
    colors[ImGuiCol_TableHeaderBg] = color(ColorRole::control);
    colors[ImGuiCol_TableBorderStrong] = color(ColorRole::border);
    colors[ImGuiCol_TableBorderLight] = color(ColorRole::border);
    colors[ImGuiCol_TableRowBg] = color(ColorRole::surface);
    colors[ImGuiCol_TableRowBgAlt] = color(ColorRole::control);
    colors[ImGuiCol_TextSelectedBg] = color(ColorRole::selected);
    colors[ImGuiCol_InputTextCursor] = color(ColorRole::text);
    colors[ImGuiCol_PlotLines] = color(ColorRole::info);
    colors[ImGuiCol_PlotLinesHovered] = color(ColorRole::focus);
    colors[ImGuiCol_PlotHistogram] = color(ColorRole::warning);
    colors[ImGuiCol_PlotHistogramHovered] = color(ColorRole::warning);
    colors[ImGuiCol_TreeLines] = color(ColorRole::border);
    colors[ImGuiCol_DragDropTarget] = color(ColorRole::focus);
    colors[ImGuiCol_NavCursor] = color(ColorRole::focus);
    colors[ImGuiCol_NavWindowingHighlight] = color(ColorRole::focus);
    auto dimmed = color(ColorRole::media);
    dimmed.w = colors[ImGuiCol_NavWindowingDimBg].w;
    colors[ImGuiCol_NavWindowingDimBg] = dimmed;
    dimmed.w = colors[ImGuiCol_ModalWindowDimBg].w;
    colors[ImGuiCol_ModalWindowDimBg] = dimmed;

    // Exactly one scale pass. Fonts are separately loaded at the current DPI.
    style.ScaleAllSizes(scale);
}

} // namespace nstu::ui
