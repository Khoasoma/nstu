#include "nstu/exam_telemetry_outbox.hpp"
#include "nstu/secret_store.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <mutex>
#include <limits>
#include <utility>

namespace nstu::exam {

namespace {

constexpr std::uint32_t kOutboxMagic = 0x544c4d4f; // "TLMO"
constexpr std::uint16_t kOutboxVersion = 2;
constexpr std::size_t kMaximumLossContexts = 64;
constexpr char kOutboxEntropyText[] = "NSTU-Exam-Telemetry-Outbox-v1"; // DPAPI entropy remains compatible with v1.

bool expired_at(std::uint64_t since, std::uint64_t now) noexcept {
    return since != 0 && now >= since && now - since >= kTelemetryRetentionMilliseconds;
}


inline void set_error(std::string* error, std::string_view message) {
    if (error != nullptr && error->empty()) {
        *error = message;
    }
}

inline std::span<const std::byte> outbox_entropy() noexcept {
    return {reinterpret_cast<const std::byte*>(kOutboxEntropyText),
            sizeof(kOutboxEntropyText) - 1};
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

bool write_atomic_file(const std::filesystem::path& path,
                       std::span<const std::byte> bytes,
                       std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        set_error(error, "failed to create outbox directory");
        return false;
    }

    const auto temp_path = path.wstring() + L".tmp-" +
                           std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());

#ifdef _WIN32
    HANDLE h_file = CreateFileW(
        temp_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (h_file == INVALID_HANDLE_VALUE) {
        set_error(error, "failed to create temporary outbox file");
        return false;
    }

    DWORD written = 0;
    const BOOL write_ok = WriteFile(h_file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const BOOL flush_ok = FlushFileBuffers(h_file);
    CloseHandle(h_file);

    if (!write_ok || written != bytes.size() || !flush_ok) {
        DeleteFileW(temp_path.c_str());
        set_error(error, "failed to write or flush temporary outbox file");
        return false;
    }

    if (!MoveFileExW(temp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp_path.c_str());
        set_error(error, "failed to atomically rename outbox file");
        return false;
    }
#else
    {
        std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            set_error(error, "failed to open temporary outbox file");
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream.good()) {
            std::filesystem::remove(temp_path, ec);
            set_error(error, "failed to write temporary outbox file");
            return false;
        }
    }
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        set_error(error, "failed to rename outbox file");
        return false;
    }
#endif
    return true;
}

std::vector<std::byte> read_whole_file(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        set_error(error, "failed to stat outbox file");
        return {};
    }
    if (file_size > kMaximumTelemetryOutboxBytes * 2u + 65536u) {
        set_error(error, "outbox file exceeds maximum quota");
        return {};
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        set_error(error, "failed to open outbox file");
        return {};
    }

    std::vector<std::byte> buffer(static_cast<std::size_t>(file_size));
    stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(file_size));
    if (!stream.good() && file_size > 0) {
        set_error(error, "failed to read outbox file");
        return {};
    }
    return buffer;
}

inline std::uint64_t current_unix_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void clean_orphan_tmp_files(const std::filesystem::path& path) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (parent.empty() || !std::filesystem::exists(parent, ec) || ec) return;
    const auto prefix = path.filename().wstring() + L".tmp";
    std::filesystem::directory_iterator it(parent, ec), end;
    while (!ec && it != end) {
        const auto& entry = *it;
        const auto name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) == 0 && entry.is_regular_file(ec) && !ec) {
            std::filesystem::remove(entry.path(), ec);
        }
        if (ec) break;
        it.increment(ec);
    }
}

// Keep at most one quarantined blob; remove all blobs older than 30 days.
// Return false when cleanup cannot establish the bound (never create another blob then).
bool clean_quarantine_files(const std::filesystem::path& path, bool make_room) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (parent.empty() || !std::filesystem::exists(parent, ec) || ec) return !ec;
    const auto prefix = path.filename().wstring() + L".corrupt-";
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    std::filesystem::directory_iterator it(parent, ec), end;
    while (!ec && it != end) {
        const auto& entry = *it;
        if (entry.path().filename().wstring().rfind(prefix, 0) == 0 &&
            entry.is_regular_file(ec) && !ec) {
            const auto timestamp = entry.last_write_time(ec);
            if (ec) return false;
            if (timestamp <= std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * kTelemetryRetentionDays)) {
                std::filesystem::remove(entry.path(), ec);
                if (ec) return false;
            } else {
                files.emplace_back(timestamp, entry.path());
            }
        }
        if (ec) return false;
        it.increment(ec);
    }
    if (ec) return false;
    std::sort(files.begin(), files.end());
    const auto keep = make_room ? 0u : 1u;
    for (std::size_t i = 0; i + keep < files.size(); ++i) {
        std::filesystem::remove(files[i].second, ec);
        if (ec) return false;
    }
    return true;
}

bool quarantine_corrupted_file(const std::filesystem::path& path) {
    if (!clean_quarantine_files(path, true)) return false;
    std::error_code ec;
    const auto corrupt_path = path.wstring() + L".corrupt-" +
        std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count());
    std::filesystem::rename(path, corrupt_path, ec);
    if (ec) return false;
    // Renaming retains the original mtime; don't extend a corrupt blob's lifetime.
    const auto age = std::filesystem::last_write_time(corrupt_path, ec);
    if (ec) return false;
    if (age <= std::filesystem::file_time_type::clock::now() -
            std::chrono::hours(24 * kTelemetryRetentionDays)) {
        std::filesystem::remove(corrupt_path, ec);
    }
    return !ec;
}

} // namespace

