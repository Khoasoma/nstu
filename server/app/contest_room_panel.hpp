#pragma once

#include "nstu/contest_room_server.hpp"
#include "nstu/operator_role.hpp"
#include "exam_digest.hpp"
#include "interim_presentation.hpp"
#include "ui_accessibility_provider.hpp"
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#ifdef NSTU_SERVER_HAS_EXAM_INSPECTOR
#include "nstu/exam_host.hpp"
#endif

#include <array>
#include <algorithm>
#include <fstream>
#include <future>
#include <limits>
#include <cstring>
#include <charconv>

namespace nstu::ui {

struct ContestSemantics {
    using Key = accessibility::Key;
    Key parent = 0, scroll_owner = 0;
    std::string prefix = "contest";
    std::uint64_t generation = 0;
    std::optional<server::StandingsSnapshot> standings_identity;
    std::uint64_t standings_generation = 0;
    struct PeerLifetime { std::uint64_t id = 0; std::string uuid; std::array<char, 6> protected_sas{}; std::uint64_t generation = 0; };
    std::vector<PeerLifetime> peers;
    std::uint64_t next_peer_generation = 1;
    std::function<Key(std::string_view)> key;
    std::function<accessibility::Node&(Key, Key, const char*, accessibility::Kind,
        ImGuiWindow*, const ImRect&, ImGuiID)> capture;
    std::function<std::optional<accessibility::Command>(accessibility::Node&, accessibility::Action)> request;
    std::function<void(accessibility::Node&)> focus;
    std::function<void(accessibility::Node&, char*, std::size_t, accessibility::TextInput, bool)> edit;
    std::function<void()> suspend;
    std::function<void(Key, Key)> select;
    std::function<void(Key)> invoked;

    void prune_peers(std::span<const server::PendingRoomPairing> pending) {
        for (std::size_t index = 0; index < peers.size();) {
            if (std::any_of(pending.begin(), pending.end(), [&](const auto& peer) { return peer.peer_id == peers[index].id && peer.peer_user_uuid == peers[index].uuid; })) { ++index; continue; }
            peers[index].protected_sas.fill(0);
            if (index + 1 != peers.size()) peers[index] = std::move(peers.back());
            peers.back().protected_sas.fill(0); peers.pop_back();
        }
    }
    std::uint64_t peer_generation(const server::PendingRoomPairing& peer) {
        auto found = std::find_if(peers.begin(), peers.end(), [&](const auto& value) { return value.id == peer.peer_id && value.uuid == peer.peer_user_uuid; });
        if (found == peers.end()) {
            IM_ASSERT(peers.size() < server::kMaximumRoomMembers);
            if (peers.size() >= server::kMaximumRoomMembers) throw std::length_error("UIA peer snapshot exceeds protocol limit");
            peers.push_back({peer.peer_id, peer.peer_user_uuid, {}, 0}); found = std::prev(peers.end());
        }
        if (!found->generation || !std::equal(found->protected_sas.begin(), found->protected_sas.end(), peer.sas.begin(), peer.sas.end())) {
            if (next_peer_generation == std::numeric_limits<std::uint64_t>::max()) throw std::bad_alloc();
            found->generation = next_peer_generation++;
            found->protected_sas.fill(0);
            if (peer.sas.size() == found->protected_sas.size()) std::copy(peer.sas.begin(), peer.sas.end(), found->protected_sas.begin());
        }
        return found->generation;
    }

    accessibility::Node& item(std::string_view id, const char* name, accessibility::Kind kind, ImGuiID native_id = 0) {
        const auto& submitted = ImGui::GetCurrentContext()->LastItemData;
        std::string label = name;
        if (kind != accessibility::Kind::text && kind != accessibility::Kind::status) if (const auto suffix = label.find("##"); suffix != std::string::npos) label.resize(suffix);
        auto& node = capture(key(prefix + "/" + std::string(id)), parent, label.c_str(), kind,
            ImGui::GetCurrentWindow(), submitted.Rect, native_id ? native_id : submitted.ID);
        node.scroll_owner = scroll_owner; node.generation = generation;
        return node;
    }
    accessibility::Node& pane(std::string_view id, const char* name, accessibility::Kind kind, const ImRect& bounds) {
        auto& node = capture(key(prefix + "/" + std::string(id)), parent, name, kind,
            ImGui::GetCurrentWindow(), bounds, 0);
        node.scroll_owner = scroll_owner; node.generation = generation;
        if (kind == accessibility::Kind::row) focus(node);
        return node;
    }
    void text(std::string_view id, const char* name, bool live = false, bool status = false) {
        auto& node = item(id, name, live || status ? accessibility::Kind::status : accessibility::Kind::text);
        node.live = live;
        focus(node);
    }
    void headers(const std::array<const char*, 4>& labels) {
        auto* table = ImGui::GetCurrentContext()->CurrentTable;
        if (!table) return;
        for (int column = 0; column != 4; ++column) {
            const auto& cell = table->Columns[column];
            auto& node = capture(key(prefix + "/header-" + std::to_string(column)), parent, labels[static_cast<std::size_t>(column)], accessibility::Kind::text,
                ImGui::GetCurrentWindow(), ImRect({cell.MinX, table->RowPosY1}, {cell.MaxX, table->RowPosY2}), 0);
            node.scroll_owner = scroll_owner; node.generation = generation;
            focus(node);
        }
    }
};

struct ContestSemanticScope {
    ContestSemantics* semantics;
    ContestSemantics::Key parent = 0, scroll_owner = 0;
    std::string prefix;
    std::uint64_t generation = 0;
    explicit ContestSemanticScope(ContestSemantics* value) : semantics(value) {
        if (value) { parent = value->parent; scroll_owner = value->scroll_owner; prefix = value->prefix; generation = value->generation; }
    }
    ~ContestSemanticScope() {
        if (semantics) { semantics->parent = parent; semantics->scroll_owner = scroll_owner;
            semantics->prefix = std::move(prefix); semantics->generation = generation; }
    }
};

using ContestClock = std::chrono::steady_clock;
inline constexpr auto kContestRefreshInterval = std::chrono::seconds(5);

[[nodiscard]] inline std::optional<std::uint16_t> contest_room_port(int port) noexcept {
    if (port < 1 || port > std::numeric_limits<std::uint16_t>::max()) return std::nullopt;
    return static_cast<std::uint16_t>(port);
}

struct JudgeReadiness {
    std::string room_id, round_id;
    std::uint64_t revision = 0;
    bool available = false;
    ContestClock::time_point checked{};
};

[[nodiscard]] inline bool contest_can_manage(server::OperatorRole role) noexcept {
    return role == server::OperatorRole::teacher || role == server::OperatorRole::developer;
}
[[nodiscard]] inline bool contest_can_begin(const server::ContestRoomSnapshot& snapshot,
    const JudgeReadiness& judge, ContestClock::time_point now) noexcept {
    if (!snapshot.running || !snapshot.room || !judge.available ||
        now < judge.checked || now - judge.checked > kContestRefreshInterval) return false;
    const auto& room = *snapshot.room;
    return (room.phase == server::RoomPhase::prepared || room.phase == server::RoomPhase::joining) && !room.members.empty() &&
        room.immutable.room_id == judge.room_id && room.immutable.round_id == judge.round_id &&
        room.revision == judge.revision && std::all_of(room.members.begin(), room.members.end(),
            [](const auto& member) { return member.prepared; });
}
[[nodiscard]] inline const char* contest_phase_label(server::RoomPhase phase, bool vi) noexcept {
    switch (phase) {
    case server::RoomPhase::draft: return vi ? "Bản nháp" : "Draft";
    case server::RoomPhase::joining: return vi ? "Đang nhận thí sinh" : "Joining";
    case server::RoomPhase::prepared: return vi ? "Sẵn sàng" : "Prepared";
    case server::RoomPhase::running: return vi ? "Đang diễn ra" : "Running";
    case server::RoomPhase::closed: return vi ? "Đã kết thúc" : "Closed";
    }
    return vi ? "Không khả dụng" : "Unavailable";
}
[[nodiscard]] inline const char* contest_member_label(bool prepared, bool vi) noexcept {
    return prepared ? (vi ? "Đã xác nhận gói" : "Package acknowledged")
        : (vi ? "Chờ xác nhận gói đã kiểm tra" : "Awaiting verified package ACK");
}
[[nodiscard]] inline bool contest_accept_standings(
    const std::optional<server::StandingsSnapshot>& previous, const server::StandingsSnapshot& next,
    const server::ImmutableRoomState& room) noexcept {
    return next.room_id == room.room_id && next.round_id == room.round_id &&
        next.rows.size() <= server::kMaximumRoomMembers &&
        (!previous || (previous->room_id == next.room_id && previous->round_id == next.round_id &&
            next.revision >= previous->revision && next.generated_time >= previous->generated_time));
}

struct ContestPrivateBundle {
    security::Sha256Digest grading_digest{};
    std::string problems_json;
};

// Only the fixed Studio wrapper is consumed here; the existing broker validates
// every problem/case and the canonical wrapper hash before accepting material.
[[nodiscard]] inline std::optional<ContestPrivateBundle> contest_private_bundle(
    std::string_view json, std::string_view package_id, const security::Sha256Digest& package_digest) {
    if (json.size() > 4u * 1024u * 1024u) return std::nullopt;
    std::size_t pos = 0;
    const auto space = [&] { while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) ++pos; };
    const auto take = [&](char ch) { space(); if (pos == json.size() || json[pos] != ch) return false; ++pos; return true; };
    const auto text = [&]() -> std::optional<std::string_view> {
        if (!take('"')) return std::nullopt;
        const auto start = pos;
        while (pos < json.size() && json[pos] != '"') {
            if (static_cast<unsigned char>(json[pos]) < 32u || json[pos] == '\\') return std::nullopt;
            ++pos;
        }
        if (pos == json.size()) return std::nullopt;
        const auto value = json.substr(start, pos - start); ++pos; return value;
    };
    ContestPrivateBundle result;
    unsigned fields = 0;
    if (!take('{')) return std::nullopt;
    for (;;) {
        const auto key = text();
        if (!key || !take(':')) return std::nullopt;
        unsigned field = 0;
        if (*key == "programmingVersion") {
            field = 1; if (!take('1')) return std::nullopt;
        } else if (*key == "packageId") {
            field = 2; const auto value = text(); if (!value || *value != package_id) return std::nullopt;
        } else if (*key == "packageDigestHex" || *key == "gradingDigestHex") {
            field = *key == "packageDigestHex" ? 4u : 8u;
            const auto value = text(); security::Sha256Digest digest{};
            if (!value || !parse_sha256_hex(*value, digest)) return std::nullopt;
            if (field == 4u && digest != package_digest) return std::nullopt;
            if (field == 8u) result.grading_digest = digest;
        } else if (*key == "problems") {
            field = 16; space(); const auto start = pos;
            if (!take('[')) return std::nullopt;
            std::array<char, 64> stack{}; stack[0] = ']'; std::size_t depth = 1;
            bool quoted = false, escaped = false;
            while (pos < json.size() && depth != 0) {
                const char ch = json[pos++];
                if (quoted) {
                    if (static_cast<unsigned char>(ch) < 32u) return std::nullopt;
                    if (escaped) escaped = false;
                    else if (ch == '\\') escaped = true;
                    else if (ch == '"') quoted = false;
                } else if (ch == '"') quoted = true;
                else if (ch == '[' || ch == '{') {
                    if (depth == stack.size()) return std::nullopt;
                    stack[depth++] = ch == '[' ? ']' : '}';
                } else if (ch == ']' || ch == '}') {
                    if (stack[depth - 1] != ch) return std::nullopt;
                    --depth;
                }
            }
            if (depth != 0 || quoted || escaped) return std::nullopt;
            result.problems_json = json.substr(start, pos - start);
        } else return std::nullopt;
        if ((fields & field) != 0) return std::nullopt;
        fields |= field;
        if (take('}')) break;
        if (!take(',')) return std::nullopt;
    }
    space();
    return fields == 31u && pos == json.size() ? std::optional{std::move(result)} : std::nullopt;
}

