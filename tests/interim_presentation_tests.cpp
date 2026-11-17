#include "../server/app/interim_presentation.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <windows.h>

namespace {

void approved_native_metrics_replace_the_interim_values() {
    assert(nstu::ui::typography::page_title == 20.0f);
    assert(nstu::ui::radius::control == 4.0f);
    assert(nstu::ui::radius::card == 5.0f);
    assert(nstu::ui::radius::dialog == 8.0f);
    assert(nstu::ui::geometry::toolbar_min == 46.0f);
    assert(nstu::ui::geometry::statusbar == 26.0f);
    assert(nstu::ui::geometry::snapshot_aspect == 1.777778f);
    assert(nstu::ui::controls::default_height == 34.0f);
    assert(nstu::ui::controls::command_height == 32.0f);
    assert(nstu::ui::controls::row_height == 28.0f);
    assert(nstu::ui::controls::icon_size == 16.0f);
    assert(nstu::ui::controls::icon_stroke == 1.7f);
    assert(nstu::ui::controls::border_width == 1.0f);
    assert(nstu::ui::controls::focus_ring == 2.0f);
    struct Metric { float actual; float expected; };
    constexpr Metric unchanged[]{
        {nstu::ui::spacing::xs,4.0f}, {nstu::ui::spacing::sm,8.0f},
        {nstu::ui::spacing::md,12.0f}, {nstu::ui::spacing::lg,16.0f},
        {nstu::ui::spacing::xl,24.0f}, {nstu::ui::spacing::xxl,32.0f},
        {nstu::ui::radius::window,0.0f}, {nstu::ui::radius::badge,0.0f},
        {nstu::ui::geometry::reference_dpi,96.0f}, {nstu::ui::geometry::outline,1.0f},
        {nstu::ui::geometry::sidebar,200.0f}, {nstu::ui::geometry::sidebar_compact,176.0f},
        {nstu::ui::geometry::header,64.0f}, {nstu::ui::geometry::content_inset,24.0f},
        {nstu::ui::geometry::content_inset_compact,16.0f},
        {nstu::ui::geometry::table_row_min,40.0f}, {nstu::ui::geometry::table_header,36.0f},
        {nstu::ui::geometry::card_min_width,208.0f}, {nstu::ui::geometry::grid_gap,12.0f},
        {nstu::ui::geometry::focus_targets,220.0f}, {nstu::ui::geometry::focus_targets_compact,180.0f},
        {nstu::ui::geometry::dialog_width,640.0f}, {nstu::ui::geometry::dialog_max_width,860.0f},
        {nstu::ui::geometry::dialog_inset,24.0f}, {nstu::ui::typography::section_title,16.0f},
        {nstu::ui::typography::body,14.0f}, {nstu::ui::typography::caption,12.0f},
        {nstu::ui::typography::candidate_body,16.0f},
        {nstu::ui::geometry::shell_compact_breakpoint,1100.0f},
        {nstu::ui::geometry::classroom_table_min,920.0f},
        {nstu::ui::geometry::classroom_search_min,220.0f},
        {nstu::ui::geometry::classroom_filter,136.0f},
        {nstu::ui::geometry::focus_viewport_min,250.0f},
    };
    for (const auto& value : unchanged) assert(value.actual == value.expected);
}

void every_native_role_projects_the_approved_palette() {
    using Role = nstu::ui::ColorRole;
    struct Expected { Role role; std::uint32_t light; std::uint32_t dark; };
    // Independently named values from the approved source, in the stable native order.
    constexpr std::array<Expected, 30> expected{{
        {Role::canvas, 0xF7F8FA, 0x202124}, {Role::window, 0xF1F2F4, 0x26272B},
        {Role::surface, 0xFFFFFF, 0x232427}, {Role::raised, 0xFFFFFF, 0x2B2D31},
        {Role::control, 0xFFFFFF, 0x2B2D31}, {Role::hover, 0xEAECF0, 0x34363B},
        {Role::pressed, 0xDFE3E9, 0x3A3D43}, {Role::border, 0xDADDE3, 0x41444B},
        {Role::input_border, 0x818A96, 0x7C8390}, {Role::text, 0x25282D, 0xE4E6EB},
        {Role::muted, 0x5D626B, 0xB0B5BE}, {Role::disabled, 0x858C96, 0x777D88},
        {Role::accent, 0x2B6590, 0x365F7D}, {Role::accent_hover, 0x235779, 0x416F91},
        {Role::on_accent, 0xFFFFFF, 0xF3F8FC}, {Role::selected, 0xE7EDF3, 0x323842},
        {Role::selected_text, 0x25282D, 0xE4E6EB}, {Role::focus, 0x2B6590, 0x90BDDF},
        {Role::success, 0x237547, 0x8FCAA5}, {Role::success_bg, 0xEBF4ED, 0x28382E},
        {Role::warning, 0x86611D, 0xE0BD7B}, {Role::warning_bg, 0xFFF4DB, 0x3D3426},
        {Role::danger, 0xAE3941, 0xE3A0A6}, {Role::danger_bg, 0xFBEFF0, 0x402D32},
        {Role::offline, 0x5F6974, 0xB3BAC5}, {Role::offline_bg, 0xEEF1F4, 0x30343B},
        {Role::info, 0x235D88, 0xA2CBE9}, {Role::info_bg, 0xECF3F9, 0x2B3A49},
        {Role::media, 0xE9EDF2, 0x17191D}, {Role::action_text, 0x235D88, 0xA2CBE9},
    }};
    assert(static_cast<std::size_t>(Role::count) == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& value = expected[index];
        assert(static_cast<std::size_t>(value.role) == index);
        assert(nstu::ui::kLightPalette[index] == value.light);
        assert(nstu::ui::kDarkPalette[index] == value.dark);
        for (const bool dark : {false, true}) {
            const auto rgb = dark ? value.dark : value.light;
            const auto color = nstu::ui::interim_color(value.role, dark);
            assert(std::lround(color.x * 255.0f) == static_cast<long>((rgb >> 16u) & 0xFFu));
            assert(std::lround(color.y * 255.0f) == static_cast<long>((rgb >> 8u) & 0xFFu));
            assert(std::lround(color.z * 255.0f) == static_cast<long>(rgb & 0xFFu));
            assert(color.w == 1.0f);
        }
    }
}

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
        for (const auto background : {ImGuiCol_WindowBg, ImGuiCol_ChildBg,
                                      ImGuiCol_PopupBg, ImGuiCol_FrameBg,
                                      ImGuiCol_Button}) {
            assert(contrast(nstu::ui::interim_color(
                                nstu::ui::ColorRole::action_text, dark),
                            style.Colors[background]) >= 4.5);
        }
    }
}

} // namespace

