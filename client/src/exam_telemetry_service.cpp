#include "nstu/exam_telemetry_service.hpp"
#include "nstu/auth.hpp"
#include "nstu/secret_store.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <limits>
#include <mutex>
#include <utility>

namespace nstu::client {

namespace {

constexpr std::uint32_t kCoordinatorStateMagic = 0x544c4d53; // "TLMS"
constexpr std::uint16_t kCoordinatorStateVersion = 3;
constexpr char kCoordinatorEntropyText[] = "NSTU-Exam-Telemetry-Service-Coordinator-v1";

inline void set_error(std::string* error, std::string_view message) {
    if (error != nullptr && error->empty()) {
        *error = message;
    }
}

inline std::span<const std::byte> coordinator_entropy() noexcept {
    return {reinterpret_cast<const std::byte*>(kCoordinatorEntropyText),
            sizeof(kCoordinatorEntropyText) - 1};
}

inline void append_u16_le(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
}

inline void append_u32_le(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 16u) & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 24u) & 0xffu));
}

inline void append_u64_le(std::vector<std::byte>& out, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (i * 8u)) & 0xffu));
    }
}

inline void append_string(std::vector<std::byte>& out, std::string_view str) {
    append_u32_le(out, static_cast<std::uint32_t>(str.size()));
    for (char c : str) {
        out.push_back(static_cast<std::byte>(c));
    }
}

inline bool read_u16_le(std::span<const std::byte> in, std::size_t& offset, std::uint16_t& out) {
    if (offset + 2 > in.size()) return false;
    out = static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(in[offset]) |
        (static_cast<std::uint8_t>(in[offset + 1]) << 8u));
    offset += 2;
    return true;
}

inline bool read_u32_le(std::span<const std::byte> in, std::size_t& offset, std::uint32_t& out) {
    if (offset + 4 > in.size()) return false;
    out = static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(in[offset]) |
        (static_cast<std::uint8_t>(in[offset + 1]) << 8u) |
        (static_cast<std::uint8_t>(in[offset + 2]) << 16u) |
        (static_cast<std::uint8_t>(in[offset + 3]) << 24u));
    offset += 4;
    return true;
}

inline bool read_u64_le(std::span<const std::byte> in, std::size_t& offset, std::uint64_t& out) {
    if (offset + 8 > in.size()) return false;
    out = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        out |= (static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[offset + i])) << (i * 8u));
    }
    offset += 8;
    return true;
}

inline bool read_string(std::span<const std::byte> in, std::size_t& offset, std::string& out) {
    std::uint32_t len = 0;
    if (!read_u32_le(in, offset, len)) return false;
    if (offset + len > in.size()) return false;
    out.assign(reinterpret_cast<const char*>(in.data() + offset), len);
    offset += len;
    return true;
}

void append_identity(std::vector<std::byte>& out, const TelemetryContextIdentity& context) {
    append_string(out, context.package_id);
    out.insert(out.end(), context.package_digest.begin(), context.package_digest.end());
    out.insert(out.end(), context.client_id.begin(), context.client_id.end());
    out.insert(out.end(), context.session_id.begin(), context.session_id.end());
    append_string(out, context.candidate_id);
}

bool read_identity(std::span<const std::byte> in, std::size_t& offset,
                   TelemetryContextIdentity& context) {
    const auto read_bytes = [&](auto& bytes) {
        if (bytes.size() > in.size() - offset) return false;
        std::copy_n(in.data() + offset, bytes.size(), bytes.begin());
        offset += bytes.size();
        return true;
    };
    return read_string(in, offset, context.package_id) &&
           read_bytes(context.package_digest) && read_bytes(context.client_id) &&
           read_bytes(context.session_id) && read_string(in, offset, context.candidate_id);
}

bool valid_identity(const TelemetryContextIdentity& context) {
    exam::TelemetryEnable enable;
    enable.package_id = context.package_id;
    enable.package_digest = context.package_digest;
    enable.client_id = context.client_id;
    enable.session_id = context.session_id;
    enable.candidate_id = context.candidate_id;
    return exam::validate_telemetry_enable(enable);
}

void add_saturated(std::uint64_t& value, std::uint64_t amount) noexcept {
    value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}

std::uint64_t current_unix_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

bool write_atomic(const std::filesystem::path& path,
                  std::span<const std::byte> bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    const auto temp_path = path.wstring() + L".tmp-" +
                           std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
    HANDLE h_file = CreateFileW(
        temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h_file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL write_ok = WriteFile(h_file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const BOOL flush_ok = FlushFileBuffers(h_file);
    CloseHandle(h_file);

    if (!write_ok || !flush_ok || written != bytes.size()) {
        DeleteFileW(temp_path.c_str());
        return false;
    }

    if (!MoveFileExW(temp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp_path.c_str());
        return false;
    }
#else
    {
        std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) return false;
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream.good()) {
            std::filesystem::remove(temp_path, ec);
            return false;
        }
    }
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return false;
    }