enum class ContestNotice { none, pending, public_imported, private_imported, listening,
    stopped, queued, rejected, import_failed, listener_failed, unavailable, selection_failed, selection_unavailable };

[[nodiscard]] inline const char* contest_notice_label(ContestNotice notice, bool vi) noexcept {
    switch (notice) {
    case ContestNotice::none: return "";
    case ContestNotice::pending: return vi ? "Yêu cầu đang xử lý." : "Operator request pending.";
    case ContestNotice::public_imported: return vi ? "Đã kiểm tra và đăng ký gói công khai." : "Public package verified and registered.";
    case ContestNotice::private_imported: return vi ? "Đã đăng ký bộ chấm riêng." : "Private grading registered.";
    case ContestNotice::listening: return vi ? "Đã mở phòng." : "Room listener started.";
    case ContestNotice::stopped: return vi ? "Đã dừng kết nối; trạng thái vòng thi vẫn được giữ." : "Listener stopped; round state retained.";
    case ContestNotice::queued: return vi ? "Yêu cầu đã vào hàng đợi; chờ xác nhận từ phòng." : "Request queued; awaiting authoritative room state.";
    case ContestNotice::rejected: return vi ? "Yêu cầu bị từ chối; kiểm tra trạng thái phòng." : "Request rejected; check the room state.";
    case ContestNotice::import_failed: return vi ? "Không thể kiểm tra hoặc đăng ký tài liệu; kiểm tra gói và dấu kiểm." : "Material verification or registration failed; check package and pins.";
    case ContestNotice::listener_failed: return vi ? "Không thể mở phòng; kiểm tra gốc hồ sơ riêng, gói và broker." : "Listener start failed; check private profile root, package and broker.";
    case ContestNotice::unavailable: return vi ? "Broker không khả dụng hoặc trả trạng thái không hợp lệ." : "Broker unavailable or returned invalid state.";
    case ContestNotice::selection_failed: return vi ? "Đường dẫn đã chọn quá dài cho ô nhập; giá trị cũ được giữ." : "Selected path exceeds the input limit; the previous value is retained.";
    case ContestNotice::selection_unavailable: return vi ? "Không mở được hộp chọn tệp hoặc thư mục. Nhập đường dẫn đầy đủ vào ô; giá trị cũ được giữ." : "File or folder chooser unavailable. Type the full path in the field; the previous value is retained.";
    }
    return "";
}

