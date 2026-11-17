#pragma once

#include "nstu/contest_discovery.hpp"
#include "nstu/contest_package.hpp"
#include "nstu/exam_host.hpp"

namespace nstu::client {

enum class StudentPhase { idle, connecting, confirm_teacher, awaiting_admission, approved,
                          receiving, prepared, running, closed, disconnected, failed };
enum class StudentRoomPhase { draft, joining, prepared, running, closed };
struct StudentLifecycle {
    StudentRoomPhase phase = StudentRoomPhase::draft;
    std::uint64_t revision = 0, server_time = 0, server_start = 0, deadline = 0;
    [[nodiscard]] bool operator==(const StudentLifecycle&) const = default;
};
struct StudentApproval {
    exam::StateRequest context;
    bool recovery_only = false;
    bool live_standings = false;
    std::uint64_t revision = 0, server_time = 0;
};
struct StudentStandingRow {
    std::uint32_t rank = 0, solved = 0;
    std::uint64_t penalty_minutes = 0;
    std::string display_name;
};
struct StudentStandings {
    std::uint64_t revision = 0, generated_time = 0;
    std::string freshness;
    std::vector<StudentStandingRow> rows;
};
[[nodiscard]] std::optional<StudentApproval> parse_student_approval(std::string_view json);
[[nodiscard]] std::optional<StudentLifecycle> parse_student_lifecycle(std::string_view json);
[[nodiscard]] bool student_lifecycle_extends(const StudentLifecycle& previous,
                                            const StudentLifecycle& next) noexcept;
[[nodiscard]] std::optional<StudentStandings> parse_student_standings(
    std::string_view json, const contest_room::Realm& realm);
[[nodiscard]] bool student_capture_preserved(const ExamCloseCaptureReport& report,
    const exam::StateRequest& context, std::span<const exam::programming::Request> durable,
    const std::map<std::string, exam::programming::Request>& pending);

struct StudentRememberedRoom {
    contest_room::Realm realm;
    contest_discovery::Endpoint endpoint;
    std::string display_label;
};
[[nodiscard]] std::vector<std::byte> encode_student_remembered_rooms(
    std::span<const StudentRememberedRoom> rooms);
[[nodiscard]] std::optional<std::vector<StudentRememberedRoom>> decode_student_remembered_rooms(
    std::span<const std::byte> wire);
// Worker-only storage read. Entries are selectable hints; authentication still
// loads the independent realm-bound identity and requires its exact membership.
[[nodiscard]] std::optional<std::vector<StudentRememberedRoom>> load_student_remembered_rooms(
    const std::filesystem::path& user_root, std::string* error = nullptr);

// Deterministic tests only; production leaves this empty. It grants no DPAPI,
// protected-filesystem, extraction, real-judge or hardware qualification.
struct StudentMemoryFixture {
    std::function<std::optional<std::vector<std::byte>>()> load_profile;
    std::function<bool(std::span<const std::byte>)> save_profile;
    std::function<std::vector<exam::programming::Request>()> load_requests;
    std::function<bool(const exam::programming::Request&)> save_request;
    std::function<std::optional<ExamPackageReport>(const contest_package::PackageOffer&,
                                                std::span<const std::byte>)> verify_package;
    std::function<std::optional<std::vector<std::byte>>()> load_rooms;
    std::function<bool(std::span<const std::byte>)> save_rooms;
};
struct ContestStudentConfig {
    contest_room::Realm realm;
    contest_discovery::Endpoint endpoint;
    std::string display_name;
    std::string room_label;
    // Trusted native owner paths; none comes from candidate packets or JS.
    std::filesystem::path user_root, powershell_executable, extractor_script;
    security::Sha256Digest extractor_digest{}, powershell_digest{};
    std::chrono::milliseconds pairing_timeout{30000};
    std::optional<StudentMemoryFixture> memory_fixture;
};
struct ContestStudentSnapshot {
    StudentPhase phase = StudentPhase::idle;
    bool connected = false, recovery_only = false, package_verified = false, live_standings = false;
    std::string sas, error;
    std::optional<exam::StateRequest> context;
    StudentLifecycle lifecycle;
    std::optional<ExamPackageReport> package;
    std::optional<StudentStandings> standings;
    std::uint64_t received_bytes = 0, archive_bytes = 0;
    // Monotonic estimate anchored to authenticated server milliseconds.
    std::uint64_t server_time = 0;
    std::size_t durable_requests = 0, queued_requests = 0;
};

class ContestStudentSession {
public:
    ContestStudentSession();
    ~ContestStudentSession();
    ContestStudentSession(const ContestStudentSession&) = delete;
    ContestStudentSession& operator=(const ContestStudentSession&) = delete;
    // Owner serializes start/stop; worker owns all socket/storage work.
    [[nodiscard]] bool start(ContestStudentConfig config, std::string* error = nullptr);
    void stop() noexcept;
    [[nodiscard]] bool confirm_sas(std::string_view sas);
    void cancel_pairing() noexcept;
    [[nodiscard]] bool reconnect();
    void disconnect() noexcept;
    [[nodiscard]] bool queue_programming(const exam::programming::Request& request);
    [[nodiscard]] bool request_standings();
    [[nodiscard]] ContestStudentSnapshot snapshot() const;
    [[nodiscard]] std::vector<exam::programming::Response> take_replies();
    [[nodiscard]] std::optional<ExamHostOptions> host_options() const;
    [[nodiscard]] bool preserve_capture(const ExamCloseCaptureReport& report,
        const std::map<std::string, exam::programming::Request>& pending) const;
    // Session must outlive its host. Closures hold a weak owner and fail closed
    // after stop/destruction; every callback is bounded and performs no I/O.
    [[nodiscard]] ExamHostCallbacks host_callbacks();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace nstu::client