#endif
    return true;
}

std::vector<std::byte> read_all(const std::filesystem::path& path) {
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size == 0 || file_size > 1024 * 1024) return {};

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) return {};

    std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));
    stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(file_size));
    if (!stream.good() && file_size > 0) return {};
    return buffer;
}

bool clean_owned_temps(const std::filesystem::path& store_path, bool include_blocked_temps) {
    if (store_path.empty()) return true;
    std::error_code ec;
    const auto parent = store_path.parent_path();
    if (!std::filesystem::exists(parent, ec)) return !ec;
    const auto base = store_path.filename().wstring();
    const std::array prefixes{base + L".tmp-", base + L".blocked.tmp-"};
    const auto prefix_count = include_blocked_temps ? prefixes.size() : std::size_t{1};
    std::filesystem::directory_iterator it(parent, ec), end;
    while (!ec && it != end) {
        const auto name = it->path().filename().wstring();
        for (std::size_t i = 0; i < prefix_count; ++i) {
            const auto& prefix = prefixes[i];
            if (name.rfind(prefix, 0) != 0 || name.size() == prefix.size()) continue;
            const auto suffix = std::wstring_view(name).substr(prefix.size());
            if (!std::all_of(suffix.begin(), suffix.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
            const auto status = it->symlink_status(ec);
            if (!ec && std::filesystem::is_regular_file(status)) {
#ifdef _WIN32
                const auto attributes = GetFileAttributesW(it->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) return false;
                if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) break;
#endif
                std::filesystem::remove(it->path(), ec);
            }
            break;
        }
        if (!ec) it.increment(ec);
    }
    return !ec;
}

} // namespace

struct ExamTelemetryServiceCoordinator::Impl {
    explicit Impl(Clock local_clock) : clock(std::move(local_clock)) {}

    Clock clock;
    std::uint64_t current_time_ms() const { return clock(); }
    mutable std::mutex mutex;
    bool is_open = false;
    std::filesystem::path outbox_path;
    std::filesystem::path state_path;

    exam::TelemetryOutbox outbox;
    ExamTelemetryLifecycle lifecycle;

    TelemetryContextIdentity context{};
    exam::ProducerId active_producer_id{};
    std::uint64_t next_seq = 1;
    std::uint32_t gen_count = 0;
    std::uint64_t dropped_samples = 0;
    std::uint64_t dropped_batches = 0;
    std::uint64_t not_authorized_samples = 0;
    std::uint64_t loss_first_recorded_ms = 0;
    std::optional<exam::TelemetryBatch> pending_batch;
    std::uint64_t pending_started_ms = 0;
    bool state_dirty = false;
    bool cleanup_pending = false;
    std::string last_error;

    void expire_loss_locked(std::uint64_t now) {
        if (now == 0) now = current_time_ms();
        if (loss_first_recorded_ms != 0 && now >= loss_first_recorded_ms &&
            now - loss_first_recorded_ms >= exam::kTelemetryRetentionMilliseconds) {
            dropped_samples = 0;
            dropped_batches = 0;
            not_authorized_samples = 0;
            loss_first_recorded_ms = 0;
            state_dirty = true;
        }
    }

    void touch_loss(std::uint64_t now) {
        expire_loss_locked(now);
        if (loss_first_recorded_ms == 0) {
            const auto local_now = current_time_ms();
            loss_first_recorded_ms = now == 0 ? local_now : std::min(now, local_now);
        }
    }

    bool fail(std::string_view message, std::string* error = nullptr) {
        if (last_error != message) {
            // Static, bounded operational messages only; no identity or payload.
            std::fputs("NSTU telemetry coordinator: ", stderr);
            std::fwrite(message.data(), 1, message.size(), stderr);
            std::fputc('\n', stderr);
#ifdef _WIN32
            OutputDebugStringA(("NSTU telemetry coordinator: " + std::string(message) + "\n").c_str());
#endif
        }
        last_error = message;
        set_error(error, message);
        return false;
    }

    bool close_locked() {
        if (!is_open) return true;
        if (!flush_locked()) return false;
        outbox.close();
        is_open = false;
        return true;
    }