[[nodiscard]] inline bool contest_sas_matches(std::string_view input, std::string_view sas) noexcept {
    return input.size() == 6 && input == sas &&
        std::all_of(input.begin(), input.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}
[[nodiscard]] inline std::string contest_grouped_sas(std::string_view sas) {
    return contest_sas_matches(sas, sas) ? std::string(sas.substr(0, 3)) + " " + std::string(sas.substr(3)) : std::string{};
}
[[nodiscard]] inline ImGuiID contest_peer_ui_id(const server::PendingRoomPairing& peer) noexcept {
    auto id = ImHashData(&peer.peer_id, sizeof(peer.peer_id));
    id = ImHashData(peer.peer_user_uuid.data(), peer.peer_user_uuid.size(), id);
    return ImHashData(peer.sas.data(), peer.sas.size(), id);
}
[[nodiscard]] inline ImGuiID contest_member_ui_id(const exam::StateRequest& context) noexcept {
    auto id = ImHashData(context.client_id.data(), context.client_id.size());
    id = ImHashData(context.session_id.data(), context.session_id.size(), id);
    id = ImHashData(context.package_digest.data(), context.package_digest.size(), id);
    const auto package_length = context.package_id.size();
    id = ImHashData(&package_length, sizeof(package_length), id);
    id = ImHashData(context.package_id.data(), context.package_id.size(), id);
    const auto candidate_length = context.candidate_id.size();
    id = ImHashData(&candidate_length, sizeof(candidate_length), id);
    return ImHashData(context.candidate_id.data(), context.candidate_id.size(), id);
}
[[nodiscard]] inline bool contest_same_member(const exam::StateRequest& a, const exam::StateRequest& b) noexcept {
    return a.package_id == b.package_id && a.package_digest == b.package_digest &&
        a.client_id == b.client_id && a.session_id == b.session_id && a.candidate_id == b.candidate_id;
}

struct ContestSasComparison {
    std::optional<server::PendingRoomPairing> peer;
    std::array<char, 7> input{};
    bool focus_input = false;
    ImGuiID return_focus = 0;
    void start(const server::PendingRoomPairing& value) {
        peer = value; input = {}; focus_input = true; return_focus = 0;
    }
    [[nodiscard]] const server::PendingRoomPairing* current(const server::ContestRoomSnapshot& snapshot) const noexcept {
        if (!peer) return nullptr;
        const auto found = std::find_if(snapshot.pending.begin(), snapshot.pending.end(), [&](const auto& value) {
            return value.peer_id == peer->peer_id && value.peer_user_uuid == peer->peer_user_uuid && value.sas == peer->sas;
        });
        return found == snapshot.pending.end() ? nullptr : &*found;
    }
    void cancel() noexcept {
        return_focus = peer ? contest_peer_ui_id(*peer) : 0;
        peer.reset(); input = {}; focus_input = false;
    }
};

[[nodiscard]] inline bool contest_finish_available(const server::ContestRoomSnapshot& snapshot,
    const server::ImmutableRoomState& round, server::OperatorRole role) noexcept {
    return contest_can_manage(role) && snapshot.running && snapshot.room &&
        snapshot.room->phase != server::RoomPhase::closed &&
        snapshot.room->immutable.room_id == round.room_id && snapshot.room->immutable.round_id == round.round_id;
}
[[nodiscard]] inline const server::RoomMember* contest_removal_target(const server::ContestRoomSnapshot& snapshot,
    const server::RoomMember& target, server::OperatorRole role) noexcept {
    if (!contest_can_manage(role) || !snapshot.running || !snapshot.room || snapshot.room->phase == server::RoomPhase::closed) return nullptr;
    const auto found = std::find_if(snapshot.room->members.begin(), snapshot.room->members.end(), [&](const auto& member) {
        return contest_same_member(member.context, target.context) && member.member_version == target.member_version &&
            member.display_name == target.display_name;
    });
    return found == snapshot.room->members.end() ? nullptr : &*found;
}
[[nodiscard]] inline const char* contest_begin_reason(const server::ContestRoomSnapshot& snapshot,
    const JudgeReadiness& judge, ContestClock::time_point now, bool vi) noexcept {
    if (!snapshot.running || !snapshot.room) return vi ? "Mở hoặc mở lại kết nối phòng trước." : "Open or resume the listener first.";
    const auto& room = *snapshot.room;
    if (room.phase != server::RoomPhase::prepared && room.phase != server::RoomPhase::joining)
        return vi ? "Vòng thi đã bắt đầu hoặc đã đóng." : "The round has begun or is closed.";
    if (room.members.empty()) return vi ? "So sánh mã trên cả hai màn hình và nhận ít nhất một thí sinh." : "Compare both screens and admit at least one candidate.";
    if (std::any_of(room.members.begin(), room.members.end(), [](const auto& member) { return !member.prepared; }))
        return vi ? "Chờ mọi thí sinh xác nhận đúng gói đã kiểm tra." : "Wait for every candidate's genuine verified package ACK.";
    if (!contest_can_begin(snapshot, judge, now)) return vi ? "Chờ xác nhận bộ chấm mới cho đúng phòng, vòng thi và phiên bản." : "Wait for fresh judge readiness for this room, round and revision.";
    return vi ? "[+] Đủ điều kiện bắt đầu vòng thi." : "[+] Round prerequisites verified.";
}
[[nodiscard]] inline const char* contest_standings_label(const std::optional<server::StandingsSnapshot>& standings,
    ContestClock::time_point checked, ContestClock::time_point now, bool vi) noexcept {
    if (!standings) return vi ? "[ ] Chưa có bảng xếp hạng" : "[ ] Standings unavailable";
    if (now < checked || now - checked > kContestRefreshInterval)
        return vi ? "[!] Bảng xếp hạng cũ; kết quả được giữ chỉ là tạm thời" : "[!] Stale standings; retained results are provisional";
    if (standings->freshness == server::StandingsFreshness::current)
        return vi ? "[+] Bảng xếp hạng tổng hợp hiện tại" : "[+] Current aggregate standings";
    if (standings->freshness == server::StandingsFreshness::pending)
        return vi ? "[!] Đang chờ chấm" : "[!] Judging pending";
    return vi ? "[!] Bộ chấm không khả dụng" : "[!] Judge unavailable";
}

[[nodiscard]] inline bool contest_button(const char* id, const char* label, bool enabled = true, ContestSemantics* semantics = nullptr, const char* semantic_label = nullptr) {
    ImGui::PushID(id);
    const float width = ImGui::GetContentRegionAvail().x;
    const auto text = ImGui::CalcTextSize(label, nullptr, false,
        std::max(1.0f, width - ImGui::GetStyle().FramePadding.x * 2.0f));
    const float height = std::max(ImGui::GetFrameHeight(), text.y + ImGui::GetStyle().FramePadding.y * 2.0f);
    const float button_width = std::min(width, text.x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button("##action", {button_width, height});
    bool clicked = pressed;
    const auto minimum = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
        {minimum.x + ImGui::GetStyle().FramePadding.x, minimum.y + ImGui::GetStyle().FramePadding.y},
        ImGui::GetColorU32(ImGuiCol_Text), label, nullptr,
        std::max(1.0f, button_width - ImGui::GetStyle().FramePadding.x * 2.0f));
    if (semantics) {
        auto& node = semantics->item(id, semantic_label ? semantic_label : label, accessibility::Kind::button);
        node.enabled = node.enabled && enabled;
        semantics->focus(node);
        const bool invoked = semantics->request(node, accessibility::Action::invoke).has_value();
        if (invoked) clicked = true;
        if (pressed && !invoked) semantics->invoked(node.key);
    }
    ImGui::EndDisabled(); ImGui::PopID();
    return clicked;
}

[[nodiscard]] inline std::optional<server::RoomMember> draw_contest_participants(const server::ContestRoomSnapshot& snapshot,
    server::OperatorRole role, bool vi, float scale, bool compact, ImGuiID* return_focus = nullptr, ContestSemantics* semantics = nullptr) {
    std::optional<server::RoomMember> removal;
    if (!snapshot.room || snapshot.room->members.empty()) {
        ImGui::TextWrapped("%s", vi ? "Mở phòng và nhận thí sinh trước." : "Open the room and approve candidates first.");
        if (semantics) semantics->text("participants-empty", vi ? "Mở phòng và nhận thí sinh trước." : "Open the room and approve candidates first.");
        return removal;
    }
    const float narrow_header = std::max({ImGui::CalcTextSize(vi ? "Gói" : "Package").x,
        ImGui::CalcTextSize(vi ? "Kết nối" : "Connection").x,
        ImGui::CalcTextSize(vi ? "Thao tác" : "Action").x});
    const float unit = std::max(narrow_header,
        ImGui::CalcTextSize(vi ? "Tên hiển thị (chưa xác minh)" : "Display name (unverified)").x / 2.0f) +
        ImGui::GetStyle().CellPadding.x * 2.0f;
    compact = compact || ImGui::GetContentRegionAvail().x < unit * 5.0f + ImGui::GetStyle().CellPadding.x * 2.0f;
    const bool table = !compact && ImGui::BeginTable("contest-members", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp);
    if (table) {
        ImGui::TableSetupColumn(vi ? "Tên hiển thị (chưa xác minh)" : "Display name (unverified)", 0, 2.0f);
        ImGui::TableSetupColumn(vi ? "Gói" : "Package", 0, 1.0f); ImGui::TableSetupColumn(vi ? "Kết nối" : "Connection", 0, 1.0f);
        ImGui::TableSetupColumn(vi ? "Thao tác" : "Action", 0, 1.0f); ImGui::TableHeadersRow();
        if (semantics) semantics->headers({vi ? "Tên hiển thị (chưa xác minh)" : "Display name (unverified)", vi ? "Gói" : "Package", vi ? "Kết nối" : "Connection", vi ? "Thao tác" : "Action"});
    }
    for (const auto& member : snapshot.room->members) {
        ContestSemanticScope semantic_scope(semantics);
        if (semantics) {
            const auto hex = [](const auto& bytes) { std::string value; constexpr char digits[] = "0123456789abcdef";
                for (const auto byte : bytes) { const auto n = std::to_integer<unsigned>(byte); value += digits[n >> 4u]; value += digits[n & 15u]; } return value; };
            semantics->prefix += "/member/" + hex(member.context.client_id) + "/" + hex(member.context.session_id) + "/" + hex(member.context.package_digest) +
                "/" + std::to_string(member.context.package_id.size()) + ":" + member.context.package_id + "/" + member.context.candidate_id;
            semantics->generation = member.member_version;
        }
        const bool connected = std::any_of(snapshot.peers.begin(), snapshot.peers.end(), [&](const auto& peer) {
            return peer.connected && peer.context.client_id == member.context.client_id;
        });
        ImGui::PushOverrideID(contest_member_ui_id(member.context));
        if (table) { ImGui::TableNextRow(0, std::max(geometry::table_row_min * scale, ImGui::GetFrameHeight() + spacing::sm * 2.0f * scale)); ImGui::TableNextColumn(); }
        if (semantics) {
            const auto cursor = ImGui::GetCursorScreenPos();
            auto& row = semantics->pane("row", vi ? "Thí sinh" : "Participant", accessibility::Kind::row,
                ImRect(cursor, {cursor.x + ImGui::GetContentRegionAvail().x, cursor.y + ImGui::GetTextLineHeight()}));
            semantics->parent = row.key;
        }
        ImGui::TextWrapped("%s%s", table ? "" : vi ? "Tên hiển thị (chưa xác minh): " : "Display name (unverified): ", member.display_name.c_str());
        if (semantics) semantics->text("name", (std::string(vi ? "Tên hiển thị (chưa xác minh): " : "Display name (unverified): ") + member.display_name).c_str());
        if (table) ImGui::TableNextColumn();
        ImGui::TextWrapped("%s%s", table ? "" : vi ? "Gói: " : "Package: ", contest_member_label(member.prepared, vi));
        if (semantics) semantics->text("package", (std::string(vi ? "Gói: " : "Package: ") + contest_member_label(member.prepared, vi)).c_str());
        if (table) ImGui::TableNextColumn();
        ImGui::TextWrapped("%s%s", table ? "" : vi ? "Kết nối: " : "Connection: ",
            connected ? (vi ? "Đã kết nối" : "Connected") : (vi ? "Mất kết nối" : "Disconnected"));
        if (semantics) semantics->text("connection", (std::string(vi ? "Kết nối: " : "Connection: ") + (connected ? (vi ? "Đã kết nối" : "Connected") : (vi ? "Mất kết nối" : "Disconnected"))).c_str());
        if (table) ImGui::TableNextColumn();
        if (return_focus && *return_focus == contest_member_ui_id(member.context)) {
            ImGui::SetKeyboardFocusHere(); *return_focus = 0;
        }
        if (contest_button("remove", vi ? "Loại thí sinh…" : "Remove participant…",
            contest_can_manage(role) && snapshot.running && snapshot.room->phase != server::RoomPhase::closed, semantics,
            (std::string(vi ? "Loại thí sinh (tên chưa xác minh): " : "Remove participant (unverified name): ") + member.display_name).c_str())) removal = member;
        if (!table) ImGui::Separator();
        ImGui::PopID();
    }
    if (return_focus) *return_focus = 0;
    if (table) ImGui::EndTable();
    return removal;
}

inline void draw_contest_standings(const std::optional<server::StandingsSnapshot>& standings,
    ContestClock::time_point checked, ContestClock::time_point now, bool vi, float scale, bool compact, ContestSemantics* semantics = nullptr) {
    ImGui::TextWrapped("%s", contest_standings_label(standings, checked, now, vi));
    if (semantics) semantics->text("standings-status", contest_standings_label(standings, checked, now, vi), false, true);
    if (!standings) {
        ImGui::TextWrapped("%s", vi ? "Chưa có lần làm mới thành công." : "No successful refresh yet.");
        if (semantics) semantics->text("standings-no-refresh", vi ? "Chưa có lần làm mới thành công." : "No successful refresh yet.");
        return;
    }
    if (semantics) {
        const auto same_row = [](const auto& a, const auto& b) { return a.rank == b.rank && a.display_name == b.display_name && a.solved == b.solved && a.penalty_minutes == b.penalty_minutes; };
        if (!semantics->standings_identity || semantics->standings_identity->room_id != standings->room_id ||
            semantics->standings_identity->round_id != standings->round_id ||
            !std::equal(semantics->standings_identity->rows.begin(), semantics->standings_identity->rows.end(), standings->rows.begin(), standings->rows.end(), same_row)) {
            semantics->standings_identity = standings; ++semantics->standings_generation;
        }
    }
    if (now >= checked) ImGui::TextWrapped("%s: %llu s", vi ? "Lần làm mới thành công cách đây" : "Last successful refresh",
        static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::seconds>(now - checked).count()));
    else ImGui::TextWrapped("%s", vi ? "Không xác định tuổi lần làm mới." : "Refresh age unavailable.");
    if (semantics) semantics->text("standings-age", now >= checked ?
        (std::string(vi ? "Lần làm mới thành công cách đây: " : "Last successful refresh: ") + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - checked).count()) + " s").c_str() :
        (vi ? "Không xác định tuổi lần làm mới." : "Refresh age unavailable."));
    const float narrow_header = std::max({ImGui::CalcTextSize(vi ? "Hạng" : "Rank").x,
        ImGui::CalcTextSize(vi ? "Đã giải" : "Solved").x,
        ImGui::CalcTextSize(vi ? "Phạt (phút)" : "Penalty (minutes)").x});
    const float unit = std::max(narrow_header,
        ImGui::CalcTextSize(vi ? "Tên hiển thị (chưa xác minh)" : "Display name (unverified)").x / 3.0f) +
        ImGui::GetStyle().CellPadding.x * 2.0f;
    compact = compact || ImGui::GetContentRegionAvail().x < unit * 6.0f + ImGui::GetStyle().CellPadding.x * 2.0f;
    const bool table = !compact && ImGui::BeginTable("contest-standings", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp);
    if (table) {
        ImGui::TableSetupColumn(vi ? "Hạng" : "Rank", 0, 1.0f);
        ImGui::TableSetupColumn(vi ? "Tên hiển thị (chưa xác minh)" : "Display name (unverified)", 0, 3.0f);
        ImGui::TableSetupColumn(vi ? "Đã giải" : "Solved", 0, 1.0f); ImGui::TableSetupColumn(vi ? "Phạt (phút)" : "Penalty (minutes)", 0, 1.0f);
        ImGui::TableHeadersRow();
        if (semantics) semantics->headers({vi ? "Hạng" : "Rank", vi ? "Tên hiển thị (chưa xác minh)" : "Display name (unverified)", vi ? "Đã giải" : "Solved", vi ? "Phạt (phút)" : "Penalty (minutes)"});
    }
    for (const auto& row : standings->rows) {
        ContestSemanticScope semantic_scope(semantics);
        if (semantics) {
            // Aggregate rows have no command target; their snapshot rank/order is not an authority identity.
            semantics->prefix += "/score/" + std::to_string(&row - standings->rows.data());
            semantics->generation = semantics->standings_generation;
        }
        if (table) { ImGui::TableNextRow(0, std::max(geometry::table_row_min * scale, ImGui::GetFrameHeight() + spacing::sm * 2.0f * scale)); ImGui::TableNextColumn(); }
        if (semantics) {
            const auto cursor = ImGui::GetCursorScreenPos();
            auto& group = semantics->pane("row", vi ? "Kết quả tổng hợp" : "Aggregate result", accessibility::Kind::row,
                ImRect(cursor, {cursor.x + ImGui::GetContentRegionAvail().x, cursor.y + ImGui::GetTextLineHeight()}));
            semantics->parent = group.key;
        }
        ImGui::TextWrapped("%s%u", table ? "" : vi ? "Hạng: " : "Rank: ", row.rank);
        if (semantics) semantics->text("rank", (std::string(vi ? "Hạng: " : "Rank: ") + std::to_string(row.rank)).c_str());
        if (table) ImGui::TableNextColumn();
        ImGui::TextWrapped("%s%s", table ? "" : vi ? "Tên hiển thị (chưa xác minh): " : "Display name (unverified): ", row.display_name.c_str());
        if (semantics) semantics->text("name", (std::string(vi ? "Tên hiển thị (chưa xác minh): " : "Display name (unverified): ") + row.display_name).c_str());
        if (table) ImGui::TableNextColumn();
        ImGui::TextWrapped("%s%u", table ? "" : vi ? "Đã giải: " : "Solved: ", row.solved);
        if (semantics) semantics->text("solved", (std::string(vi ? "Đã giải: " : "Solved: ") + std::to_string(row.solved)).c_str());
        if (table) ImGui::TableNextColumn();
        ImGui::TextWrapped("%s%llu", table ? "" : vi ? "Phạt (phút): " : "Penalty (minutes): ", static_cast<unsigned long long>(row.penalty_minutes));
        if (semantics) semantics->text("penalty", (std::string(vi ? "Phạt (phút): " : "Penalty (minutes): ") + std::to_string(row.penalty_minutes)).c_str());
        if (!table) ImGui::Separator();
    }
    if (table) ImGui::EndTable();
}