struct OutboxRecord {
    TelemetryBatch batch;
    std::vector<std::byte> encoded_bytes;
    std::uint16_t retry_count = 0;
    std::uint64_t next_eligible_retry_time_ms = 0;
    std::uint64_t local_enqueued_at_unix_ms = 0;
    bool in_flight = false;
    std::uint64_t in_flight_until_ms = 0;
    bool expiry_observed = false;
};

struct ExpiredBatchIdentity {
    SessionId session_id{};
    ProducerId producer_id{};
    std::uint64_t sequence = 0;
    bool operator==(const ExpiredBatchIdentity&) const noexcept = default;
};

struct TelemetryOutbox::Impl {
    mutable std::mutex mutex;
    bool is_open = false;
    bool revocation_pending = false;
    std::filesystem::path path;
    std::vector<TelemetryOutboxAggregateLoss> losses;
    SessionId active_session_id{};
    std::vector<OutboxRecord> records;
    std::size_t total_payload_bytes = 0;
    bool expiry_retry_pending = false;
    std::vector<SessionId> expired_loss_contexts;
    std::vector<ExpiredBatchIdentity> expired_batch_identities;
    bool expiry_observation_overflow = false;
    std::uint64_t expiry_barrier_ms = 0;

    bool persist_expiry_barrier(std::uint64_t now, std::string* error) {
        if (expiry_barrier_ms >= now) return true;
        std::vector<std::byte> plaintext;
        append_u64_le(plaintext, now);
        auto blob = security::protect_machine_secret(plaintext, outbox_entropy(), error);
        if (blob.empty() || !write_atomic_file(path.wstring() + L".expiry", blob, error)) return false;
        expiry_barrier_ms = now;
        return true;
    }

    bool clear_expiry_barrier(std::string* error) {
        std::error_code ec;
        std::filesystem::remove(path.wstring() + L".expiry", ec);
        if (ec) { set_error(error, "expiry barrier cleanup failed"); return false; }
        expiry_barrier_ms = 0;
        return true;
    }

    bool batch_expiry_observed(const OutboxRecord& rec) const noexcept {
        const ExpiredBatchIdentity id{rec.batch.session_id, rec.batch.producer_id, rec.batch.sequence};
        return std::find(expired_batch_identities.begin(), expired_batch_identities.end(), id) != expired_batch_identities.end();
    }

    bool loss_expiry_observed(const SessionId& id) const noexcept {
        return std::find(expired_loss_contexts.begin(), expired_loss_contexts.end(), id) != expired_loss_contexts.end();
    }

    bool loss_has_expired(const TelemetryOutboxAggregateLoss& loss, std::uint64_t now) const noexcept {
        return loss_expiry_observed(loss.session_id) || expired_at(loss.first_recorded_unix_ms, now);
    }

    void close_locked() noexcept {
        is_open = false;
        records.clear();
        total_payload_bytes = 0;
        // Keep bounded identities after a failed cleanup, never payload copies.
        // Reopening this same file must retry observed expiry before offering data.
        if (!expiry_retry_pending) {
            expired_loss_contexts.clear();
            expired_batch_identities.clear();
        }
    }

