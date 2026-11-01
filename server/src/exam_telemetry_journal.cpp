#include "nstu/exam_telemetry_journal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nstu::server {
namespace {

inline constexpr std::uint32_t kDetailRecordMagic = 0x3144544eu; // "NTD1"
inline constexpr std::uint16_t kDetailRecordVersion = 1;

inline constexpr std::uint32_t kMetadataMagic = 0x314d544eu;     // "NTM1"
inline constexpr std::uint16_t kMetadataVersion = 1;

void set_err(std::string* err, std::string_view msg) {
    if (err != nullptr) {
        *err = std::string(msg);
    }
}

template <typename T>
void append_le(std::vector<std::byte>& out, T value) {
    static_assert(std::is_unsigned_v<T>);
    const auto offset = out.size();
    out.resize(offset + sizeof(T));
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out[offset + i] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> in, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > in.size() || sizeof(T) > in.size() - offset) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(std::to_integer<unsigned int>(in[offset++])) << (i * 8u);
    }
    return true;
}

template <std::size_t N>
void append_array(std::vector<std::byte>& out, const std::array<std::byte, N>& arr) {
    out.insert(out.end(), arr.begin(), arr.end());
}

template <std::size_t N>
bool read_array(std::span<const std::byte> in, std::size_t& offset, std::array<std::byte, N>& arr) {
    if (offset > in.size() || N > in.size() - offset) {
        return false;
    }
    std::copy_n(in.data() + offset, N, arr.data());
    offset += N;
    return true;
}

struct ArrayHash {
    template <std::size_t N>
    std::size_t operator()(const std::array<std::byte, N>& arr) const noexcept {
        std::size_t h = 0xcbf29ce484222325ULL;
        for (const auto b : arr) {
            h ^= static_cast<std::size_t>(b);
            h *= 0x100000001b3ULL;
        }
        return h;
    }
};

} // namespace

struct ProducerRecord {
    exam::ProducerId producer_id{};
    bool admitted = false;
    bool retired = false;
    std::uint32_t generation = 0;
    std::uint64_t current_sequence = 0;
    std::uint64_t replay_floor = 0;
    std::map<std::uint64_t, std::pair<security::Sha256Digest, std::uint64_t>> tracked_sequences; // seq -> (hash, server_time)
    std::map<std::uint64_t, security::Sha256Digest> conflicts; // seq -> conflict_hash
};

struct ContextRecord {
    exam::SessionId session_id{};
    bool authorized = false;
    std::uint64_t authorized_at_unix_ms = 0;
    bool stopped = false;
    std::uint32_t lifetime_generations = 0;
    std::unordered_map<exam::ProducerId, ProducerRecord, ArrayHash> producers;
};

struct ExamTelemetryJournal::Impl {
    mutable std::mutex mutex;
    std::filesystem::path journal_dir;
    bool is_open = false;
    bool disk_failure_injection = false;
    std::size_t segment_size_limit_for_testing = 0;

    [[nodiscard]] std::size_t effective_segment_bytes() const noexcept {
        return segment_size_limit_for_testing > 0 ? segment_size_limit_for_testing : kDetailSegmentBytes;
    }

    std::size_t active_segment = 0;
    std::size_t active_segment_bytes = 0;
    std::array<std::size_t, kDetailSegmentCount> segment_sizes{};

    std::unordered_map<exam::SessionId, ContextRecord, ArrayHash> contexts;

    [[nodiscard]] std::filesystem::path segment_path(std::size_t index) const {
        return journal_dir / ("segment_" + std::to_string(index) + ".bin");
    }

    [[nodiscard]] std::filesystem::path metadata_path() const {
        return journal_dir / "metadata.bin";
    }

