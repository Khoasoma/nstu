#include "../client/student/contest_room_views.hpp"

#include <cassert>
#include <iostream>

using namespace nstu::client;
using namespace nstu::client::student_ui;

int main() {
    for (const auto failure : {NativeFailure::deployment, NativeFailure::discovery,
                              NativeFailure::configuration, NativeFailure::startup}) {
        const std::string english(native_failure_label(failure, false));
        const std::string vietnamese(native_failure_label(failure, true));
        assert(!english.empty() && !vietnamese.empty() && english != vietnamese);
    }
    assert(std::string(native_failure_label(NativeFailure::deployment, false)).find("NSTU_CONTEST_POWERSHELL_SHA256") != std::string::npos);
    assert(std::string(native_failure_label(NativeFailure::deployment, true)).find("NSTU_CONTEST_POWERSHELL_SHA256") != std::string::npos);
    assert(std::string(native_failure_label(NativeFailure::discovery, true)).find("không khả dụng") != std::string::npos);
    assert(std::string(native_failure_label(NativeFailure::configuration, false)).find("--profile-root") != std::string::npos);
    assert(std::string(native_failure_label(NativeFailure::configuration, true)).find("--profile-root") != std::string::npos);
    assert(std::string(native_failure_label(NativeFailure::startup, false)).find("preserved") != std::string::npos);
    assert(std::string(native_failure_label(NativeFailure::startup, true)).find("được giữ lại") != std::string::npos);
    assert(std::string(native_failure_title(NativeFailure::configuration, false)) == "NSTU student configuration");
    assert(std::string(native_failure_title(NativeFailure::configuration, true)) == "Cấu hình học sinh NSTU");
    assert(!host_needs_preservation(ExamHostState::idle));
    for (const auto host_state : {ExamHostState::preparing, ExamHostState::initializing,
                                 ExamHostState::running, ExamHostState::degraded, ExamHostState::failed})
        assert(host_needs_preservation(host_state));
    const auto now = std::chrono::steady_clock::now();
    nstu::contest_discovery::RoomCandidate room;
    room.address = "127.0.0.1";
    room.room.room_id = std::string(32, '1');
    room.room.round_id = std::string(32, '2');
    room.room.teacher_id = "teacher-one";
    room.room.title = "%s##<script>";
    room.room.teacher_label = "Giáo viên";
    room.observed_at = now;
    assert(!can_join({}, false, now)); // Discovery never chooses a room.
    assert(can_join(room, false, now));
    assert(!can_join(room, true, now));
    assert(!can_join(room, false, now + std::chrono::seconds(10)));
    room.room.join_state = nstu::contest_discovery::JoinState::join_closed;
    assert(!can_join(room, false, now));
    assert(can_connect(room, false, now));
    assert(!can_connect({}, false, now));
    assert(!can_connect(room, true, now));
    assert(!can_connect(room, false, now + std::chrono::seconds(10)));
    assert(can_connect(room, false, now + std::chrono::hours(1), true));
    assert(!can_connect(room, true, now + std::chrono::hours(1), true));
    room.room.join_state = nstu::contest_discovery::JoinState::open;
    assert(room_label(room).find("%s##<script>") != std::string::npos);
    assert(std::string(lobby_label(LobbyPhase::empty, false)).find("No rooms") != std::string::npos);
    assert(std::string(lobby_label(LobbyPhase::stale, true)).find("hết hạn") != std::string::npos);
    assert(lobby_label(LobbyPhase::scanning, false) != lobby_label(LobbyPhase::failed, false));

    auto endpoint = manual_endpoint("127.0.0.1:47002");
    assert(endpoint && endpoint->address == "127.0.0.1" && endpoint->port == 47002);
    for (const auto invalid : {"localhost:47002", "127.0.0.1", "127.0.0.1:0", "127.0.0.1:65536",
                               "256.1.1.1:47002", "1.2.3:47002", "1.2.3.4:2x", "01.2.3.4:4",
                               "1.2.3.4:04", " 1.2.3.4:4", "1.2.3.4:4 ", "1.2.3.4:4:5"})
        assert(!manual_endpoint(invalid));

    ContestStudentSnapshot state;
    assert(!can_confirm(state));
    state.phase = StudentPhase::confirm_teacher; state.sas = "123456";
    assert(can_confirm(state));
    state.sas = "12345x"; assert(!can_confirm(state));
    state.sas = "123456"; state.phase = StudentPhase::disconnected;
    assert(!can_confirm(state));
    state.connected = true;
    state.context = nstu::exam::StateRequest{"cs", {}, {}, {}, "candidate"};
    assert(!can_show_standings(state));
    state.live_standings = true;
    assert(can_show_standings(state));
    state.connected = false;
    assert(can_show_standings(state)); // Last known native rows remain provisional.
    assert(!can_refresh_standings(state));
    state.connected = true;
    assert(can_refresh_standings(state));
    assert(std::string(standing_label("unavailable", true)).find("không") != std::string::npos);
    assert(standing_freshness(state) == "unavailable");
    state.standings = StudentStandings{4, 1000, "current", {}};
    state.server_time = 10999; assert(standing_freshness(state) == "current");
    state.server_time = 11000; assert(standing_freshness(state) == "stale");
    state.connected = false; assert(standing_freshness(state) == "unavailable");
    state.connected = true;

    state.context->package_digest[0] = std::byte{1};
    state.context->client_id[0] = std::byte{1};
    state.context->session_id[0] = std::byte{1};
    auto other_context = *state.context;
    assert(same_context(other_context, *state.context));
    other_context.candidate_id = "different";
    assert(!same_context(other_context, *state.context));
    state.lifecycle = {StudentRoomPhase::running, 4, 10000, 10000, 70000};
    state.server_time = 11000;
    auto life = host_lifecycle(state);
    assert(life && life->phase == ExamHostContestPhase::running && life->server_start == 10000);
    assert(life->deadline == 70000 && life->revision == 4 && life->server_time == 11000);
    assert(life->context.candidate_id == "candidate");
    state.lifecycle.phase = StudentRoomPhase::closed;
    life = host_lifecycle(state); assert(life && life->phase == ExamHostContestPhase::closed);
    state.lifecycle.revision = 0; assert(!host_lifecycle(state));
    state.lifecycle.revision = 4; state.server_time = 0; assert(!host_lifecycle(state));
    state.server_time = 11000; state.context.reset(); assert(!host_lifecycle(state));

    const nstu::contest_room::Realm realm{1, "teacher-one", std::string(32, '1'), std::string(32, '2')};
    const std::string json = "{\"roomId\":\"" + realm.room_id + "\",\"roundId\":\"" + realm.round_id +
        "\",\"revision\":4,\"generatedTime\":10000,\"freshness\":\"pending\",\"rows\":[{\"rank\":1,\"displayName\":\"%n##Học sinh\",\"solved\":1,\"penaltyMinutes\":2}]}";
    auto scores = nstu::client::parse_student_standings(json, realm);
    assert(scores && scores->rows[0].display_name == "%n##Học sinh");
    assert(accept_standings({}, *scores));
    auto previous = *scores;
    scores->revision = 3; assert(!accept_standings(previous, *scores));
    scores->revision = 4; scores->generated_time = 9999; assert(!accept_standings(previous, *scores));
    scores->generated_time = 10000; scores->rows.resize(65); assert(!accept_standings(previous, *scores));
    auto other = realm; other.round_id = std::string(32, '3');
    assert(!nstu::client::parse_student_standings(json, other));
    std::cout << "Native lobby selection, manual endpoint, SAS, sealed standings and trusted lifecycle: PASS\n";
}