enum class ContestAdmissionKind { approve, reject };
struct ContestAdmissionAction {
    ContestAdmissionKind kind;
    server::PendingRoomPairing peer;
    std::string sas;
};
[[nodiscard]] inline std::optional<ContestAdmissionAction> draw_contest_admission(
    const server::ContestRoomSnapshot& snapshot, server::OperatorRole role, bool vi, ImFont* sas,
    ContestSasComparison& comparison, bool dark = false, ContestSemantics* semantics = nullptr) {
    const auto tr = [vi](const char* en, const char* vn) { return vi ? vn : en; };
    const bool permitted = contest_can_manage(role);
    std::optional<ContestAdmissionAction> action;
    if (semantics) semantics->prune_peers(snapshot.pending);
    if (comparison.peer && !comparison.current(snapshot)) comparison.cancel();
    ImGui::TextWrapped("%s", tr("Names are unverified display hints. Compare the six-digit code on both screens before approving.",
        "Tên chỉ là thông tin hiển thị chưa xác minh. So sánh mã sáu chữ số trên cả hai màn hình trước khi nhận."));
    if (semantics) semantics->text("admission-instructions", tr("Names are unverified display hints. Compare the six-digit code on both screens before approving.",
        "Tên chỉ là thông tin hiển thị chưa xác minh. So sánh mã sáu chữ số trên cả hai màn hình trước khi nhận."));
    ImGui::Text("%s: %zu", tr("Pending requests", "Yêu cầu đang chờ"), snapshot.pending.size());
    if (semantics) semantics->text("pending-count", (std::string(tr("Pending requests: ", "Yêu cầu đang chờ: ")) + std::to_string(snapshot.pending.size())).c_str());
    const bool admitting = snapshot.running && (!snapshot.room ||
        (snapshot.room->phase != server::RoomPhase::running && snapshot.room->phase != server::RoomPhase::closed));
    if (!admitting) {
        ImGui::TextWrapped("%s", tr("Admission is closed until the listener accepts joins; it closes when the round begins.", "Chỉ nhận thí sinh khi kết nối phòng cho phép; ngừng nhận khi vòng thi bắt đầu."));
        if (semantics) semantics->text("admission-closed", tr("Admission is closed until the listener accepts joins; it closes when the round begins.", "Chỉ nhận thí sinh khi kết nối phòng cho phép; ngừng nhận khi vòng thi bắt đầu."));
    }
    for (const auto& peer : snapshot.pending) {
        ContestSemanticScope semantic_scope(semantics);
        if (semantics) {
            semantics->prefix += "/peer/" + std::to_string(peer.peer_id) + "/" + peer.peer_user_uuid;
            // Exact private tuple lifetime; protected bytes never enter the published node or key.
            semantics->generation = semantics->peer_generation(peer);
            const auto cursor = ImGui::GetCursorScreenPos();
            auto& row = semantics->pane("row", tr("Pending request", "Yêu cầu đang chờ"), accessibility::Kind::row,
                ImRect(cursor, {cursor.x + ImGui::GetContentRegionAvail().x, cursor.y + ImGui::GetTextLineHeight()}));
            semantics->parent = row.key;
        }
        ImGui::PushOverrideID(contest_peer_ui_id(peer));
        ImGui::TextWrapped("%s: %s", tr("Display name (unverified)", "Tên hiển thị (chưa xác minh)"), peer.display_name.c_str());
        if (semantics) semantics->text("name", (std::string(tr("Display name (unverified): ", "Tên hiển thị (chưa xác minh): ")) + peer.display_name).c_str());
        if (sas) ImGui::PushFont(sas);
        const auto grouped = contest_grouped_sas(peer.sas);
        ImGui::TextUnformatted(grouped.empty() ? tr("Code unavailable", "Mã không khả dụng") : grouped.c_str());
        if (semantics) {
            auto& code = semantics->item("displayed-sas", grouped.empty() ? tr("Code unavailable", "Mã không khả dụng") :
                tr("Protected verification code; compare both screens visually", "Mã xác minh được bảo vệ; so sánh trực tiếp trên cả hai màn hình"), accessibility::Kind::text);
            code.protected_value = true;
            semantics->focus(code);
        }
        if (sas) ImGui::PopFont();
        if (comparison.return_focus == contest_peer_ui_id(peer)) { ImGui::SetKeyboardFocusHere(); comparison.return_focus = 0; }
        if (contest_button("compare", tr("Compare SAS", "So sánh mã SAS"), permitted && admitting, semantics,
            (std::string(tr("Compare SAS (unverified name): ", "So sánh mã SAS (tên chưa xác minh): ")) + peer.display_name).c_str())) comparison.start(peer);
        if (contest_button("reject", tr("Reject request", "Từ chối yêu cầu"), permitted, semantics,
            (std::string(tr("Reject request (unverified name): ", "Từ chối yêu cầu (tên chưa xác minh): ")) + peer.display_name).c_str()))
            action = ContestAdmissionAction{ContestAdmissionKind::reject, peer, {}};
        if (comparison.peer && comparison.current(snapshot) == &peer) {
            ImGui::TextWrapped("%s", tr("Type all six digits from the student's screen. Approve only after both screens match.",
                "Nhập đủ sáu chữ số trên màn hình thí sinh. Chỉ nhận sau khi mã trên cả hai màn hình trùng nhau."));
            if (semantics) semantics->text("comparison-instructions", tr("Type all six digits from the student's screen. Approve only after both screens match.",
                "Nhập đủ sáu chữ số trên màn hình thí sinh. Chỉ nhận sau khi mã trên cả hai màn hình trùng nhau."));
            ImGui::TextWrapped("%s", tr("Six-digit comparison code", "Mã so sánh sáu chữ số"));
            ImGui::SetNextItemWidth(-1);
            if (comparison.focus_input) { ImGui::SetKeyboardFocusHere(); comparison.focus_input = false; }
            ImGui::PushStyleColor(ImGuiCol_Border, interim_color(ColorRole::input_border, dark));
            const bool edited = ImGui::InputText("##contest-confirm-sas", comparison.input.data(), comparison.input.size(), ImGuiInputTextFlags_CharsDecimal);
            if (semantics) {
                auto& node = semantics->item("comparison-input", tr("Six-digit comparison code", "Mã so sánh sáu chữ số"), accessibility::Kind::edit);
                node.enabled = permitted && admitting && node.enabled;
                semantics->edit(node, comparison.input.data(), comparison.input.size(), accessibility::TextInput::digits, edited);
            }
            ImGui::PopStyleColor();
            if (contest_button("approve", tr("Approve matching code", "Nhận khi mã trùng khớp"),
                permitted && admitting && contest_sas_matches(comparison.input.data(), peer.sas), semantics)) {
                action = ContestAdmissionAction{ContestAdmissionKind::approve, peer, comparison.input.data()};
                comparison.cancel();
            }
            if (contest_button("cancel", tr("Cancel comparison", "Hủy so sánh"), true, semantics) || ImGui::IsKeyPressed(ImGuiKey_Escape)) comparison.cancel();
        }
        ImGui::Separator(); ImGui::PopID();
    }
    if (snapshot.pending.empty()) {
        ImGui::TextWrapped("%s", tr("No pending join requests.", "Chưa có yêu cầu tham gia."));
        if (semantics) semantics->text("admission-empty", tr("No pending join requests.", "Chưa có yêu cầu tham gia."));
    }
    if (comparison.return_focus && std::none_of(snapshot.pending.begin(), snapshot.pending.end(),
        [&](const auto& peer) { return contest_peer_ui_id(peer) == comparison.return_focus; })) comparison.return_focus = 0;
    return action;
}
template<class Function> [[nodiscard]] inline Function contest_platform_function(HMODULE module, const char* name) noexcept {
    const auto address = module ? GetProcAddress(module, name) : nullptr;
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}
[[nodiscard]] inline std::string contest_utf8_path(std::wstring_view value) {
    if (value.empty() || value.size() > 32767) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr)) return {};
    return result;
}
// Platform pickers only populate the operator's existing buffer; import/pin validation stays separate.
[[nodiscard]] inline std::string contest_pick_path(bool folder, bool vi, bool* unavailable = nullptr) {
    std::array<wchar_t, 32768> selected{};
    bool failed = true;
    if (folder) {
        // shortcut: the folder chooser uses MAX_PATH; keep manual full-path entry, upgrade when longer picker paths are required.
        const auto module = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto browse = contest_platform_function<decltype(&SHBrowseForFolderW)>(module, "SHBrowseForFolderW");
        const auto resolve = contest_platform_function<decltype(&SHGetPathFromIDListW)>(module, "SHGetPathFromIDListW");
        const auto com = LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto initialize = contest_platform_function<decltype(&CoInitializeEx)>(com, "CoInitializeEx");
        const auto uninitialize = contest_platform_function<decltype(&CoUninitialize)>(com, "CoUninitialize");
        const auto release = contest_platform_function<decltype(&CoTaskMemFree)>(com, "CoTaskMemFree");
        if (browse && resolve && release && initialize && uninitialize) {
            const HRESULT initialized = initialize(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(initialized)) {
                failed = false;
                BROWSEINFOW info{}; info.hwndOwner = GetActiveWindow();
                info.lpszTitle = vi ? L"Chọn thư mục hiện có" : L"Choose an existing folder";
                info.ulFlags = BIF_RETURNONLYFSDIRS;
                if (auto* pidl = browse(&info)) { failed = !resolve(pidl, selected.data()); release(pidl); }
                uninitialize(); // Balances our S_OK or S_FALSE; failed apartment initialization owns nothing.
            }
        }
        if (com) FreeLibrary(com);
        if (module) FreeLibrary(module);
    } else {
        const auto module = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto choose = contest_platform_function<decltype(&GetOpenFileNameW)>(module, "GetOpenFileNameW");
        const auto extended_error = contest_platform_function<decltype(&CommDlgExtendedError)>(module, "CommDlgExtendedError");
        if (choose && extended_error) {
            failed = false;
            OPENFILENAMEW info{}; info.lStructSize = static_cast<DWORD>(sizeof(info)); info.hwndOwner = GetActiveWindow();
            info.lpstrFile = selected.data(); info.nMaxFile = static_cast<DWORD>(selected.size());
            info.lpstrFilter = L"All files\0*.*\0\0";
            info.lpstrTitle = vi ? L"Chọn tệp hiện có" : L"Choose an existing file";
            info.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!choose(&info)) { failed = extended_error() != 0; selected[0] = L'\0'; }
        }
        if (module) FreeLibrary(module);
    }
    if (failed) selected[0] = L'\0';
    auto result = contest_utf8_path(selected.data());
    if (selected[0] != L'\0' && result.empty()) failed = true;
    if (unavailable) *unavailable = failed;
    return result;
}