    bool persist_locked(std::string* error) {
        if (!is_open) {
            set_error(error, "outbox is not open");
            return false;
        }

        std::vector<std::byte> plaintext;
        plaintext.reserve(256 + total_payload_bytes + records.size() * 24 + losses.size() * 56);

        append_u32_le(plaintext, kOutboxMagic);
        append_u16_le(plaintext, kOutboxVersion);
        append_u16_le(plaintext, 0); // reserved
        plaintext.insert(plaintext.end(), active_session_id.begin(), active_session_id.end());
        append_u32_le(plaintext, static_cast<std::uint32_t>(losses.size()));
        for (const auto& loss : losses) {
            plaintext.insert(plaintext.end(), loss.session_id.begin(), loss.session_id.end());
            append_u64_le(plaintext, loss.rejected_batches);
            append_u64_le(plaintext, loss.conflict_batches);
            append_u64_le(plaintext, loss.expired_batches);
            append_u64_le(plaintext, loss.first_recorded_unix_ms);
            append_u64_le(plaintext, loss.last_updated_unix_ms);
        }

        append_u32_le(plaintext, static_cast<std::uint32_t>(records.size()));

        for (const auto& rec : records) {
            append_u32_le(plaintext, static_cast<std::uint32_t>(rec.encoded_bytes.size()));
            plaintext.insert(plaintext.end(), rec.encoded_bytes.begin(), rec.encoded_bytes.end());
            append_u16_le(plaintext, rec.retry_count);
            append_u64_le(plaintext, rec.next_eligible_retry_time_ms);
            append_u64_le(plaintext, rec.local_enqueued_at_unix_ms);
        }

        const auto digest = security::sha256(plaintext);
        if (!digest.has_value()) {
            set_error(error, "failed to compute outbox checksum");
            return false;
        }
        plaintext.insert(plaintext.end(), digest->begin(), digest->end());

        // Protect secret using machine protection
        auto protected_blob = security::protect_machine_secret(plaintext, outbox_entropy(), error);
        if (protected_blob.empty()) {
            return false;
        }

        return write_atomic_file(path, protected_blob, error);
    }
    std::optional<std::size_t> prune_expired_locked(
        std::uint64_t now_unix_ms, std::string* error, bool force_persist = false) {
        now_unix_ms = std::max(now_unix_ms, expiry_barrier_ms);
        if (expiry_observation_overflow) {
            set_error(error, "expiry observation capacity exceeded; explicit cleanup required");
            return std::nullopt;
        }
        std::size_t pruned_count = 0;
        for (auto& rec : records) {
            rec.expiry_observed = rec.expiry_observed || batch_expiry_observed(rec) || expired_at(
                std::min(rec.local_enqueued_at_unix_ms, rec.batch.first_enqueued_at_unix_milliseconds), now_unix_ms);
            if (rec.expiry_observed) {
                if (!batch_expiry_observed(rec)) {
                    if (expired_batch_identities.size() == kMaximumTelemetryOutboxBatches) {
                        expiry_retry_pending = true;
                        expiry_observation_overflow = true;
                        set_error(error, "expiry observation capacity exceeded; explicit cleanup required");
                        return std::nullopt;
                    }
                    expired_batch_identities.push_back({rec.batch.session_id, rec.batch.producer_id, rec.batch.sequence});
                }
                ++pruned_count;
            }
        }
        for (const auto& loss : losses) {
            if (expired_at(loss.first_recorded_unix_ms, now_unix_ms) && !loss_expiry_observed(loss.session_id)) {
                if (expired_loss_contexts.size() >= kMaximumLossContexts) {
                    expiry_retry_pending = true;
                    expiry_observation_overflow = true;
                    set_error(error, "expiry observation capacity exceeded; explicit cleanup required");
                    return std::nullopt;
                }
                expired_loss_contexts.push_back(loss.session_id);
            }
        }
        const bool loss_expired = std::any_of(losses.begin(), losses.end(), [&](const auto& loss) {
            return loss_has_expired(loss, now_unix_ms);
        });
        if (pruned_count == 0 && !loss_expired && !force_persist && !expiry_retry_pending) {
            if (expiry_barrier_ms != 0 && !clear_expiry_barrier(error)) return std::nullopt;
            return 0;
        }
        expiry_retry_pending = expiry_retry_pending || pruned_count != 0 || loss_expired;
        // Persist the observation before replacing the payload file. A sibling
        // barrier survives a crash or failed replace followed by clock rollback.
        if (expiry_retry_pending && !persist_expiry_barrier(now_unix_ms, error)) return std::nullopt;
        const auto old_losses = losses;
        const auto old_bytes = total_payload_bytes;
        std::vector<OutboxRecord> retained;
        retained.reserve(records.size() - pruned_count);
        std::vector<std::pair<std::size_t, OutboxRecord>> expired;
        expired.reserve(pruned_count);
        std::vector<SessionId> incoming_contexts;
        for (const auto& rec : records) {
            if (!rec.expiry_observed) continue;
            const auto id = rec.batch.session_id;
            const bool already_retained = std::any_of(losses.begin(), losses.end(), [&](const auto& loss) {
                return loss.session_id == id && !loss_has_expired(loss, now_unix_ms);
            });
            if (!already_retained && std::find(incoming_contexts.begin(), incoming_contexts.end(), id) == incoming_contexts.end()) {
                incoming_contexts.push_back(id);
            }
        }
        const auto expiring_contexts = static_cast<std::size_t>(std::count_if(losses.begin(), losses.end(), [&](const auto& loss) {
            return loss_has_expired(loss, now_unix_ms);
        }));
        if (losses.size() - expiring_contexts + incoming_contexts.size() > kMaximumLossContexts) {
            set_error(error, "telemetry loss context quota exceeded");
            return std::nullopt;
        }
        losses.erase(std::remove_if(losses.begin(), losses.end(), [&](const auto& loss) {
            return loss_has_expired(loss, now_unix_ms);
        }), losses.end());
        for (std::size_t index = 0; index < records.size(); ++index) {
            auto& rec = records[index];
            if (rec.expiry_observed) {
                total_payload_bytes -= rec.encoded_bytes.size();
                auto loss = std::find_if(losses.begin(), losses.end(), [&](const auto& item) {
                    return item.session_id == rec.batch.session_id;
                });
                if (loss == losses.end()) {
                    losses.push_back({});
                    loss = std::prev(losses.end());
                    loss->session_id = rec.batch.session_id;
                }
                if (loss != losses.end()) {
                    if (loss->expired_batches < kMaximumTelemetryOutboxBatches) ++loss->expired_batches;
                    loss->last_updated_unix_ms = std::min(now_unix_ms, current_unix_ms());
                    if (loss->first_recorded_unix_ms == 0) loss->first_recorded_unix_ms = loss->last_updated_unix_ms;
                }
                expired.emplace_back(index, std::move(rec));
            } else {
                retained.push_back(std::move(rec));
            }
        }
        records.swap(retained);
        if (!persist_locked(error)) {
            records.swap(retained);
            records.clear();
            auto kept = retained.begin();
            auto removed = expired.begin();
            // Restore original sequence without copying payloads.
            for (std::size_t index = 0; index < retained.size() + expired.size(); ++index) {
                if (removed != expired.end() && removed->first == index) {
                    records.push_back(std::move(removed++->second));
                } else {
                    records.push_back(std::move(*kept++));
                }
            }
            total_payload_bytes = old_bytes;
            losses = old_losses;
            return std::nullopt;
        }
        expiry_retry_pending = false;
        expired_loss_contexts.clear();
        expired_batch_identities.clear();
        if (!clear_expiry_barrier(error)) return std::nullopt;
        return pruned_count;
    }

};

TelemetryOutbox::TelemetryOutbox() : impl_(std::make_unique<Impl>()) {}
TelemetryOutbox::~TelemetryOutbox() = default;
TelemetryOutbox::TelemetryOutbox(TelemetryOutbox&&) noexcept = default;
TelemetryOutbox& TelemetryOutbox::operator=(TelemetryOutbox&&) noexcept = default;

bool TelemetryOutbox::open(const std::filesystem::path& path, std::string* error) {
    return open(path, current_unix_ms(), error);
}