void teacher_reading_sizes_fit_real_vietnamese_fonts() {
    char directory[MAX_PATH]{};
    const auto length = GetWindowsDirectoryA(directory, MAX_PATH);
    assert(length > 0 && length < MAX_PATH);
    const auto fonts = std::string(directory) + "\\Fonts\\";
    for (const float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (int preset = 0; preset < 4; ++preset) {
            const float zoom = std::array{1.0f, 1.25f, 1.5f, 2.0f}[static_cast<std::size_t>(preset)];
            assert(std::abs(nstu::ui::reading::font_size(14.0f, dpi, preset) - 18.2f * dpi * zoom) < 0.001f);
            assert(std::abs(nstu::ui::reading::font_size(12.0f, dpi, preset) - 15.6f * dpi * zoom) < 0.001f);
            ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {500.0f * dpi, 400.0f * dpi};
            io.DeltaTime = 1.0f / 60.0f;
            const auto body = nstu::ui::reading::load_font(*io.Fonts, fonts, false,
                nstu::ui::reading::font_size(14.0f, dpi, preset));
            assert(body.system_font && body.font);
            io.FontDefault = body.font;
            const auto caption = nstu::ui::reading::load_font(*io.Fonts, fonts, false,
                nstu::ui::reading::font_size(12.0f, dpi, preset));
            const auto heading = nstu::ui::reading::load_font(*io.Fonts, fonts, true,
                nstu::ui::reading::font_size(16.0f, dpi, preset));
            const auto title = nstu::ui::reading::load_font(*io.Fonts, fonts, true,
                nstu::ui::reading::font_size(20.0f, dpi, preset));
            assert(caption.system_font && heading.system_font && title.system_font);
            assert(io.Fonts->Build());
            nstu::ui::apply_teacher_style(ImGui::GetStyle(), false, dpi, preset);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Actual teacher font", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            assert(std::abs(body.font->LegacySize - 18.2f * dpi * zoom) < 0.001f);
            assert(ImGui::GetFontSize() == std::round(body.font->LegacySize));
            assert(ImGui::GetFrameHeight() + 0.01f >= 44.2f * dpi * zoom);
            const auto text = ImGui::CalcTextSize("Giáo viên kiểm tra đề thi: ế ể ợ Đ");
            assert(text.x > 0 && text.y > 0 && text.y < ImGui::GetFrameHeight());
            for (const ImWchar codepoint : {0x1EBF, 0x1EC3, 0x1EE3, 0x0110}) {
                const auto* glyph = ImGui::GetFontBaked()->FindGlyphNoFallback(codepoint);
                assert(glyph && glyph->Visible);
                assert(glyph->Y0 >= -ImGui::GetStyle().FramePadding.y);
                assert(glyph->Y1 <= ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y);
            }
            for (const auto& [font, logical] : {std::pair{caption.font, 15.6f},
                    std::pair{heading.font, 20.8f}, std::pair{title.font, 26.0f}}) {
                assert(std::abs(font->LegacySize - logical * dpi * zoom) < 0.001f);
                ImGui::PushFont(font);
                assert(ImGui::GetFontSize() == std::round(font->LegacySize));
                assert(ImGui::GetFontBaked()->FindGlyphNoFallback(0x1EE3));
                ImGui::PopFont();
            }
            bool checked = false;
            for (const auto* label : {"Monitor every computer automatically",
                    "Thu thập chẩn đoán đã lọc riêng tư trên máy"}) {
                const float left = ImGui::GetCursorScreenPos().x;
                nstu::ui::wrapped_checkbox(label, label, &checked);
                assert(ImGui::GetItemRectMax().x <= left + ImGui::GetContentRegionAvail().x + 1.0f);
            }
            const float available = ImGui::GetContentRegionAvail().x;
            if (nstu::ui::begin_workspace_combo("dev-workspace", "Cuộc thi", true,
                    "DEV · Không bảo vệ", "DEV: exams run without UWF", caption.font, dpi, preset)) ImGui::EndCombo();
            assert(ImGui::GetItemRectSize().y + 0.01f >=
                nstu::ui::layout::navigation_frame(dpi, preset, ImGui::GetFrameHeight(), true));
            assert(ImGui::CalcTextSize("Cuộc thi").x < available - ImGui::GetItemRectSize().y);
            ImGui::End(); ImGui::Render(); ImGui::DestroyContext();
        }
    }
    assert(std::abs(nstu::ui::reading::font_size(14.0f, 1.0f, 1) - 22.75f) < 0.001f);
    assert(nstu::ui::reading::zoom(-1) == 1.0f && nstu::ui::reading::zoom(4) == 1.0f);
}

