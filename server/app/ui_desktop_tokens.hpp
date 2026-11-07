// Generated from config/ui-desktop-tokens.json; do not edit.
// SHA256: 080f3bf8a4cc5b873142f99ed2669b2718a0cd15e934db1d50528fc2ebe8a030
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace nstu::ui::desktop_tokens {
inline constexpr char source_sha256[] = "080f3bf8a4cc5b873142f99ed2669b2718a0cd15e934db1d50528fc2ebe8a030";
inline constexpr float k_reference_dpi = 96.0f;
enum class ColorRole : std::size_t {
    k_accent,
    k_accent_hover,
    k_action_text,
    k_border,
    k_canvas,
    k_control,
    k_danger,
    k_danger_bg,
    k_disabled,
    k_focus,
    k_hover,
    k_info,
    k_info_bg,
    k_input_border,
    k_media,
    k_muted,
    k_offline,
    k_offline_bg,
    k_on_accent,
    k_pressed,
    k_raised,
    k_selected,
    k_selected_text,
    k_success,
    k_success_bg,
    k_surface,
    k_text,
    k_warning,
    k_warning_bg,
    k_window,
    count
};
using Palette = std::array<std::uint32_t, static_cast<std::size_t>(ColorRole::count)>;
inline constexpr Palette light_palette{
    0x2B6590, // accent
    0x235779, // accent-hover
    0x235D88, // action-text
    0xDADDE3, // border
    0xF7F8FA, // canvas
    0xFFFFFF, // control
    0xAE3941, // danger
    0xFBEFF0, // danger-bg
    0x858C96, // disabled
    0x2B6590, // focus
    0xEAECF0, // hover
    0x235D88, // info
    0xECF3F9, // info-bg
    0x818A96, // input-border
    0xE9EDF2, // media
    0x5D626B, // muted
    0x5F6974, // offline
    0xEEF1F4, // offline-bg
    0xFFFFFF, // on-accent
    0xDFE3E9, // pressed
    0xFFFFFF, // raised
    0xE7EDF3, // selected
    0x25282D, // selected-text
    0x237547, // success
    0xEBF4ED, // success-bg
    0xFFFFFF, // surface
    0x25282D, // text
    0x86611D, // warning
    0xFFF4DB, // warning-bg
    0xF1F2F4, // window
};
inline constexpr Palette dark_palette{
    0x365F7D, // accent
    0x416F91, // accent-hover
    0xA2CBE9, // action-text
    0x41444B, // border
    0x202124, // canvas
    0x2B2D31, // control
    0xE3A0A6, // danger
    0x402D32, // danger-bg
    0x777D88, // disabled
    0x90BDDF, // focus
    0x34363B, // hover
    0xA2CBE9, // info
    0x2B3A49, // info-bg
    0x7C8390, // input-border
    0x17191D, // media
    0xB0B5BE, // muted
    0xB3BAC5, // offline
    0x30343B, // offline-bg
    0xF3F8FC, // on-accent
    0x3A3D43, // pressed
    0x2B2D31, // raised
    0x323842, // selected
    0xE4E6EB, // selected-text
    0x8FCAA5, // success
    0x28382E, // success-bg
    0x232427, // surface
    0xE4E6EB, // text
    0xE0BD7B, // warning
    0x3D3426, // warning-bg
    0x26272B, // window
};
[[nodiscard]] constexpr std::uint32_t color(ColorRole role, bool dark) noexcept {
    return (dark ? dark_palette : light_palette)[static_cast<std::size_t>(role)];
}
namespace spacing {
inline constexpr float k_lg = 16.0f;
inline constexpr float k_md = 12.0f;
inline constexpr float k_sm = 8.0f;
inline constexpr float k_xl = 24.0f;
inline constexpr float k_xs = 4.0f;
inline constexpr float k_xxl = 32.0f;
} // namespace spacing
namespace radius {
inline constexpr float k_badge = 0.0f;
inline constexpr float k_card = 5.0f;
inline constexpr float k_control = 4.0f;
inline constexpr float k_dialog = 8.0f;
inline constexpr float k_window = 0.0f;
} // namespace radius
namespace geometry {
inline constexpr float k_card_min_width = 208.0f;
inline constexpr float k_content_inset = 24.0f;
inline constexpr float k_content_inset_compact = 16.0f;
inline constexpr float k_dialog_inset = 24.0f;
inline constexpr float k_dialog_max_width = 860.0f;
inline constexpr float k_dialog_width = 640.0f;
inline constexpr float k_focus_target_list = 220.0f;
inline constexpr float k_focus_target_list_compact = 180.0f;
inline constexpr float k_grid_gap = 12.0f;
inline constexpr float k_harness = 44.0f;
inline constexpr float k_sidebar = 200.0f;
inline constexpr float k_sidebar_collapsed = 64.0f;
inline constexpr float k_sidebar_compact = 176.0f;
inline constexpr float k_snapshot_aspect = 1.777778f;
inline constexpr float k_statusbar = 26.0f;
inline constexpr float k_studio_canvas_inset = 32.0f;
inline constexpr float k_studio_canvas_inset_compact = 24.0f;
inline constexpr float k_studio_commandbar = 48.0f;
inline constexpr float k_studio_inspector = 276.0f;
inline constexpr float k_studio_inspector_compact = 236.0f;
inline constexpr float k_studio_instruction_height = 62.0f;
inline constexpr float k_studio_passage_height = 146.0f;
inline constexpr float k_studio_prompt_height = 76.0f;
inline constexpr float k_studio_question_rail = 224.0f;
inline constexpr float k_studio_question_rail_compact = 192.0f;
inline constexpr float k_studio_titlebar = 52.0f;
inline constexpr float k_table_header = 36.0f;
inline constexpr float k_table_row_min = 40.0f;
inline constexpr float k_titlebar = 64.0f;
inline constexpr float k_toolbar_min = 46.0f;
} // namespace geometry
namespace controls {
inline constexpr float k_border_width = 1.0f;
inline constexpr float k_command_height = 32.0f;
inline constexpr float k_default_height = 34.0f;
inline constexpr float k_focus_ring = 2.0f;
inline constexpr float k_icon_size = 16.0f;
inline constexpr float k_icon_stroke = 1.7f;
inline constexpr float k_row_height = 28.0f;
} // namespace controls
namespace typography {
inline constexpr float k_body = 14.0f;
inline constexpr float k_candidate_body = 16.0f;
inline constexpr std::array<unsigned, 4> k_candidate_scale_options{100, 110, 120, 130};
inline constexpr float k_caption = 12.0f;
inline constexpr char k_fallback[] = "system-ui, sans-serif";
inline constexpr char k_family[] = "Segoe UI";
inline constexpr float k_line_height = 1.5f;
inline constexpr float k_page_title = 20.0f;
inline constexpr float k_section_title = 16.0f;
inline constexpr float k_studio_prompt = 18.0f;
inline constexpr float k_studio_question_title = 24.0f;
inline constexpr float k_weights_medium = 500.0f;
inline constexpr float k_weights_regular = 400.0f;
inline constexpr float k_weights_semibold = 600.0f;
} // namespace typography
} // namespace nstu::ui::desktop_tokens