bool TelemetryOutbox::open(const std::filesystem::path& path,
                           std::uint64_t now_ms, std::string* error) {
    if (now_ms == 0) now_ms = current_unix_ms();
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    std::scoped_lock lock(impl_->mutex);
    if ((impl_->expiry_retry_pending || impl_->expiry_barrier_ms != 0) && path != impl_->path) {
        set_error(error, "observed expiry cleanup pending; cannot change outbox path");
        return false;
    }
    if (impl_->is_open) {
        impl_->close_locked();
    }

    if (path.empty()) {
        set_error(error, "outbox path cannot be empty");
        return false;
    }
    if (impl_->revocation_pending) {
        set_error(error, "outbox revocation pending; restart cleanup required");
        return false;
    }

    clean_orphan_tmp_files(path);
    if (!clean_quarantine_files(path, false)) {
        set_error(error, "failed to clean quarantined outbox files");
        return false;
    }

    impl_->path = path;
    impl_->records.clear();
    impl_->total_payload_bytes = 0;
    impl_->losses.clear();
    impl_->active_session_id = {};

    std::error_code ec;
    // A refusal must not retain identifiable payload forever after the last write.
    // Files modified within retention are left intact on transient read/decrypt errors.
    if (std::filesystem::exists(path, ec) && !ec) {
        const auto last_write = std::filesystem::last_write_time(path, ec);
        if (ec) {
            set_error(error, "failed to stat outbox age");
            return false;
        }
        if (last_write <= std::filesystem::file_time_type::clock::now() -
                std::chrono::hours(24 * kTelemetryRetentionDays)) {
            std::filesystem::remove(path, ec);
            if (ec) {
                set_error(error, "failed to expire old outbox file");
                return false;
            }
        }
    }
    if (ec) {
        set_error(error, "failed to stat outbox file");
        return false;
    }
    const auto expiry_path = std::filesystem::path(path.wstring() + L".expiry");
    clean_orphan_tmp_files(expiry_path);
    const bool has_expiry_barrier = std::filesystem::exists(expiry_path, ec);
    if (ec) { set_error(error, "expiry barrier unreadable"); return false; }
    if (has_expiry_barrier) {
        const auto size = std::filesystem::file_size(expiry_path, ec);
        if (ec || size > 4096) { set_error(error, "invalid expiry barrier size"); return false; }
        const auto blob = read_whole_file(expiry_path, error);
        const auto plaintext = security::unprotect_machine_secret(blob, outbox_entropy(), error);
        std::size_t offset = 0;
        std::uint64_t observed = 0;
        if (plaintext.size() != 8 || !read_u64_le(plaintext, offset, observed) || observed == 0) {
            set_error(error, "invalid expiry barrier");
            return false;
        }
        impl_->expiry_barrier_ms = std::max(impl_->expiry_barrier_ms, observed);
        now_ms = std::max(now_ms, impl_->expiry_barrier_ms);
    }
    const auto revoked_path = std::filesystem::path(path.wstring() + L".revoked");
    const bool revoked = std::filesystem::exists(revoked_path, ec);
    if (ec) {
        set_error(error, "failed to read revocation marker");
        return false;
    }
    if (revoked) {
        // Marker precedes payload deletion. Reopen may only continue after deletion succeeds.
        ec.clear();
        std::filesystem::remove(path, ec);
        if (ec) {
            set_error(error, "outbox revoked; payload deletion still blocked");
            return false;
        }
        // A failed marker removal is safe: subsequent opens retry cleanup.
        std::filesystem::remove(revoked_path, ec);
        if (ec) {
            set_error(error, "outbox revoked; marker cleanup failed");
            return false;
        }
    }
    ec.clear();
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            set_error(error, "failed to stat outbox file");
            return false;
        }
        impl_->is_open = true;
        if (!impl_->persist_locked(error)) {
            impl_->is_open = false;
            return false;
        }
        if (!impl_->clear_expiry_barrier(error)) { impl_->is_open = false; return false; }
        return true;
    }
    if (ec) {
        set_error(error, "failed to stat outbox file");
        return false;
    }

    auto fail_quarantine = [&](std::string_view msg) {
        if (!quarantine_corrupted_file(path)) {
            set_error(error, "failed to quarantine invalid outbox");
        } else {
            set_error(error, msg);
        }
        impl_->records.clear();
        impl_->total_payload_bytes = 0;
        impl_->losses.clear();
        impl_->active_session_id = {};
        impl_->is_open = false;
        return false;
    };

    // Read and DPAPI errors can be transient; never move or discard their source file.
    const auto stored = read_whole_file(path, error);
    if (stored.empty()) {
        set_error(error, "outbox file could not be read");
        return false;
    }
    auto plaintext = security::unprotect_machine_secret(stored, outbox_entropy(), error);
    if (plaintext.empty()) {
        set_error(error, "outbox decryption failed");
        return false;
    }
    if (plaintext.size() < 4 + 2 + 2 + 4 + 32) {
        return fail_quarantine("outbox plaintext too short");
    }

    // A checksum/format failure after successful decryption proves integrity failure.
    const auto payload_span = std::span<const std::byte>(plaintext.data(), plaintext.size() - 32);
    const auto expected_digest = security::sha256(payload_span);
    if (!expected_digest.has_value()) {
        set_error(error, "failed to compute outbox checksum");
        return false;
    }
    const auto stored_digest = std::span<const std::byte>(plaintext.data() + plaintext.size() - 32, 32);
    if (!std::equal(expected_digest->begin(), expected_digest->end(), stored_digest.begin())) {
        return fail_quarantine("outbox checksum mismatch: file corrupted or tampered");
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    if (!read_u32_le(payload_span, offset, magic) || magic != kOutboxMagic ||
        !read_u16_le(payload_span, offset, version) || (version != 1 && version != kOutboxVersion) ||
        !read_u16_le(payload_span, offset, reserved) || reserved != 0) {
        return fail_quarantine("outbox header invalid");
    }

    std::vector<TelemetryOutboxAggregateLoss> loaded_losses;
    SessionId active_session{};
    if (version == 1) {
        TelemetryOutboxAggregateLoss legacy_loss;
        if (!read_u64_le(payload_span, offset, legacy_loss.rejected_batches) ||
            !read_u64_le(payload_span, offset, legacy_loss.conflict_batches) ||
            !read_u64_le(payload_span, offset, legacy_loss.expired_batches) ||
            !read_u64_le(payload_span, offset, legacy_loss.first_recorded_unix_ms) ||
            !read_u64_le(payload_span, offset, legacy_loss.last_updated_unix_ms)) {
            return fail_quarantine("outbox loss metadata corrupted");
        }
        loaded_losses.push_back(legacy_loss);
    } else {
        if (offset + active_session.size() > payload_span.size()) return fail_quarantine("outbox context truncated");
        std::copy_n(plaintext.begin() + static_cast<std::ptrdiff_t>(offset), active_session.size(), active_session.begin());
        offset += active_session.size();
        std::uint32_t loss_count = 0;
        if (!read_u32_le(payload_span, offset, loss_count) || loss_count > kMaximumLossContexts) {
            return fail_quarantine("outbox loss context count invalid");
        }
        for (std::uint32_t i = 0; i < loss_count; ++i) {
            TelemetryOutboxAggregateLoss loss;
            if (offset + loss.session_id.size() > payload_span.size()) return fail_quarantine("outbox loss context truncated");
            std::copy_n(plaintext.begin() + static_cast<std::ptrdiff_t>(offset), loss.session_id.size(), loss.session_id.begin());
            offset += loss.session_id.size();
            if (!read_u64_le(payload_span, offset, loss.rejected_batches) ||
                !read_u64_le(payload_span, offset, loss.conflict_batches) ||
                !read_u64_le(payload_span, offset, loss.expired_batches) ||
                !read_u64_le(payload_span, offset, loss.first_recorded_unix_ms) ||
                !read_u64_le(payload_span, offset, loss.last_updated_unix_ms) ||
                loss.session_id == SessionId{} ||
                std::any_of(loaded_losses.begin(), loaded_losses.end(), [&](const auto& other) { return other.session_id == loss.session_id; })) {
                return fail_quarantine("outbox loss metadata corrupted");
            }
            loaded_losses.push_back(loss);
        }
    }

    std::uint32_t batch_count = 0;
    if (!read_u32_le(payload_span, offset, batch_count)) {
        return fail_quarantine("outbox batch count corrupted");
    }
    if (batch_count > kMaximumTelemetryOutboxBatches) {
        set_error(error, "outbox batch count exceeds maximum quota");
        return false;
    }
    std::vector<OutboxRecord> loaded_records;
    loaded_records.reserve(batch_count);
    std::size_t loaded_payload_bytes = 0;
    bool clamped = version == 1; // migrate readable v1 to durable arrival timestamps.

    for (std::uint32_t i = 0; i < batch_count; ++i) {
        std::uint32_t len = 0;
        if (!read_u32_le(payload_span, offset, len) || len > payload_span.size() - offset) {
            return fail_quarantine("outbox batch payload truncated");
        }
        const bool over_quota = len > kMaximumTelemetryOutboxBytes - loaded_payload_bytes;
        const auto batch_span = payload_span.subspan(offset, len);
        offset += len;
        const auto decoded = decode_telemetry_batch(batch_span, error);
        if (!decoded.has_value()) return fail_quarantine("outbox batch payload could not be decoded");
        if (over_quota) {
            // ponytail: reject over-quota files whole; trim only with a durable loss-accounting design.
            set_error(error, "outbox byte quota exceeded on open");
            return false;
        }
        OutboxRecord rec;
        rec.batch = std::move(*decoded);
        rec.encoded_bytes.assign(batch_span.begin(), batch_span.end());
        if (!read_u16_le(payload_span, offset, rec.retry_count) ||
            !read_u64_le(payload_span, offset, rec.next_eligible_retry_time_ms) ||
            (version == kOutboxVersion && !read_u64_le(payload_span, offset, rec.local_enqueued_at_unix_ms))) {
            return fail_quarantine("outbox retry metadata corrupted");
        }
        if (version == 1) rec.local_enqueued_at_unix_ms = rec.batch.first_enqueued_at_unix_milliseconds;
        if (rec.local_enqueued_at_unix_ms > now_ms) {
            rec.local_enqueued_at_unix_ms = now_ms;
            clamped = true;
        }
        if (rec.next_eligible_retry_time_ms > now_ms &&
            rec.next_eligible_retry_time_ms - now_ms > 60000ULL) {
            rec.next_eligible_retry_time_ms = now_ms + 60000ULL;
            clamped = true;
        }
        // Batch wire bytes are immutable ACK identity; do not rewrite their producer timestamp.
        // Expiry uses the separately persisted, clamped local arrival timestamp.
        if (rec.local_enqueued_at_unix_ms == 0) return fail_quarantine("outbox arrival time invalid");
        // Only a v1 file has no stored context header; v2 must match its persisted ID.
        if (version == 1 && active_session == SessionId{}) active_session = rec.batch.session_id;
        if (rec.batch.session_id != active_session) return fail_quarantine("outbox contains mixed contexts");
        loaded_payload_bytes += rec.encoded_bytes.size();
        loaded_records.push_back(std::move(rec));
    }
    if (offset != payload_span.size()) return fail_quarantine("trailing bytes in outbox plaintext");
    if (version == 1 && !loaded_records.empty()) {
        loaded_losses.front().session_id = active_session;
    }
    if (version == 1 && loaded_records.empty() &&
        (loaded_losses.front().rejected_batches || loaded_losses.front().conflict_batches || loaded_losses.front().expired_batches)) {
        // Legacy loss without a context cannot safely be attributed to a new session.
        set_error(error, "legacy outbox loss context cannot be determined");
        return false;
    }
    if (version == kOutboxVersion && active_session == SessionId{} && !loaded_records.empty()) {
        return fail_quarantine("outbox context missing");
    }
    for (auto& loss : loaded_losses) {
        if (loss.first_recorded_unix_ms == 0 &&
            (loss.rejected_batches != 0 || loss.conflict_batches != 0 || loss.expired_batches != 0)) {
            return fail_quarantine("outbox loss retention timestamp missing");
        }
        if (loss.first_recorded_unix_ms > now_ms) {
            loss.first_recorded_unix_ms = now_ms;
            clamped = true;
        }
        if (loss.last_updated_unix_ms > now_ms) {
            loss.last_updated_unix_ms = now_ms;
            clamped = true;
        }
    }
    const auto prior_loss_count = loaded_losses.size();
    loaded_losses.erase(std::remove_if(loaded_losses.begin(), loaded_losses.end(), [&](const auto& loss) {
        return loss.rejected_batches == 0 && loss.conflict_batches == 0 && loss.expired_batches == 0;
    }), loaded_losses.end());
    clamped |= loaded_losses.size() != prior_loss_count;
    impl_->active_session_id = active_session;
    impl_->losses = std::move(loaded_losses);
    impl_->records = std::move(loaded_records);
    impl_->total_payload_bytes = loaded_payload_bytes;
    impl_->is_open = true;
    if (!impl_->prune_expired_locked(now_ms, error, clamped).has_value()) {
        impl_->close_locked();
        return false;
    }
    return true;
}