    bool generate_new_producer() {
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            exam::ProducerId candidate{};
            if (security::generate_random(candidate) &&
                std::any_of(candidate.begin(), candidate.end(), [](std::byte b) { return b != std::byte{0}; })) {
                active_producer_id = candidate;
                next_seq = 1;
                ++gen_count;
                return true;
            }
        }
        return fail("producer randomness unavailable");
    }

    bool persist_state_locked() {
        if (!is_open || state_path.empty()) return true;

        std::vector<std::byte> plaintext;
        plaintext.reserve(256);

        append_u32_le(plaintext, kCoordinatorStateMagic);
        append_u16_le(plaintext, kCoordinatorStateVersion);
        append_u16_le(plaintext, 0); // reserved

        append_identity(plaintext, context);

        plaintext.insert(plaintext.end(), active_producer_id.begin(), active_producer_id.end());
        append_u64_le(plaintext, next_seq);
        append_u32_le(plaintext, gen_count);
        plaintext.push_back(static_cast<std::byte>(lifecycle.state()));

        append_u64_le(plaintext, dropped_samples);
        append_u64_le(plaintext, dropped_batches);
        append_u64_le(plaintext, not_authorized_samples);
        append_u64_le(plaintext, loss_first_recorded_ms);
        const auto revoked = lifecycle.revoked_identities();
        append_u32_le(plaintext, static_cast<std::uint32_t>(revoked.size()));
        for (const auto& identity : revoked) append_identity(plaintext, identity);
        const auto pending_bytes = pending_batch ? exam::encode_telemetry_batch(*pending_batch)
                                                : std::vector<std::byte>{};
        if (pending_batch && pending_bytes.empty()) return false;
        append_u64_le(plaintext, pending_started_ms);
        append_u32_le(plaintext, static_cast<std::uint32_t>(pending_bytes.size()));
        plaintext.insert(plaintext.end(), pending_bytes.begin(), pending_bytes.end());

        const auto digest = security::sha256(plaintext);
        if (!digest) return false;
        plaintext.insert(plaintext.end(), digest->begin(), digest->end());

        auto protected_blob = security::protect_machine_secret(plaintext, coordinator_entropy());
        if (protected_blob.empty()) {
            return false;
        }

        if (!write_atomic(state_path, protected_blob)) return false;
        state_dirty = false;
        return true;
    }

    bool persist_revocation_locked(std::string* error, std::uint64_t now) {
        // Every revocation uses the same durable fallback before payload cleanup.
        if (!persist_state_locked()) {
            const std::array<std::byte, 1> marker{std::byte{1}};
            if (!write_atomic(state_path.wstring() + L".blocked", marker)) {
                fail("revocation state and marker persistence failed", error);
            } else {
                fail("revocation state persistence failed; durable marker blocks restart", error);
            }
            if (!outbox.purge()) {
                fail("revocation persistence and payload cleanup failed; restart blocked when marker is durable", error);
            }
            return false;
        }
        return flush_locked(error, now);
    }

    bool pending_has_expired(std::uint64_t now) const noexcept {
        if (!pending_batch) return false;
        const auto stamp = std::min(pending_started_ms, pending_batch->first_enqueued_at_unix_milliseconds);
        return stamp == 0 || (now >= stamp && now - stamp >= exam::kTelemetryRetentionMilliseconds);
    }

    bool revoke_pending_locked(std::string_view reason, std::string* error, std::uint64_t now) {
        if (pending_batch) {
            touch_loss(now);
            add_saturated(dropped_samples, pending_batch->samples.size());
            add_saturated(dropped_batches, 1);
        }
        pending_batch.reset();
        pending_started_ms = 0;
        (void)lifecycle.on_explicit_stop();
        cleanup_pending = true;
        state_dirty = true;
        (void)persist_revocation_locked(error, now);
        return fail(reason, error);
    }

    bool finish_pending_locked(std::string* error = nullptr, std::uint64_t now = 0) {
        if (!pending_batch) return true;
        if (now == 0) now = current_time_ms();
        const auto records = outbox.pending_batches();
        const auto existing = std::find_if(records.begin(), records.end(), [&](const auto& batch) {
            return batch.producer_id == pending_batch->producer_id && batch.sequence == pending_batch->sequence;
        });
        if (existing != records.end()) {
            if (exam::encode_telemetry_batch(*existing) != exam::encode_telemetry_batch(*pending_batch)) {
                return fail("pending telemetry conflicts with durable batch", error);
            }
        } else {
            if (outbox.aggregate_loss(pending_batch->session_id).expired_batches != 0) {
                // Open may have just retired this exact durable record by its
                // earlier arrival anchor. An empty outbox is not proof of a new intent.
                return revoke_pending_locked("pending telemetry age is uncertain after expiry; context revoked", error, now);
            }
            if (!outbox.enqueue(*pending_batch, error)) {
                return fail("pending telemetry enqueue persistence failed", error);
            }
        }
        ++next_seq;
        pending_batch.reset();
        pending_started_ms = 0;
        state_dirty = true;
        if (!persist_state_locked()) return fail("pending telemetry completion persistence failed", error);
        return true;
    }

    bool flush_locked(std::string* error = nullptr, std::uint64_t now = 0) {
        if (now == 0) now = current_time_ms();
        expire_loss_locked(now);
        if (pending_has_expired(now)) {
            return revoke_pending_locked("pending telemetry expired; context revoked", error, now);
        }
        if (state_dirty && !persist_state_locked()) {
            return fail("coordinator state persistence failed", error);
        }
        if (cleanup_pending) {
            if (!outbox.is_open()) {
                // A failed purge latches the old instance closed. A fresh instance
                // follows its durable revoked marker without ever authorizing replay.
                outbox = exam::TelemetryOutbox{};
                if (!outbox.open(outbox_path, error)) return fail("revoked payload cleanup reopen failed", error);
            }
            if (!outbox.purge(error)) return fail("revoked payload cleanup failed", error);
            std::error_code ec;
            std::filesystem::remove(state_path.wstring() + L".blocked", ec);
            if (ec) return fail("revocation marker cleanup failed", error);
            cleanup_pending = false;
        }
        return finish_pending_locked(error, now);
    }

    bool maintain_locked(std::uint64_t now, std::string* error, std::size_t* pruned = nullptr) {
        if (now == 0) now = current_time_ms();
        if (!flush_locked(error, now)) return false;
        std::string prune_error;
        const auto count = outbox.prune_expired(now, &prune_error);
        if (!prune_error.empty()) return fail("telemetry expiry persistence failed", error);
        if (pruned != nullptr) *pruned = count;
        return true;
    }

    bool load_state_locked() {
        if (state_path.empty()) return false;
        std::error_code ec;
        if (!std::filesystem::exists(state_path, ec)) return false;

        const auto stored = read_all(state_path);
        if (stored.empty()) return false;

        auto plaintext = security::unprotect_machine_secret(stored, coordinator_entropy());
        if (plaintext.size() < 32 + 32) return false;

        // Verify SHA-256
        const auto payload_span = std::span<const std::byte>(plaintext.data(), plaintext.size() - 32);
        const auto expected_digest = security::sha256(payload_span);
        const auto stored_digest = std::span<const std::byte>(plaintext.data() + plaintext.size() - 32, 32);
        if (!expected_digest.has_value() ||
            !std::equal(expected_digest->begin(), expected_digest->end(), stored_digest.begin())) {
            return false;
        }

        std::size_t offset = 0;
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t reserved = 0;

        if (!read_u32_le(payload_span, offset, magic) || magic != kCoordinatorStateMagic ||
            !read_u16_le(payload_span, offset, version) || (version != 2 && version != kCoordinatorStateVersion) ||
            !read_u16_le(payload_span, offset, reserved) || reserved != 0) {
            return false;
        }

        TelemetryContextIdentity loaded_ctx;
        exam::ProducerId loaded_producer{};
        std::uint64_t loaded_seq = 0, loaded_drops = 0, loaded_batch_drops = 0, loaded_unauthorized = 0, loaded_loss_first = 0;
        std::uint32_t loaded_generations = 0;
        if (!read_identity(payload_span, offset, loaded_ctx)) return false;
        if (loaded_producer.size() > payload_span.size() - offset) return false;
        std::copy_n(payload_span.data() + offset, loaded_producer.size(), loaded_producer.begin());
        offset += loaded_producer.size();
        if (!read_u64_le(payload_span, offset, loaded_seq) ||
            !read_u32_le(payload_span, offset, loaded_generations)) return false;
        if (offset >= payload_span.size()) return false;
        const auto raw_state = static_cast<std::uint8_t>(payload_span[offset++]);
        if (raw_state > static_cast<std::uint8_t>(TelemetryLifecycleState::revoked)) return false;
        const auto restored_state = static_cast<TelemetryLifecycleState>(raw_state);
        if (!read_u64_le(payload_span, offset, loaded_drops) ||
            !read_u64_le(payload_span, offset, loaded_batch_drops) ||
            !read_u64_le(payload_span, offset, loaded_unauthorized) ||
            !read_u64_le(payload_span, offset, loaded_loss_first)) return false;
        if (loaded_loss_first == 0 && (loaded_drops != 0 || loaded_batch_drops != 0 || loaded_unauthorized != 0)) return false;

        const bool producer_nonzero = std::any_of(loaded_producer.begin(), loaded_producer.end(),
            [](std::byte b) { return b != std::byte{0}; });
        if (loaded_ctx.empty()) {
            if (loaded_ctx != TelemetryContextIdentity{} || producer_nonzero || loaded_seq != 1 ||
                loaded_generations != 0 || restored_state != TelemetryLifecycleState::disabled) return false;
        } else if (!valid_identity(loaded_ctx) || !producer_nonzero || loaded_seq == 0 ||
                   loaded_generations == 0 || loaded_generations > kMaximumLifetimeProducerGenerations) {
            return false;
        }

        std::uint32_t revoked_count = 0;
        if (!read_u32_le(payload_span, offset, revoked_count) ||
            revoked_count > kMaximumRevokedTelemetryIdentities) return false;
        ExamTelemetryLifecycle restored;
        for (std::uint32_t i = 0; i < revoked_count; ++i) {
            TelemetryContextIdentity identity;
            if (!read_identity(payload_span, offset, identity) || !valid_identity(identity) ||
                restored.is_identity_revoked(identity) || !restored.set_context(identity)) return false;
            (void)restored.on_explicit_stop();
        }
        // Binding the saved current identity archives the last replayed revocation.
        if (!restored.set_context(loaded_ctx)) return false;
        if (restored.is_identity_revoked(loaded_ctx) && restored_state != TelemetryLifecycleState::revoked) return false;
        (void)restored.on_service_restart(restored_state);

        std::uint32_t pending_size = 0;
        std::uint64_t loaded_pending_started = 0;
        std::optional<exam::TelemetryBatch> loaded_pending;
        if (version == kCoordinatorStateVersion && !read_u64_le(payload_span, offset, loaded_pending_started)) return false;
        if (!read_u32_le(payload_span, offset, pending_size) ||
            pending_size > exam::kMaximumTelemetryBatchBytes || pending_size != payload_span.size() - offset) return false;
        if (pending_size != 0) {
            // v2 pending data never persisted a service-owned age anchor.
            if (version == 2 || loaded_pending_started == 0 || loaded_pending_started > current_time_ms()) return false;
            loaded_pending = exam::decode_telemetry_batch(payload_span.subspan(offset, pending_size));
            if (!loaded_pending || !loaded_ctx.matches(*loaded_pending) ||
                loaded_pending->producer_id != loaded_producer || loaded_pending->sequence != loaded_seq ||
                restored_state == TelemetryLifecycleState::revoked || loaded_seq == std::numeric_limits<std::uint64_t>::max()) return false;
        } else if (loaded_pending_started != 0) return false;
        context = std::move(loaded_ctx);
        lifecycle = std::move(restored);
        active_producer_id = loaded_producer;
        next_seq = loaded_seq;
        gen_count = loaded_generations;
        dropped_samples = loaded_drops;
        dropped_batches = loaded_batch_drops;
        not_authorized_samples = loaded_unauthorized;
        loss_first_recorded_ms = std::min(loaded_loss_first, current_time_ms());
        pending_batch = std::move(loaded_pending);
        pending_started_ms = loaded_pending_started;
        state_dirty = loss_first_recorded_ms != loaded_loss_first || version != kCoordinatorStateVersion;
        return true;
    }
};