    void update_segment_sizes() {
        for (std::size_t i = 0; i < kDetailSegmentCount; ++i) {
            const auto p = segment_path(i);
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) {
                segment_sizes[i] = static_cast<std::size_t>(std::filesystem::file_size(p, ec));
            } else {
                segment_sizes[i] = 0;
            }
        }
    }

    [[nodiscard]] std::size_t total_detail_bytes() const noexcept {
        std::size_t total = 0;
        for (const auto sz : segment_sizes) {
            total += sz;
        }
        return total;
    }

    [[nodiscard]] std::size_t total_tracked_hashes() const noexcept {
        std::size_t total = 0;
        for (const auto& [_, ctx] : contexts) {
            for (const auto& [__, prod] : ctx.producers) {
                total += prod.tracked_sequences.size();
            }
        }
        return total;
    }

    bool is_context_active(const ContextRecord& ctx, std::uint64_t now_unix_ms) const noexcept {
        if (!ctx.authorized || ctx.stopped) {
            return false;
        }
        if (now_unix_ms > 0 &&
            now_unix_ms >= ctx.authorized_at_unix_ms + kTelemetryMaxActiveContextMilliseconds) {
            return false;
        }
        return true;
    }

    bool persist_metadata(std::string* err) {
        if (disk_failure_injection) {
            set_err(err, "simulated disk failure");
            return false;
        }

        std::vector<std::byte> wire;
        append_le(wire, kMetadataMagic);
        append_le(wire, kMetadataVersion);
        append_le(wire, static_cast<std::uint32_t>(active_segment));
        append_le(wire, static_cast<std::uint64_t>(active_segment_bytes));

        append_le(wire, static_cast<std::uint32_t>(contexts.size()));
        for (const auto& [_, ctx] : contexts) {
            append_array(wire, ctx.session_id);
            append_le(wire, static_cast<std::uint8_t>(ctx.authorized ? 1 : 0));
            append_le(wire, static_cast<std::uint8_t>(ctx.stopped ? 1 : 0));
            append_le(wire, static_cast<std::uint16_t>(0)); // padding
            append_le(wire, ctx.authorized_at_unix_ms);
            append_le(wire, ctx.lifetime_generations);

            append_le(wire, static_cast<std::uint32_t>(ctx.producers.size()));
            for (const auto& [__, prod] : ctx.producers) {
                append_array(wire, prod.producer_id);
                append_le(wire, static_cast<std::uint8_t>(prod.admitted ? 1 : 0));
                append_le(wire, static_cast<std::uint8_t>(prod.retired ? 1 : 0));
                append_le(wire, static_cast<std::uint16_t>(0)); // padding
                append_le(wire, prod.generation);
                append_le(wire, prod.current_sequence);
                append_le(wire, prod.replay_floor);

                append_le(wire, static_cast<std::uint32_t>(prod.tracked_sequences.size()));
                for (const auto& [seq, hash_time] : prod.tracked_sequences) {
                    append_le(wire, seq);
                    append_le(wire, hash_time.second);
                    append_array(wire, hash_time.first);
                }

                append_le(wire, static_cast<std::uint32_t>(prod.conflicts.size()));
                for (const auto& [seq, conflict_hash] : prod.conflicts) {
                    append_le(wire, seq);
                    append_array(wire, conflict_hash);
                }
            }
        }

        const auto digest = security::sha256(wire);
        if (!digest.has_value()) {
            set_err(err, "digest calculation failed");
            return false;
        }
        append_array(wire, *digest);

        const auto tmp_p = journal_dir / "metadata.bin.tmp";
        const auto meta_p = metadata_path();

        std::ofstream out(tmp_p, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_err(err, "cannot open metadata tmp for write");
            return false;
        }
        out.write(reinterpret_cast<const char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
        out.flush();
        if (!out) {
            set_err(err, "write error on metadata tmp");
            return false;
        }
        out.close();

        std::error_code ec;
        std::filesystem::rename(tmp_p, meta_p, ec);
        if (ec) {
            set_err(err, "rename failed on metadata: " + ec.message());
            return false;
        }
        return true;
    }

    bool load_metadata() {
        const auto meta_p = metadata_path();
        std::error_code ec;
        if (!std::filesystem::exists(meta_p, ec)) {
            return false;
        }

        const auto sz = std::filesystem::file_size(meta_p, ec);
        if (sz < 16 + security::kSha256Bytes || sz > 32u * 1024u * 1024u) {
            return false;
        }

        std::ifstream in(meta_p, std::ios::binary);
        if (!in) {
            return false;
        }

        std::vector<std::byte> wire(static_cast<std::size_t>(sz));
        in.read(reinterpret_cast<char*>(wire.data()), static_cast<std::streamsize>(sz));
        if (in.gcount() != static_cast<std::streamsize>(sz)) {
            return false;
        }

        const auto payload_size = wire.size() - security::kSha256Bytes;
        const auto calculated_digest = security::sha256(std::span<const std::byte>(wire.data(), payload_size));
        if (!calculated_digest.has_value()) {
            return false;
        }
        security::Sha256Digest stored_digest{};
        std::copy_n(wire.data() + payload_size, security::kSha256Bytes, stored_digest.data());
        if (*calculated_digest != stored_digest) {
            return false;
        }

        std::size_t offset = 0;
        std::uint32_t magic = 0;
        std::uint16_t ver = 0;
        std::uint32_t loaded_active_seg = 0;
        std::uint64_t loaded_active_seg_bytes = 0;

        if (!read_le(wire, offset, magic) || magic != kMetadataMagic ||
            !read_le(wire, offset, ver) || ver != kMetadataVersion ||
            !read_le(wire, offset, loaded_active_seg) ||
            !read_le(wire, offset, loaded_active_seg_bytes)) {
            return false;
        }

        active_segment = loaded_active_seg % kDetailSegmentCount;
        active_segment_bytes = static_cast<std::size_t>(loaded_active_seg_bytes);

        std::uint32_t num_contexts = 0;
        if (!read_le(wire, offset, num_contexts)) {
            return false;
        }

        std::unordered_map<exam::SessionId, ContextRecord, ArrayHash> loaded_contexts;
        for (std::uint32_t ci = 0; ci < num_contexts; ++ci) {
            ContextRecord ctx;
            std::uint8_t auth_byte = 0;
            std::uint8_t stop_byte = 0;
            std::uint16_t pad = 0;
            if (!read_array(wire, offset, ctx.session_id) ||
                !read_le(wire, offset, auth_byte) ||
                !read_le(wire, offset, stop_byte) ||
                !read_le(wire, offset, pad) ||
                !read_le(wire, offset, ctx.authorized_at_unix_ms) ||
                !read_le(wire, offset, ctx.lifetime_generations)) {
                return false;
            }
            ctx.authorized = (auth_byte != 0);
            ctx.stopped = (stop_byte != 0);

            std::uint32_t num_prods = 0;
            if (!read_le(wire, offset, num_prods)) {
                return false;
            }

            for (std::uint32_t pi = 0; pi < num_prods; ++pi) {
                ProducerRecord prod;
                std::uint8_t adm_byte = 0;
                std::uint8_t ret_byte = 0;
                std::uint16_t prod_pad = 0;
                if (!read_array(wire, offset, prod.producer_id) ||
                    !read_le(wire, offset, adm_byte) ||
                    !read_le(wire, offset, ret_byte) ||
                    !read_le(wire, offset, prod_pad) ||
                    !read_le(wire, offset, prod.generation) ||
                    !read_le(wire, offset, prod.current_sequence) ||
                    !read_le(wire, offset, prod.replay_floor)) {
                    return false;
                }
                prod.admitted = (adm_byte != 0);
                prod.retired = (ret_byte != 0);

                std::uint32_t num_seqs = 0;
                if (!read_le(wire, offset, num_seqs)) {
                    return false;
                }
                for (std::uint32_t si = 0; si < num_seqs; ++si) {
                    std::uint64_t seq = 0;
                    std::uint64_t srv_time = 0;
                    security::Sha256Digest hash{};
                    if (!read_le(wire, offset, seq) ||
                        !read_le(wire, offset, srv_time) ||
                        !read_array(wire, offset, hash)) {
                        return false;
                    }
                    prod.tracked_sequences[seq] = {hash, srv_time};
                }

                std::uint32_t num_conflicts = 0;
                if (!read_le(wire, offset, num_conflicts)) {
                    return false;
                }
                for (std::uint32_t cfi = 0; cfi < num_conflicts; ++cfi) {
                    std::uint64_t seq = 0;
                    security::Sha256Digest chash{};
                    if (!read_le(wire, offset, seq) ||
                        !read_array(wire, offset, chash)) {
                        return false;
                    }
                    prod.conflicts[seq] = chash;
                }

                ctx.producers[prod.producer_id] = std::move(prod);
            }
            loaded_contexts[ctx.session_id] = std::move(ctx);
        }

        contexts = std::move(loaded_contexts);
        return true;
    }

    void recover_and_sanitize_segments() {
        for (std::size_t i = 0; i < kDetailSegmentCount; ++i) {
            const auto p = segment_path(i);
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                segment_sizes[i] = 0;
                continue;
            }

            const auto file_size = std::filesystem::file_size(p, ec);
            if (file_size == 0) {
                segment_sizes[i] = 0;
                continue;
            }

            std::ifstream in(p, std::ios::binary);
            if (!in) {
                continue;
            }

            std::vector<std::byte> file_data(static_cast<std::size_t>(file_size));
            in.read(reinterpret_cast<char*>(file_data.data()), static_cast<std::streamsize>(file_size));
            const auto bytes_read = static_cast<std::size_t>(in.gcount());
            in.close();

            std::size_t valid_offset = 0;
            std::size_t scan = 0;
            while (scan + 104 <= bytes_read) {
                std::size_t rec_offset = scan;
                std::uint32_t magic = 0;
                std::uint16_t ver = 0;
                std::uint16_t reserved = 0;
                std::uint32_t rec_len = 0;

                if (!read_le(file_data, rec_offset, magic) || magic != kDetailRecordMagic ||
                    !read_le(file_data, rec_offset, ver) || ver != kDetailRecordVersion ||
                    !read_le(file_data, rec_offset, reserved) ||
                    !read_le(file_data, rec_offset, rec_len) ||
                    rec_len < 104 || scan + rec_len > bytes_read) {
                    break; // Torn tail / incomplete record
                }

                const auto rec_span = std::span<const std::byte>(file_data.data() + scan, rec_len);
                const auto payload_size = rec_len - security::kSha256Bytes;
                const auto checksum = security::sha256(rec_span.first(payload_size));
                if (!checksum.has_value()) {
                    break;
                }
                security::Sha256Digest stored_checksum{};
                std::copy_n(rec_span.data() + payload_size, security::kSha256Bytes, stored_checksum.data());

                if (*checksum != stored_checksum) {
                    break; // Checksum failure / corrupted tail
                }

                scan += rec_len;
                valid_offset = scan;
            }

            if (valid_offset < bytes_read) {
                // Truncate corrupted / torn tail cleanly
                std::filesystem::resize_file(p, valid_offset, ec);
            }
            segment_sizes[i] = valid_offset;
        }

        active_segment_bytes = segment_sizes[active_segment];
    }

    void close_locked() noexcept {
        if (!is_open) {
            return;
        }
        std::string err;
        persist_metadata(&err);
        is_open = false;
    }

    std::size_t prune_expired_locked(std::uint64_t now_unix_ms, std::string* error) {
        if (!is_open) {
            set_err(error, "journal not open");
            return 0;
        }

        const std::uint64_t retention_threshold =
            (now_unix_ms >= kTelemetryRetentionMilliseconds)
                ? (now_unix_ms - kTelemetryRetentionMilliseconds)
                : 0;

        std::size_t pruned_count = 0;
        for (auto& [_, ctx] : contexts) {
            for (auto& [__, prod] : ctx.producers) {
                auto it = prod.tracked_sequences.begin();
                while (it != prod.tracked_sequences.end()) {
                    const auto server_received_time = it->second.second;
                    if (server_received_time < retention_threshold) {
                        prod.replay_floor = std::max(prod.replay_floor, it->first);
                        it = prod.tracked_sequences.erase(it);
                        ++pruned_count;
                    } else {
                        ++it;
                    }
                }
            }
        }

        if (pruned_count > 0) {
            persist_metadata(error);
        }
        return pruned_count;
    }
};