void TelemetryOutbox::close() noexcept {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->close_locked();
}

bool TelemetryOutbox::is_open() const noexcept {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    return impl_->is_open;
}

bool TelemetryOutbox::enqueue(const TelemetryBatch& batch, std::string* error) {
    if (!impl_) {
        set_error(error, "outbox uninitialized");
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        set_error(error, "outbox is not open");
        return false;
    }

    if (!validate_telemetry_batch(batch)) {
        set_error(error, "invalid telemetry batch");
        return false;
    }
    if ((impl_->expiry_retry_pending || impl_->expiry_barrier_ms != 0) &&
        !impl_->prune_expired_locked(current_unix_ms(), error).has_value()) return false;

    // Pending records belong to one context; drained contexts retain separate loss metadata.
    if (!impl_->records.empty()) {
        const auto& active = impl_->records.front().batch;
        if (active.session_id != batch.session_id || active.package_id != batch.package_id ||
            active.package_digest != batch.package_digest || active.client_id != batch.client_id ||
            active.candidate_id != batch.candidate_id) {
            set_error(error, "conflicting exam context for outbox");
            return false;
        }
    }
    auto loss = std::find_if(impl_->losses.begin(), impl_->losses.end(), [&](const auto& item) {
        return item.session_id == batch.session_id;
    });
    if (loss == impl_->losses.end() && impl_->losses.size() >= kMaximumLossContexts) {
        auto unused = std::find_if(impl_->losses.begin(), impl_->losses.end(), [&](const auto& item) {
            return item.rejected_batches == 0 && item.conflict_batches == 0 && item.expired_batches == 0 &&
                std::none_of(impl_->records.begin(), impl_->records.end(), [&](const auto& rec) {
                    return rec.batch.session_id == item.session_id;
                });
        });
        if (unused == impl_->losses.end()) {
            set_error(error, "telemetry loss context quota exceeded");
            return false;
        }
        // Reuse an inactive empty slot only after successful persistence below.
        loss = unused;
    }

    // Duplicate sequence check per producer
    for (const auto& rec : impl_->records) {
        if (rec.batch.producer_id == batch.producer_id &&
            rec.batch.sequence == batch.sequence) {
            set_error(error, "duplicate sequence for producer");
            return false;
        }
    }

    const auto encoded = encode_telemetry_batch(batch);
    if (encoded.empty()) {
        set_error(error, "failed to encode telemetry batch");
        return false;
    }

    if (impl_->records.size() + 1 > kMaximumTelemetryOutboxBatches) {
        set_error(error, "telemetry outbox batch quota exceeded");
        return false;
    }

    if (impl_->total_payload_bytes + encoded.size() > kMaximumTelemetryOutboxBytes) {
        set_error(error, "telemetry outbox byte quota exceeded");
        return false;
    }

    // Reserve the loss slot before writing the payload; a terminal ACK can never
    // silently lose its counters when the context table later reaches its cap.
    const bool add_loss_slot = loss == impl_->losses.end();
    const auto old_loss_slot = add_loss_slot ? TelemetryOutboxAggregateLoss{} : *loss;
    if (add_loss_slot) {
        impl_->losses.push_back({});
        impl_->losses.back().session_id = batch.session_id;
    } else if (loss->session_id != batch.session_id) {
        loss->session_id = batch.session_id;
    }
    OutboxRecord rec;
    rec.batch = batch;
    rec.encoded_bytes = encoded;
    rec.retry_count = 0;
    rec.next_eligible_retry_time_ms = 0;
    rec.local_enqueued_at_unix_ms = current_unix_ms();
    rec.in_flight = false;
    rec.in_flight_until_ms = 0;

    const auto previous_session = impl_->active_session_id;
    impl_->active_session_id = batch.session_id;
    impl_->records.push_back(std::move(rec));
    impl_->total_payload_bytes += encoded.size();

    if (!impl_->persist_locked(error)) {
        impl_->records.pop_back();
        impl_->total_payload_bytes -= encoded.size();
        impl_->active_session_id = previous_session;
        if (add_loss_slot) impl_->losses.pop_back();
        else *loss = old_loss_slot;
        return false;
    }

    return true;
}