void installed_bold_fallback_retains_vietnamese_coverage() {
    char windows[MAX_PATH]{}, temporary[MAX_PATH]{}, sentinel[MAX_PATH]{};
    assert(GetWindowsDirectoryA(windows, MAX_PATH));
    assert(GetTempPathA(MAX_PATH, temporary));
    assert(GetTempFileNameA(temporary, "nfr", 0, sentinel));
    const auto fixture = std::filesystem::path(std::string(sentinel) + ".fonts");
    assert(std::filesystem::create_directory(fixture));
    assert(std::filesystem::copy_file(std::filesystem::path(windows) / "Fonts" / "segoeuib.ttf",
                                     fixture / "segoeuib.ttf"));
    ImGui::CreateContext();
    auto& atlas = *ImGui::GetIO().Fonts;
    const auto directory = fixture.string() + "\\";
    for (const bool heading : {false, true}) {
        const auto loaded = nstu::ui::reading::load_font(atlas, directory, heading, 26.0f);
        assert(loaded.system_font && loaded.font);
        assert(atlas.Build());
        assert(std::string_view(loaded.font->GetDebugName()).find("segoeuib.ttf") != std::string_view::npos);
        assert(loaded.font->GetFontBaked(26.0f)->FindGlyphNoFallback(0x1EE3));
    }
    const auto exhausted = nstu::ui::reading::load_font(atlas, "", false, 22.75f);
    assert(!exhausted.system_font && exhausted.font && exhausted.font->LegacySize == 22.75f);
    ImGui::DestroyContext();
    assert(std::filesystem::remove(fixture / "segoeuib.ttf"));
    assert(std::filesystem::remove(fixture));
    assert(std::filesystem::remove(sentinel));
}

void compact_shell_preserves_content_at_largest_reading_size() {
    for (const float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) for (int preset = 0; preset < 4; ++preset) {
        const float frame = nstu::ui::reading::font_size(34.0f, dpi, preset);
        assert(nstu::ui::layout::compact(500.0f * dpi, 400.0f * dpi, dpi));
        assert(nstu::ui::layout::content_height(400.0f * dpi, dpi, preset, frame) >= 160.0f * dpi);
        assert(nstu::ui::layout::content_height(400.0f * dpi, dpi, preset, frame, true) >= 160.0f * dpi);
        assert(nstu::ui::layout::minimum_client_width * dpi == 500.0f * dpi);
        assert(nstu::ui::layout::minimum_client_height * dpi == 400.0f * dpi);
    }
    assert(!nstu::ui::layout::compact(1280, 820, 1));
    assert(nstu::ui::layout::compact(1280, 600, 1));
    assert(nstu::ui::layout::minimum_scale(1, 2) == 2);
    assert(nstu::ui::layout::minimum_scale(2, 1) == 2);
}

int main() {
    approved_native_metrics_replace_the_interim_values();
    every_native_role_projects_the_approved_palette();
    repeated_theme_swaps_preserve_absolute_geometry();
    monitor_scale_changes_do_not_reuse_scaled_metrics();
    theme_restores_defaults_and_applies_the_absolute_scale_once();
    normal_text_and_primary_actions_keep_readable_contrast();
    teacher_reading_sizes_fit_real_vietnamese_fonts();
    installed_bold_fallback_retains_vietnamese_coverage();
    compact_shell_preserves_content_at_largest_reading_size();
    std::cout << "interim presentation tests passed\n";
}