ExamTelemetryServiceCoordinator::ExamTelemetryServiceCoordinator()
    : ExamTelemetryServiceCoordinator(current_unix_ms) {}
ExamTelemetryServiceCoordinator::ExamTelemetryServiceCoordinator(Clock clock)
    : impl_(std::make_unique<Impl>(clock ? std::move(clock) : Clock(current_unix_ms))) {}
ExamTelemetryServiceCoordinator::~ExamTelemetryServiceCoordinator() = default;

bool ExamTelemetryServiceCoordinator::open(const std::filesystem::path& outbox_path,
                                          std::string* error) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->is_open) {
        if (!impl_->close_locked()) return impl_->fail("reopen refused until pending storage recovery succeeds", error);
    }

    if (outbox_path.empty()) {
        set_error(error, "outbox path cannot be empty");
        return false;
    }

    impl_->outbox_path = outbox_path;
    impl_->state_path = outbox_path.wstring() + L".coord_state";
    if (!clean_owned_temps(impl_->state_path, true)) return impl_->fail("coordinator temporary cleanup failed", error);

    std::error_code ec;
    const bool blocked = std::filesystem::exists(impl_->state_path.wstring() + L".blocked", ec);
    if (!ec && blocked && !clean_owned_temps(impl_->outbox_path, false)) {
        return impl_->fail("blocked outbox temporary cleanup failed", error);
    }
    if (ec || blocked) return impl_->fail("coordinator revocation marker present; explicit storage recovery required", error);
    const bool had_state = std::filesystem::exists(impl_->state_path, ec);
    if (ec) return impl_->fail("coordinator state is unreadable", error);
    const bool had_outbox = std::filesystem::exists(outbox_path, ec);
    if (ec) return impl_->fail("outbox existence check failed", error);
    if (had_state && !impl_->load_state_locked()) {
        return impl_->fail("coordinator state corrupt, unreadable or legacy; explicit storage recovery required", error);
    }
    if (!had_state && had_outbox) {
        return impl_->fail("missing coordinator state for existing outbox; explicit storage recovery required", error);
    }

    bool pending_file_expired = false;
    if (impl_->pending_batch) {
        if (!had_outbox) return impl_->fail("pending telemetry storage missing; explicit recovery required", error);
        const auto modified = std::filesystem::last_write_time(outbox_path, ec);
        if (ec) return impl_->fail("pending telemetry storage age unreadable", error);
        pending_file_expired = modified <= std::filesystem::file_time_type::clock::now() -
                               std::chrono::hours(24 * exam::kTelemetryRetentionDays);
    }

    if (!had_state) {
        // Fresh start
        impl_->context = {};
        impl_->active_producer_id = {};
        impl_->next_seq = 1;
        impl_->gen_count = 0;
        impl_->dropped_samples = 0;
        impl_->dropped_batches = 0;
        impl_->not_authorized_samples = 0;
        impl_->loss_first_recorded_ms = 0;
        impl_->pending_batch.reset();
        impl_->pending_started_ms = 0;
        impl_->lifecycle = ExamTelemetryLifecycle{};
        impl_->cleanup_pending = false;
        // Publish the initial state before creating an outbox. A crash can
        // leave a valid unbound state, but never an unexplained outbox.
        impl_->is_open = true;
        if (!impl_->persist_state_locked()) {
            impl_->outbox.close();
            impl_->is_open = false;
            return impl_->fail("initial coordinator state persistence failed", error);
        }
    }
    if (!impl_->outbox.open(outbox_path, impl_->current_time_ms(), error)) {
        impl_->is_open = false;
        return false;
    }
    impl_->is_open = true;
    impl_->cleanup_pending = impl_->lifecycle.is_revoked();

    std::vector<std::uint64_t> sequences;
    for (const auto& batch : impl_->outbox.pending_batches()) {
        if (!impl_->context.matches(batch) || batch.producer_id != impl_->active_producer_id ||
            (batch.sequence >= impl_->next_seq &&
             (!impl_->pending_batch || batch.sequence != impl_->pending_batch->sequence))) {
            impl_->outbox.close();
            impl_->is_open = false;
            return impl_->fail("orphan or inconsistent durable telemetry batch", error);
        }
        sequences.push_back(batch.sequence);
    }
    std::sort(sequences.begin(), sequences.end());
    if (std::adjacent_find(sequences.begin(), sequences.end()) != sequences.end()) {
        impl_->outbox.close();
        impl_->is_open = false;
        return impl_->fail("duplicate durable telemetry sequence", error);
    }
    if (pending_file_expired) {
        (void)impl_->revoke_pending_locked("pending telemetry file expired; context revoked",
                                           error, impl_->current_time_ms());
        impl_->outbox.close();
        impl_->is_open = false;
        return false;
    }
    if (!impl_->flush_locked(error)) {
        impl_->outbox.close();
        impl_->is_open = false;
        return false;
    }

    return true;
}