class ContestRoomPanel {
public:
    ContestRoomPanel() : broker_(server::programming_broker_environment()), listener_(broker_) {
        put(room_id_, random_id()); put(round_id_, random_id());
        std::array<wchar_t, 32768> runtime{};
        const auto runtime_length = GetEnvironmentVariableW(L"NSTU_CONTEST_POWERSHELL_PATH", runtime.data(), static_cast<DWORD>(runtime.size()));
        if (runtime_length != 0 && runtime_length < runtime.size()) trusted_powershell_ = runtime.data();
        std::array<char, 65> runtime_pin{};
        if (GetEnvironmentVariableA("NSTU_CONTEST_POWERSHELL_SHA256", runtime_pin.data(), static_cast<DWORD>(runtime_pin.size())) == 64)
            trusted_powershell_pin_ = runtime_pin.data();
    }
    ~ContestRoomPanel() { shutdown(); }
    ContestRoomPanel(const ContestRoomPanel&) = delete;
    ContestRoomPanel& operator=(const ContestRoomPanel&) = delete;

    void shutdown() noexcept {
        if (operation_.valid()) operation_.wait();
        listener_.stop(); broker_.stop();
    }
    void poll() {
        if (operation_.valid() && operation_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto update = operation_.get(); notice_ = update.notice;
            if (update.material) { material_ = std::move(update.material); private_digest_.reset(); }
            if (update.private_digest) private_digest_ = update.private_digest;
            if (update.config) { config_ = std::move(update.config); locked_ = true; }
            if (update.readiness) readiness_ = std::move(*update.readiness);
            if (update.standings && config_ && contest_accept_standings(standings_, *update.standings, config_->room)) {
                standings_ = std::move(update.standings); standings_checked_ = ContestClock::now();
            }
        }
        const auto now = ContestClock::now();
        if (!operation_.valid() && config_ && listener_.snapshot().running && now >= next_refresh_) {
            next_refresh_ = now + kContestRefreshInterval;
            (void)launch([this] { return refresh(); });
        }
    }

    void draw(server::OperatorRole role, bool vi, bool dark, float scale,
        ImFont* heading = nullptr, ImFont* sas = nullptr, bool compact = false, ContestSemantics* semantics = nullptr);
    [[nodiscard]] const char* active_tab_label(bool vi) const noexcept;
    void draw_navigation(server::OperatorRole role, bool vi, ContestSemantics* semantics = nullptr);

private:
    enum class SetupAction { none, public_import, private_import, open };
    void draw_lifecycle(server::OperatorRole role, bool vi, bool menu, ContestSemantics* semantics = nullptr);
    void open_room();
    void show_setup_error(bool failed = false);
    struct Material { std::string id, manifest; security::Sha256Digest digest{}; std::uint32_t duration = 0; };
    struct Update {
        ContestNotice notice = ContestNotice::none;
        std::optional<Material> material;
        std::optional<security::Sha256Digest> private_digest;
        std::optional<server::ContestRoomServerConfig> config;
        std::optional<JudgeReadiness> readiness;
        std::optional<server::StandingsSnapshot> standings;
    };
    template<std::size_t N> static void put(std::array<char, N>& buffer, std::string_view text) {
        const auto count = std::min(text.size(), N - 1); std::copy_n(text.data(), count, buffer.data()); buffer[count] = 0;
    }
    static std::string random_id() {
        security::ClientId bytes{}; if (!security::generate_random(bytes)) return {};
        std::string value; constexpr char digits[] = "0123456789abcdef";
        for (const auto byte : bytes) { const auto n = std::to_integer<unsigned>(byte); value += digits[n >> 4u]; value += digits[n & 15u]; }
        return value;
    }
    static std::filesystem::path path(std::string_view text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
    static std::optional<std::string> read_private(const std::filesystem::path& file) {
        if (!file.is_absolute()) return std::nullopt;
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        if (!stream) return std::nullopt;
        const auto length = stream.tellg();
        if (length <= 0 || length > 4 * 1024 * 1024) return std::nullopt;
        std::string text(static_cast<std::size_t>(length), '\0'); stream.seekg(0);
        if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) return std::nullopt;
        return text;
    }
    [[nodiscard]] bool launch(std::function<Update()> action) {
        if (operation_.valid()) return false;
        try {
            operation_ = std::async(std::launch::async, [action = std::move(action)] {
                try { return action(); } catch (...) { Update failed; failed.notice = ContestNotice::unavailable; return failed; }
            });
            return true;
        } catch (...) { notice_ = ContestNotice::rejected; return false; }
    }
    Update refresh() {
        Update result;
        (void)listener_.refresh();
        const auto snapshot = listener_.snapshot();
        if (!snapshot.running || !snapshot.room) return result;
        const auto& room = *snapshot.room;
        JudgeReadiness ready{room.immutable.room_id, room.immutable.round_id, room.revision, false, ContestClock::now()};
        if ((room.phase == server::RoomPhase::prepared || room.phase == server::RoomPhase::joining) && !room.members.empty()) {
            ready.available = true;
            for (const auto& member : room.members) {
                if (!member.prepared || !broker_.ready(member.context)) { ready.available = false; break; }
            }
        }
        result.readiness = std::move(ready);
        result.standings = broker_.room_standings(room.immutable);
        return result;
    }

    server::ProgrammingBroker broker_;
    server::ContestRoomServer listener_;
    std::future<Update> operation_;
    std::optional<Material> material_;
    std::optional<security::Sha256Digest> private_digest_;
    std::optional<server::ContestRoomServerConfig> config_;
    std::filesystem::path trusted_powershell_;
    std::string trusted_powershell_pin_;
    JudgeReadiness readiness_;
    std::optional<server::StandingsSnapshot> standings_;
    ContestClock::time_point standings_checked_{}, next_refresh_{};
    ContestNotice notice_ = ContestNotice::none;
    bool locked_ = false, live_standings_ = false;
    int room_port_ = contest_discovery::kDefaultPort;
    std::array<char, 33> room_id_{}, round_id_{};
    std::array<char, 97> owner_{"teacher"}, package_id_{}, title_{"Programming contest"}, teacher_{"Teacher"};
    std::array<char, 65> content_pin_{};
    std::array<char, 2048> public_folder_{}, archive_{}, packages_root_{}, extractor_{}, profile_root_{}, private_file_{};
    int active_tab_ = 0;
    int content_tab_ = -1;
    bool select_tab_ = false, technical_open_ = false, public_technical_open_ = false;
    const char* focus_setup_ = nullptr;
    const char* unavailable_picker_field_ = nullptr;
    ContestNotice presented_notice_ = ContestNotice::none;
    SetupAction setup_action_ = SetupAction::none;
    ContestSasComparison comparison_;
    std::optional<server::RoomMember> removal_;
    std::optional<server::ImmutableRoomState> finishing_;
    ImGuiID return_member_focus_ = 0;
    bool confirmation_visible_ = false;
    std::uint64_t confirmation_generation_ = 0;
};

inline const char* ContestRoomPanel::active_tab_label(bool vi) const noexcept {
    constexpr std::array en{"Setup", "Admission", "Participants", "Standings"};
    constexpr std::array vn{"Thiết lập", "Nhận thí sinh", "Thí sinh", "Xếp hạng"};
    return (vi ? vn : en)[static_cast<std::size_t>(active_tab_)];
}

inline void ContestRoomPanel::show_setup_error(bool failed) {
    active_tab_ = 0; select_tab_ = true;
    technical_open_ = public_technical_open_ = true;
    security::Sha256Digest pin{};
    if (failed && setup_action_ == SetupAction::private_import) focus_setup_ = "##contest-private";
    else if (failed && setup_action_ == SetupAction::public_import) {
        if (public_folder_[0] == 0) focus_setup_ = "##contest-folder";
        else if (package_id_[0] == 0) focus_setup_ = "##contest-package";
        else if (!parse_sha256_hex(content_pin_.data(), pin)) focus_setup_ = "##contest-content-pin";
        else focus_setup_ = "##contest-folder";
    }
    else if (room_id_[0] == 0) focus_setup_ = "##contest-room";
    else if (round_id_[0] == 0) focus_setup_ = "##contest-round";
    else if (owner_[0] == 0) focus_setup_ = "##contest-owner";
    else if (!contest_room_port(room_port_)) focus_setup_ = "##contest-port";
    else if (profile_root_[0] == 0) focus_setup_ = "##contest-profile";
    else if (package_id_[0] == 0) focus_setup_ = "##contest-package";
    else if (!parse_sha256_hex(content_pin_.data(), pin)) focus_setup_ = "##contest-content-pin";
    else if (public_folder_[0] == 0) focus_setup_ = "##contest-folder";
    else if (archive_[0] == 0) focus_setup_ = "##contest-archive";
    else if (packages_root_[0] == 0) focus_setup_ = "##contest-cache";
    else if (extractor_[0] == 0) focus_setup_ = "##contest-extractor";
    else if (private_file_[0] == 0) focus_setup_ = "##contest-private";
    else focus_setup_ = "##contest-profile";
}

inline void ContestRoomPanel::open_room() {
    const auto room_port = contest_room_port(room_port_);
    if (!material_ || !private_digest_ || !broker_.configured() || !room_port || operation_.valid()) return;
    server::ContestRoomServerConfig config;
    config.tcp_port = *room_port; config.udp_port = *room_port;
    config.room.room_id = room_id_.data(); config.room.round_id = round_id_.data(); config.room.owner_id = owner_.data();
    config.room.package_id = material_->id; config.room.package_digest = material_->digest;
    config.room.grading_digest = *private_digest_; config.room.duration_seconds = material_->duration;
    config.room.publication_policy = live_standings_ ? server::RoomPublicationPolicy::live : server::RoomPublicationPolicy::teacher_only;
    config.title = title_.data(); config.teacher_label = teacher_.data(); config.state_root = path(profile_root_.data());
    config.public_archive = server::RoomPublicArchiveSource{path(archive_.data()), path(packages_root_.data()), path(extractor_.data()),
        trusted_powershell_, trusted_powershell_pin_};
    setup_action_ = SetupAction::open; notice_ = ContestNotice::pending;
    (void)launch([this, config = std::move(config)]() mutable {
        Update result; result.notice = ContestNotice::listener_failed;
        if (!listener_.start(config)) return result;
        const auto state = listener_.snapshot();
        if (!state.running || !state.room) { listener_.stop(); return result; }
        config.room = state.room->immutable; result.config = std::move(config); result.notice = ContestNotice::listening;
        return result;
    });
    next_refresh_ = ContestClock::now();
}

