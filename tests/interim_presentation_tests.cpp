#include "../server/app/interim_presentation.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>

namespace {

std::array<float, 25> geometry(const ImGuiStyle& style) {
    return {
        style.WindowPadding.x, style.WindowPadding.y,
        style.FramePadding.x, style.FramePadding.y,
        style.CellPadding.x, style.CellPadding.y,
        style.ItemSpacing.x, style.ItemSpacing.y,
        style.ItemInnerSpacing.x, style.ItemInnerSpacing.y,
        style.WindowRounding, style.ChildRounding, style.FrameRounding,
        style.PopupRounding, style.ScrollbarRounding, style.GrabRounding,
        style.TabRounding, style.IndentSpacing, style.ScrollbarSize,
        style.GrabMinSize, style.WindowBorderSize, style.ChildBorderSize,
        style.PopupBorderSize, style.FrameBorderSize, style.TabBorderSize,
    };
}

void repeated_theme_swaps_preserve_absolute_geometry() {
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        ImGuiStyle expected;
        nstu::ui::apply_interim_style(expected, false, scale);
        const auto expected_geometry = geometry(expected);
        ImGuiStyle reused;
        for (int swap = 0; swap < 40; ++swap) {
            // Other components may temporarily customize style. A theme swap
            // must reconstruct its baseline rather than rescale those changes.
            reused.WindowPadding = {137.0f, 89.0f};
            reused.ScrollbarSize = 55.0f;
            reused.FrameRounding = 97.0f;
            nstu::ui::apply_interim_style(reused, swap % 2 == 0, scale);
            assert(geometry(reused) == expected_geometry);
        }
    }
}

void monitor_scale_changes_do_not_reuse_scaled_metrics() {
    ImGuiStyle reused;
    for (const float scale : {2.0f, 1.0f, 1.5f, 1.25f, 2.0f, 1.0f}) {
        nstu::ui::apply_interim_style(reused, true, scale);
        ImGuiStyle expected;
        nstu::ui::apply_interim_style(expected, true, scale);
        assert(geometry(reused) == geometry(expected));
    }
}

void theme_restores_defaults_and_applies_the_absolute_scale_once() {
    const ImGuiStyle defaults;
    ImGuiStyle reused;
    for (const float scale : {1.0f, 1.1f, 1.25f, 1.5f, 2.0f}) {
        for (const bool dark : {false, true}) {
            reused.WindowMinSize = {777.0f, 555.0f};
            reused.Alpha = 0.1f;
            nstu::ui::apply_interim_style(reused, dark, scale);
            assert(reused.Alpha == defaults.Alpha);
            assert(reused.WindowMinSize.x ==
                   std::trunc(defaults.WindowMinSize.x * scale));
            assert(reused.WindowMinSize.y ==
                   std::trunc(defaults.WindowMinSize.y * scale));
            // The specification fixes the baseline inset at 24 DIP. The
            // expected value is independent of the adapter under test.
            assert(reused.WindowPadding.x == std::trunc(24.0f * scale));
            assert(reused.WindowPadding.y == std::trunc(24.0f * scale));
        }
    }
}

double linear_channel(float channel) {
    const double value = static_cast<double>(channel);
    return value <= 0.04045 ? value / 12.92
                           : std::pow((value + 0.055) / 1.055, 2.4);
}

double luminance(const ImVec4& color) {
    return 0.2126 * linear_channel(color.x) +
           0.7152 * linear_channel(color.y) +
           0.0722 * linear_channel(color.z);
}

double contrast(const ImVec4& foreground, const ImVec4& background) {
    const double a = luminance(foreground);
    const double b = luminance(background);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

void normal_text_and_primary_actions_keep_readable_contrast() {
    for (const bool dark : {false, true}) {
        ImGuiStyle style;
        nstu::ui::apply_interim_style(style, dark, 1.0f);
        for (const auto background : {
                 ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg,
                 ImGuiCol_FrameBg, ImGuiCol_Button}) {
            assert(contrast(style.Colors[ImGuiCol_Text],
                            style.Colors[background]) >= 4.5);
            // Existing native renderers use TextDisabled for address, latency
            // and other readable metadata, rather than only disabled controls.
            assert(contrast(style.Colors[ImGuiCol_TextDisabled],
                            style.Colors[background]) >= 4.5);
            assert(contrast(
                       nstu::ui::interim_color(nstu::ui::ColorRole::muted, dark),
                       style.Colors[background]) >= 4.5);
        }
        const auto label =
            nstu::ui::interim_color(nstu::ui::ColorRole::on_accent, dark);
        assert(contrast(label, nstu::ui::interim_color(
                                   nstu::ui::ColorRole::accent, dark)) >= 4.5);
        assert(contrast(label, nstu::ui::interim_color(
                                   nstu::ui::ColorRole::accent_hover, dark)) >= 4.5);
    }
}

} // namespace

int main() {
    repeated_theme_swaps_preserve_absolute_geometry();
    monitor_scale_changes_do_not_reuse_scaled_metrics();
    theme_restores_defaults_and_applies_the_absolute_scale_once();
    normal_text_and_primary_actions_keep_readable_contrast();
    std::cout << "interim presentation tests passed\n";
}