void ExamTelemetryServiceCoordinator::close() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->close_locked();
}

bool ExamTelemetryServiceCoordinator::is_open() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->is_open;
}

bool ExamTelemetryServiceCoordinator::on_server_enable(
    const exam::TelemetryEnable& enable,
    std::string* error) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        set_error(error, "coordinator is not open");
        return false;
    }

    if (!exam::validate_telemetry_enable(enable)) {
        set_error(error, "invalid telemetry enable command");
        return false;
    }
    if (!impl_->flush_locked(error)) return false;

    TelemetryContextIdentity requested{enable.package_id, enable.package_digest,
        enable.client_id, enable.session_id, enable.candidate_id};
    if (impl_->lifecycle.is_identity_revoked(requested)) {
        return impl_->fail("exam telemetry identity is revoked", error);
    }

    // 2. If a context is already established:
    if (!impl_->context.empty() && !impl_->lifecycle.is_revoked()) {
        if (!impl_->context.matches(enable)) {
            set_error(error, "stale or mismatched telemetry enable context");
            return false;
        }
        // Matching reconnect/re-enable: resumes collection and upload, preserves producer ID and sequence.
        const auto res = impl_->lifecycle.on_enable(enable);
        if (!impl_->persist_state_locked()) {
            (void)impl_->lifecycle.on_disconnect();
            impl_->state_dirty = true;
            return impl_->fail("enable state persistence failed", error);
        }
        return res.accepted;
    }

    // 3. New context arriving (context was empty):
    const auto previous_producer = impl_->active_producer_id;
    const auto previous_sequence = impl_->next_seq;
    const auto previous_generations = impl_->gen_count;
    impl_->gen_count = 0; // Lifetime generation ceiling belongs to each exam context.
    if (!impl_->generate_new_producer()) {
        impl_->gen_count = previous_generations;
        return impl_->fail("producer randomness unavailable", error);
    }
    if (!impl_->lifecycle.set_context(requested)) {
        impl_->active_producer_id = previous_producer;
        impl_->next_seq = previous_sequence;
        impl_->gen_count = previous_generations;
        return impl_->fail("revoked identity archive full; new context refused", error);
    }
    impl_->context = std::move(requested);
    impl_->dropped_samples = 0;
    impl_->dropped_batches = 0;
    impl_->not_authorized_samples = 0;
    impl_->loss_first_recorded_ms = 0;

    const auto res = impl_->lifecycle.on_enable(enable);
    if (!impl_->persist_state_locked()) {
        (void)impl_->lifecycle.on_disconnect();
        impl_->state_dirty = true;
        return impl_->fail("enable state persistence failed", error);
    }
    return res.accepted;
}