TelemetryAckApplyResult TelemetryOutbox::acknowledge(
    const TelemetryAck& ack,
    std::uint64_t now_unix_ms,
    std::string* error) {
    if (!impl_) {
        set_error(error, "outbox uninitialized");
        return TelemetryAckApplyResult::ignored_mismatch;
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        set_error(error, "outbox is not open");
        return TelemetryAckApplyResult::ignored_mismatch;
    }

    if (now_unix_ms == 0) {
        now_unix_ms = current_unix_ms();
    }

    if (impl_->expiry_retry_pending &&
        !impl_->prune_expired_locked(now_unix_ms, error).has_value()) return TelemetryAckApplyResult::storage_failed;

    auto it = std::find_if(impl_->records.begin(), impl_->records.end(),
        [&ack](const OutboxRecord& rec) {
            return rec.batch.session_id == ack.session_id &&
                   rec.batch.producer_id == ack.producer_id &&
                   rec.batch.sequence == ack.sequence;
        });

    if (it == impl_->records.end()) {
        return TelemetryAckApplyResult::ignored_mismatch;
    }

    const auto batch_hash = compute_telemetry_batch_hash(it->batch);
    if (!batch_hash.has_value() || *batch_hash != ack.batch_hash) {
        return TelemetryAckApplyResult::ignored_mismatch;
    }

    const auto index = static_cast<std::size_t>(it - impl_->records.begin());
    TelemetryAckApplyResult result = TelemetryAckApplyResult::ignored_mismatch;
    if (ack.status == TelemetryAckStatus::unavailable) {
        const auto old_retry = it->retry_count;
        const auto old_deadline = it->next_eligible_retry_time_ms;
        const auto old_in_flight = it->in_flight;
        const auto old_in_flight_until = it->in_flight_until_ms;
        it->in_flight = false;
        it->in_flight_until_ms = 0;
        if (it->retry_count < std::numeric_limits<std::uint16_t>::max()) ++it->retry_count;
        const auto exponent = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(it->retry_count) - 1u, 5u);
        const auto delay_ms = std::min<std::uint64_t>(2000ULL << exponent, 60000ULL);
        it->next_eligible_retry_time_ms = now_unix_ms + delay_ms;
        if (!impl_->persist_locked(error)) {
            it->retry_count = old_retry;
            it->next_eligible_retry_time_ms = old_deadline;
            it->in_flight = old_in_flight;
            it->in_flight_until_ms = old_in_flight_until;
            return TelemetryAckApplyResult::storage_failed;
        }
        return TelemetryAckApplyResult::retry_scheduled;
    }

    const auto old_loss = impl_->losses;
    const auto old_bytes = impl_->total_payload_bytes;
    OutboxRecord removed = std::move(*it);
    impl_->records.erase(it);
    impl_->total_payload_bytes -= removed.encoded_bytes.size();
    if (ack.status == TelemetryAckStatus::accepted || ack.status == TelemetryAckStatus::duplicate) {
        result = TelemetryAckApplyResult::retired;
    } else {
        auto loss = std::find_if(impl_->losses.begin(), impl_->losses.end(), [&](const auto& item) {
            return item.session_id == removed.batch.session_id;
        });
        if (loss == impl_->losses.end()) {
            // Legacy v1 files may lack a loss slot; keep terminal deletion fail-closed
            // when the context cap is reached instead of forgetting its counters.
            if (impl_->losses.size() >= kMaximumLossContexts) {
                impl_->records.insert(impl_->records.begin() + static_cast<std::ptrdiff_t>(index), std::move(removed));
                impl_->total_payload_bytes = old_bytes;
                set_error(error, "telemetry loss context quota exceeded");
                return TelemetryAckApplyResult::storage_failed;
            }
            impl_->losses.push_back({});
            loss = std::prev(impl_->losses.end());
            loss->session_id = ack.session_id;
        }
        if (ack.status == TelemetryAckStatus::rejected && loss->rejected_batches < kMaximumTelemetryOutboxBatches) {
            ++loss->rejected_batches;
        } else if (ack.status == TelemetryAckStatus::conflict && loss->conflict_batches < kMaximumTelemetryOutboxBatches) {
            ++loss->conflict_batches;
        }
        loss->last_updated_unix_ms = std::min(now_unix_ms, current_unix_ms());
        if (loss->first_recorded_unix_ms == 0) loss->first_recorded_unix_ms = loss->last_updated_unix_ms;
        result = TelemetryAckApplyResult::deleted_terminal;
    }
    if (!impl_->persist_locked(error)) {
        impl_->records.insert(impl_->records.begin() + static_cast<std::ptrdiff_t>(index), std::move(removed));
        impl_->total_payload_bytes = old_bytes;
        impl_->losses = old_loss;
        return TelemetryAckApplyResult::storage_failed;
    }
    return result;
}

