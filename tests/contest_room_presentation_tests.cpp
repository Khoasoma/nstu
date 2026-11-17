#include "../server/app/contest_room_panel.hpp"

#include <cassert>
#include <iostream>

using namespace nstu::ui;
using namespace nstu::server;

int main() {
    assert(!contest_room_port(-1));
    assert(!contest_room_port(0));
    assert(!contest_room_port(65536));
    assert(!contest_room_port(std::numeric_limits<int>::max()));
    assert(contest_room_port(nstu::contest_discovery::kDefaultPort) == 47002);
    assert(contest_room_port(1) == 1);
    assert(contest_room_port(65535) == 65535);
    ContestRoomSnapshot snapshot;
    JudgeReadiness ready;
    const auto now = std::chrono::steady_clock::now();
    assert(!contest_can_begin(snapshot, ready, now));
    snapshot.running = true;
    snapshot.room = RoomState{};
    auto& room = *snapshot.room;
    room.immutable.room_id = std::string(32, '1');
    room.immutable.round_id = std::string(32, '2');
    room.phase = RoomPhase::prepared;
    room.revision = 3;
    room.members.push_back({{}, "%s##unverified", false, 1});
    assert(!contest_can_begin(snapshot, ready, now));
    ready = {room.immutable.room_id, room.immutable.round_id, room.revision, true, now};
    assert(!contest_can_begin(snapshot, ready, now));
    room.members[0].prepared = true;
    assert(contest_can_begin(snapshot, ready, now));
    ready.available = false;
    assert(!contest_can_begin(snapshot, ready, now));
    ready.available = true;
    assert(!contest_can_begin(snapshot, ready, now + std::chrono::seconds(6)));
    ready.revision = 2;
    assert(!contest_can_begin(snapshot, ready, now));
    ready.revision = room.revision;
    ready.round_id = std::string(32, '3');
    assert(!contest_can_begin(snapshot, ready, now));
    ready.round_id = room.immutable.round_id;
    room.phase = RoomPhase::joining;
    room.members[0].prepared = false;
    assert(!contest_can_begin(snapshot, ready, now));
    room.members[0].prepared = true;
    assert(contest_can_begin(snapshot, ready, now)); // Final genuine ACK can arrive during a judge outage.
    ready.available = false;
    assert(!contest_can_begin(snapshot, ready, now));
    ready.available = true;
    assert(!contest_can_begin(snapshot, ready, now + std::chrono::seconds(6)));
    room.members.push_back({{}, "Still transferring", false, 1});
    assert(!contest_can_begin(snapshot, ready, now));
    room.members.pop_back();
    room.phase = RoomPhase::draft;
    assert(!contest_can_begin(snapshot, ready, now));
    room.phase = RoomPhase::running;
    assert(!contest_can_begin(snapshot, ready, now));
    assert(std::string(contest_phase_label(room.phase, false)) == "Running");
    assert(std::string(contest_phase_label(RoomPhase::prepared, false)) == "Prepared");
    assert(std::string(contest_phase_label(RoomPhase::prepared, true)) == "Sẵn sàng");
    assert(std::string(contest_member_label(false, false)) == "Awaiting verified package ACK");
    assert(std::string(contest_member_label(true, true)) == "Đã xác nhận gói");
    assert(contest_can_manage(OperatorRole::teacher));
    assert(!contest_can_manage(OperatorRole::technician));
    assert(room.members[0].display_name == "%s##unverified");

    StandingsSnapshot scores;
    scores.room_id = room.immutable.room_id;
    scores.round_id = room.immutable.round_id;
    scores.revision = 4;
    scores.generated_time = 100;
    scores.freshness = StandingsFreshness::current;
    scores.rows = {{1, "%n##one", 1, 5}, {1, "two", 1, 5}, {3, "three", 0, 0}};
    assert(contest_accept_standings({}, scores, room.immutable));
    assert(scores.rows[0].rank == scores.rows[1].rank);
    auto previous = scores;
    scores.revision = 3;
    assert(!contest_accept_standings(previous, scores, room.immutable));
    scores = previous; scores.generated_time = 99;
    assert(!contest_accept_standings(previous, scores, room.immutable));
    scores = previous; scores.round_id = std::string(32, '4');
    assert(!contest_accept_standings({}, scores, room.immutable));
    scores = previous; scores.rows.resize(65);
    assert(!contest_accept_standings({}, scores, room.immutable));

    nstu::security::Sha256Digest pin{}; pin[0] = std::byte{1};
    const std::string content = "01" + std::string(62, '0');
    const std::string grading = "02" + std::string(62, '0');
    const std::string problems = R"([{"id":"sum","revision":1,"cases":[{"input":"private","output":"1"}]}])";
    const std::string bundle = "{\"programmingVersion\":1,\"packageId\":\"cs\",\"packageDigestHex\":\"" + content +
        "\",\"gradingDigestHex\":\"" + grading + "\",\"problems\":" + problems + "}";
    auto parsed = contest_private_bundle(bundle, "cs", pin);
    assert(parsed && parsed->problems_json == problems && parsed->grading_digest[0] == std::byte{2});
    assert(!contest_private_bundle(bundle, "other", pin));
    pin[0] = std::byte{3}; assert(!contest_private_bundle(bundle, "cs", pin)); pin[0] = std::byte{1};
    assert(!contest_private_bundle(bundle + "x", "cs", pin));
    assert(!contest_private_bundle("{\"operatorToken\":\"secret\"," + bundle.substr(1), "cs", pin));
    assert(!contest_private_bundle("{\"programmingVersion\":1," + bundle.substr(1), "cs", pin));
    assert(!contest_private_bundle(bundle.substr(0, bundle.size() - 1), "cs", pin));
    assert(!contest_private_bundle(std::string(4u * 1024u * 1024u + 1u, ' '), "cs", pin));
    const auto replace = [](std::string value, std::string_view from, std::string_view to) {
        const auto offset = value.find(from); assert(offset != std::string::npos); value.replace(offset, from.size(), to); return value;
    };
    assert(!contest_private_bundle(replace(bundle, "\"programmingVersion\":1", "\"programmingVersion\":10"), "cs", pin));
    assert(!contest_private_bundle(replace(bundle, grading, std::string(64, '0')), "cs", pin));
    assert(!contest_private_bundle(replace(bundle, "\"packageId\":\"cs\"", "\"packageId\":\"cs\",\"packageId\":\"cs\""), "cs", pin));
    assert(!contest_private_bundle(replace(bundle, "\"packageId\"", "\"package\\u0049d\""), "cs", pin));
    assert(contest_private_bundle(replace(bundle, "private", R"(quotes\" and brackets ]})"), "cs", pin));

    // Synthetic presentation state; none of these values qualify a real judge or room.
    assert(contest_grouped_sas("012345") == "012 345");
    assert(contest_grouped_sas("12345").empty());
    assert(contest_grouped_sas("123x56").empty());
    assert(!contest_sas_matches("012 345", "012345"));
    assert(!contest_sas_matches("", "012345"));
    assert(contest_sas_matches("012345", "012345"));
    assert(contest_utf8_path(L"C:\\Hồ sơ\\gói.nstu") == "C:\\Hồ sơ\\gói.nstu");
    assert(contest_utf8_path(L"").empty());
    assert(contest_utf8_path(std::wstring(32768, L'x')).empty());
    assert(contest_utf8_path(std::wstring(1, static_cast<wchar_t>(0xd800))).empty());
    PendingRoomPairing candidate{0x100000001ULL, "synthetic-peer-a", "Nguyễn Thị Bích Phượng %s##unverified Long synthetic contestant display name", "012345"};
    snapshot.pending = {candidate};
    ContestSasComparison comparison;
    comparison.start(candidate);
    assert(comparison.input[0] == '\0' && comparison.focus_input);
    assert(comparison.current(snapshot) != nullptr);
    snapshot.pending[0].peer_user_uuid = "synthetic-peer-b";
    assert(comparison.current(snapshot) == nullptr);
    snapshot.pending[0] = candidate;
    snapshot.pending[0].sas = "543210";
    assert(comparison.current(snapshot) == nullptr);
    snapshot.pending[0] = candidate;
    snapshot.pending.insert(snapshot.pending.begin(), {1, "other-peer", "Other", "123456"});
    assert(comparison.current(snapshot)->peer_id == candidate.peer_id);
    assert(contest_peer_ui_id(candidate) != contest_peer_ui_id(snapshot.pending[0]));
    comparison.cancel();
    assert(comparison.current(snapshot) == nullptr && comparison.input[0] == '\0');
    assert(comparison.return_focus == contest_peer_ui_id(candidate));

    RoomMember target = room.members[0];
    target.context.client_id[0] = std::byte{1};
    target.display_name = candidate.display_name;
    target.member_version = 7;
    room.members = {target};
    const auto target_id = contest_member_ui_id(target.context);
    auto other = target; other.context.client_id[0] = std::byte{2};
    assert(target_id != contest_member_ui_id(other.context));
    room.members.insert(room.members.begin(), other);
    assert(target_id == contest_member_ui_id(room.members[1].context));
    assert(contest_removal_target(snapshot, target, OperatorRole::teacher));
    assert(!contest_removal_target(snapshot, target, OperatorRole::technician));
    room.members[1].member_version = 8;
    assert(!contest_removal_target(snapshot, target, OperatorRole::teacher));
    room.members[1] = target;
    room.phase = RoomPhase::closed;
    assert(!contest_removal_target(snapshot, target, OperatorRole::teacher));
    assert(!contest_finish_available(snapshot, room.immutable, OperatorRole::teacher));
    room.phase = RoomPhase::running;
    assert(contest_finish_available(snapshot, room.immutable, OperatorRole::teacher));
    auto replaced_round = room.immutable; replaced_round.round_id = "replacement";
    assert(!contest_finish_available(snapshot, replaced_round, OperatorRole::teacher));
    snapshot.running = false;
    assert(!contest_finish_available(snapshot, room.immutable, OperatorRole::teacher));
    snapshot.running = true;

    std::optional<StandingsSnapshot> synthetic_scores = previous;
    assert(std::string(contest_standings_label(synthetic_scores, now, now, false)) == "[+] Current aggregate standings");
    synthetic_scores->freshness = StandingsFreshness::pending;
    assert(std::string(contest_standings_label(synthetic_scores, now, now, false)) == "[!] Judging pending");
    synthetic_scores->freshness = StandingsFreshness::unavailable;
    assert(std::string(contest_standings_label(synthetic_scores, now, now, false)) == "[!] Judge unavailable");
    assert(std::string(contest_standings_label(synthetic_scores, now, now + std::chrono::seconds(6), false)) == "[!] Stale standings; retained results are provisional");
    assert(std::string(contest_standings_label({}, {}, now, false)) == "[ ] Standings unavailable");
    assert(std::string(contest_standings_label(synthetic_scores, now, now - std::chrono::seconds(1), false)) == "[!] Stale standings; retained results are provisional");
    assert(synthetic_scores->rows[0].rank == synthetic_scores->rows[1].rank);
    assert(std::string(contest_notice_label(ContestNotice::selection_failed, false)).find("retained") != std::string::npos);

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = {800, 600};
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    assert(ImGui::GetIO().Fonts->Build());
    {
        ContestRoomPanel panel;
        for (const bool vi : {false, true}) for (const bool dark : {false, true})
            for (const float scale : {1.0f, 1.25f, 1.5f}) for (const auto role : {OperatorRole::teacher, OperatorRole::technician}) {
                apply_interim_style(ImGui::GetStyle(), dark, scale);
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({800, 600});
                ImGui::Begin("Teacher fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
                panel.draw(role, vi, dark, scale);
                ImGui::End(); ImGui::Render();
                assert(ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0);
            }
    }

    // Actual Segoe UI metrics and real ImGui bounds, across every D/Z/language/theme combination.
    for (const float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) for (int reading_preset = 0; reading_preset != 4; ++reading_preset) {
        auto& io = ImGui::GetIO();
        io.Fonts->Clear();
        const auto body = reading::load_font(*io.Fonts, "C:/Windows/Fonts/", false,
            reading::font_size(typography::body, dpi, reading_preset));
        const auto heading = reading::load_font(*io.Fonts, "C:/Windows/Fonts/", true,
            reading::font_size(typography::page_title, dpi, reading_preset));
        const auto sas = reading::load_font(*io.Fonts, "C:/Windows/Fonts/", true,
            reading::sas_size(dpi, reading_preset));
        assert(body.font && heading.font && sas.font);
        assert(body.system_font && heading.system_font && sas.system_font);
        io.FontDefault = body.font;
        assert(io.Fonts->Build());
        io.DisplaySize = {500 * dpi, 400 * dpi};
        for (const bool vi : {false, true}) for (const bool dark : {false, true}) {
            apply_teacher_style(ImGui::GetStyle(), dark, dpi, reading_preset);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Compact contest metrics", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
            // The shell owns the external workspace selector in compact mode.
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##workspace", "Contests: Setup")) ImGui::EndCombo();
            ContestRoomPanel panel;
            assert(std::string(panel.active_tab_label(vi)) == (vi ? "Thiết lập" : "Setup"));
            panel.draw(OperatorRole::teacher, vi, dark, dpi, heading.font, sas.font, true);
            ImGuiWindow* content_window = nullptr;
            for (auto* window : ImGui::GetCurrentContext()->Windows)
                if (window->ParentWindow == ImGui::GetCurrentWindow() &&
                    std::string_view(window->Name).find("contest-content") != std::string_view::npos) content_window = window;
            assert(content_window && content_window->InnerRect.GetHeight() >= layout::minimum_content_height * dpi);
            ImGui::End(); ImGui::Render();

            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Synthetic populated contest rows", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextWrapped("%s", vi ? "Dữ liệu giao diện tổng hợp; không phải phòng thật." : "Synthetic UI data; no real room.");
            auto admission_snapshot = snapshot;
            admission_snapshot.room->phase = RoomPhase::joining;
            ContestSasComparison synthetic_comparison;
            synthetic_comparison.start(candidate);
            const auto admission = draw_contest_admission(admission_snapshot, OperatorRole::teacher, vi, sas.font, synthetic_comparison, dark);
            assert(!admission && synthetic_comparison.input[0] == '\0');
            assert(synthetic_comparison.current(admission_snapshot));
            ImGui::PushFont(sas.font);
            const auto code_size = ImGui::CalcTextSize(contest_grouped_sas(candidate.sas).c_str());
            assert(code_size.y >= reading::sas_size(dpi, reading_preset) - 1.0f);
            assert(code_size.x <= ImGui::GetContentRegionAvail().x);
            ImGui::PopFont();
            const auto row_start = ImGui::GetCursorPosY();
            const auto removal = draw_contest_participants(snapshot, OperatorRole::technician, vi, dpi, true);
            assert(!removal);
            assert(ImGui::GetCursorPosY() - row_start > ImGui::GetFrameHeight() * 2.0f);
            draw_contest_standings(synthetic_scores, now, now + std::chrono::seconds(6), vi, dpi, true);
            const auto label = std::string(vi ? "Tên hiển thị (chưa xác minh): " : "Display name (unverified): ") + target.display_name;
            const auto measured = ImGui::CalcTextSize(label.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x);
            assert(measured.y > ImGui::GetFontSize());
            assert(synthetic_scores->rows[0].rank == synthetic_scores->rows[1].rank);
            ImGui::End(); ImGui::Render();

            for (const float width : {700.0f, 1500.0f}) {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({width * dpi, 400.0f * dpi});
                ImGui::Begin("Standings header bounds", nullptr, ImGuiWindowFlags_NoSavedSettings);
                const auto member_table_id = ImGui::GetID("contest-members");
                assert(!draw_contest_participants(snapshot, OperatorRole::technician, vi, dpi, false));
                const auto table_id = ImGui::GetID("contest-standings");
                draw_contest_standings(synthetic_scores, now, now, vi, dpi, false);
                for (const auto id : {member_table_id, table_id}) {
                    const auto* table = ImGui::GetCurrentContext()->Tables.GetByKey(id);
                    if (reading_preset == 0 && width == 1500.0f) assert(table && table->LastFrameActive == ImGui::GetFrameCount());
                    if (!table || table->LastFrameActive != ImGui::GetFrameCount()) continue;
                    for (int column = 0; column != table->ColumnsCount; ++column) {
                        const auto& cell = table->Columns[column];
                        if (cell.WorkMaxX - cell.WorkMinX + 0.01f < ImGui::CalcTextSize(ImGui::TableGetColumnName(table, column)).x)
                            std::cerr << "Header width dpi=" << dpi << " reading=" << reading_preset << " vi=" << vi
                                << " width=" << width << " column=" << column << " available=" << cell.WorkMaxX - cell.WorkMinX
                                << " text=" << ImGui::CalcTextSize(ImGui::TableGetColumnName(table, column)).x << '\n';
                        assert(cell.WorkMaxX - cell.WorkMinX + 0.01f >=
                            ImGui::CalcTextSize(ImGui::TableGetColumnName(table, column)).x);
                    }
                }
                ImGui::End(); ImGui::Render();
            }
        }
    }
    ImGui::DestroyContext();
    std::cout << "Teacher room readiness, aggregate standings, EN/VI and private import boundaries: PASS\n";
}