void ExamTelemetryServiceCoordinator::on_disconnect() noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) return;
    (void)impl_->lifecycle.on_disconnect();
    impl_->outbox.clear_inflight();
    impl_->state_dirty = true;
    (void)impl_->flush_locked();
}

void ExamTelemetryServiceCoordinator::on_service_shutdown() noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) return;
    (void)impl_->lifecycle.on_service_shutdown();
    impl_->outbox.clear_inflight();
    impl_->state_dirty = true;
    (void)impl_->flush_locked();
}

void ExamTelemetryServiceCoordinator::on_explicit_stop() noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) return;
    if (!impl_->context.bound()) return;
    (void)impl_->lifecycle.on_explicit_stop();
    impl_->pending_batch.reset();
    impl_->pending_started_ms = 0;
    impl_->cleanup_pending = true;
    impl_->state_dirty = true;
    (void)impl_->persist_revocation_locked(nullptr, impl_->current_time_ms());
}

bool ExamTelemetryServiceCoordinator::ingest_samples(
    std::span<const exam::TelemetrySample> samples,
    std::uint64_t now_unix_ms,
    std::string* error) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        set_error(error, "coordinator is not open");
        return false;
    }

    if (samples.empty()) {
        return true;
    }

    if (!impl_->lifecycle.can_collect()) {
        impl_->touch_loss(now_unix_ms);
        add_saturated(impl_->not_authorized_samples, samples.size());
        impl_->state_dirty = true;
        (void)impl_->flush_locked(error, now_unix_ms);
        set_error(error, "telemetry collection is not enabled");
        return false;
    }

    if (now_unix_ms == 0) now_unix_ms = impl_->current_time_ms();

    const auto drop_remaining = [&](std::size_t count) {
        impl_->touch_loss(now_unix_ms);
        add_saturated(impl_->dropped_samples, count);
        add_saturated(impl_->dropped_batches, count / kMaximumBatchSamplesLimit +
            static_cast<std::size_t>(count % kMaximumBatchSamplesLimit != 0));
        impl_->state_dirty = true;
        (void)impl_->flush_locked(error, now_unix_ms);
    };

    if (!impl_->flush_locked(error, now_unix_ms)) {
        drop_remaining(samples.size());
        return false;
    }

    std::size_t offset = 0;
    while (offset < samples.size()) {
        const auto chunk_size = std::min<std::size_t>(
            samples.size() - offset, kMaximumBatchSamplesLimit);

        exam::TelemetryBatch batch;
        batch.package_id = impl_->context.package_id;
        batch.package_digest = impl_->context.package_digest;
        batch.client_id = impl_->context.client_id;
        batch.session_id = impl_->context.session_id;
        batch.candidate_id = impl_->context.candidate_id;
        batch.producer_id = impl_->active_producer_id;
        if (impl_->next_seq == std::numeric_limits<std::uint64_t>::max()) {
            drop_remaining(samples.size() - offset);
            return impl_->fail("telemetry sequence exhausted", error);
        }
        batch.sequence = impl_->next_seq;
        batch.first_enqueued_at_unix_milliseconds = std::min(now_unix_ms, impl_->current_time_ms());
        batch.samples.assign(samples.begin() + offset, samples.begin() + offset + chunk_size);
        const auto encoded = exam::encode_telemetry_batch(batch);
        if (encoded.empty()) {
            drop_remaining(samples.size() - offset);
            return impl_->fail("invalid telemetry samples", error);
        }
        if (impl_->outbox.size() >= exam::kMaximumTelemetryOutboxBatches ||
            encoded.size() > exam::kMaximumTelemetryOutboxBytes - impl_->outbox.bytes()) {
            drop_remaining(samples.size() - offset);
            return impl_->fail("telemetry outbox quota exceeded", error);
        }
        // Write-ahead intent preserves sequence 1 even if enqueue crashes or fails.
        impl_->pending_batch = std::move(batch);
        impl_->pending_started_ms = std::min(now_unix_ms, impl_->current_time_ms());
        impl_->state_dirty = true;
        offset += chunk_size;
        if (!impl_->flush_locked(error, now_unix_ms)) {
            // The pending chunk is retained for exact retry; only the unprocessed tail is loss.
            if (offset < samples.size()) {
                impl_->touch_loss(now_unix_ms);
                add_saturated(impl_->dropped_samples, samples.size() - offset);
                const auto remaining = samples.size() - offset;
                add_saturated(impl_->dropped_batches, remaining / kMaximumBatchSamplesLimit +
                    static_cast<std::size_t>(remaining % kMaximumBatchSamplesLimit != 0));
                impl_->state_dirty = true;
                (void)impl_->persist_state_locked();
            }
            return false;
        }
    }
    return true;
}