std::optional<TelemetryBatch> TelemetryOutbox::next_batch_to_send(
    std::uint64_t now_unix_ms,
    std::uint64_t inflight_timeout_ms) {
    if (!impl_) return std::nullopt;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        return std::nullopt;
    }

    if (now_unix_ms == 0) {
        now_unix_ms = current_unix_ms();
    }

    std::string prune_error;
    if (!impl_->prune_expired_locked(now_unix_ms, &prune_error).has_value()) return std::nullopt;

    for (auto& rec : impl_->records) {
        // Keep expiry and selection under one lock, including newly enqueued records.
        if (rec.expiry_observed || expired_at(std::min(rec.local_enqueued_at_unix_ms,
                                rec.batch.first_enqueued_at_unix_milliseconds), now_unix_ms)) continue;
        if (rec.in_flight) {
            if (now_unix_ms > rec.in_flight_until_ms ||
                rec.in_flight_until_ms > now_unix_ms + inflight_timeout_ms) {
                // In-flight timed out or clock moved backward; reclaim
                rec.in_flight = false;
                rec.in_flight_until_ms = 0;
            } else {
                continue;
            }
        }

        // Clamp retry deadline against backward clock jump (60s bound)
        if (rec.next_eligible_retry_time_ms > now_unix_ms + 60000ULL) {
            rec.next_eligible_retry_time_ms = now_unix_ms + 60000ULL;
        }

        if (now_unix_ms < rec.next_eligible_retry_time_ms) {
            continue;
        }

        rec.in_flight = true;
        rec.in_flight_until_ms = now_unix_ms + inflight_timeout_ms;
        return rec.batch;
    }
    return std::nullopt;
}