ExamTelemetryJournal::ExamTelemetryJournal() : impl_(std::make_unique<Impl>()) {}
ExamTelemetryJournal::~ExamTelemetryJournal() = default;
ExamTelemetryJournal::ExamTelemetryJournal(ExamTelemetryJournal&&) noexcept = default;
ExamTelemetryJournal& ExamTelemetryJournal::operator=(ExamTelemetryJournal&&) noexcept = default;

bool ExamTelemetryJournal::open(const std::filesystem::path& journal_dir, std::string* error) {
    return open(journal_dir, 0, error);
}

bool ExamTelemetryJournal::open(const std::filesystem::path& journal_dir, std::uint64_t now_unix_ms, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->is_open) {
        impl_->close_locked();
    }

    std::error_code ec;
    std::filesystem::create_directories(journal_dir, ec);
    if (ec) {
        set_err(error, "cannot create journal directory: " + ec.message());
        return false;
    }

    impl_->journal_dir = journal_dir;
    impl_->load_metadata();
    impl_->recover_and_sanitize_segments();
    impl_->update_segment_sizes();
    impl_->is_open = true;

    if (now_unix_ms > 0) {
        impl_->prune_expired_locked(now_unix_ms, nullptr);
    }
    return true;
}

void ExamTelemetryJournal::close() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->close_locked();
}

