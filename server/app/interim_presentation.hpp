#pragma once

#include "imgui.h"
#include "ui_desktop_tokens.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Presentation adapter for the reviewed generated desktop tokens. This layer
// accepts only ImGui style and appearance; it cannot change operational state.
namespace nstu::ui {

enum class ColorRole : std::size_t {
    canvas, window, surface, raised, control, hover, pressed, border,
    input_border, text, muted, disabled, accent, accent_hover, on_accent,
    selected, selected_text, focus, success, success_bg, warning, warning_bg,
    danger, danger_bg, offline, offline_bg, info, info_bg, media, action_text, count,
};

using Palette =
    std::array<std::uint32_t, static_cast<std::size_t>(ColorRole::count)>;

[[nodiscard]] constexpr Palette projected_palette(bool dark) noexcept {
    // Preserve native enum ordinals while generated roles remain sorted by name.
    using Role = desktop_tokens::ColorRole;
    constexpr std::array<Role, static_cast<std::size_t>(ColorRole::count)> roles{
        Role::k_canvas, Role::k_window, Role::k_surface, Role::k_raised,
        Role::k_control, Role::k_hover, Role::k_pressed, Role::k_border,
        Role::k_input_border, Role::k_text, Role::k_muted, Role::k_disabled,
        Role::k_accent, Role::k_accent_hover, Role::k_on_accent,
        Role::k_selected, Role::k_selected_text, Role::k_focus,
        Role::k_success, Role::k_success_bg, Role::k_warning, Role::k_warning_bg,
        Role::k_danger, Role::k_danger_bg, Role::k_offline, Role::k_offline_bg,
        Role::k_info, Role::k_info_bg, Role::k_media, Role::k_action_text,
    };
    Palette result{};
    for (std::size_t index = 0; index < roles.size(); ++index) {
        result[index] = desktop_tokens::color(roles[index], dark);
    }
    return result;
}

inline constexpr Palette kLightPalette = projected_palette(false);
inline constexpr Palette kDarkPalette = projected_palette(true);

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
inline constexpr float xs = desktop_tokens::spacing::k_xs;
inline constexpr float sm = desktop_tokens::spacing::k_sm;
inline constexpr float md = desktop_tokens::spacing::k_md;
inline constexpr float lg = desktop_tokens::spacing::k_lg;
inline constexpr float xl = desktop_tokens::spacing::k_xl;
inline constexpr float xxl = desktop_tokens::spacing::k_xxl;
} // namespace spacing

namespace radius {
inline constexpr float window = desktop_tokens::radius::k_window;
inline constexpr float control = desktop_tokens::radius::k_control;
inline constexpr float card = desktop_tokens::radius::k_card;
inline constexpr float dialog = desktop_tokens::radius::k_dialog;
inline constexpr float badge = desktop_tokens::radius::k_badge;
} // namespace radius

namespace geometry {
inline constexpr float reference_dpi = desktop_tokens::k_reference_dpi;
inline constexpr float outline = desktop_tokens::controls::k_border_width;
inline constexpr float shell_compact_breakpoint = 1100.0f;
inline constexpr float sidebar = desktop_tokens::geometry::k_sidebar;
inline constexpr float sidebar_compact = desktop_tokens::geometry::k_sidebar_compact;
inline constexpr float header = desktop_tokens::geometry::k_titlebar;
inline constexpr float toolbar_min = desktop_tokens::geometry::k_toolbar_min;
inline constexpr float statusbar = desktop_tokens::geometry::k_statusbar;
inline constexpr float content_inset = desktop_tokens::geometry::k_content_inset;
inline constexpr float content_inset_compact = desktop_tokens::geometry::k_content_inset_compact;
inline constexpr float table_row_min = desktop_tokens::geometry::k_table_row_min;
inline constexpr float table_header = desktop_tokens::geometry::k_table_header;
inline constexpr float classroom_table_min = 920.0f;
inline constexpr float classroom_search_min = 220.0f;
inline constexpr float classroom_filter = 136.0f;
inline constexpr float card_min_width = desktop_tokens::geometry::k_card_min_width;
inline constexpr float grid_gap = desktop_tokens::geometry::k_grid_gap;
inline constexpr float snapshot_aspect = desktop_tokens::geometry::k_snapshot_aspect;
inline constexpr float focus_targets = desktop_tokens::geometry::k_focus_target_list;
inline constexpr float focus_targets_compact = desktop_tokens::geometry::k_focus_target_list_compact;
inline constexpr float focus_viewport_min = 250.0f;
inline constexpr float dialog_width = desktop_tokens::geometry::k_dialog_width;
inline constexpr float dialog_max_width = desktop_tokens::geometry::k_dialog_max_width;
inline constexpr float dialog_inset = desktop_tokens::geometry::k_dialog_inset;
} // namespace geometry

namespace typography {
inline constexpr float page_title = desktop_tokens::typography::k_page_title;
inline constexpr float section_title = desktop_tokens::typography::k_section_title;
inline constexpr float body = desktop_tokens::typography::k_body;
inline constexpr float caption = desktop_tokens::typography::k_caption;
inline constexpr float candidate_body = desktop_tokens::typography::k_candidate_body;
} // namespace typography

namespace controls {
inline constexpr float default_height = desktop_tokens::controls::k_default_height;
inline constexpr float command_height = desktop_tokens::controls::k_command_height;
inline constexpr float row_height = desktop_tokens::controls::k_row_height;
inline constexpr float icon_size = desktop_tokens::controls::k_icon_size;
inline constexpr float icon_stroke = desktop_tokens::controls::k_icon_stroke;
inline constexpr float border_width = desktop_tokens::controls::k_border_width;
inline constexpr float focus_ring = desktop_tokens::controls::k_focus_ring;
} // namespace controls

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