exam::TelemetryAckApplyResult ExamTelemetryServiceCoordinator::on_server_ack(
    const exam::TelemetryAck& ack,
    std::uint64_t now_unix_ms,
    std::string* error) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        set_error(error, "coordinator is not open");
        return exam::TelemetryAckApplyResult::ignored_mismatch;
    }

    if (!impl_->flush_locked(error, now_unix_ms)) return exam::TelemetryAckApplyResult::storage_failed;

    if (now_unix_ms == 0) now_unix_ms = impl_->current_time_ms();
    return impl_->outbox.acknowledge(ack, now_unix_ms, error);
}

std::optional<exam::TelemetryBatch> ExamTelemetryServiceCoordinator::next_batch_to_send(
    std::uint64_t now_unix_ms,
    std::uint64_t inflight_timeout_ms) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) return std::nullopt;

    if (now_unix_ms == 0) now_unix_ms = impl_->current_time_ms();
    if (!impl_->maintain_locked(now_unix_ms, nullptr)) return std::nullopt;
    if (!impl_->lifecycle.can_upload()) return std::nullopt;
    return impl_->outbox.next_batch_to_send(now_unix_ms, inflight_timeout_ms);
}

TelemetryLifecycleState ExamTelemetryServiceCoordinator::lifecycle_state() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->lifecycle.state();
}