inline void ContestRoomPanel::draw_lifecycle(server::OperatorRole role, bool vi, bool menu, ContestSemantics* semantics) {
    const auto tr = [vi](const char* en, const char* vn) { return vi ? vn : en; };
    const bool permitted = contest_can_manage(role);
    const auto snapshot = listener_.snapshot();
    const auto now = ContestClock::now();
    const bool busy = operation_.valid();
    bool first = true;
    const auto action = [&](const char* id, const char* label, bool enabled) {
        if (menu) {
            bool pressed = ImGui::MenuItem(label, nullptr, false, enabled);
            if (semantics) {
                auto& node = semantics->item(id, label, accessibility::Kind::button); node.enabled = enabled;
                semantics->focus(node); const bool invoked = semantics->request(node, accessibility::Action::invoke).has_value();
                if (pressed && !invoked) semantics->invoked(node.key);
                if (invoked) { pressed = true; ImGui::CloseCurrentPopup(); }
            }
            return pressed;
        }
        const float measured = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (!first && right - ImGui::GetItemRectMax().x >= measured + ImGui::GetStyle().ItemSpacing.x)
            ImGui::SameLine();
        first = false;
        return contest_button(id, label, enabled, semantics);
    };
    if (snapshot.running) {
        if (action("stop", tr("Stop listener", "Đóng kết nối phòng"), permitted && !busy)) {
            notice_ = ContestNotice::pending;
            (void)launch([this] { listener_.stop(); Update update; update.notice = ContestNotice::stopped; return update; });
            readiness_.available = false;
        }
    } else if (config_) {
        if (action("resume", tr("Resume listener", "Mở lại kết nối phòng"), permitted && !busy)) {
            const auto config = *config_; notice_ = ContestNotice::pending;
            (void)launch([this, config] {
                Update update; update.notice = listener_.start(config) ? ContestNotice::listening : ContestNotice::listener_failed;
                return update;
            });
            next_refresh_ = now;
        }
    } else if (action("open", tr("Open room", "Mở phòng"),
        permitted && !busy && material_ && private_digest_ && broker_.configured() && contest_room_port(room_port_))) open_room();
    if (action("begin", tr("Begin round", "Bắt đầu vòng thi"), permitted && config_ && contest_can_begin(snapshot, readiness_, now))) {
        notice_ = listener_.begin("teacher-begin-" + config_->room.round_id) ? ContestNotice::queued : ContestNotice::rejected;
        readiness_.available = false;
    }
    if (config_ && snapshot.room && snapshot.room->phase != server::RoomPhase::closed &&
        action("finish", tr("Finish round…", "Kết thúc vòng thi…"), permitted && snapshot.running))
        finishing_ = config_->room;
}

inline void ContestRoomPanel::draw_navigation(server::OperatorRole role, bool vi, ContestSemantics* semantics) {
    constexpr std::array en{"Setup", "Admission", "Participants", "Standings"};
    constexpr std::array vn{"Thiết lập", "Nhận thí sinh", "Thí sinh", "Xếp hạng"};
    ContestSemanticScope semantic_scope(semantics);
    if (semantics) {
        const auto snapshot = listener_.snapshot();
        semantics->prefix = "contest";
        if (snapshot.room) semantics->prefix += "/" + snapshot.room->immutable.room_id + "/" + snapshot.room->immutable.round_id;
    }
    ContestSemantics::Key tabs = 0;
    if (semantics) {
        auto& group = semantics->pane("tabs", vi ? "Không gian cuộc thi" : "Contest workspaces", accessibility::Kind::tab, ImGui::GetCurrentWindow()->InnerRect);
        tabs = group.key; semantics->parent = tabs;
    }
    for (int tab = 0; tab != 4; ++tab) {
        ImGui::PushID(tab);
        bool selected = ImGui::Selectable((vi ? vn : en)[static_cast<std::size_t>(tab)], active_tab_ == tab);
        if (semantics) {
            auto& node = semantics->item("nav-" + std::to_string(tab), (vi ? vn : en)[static_cast<std::size_t>(tab)], accessibility::Kind::tab_item);
            node.selected = active_tab_ == tab; semantics->focus(node);
            if (semantics->request(node, accessibility::Action::select)) { selected = true; ImGui::CloseCurrentPopup(); }
        }
        if (selected) {
            active_tab_ = tab; select_tab_ = true;
        }
        ImGui::PopID();
    }
    if (semantics) { semantics->select(tabs, semantics->key(semantics->prefix + "/nav-" + std::to_string(active_tab_))); semantics->parent = semantic_scope.parent; }
    ImGui::Separator();
    draw_lifecycle(role, vi, true, semantics);
    bool review = ImGui::MenuItem(vi ? "Điều kiện mở phòng" : "Opening checks");
    if (semantics) {
        auto& node = semantics->item("opening-checks", vi ? "Điều kiện mở phòng" : "Opening checks", accessibility::Kind::button);
        semantics->focus(node); if (semantics->request(node, accessibility::Action::invoke)) { review = true; ImGui::CloseCurrentPopup(); }
    }
    if (review) show_setup_error();
}