bool ExamTelemetryJournal::is_open() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->is_open;
}

bool ExamTelemetryJournal::authorize_context(
    const exam::SessionId& session_id,
    std::uint64_t authorized_at_unix_ms,
    std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->is_open) {
        set_err(error, "journal not open");
        return false;
    }
    if (impl_->contexts.size() >= kMaximumActiveContexts &&
        !impl_->contexts.contains(session_id)) {
        set_err(error, "active context quota exceeded");
        return false;
    }

    auto& ctx = impl_->contexts[session_id];
    ctx.session_id = session_id;
    ctx.authorized = true;
    ctx.authorized_at_unix_ms = authorized_at_unix_ms;
    ctx.stopped = false;

    return impl_->persist_metadata(error);
}

bool ExamTelemetryJournal::is_context_authorized(
    const exam::SessionId& session_id,
    std::uint64_t now_unix_ms) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->is_open) {
        return false;
    }
    const auto it = impl_->contexts.find(session_id);
    if (it == impl_->contexts.end()) {
        return false;
    }
    return impl_->is_context_active(it->second, now_unix_ms);
}

bool ExamTelemetryJournal::stop_authorization(
    const exam::SessionId& session_id,
    std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->is_open) {
        set_err(error, "journal not open");
        return false;
    }
    const auto it = impl_->contexts.find(session_id);
    if (it == impl_->contexts.end()) {
        return true;
    }

    it->second.stopped = true;
    // When telemetry authorization ends, replay floor is deleted; no behavioral detail survives
    for (auto& [_, prod] : it->second.producers) {
        prod.replay_floor = 0;
        prod.tracked_sequences.clear();
        prod.conflicts.clear();
    }

    return impl_->persist_metadata(error);
}