void TelemetryOutbox::clear_inflight() noexcept {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    for (auto& rec : impl_->records) {
        rec.in_flight = false;
        rec.in_flight_until_ms = 0;
    }
}

std::size_t TelemetryOutbox::prune_expired(std::uint64_t now_unix_ms, std::string* error) {
    if (!impl_) return 0;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) return 0;
    if (now_unix_ms == 0) now_unix_ms = current_unix_ms();
    return impl_->prune_expired_locked(now_unix_ms, error).value_or(0);
}

bool TelemetryOutbox::purge(std::string* error) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        set_error(error, "outbox is not open");
        return false;
    }
    impl_->records.clear();
    impl_->total_payload_bytes = 0;
    impl_->losses.erase(std::remove_if(impl_->losses.begin(), impl_->losses.end(), [&](const auto& loss) {
        return impl_->loss_expiry_observed(loss.session_id);
    }), impl_->losses.end());
    impl_->expired_loss_contexts.clear();
    impl_->expired_batch_identities.clear();
    impl_->expiry_observation_overflow = false;
    impl_->expiry_retry_pending = false;
    if (!impl_->persist_locked(error)) {
        // A durable marker prevents reload even if deleting the old payload fails.
        const auto marker = std::filesystem::path(impl_->path.wstring() + L".revoked");
        const std::array<std::byte, 1> revoked{std::byte{1}};
        const bool marked = write_atomic_file(marker, revoked, error);
        std::error_code ec;
        const bool removed = std::filesystem::remove(impl_->path, ec);
        impl_->is_open = false;
        impl_->revocation_pending = true;
        if (!marked && (!removed || ec)) {
            set_error(error, "revoked payload remains: deletion and marker failed");
        } else {
            set_error(error, "failed to persist purged outbox; closed");
        }
        return false;
    }
    return true;
}

bool TelemetryOutbox::clear(std::string* error) {
    if (!impl_) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_open) {
        set_error(error, "outbox is not open");
        return false;
    }
    impl_->records.clear();
    impl_->total_payload_bytes = 0;
    impl_->losses.clear();
    impl_->active_session_id = {};
    impl_->expired_loss_contexts.clear();
    impl_->expired_batch_identities.clear();
    impl_->expiry_observation_overflow = false;
    impl_->expiry_retry_pending = false;
    if (!impl_->persist_locked(error)) {
        const auto marker = std::filesystem::path(impl_->path.wstring() + L".revoked");
        const std::array<std::byte, 1> revoked{std::byte{1}};
        const bool marked = write_atomic_file(marker, revoked, error);
        std::error_code ec;
        const bool removed = std::filesystem::remove(impl_->path, ec);
        impl_->is_open = false;
        impl_->revocation_pending = true;
        if (!marked && (!removed || ec)) {
            set_error(error, "cleared payload remains: deletion and marker failed");
        } else {
            set_error(error, "failed to persist cleared outbox; closed");
        }
        return false;
    }
    return true;
}

std::size_t TelemetryOutbox::size() const noexcept {
    if (!impl_) return 0;
    std::scoped_lock lock(impl_->mutex);
    return impl_->records.size();
}

std::size_t TelemetryOutbox::bytes() const noexcept {
    if (!impl_) return 0;
    std::scoped_lock lock(impl_->mutex);
    return impl_->total_payload_bytes;
}

bool TelemetryOutbox::empty() const noexcept {
    if (!impl_) return true;
    std::scoped_lock lock(impl_->mutex);
    return impl_->records.empty();
}

std::vector<TelemetryBatch> TelemetryOutbox::pending_batches() const {
    if (!impl_) return {};
    std::scoped_lock lock(impl_->mutex);
    std::vector<TelemetryBatch> result;
    result.reserve(impl_->records.size());
    for (const auto& rec : impl_->records) {
        result.push_back(rec.batch);
    }
    return result;
}

TelemetryOutboxAggregateLoss TelemetryOutbox::aggregate_loss() const noexcept {
    if (!impl_) return {};
    std::scoped_lock lock(impl_->mutex);
    if (impl_->expiry_observation_overflow || impl_->loss_expiry_observed(impl_->active_session_id)) return {};
    const auto it = std::find_if(impl_->losses.begin(), impl_->losses.end(), [&](const auto& loss) {
        return loss.session_id == impl_->active_session_id;
    });
    return it == impl_->losses.end() ? TelemetryOutboxAggregateLoss{} : *it;
}

TelemetryOutboxAggregateLoss TelemetryOutbox::aggregate_loss(const SessionId& session_id) const noexcept {
    if (!impl_) return {};
    std::scoped_lock lock(impl_->mutex);
    if (impl_->expiry_observation_overflow || impl_->loss_expiry_observed(session_id)) return {};
    const auto it = std::find_if(impl_->losses.begin(), impl_->losses.end(), [&](const auto& loss) {
        return loss.session_id == session_id;
    });
    return it == impl_->losses.end() ? TelemetryOutboxAggregateLoss{} : *it;
}

} // namespace nstu::exam