inline void ContestRoomPanel::draw(server::OperatorRole role, bool vi, bool dark, float scale,
    ImFont* heading, ImFont* sas, bool compact, ContestSemantics* semantics) {
    const auto tr = [vi](const char* en, const char* vn) { return vi ? vn : en; };
    const bool permitted = contest_can_manage(role);
    const auto snapshot = listener_.snapshot();
    const auto now = ContestClock::now();
    const bool busy = operation_.valid();
    ContestSemanticScope semantic_scope(semantics);
    ContestSemantics::Key pane = 0;
    if (semantics) {
        semantics->prefix = "contest";
        if (snapshot.room) semantics->prefix += "/" + snapshot.room->immutable.room_id + "/" + snapshot.room->immutable.round_id;
        semantics->parent = 0; semantics->scroll_owner = 0;
        auto& root = semantics->pane("pane", tr("Contests", "Cuộc thi"), accessibility::Kind::pane, ImGui::GetCurrentWindow()->InnerRect);
        pane = root.key; semantics->parent = pane;
        if (active_tab_ != 1) semantics->prune_peers({});
    }
    if (notice_ != presented_notice_) {
        if (!locked_ && (notice_ == ContestNotice::import_failed || notice_ == ContestNotice::listener_failed)) show_setup_error(true);
        presented_notice_ = notice_;
    }
    if (comparison_.peer && !comparison_.current(snapshot)) comparison_.cancel();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
        {spacing::md * scale, std::max(ImGui::GetStyle().CellPadding.y, spacing::sm * scale)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {spacing::md * scale, spacing::sm * scale});
    if (!compact) {
        if (heading) ImGui::PushFont(heading);
        ImGui::TextUnformatted(tr("Contests", "Cuộc thi"));
        if (semantics) semantics->text("heading", tr("Contests", "Cuộc thi"));
        if (heading) ImGui::PopFont();
    }
    const char* summary = busy ? tr("[~] Request pending", "[~] Đang xử lý")
        : !snapshot.running ? tr("[ ] Listener offline", "[ ] Phòng đóng")
        : contest_can_begin(snapshot, readiness_, now) ? tr("[+] Ready to begin", "[+] Có thể bắt đầu")
        : snapshot.room && snapshot.room->phase == server::RoomPhase::running ? tr("[+] Round running", "[+] Đang thi")
        : snapshot.room && snapshot.room->phase == server::RoomPhase::closed ? tr("[ ] Round closed", "[ ] Đã kết thúc")
        : tr("[!] Begin blocked", "[!] Chưa thể bắt đầu");
    ImGui::PushStyleColor(ImGuiCol_Text, interim_color(ColorRole::text, dark));
    ImGui::TextUnformatted(summary);
    if (semantics) semantics->text("summary", summary);
    ImGui::PopStyleColor();
    if (!compact) {
        draw_lifecycle(role, vi, false, semantics);
        constexpr std::array en{"Setup###contest-setup", "Admission###contest-admission", "Participants###contest-participants", "Standings###contest-standings"};
        constexpr std::array vn{"Thiết lập###contest-setup", "Nhận thí sinh###contest-admission", "Thí sinh###contest-participants", "Xếp hạng###contest-standings"};
        bool requested_tab = false;
        if (ImGui::BeginTabBar("contest-tabs")) {
            ContestSemantics::Key tabs = 0;
            if (semantics) {
                auto& group = semantics->pane("tabs", tr("Contest workspaces", "Không gian cuộc thi"), accessibility::Kind::tab,
                    ImGui::GetCurrentContext()->CurrentTabBar->BarRect); tabs = group.key; semantics->parent = tabs;
            }
            for (int tab = 0; tab != 4; ++tab) {
                const bool visible = ImGui::BeginTabItem((vi ? vn : en)[static_cast<std::size_t>(tab)], nullptr,
                    select_tab_ && active_tab_ == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None);
                if (semantics) {
                    auto& node = semantics->item("nav-" + std::to_string(tab), (vi ? vn : en)[static_cast<std::size_t>(tab)], accessibility::Kind::tab_item);
                    node.selected = active_tab_ == tab; semantics->focus(node);
                    if (semantics->request(node, accessibility::Action::select)) { active_tab_ = tab; select_tab_ = true; requested_tab = true; }
                }
                if (visible && !select_tab_) active_tab_ = tab;
                if (visible) ImGui::EndTabItem();
            }
            if (semantics) { semantics->select(tabs, semantics->key(semantics->prefix + "/nav-" + std::to_string(active_tab_))); semantics->parent = pane; }
            ImGui::EndTabBar();
        }
        select_tab_ = requested_tab;
    }
    if (ImGui::BeginChild("contest-content", {0, 0})) {
        if (semantics) {
            auto& content = semantics->pane("content", tr("Contest content", "Nội dung cuộc thi"), accessibility::Kind::pane, ImGui::GetCurrentWindow()->InnerRect);
            content.scrollable = true; content.generation = static_cast<std::uint64_t>(active_tab_ + 1);
            semantics->parent = content.key; semantics->scroll_owner = content.key;
        }
        if (content_tab_ != active_tab_) { ImGui::SetScrollY(0); content_tab_ = active_tab_; }
        ImGui::TextWrapped("%s", contest_begin_reason(snapshot, readiness_, now, vi));
        if (semantics) semantics->text("begin-reason", contest_begin_reason(snapshot, readiness_, now, vi), false, true);
        if (busy) {
            ImGui::TextWrapped("%s", contest_notice_label(ContestNotice::pending, vi));
            if (semantics) semantics->text("pending", contest_notice_label(ContestNotice::pending, vi));
        } else if (notice_ != ContestNotice::none) {
            ImGui::TextWrapped("%s", contest_notice_label(notice_, vi));
            if (semantics) semantics->text("notice", contest_notice_label(notice_, vi), true);
        }
        if (!snapshot.error.empty()) {
            ImGui::TextWrapped("%s", tr("[!] Listener reported an error; refresh or resume.", "[!] Phòng báo lỗi; làm mới hoặc mở lại kết nối."));
            if (semantics) semantics->text("listener-error", tr("[!] Listener reported an error; refresh or resume.", "[!] Phòng báo lỗi; làm mới hoặc mở lại kết nối."), true);
        }
        if (!permitted) {
            ImGui::TextWrapped("%s", tr("Switch to Teacher to manage this contest.", "Chọn vai trò Giáo viên để quản lý cuộc thi."));
            if (semantics) semantics->text("role-reason", tr("Switch to Teacher to manage this contest.", "Chọn vai trò Giáo viên để quản lý cuộc thi."));
        }
        if (snapshot.room) {
            ImGui::TextWrapped("%s: %s", tr("Round state", "Trạng thái vòng thi"), contest_phase_label(snapshot.room->phase, vi));
            if (semantics) semantics->text("round-state", (std::string(tr("Round state: ", "Trạng thái vòng thi: ")) + contest_phase_label(snapshot.room->phase, vi)).c_str());
            ImGui::Text("TCP %u / UDP %u", static_cast<unsigned>(snapshot.tcp_port), static_cast<unsigned>(snapshot.udp_port));
            if (semantics) semantics->text("ports", ("TCP " + std::to_string(snapshot.tcp_port) + " / UDP " + std::to_string(snapshot.udp_port)).c_str());
            if (snapshot.room->phase == server::RoomPhase::running || snapshot.room->phase == server::RoomPhase::closed) {
                const auto remaining = snapshot.room->deadline > snapshot.room->server_time
                    ? (snapshot.room->deadline - snapshot.room->server_time) / 1000u : 0u;
                ImGui::TextWrapped("%s: %llu s", tr("Broker clock remaining", "Thời gian còn lại từ broker"),
                    static_cast<unsigned long long>(remaining));
                if (semantics) semantics->text("remaining", (std::string(tr("Broker clock remaining: ", "Thời gian còn lại từ broker: ")) + std::to_string(remaining) + " s").c_str());
            }
        }
        if (compact && !locked_ && active_tab_ != 0 &&
            contest_button("setup-correction", tr("Review opening checks", "Xem điều kiện mở phòng"), true, semantics)) show_setup_error();
        ImGui::Separator();
        if (active_tab_ == 0) {
            const auto field = [&](const char* id, const char* label, auto& buffer) {
                ImGui::TextWrapped("%s", label);
                ImGui::SetNextItemWidth(-1);
                if (focus_setup_ && std::string_view(focus_setup_) == id) {
                    ImGui::SetKeyboardFocusHere(); focus_setup_ = nullptr;
                }
                ImGui::PushStyleColor(ImGuiCol_Border, interim_color(ColorRole::input_border, dark));
                const bool edited = ImGui::InputText(id, buffer.data(), buffer.size());
                if (semantics) {
                    auto& node = semantics->item(id, label, accessibility::Kind::edit);
                    semantics->edit(node, buffer.data(), buffer.size(), accessibility::TextInput::plain, edited);
                }
                ImGui::PopStyleColor();
                ImGui::Dummy({0, spacing::sm * scale});
            };
            const auto selection = [&](const char* id, const char* label, auto& buffer, bool folder) {
                ContestSemanticScope field_scope(semantics);
                field(id, label, buffer);
                ImGui::PushID(id);
                if (semantics) semantics->prefix += "/picker/" + std::string(id);
                const auto picker_name = std::string(folder ? tr("Choose folder: ", "Chọn thư mục: ") : tr("Choose file: ", "Chọn tệp: ")) + label;
                if (contest_button("choose", folder ? tr("Choose folder…", "Chọn thư mục…") : tr("Choose file…", "Chọn tệp…"), true, semantics, picker_name.c_str())) {
                    bool unavailable = false;
                    if (semantics) semantics->suspend();
                    const auto selected = contest_pick_path(folder, vi, &unavailable);
                    if (unavailable) { unavailable_picker_field_ = id; notice_ = ContestNotice::selection_unavailable; }
                    else if (unavailable_picker_field_ && std::string_view(unavailable_picker_field_) == id) unavailable_picker_field_ = nullptr;
                    if (!selected.empty() && selected.size() < buffer.size()) put(buffer, selected);
                    else if (selected.size() >= buffer.size()) notice_ = ContestNotice::selection_failed;
                }
                ImGui::PopID();
                if (unavailable_picker_field_ && std::string_view(unavailable_picker_field_) == id) {
                    ImGui::TextWrapped("%s", contest_notice_label(ContestNotice::selection_unavailable, vi));
                    if (semantics) semantics->text("unavailable", contest_notice_label(ContestNotice::selection_unavailable, vi));
                }
            };
            int section_index = 0;
            const auto section = [&](const char* label) {
                ImGui::Separator();
                if (heading) ImGui::PushFont(heading, heading->LegacySize * typography::section_title / typography::page_title);
                ImGui::TextWrapped("%s", label);
                if (semantics) semantics->text("section-" + std::to_string(section_index++), label);
                if (heading) ImGui::PopFont();
            };
            const auto disclosure = [&](const char* id, const char* label, bool& open) {
                ContestSemantics::Key key = 0;
                ImGui::SetNextItemOpen(open);
                open = ImGui::CollapsingHeader(label);
                if (semantics) {
                    auto& node = semantics->item(id, label, accessibility::Kind::disclosure); node.expanded = open;
                    key = node.key;
                    semantics->focus(node);
                    if (semantics->request(node, accessibility::Action::expand)) open = true;
                    if (semantics->request(node, accessibility::Action::collapse)) open = false;
                    node.expanded = open;
                }
                return key;
            };
            if (locked_) {
                ImGui::TextWrapped("%s", tr("Round identity and imported pins are fixed; resume this round after a listener restart.",
                    "Danh tính và dấu kiểm vòng thi đã cố định; mở lại cùng vòng khi kết nối khởi động lại."));
                if (semantics) semantics->text("locked-reason", tr("Round identity and imported pins are fixed; resume this round after a listener restart.",
                    "Danh tính và dấu kiểm vòng thi đã cố định; mở lại cùng vòng khi kết nối khởi động lại."));
                ImGui::TextWrapped("%s: %s", tr("Room", "Phòng"), config_->room.room_id.c_str());
                if (semantics) semantics->text("locked-room", (std::string(tr("Room: ", "Phòng: ")) + config_->room.room_id).c_str());
                ImGui::TextWrapped("%s: %s", tr("Round", "Vòng thi"), config_->room.round_id.c_str());
                if (semantics) semantics->text("locked-round", (std::string(tr("Round: ", "Vòng thi: ")) + config_->room.round_id).c_str());
                ImGui::TextWrapped("%s: %s", tr("Public package", "Gói công khai"), config_->room.package_id.c_str());
                if (semantics) semantics->text("locked-package", (std::string(tr("Public package: ", "Gói công khai: ")) + config_->room.package_id).c_str());
            } else {
                ImGui::BeginDisabled(!permitted || busy);
                field("##contest-title", tr("Room title", "Tên phòng"), title_);
                field("##contest-teacher", tr("Teacher display label", "Tên hiển thị giáo viên"), teacher_);
                section(tr("Public package", "Gói công khai"));
                if (material_) ImGui::TextWrapped("%s %s", tr("[+] Registered:", "[+] Đã đăng ký:"), material_->id.c_str());
                else ImGui::TextWrapped("%s", tr("[ ] No verified public package imported.", "[ ] Chưa nhập gói công khai đã kiểm tra."));
                if (semantics) semantics->text("public-status", material_ ? (std::string(tr("[+] Registered: ", "[+] Đã đăng ký: ")) + material_->id).c_str() :
                    tr("[ ] No verified public package imported.", "[ ] Chưa nhập gói công khai đã kiểm tra."));
                selection("##contest-folder", tr("Verified unpacked public package folder", "Thư mục gói công khai đã giải nén và kiểm tra"), public_folder_, true);
                selection("##contest-archive", tr("Public archive (.nstu)", "Gói công khai (.nstu)"), archive_, false);
                const auto public_details = disclosure("public-details", tr("Verification details###public-details", "Chi tiết kiểm tra###public-details"), public_technical_open_);
                if (public_technical_open_) {
                    ContestSemanticScope details_scope(semantics); if (semantics) semantics->parent = public_details;
                    field("##contest-package", tr("Package ID", "Mã gói"), package_id_);
                    field("##contest-content-pin", tr("Reviewed public content SHA256", "SHA256 nội dung công khai đã kiểm tra"), content_pin_);
                    selection("##contest-cache", tr("Private public-package cache root", "Gốc bộ nhớ gói công khai riêng"), packages_root_, true);
                    selection("##contest-extractor", tr("Reviewed public package extractor script", "Tập lệnh giải nén gói công khai đã kiểm tra"), extractor_, false);
                    ImGui::TextWrapped("%s", tr("Keep the reviewed pin and extractor. Selecting a file does not verify or import it.",
                        "Giữ dấu kiểm và tập lệnh đã kiểm tra. Chọn tệp không tự kiểm tra hoặc nhập gói."));
                    if (semantics) semantics->text("pin-guidance", tr("Keep the reviewed pin and extractor. Selecting a file does not verify or import it.",
                        "Giữ dấu kiểm và tập lệnh đã kiểm tra. Chọn tệp không tự kiểm tra hoặc nhập gói."));
                }
                if (contest_button("import-public", tr("Verify and import public package", "Kiểm tra và nhập gói công khai"), true, semantics)) {
                    public_technical_open_ = true;
                    const auto folder = path(public_folder_.data());
                    const std::string id = package_id_.data(), pin = content_pin_.data();
                    material_.reset(); private_digest_.reset();
                    setup_action_ = SetupAction::public_import; notice_ = ContestNotice::pending;
                    (void)launch([this, folder, id, pin] {
                        Update result; result.notice = ContestNotice::import_failed;
#ifdef NSTU_SERVER_HAS_EXAM_INSPECTOR
                        client::ExamPackageReport report; client::ExamLockdownPolicy policy;
                        security::Sha256Digest digest{};
                        if (!folder.is_absolute() || !parse_sha256_hex(pin, digest) ||
                            !client::inspect_exam_package(folder, folder / L"exam" / L"web", pin, true, report) ||
                            !client::extract_exam_lockdown_policy(report.manifest_json, policy) || policy.programming_mode != "contest" ||
                            !broker_.register_package(id, digest, report.manifest_json)) return result;
                        result.material = Material{id, std::move(report.manifest_json), digest, policy.duration_seconds};
                        result.notice = ContestNotice::public_imported;
#else
                        (void)this; (void)folder; (void)id; (void)pin;
#endif
                        return result;
                    });
                }
                section(tr("Private grading (teacher only)", "Bộ chấm riêng (chỉ giáo viên)"));
                ImGui::TextWrapped("%s", tr("Private tests and answers remain with the teacher broker. Never select this bundle as the public archive.",
                    "Bộ kiểm thử và đáp án riêng chỉ ở broker giáo viên. Không chọn bộ chấm này làm gói công khai."));
                if (semantics) semantics->text("private-guidance", tr("Private tests and answers remain with the teacher broker. Never select this bundle as the public archive.",
                    "Bộ kiểm thử và đáp án riêng chỉ ở broker giáo viên. Không chọn bộ chấm này làm gói công khai."));
                selection("##contest-private", tr("Studio private grading bundle (.json)", "Bộ chấm riêng từ Studio (.json)"), private_file_, false);
                if (contest_button("import-private", tr("Import private grading", "Nhập bộ chấm riêng"), material_.has_value(), semantics)) {
                    const auto file = path(private_file_.data()); const auto material = *material_;
                    private_digest_.reset();
                    setup_action_ = SetupAction::private_import; notice_ = ContestNotice::pending;
                    (void)launch([this, file, material] {
                        Update result; result.notice = ContestNotice::import_failed;
                        auto text = read_private(file); if (!text) return result;
                        auto bundle = contest_private_bundle(*text, material.id, material.digest);
                        if (!bundle || !broker_.register_grading(material.id, material.digest, bundle->grading_digest, bundle->problems_json)) return result;
                        result.private_digest = bundle->grading_digest; result.notice = ContestNotice::private_imported;
                        return result;
                    });
                }
                if (!material_) {
                    ImGui::TextWrapped("%s", tr("Verify and import the public package before importing private grading.", "Kiểm tra và nhập gói công khai trước khi nhập bộ chấm riêng."));
                    if (semantics) semantics->text("private-prerequisite", tr("Verify and import the public package before importing private grading.", "Kiểm tra và nhập gói công khai trước khi nhập bộ chấm riêng."));
                }
                if (private_digest_) {
                    ImGui::TextWrapped("%s", tr("[+] Private grading registered", "[+] Đã đăng ký bộ chấm riêng"));
                    if (semantics) semantics->text("private-status", tr("[+] Private grading registered", "[+] Đã đăng ký bộ chấm riêng"));
                }
                wrapped_checkbox("contest-publication", tr("Publish aggregate standings to students", "Công bố bảng xếp hạng tổng hợp cho thí sinh"), &live_standings_);
                if (semantics) {
                    // EndGroup replaces LastItemData; retain the house checkbox's native focus ID.
                    const auto checkbox_id = ImHashStr("##value", 0, ImGui::GetID("contest-publication"));
                    auto& node = semantics->item("publication", tr("Publish aggregate standings to students", "Công bố bảng xếp hạng tổng hợp cho thí sinh"), accessibility::Kind::checkbox, checkbox_id);
                    node.checked = live_standings_; semantics->focus(node);
                    if (const auto command = semantics->request(node, accessibility::Action::toggle)) live_standings_ = command->target_checked;
                    node.checked = live_standings_;
                }
                section(tr("Opening checks", "Điều kiện mở phòng"));
                const char* opening_reason = !material_ ? tr("[!] Public package verification is required.", "[!] Cần kiểm tra gói công khai.")
                    : !private_digest_ ? tr("[!] Import private grading for this public package.", "[!] Nhập bộ chấm riêng cho đúng gói công khai.")
                    : !broker_.configured() ? tr("[!] Configure the teacher broker environment before importing or opening a room.",
                        "[!] Cấu hình môi trường broker giáo viên trước khi nhập hoặc mở phòng.")
                    : !contest_room_port(room_port_) ? tr("[!] Use a port from 1 to 65535.", "[!] Chọn cổng từ 1 đến 65535.")
                    : tr("Public and private material is registered. The listener still validates paths, identity, pins and broker state.",
                        "Tài liệu công khai và riêng đã đăng ký. Kết nối phòng vẫn kiểm tra đường dẫn, danh tính, dấu kiểm và trạng thái broker.");
                ImGui::TextWrapped("%s", opening_reason);
                if (semantics) semantics->text("opening-reason", opening_reason);
                const auto room_details = disclosure("room-details", tr("Room details###room-details", "Chi tiết phòng###room-details"), technical_open_);
                if (technical_open_) {
                    ContestSemanticScope details_scope(semantics); if (semantics) semantics->parent = room_details;
                    field("##contest-owner", tr("Broker owner ID", "Mã chủ phòng trên broker"), owner_);
                    field("##contest-room", tr("Room ID", "Mã phòng"), room_id_);
                    field("##contest-round", tr("Round ID", "Mã vòng thi"), round_id_);
                    ImGui::TextWrapped("%s", tr("Room port (TCP and UDP)", "Cổng phòng (TCP và UDP)"));
                    ImGui::SetNextItemWidth(-1);
                    if (focus_setup_ && std::string_view(focus_setup_) == "##contest-port") { ImGui::SetKeyboardFocusHere(); focus_setup_ = nullptr; }
                    ImGui::PushStyleColor(ImGuiCol_Border, interim_color(ColorRole::input_border, dark));
                    const bool edited = ImGui::InputInt("##contest-port", &room_port_, 0, 0);
                    if (semantics) {
                        std::array<char, 64> value{}; put(value, std::to_string(room_port_));
                        auto& node = semantics->item("##contest-port", tr("Room port (TCP and UDP)", "Cổng phòng (TCP và UDP)"), accessibility::Kind::edit);
                        semantics->edit(node, value.data(), value.size(), accessibility::TextInput::integer, edited);
                        int updated = 0; const auto end = value.data() + std::char_traits<char>::length(value.data());
                        const auto begin = value[0] == '+' ? value.data() + 1 : value.data();
                        const auto parsed = std::from_chars(begin, end, updated);
                        if (parsed.ec == std::errc{} && parsed.ptr == end) room_port_ = updated;
                    }
                    ImGui::PopStyleColor();
                    if (!contest_room_port(room_port_)) {
                        ImGui::TextWrapped("%s", tr("Use a port from 1 to 65535.", "Chọn cổng từ 1 đến 65535."));
                        if (semantics) semantics->text("port-error", tr("Use a port from 1 to 65535.", "Chọn cổng từ 1 đến 65535."));
                    }
                    selection("##contest-profile", tr("Private per-user profile root (absolute, existing)", "Gốc hồ sơ riêng của người dùng (đường dẫn tuyệt đối đã có)"), profile_root_, true);
                }
                ImGui::EndDisabled();
                if (contest_button("review-details", tr("Review required details", "Xem chi tiết bắt buộc"), true, semantics)) show_setup_error();
            }
        } else if (active_tab_ == 1) {
            if (const auto action = draw_contest_admission(snapshot, role, vi, sas, comparison_, dark, semantics)) {
                const auto latest = listener_.snapshot();
                const auto target = std::find_if(latest.pending.begin(), latest.pending.end(), [&](const auto& peer) {
                    return peer.peer_id == action->peer.peer_id && peer.peer_user_uuid == action->peer.peer_user_uuid && peer.sas == action->peer.sas;
                });
                if (target == latest.pending.end() || !permitted) notice_ = ContestNotice::rejected;
                else if (action->kind == ContestAdmissionKind::approve)
                    notice_ = listener_.approve(target->peer_id, action->sas) ? ContestNotice::queued : ContestNotice::rejected;
                else notice_ = listener_.reject(target->peer_id) ? ContestNotice::queued : ContestNotice::rejected;
            }
        } else if (active_tab_ == 2) {
            if (auto target = draw_contest_participants(snapshot, role, vi, scale, compact, &return_member_focus_, semantics)) removal_ = std::move(target);
        } else draw_contest_standings(standings_, standings_checked_, now, vi, scale, compact, semantics);
    }
    ImGui::EndChild(); ImGui::PopStyleVar(2);

    if (removal_ || finishing_) {
        if (!confirmation_visible_) ++confirmation_generation_;
        confirmation_visible_ = true; ImGui::OpenPopup("contest-confirm-action");
    } else confirmation_visible_ = false;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize({std::min(geometry::dialog_width * scale, viewport->WorkSize.x - spacing::sm * 2.0f * scale),
        std::min(layout::minimum_client_height * scale, viewport->WorkSize.y - spacing::sm * 2.0f * scale)});
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("contest-confirm-action", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        ContestSemanticScope confirmation_scope(semantics);
        ContestSemantics::Key dialog = 0;
        if (semantics) {
            semantics->parent = 0; semantics->scroll_owner = 0; semantics->generation = confirmation_generation_;
            semantics->prefix += "/confirmation";
            auto& root = semantics->pane("dialog", removal_ ? tr("Remove participant", "Loại thí sinh") : tr("Finish round", "Kết thúc vòng thi"), accessibility::Kind::pane, ImGui::GetCurrentWindow()->InnerRect);
            dialog = root.key; semantics->parent = dialog;
        }
        const auto current = listener_.snapshot();
        const bool available = removal_ ? contest_removal_target(current, *removal_, role) != nullptr
            : config_ && contest_finish_available(current, *finishing_, role);
        const float footer = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        if (ImGui::BeginChild("confirmation-details", {0, -footer})) {
            if (semantics) {
                auto& content = semantics->pane("details", tr("Confirmation details", "Chi tiết xác nhận"), accessibility::Kind::pane, ImGui::GetCurrentWindow()->InnerRect);
                content.scrollable = true; semantics->parent = content.key; semantics->scroll_owner = content.key;
            }
            ImGui::TextWrapped("%s", removal_ ? tr("Remove participant?", "Loại thí sinh?") : tr("Finish this round?", "Kết thúc vòng thi này?"));
            ImGui::TextWrapped("%s: %s", removal_ ? tr("Display name (unverified)", "Tên hiển thị (chưa xác minh)") : tr("Round", "Vòng thi"),
                removal_ ? removal_->display_name.c_str() : finishing_->round_id.c_str());
            if (semantics) semantics->text("target", (std::string(removal_ ? tr("Display name (unverified): ", "Tên hiển thị (chưa xác minh): ") : tr("Round: ", "Vòng thi: ")) +
                (removal_ ? removal_->display_name : finishing_->round_id)).c_str());
            ImGui::TextWrapped("%s", removal_ ? tr("This removes the named participant from the current round.",
                "Thao tác này loại thí sinh có tên trên khỏi vòng thi hiện tại.")
                : tr("This closes the named round. Its identity and results remain retained.",
                    "Thao tác này kết thúc vòng thi có mã trên. Danh tính và kết quả vẫn được giữ."));
            if (semantics) semantics->text("consequence", removal_ ? tr("This removes the named participant from the current round.", "Thao tác này loại thí sinh có tên trên khỏi vòng thi hiện tại.") :
                tr("This closes the named round. Its identity and results remain retained.", "Thao tác này kết thúc vòng thi có mã trên. Danh tính và kết quả vẫn được giữ."));
            if (!available) {
                ImGui::TextWrapped("%s", tr("[!] Target or room state changed; cancel and review the current state.", "[!] Thí sinh hoặc trạng thái phòng đã thay đổi; hủy và kiểm tra lại."));
                if (semantics) semantics->text("unavailable", tr("[!] Target or room state changed; cancel and review the current state.", "[!] Thí sinh hoặc trạng thái phòng đã thay đổi; hủy và kiểm tra lại."));
            }
        }
        ImGui::EndChild();
        if (semantics) { semantics->parent = dialog; semantics->scroll_owner = 0; }
        if (contest_button("confirm-removal-finish", tr("Confirm", "Xác nhận"), available, semantics)) {
            const auto latest = listener_.snapshot();
            if (removal_) {
                if (const auto* target = contest_removal_target(latest, *removal_, role))
                    notice_ = listener_.remove(target->context) ? ContestNotice::queued : ContestNotice::rejected;
                else notice_ = ContestNotice::rejected;
            } else if (config_ && contest_finish_available(latest, *finishing_, role)) {
                notice_ = listener_.finish("teacher-finish-" + finishing_->round_id) ? ContestNotice::queued : ContestNotice::rejected;
                readiness_.available = false;
            } else notice_ = ContestNotice::rejected;
            return_member_focus_ = removal_ ? contest_member_ui_id(removal_->context) : 0;
            removal_.reset(); finishing_.reset(); ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (contest_button("cancel-removal-finish", tr("Cancel", "Hủy"), true, semantics) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            return_member_focus_ = removal_ ? contest_member_ui_id(removal_->context) : 0;
            removal_.reset(); finishing_.reset(); ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace nstu::ui