TelemetryContextIdentity ExamTelemetryServiceCoordinator::context() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->context;
}

exam::ProducerId ExamTelemetryServiceCoordinator::active_producer_id() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->active_producer_id;
}

std::uint64_t ExamTelemetryServiceCoordinator::next_sequence() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->next_seq;
}

std::uint32_t ExamTelemetryServiceCoordinator::generation_count() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->gen_count;
}

std::size_t ExamTelemetryServiceCoordinator::outbox_size() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->outbox.size();
}

std::uint64_t ExamTelemetryServiceCoordinator::dropped_sample_count() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->dropped_samples;
}

std::uint64_t ExamTelemetryServiceCoordinator::dropped_batch_count() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->dropped_batches;
}

std::uint64_t ExamTelemetryServiceCoordinator::not_authorized_sample_count() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->not_authorized_samples;
}

std::string ExamTelemetryServiceCoordinator::last_error() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->last_error;
}

exam::TelemetryOutboxAggregateLoss ExamTelemetryServiceCoordinator::aggregate_loss() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->outbox.aggregate_loss();
}

std::size_t ExamTelemetryServiceCoordinator::prune_expired(std::uint64_t now_unix_ms) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) return 0;
    std::size_t count = 0;
    if (!impl_->maintain_locked(now_unix_ms, nullptr, &count)) return 0;
    return count;
}

bool ExamTelemetryServiceCoordinator::maintain(std::uint64_t now_unix_ms, std::string* error) {
    std::scoped_lock lock(impl_->mutex);
    if (!clean_owned_temps(impl_->state_path, true)) return impl_->fail("coordinator temporary cleanup failed", error);
    if (!impl_->is_open) {
        std::error_code ec;
        if (!impl_->state_path.empty() && std::filesystem::exists(impl_->state_path.wstring() + L".blocked", ec) && !ec) {
            if (!clean_owned_temps(impl_->outbox_path, false)) {
                return impl_->fail("blocked outbox temporary cleanup failed", error);
            }
            // The durable marker continues to refuse every bind. Scrub the old
            // state, which may still carry an expired pending behavioral batch.
            std::filesystem::remove(impl_->outbox_path, ec);
            if (ec) return impl_->fail("blocked outbox payload cleanup failed", error);
            std::filesystem::remove(impl_->state_path, ec);
            if (ec) return impl_->fail("blocked coordinator payload cleanup failed", error);
        }
        return impl_->fail("coordinator is not open", error);
    }
    return impl_->maintain_locked(now_unix_ms, error);
}

} // namespace nstu::client
