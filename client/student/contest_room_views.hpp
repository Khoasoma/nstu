#pragma once

#include "nstu/contest_student_session.hpp"

#include <algorithm>
#include <charconv>

namespace nstu::client::student_ui {

enum class LobbyPhase { scanning, empty, available, stale, failed };

[[nodiscard]] inline bool host_needs_preservation(ExamHostState state) noexcept {
    return state != ExamHostState::idle;
}

[[nodiscard]] inline bool same_context(const exam::StateRequest& first, const exam::StateRequest& second) noexcept {
    return first.package_id == second.package_id && first.package_digest == second.package_digest &&
        first.client_id == second.client_id && first.session_id == second.session_id && first.candidate_id == second.candidate_id;
}

[[nodiscard]] inline const char* text(bool vi, const char* en, const char* vietnamese) noexcept {
    return vi ? vietnamese : en;
}

enum class NativeFailure { deployment, discovery, configuration, startup };
[[nodiscard]] inline const char* native_failure_label(NativeFailure failure, bool vi) noexcept {
    switch (failure) {
    case NativeFailure::deployment: return text(vi,
        "Configure trusted NSTU_CONTEST_POWERSHELL_PATH and NSTU_CONTEST_POWERSHELL_SHA256 before joining.",
        "Cấu hình NSTU_CONTEST_POWERSHELL_PATH và NSTU_CONTEST_POWERSHELL_SHA256 tin cậy trước khi tham gia.");
    case NativeFailure::discovery: return text(vi, "Room discovery unavailable", "Tìm phòng không khả dụng");
    case NativeFailure::configuration: return text(vi,
        "Use an absolute per-user --profile-root path (optional), and --language=en or --language=vi.",
        "Dùng đường dẫn tuyệt đối theo người dùng cho --profile-root (tùy chọn), và --language=en hoặc --language=vi.");
    case NativeFailure::startup: return text(vi, "NSTU student could not start. Existing profiles were preserved.",
        "Không thể khởi động ứng dụng học sinh NSTU. Hồ sơ hiện có được giữ lại.");
    }
    return "";
}
[[nodiscard]] inline const char* native_failure_title(NativeFailure failure, bool vi) noexcept {
    return failure == NativeFailure::configuration ? text(vi, "NSTU student configuration", "Cấu hình học sinh NSTU") : "NSTU";
}

[[nodiscard]] inline const char* lobby_label(LobbyPhase phase, bool vi) noexcept {
    switch (phase) {
    case LobbyPhase::scanning: return text(vi, "Scanning active LAN adapters...", "Đang tìm phòng trên mạng LAN...");
    case LobbyPhase::empty: return text(vi, "No rooms found. Refresh or probe a teacher IPv4:UDPport.", "Không tìm thấy phòng. Làm mới hoặc tìm theo IPv4:cổng UDP của giáo viên.");
    case LobbyPhase::available: return text(vi, "Select a room. Labels are unverified until both screens confirm the same code.", "Chọn một phòng. Tên chưa được xác minh cho đến khi hai màn hình xác nhận cùng mã.");
    case LobbyPhase::stale: return text(vi, "Room hint expired. Refresh before joining.", "Thông tin phòng đã hết hạn. Làm mới trước khi tham gia.");
    case LobbyPhase::failed: return text(vi, "Discovery failed. Check the network or probe a teacher IPv4:UDPport.", "Tìm phòng thất bại. Kiểm tra mạng hoặc tìm theo IPv4:cổng UDP của giáo viên.");
    }
    return "";
}

[[nodiscard]] inline const char* phase_label(StudentPhase phase, bool vi) noexcept {
    switch (phase) {
    case StudentPhase::idle: return text(vi, "Choose a room", "Chọn phòng");
    case StudentPhase::connecting: return text(vi, "Connecting to teacher", "Đang kết nối giáo viên");
    case StudentPhase::confirm_teacher: return text(vi, "Compare this code on the teacher screen before confirming", "So sánh mã này trên màn hình giáo viên trước khi xác nhận");
    case StudentPhase::awaiting_admission: return text(vi, "Waiting for teacher approval", "Đang chờ giáo viên chấp thuận");
    case StudentPhase::approved: return text(vi, "Approved; waiting for public package", "Đã chấp thuận; đang chờ gói đề công khai");
    case StudentPhase::receiving: return text(vi, "Receiving and verifying public package", "Đang nhận và xác minh gói đề công khai");
    case StudentPhase::prepared: return text(vi, "Verified package; waiting for teacher Begin", "Đã xác minh gói; đang chờ giáo viên bắt đầu");
    case StudentPhase::running: return text(vi, "Contest running", "Cuộc thi đang diễn ra");
    case StudentPhase::closed: return text(vi, "Room closed; source and receipt recovery remain available", "Phòng đã đóng; vẫn có thể khôi phục mã nguồn và biên nhận");
    case StudentPhase::disconnected: return text(vi, "Disconnected; keep the editor open while source is preserved", "Mất kết nối; giữ trình soạn thảo mở trong khi lưu mã nguồn");
    case StudentPhase::failed: return text(vi, "Session unavailable; existing source retained", "Phiên không khả dụng; mã nguồn hiện có được giữ lại");
    }
    return "";
}

[[nodiscard]] inline std::optional<contest_discovery::Endpoint> manual_endpoint(std::string_view input) {
    const auto colon = input.find(':');
    if (colon == std::string_view::npos || input.find(':', colon + 1) != std::string_view::npos) return {};
    const auto decimal = [](std::string_view field, unsigned maximum) -> std::optional<unsigned> {
        if (field.empty() || (field.size() > 1 && field.front() == '0')) return {};
        unsigned value = 0;
        const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
        if (result.ec != std::errc{} || result.ptr != field.data() + field.size() || value > maximum) return {};
        return value;
    };
    const auto address = input.substr(0, colon);
    std::size_t start = 0;
    for (unsigned octet = 0; octet < 4; ++octet) {
        const auto end = address.find('.', start);
        if ((octet < 3) != (end != std::string_view::npos)) return {};
        if (!decimal(address.substr(start, end == std::string_view::npos ? end : end - start), 255)) return {};
        start = end == std::string_view::npos ? address.size() : end + 1;
    }
    const auto port = decimal(input.substr(colon + 1), 65535);
    if (!port || !*port) return {};
    return contest_discovery::Endpoint{std::string(address), static_cast<std::uint16_t>(*port)};
}

[[nodiscard]] inline bool can_join(const std::optional<contest_discovery::RoomCandidate>& selected,
    bool session_active, std::chrono::steady_clock::time_point now) noexcept {
    return !session_active && selected && selected->room.join_state == contest_discovery::JoinState::open &&
        now >= selected->observed_at && now - selected->observed_at < contest_discovery::kHintLifetime;
}

[[nodiscard]] inline std::string room_label(const contest_discovery::RoomCandidate& candidate) {
    return candidate.room.teacher_label + " | " + candidate.room.title + " | " + candidate.address + ":" + std::to_string(candidate.room.control_port);
}

[[nodiscard]] inline bool can_connect(const std::optional<contest_discovery::RoomCandidate>& selected,
    bool session_active, std::chrono::steady_clock::time_point now, bool remembered = false) noexcept {
    return !session_active && selected && (remembered || (now >= selected->observed_at && now - selected->observed_at < contest_discovery::kHintLifetime));
}

[[nodiscard]] inline bool can_confirm(const ContestStudentSnapshot& snapshot) noexcept {
    return snapshot.phase == StudentPhase::confirm_teacher && snapshot.sas.size() == pairing::kSasDigits &&
        std::all_of(snapshot.sas.begin(), snapshot.sas.end(), [](char c) { return c >= '0' && c <= '9'; });
}
[[nodiscard]] inline bool can_show_standings(const ContestStudentSnapshot& snapshot) noexcept {
    return snapshot.context && snapshot.live_standings;
}
[[nodiscard]] inline bool can_refresh_standings(const ContestStudentSnapshot& snapshot) noexcept {
    return snapshot.connected && can_show_standings(snapshot);
}
[[nodiscard]] inline const char* standing_label(std::string_view freshness, bool vi) noexcept {
    if (freshness == "current") return text(vi, "Current published standings", "Bảng xếp hạng công khai hiện tại");
    if (freshness == "pending") return text(vi, "Provisional standings; judging pending", "Bảng xếp hạng tạm thời; đang chờ chấm");
    if (freshness == "stale") return text(vi, "Provisional standings; snapshot is stale", "Bảng xếp hạng tạm thời; dữ liệu đã cũ");
    return text(vi, "Provisional standings; judge unavailable", "Bảng xếp hạng tạm thời; máy chấm không khả dụng");
}
[[nodiscard]] inline std::string_view standing_freshness(const ContestStudentSnapshot& snapshot) noexcept {
    if (!snapshot.connected || !snapshot.standings) return "unavailable";
    if (snapshot.server_time >= snapshot.standings->generated_time && snapshot.server_time - snapshot.standings->generated_time >= 10000) return "stale";
    return snapshot.standings->freshness;
}
[[nodiscard]] inline bool accept_standings(const std::optional<StudentStandings>& previous,
    const StudentStandings& next) noexcept {
    return next.rows.size() <= 64 && (!previous || (next.revision >= previous->revision && next.generated_time >= previous->generated_time));
}

[[nodiscard]] inline std::optional<ExamHostContestLifecycle> host_lifecycle(const ContestStudentSnapshot& snapshot) {
    const auto& life = snapshot.lifecycle;
    if (!snapshot.context || !life.revision || !snapshot.server_time) return {};
    ExamHostContestPhase phase = ExamHostContestPhase::draft;
    switch (life.phase) {
    case StudentRoomPhase::draft: phase = ExamHostContestPhase::draft; break;
    case StudentRoomPhase::joining: phase = ExamHostContestPhase::joining; break;
    case StudentRoomPhase::prepared: phase = ExamHostContestPhase::prepared; break;
    case StudentRoomPhase::running: phase = ExamHostContestPhase::running; break;
    case StudentRoomPhase::closed: phase = ExamHostContestPhase::closed; break;
    }
    return ExamHostContestLifecycle{*snapshot.context, phase, life.revision, life.server_start, life.deadline, snapshot.server_time};
}

} // namespace nstu::client::student_ui