bool ExamTelemetryJournal::remove_context(
    const exam::SessionId& session_id,
    std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->is_open) {
        set_err(error, "journal not open");
        return false;
    }
    impl_->contexts.erase(session_id);
    return impl_->persist_metadata(error);
}

bool ExamTelemetryJournal::retire_producer(
    const exam::SessionId& session_id,
    const exam::ProducerId& producer_id,
    std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->is_open) {
        set_err(error, "journal not open");
        return false;
    }
    const auto it = impl_->contexts.find(session_id);
    if (it == impl_->contexts.end()) {
        set_err(error, "context not found");
        return false;
    }
    auto& ctx = it->second;
    const auto pit = ctx.producers.find(producer_id);
    if (pit == ctx.producers.end()) {
        set_err(error, "producer not found");
        return false;
    }
    pit->second.retired = true;
    return impl_->persist_metadata(error);
}

bool ExamTelemetryJournal::is_producer_admitted(
    const exam::SessionId& session_id,
    const exam::ProducerId& producer_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->contexts.find(session_id);
    if (it == impl_->contexts.end()) {
        return false;
    }
    const auto pit = it->second.producers.find(producer_id);
    if (pit == it->second.producers.end()) {
        return false;
    }
    return pit->second.admitted;
}

bool ExamTelemetryJournal::is_producer_retired(
    const exam::SessionId& session_id,
    const exam::ProducerId& producer_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->contexts.find(session_id);
    if (it == impl_->contexts.end()) {
        return false;
    }
    const auto pit = it->second.producers.find(producer_id);
    if (pit == it->second.producers.end()) {
        return false;
    }
    return pit->second.retired;
}

std::uint32_t ExamTelemetryJournal::producer_generation(
    const exam::SessionId& session_id,
    const exam::ProducerId& producer_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->contexts.find(session_id);
    if (it == impl_->contexts.end()) {
        return 0;
    }
    const auto pit = it->second.producers.find(producer_id);
    if (pit == it->second.producers.end()) {
        return 0;
    }
    return pit->second.generation;
}

std::uint64_t ExamTelemetryJournal::producer_replay_floor(
    const exam::SessionId& session_id,
    const exam::ProducerId& producer_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->contexts.find(session_id);
    if (it == impl_->contexts.end()) {
        return 0;
    }
    // If telemetry authorization ended, replay floor is 0 (deleted)
    if (it->second.stopped || !it->second.authorized) {
        return 0;
    }
    const auto pit = it->second.producers.find(producer_id);
    if (pit == it->second.producers.end()) {
        return 0;
    }
    return pit->second.replay_floor;
}

exam::TelemetryAck ExamTelemetryJournal::record_batch(
    const exam::TelemetryBatch& batch,
    std::uint64_t now_unix_ms,
    std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    exam::TelemetryAck ack;
    ack.session_id = batch.session_id;
    ack.producer_id = batch.producer_id;
    ack.sequence = batch.sequence;
    ack.server_time_unix_milliseconds = now_unix_ms;
    ack.status = exam::TelemetryAckStatus::rejected;

    if (!impl_->is_open) {
        set_err(error, "journal not open");
        ack.status = exam::TelemetryAckStatus::unavailable;
        return ack;
    }

    if (!exam::validate_telemetry_batch(batch)) {
        set_err(error, "invalid telemetry batch");
        return ack;
    }

    const auto canonical_hash_opt = exam::compute_telemetry_batch_hash(batch);
    if (!canonical_hash_opt.has_value()) {
        set_err(error, "cannot compute batch canonical hash");
        return ack;
    }
    const auto canonical_hash = *canonical_hash_opt;
    ack.batch_hash = canonical_hash;

    // Check active telemetry authorization
    const auto cit = impl_->contexts.find(batch.session_id);
    if (cit == impl_->contexts.end() || !impl_->is_context_active(cit->second, now_unix_ms)) {
        set_err(error, "inactive telemetry authorization");
        return ack;
    }
    auto& ctx = cit->second;

    auto pit = ctx.producers.find(batch.producer_id);
    const bool is_unseen = (pit == ctx.producers.end() || !pit->second.admitted);

    if (is_unseen) {
        // Unseen producer checks
        if (pit != ctx.producers.end() && pit->second.retired) {
            set_err(error, "retired producer ID cannot register again");
            return ack;
        }

        if (ctx.lifetime_generations >= kMaximumLifetimeGenerationsPerContext) {
            set_err(error, "lifetime generation cap exceeded for context");
            return ack;
        }

        if (batch.sequence != 1) {
            set_err(error, "first valid batch for unseen producer must register at sequence 1");
            return ack;
        }

        // Admit producer
        ctx.lifetime_generations += 1;
        auto& prod = ctx.producers[batch.producer_id];
        prod.producer_id = batch.producer_id;
        prod.admitted = true;
        prod.retired = false;
        prod.generation = ctx.lifetime_generations;
        prod.current_sequence = 1;
        prod.replay_floor = 0;
        pit = ctx.producers.find(batch.producer_id);
    }

    auto& prod = pit->second;
    if (prod.retired) {
        set_err(error, "producer retired");
        return ack;
    }

    // Check conflict table
    const auto cfit = prod.conflicts.find(batch.sequence);
    if (cfit != prod.conflicts.end()) {
        ack.status = exam::TelemetryAckStatus::conflict;
        set_err(error, "sequence is in permanent conflict");
        return ack;
    }

    // Check existing sequence in dedup index
    const auto seq_it = prod.tracked_sequences.find(batch.sequence);
    if (seq_it != prod.tracked_sequences.end()) {
        if (seq_it->second.first == canonical_hash) {
            ack.status = exam::TelemetryAckStatus::duplicate;
            return ack; // Exact duplicate consumes zero extra storage
        } else {
            // Hash mismatch on same sequence -> CONFLICT (terminal)
            prod.conflicts[batch.sequence] = canonical_hash;
            impl_->persist_metadata(nullptr);
            ack.status = exam::TelemetryAckStatus::conflict;
            set_err(error, "batch conflict on sequence");
            return ack;
        }
    }

    // Check replay floor
    if (batch.sequence <= prod.replay_floor) {
        ack.status = exam::TelemetryAckStatus::rejected;
        set_err(error, "sequence rejected by replay floor");
        return ack;
    }

    // Check simulated disk failure injection
    if (impl_->disk_failure_injection) {
        ack.status = exam::TelemetryAckStatus::unavailable;
        set_err(error, "storage unavailable");
        return ack;
    }

    // Encode batch wire
    const auto wire_batch = exam::encode_telemetry_batch(batch);
    if (wire_batch.empty()) {
        ack.status = exam::TelemetryAckStatus::rejected;
        set_err(error, "encode batch wire failed");
        return ack;
    }

    // Prepare detail record
    // Header: magic (4), ver (2), reserved (2), rec_len (4), server_time (8),
    // session_id (16), producer_id (16), sequence (8), client_time (8),
    // batch_hash (32), wire_payload_size (4) = 104 bytes
    const std::uint32_t record_len = static_cast<std::uint32_t>(104 + wire_batch.size() + security::kSha256Bytes);

    std::vector<std::byte> record_bytes;
    record_bytes.reserve(record_len);
    append_le(record_bytes, kDetailRecordMagic);
    append_le(record_bytes, kDetailRecordVersion);
    append_le(record_bytes, static_cast<std::uint16_t>(0));
    append_le(record_bytes, record_len);
    append_le(record_bytes, now_unix_ms);
    append_array(record_bytes, batch.session_id);
    append_array(record_bytes, batch.producer_id);
    append_le(record_bytes, batch.sequence);
    append_le(record_bytes, batch.first_enqueued_at_unix_milliseconds);
    append_array(record_bytes, canonical_hash);
    append_le(record_bytes, static_cast<std::uint32_t>(wire_batch.size()));
    record_bytes.insert(record_bytes.end(), wire_batch.begin(), wire_batch.end());

    const auto rec_checksum = security::sha256(record_bytes);
    if (!rec_checksum.has_value()) {
        ack.status = exam::TelemetryAckStatus::unavailable;
        set_err(error, "cannot compute record checksum");
        return ack;
    }
    append_array(record_bytes, *rec_checksum);

    // Detail store segment rotation check
    if (impl_->active_segment_bytes + record_bytes.size() > impl_->effective_segment_bytes()) {
        impl_->active_segment = (impl_->active_segment + 1) % kDetailSegmentCount;
        // Truncate/overwrite the rotated segment (evicting older records in that segment)
        const auto p = impl_->segment_path(impl_->active_segment);
        std::ofstream trunc_out(p, std::ios::binary | std::ios::trunc);
        trunc_out.close();
        impl_->segment_sizes[impl_->active_segment] = 0;
        impl_->active_segment_bytes = 0;
    }

    const auto seg_p = impl_->segment_path(impl_->active_segment);
    std::ofstream seg_out(seg_p, std::ios::binary | std::ios::app);
    if (!seg_out) {
        ack.status = exam::TelemetryAckStatus::unavailable;
        set_err(error, "cannot open segment for append");
        return ack;
    }
    seg_out.write(reinterpret_cast<const char*>(record_bytes.data()), static_cast<std::streamsize>(record_bytes.size()));
    seg_out.flush();
    if (!seg_out) {
        ack.status = exam::TelemetryAckStatus::unavailable;
        set_err(error, "segment write failure");
        return ack;
    }
    seg_out.close();

    impl_->active_segment_bytes += record_bytes.size();
    impl_->segment_sizes[impl_->active_segment] = impl_->active_segment_bytes;

    // Update deduplication index
    prod.tracked_sequences[batch.sequence] = {canonical_hash, now_unix_ms};
    prod.current_sequence = std::max(prod.current_sequence, batch.sequence);

    // Enforce quota: <= 4096 hashes per producer
    while (prod.tracked_sequences.size() > kMaximumHashesPerProducer) {
        const auto oldest_it = prod.tracked_sequences.begin();
        prod.replay_floor = std::max(prod.replay_floor, oldest_it->first);
        prod.tracked_sequences.erase(oldest_it);
    }

    // Persist metadata
    impl_->persist_metadata(nullptr);

    ack.status = exam::TelemetryAckStatus::accepted;
    return ack;
}

std::size_t ExamTelemetryJournal::prune_expired(std::uint64_t now_unix_ms, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->prune_expired_locked(now_unix_ms, error);
}

void ExamTelemetryJournal::set_disk_failure_injection(bool enable) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->disk_failure_injection = enable;
}

void ExamTelemetryJournal::set_segment_size_limit_for_testing(std::size_t bytes) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->segment_size_limit_for_testing = bytes;
}

std::size_t ExamTelemetryJournal::active_context_count() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t count = 0;
    for (const auto& [_, ctx] : impl_->contexts) {
        if (ctx.authorized && !ctx.stopped) {
            ++count;
        }
    }
    return count;
}

std::size_t ExamTelemetryJournal::tracked_hash_count() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total_tracked_hashes();
}

std::size_t ExamTelemetryJournal::detail_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total_detail_bytes();
}

std::size_t ExamTelemetryJournal::index_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::error_code ec;
    return static_cast<std::size_t>(std::filesystem::file_size(impl_->metadata_path(), ec));
}

std::size_t ExamTelemetryJournal::active_segment_index() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->active_segment;
}

std::size_t ExamTelemetryJournal::segment_count() const noexcept {
    return kDetailSegmentCount;
}

} // namespace nstu::server
