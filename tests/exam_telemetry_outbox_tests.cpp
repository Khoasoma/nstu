#include "nstu/exam_telemetry_outbox.hpp"
#include "nstu/secret_store.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace {

nstu::exam::TelemetrySample make_sample(std::uint64_t collector_id,
                                        std::uint32_t count = 1) {
    nstu::exam::TelemetrySample sample;
    sample.kind = nstu::exam::TelemetrySampleKind::question_first_view;
    sample.source = nstu::exam::TelemetrySource::native;
    sample.collector_id = collector_id;
    sample.question_id = "reading-q1";
    sample.question_revision = 1;
    sample.monotonic_elapsed_milliseconds = 1000;
    sample.duration_milliseconds = 0;
    sample.client_time_unix_milliseconds = 1700000000000ULL;
    sample.numeric_value = 0;
    sample.count = count;
    sample.reason = nstu::exam::TelemetryReason::navigation_button;
    return sample;
}

std::uint64_t wall_clock_ms();

nstu::exam::TelemetryBatch make_batch(
    std::uint64_t sequence,
    std::uint8_t producer_marker = 1,
    std::uint64_t enqueue_time = 0) {
    nstu::exam::TelemetryBatch batch;
    batch.package_id = "test-pkg";
    batch.package_digest = nstu::security::Sha256Digest{std::byte{0xaa}};
    batch.client_id = nstu::security::ClientId{std::byte{0xbb}};
    batch.session_id = nstu::exam::SessionId{std::byte{0xcc}};
    batch.candidate_id = "candidate-1";
    batch.producer_id = nstu::exam::ProducerId{std::byte{producer_marker}};
    batch.sequence = sequence;
    batch.first_enqueued_at_unix_milliseconds = enqueue_time == 0 ? wall_clock_ms() : enqueue_time;
    batch.samples.push_back(make_sample(sequence));
    return batch;
}

std::filesystem::path temp_test_path(const std::string& suffix) {
    const auto temp_dir = std::filesystem::temp_directory_path() / "nstu_telemetry_outbox_tests";
    std::filesystem::create_directories(temp_dir);
    return temp_dir / ("outbox_" + suffix + ".bin");
}

std::uint64_t wall_clock_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::vector<std::filesystem::path> quarantine_files(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> files;
    const auto prefix = path.filename().wstring() + L".corrupt-";
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.path().filename().wstring().rfind(prefix, 0) == 0) {
            files.push_back(entry.path());
        }
    }
    return files;
}

void write_raw_outbox(
    const std::filesystem::path& path,
    const std::vector<nstu::exam::TelemetryBatch>& batches,
    std::uint32_t batch_count_field, bool corrupt_digest = false) {
    constexpr std::uint32_t kOutboxMagic = 0x544c4d4f;
    constexpr std::uint16_t kOutboxVersion = 1;
    constexpr char kOutboxEntropyText[] = "NSTU-Exam-Telemetry-Outbox-v1";
    std::span<const std::byte> entropy{
        reinterpret_cast<const std::byte*>(kOutboxEntropyText),
        sizeof(kOutboxEntropyText) - 1};

    std::vector<std::byte> plaintext;
    auto append_u16 = [&](std::uint16_t v) {
        plaintext.push_back(static_cast<std::byte>(v & 0xffu));
        plaintext.push_back(static_cast<std::byte>((v >> 8u) & 0xffu));
    };
    auto append_u32 = [&](std::uint32_t v) {
        plaintext.push_back(static_cast<std::byte>(v & 0xffu));
        plaintext.push_back(static_cast<std::byte>((v >> 8u) & 0xffu));
        plaintext.push_back(static_cast<std::byte>((v >> 16u) & 0xffu));
        plaintext.push_back(static_cast<std::byte>((v >> 24u) & 0xffu));
    };
    auto append_u64 = [&](std::uint64_t v) {
        for (std::size_t i = 0; i < 8; ++i) {
            plaintext.push_back(static_cast<std::byte>((v >> (i * 8u)) & 0xffu));
        }
    };

    append_u32(kOutboxMagic);
    append_u16(kOutboxVersion);
    append_u16(0); // reserved

    append_u64(0); // rejected
    append_u64(0); // conflict
    append_u64(0); // expired
    append_u64(0); // first_recorded
    append_u64(0); // last_updated

    append_u32(batch_count_field);

    for (std::size_t i = 0; i < batches.size(); ++i) {
        const auto enc = nstu::exam::encode_telemetry_batch(batches[i]);
        assert(!enc.empty());
        append_u32(static_cast<std::uint32_t>(enc.size()));
        plaintext.insert(plaintext.end(), enc.begin(), enc.end());
        append_u16(0); // retry_count
        append_u64(0); // next_eligible_retry_time_ms
    }

    const auto digest = nstu::security::sha256(plaintext);
    assert(digest.has_value());
    plaintext.insert(plaintext.end(), digest->begin(), digest->end());
    if (corrupt_digest) plaintext.back() ^= std::byte{1};

    std::string error;
    const auto protected_blob = nstu::security::protect_machine_secret(plaintext, entropy, &error);
    assert(!protected_blob.empty());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.is_open());
    out.write(reinterpret_cast<const char*>(protected_blob.data()),
              static_cast<std::streamsize>(protected_blob.size()));
    out.flush();
    assert(out.good());
}

void test_outbox_open_and_enqueue() {
    const auto path = temp_test_path("open_enqueue");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    std::string error;
    assert(outbox.open(path, &error));
    assert(outbox.is_open());
    assert(outbox.empty());
    assert(outbox.size() == 0);

    const auto b1 = make_batch(1);
    assert(outbox.enqueue(b1, &error));
    assert(outbox.size() == 1);
    assert(!outbox.empty());

    // Calling open() while already open must succeed without deadlocking
    assert(outbox.open(path, &error));
    assert(outbox.is_open());

    // Duplicate sequence for same producer rejected
    assert(!outbox.enqueue(b1, &error));
    assert(outbox.size() == 1);

    // Sequence 2 succeeds
    const auto b2 = make_batch(2);
    assert(outbox.enqueue(b2, &error));
    assert(outbox.size() == 2);
}

void test_outbox_ack_handling() {
    const auto path = temp_test_path("ack_handling");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    std::string error;
    assert(outbox.open(path, &error));

    const auto b1 = make_batch(1);
    const auto b2 = make_batch(2);
    const auto b3 = make_batch(3);
    const auto b4 = make_batch(4);

    assert(outbox.enqueue(b1));
    assert(outbox.enqueue(b2));
    assert(outbox.enqueue(b3));
    assert(outbox.enqueue(b4));
    assert(outbox.size() == 4);

    const auto hash1 = *nstu::exam::compute_telemetry_batch_hash(b1);
    const auto hash2 = *nstu::exam::compute_telemetry_batch_hash(b2);
    const auto hash3 = *nstu::exam::compute_telemetry_batch_hash(b3);
    const auto hash4 = *nstu::exam::compute_telemetry_batch_hash(b4);

    // 1. Accepted ACK retires b1
    nstu::exam::TelemetryAck ack1;
    ack1.status = nstu::exam::TelemetryAckStatus::accepted;
    ack1.session_id = b1.session_id;
    ack1.producer_id = b1.producer_id;
    ack1.sequence = b1.sequence;
    ack1.batch_hash = hash1;
    ack1.server_time_unix_milliseconds = 1700000010000ULL;

    const auto res1 = outbox.acknowledge(ack1);
    assert(res1 == nstu::exam::TelemetryAckApplyResult::retired);
    assert(outbox.size() == 3);

    // 2. Duplicate ACK with exact matching hash retires b2
    nstu::exam::TelemetryAck ack2 = ack1;
    ack2.status = nstu::exam::TelemetryAckStatus::duplicate;
    ack2.sequence = b2.sequence;
    ack2.batch_hash = hash2;

    const auto res2 = outbox.acknowledge(ack2);
    assert(res2 == nstu::exam::TelemetryAckApplyResult::retired);
    assert(outbox.size() == 2);

    // 3. Rejected ACK is terminal: deletes payload immediately, increments aggregate rejected
    nstu::exam::TelemetryAck ack3 = ack1;
    ack3.status = nstu::exam::TelemetryAckStatus::rejected;
    ack3.sequence = b3.sequence;
    ack3.batch_hash = hash3;

    const auto res3 = outbox.acknowledge(ack3);
    assert(res3 == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    assert(outbox.size() == 1);
    assert(outbox.aggregate_loss().rejected_batches == 1);

    // 4. Conflict ACK is terminal: deletes payload immediately, increments aggregate conflict
    nstu::exam::TelemetryAck ack4 = ack1;
    ack4.status = nstu::exam::TelemetryAckStatus::conflict;
    ack4.sequence = b4.sequence;
    ack4.batch_hash = hash4;

    const auto res4 = outbox.acknowledge(ack4);
    assert(res4 == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    assert(outbox.size() == 0);
    assert(outbox.aggregate_loss().conflict_batches == 1);

    // 5. Unavailable ACK retries with exponential backoff (2..60s)
    const auto b5 = make_batch(5);
    assert(outbox.enqueue(b5));
    const auto hash5 = *nstu::exam::compute_telemetry_batch_hash(b5);

    nstu::exam::TelemetryAck ack5 = ack1;
    ack5.status = nstu::exam::TelemetryAckStatus::unavailable;
    ack5.sequence = b5.sequence;
    ack5.batch_hash = hash5;

    const std::uint64_t now_ms = wall_clock_ms();
    const auto res5 = outbox.acknowledge(ack5, now_ms);
    assert(res5 == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(outbox.size() == 1); // payload retained!

    // Not eligible immediately (2000ms backoff)
    assert(!outbox.next_batch_to_send(now_ms + 1000).has_value());
    // Eligible after 2000ms
    const auto retry_batch = outbox.next_batch_to_send(now_ms + 2001);
    assert(retry_batch.has_value());
    assert(retry_batch->sequence == 5);
}

void test_outbox_restart_and_persistence() {
    const auto path = temp_test_path("restart_persistence");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        nstu::exam::TelemetryOutbox outbox;
        assert(outbox.open(path));
        assert(outbox.enqueue(make_batch(1)));
        assert(outbox.enqueue(make_batch(2)));

        // Increment conflict loss
        const auto b_temp = make_batch(99);
        assert(outbox.enqueue(b_temp));
        nstu::exam::TelemetryAck conflict_ack;
        conflict_ack.status = nstu::exam::TelemetryAckStatus::conflict;
        conflict_ack.session_id = b_temp.session_id;
        conflict_ack.producer_id = b_temp.producer_id;
        conflict_ack.sequence = b_temp.sequence;
        conflict_ack.batch_hash = *nstu::exam::compute_telemetry_batch_hash(b_temp);
        assert(outbox.acknowledge(conflict_ack) ==
               nstu::exam::TelemetryAckApplyResult::deleted_terminal);
        assert(outbox.aggregate_loss().conflict_batches == 1);
        assert(outbox.size() == 2);
        outbox.close();
    }

    // Reopen outbox from same path
    {
        nstu::exam::TelemetryOutbox outbox;
        assert(outbox.open(path));
        assert(outbox.size() == 2);
        assert(outbox.aggregate_loss().conflict_batches == 1);

        const auto batches = outbox.pending_batches();
        assert(batches.size() == 2);
        assert(batches[0].sequence == 1);
        assert(batches[1].sequence == 2);
    }
}

void test_outbox_30day_expiry() {
    const auto path = temp_test_path("expiry");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const std::uint64_t base_time = 1700000000000ULL;
    // Legacy v1 lacked an arrival field; its timestamp is the conservative expiry anchor.
    write_raw_outbox(path,
        {make_batch(1, 1, base_time), make_batch(2, 1, base_time + 10ULL * 86400ULL * 1000ULL)}, 2);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path, base_time + 15ULL * 86400ULL * 1000ULL, nullptr));
    assert(outbox.size() == 2);

    // Prune at base_time + 15 days -> neither expired
    assert(outbox.prune_expired(base_time + 15ULL * 86400ULL * 1000ULL) == 0);
    assert(outbox.size() == 2);

    // Prune at base_time + 30 days + 1 second -> Batch 1 expired
    const std::uint64_t expire_time = base_time + 30ULL * 86400ULL * 1000ULL + 1000ULL;
    assert(outbox.prune_expired(expire_time) == 1);
    assert(outbox.size() == 1);
    assert(outbox.aggregate_loss().expired_batches == 1);
    assert(outbox.pending_batches().front().sequence == 2);

    // Prune at base_time + 40 days + 2 seconds -> Batch 2 also expired
    const std::uint64_t expire_time2 = base_time + 40ULL * 86400ULL * 1000ULL + 2000ULL;
    assert(outbox.prune_expired(expire_time2) == 1);
    assert(outbox.empty());
    assert(outbox.aggregate_loss().expired_batches == 2);

    // Advance 30 days past when aggregate loss was first recorded -> metadata expires to 0
    const std::uint64_t agg_expire_time = expire_time + 30ULL * 86400ULL * 1000ULL + 1000ULL;
    assert(outbox.prune_expired(agg_expire_time) == 0);
    assert(outbox.aggregate_loss().expired_batches == 0);
}

void test_outbox_tamper_and_torn_tail() {
    const auto path = temp_test_path("tamper");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        nstu::exam::TelemetryOutbox outbox;
        assert(outbox.open(path));
        assert(outbox.enqueue(make_batch(1)));
        assert(outbox.enqueue(make_batch(2)));
        outbox.close();
    }

    // Corrupt the file tail (torn tail / truncated record)
    const auto file_size = std::filesystem::file_size(path);
    assert(file_size > 20);
    std::filesystem::resize_file(path, file_size - 10);

    // Opening detects corruption / torn tail and fails closed
    nstu::exam::TelemetryOutbox corrupted_outbox;
    std::string error;
    assert(!corrupted_outbox.open(path, &error));
    assert(!error.empty());
}

void test_outbox_lost_ack_and_inflight() {
    const auto path = temp_test_path("lost_ack");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1)));

    const std::uint64_t now_ms = wall_clock_ms() + 10000;
    // Next batch to send marks batch 1 in-flight
    auto b1 = outbox.next_batch_to_send(now_ms, 5000);
    assert(b1.has_value());
    assert(b1->sequence == 1);

    // Immediate second call returns nothing because batch 1 is in-flight
    assert(!outbox.next_batch_to_send(now_ms + 1000).has_value());

    // After in-flight timeout (5000ms), batch 1 is reclaimed and returned again
    auto b1_reclaimed = outbox.next_batch_to_send(now_ms + 5001);
    assert(b1_reclaimed.has_value());
    assert(b1_reclaimed->sequence == 1);

    // clear_inflight() immediately resets in-flight markers (e.g. on disconnect)
    outbox.clear_inflight();
    auto b1_after_clear = outbox.next_batch_to_send(now_ms + 100);
    assert(b1_after_clear.has_value());
    assert(b1_after_clear->sequence == 1);
}

void test_outbox_purge_and_clear() {
    const auto path = temp_test_path("purge_clear");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1)));
    assert(outbox.enqueue(make_batch(2)));

    // Add conflict loss
    auto b = make_batch(3);
    assert(outbox.enqueue(b));
    nstu::exam::TelemetryAck ack;
    ack.status = nstu::exam::TelemetryAckStatus::conflict;
    ack.session_id = b.session_id;
    ack.producer_id = b.producer_id;
    ack.sequence = b.sequence;
    ack.batch_hash = *nstu::exam::compute_telemetry_batch_hash(b);
    assert(outbox.acknowledge(ack) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    assert(outbox.aggregate_loss().conflict_batches == 1);
    assert(outbox.size() == 2);

    // purge() removes all pending payloads but preserves aggregate loss
    assert(outbox.purge());
    assert(outbox.empty());
    assert(outbox.aggregate_loss().conflict_batches == 1);

    // clear() removes all payloads and resets aggregate loss
    assert(outbox.enqueue(make_batch(4)));
    assert(outbox.clear());
    assert(outbox.empty());
    assert(outbox.aggregate_loss().conflict_batches == 0);
}

void test_outbox_storage_unavailable() {
    nstu::exam::TelemetryOutbox outbox;
    std::string error;
    // Empty path
    assert(!outbox.open("", &error));
    assert(!error.empty());
    assert(!outbox.is_open());

    const auto fail_path = temp_test_path("persist_fail");
    std::error_code ec;
    std::filesystem::remove(fail_path, ec);

    assert(outbox.open(fail_path, &error));
    assert(outbox.size() == 0);

#ifdef _WIN32
    // Make target file read-only so atomic rename fails
    SetFileAttributesW(fail_path.c_str(), FILE_ATTRIBUTE_READONLY);
    const auto b1 = make_batch(1);
    assert(!outbox.enqueue(b1, &error));
    // Rollback check: outbox size must remain 0 and bytes 0 on persist failure
    assert(outbox.size() == 0);
    assert(outbox.bytes() == 0);

    // After restoring write permission, retrying the same sequence must succeed
    SetFileAttributesW(fail_path.c_str(), FILE_ATTRIBUTE_NORMAL);
    assert(outbox.enqueue(b1, &error));
    assert(outbox.size() == 1);
#endif
}

void test_outbox_quota_limits() {
    const auto path = temp_test_path("quota_limits_4096");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // Synthesize an outbox with 4096 batches
    std::vector<nstu::exam::TelemetryBatch> batches_4096;
    batches_4096.reserve(4096);
    for (std::uint64_t i = 1; i <= 4096; ++i) {
        batches_4096.push_back(make_batch(i));
    }
    write_raw_outbox(path, batches_4096, 4096);

    nstu::exam::TelemetryOutbox outbox;
    std::string error;
    assert(outbox.open(path, &error));
    assert(outbox.size() == 4096);

    // 4097th batch must be rejected due to 4096 quota
    const auto b_extra = make_batch(4097);
    assert(!outbox.enqueue(b_extra, &error));
    assert(error.find("batch quota exceeded") != std::string::npos);
    assert(outbox.size() == 4096);
    outbox.close();

    // Synthesize an outbox file with 4097 batches -> open fails closed
    const auto path_over = temp_test_path("quota_limits_over");
    std::filesystem::remove(path_over, ec);
    std::vector<nstu::exam::TelemetryBatch> batches_4097 = batches_4096;
    batches_4097.push_back(make_batch(4097));
    write_raw_outbox(path_over, batches_4097, 4097);

    nstu::exam::TelemetryOutbox over_outbox;
    error.clear();
    assert(!over_outbox.open(path_over, &error));
    assert(error.find("batch count exceeds maximum quota") != std::string::npos);

    // 8 MiB quota test
    const auto path_byte_quota = temp_test_path("quota_byte_8mb");
    std::filesystem::remove(path_byte_quota, ec);

    nstu::exam::TelemetrySample max_sample = make_sample(1);
    max_sample.question_id = std::string(128, 'x');
    max_sample.question_revision = 1;
    nstu::exam::TelemetrySample quota_sample = max_sample;
    quota_sample.kind = nstu::exam::TelemetrySampleKind::clock_discontinuity;
    quota_sample.reason = nstu::exam::TelemetryReason::discontinuity_wall_clock;
    quota_sample.count = 1;

    std::vector<nstu::exam::TelemetryBatch> large_batches;
    // 1400 batches with 32 samples (~6 KiB each) = ~8.4 MiB
    large_batches.reserve(1400);
    for (std::uint64_t i = 1; i <= 1400; ++i) {
        auto b = make_batch(i);
        b.samples.assign(32, max_sample);
        large_batches.push_back(std::move(b));
    }
    write_raw_outbox(path_byte_quota, large_batches, 1400);

    nstu::exam::TelemetryOutbox byte_quota_outbox;
    error.clear();
    assert(!byte_quota_outbox.open(path_byte_quota, &error));
    assert(error.find("byte quota exceeded on open") != std::string::npos);
    assert(std::filesystem::exists(path_byte_quota));

    // Start below the byte limit; an enqueue succeeds, the next crosses 8 MiB.
    const auto batch_size = nstu::exam::encode_telemetry_batch(large_batches.front()).size();
    const auto initial_count = nstu::exam::kMaximumTelemetryOutboxBytes / batch_size - 1;
    assert(initial_count < large_batches.size());
    const auto path_near = temp_test_path("quota_near_8mb");
    std::filesystem::remove(path_near, ec);
    large_batches.resize(initial_count);
    write_raw_outbox(path_near, large_batches, static_cast<std::uint32_t>(initial_count));
    nstu::exam::TelemetryOutbox near_outbox;
    assert(near_outbox.open(path_near));
    auto near_batch = make_batch(initial_count + 1);
    near_batch.samples.assign(32, max_sample);
    assert(near_outbox.enqueue(near_batch, &error));
    assert(near_outbox.bytes() <= nstu::exam::kMaximumTelemetryOutboxBytes);
    auto crossing = make_batch(initial_count + 2);
    crossing.samples.assign(32, max_sample);
    error.clear();
    assert(!near_outbox.enqueue(crossing, &error));
    assert(error.find("byte quota exceeded") != std::string::npos);
    assert(near_outbox.bytes() <= nstu::exam::kMaximumTelemetryOutboxBytes);

    // Seed a valid near-limit file; exercise successful enqueue from below and crossing refusal.
    const auto path_from_below = temp_test_path("quota_enqueue_to_limit");
    std::filesystem::remove(path_from_below, ec);
    auto quota_batch = make_batch(1);
    quota_batch.samples.assign(32, quota_sample);
    const auto quota_batch_size = nstu::exam::encode_telemetry_batch(quota_batch).size();
    const auto count_below = nstu::exam::kMaximumTelemetryOutboxBytes / quota_batch_size - 1;
    assert(count_below < 4096);
    std::vector<nstu::exam::TelemetryBatch> below;
    below.reserve(count_below);
    for (std::size_t i = 1; i <= count_below; ++i) {
        auto b = quota_batch;
        b.sequence = i;
        below.push_back(std::move(b));
    }
    write_raw_outbox(path_from_below, below, static_cast<std::uint32_t>(count_below));
    nstu::exam::TelemetryOutbox filled;
    assert(filled.open(path_from_below));
    quota_batch.sequence = count_below + 1;
    assert(filled.enqueue(quota_batch, &error));
    quota_batch.sequence = count_below + 2;
    error.clear();
    assert(!filled.enqueue(quota_batch, &error));
    assert(error.find("byte quota exceeded") != std::string::npos);
    assert(filled.bytes() <= nstu::exam::kMaximumTelemetryOutboxBytes);
}

void test_outbox_byte_flip_tamper() {
    const auto path = temp_test_path("byte_flip_tamper");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        nstu::exam::TelemetryOutbox outbox;
        assert(outbox.open(path));
        assert(outbox.enqueue(make_batch(1)));
        assert(outbox.enqueue(make_batch(2)));
        outbox.close();
    }

    // Read file bytes and flip a byte
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    in.close();
    assert(bytes.size() > 50);

    // Flip byte in ciphertext
    bytes[bytes.size() / 2] ^= static_cast<char>(0x55);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();

    // Ciphertext corruption does not prove tampering: DPAPI failures can be transient.
    nstu::exam::TelemetryOutbox corrupted_outbox;
    std::string error;
    assert(!corrupted_outbox.open(path, &error));
    assert(!error.empty());
    assert(!corrupted_outbox.is_open());
    assert(std::filesystem::exists(path));
    assert(!corrupted_outbox.open(path));
}

void test_outbox_ack_extended() {
    const auto path = temp_test_path("ack_extended");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));

    const auto b1 = make_batch(1);
    assert(outbox.enqueue(b1));
    const auto hash1 = *nstu::exam::compute_telemetry_batch_hash(b1);

    // 1. Unknown sequence ACK -> ignored_mismatch
    nstu::exam::TelemetryAck unknown_seq_ack;
    unknown_seq_ack.status = nstu::exam::TelemetryAckStatus::accepted;
    unknown_seq_ack.session_id = b1.session_id;
    unknown_seq_ack.producer_id = b1.producer_id;
    unknown_seq_ack.sequence = 999;
    unknown_seq_ack.batch_hash = hash1;
    assert(outbox.acknowledge(unknown_seq_ack) ==
           nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(outbox.size() == 1);

    // 2. Conflicting hash ACK -> ignored_mismatch
    nstu::exam::TelemetryAck bad_hash_ack = unknown_seq_ack;
    bad_hash_ack.sequence = 1;
    bad_hash_ack.batch_hash = nstu::security::Sha256Digest{std::byte{0xfe}};
    assert(outbox.acknowledge(bad_hash_ack) ==
           nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(outbox.size() == 1);

    // 3. Wrong session_id -> ignored_mismatch
    nstu::exam::TelemetryAck wrong_session_ack = unknown_seq_ack;
    wrong_session_ack.sequence = 1;
    wrong_session_ack.batch_hash = hash1;
    wrong_session_ack.session_id = nstu::exam::SessionId{std::byte{0x99}};
    assert(outbox.acknowledge(wrong_session_ack) ==
           nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(outbox.size() == 1);

    // 4. Wrong producer_id -> ignored_mismatch
    nstu::exam::TelemetryAck wrong_prod_ack = unknown_seq_ack;
    wrong_prod_ack.sequence = 1;
    wrong_prod_ack.batch_hash = hash1;
    wrong_prod_ack.producer_id = nstu::exam::ProducerId{std::byte{0x99}};
    assert(outbox.acknowledge(wrong_prod_ack) ==
           nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(outbox.size() == 1);
}

void test_outbox_retry_backoff_cap_and_clock_jump() {
    const auto path = temp_test_path("retry_backoff");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));

    const auto b1 = make_batch(1);
    assert(outbox.enqueue(b1));
    const auto hash1 = *nstu::exam::compute_telemetry_batch_hash(b1);

    nstu::exam::TelemetryAck ack;
    ack.status = nstu::exam::TelemetryAckStatus::unavailable;
    ack.session_id = b1.session_id;
    ack.producer_id = b1.producer_id;
    ack.sequence = b1.sequence;
    ack.batch_hash = hash1;

    std::uint64_t now_ms = wall_clock_ms() + 100000;
    // Retry 1: 2000 ms
    assert(outbox.acknowledge(ack, now_ms) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!outbox.next_batch_to_send(now_ms + 1999).has_value());
    assert(outbox.next_batch_to_send(now_ms + 2000).has_value());
    outbox.clear_inflight();

    // Retry 2: 4000 ms
    assert(outbox.acknowledge(ack, now_ms) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!outbox.next_batch_to_send(now_ms + 3999).has_value());
    assert(outbox.next_batch_to_send(now_ms + 4000).has_value());
    outbox.clear_inflight();

    // Retry 3: 8000 ms
    assert(outbox.acknowledge(ack, now_ms) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!outbox.next_batch_to_send(now_ms + 7999).has_value());
    assert(outbox.next_batch_to_send(now_ms + 8000).has_value());
    outbox.clear_inflight();

    // Retry 4: 16000 ms
    assert(outbox.acknowledge(ack, now_ms) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!outbox.next_batch_to_send(now_ms + 15999).has_value());
    assert(outbox.next_batch_to_send(now_ms + 16000).has_value());
    outbox.clear_inflight();

    // Retry 5: 32000 ms
    assert(outbox.acknowledge(ack, now_ms) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!outbox.next_batch_to_send(now_ms + 31999).has_value());
    assert(outbox.next_batch_to_send(now_ms + 32000).has_value());
    outbox.clear_inflight();

    // Retry 6: capped at 60000 ms
    assert(outbox.acknowledge(ack, now_ms) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!outbox.next_batch_to_send(now_ms + 59999).has_value());
    assert(outbox.next_batch_to_send(now_ms + 60000).has_value());
    outbox.clear_inflight();

    // Retry 7: still capped at 60000 ms
    assert(outbox.acknowledge(ack, now_ms) == nstu::exam::TelemetryAckApplyResult::retry_scheduled);
    assert(!outbox.next_batch_to_send(now_ms + 59999).has_value());
    assert(outbox.next_batch_to_send(now_ms + 60000).has_value());
    outbox.clear_inflight();

    // Backward clock correction: system clock jumps backward to 1000
    const std::uint64_t backward_clock_ms = now_ms - 99000;
    assert(!outbox.next_batch_to_send(backward_clock_ms).has_value());
    assert(outbox.next_batch_to_send(backward_clock_ms + 60000).has_value());
    outbox.clear_inflight();

    // In-flight timing with backward clock correction
    auto sent = outbox.next_batch_to_send(now_ms, 5000);
    assert(sent.has_value());
    // System clock jumps backward by 10 seconds (before sent was called)
    auto reclaimed = outbox.next_batch_to_send(now_ms - 10000, 5000);
    assert(reclaimed.has_value());
    assert(reclaimed->sequence == 1);
}

void test_outbox_context_separation() {
    const auto path = temp_test_path("ctx_sep_single_file");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));

    auto b1 = make_batch(1);
    b1.session_id = nstu::exam::SessionId{std::byte{0x11}};

    auto b2 = make_batch(2);
    b2.session_id = nstu::exam::SessionId{std::byte{0x22}};

    assert(outbox.enqueue(b1));

    // Reject batch with different session_id while b1 is active
    std::string err;
    assert(!outbox.enqueue(b2, &err));
    assert(err.find("conflicting exam context") != std::string::npos);
    assert(outbox.size() == 1);

    // Terminal ACK on b1 deletes it and records loss for context 0x11
    nstu::exam::TelemetryAck ack1;
    ack1.status = nstu::exam::TelemetryAckStatus::rejected;
    ack1.session_id = b1.session_id;
    ack1.producer_id = b1.producer_id;
    ack1.sequence = b1.sequence;
    ack1.batch_hash = *nstu::exam::compute_telemetry_batch_hash(b1);
    assert(outbox.acknowledge(ack1) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    assert(outbox.empty());

    // Context 0x11 has 1 rejected batch
    assert(outbox.aggregate_loss().rejected_batches == 1);

    // Now outbox is empty, enqueue b2 for session 0x22 into the SAME outbox
    assert(outbox.enqueue(b2));
    assert(outbox.size() == 1);

    // Context 0x22 must NOT inherit old counters from context 0x11
    assert(outbox.aggregate_loss().rejected_batches == 0);
    assert(outbox.aggregate_loss(b1.session_id).rejected_batches == 1);
    outbox.close();
    assert(outbox.open(path));
    assert(outbox.aggregate_loss().rejected_batches == 0);
    assert(outbox.aggregate_loss(b1.session_id).rejected_batches == 1);
}

void test_outbox_completed_contexts_release_empty_loss_slots() {
    const auto path = temp_test_path("empty_loss_slots");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    for (std::uint64_t i = 1; i <= 65; ++i) {
        auto batch = make_batch(i);
        batch.session_id = nstu::exam::SessionId{static_cast<std::byte>(i)};
        assert(outbox.enqueue(batch));
        nstu::exam::TelemetryAck accepted;
        accepted.status = nstu::exam::TelemetryAckStatus::accepted;
        accepted.session_id = batch.session_id;
        accepted.producer_id = batch.producer_id;
        accepted.sequence = batch.sequence;
        accepted.batch_hash = *nstu::exam::compute_telemetry_batch_hash(batch);
        assert(outbox.acknowledge(accepted) == nstu::exam::TelemetryAckApplyResult::retired);
        assert(outbox.empty());
    }
    outbox.close();
    assert(outbox.open(path));
    assert(outbox.empty());
}

void test_outbox_future_stamp_expiry() {
    const auto path = temp_test_path("future_expiry");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto arrival = wall_clock_ms();
    const auto future = arrival + 90ULL * 86400ULL * 1000ULL;
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    auto b = make_batch(1, 1, future);
    assert(outbox.enqueue(b));
    outbox.close();
    assert(outbox.open(path));
    assert(outbox.prune_expired(arrival + nstu::exam::kTelemetryRetentionMilliseconds - 1000) == 0);
    assert(outbox.prune_expired(arrival + nstu::exam::kTelemetryRetentionMilliseconds + 10000) == 1);
    assert(outbox.empty());

    auto terminal = make_batch(2, 1, future);
    assert(outbox.enqueue(terminal));
    nstu::exam::TelemetryAck ack;
    ack.status = nstu::exam::TelemetryAckStatus::rejected;
    ack.session_id = terminal.session_id;
    ack.producer_id = terminal.producer_id;
    ack.sequence = terminal.sequence;
    ack.batch_hash = *nstu::exam::compute_telemetry_batch_hash(terminal);
    assert(outbox.acknowledge(ack, future) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    outbox.close();
    assert(outbox.open(path));
    assert(outbox.aggregate_loss().rejected_batches == 1);
    assert(outbox.prune_expired(arrival + nstu::exam::kTelemetryRetentionMilliseconds + 10000) == 0);
    assert(outbox.aggregate_loss().rejected_batches == 0);
}

void test_outbox_future_loaded_stamp_clamps_once() {
    const auto path = temp_test_path("future_loaded_stamp");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto arrival = wall_clock_ms();
    const auto future = arrival + 90ULL * 86400ULL * 1000ULL;
    write_raw_outbox(path, {make_batch(1, 1, future)}, 1);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    outbox.close();
    assert(outbox.open(path));
    assert(outbox.prune_expired(arrival + nstu::exam::kTelemetryRetentionMilliseconds + 10000) == 1);
}

void test_outbox_quarantine_retention() {
    const auto path = temp_test_path("quarantine_retention");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (const auto& old : quarantine_files(path)) std::filesystem::remove(old, ec);
    const auto prefix = path.wstring() + L".corrupt-";
    for (int i = 0; i < 3; ++i) {
        const auto old_file = std::filesystem::path(prefix + std::to_wstring(i));
        std::ofstream out(old_file);
        out << "test";
    }
    const auto old_path = std::filesystem::path(prefix + L"0");
    std::filesystem::last_write_time(old_path, std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24 * 31));
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    const auto files = quarantine_files(path);
    assert(files.size() == 1);
    assert(files.front() != old_path);
    std::filesystem::last_write_time(files.front(), std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24 * 31));
    assert(outbox.open(path));
    assert(quarantine_files(path).empty());

    // A decrypted, authenticated-format failure is proven corruption.
    outbox.close();
    write_raw_outbox(path, {make_batch(1)}, 1, true);
    assert(!outbox.open(path));
    assert(!std::filesystem::exists(path));
    assert(quarantine_files(path).size() == 1);
}

#ifdef _WIN32
void test_outbox_transient_read_failure() {
    const auto path = temp_test_path("read_locked");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1)));
    outbox.close();
    const auto prior = quarantine_files(path).size();
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);
    std::string error;
    assert(!outbox.open(path, &error));
    assert(!outbox.is_open());
    assert(std::filesystem::exists(path));
    assert(quarantine_files(path).size() == prior);
    CloseHandle(lock);
    assert(outbox.open(path));
    assert(outbox.size() == 1);
}
#endif

void test_outbox_move_safety() {
    const auto path = temp_test_path("move_safety");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1)));

    nstu::exam::TelemetryOutbox moved = std::move(outbox);
    assert(moved.is_open());
    assert(moved.size() == 1);

    // Operations on moved-from object must not crash or dereference null
    assert(!outbox.is_open());
    assert(outbox.size() == 0);
    assert(outbox.bytes() == 0);
    assert(outbox.empty());
    assert(outbox.pending_batches().empty());
    assert(!outbox.next_batch_to_send().has_value());
    assert(outbox.aggregate_loss().rejected_batches == 0);
    outbox.clear_inflight();
    outbox.close();

    std::string err;
    assert(!outbox.enqueue(make_batch(2), &err));
    assert(!outbox.purge(&err));
    assert(!outbox.clear(&err));

    nstu::exam::TelemetryAck ack;
    assert(outbox.acknowledge(ack) == nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(outbox.prune_expired(1000) == 0);
}

void test_outbox_persist_failure_rollback() {
    const auto path = temp_test_path("persist_rollback_full");
    std::error_code ec;
#ifdef _WIN32
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path.wstring() + L".revoked"), ec);

    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));

    auto b1 = make_batch(1);
    assert(outbox.enqueue(b1));
    const auto hash1 = *nstu::exam::compute_telemetry_batch_hash(b1);

#ifdef _WIN32
    // 1. Storage persist failure on ACK -> memory rolled back
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);

    nstu::exam::TelemetryAck ack;
    ack.status = nstu::exam::TelemetryAckStatus::rejected;
    ack.session_id = b1.session_id;
    ack.producer_id = b1.producer_id;
    ack.sequence = b1.sequence;
    ack.batch_hash = hash1;

    std::string err;
    assert(outbox.acknowledge(ack, 1000, &err) ==
           nstu::exam::TelemetryAckApplyResult::storage_failed);
    // Rolled back: batch 1 is still in outbox, loss was not incremented
    assert(outbox.size() == 1);
    assert(outbox.aggregate_loss().rejected_batches == 0);

    // 2. Storage persist failure on prune -> memory rolled back
    const std::uint64_t expire_time = wall_clock_ms() +
                                      nstu::exam::kTelemetryRetentionMilliseconds + 1000ULL;
    assert(outbox.prune_expired(expire_time, &err) == 0);
    // Rolled back: batch 1 still present
    assert(outbox.size() == 1);
    assert(outbox.aggregate_loss().expired_batches == 0);

    // 3. Storage persist failure on purge -> fails closed
    assert(!outbox.purge(&err));
    assert(!outbox.is_open());
    assert(!outbox.next_batch_to_send().has_value());
    const bool removed = !std::filesystem::exists(path);
    const bool marked = std::filesystem::exists(std::filesystem::path(path.wstring() + L".revoked"));
    assert(removed || marked);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    assert(!outbox.open(path)); // Same instance remains closed after a failed revocation.
    nstu::exam::TelemetryOutbox restarted;
    if (marked) {
        assert(restarted.open(path)); // Marker deletes any remaining old payload first.
        assert(!std::filesystem::exists(std::filesystem::path(path.wstring() + L".revoked")));
    } else {
        assert(restarted.open(path));
    }
    assert(restarted.empty());
#endif
}

#ifdef _WIN32
void test_outbox_purge_delete_failure() {
    const auto path = temp_test_path("purge_delete_fail");
    std::error_code ec;
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path.wstring() + L".revoked"), ec);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1)));
    outbox.close();
    assert(outbox.open(path));
    // A locked, read-only file prevents replacement and deletion.
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);
    std::string error;
    assert(!outbox.purge(&error));
    assert(!outbox.is_open());
    assert(std::filesystem::exists(path));
    assert(std::filesystem::exists(std::filesystem::path(path.wstring() + L".revoked")));
    nstu::exam::TelemetryOutbox reopened;
    error.clear();
    assert(!reopened.open(path, &error));
    assert(std::filesystem::exists(path));
    assert(!reopened.next_batch_to_send().has_value());
    CloseHandle(lock);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    error.clear();
    assert(reopened.open(path, &error));
    assert(reopened.empty());
    assert(!std::filesystem::exists(std::filesystem::path(path.wstring() + L".revoked")));
}
#endif

void test_outbox_poll_automatically_expires_records() {
    const auto path = temp_test_path("auto_poll");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1, 1, now - nstu::exam::kTelemetryRetentionMilliseconds)));
    assert(!outbox.next_batch_to_send(now));
    assert(outbox.empty());
    assert(outbox.aggregate_loss().expired_batches == 1);
    outbox.close();
    assert(outbox.open(path));
    assert(outbox.empty());
    assert(outbox.aggregate_loss().expired_batches == 1);
}

void test_outbox_open_automatically_expires_individual_records() {
    const auto path = temp_test_path("auto_open");
    const auto now = wall_clock_ms();
    write_raw_outbox(path, {make_batch(1, 1, now - nstu::exam::kTelemetryRetentionMilliseconds),
                            make_batch(2, 1, now)}, 2);
    // The file was just written: expiry must use individual records, not mtime.
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.size() == 1);
    assert(outbox.pending_batches().front().sequence == 2);
    assert(outbox.aggregate_loss().expired_batches == 1);
    outbox.close();
    assert(outbox.open(path));
    assert(outbox.size() == 1);
    assert(outbox.aggregate_loss().expired_batches == 1);
}

void test_outbox_poll_expires_loss_without_records() {
    const auto path = temp_test_path("auto_loss");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    const auto batch = make_batch(1, 1, now);
    assert(outbox.enqueue(batch));
    nstu::exam::TelemetryAck ack{batch.session_id, batch.producer_id, batch.sequence,
        *nstu::exam::compute_telemetry_batch_hash(batch), now, nstu::exam::TelemetryAckStatus::rejected};
    assert(outbox.acknowledge(ack, now) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    assert(!outbox.next_batch_to_send(now + nstu::exam::kTelemetryRetentionMilliseconds));
    assert(outbox.aggregate_loss().rejected_batches == 0);
}

void test_automatic_expiry_exact_boundary_uses_controlled_clock() {
    const auto path = temp_test_path("auto_boundary");
    const auto stamp = wall_clock_ms() - 10ULL * 86400ULL * 1000ULL;
    write_raw_outbox(path, {make_batch(1, 1, stamp)}, 1);
    nstu::exam::TelemetryOutbox outbox;
    const auto boundary = stamp + nstu::exam::kTelemetryRetentionMilliseconds;
    assert(outbox.open(path, boundary - 1, nullptr));
    assert(outbox.next_batch_to_send(boundary - 1));
    outbox.clear_inflight();
    assert(!outbox.next_batch_to_send(boundary));
    assert(outbox.empty());
    assert(outbox.aggregate_loss().expired_batches == 1);
}

void test_nonzero_loss_without_first_stamp_is_refused() {
    const auto path = temp_test_path("zero_loss_stamp");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    const auto batch = make_batch(1, 1, now);
    assert(outbox.enqueue(batch));
    nstu::exam::TelemetryAck ack{batch.session_id, batch.producer_id, batch.sequence,
        *nstu::exam::compute_telemetry_batch_hash(batch), now, nstu::exam::TelemetryAckStatus::rejected};
    assert(outbox.acknowledge(ack, now) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    outbox.close();
    constexpr std::string_view entropy_text = "NSTU-Exam-Telemetry-Outbox-v1";
    const std::span<const std::byte> entropy{reinterpret_cast<const std::byte*>(entropy_text.data()), entropy_text.size()};
    auto payload = nstu::security::load_machine_secret(path.wstring(), entropy);
    assert(payload.size() >= 76 + 32);
    payload.resize(payload.size() - 32);
    // v2 header + active session/count + loss session/three counters.
    std::fill_n(payload.begin() + 68, 8, std::byte{0});
    const auto digest = nstu::security::sha256(payload);
    assert(digest);
    payload.insert(payload.end(), digest->begin(), digest->end());
    assert(nstu::security::save_machine_secret(path.wstring(), payload, entropy));
    assert(!outbox.open(path));
    assert(!outbox.is_open());
    assert(!outbox.next_batch_to_send(now));
}

void test_expired_enqueue_cannot_race_a_send_offer() {
    const auto path = temp_test_path("expiry_enqueue_race");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    std::latch ready(1);
    std::atomic_bool enqueued = false;
    std::thread producer([&] {
        ready.wait();
        enqueued = outbox.enqueue(make_batch(1, 1, now - nstu::exam::kTelemetryRetentionMilliseconds));
    });
    ready.count_down();
    const auto offered = outbox.next_batch_to_send(now);
    producer.join();
    assert(enqueued);
    assert(!offered);
    assert(!outbox.next_batch_to_send(now));
    assert(outbox.empty());
    assert(outbox.aggregate_loss().expired_batches == 1);
}

#ifdef _WIN32
std::vector<std::byte> read_protected_fixture(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    assert(stream.is_open());
    const auto size = stream.tellg();
    assert(size > 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(stream.good());
    return bytes;
}

void assert_invalid_protected_expiry_barrier(
    const std::string& suffix, const std::vector<std::byte>& plaintext, bool tamper = false) {
    constexpr std::string_view entropy_text = "NSTU-Exam-Telemetry-Outbox-v1";
    const std::span<const std::byte> entropy{
        reinterpret_cast<const std::byte*>(entropy_text.data()), entropy_text.size()};
    std::string error;
    auto protected_barrier = nstu::security::protect_machine_secret(plaintext, entropy, &error);
    assert(!protected_barrier.empty());
    assert(error.empty());
    assert(nstu::security::unprotect_machine_secret(protected_barrier, entropy, &error) == plaintext);
    assert(error.empty());
    if (tamper) {
        assert(plaintext.size() == 8);
        protected_barrier.back() ^= std::byte{1};
        assert(nstu::security::unprotect_machine_secret(protected_barrier, entropy, &error).empty());
        assert(!error.empty());
    }

    const auto path = temp_test_path("protected_barrier_" + suffix);
    const auto barrier = std::filesystem::path(path.wstring() + L".expiry");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    assert(!ec);
    std::filesystem::remove(barrier, ec);
    assert(!ec);
    assert(quarantine_files(path).empty());
    const auto batch = make_batch(1, 1, wall_clock_ms() - 60000ULL);
    const auto encoded_batch = nstu::exam::encode_telemetry_batch(batch);
    const auto batch_hash = nstu::exam::compute_telemetry_batch_hash(batch);
    assert(!encoded_batch.empty() && batch_hash);
    nstu::exam::TelemetryOutbox seed;
    assert(seed.open(path));
    assert(seed.enqueue(batch));
    // Normalize the empty loss slot before freezing the valid durable fixture.
    assert(seed.open(path));
    const auto now = wall_clock_ms();
    const auto eligible = seed.next_batch_to_send(now);
    assert(eligible && nstu::exam::encode_telemetry_batch(*eligible) == encoded_batch);
    seed.close();
    const auto payload_bytes = read_protected_fixture(path);
    const auto payload_write_time = std::filesystem::last_write_time(path);
    assert(payload_write_time > std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24 * nstu::exam::kTelemetryRetentionDays));
    {
        std::ofstream stream(barrier, std::ios::binary | std::ios::trunc);
        assert(stream.is_open());
        stream.write(reinterpret_cast<const char*>(protected_barrier.data()),
                     static_cast<std::streamsize>(protected_barrier.size()));
        stream.flush();
        assert(stream.good());
    }
    assert(read_protected_fixture(barrier) == protected_barrier);
    const auto barrier_write_time = std::filesystem::last_write_time(barrier);

    nstu::exam::TelemetryOutbox recovered;
    for (int attempt = 0; attempt < 2; ++attempt) {
        error.clear();
        assert(!recovered.open(path, now, &error));
        assert(!error.empty());
        assert(!recovered.is_open());
        assert(recovered.pending_batches().empty());
        assert(recovered.empty() && recovered.bytes() == 0);
        assert(!recovered.next_batch_to_send(now));
        assert(read_protected_fixture(path) == payload_bytes);
        assert(std::filesystem::last_write_time(path) == payload_write_time);
        assert(read_protected_fixture(barrier) == protected_barrier);
        assert(std::filesystem::last_write_time(barrier) == barrier_write_time);
        assert(quarantine_files(path).empty());
    }

    assert(std::filesystem::remove(barrier));
    error.clear();
    assert(recovered.open(path, now, &error));
    assert(error.empty() && recovered.is_open());
    const auto pending = recovered.pending_batches();
    assert(pending.size() == 1);
    assert(pending.front().session_id == batch.session_id);
    assert(pending.front().producer_id == batch.producer_id);
    assert(pending.front().sequence == batch.sequence);
    assert(pending.front().first_enqueued_at_unix_milliseconds == batch.first_enqueued_at_unix_milliseconds);
    assert(nstu::exam::compute_telemetry_batch_hash(pending.front()) == batch_hash);
    assert(nstu::exam::encode_telemetry_batch(pending.front()) == encoded_batch);
    assert(read_protected_fixture(path) == payload_bytes);
    assert(std::filesystem::last_write_time(path) == payload_write_time);
    assert(!std::filesystem::exists(barrier));
    assert(quarantine_files(path).empty());
    const auto offered = recovered.next_batch_to_send(now);
    assert(offered && nstu::exam::encode_telemetry_batch(*offered) == encoded_batch);
    // The earlier producer stamp must still expire the recovered batch on time.
    const auto boundary = batch.first_enqueued_at_unix_milliseconds + nstu::exam::kTelemetryRetentionMilliseconds;
    recovered.clear_inflight();
    assert(recovered.next_batch_to_send(boundary - 1));
    assert(!recovered.next_batch_to_send(boundary));
    assert(recovered.empty());
    assert(recovered.aggregate_loss(batch.session_id).expired_batches == 1);
    recovered.close();
    assert(!std::filesystem::exists(barrier));
    assert(quarantine_files(path).empty());
    assert(std::filesystem::remove(path));
    std::cout << "Protected expiry barrier " << suffix << ": round-trip, refusal, preservation, recovery and retention passed.\n";
}

void test_protected_seven_byte_expiry_barrier_is_refused() {
    assert_invalid_protected_expiry_barrier("seven_byte", std::vector<std::byte>(7, std::byte{1}));
}

void test_protected_nine_byte_expiry_barrier_is_refused() {
    assert_invalid_protected_expiry_barrier("nine_byte", std::vector<std::byte>(9, std::byte{1}));
}

void test_protected_zero_expiry_barrier_is_refused() {
    assert_invalid_protected_expiry_barrier("eight_byte_zero", std::vector<std::byte>(8, std::byte{0}));
}

void test_tampered_protected_expiry_barrier_is_refused() {
    const auto observed = wall_clock_ms();
    assert(observed != 0);
    std::vector<std::byte> plaintext;
    for (std::size_t i = 0; i < 8; ++i) {
        plaintext.push_back(static_cast<std::byte>((observed >> (8u * i)) & 0xffu));
    }
    assert_invalid_protected_expiry_barrier("tampered_eight_byte_nonzero", plaintext, true);
}

std::span<const std::byte> mixed_cleanup_entropy() {
    constexpr std::string_view text = "NSTU-Exam-Telemetry-Outbox-v1";
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::vector<std::byte> mixed_cleanup_time_bytes(std::uint64_t time) {
    std::vector<std::byte> bytes;
    for (std::size_t i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::byte>((time >> (8u * i)) & 0xffu));
    }
    return bytes;
}

struct MixedCleanupDiskState {
    nstu::exam::SessionId active_session{};
    std::vector<nstu::exam::TelemetryOutboxAggregateLoss> losses;
    std::vector<std::vector<std::byte>> batches;
    std::vector<std::uint64_t> arrivals;
};

MixedCleanupDiskState read_mixed_cleanup_disk(const std::filesystem::path& path) {
    std::string error;
    const auto plaintext = nstu::security::unprotect_machine_secret(
        read_protected_fixture(path), mixed_cleanup_entropy(), &error);
    assert(error.empty() && plaintext.size() > nstu::security::Sha256Digest{}.size());
    const std::span<const std::byte> payload{plaintext.data(), plaintext.size() - nstu::security::Sha256Digest{}.size()};
    const auto digest = nstu::security::sha256(payload);
    assert(digest);
    for (std::size_t i = 0; i < digest->size(); ++i) {
        assert((*digest)[i] == plaintext[payload.size() + i]);
    }
    std::size_t offset = 0;
    const auto number = [&](std::size_t width) {
        assert(offset + width <= payload.size());
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) {
            value |= std::to_integer<std::uint64_t>(payload[offset++]) << (8u * i);
        }
        return value;
    };
    const auto session = [&] {
        nstu::exam::SessionId id{};
        assert(offset + id.size() <= payload.size());
        for (auto& byte : id) byte = payload[offset++];
        return id;
    };
    assert(number(4) == 0x544c4d4f);
    assert(number(2) == 2);
    assert(number(2) == 0);
    MixedCleanupDiskState state;
    state.active_session = session();
    const auto loss_count = number(4);
    assert(loss_count <= 2);
    for (std::uint64_t i = 0; i < loss_count; ++i) {
        nstu::exam::TelemetryOutboxAggregateLoss loss;
        loss.session_id = session();
        loss.rejected_batches = number(8);
        loss.conflict_batches = number(8);
        loss.expired_batches = number(8);
        loss.first_recorded_unix_ms = number(8);
        loss.last_updated_unix_ms = number(8);
        state.losses.push_back(loss);
    }
    const auto batch_count = number(4);
    assert(batch_count <= 4);
    for (std::uint64_t i = 0; i < batch_count; ++i) {
        const auto length = static_cast<std::size_t>(number(4));
        assert(offset + length <= payload.size());
        const auto encoded = payload.subspan(offset, length);
        offset += length;
        const auto batch = nstu::exam::decode_telemetry_batch(encoded, &error);
        assert(batch && error.empty());
        state.batches.emplace_back(encoded.begin(), encoded.end());
        assert(nstu::exam::encode_telemetry_batch(*batch) == state.batches.back());
        assert(number(2) == 0); // no retry in this fixture
        assert(number(8) == 0); // immediately eligible
        state.arrivals.push_back(number(8));
        assert(state.arrivals.back() != 0);
    }
    assert(offset == payload.size());
    return state;
}

void assert_mixed_cleanup_loss_equal(const nstu::exam::TelemetryOutboxAggregateLoss& actual,
                                     const nstu::exam::TelemetryOutboxAggregateLoss& expected) {
    assert(actual.session_id == expected.session_id);
    assert(actual.rejected_batches == expected.rejected_batches);
    assert(actual.conflict_batches == expected.conflict_batches);
    assert(actual.expired_batches == expected.expired_batches);
    assert(actual.first_recorded_unix_ms == expected.first_recorded_unix_ms);
    assert(actual.last_updated_unix_ms == expected.last_updated_unix_ms);
}

struct MixedCleanupFixture {
    const std::uint64_t time = wall_clock_ms();
    const std::uint64_t boundary = time + 3600000ULL;
    const std::uint64_t stale_stamp = boundary - nstu::exam::kTelemetryRetentionMilliseconds;
    const nstu::exam::SessionId stale_session{std::byte{0x71}};
    const nstu::exam::SessionId current_session{std::byte{0xcc}};
    std::filesystem::path path, barrier, alternate_path, alternate_barrier, alternate_temp;
    nstu::exam::TelemetryOutbox outbox;
    std::vector<nstu::exam::TelemetryBatch> batches;
    std::vector<std::vector<std::byte>> encodings;
    std::vector<nstu::security::Sha256Digest> hashes;
    std::vector<std::byte> original_payload, alternate_payload, alternate_barrier_bytes, alternate_temp_bytes;
    std::filesystem::file_time_type original_write_time;
    MixedCleanupDiskState original_disk;
    nstu::exam::TelemetryBatch alternate_batch;
    std::size_t original_bytes = 0, retained_bytes = 0;

    explicit MixedCleanupFixture(const std::string& suffix) {
        path = temp_test_path("mixed_cleanup_" + suffix);
        barrier = std::filesystem::path(path.wstring() + L".expiry");
        alternate_path = temp_test_path("mixed_cleanup_" + suffix + "_alternate");
        alternate_barrier = std::filesystem::path(alternate_path.wstring() + L".expiry");
        alternate_temp = std::filesystem::path(alternate_path.wstring() + L".tmp-91001-1");
        std::error_code ec;
        for (const auto& owned : {path, barrier, alternate_path, alternate_barrier, alternate_temp}) {
            std::filesystem::remove(owned, ec);
            assert(!ec);
        }
        assert(quarantine_files(path).empty() && quarantine_files(alternate_path).empty());
        assert(outbox.open(path));
        const auto terminal = [&](nstu::exam::TelemetryBatch batch,
                                  nstu::exam::TelemetryAckStatus status, std::uint64_t stamp) {
            assert(outbox.enqueue(batch));
            const auto hash = nstu::exam::compute_telemetry_batch_hash(batch);
            assert(hash);
            const nstu::exam::TelemetryAck ack{batch.session_id, batch.producer_id, batch.sequence, *hash, stamp, status};
            assert(outbox.acknowledge(ack, stamp) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
        };
        auto stale = make_batch(90, 1, time);
        stale.session_id = stale_session;
        terminal(stale, nstu::exam::TelemetryAckStatus::rejected, stale_stamp);
        terminal(make_batch(91, 2, time), nstu::exam::TelemetryAckStatus::conflict, time);
        batches = {make_batch(101, 2, stale_stamp), make_batch(102, 2, time),
                   make_batch(103, 2, stale_stamp), make_batch(104, 2, time)};
        for (std::size_t i = 0; i < batches.size(); ++i) {
            assert(outbox.enqueue(batches[i]));
            encodings.push_back(nstu::exam::encode_telemetry_batch(batches[i]));
            const auto hash = nstu::exam::compute_telemetry_batch_hash(batches[i]);
            assert(!encodings.back().empty() && hash);
            hashes.push_back(*hash);
            original_bytes += encodings.back().size();
            if (i == 1 || i == 3) retained_bytes += encodings.back().size();
        }
        original_payload = read_protected_fixture(path);
        original_write_time = std::filesystem::last_write_time(path);
        assert(original_write_time > std::filesystem::file_time_type::clock::now() -
            std::chrono::hours(24 * nstu::exam::kTelemetryRetentionDays));
        original_disk = read_mixed_cleanup_disk(path);

        nstu::exam::TelemetryOutbox alternate;
        alternate_batch = make_batch(201, 3, time);
        alternate_batch.session_id[0] = std::byte{0xd4};
        assert(alternate.open(alternate_path));
        assert(alternate.enqueue(alternate_batch));
        assert(alternate.open(alternate_path)); // normalize its empty loss slot
        assert(alternate.next_batch_to_send(time));
        alternate.close();
        alternate_payload = read_protected_fixture(alternate_path);
        std::string error;
        assert(nstu::security::save_machine_secret(alternate_barrier.wstring(),
            mixed_cleanup_time_bytes(time), mixed_cleanup_entropy(), &error));
        assert(error.empty());
        alternate_barrier_bytes = read_protected_fixture(alternate_barrier);
        assert(nstu::security::unprotect_machine_secret(alternate_barrier_bytes,
            mixed_cleanup_entropy(), &error) == mixed_cleanup_time_bytes(time));
        assert(error.empty());
        {
            std::ofstream stream(alternate_temp, std::ios::binary);
            assert(stream.is_open());
            stream << "owned alternate numeric temporary";
            stream.flush();
            assert(stream.good());
        }
        alternate_temp_bytes = read_protected_fixture(alternate_temp);
    }

    void assert_alternate_untouched() const {
        assert(read_protected_fixture(alternate_path) == alternate_payload);
        assert(read_protected_fixture(alternate_barrier) == alternate_barrier_bytes);
        assert(read_protected_fixture(alternate_temp) == alternate_temp_bytes);
        assert(quarantine_files(alternate_path).empty());
    }

    void assert_state(const nstu::exam::TelemetryOutbox& loaded, bool cleaned, bool stale_observed) const {
        const auto pending = loaded.pending_batches();
        const auto disk = read_mixed_cleanup_disk(path);
        const std::vector<std::size_t> indices = cleaned ? std::vector<std::size_t>{1, 3} : std::vector<std::size_t>{0, 1, 2, 3};
        assert(pending.size() == indices.size() && disk.batches.size() == indices.size());
        assert(loaded.bytes() == (cleaned ? retained_bytes : original_bytes));
        assert(disk.active_session == current_session);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto index = indices[i];
            assert(nstu::exam::encode_telemetry_batch(pending[i]) == encodings[index]);
            assert(nstu::exam::compute_telemetry_batch_hash(pending[i]) == hashes[index]);
            assert(disk.batches[i] == encodings[index]);
            assert(disk.arrivals[i] == original_disk.arrivals[index]);
        }
        assert(disk.losses.size() == (cleaned ? 1u : 2u));
        for (const auto& loss : disk.losses) {
            if (loss.session_id == current_session) {
                assert(loss.rejected_batches == 0 && loss.conflict_batches == 1);
                assert(loss.expired_batches == (cleaned ? 2u : 0u));
                assert(loss.first_recorded_unix_ms == time);
                assert_mixed_cleanup_loss_equal(loaded.aggregate_loss(current_session), loss);
            } else {
                assert(!cleaned && loss.session_id == stale_session);
                assert(loss.rejected_batches == 1 && loss.conflict_batches == 0 && loss.expired_batches == 0);
                assert(loss.first_recorded_unix_ms == stale_stamp && loss.last_updated_unix_ms == stale_stamp);
            }
        }
        if (cleaned || stale_observed) {
            assert_mixed_cleanup_loss_equal(loaded.aggregate_loss(stale_session), {});
        } else {
            assert(loaded.aggregate_loss(stale_session).rejected_batches == 1);
        }
        assert(quarantine_files(path).empty());
    }

    void finish() {
        assert_alternate_untouched();
        std::string error;
        assert(outbox.open(alternate_path, boundary - 1, &error));
        assert(error.empty() && outbox.is_open());
        assert(!std::filesystem::exists(alternate_barrier) && !std::filesystem::exists(alternate_temp));
        assert(read_protected_fixture(alternate_path) == alternate_payload);
        const auto offered = outbox.next_batch_to_send(boundary - 1);
        assert(offered && nstu::exam::encode_telemetry_batch(*offered) == nstu::exam::encode_telemetry_batch(alternate_batch));
        assert(nstu::exam::compute_telemetry_batch_hash(*offered) == nstu::exam::compute_telemetry_batch_hash(alternate_batch));
        outbox.close();
        assert(std::filesystem::remove(path));
        assert(std::filesystem::remove(alternate_path));
        assert(!std::filesystem::exists(barrier));
        assert(quarantine_files(path).empty() && quarantine_files(alternate_path).empty());
    }
};

void run_mixed_expiry_cleanup_case(bool block_barrier_deletion) {
    const std::string suffix = block_barrier_deletion ? "barrier_deletion" : "payload_replacement";
    MixedCleanupFixture fixture(suffix);
    fixture.assert_state(fixture.outbox, false, false);
    HANDLE payload_lock = CreateFileW(fixture.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(payload_lock != INVALID_HANDLE_VALUE);
    std::string error;
    assert(fixture.outbox.prune_expired(fixture.boundary, &error) == 0);
    assert(error == "failed to atomically rename outbox file");
    assert(read_protected_fixture(fixture.path) == fixture.original_payload);
    assert(std::filesystem::last_write_time(fixture.path) == fixture.original_write_time);
    const auto barrier_bytes = read_protected_fixture(fixture.barrier);
    error.clear();
    assert(nstu::security::unprotect_machine_secret(barrier_bytes, mixed_cleanup_entropy(), &error) ==
        mixed_cleanup_time_bytes(fixture.boundary));
    assert(error.empty());
    fixture.assert_state(fixture.outbox, false, true);
    HANDLE barrier_lock = INVALID_HANDLE_VALUE;
    if (block_barrier_deletion) {
        barrier_lock = CreateFileW(fixture.barrier.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(barrier_lock != INVALID_HANDLE_VALUE);
        assert(CloseHandle(payload_lock));
        payload_lock = INVALID_HANDLE_VALUE;
        error.clear();
        // A zero public count with this error still leaves the cleaned store committed.
        assert(fixture.outbox.prune_expired(fixture.boundary - 1, &error) == 0);
        assert(error == "expiry barrier cleanup failed");
        assert(read_protected_fixture(fixture.path) != fixture.original_payload);
        fixture.assert_state(fixture.outbox, true, true);
    }
    const auto gated_payload = read_protected_fixture(fixture.path);
    const auto gated_write_time = std::filesystem::last_write_time(fixture.path);
    const auto gated_loss = fixture.outbox.aggregate_loss(fixture.current_session);
    assert(!fixture.outbox.next_batch_to_send(fixture.boundary - 1));
    error.clear();
    assert(!fixture.outbox.enqueue(make_batch(105, 2, fixture.time), &error));
    assert(error == (block_barrier_deletion ? "expiry barrier cleanup failed" : "failed to atomically rename outbox file"));
    error.clear();
    assert(!fixture.outbox.open(fixture.alternate_path, fixture.boundary - 1, &error));
    assert(error == "observed expiry cleanup pending; cannot change outbox path");
    assert(fixture.outbox.is_open());
    fixture.assert_state(fixture.outbox, block_barrier_deletion, true);
    assert_mixed_cleanup_loss_equal(fixture.outbox.aggregate_loss(fixture.current_session), gated_loss);
    assert(read_protected_fixture(fixture.path) == gated_payload);
    assert(std::filesystem::last_write_time(fixture.path) == gated_write_time);
    assert(read_protected_fixture(fixture.barrier) == barrier_bytes);
    fixture.assert_alternate_untouched();
    if (block_barrier_deletion) {
        nstu::exam::TelemetryOutbox closed_restart;
        error.clear();
        assert(!closed_restart.open(fixture.path, fixture.boundary - 1, &error));
        assert(error == "expiry barrier cleanup failed");
        assert(!closed_restart.is_open() && closed_restart.pending_batches().empty());
        assert(!closed_restart.next_batch_to_send(fixture.boundary - 1));
        assert(read_protected_fixture(fixture.path) == gated_payload);
        assert(read_protected_fixture(fixture.barrier) == barrier_bytes);
        assert(CloseHandle(barrier_lock));
    } else {
        assert(CloseHandle(payload_lock));
    }
    error.clear();
    assert(fixture.outbox.open(fixture.path, fixture.boundary - 1, &error));
    assert(error.empty());
    fixture.assert_state(fixture.outbox, true, true);
    assert(!std::filesystem::exists(fixture.barrier));
    const auto recovered_payload = read_protected_fixture(fixture.path);
    const auto recovered_loss = fixture.outbox.aggregate_loss(fixture.current_session);
    if (block_barrier_deletion) {
        assert(recovered_payload == gated_payload);
        assert_mixed_cleanup_loss_equal(recovered_loss, gated_loss);
    } else {
        assert(recovered_payload != fixture.original_payload);
    }
    const auto offered = fixture.outbox.next_batch_to_send(fixture.boundary - 1);
    assert(offered && nstu::exam::encode_telemetry_batch(*offered) == fixture.encodings[1]);
    fixture.outbox.close();
    nstu::exam::TelemetryOutbox restarted;
    assert(restarted.open(fixture.path, fixture.boundary - 1, nullptr));
    fixture.assert_state(restarted, true, true);
    assert(restarted.open(fixture.path, fixture.boundary - 2, nullptr));
    fixture.assert_state(restarted, true, true);
    assert_mixed_cleanup_loss_equal(restarted.aggregate_loss(fixture.current_session), recovered_loss);
    assert(read_protected_fixture(fixture.path) == recovered_payload);
    assert(!std::filesystem::exists(fixture.barrier));
    fixture.assert_alternate_untouched();
    restarted.close();
    fixture.finish();
    std::cout << "Mixed expiry cleanup " << suffix << ": locks, durable state, rollback and alternate path passed.\n";
}

void test_mixed_expiry_payload_replacement_failure_preserves_state_and_path() {
    run_mixed_expiry_cleanup_case(false);
}

void test_mixed_expiry_barrier_deletion_failure_preserves_committed_cleanup_and_path() {
    run_mixed_expiry_cleanup_case(true);
}

void assert_mixed_cleanup_r2_only(const MixedCleanupFixture& fixture,
                                const nstu::exam::TelemetryOutbox& loaded,
                                const nstu::exam::TelemetryOutboxAggregateLoss& expected_loss) {
    const auto pending = loaded.pending_batches();
    const auto disk = read_mixed_cleanup_disk(fixture.path);
    assert(loaded.size() == 1 && pending.size() == 1 && disk.batches.size() == 1);
    assert(loaded.bytes() == fixture.encodings[3].size());
    assert(nstu::exam::encode_telemetry_batch(pending[0]) == fixture.encodings[3]);
    assert(nstu::exam::compute_telemetry_batch_hash(pending[0]) == fixture.hashes[3]);
    assert(disk.batches[0] == fixture.encodings[3]);
    assert(disk.arrivals.size() == 1 && disk.arrivals[0] == fixture.original_disk.arrivals[3]);
    assert(disk.active_session == fixture.current_session);
    assert(disk.losses.size() == 1);
    assert_mixed_cleanup_loss_equal(disk.losses[0], expected_loss);
    assert_mixed_cleanup_loss_equal(loaded.aggregate_loss(fixture.current_session), expected_loss);
    assert_mixed_cleanup_loss_equal(loaded.aggregate_loss(fixture.stale_session), {});
    assert(quarantine_files(fixture.path).empty());
}

void run_mixed_expiry_ack_with_locked_barrier(nstu::exam::TelemetryAckStatus status) {
    const std::string suffix = status == nstu::exam::TelemetryAckStatus::accepted ? "accepted" : "duplicate";
    MixedCleanupFixture fixture("ack_" + suffix);
    fixture.assert_state(fixture.outbox, false, false);
    HANDLE payload_lock = CreateFileW(fixture.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(payload_lock != INVALID_HANDLE_VALUE);
    std::string error;
    assert(fixture.outbox.prune_expired(fixture.boundary, &error) == 0);
    assert(error == "failed to atomically rename outbox file");
    assert(read_protected_fixture(fixture.path) == fixture.original_payload);
    assert(std::filesystem::last_write_time(fixture.path) == fixture.original_write_time);
    const auto barrier_bytes = read_protected_fixture(fixture.barrier);
    error.clear();
    assert(nstu::security::unprotect_machine_secret(barrier_bytes, mixed_cleanup_entropy(), &error) ==
        mixed_cleanup_time_bytes(fixture.boundary));
    assert(error.empty());
    fixture.assert_state(fixture.outbox, false, true);
    HANDLE barrier_lock = CreateFileW(fixture.barrier.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(barrier_lock != INVALID_HANDLE_VALUE);
    assert(CloseHandle(payload_lock));
    error.clear();
    // Real cleanup publishes R1/R2 even though removing its existing barrier fails.
    assert(fixture.outbox.prune_expired(fixture.boundary - 1, &error) == 0);
    assert(error == "expiry barrier cleanup failed");
    fixture.assert_state(fixture.outbox, true, true);
    const auto cleaned_payload = read_protected_fixture(fixture.path);
    assert(cleaned_payload != fixture.original_payload);
    const auto cleaned_write_time = std::filesystem::last_write_time(fixture.path);
    const auto cleaned_loss = fixture.outbox.aggregate_loss(fixture.current_session);
    assert(cleaned_loss.conflict_batches == 1 && cleaned_loss.expired_batches == 2);
    assert(read_protected_fixture(fixture.barrier) == barrier_bytes);
    const auto ack_for = [&](std::size_t index) {
        const auto& batch = fixture.batches[index];
        return nstu::exam::TelemetryAck{batch.session_id, batch.producer_id, batch.sequence,
            fixture.hashes[index], fixture.boundary - 1, status};
    };
    auto wrong_hash = ack_for(1);
    wrong_hash.batch_hash[0] ^= std::byte{1};
    for (const auto& ack : {wrong_hash, ack_for(0), ack_for(2)}) {
        error.clear();
        assert(fixture.outbox.acknowledge(ack, fixture.boundary - 1, &error) ==
            nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
        assert(error.empty() && fixture.outbox.is_open());
        fixture.assert_state(fixture.outbox, true, true);
        assert_mixed_cleanup_loss_equal(fixture.outbox.aggregate_loss(fixture.current_session), cleaned_loss);
        assert(read_protected_fixture(fixture.path) == cleaned_payload);
        assert(std::filesystem::last_write_time(fixture.path) == cleaned_write_time);
        assert(read_protected_fixture(fixture.barrier) == barrier_bytes);
    }
    const auto r1_ack = ack_for(1);
    error.clear();
    assert(fixture.outbox.acknowledge(r1_ack, fixture.boundary - 1, &error) ==
        nstu::exam::TelemetryAckApplyResult::retired);
    assert(error.empty());
    assert_mixed_cleanup_r2_only(fixture, fixture.outbox, cleaned_loss);
    const auto retired_payload = read_protected_fixture(fixture.path);
    assert(retired_payload != cleaned_payload);
    const auto retired_write_time = std::filesystem::last_write_time(fixture.path);
    const auto assert_retired_unchanged = [&] {
        assert(fixture.outbox.is_open());
        assert_mixed_cleanup_r2_only(fixture, fixture.outbox, cleaned_loss);
        assert(read_protected_fixture(fixture.path) == retired_payload);
        assert(std::filesystem::last_write_time(fixture.path) == retired_write_time);
        assert(read_protected_fixture(fixture.barrier) == barrier_bytes);
        fixture.assert_alternate_untouched();
    };
    assert_retired_unchanged();
    error.clear();
    assert(fixture.outbox.acknowledge(r1_ack, fixture.boundary - 1, &error) ==
        nstu::exam::TelemetryAckApplyResult::ignored_mismatch);
    assert(error.empty());
    assert_retired_unchanged();
    assert(!fixture.outbox.next_batch_to_send(fixture.boundary - 1));
    assert_retired_unchanged();
    error.clear();
    assert(!fixture.outbox.enqueue(make_batch(105, 2, fixture.time), &error));
    assert(error == "expiry barrier cleanup failed");
    assert_retired_unchanged();
    error.clear();
    assert(!fixture.outbox.open(fixture.alternate_path, fixture.boundary - 1, &error));
    assert(error == "observed expiry cleanup pending; cannot change outbox path");
    assert_retired_unchanged();
    nstu::exam::TelemetryOutbox blocked_restart;
    error.clear();
    assert(!blocked_restart.open(fixture.path, fixture.boundary - 1, &error));
    assert(error == "expiry barrier cleanup failed");
    assert(!blocked_restart.is_open() && blocked_restart.pending_batches().empty());
    assert(blocked_restart.bytes() == 0 && !blocked_restart.next_batch_to_send(fixture.boundary - 1));
    assert_retired_unchanged();
    assert(CloseHandle(barrier_lock));
    error.clear();
    assert(fixture.outbox.open(fixture.path, fixture.boundary - 1, &error));
    assert(error.empty() && fixture.outbox.is_open());
    assert(!std::filesystem::exists(fixture.barrier));
    assert_mixed_cleanup_r2_only(fixture, fixture.outbox, cleaned_loss);
    assert(read_protected_fixture(fixture.path) == retired_payload);
    assert(std::filesystem::last_write_time(fixture.path) == retired_write_time);
    const auto offered = fixture.outbox.next_batch_to_send(fixture.boundary - 1);
    assert(offered && nstu::exam::encode_telemetry_batch(*offered) == fixture.encodings[3]);
    assert(nstu::exam::compute_telemetry_batch_hash(*offered) == fixture.hashes[3]);
    fixture.outbox.close();
    nstu::exam::TelemetryOutbox restarted;
    for (const auto reopen_time : {fixture.boundary - 1, fixture.boundary - 2, fixture.boundary - 2}) {
        error.clear();
        assert(restarted.open(fixture.path, reopen_time, &error));
        assert(error.empty() && restarted.is_open());
        assert_mixed_cleanup_r2_only(fixture, restarted, cleaned_loss);
        assert(read_protected_fixture(fixture.path) == retired_payload);
        assert(std::filesystem::last_write_time(fixture.path) == retired_write_time);
        assert(!std::filesystem::exists(fixture.barrier));
        fixture.assert_alternate_untouched();
        const auto replay = restarted.next_batch_to_send(reopen_time);
        assert(replay && nstu::exam::encode_telemetry_batch(*replay) == fixture.encodings[3]);
        assert(nstu::exam::compute_telemetry_batch_hash(*replay) == fixture.hashes[3]);
        assert_mixed_cleanup_r2_only(fixture, restarted, cleaned_loss);
    }
    restarted.close();
    fixture.finish();
    std::cout << "Mixed expiry ACK " << suffix
              << ": real locked barrier, exact retirement, refused gates and R2-only rollback recovery passed.\n";
}

void test_mixed_expiry_accepted_ack_retires_exact_batch_while_barrier_deletion_is_blocked() {
    run_mixed_expiry_ack_with_locked_barrier(nstu::exam::TelemetryAckStatus::accepted);
}

void test_mixed_expiry_duplicate_ack_retires_exact_batch_while_barrier_deletion_is_blocked() {
    run_mixed_expiry_ack_with_locked_barrier(nstu::exam::TelemetryAckStatus::duplicate);
}

void assert_mixed_cleanup_empty(const MixedCleanupFixture& fixture,
                                const nstu::exam::TelemetryOutbox& loaded, bool cleared,
                                const nstu::exam::TelemetryOutboxAggregateLoss& expected_loss) {
    const auto disk = read_mixed_cleanup_disk(fixture.path);
    assert(loaded.empty() && loaded.size() == 0 && loaded.bytes() == 0);
    assert(loaded.pending_batches().empty());
    assert(disk.batches.empty() && disk.arrivals.empty());
    assert(disk.active_session == (cleared ? nstu::exam::SessionId{} : fixture.current_session));
    if (cleared) {
        assert(disk.losses.empty());
    } else {
        assert(disk.losses.size() == 1);
        assert_mixed_cleanup_loss_equal(disk.losses[0], expected_loss);
    }
    assert_mixed_cleanup_loss_equal(loaded.aggregate_loss(), expected_loss);
    assert_mixed_cleanup_loss_equal(loaded.aggregate_loss(fixture.current_session), expected_loss);
    assert_mixed_cleanup_loss_equal(loaded.aggregate_loss(fixture.stale_session), {});
    assert(!std::filesystem::exists(std::filesystem::path(fixture.path.wstring() + L".revoked")));
    assert(quarantine_files(fixture.path).empty());
}

void run_mixed_expiry_removal_with_locked_barrier(bool cleared) {
    const std::string suffix = cleared ? "clear" : "purge";
    MixedCleanupFixture fixture("removal_" + suffix);
    fixture.assert_state(fixture.outbox, false, false);
    assert(fixture.outbox.is_open() && fixture.outbox.size() == 4);
    HANDLE payload_lock = CreateFileW(fixture.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(payload_lock != INVALID_HANDLE_VALUE);
    std::string error;
    assert(fixture.outbox.prune_expired(fixture.boundary, &error) == 0);
    assert(error == "failed to atomically rename outbox file");
    assert(read_protected_fixture(fixture.path) == fixture.original_payload);
    assert(std::filesystem::last_write_time(fixture.path) == fixture.original_write_time);
    const auto barrier_bytes = read_protected_fixture(fixture.barrier);
    error.clear();
    assert(nstu::security::unprotect_machine_secret(barrier_bytes, mixed_cleanup_entropy(), &error) ==
        mixed_cleanup_time_bytes(fixture.boundary));
    assert(error.empty());
    fixture.assert_state(fixture.outbox, false, true);
    HANDLE barrier_lock = CreateFileW(fixture.barrier.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(barrier_lock != INVALID_HANDLE_VALUE);
    assert(CloseHandle(payload_lock));
    error.clear();
    // Publish the real R1/R2 cleanup before testing successful removal.
    assert(fixture.outbox.prune_expired(fixture.boundary - 1, &error) == 0);
    assert(error == "expiry barrier cleanup failed");
    fixture.assert_state(fixture.outbox, true, true);
    assert(fixture.outbox.is_open() && fixture.outbox.size() == 2);
    const auto cleaned_payload = read_protected_fixture(fixture.path);
    assert(cleaned_payload != fixture.original_payload);
    const auto cleaned_loss = fixture.outbox.aggregate_loss(fixture.current_session);
    assert(cleaned_loss.session_id == fixture.current_session);
    assert(cleaned_loss.rejected_batches == 0 && cleaned_loss.conflict_batches == 1 && cleaned_loss.expired_batches == 2);
    assert(cleaned_loss.first_recorded_unix_ms == fixture.time && cleaned_loss.last_updated_unix_ms >= fixture.time);
    assert(read_protected_fixture(fixture.barrier) == barrier_bytes);
    fixture.assert_alternate_untouched();
    error.clear();
    assert(cleared ? fixture.outbox.clear(&error) : fixture.outbox.purge(&error));
    assert(error.empty() && fixture.outbox.is_open());
    const auto expected_loss = cleared ? nstu::exam::TelemetryOutboxAggregateLoss{} : cleaned_loss;
    assert_mixed_cleanup_empty(fixture, fixture.outbox, cleared, expected_loss);
    const auto empty_payload = read_protected_fixture(fixture.path);
    assert(empty_payload != cleaned_payload);
    const auto empty_write_time = std::filesystem::last_write_time(fixture.path);
    const auto assert_empty_unchanged = [&] {
        assert(fixture.outbox.is_open());
        assert_mixed_cleanup_empty(fixture, fixture.outbox, cleared, expected_loss);
        assert(read_protected_fixture(fixture.path) == empty_payload);
        assert(std::filesystem::last_write_time(fixture.path) == empty_write_time);
        assert(read_protected_fixture(fixture.barrier) == barrier_bytes);
        fixture.assert_alternate_untouched();
    };
    assert_empty_unchanged();
    assert(!fixture.outbox.next_batch_to_send(fixture.boundary - 1));
    assert_empty_unchanged();
    error.clear();
    assert(!fixture.outbox.enqueue(make_batch(105, 2, fixture.time), &error));
    assert(error == "expiry barrier cleanup failed");
    assert_empty_unchanged();
    error.clear();
    assert(!fixture.outbox.open(fixture.alternate_path, fixture.boundary - 1, &error));
    assert(error == "observed expiry cleanup pending; cannot change outbox path");
    assert_empty_unchanged();
    nstu::exam::TelemetryOutbox blocked_restart;
    error.clear();
    assert(!blocked_restart.open(fixture.path, fixture.boundary - 1, &error));
    assert(error == "expiry barrier cleanup failed");
    assert(!blocked_restart.is_open() && blocked_restart.empty() && blocked_restart.size() == 0);
    assert(blocked_restart.bytes() == 0 && blocked_restart.pending_batches().empty());
    assert(!blocked_restart.next_batch_to_send(fixture.boundary - 1));
    assert_mixed_cleanup_loss_equal(blocked_restart.aggregate_loss(), expected_loss);
    assert_mixed_cleanup_loss_equal(blocked_restart.aggregate_loss(fixture.current_session), expected_loss);
    assert_mixed_cleanup_loss_equal(blocked_restart.aggregate_loss(fixture.stale_session), {});
    assert_empty_unchanged();
    assert(CloseHandle(barrier_lock));
    error.clear();
    assert(fixture.outbox.open(fixture.path, fixture.boundary - 1, &error));
    assert(error.empty() && fixture.outbox.is_open());
    assert(!std::filesystem::exists(fixture.barrier));
    assert_mixed_cleanup_empty(fixture, fixture.outbox, cleared, expected_loss);
    assert(read_protected_fixture(fixture.path) == empty_payload);
    assert(std::filesystem::last_write_time(fixture.path) == empty_write_time);
    assert(!fixture.outbox.next_batch_to_send(fixture.boundary - 1));
    assert_mixed_cleanup_empty(fixture, fixture.outbox, cleared, expected_loss);
    fixture.assert_alternate_untouched();
    fixture.outbox.close();
    nstu::exam::TelemetryOutbox restarted;
    for (const auto reopen_time : {fixture.boundary - 1, fixture.boundary - 2, fixture.boundary - 2}) {
        error.clear();
        assert(restarted.open(fixture.path, reopen_time, &error));
        assert(error.empty() && restarted.is_open());
        assert_mixed_cleanup_empty(fixture, restarted, cleared, expected_loss);
        assert(!restarted.next_batch_to_send(reopen_time));
        assert_mixed_cleanup_empty(fixture, restarted, cleared, expected_loss);
        assert(read_protected_fixture(fixture.path) == empty_payload);
        assert(std::filesystem::last_write_time(fixture.path) == empty_write_time);
        assert(!std::filesystem::exists(fixture.barrier));
        fixture.assert_alternate_untouched();
    }
    restarted.close();
    fixture.finish();
    std::cout << "Mixed expiry removal " << suffix
              << ": real locked barrier, successful empty publication, refused gates and empty rollback recovery passed.\n";
}

void test_mixed_expiry_purge_succeeds_after_committed_cleanup_while_barrier_deletion_is_blocked() {
    run_mixed_expiry_removal_with_locked_barrier(false);
}

void test_mixed_expiry_clear_succeeds_after_committed_cleanup_while_barrier_deletion_is_blocked() {
    run_mixed_expiry_removal_with_locked_barrier(true);
}

nstu::exam::SessionId disjoint_loss_context_id(std::uint8_t bank, std::size_t index) {
    nstu::exam::SessionId id{};
    id[0] = static_cast<std::byte>(bank + 1u);
    id[1] = static_cast<std::byte>(index + 1u);
    return id;
}

void write_disjoint_loss_only_fixture(const std::filesystem::path& path,
                                      std::uint8_t bank, std::uint64_t first_recorded) {
    constexpr std::size_t kFixtureLossContexts = 64;
    std::vector<std::byte> payload;
    const auto number = [&](std::uint64_t value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            payload.push_back(static_cast<std::byte>((value >> (8u * i)) & 0xffu));
        }
    };
    number(0x544c4d4f, 4);
    number(2, 2);
    number(0, 2);
    const auto active = disjoint_loss_context_id(bank, 0);
    payload.insert(payload.end(), active.begin(), active.end());
    number(kFixtureLossContexts, 4);
    for (std::size_t index = 0; index < kFixtureLossContexts; ++index) {
        const auto id = disjoint_loss_context_id(bank, index);
        payload.insert(payload.end(), id.begin(), id.end());
        number(1, 8); // rejected
        number(0, 8); // conflicts
        number(0, 8); // expired batches
        number(first_recorded, 8);
        number(first_recorded, 8);
    }
    number(0, 4); // no payload records
    const auto digest = nstu::security::sha256(payload);
    assert(digest);
    payload.insert(payload.end(), digest->begin(), digest->end());
    constexpr std::string_view entropy_text = "NSTU-Exam-Telemetry-Outbox-v1";
    const std::span<const std::byte> entropy{reinterpret_cast<const std::byte*>(entropy_text.data()), entropy_text.size()};
    const auto protected_blob = nstu::security::protect_machine_secret(payload, entropy);
    assert(!protected_blob.empty());
    // Direct writes are allowed by the held FILE_SHARE_WRITE handle, while
    // atomic replacement remains denied. Each reload is independently valid.
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream.is_open());
    stream.write(reinterpret_cast<const char*>(protected_blob.data()), static_cast<std::streamsize>(protected_blob.size()));
    stream.flush();
    assert(stream.good());
}

void test_loss_expiry_observation_union_is_bounded() {
    const auto path = temp_test_path("loss_observation_cap");
    const auto boundary = wall_clock_ms() + 3600000ULL;
    const auto first = boundary - nstu::exam::kTelemetryRetentionMilliseconds;
    write_disjoint_loss_only_fixture(path, 0, first);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path, boundary - 1, nullptr));
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!outbox.next_batch_to_send(boundary)); // Retains exactly 64 observed identities.
    outbox.close();
    write_disjoint_loss_only_fixture(path, 1, first);
    std::string error;
    assert(!outbox.open(path, boundary, &error));
    assert(error.find("expiry observation capacity exceeded") != std::string::npos);
    assert(!outbox.is_open());
    assert(outbox.aggregate_loss(disjoint_loss_context_id(1, 0)).rejected_batches == 0);
    assert(!outbox.next_batch_to_send(boundary - 1));
    assert(CloseHandle(locked));
    error.clear();
    assert(!outbox.open(path, boundary - 1, &error)); // Overflow remains fail closed across rollback.
    assert(error.find("expiry observation capacity exceeded") != std::string::npos);
    nstu::exam::TelemetryOutbox recovered; // Explicit fresh-instance recovery once writes are available.
    assert(recovered.open(path, boundary, nullptr));
    assert(recovered.aggregate_loss().rejected_batches == 0);
    auto fresh = make_batch(1, 2);
    fresh.session_id[0] = std::byte{0xe3};
    assert(recovered.enqueue(fresh));
    assert(recovered.next_batch_to_send(wall_clock_ms()));
}

nstu::exam::TelemetryBatch prepare_expiry_reopen_fixture(
    nstu::exam::TelemetryOutbox& outbox, const std::filesystem::path& path,
    std::uint64_t boundary) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    assert(outbox.open(path));
    const auto expired = make_batch(1, 1, boundary - nstu::exam::kTelemetryRetentionMilliseconds);
    assert(outbox.enqueue(expired));
    // Keep a valid nonempty loss entry so migration/empty-slot pruning cannot
    // independently make a locked reopen fail and mask loss of the expiry latch.
    const auto terminal = make_batch(2);
    assert(outbox.enqueue(terminal));
    nstu::exam::TelemetryAck ack{terminal.session_id, terminal.producer_id, terminal.sequence,
        *nstu::exam::compute_telemetry_batch_hash(terminal), wall_clock_ms(), nstu::exam::TelemetryAckStatus::rejected};
    assert(outbox.acknowledge(ack) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    return expired;
}

void test_observed_expiry_survives_same_instance_reopen_failure() {
    const auto path = temp_test_path("observed_reopen");
    const auto boundary = wall_clock_ms() + 3600000ULL;
    nstu::exam::TelemetryOutbox outbox;
    const auto expired = prepare_expiry_reopen_fixture(outbox, path, boundary);
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!outbox.next_batch_to_send(boundary));
    std::string error;
    assert(!outbox.open(path, boundary - 1, &error));
    assert(!error.empty());
    assert(!outbox.is_open());
    assert(outbox.pending_batches().empty());
    assert(!outbox.next_batch_to_send(boundary - 1));
    assert(CloseHandle(locked));
    assert(outbox.open(path, boundary - 1, nullptr));
    assert(outbox.empty());
    assert(outbox.aggregate_loss(expired.session_id).expired_batches == 1);
    assert(outbox.aggregate_loss(expired.session_id).rejected_batches == 1);
    auto fresh = make_batch(1, 2);
    fresh.session_id[0] = std::byte{0xe1};
    assert(outbox.enqueue(fresh));
    const auto offered = outbox.next_batch_to_send(wall_clock_ms());
    assert(offered && offered->session_id == fresh.session_id);
}

void test_observed_expiry_survives_explicit_close_and_reopen() {
    const auto path = temp_test_path("observed_close_reopen");
    const auto boundary = wall_clock_ms() + 3600000ULL;
    nstu::exam::TelemetryOutbox outbox;
    const auto expired = prepare_expiry_reopen_fixture(outbox, path, boundary);
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!outbox.next_batch_to_send(boundary));
    outbox.close();
    assert(!outbox.is_open() && outbox.pending_batches().empty());
    assert(!outbox.open(path, boundary - 1, nullptr));
    assert(!outbox.next_batch_to_send(boundary - 1));
    assert(CloseHandle(locked));
    assert(outbox.open(path, boundary - 1, nullptr));
    assert(outbox.empty());
    assert(outbox.aggregate_loss(expired.session_id).expired_batches == 1);
    outbox.close();
    assert(outbox.open(path, boundary - 2, nullptr));
    assert(outbox.aggregate_loss(expired.session_id).expired_batches == 1);
}

void test_observed_loss_remains_expired_while_closed_and_reopening() {
    const auto path = temp_test_path("observed_loss_close");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    const auto batch = make_batch(1, 1, now);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(batch));
    nstu::exam::TelemetryAck ack{batch.session_id, batch.producer_id, batch.sequence,
        *nstu::exam::compute_telemetry_batch_hash(batch), now, nstu::exam::TelemetryAckStatus::rejected};
    assert(outbox.acknowledge(ack, now) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    const auto boundary = now + nstu::exam::kTelemetryRetentionMilliseconds;
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!outbox.next_batch_to_send(boundary));
    outbox.close();
    assert(outbox.aggregate_loss(batch.session_id).rejected_batches == 0);
    assert(!outbox.open(path, boundary - 1, nullptr));
    assert(outbox.aggregate_loss(batch.session_id).rejected_batches == 0);
    assert(CloseHandle(locked));
    assert(outbox.open(path, boundary - 1, nullptr));
    assert(outbox.aggregate_loss(batch.session_id).rejected_batches == 0);
    auto fresh = make_batch(1, 2);
    fresh.session_id[0] = std::byte{0xe2};
    assert(outbox.enqueue(fresh));
    assert(outbox.next_batch_to_send(wall_clock_ms()));
}

void test_observed_expiry_survives_failed_cleanup_and_clock_rollback() {
    const auto path = temp_test_path("observed_expiry_rollback");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    const auto expired = make_batch(1, 1, now - nstu::exam::kTelemetryRetentionMilliseconds);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(expired));
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!outbox.next_batch_to_send(now));
    assert(outbox.size() == 1 && outbox.aggregate_loss().expired_batches == 0);
    assert(!outbox.next_batch_to_send(now - 1));
    assert(outbox.size() == 1 && outbox.aggregate_loss().expired_batches == 0);
    assert(CloseHandle(locked));
    assert(!outbox.next_batch_to_send(now - 1));
    assert(outbox.empty());
    assert(outbox.aggregate_loss(expired.session_id).expired_batches == 1);
    assert(!outbox.next_batch_to_send(now - 2));
    assert(outbox.aggregate_loss(expired.session_id).expired_batches == 1);

    auto fresh = make_batch(1, 2, now - 2);
    fresh.session_id[0] = std::byte{0xdd};
    assert(outbox.enqueue(fresh));
    const auto offered = outbox.next_batch_to_send(now - 2);
    assert(offered && offered->session_id == fresh.session_id);
    outbox.close();
    assert(outbox.open(path, now - 2, nullptr));
    assert(outbox.aggregate_loss(expired.session_id).expired_batches == 1);
    assert(outbox.pending_batches().size() == 1);
    assert(outbox.pending_batches().front().session_id == fresh.session_id);
}

void test_observed_loss_expiry_cannot_return_after_failed_cleanup() {
    const auto path = temp_test_path("observed_loss_rollback");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    const auto batch = make_batch(1, 1, now);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(batch));
    nstu::exam::TelemetryAck ack{batch.session_id, batch.producer_id, batch.sequence,
        *nstu::exam::compute_telemetry_batch_hash(batch), now, nstu::exam::TelemetryAckStatus::rejected};
    assert(outbox.acknowledge(ack, now) == nstu::exam::TelemetryAckApplyResult::deleted_terminal);
    const auto expiry = now + nstu::exam::kTelemetryRetentionMilliseconds;
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!outbox.next_batch_to_send(expiry));
    assert(outbox.aggregate_loss().rejected_batches == 0);
    assert(!outbox.next_batch_to_send(expiry - 1));
    assert(outbox.aggregate_loss(batch.session_id).rejected_batches == 0);
    assert(CloseHandle(locked));
    assert(!outbox.next_batch_to_send(expiry - 1));
    assert(outbox.aggregate_loss().rejected_batches == 0);
    auto fresh = make_batch(1, 2);
    fresh.session_id[0] = std::byte{0xde};
    assert(outbox.enqueue(fresh));
    assert(outbox.next_batch_to_send(wall_clock_ms()));
}

void test_automatic_expiry_persistence_failure_is_closed() {
    const auto path = temp_test_path("auto_expiry_failure");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const auto now = wall_clock_ms();
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1, 1, now - nstu::exam::kTelemetryRetentionMilliseconds)));
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!outbox.next_batch_to_send(now));
    assert(outbox.size() == 1);
    assert(outbox.aggregate_loss().expired_batches == 0);
    outbox.close();
    std::string error;
    assert(!outbox.open(path, &error));
    assert(!outbox.is_open());
    assert(!error.empty());
    assert(CloseHandle(locked));
    error.clear();
    assert(outbox.open(path, &error));
    assert(outbox.empty());
    assert(outbox.aggregate_loss().expired_batches == 1);
}
#endif

} // namespace

void test_malformed_expiry_barrier_refuses_replay_and_cleans_old_payload() {
    const auto path = temp_test_path("review_bad_barrier");
    const auto barrier = std::filesystem::path(path.wstring() + L".expiry");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    std::filesystem::remove(barrier, cleanup_error);
    nstu::exam::TelemetryOutbox outbox;
    assert(outbox.open(path));
    assert(outbox.enqueue(make_batch(1)));
    outbox.close();
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 31));
    { std::ofstream stream(barrier, std::ios::binary); stream << "invalid"; }
    std::string error;
    assert(!outbox.open(path, &error));
    assert(!error.empty());
    assert(!outbox.next_batch_to_send());
    assert(!std::filesystem::exists(path));
    assert(std::filesystem::exists(barrier));
    assert(std::filesystem::remove(barrier));
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "review-bad-barrier") { test_malformed_expiry_barrier_refuses_replay_and_cleans_old_payload(); return 0; }
    test_malformed_expiry_barrier_refuses_replay_and_cleans_old_payload();
    std::cout << std::unitbuf;
    if (argc == 2) {
        const std::string_view selected(argv[1]);
        if (selected == "auto-poll") { test_outbox_poll_automatically_expires_records(); return 0; }
        if (selected == "auto-open") { test_outbox_open_automatically_expires_individual_records(); return 0; }
        if (selected == "auto-loss") { test_outbox_poll_expires_loss_without_records(); return 0; }
        if (selected == "boundary") { test_automatic_expiry_exact_boundary_uses_controlled_clock(); return 0; }
        if (selected == "zero-loss") { test_nonzero_loss_without_first_stamp_is_refused(); return 0; }
        if (selected == "concurrent") { test_expired_enqueue_cannot_race_a_send_offer(); return 0; }
#ifdef _WIN32
        if (selected == "mixed-cleanup-removals") {
            test_mixed_expiry_purge_succeeds_after_committed_cleanup_while_barrier_deletion_is_blocked();
            test_mixed_expiry_clear_succeeds_after_committed_cleanup_while_barrier_deletion_is_blocked();
            return 0;
        }
        if (selected == "mixed-cleanup-acks") {
            test_mixed_expiry_accepted_ack_retires_exact_batch_while_barrier_deletion_is_blocked();
            test_mixed_expiry_duplicate_ack_retires_exact_batch_while_barrier_deletion_is_blocked();
            return 0;
        }
        if (selected == "mixed-cleanup") {
            test_mixed_expiry_payload_replacement_failure_preserves_state_and_path();
            test_mixed_expiry_barrier_deletion_failure_preserves_committed_cleanup_and_path();
            return 0;
        }
        if (selected == "protected-barriers") {
            test_protected_seven_byte_expiry_barrier_is_refused();
            test_protected_nine_byte_expiry_barrier_is_refused();
            test_protected_zero_expiry_barrier_is_refused();
            test_tampered_protected_expiry_barrier_is_refused();
            return 0;
        }
        if (selected == "observation-cap") { test_loss_expiry_observation_union_is_bounded(); return 0; }
        if (selected == "observed-reopen") { test_observed_expiry_survives_same_instance_reopen_failure(); return 0; }
        if (selected == "observed-close") { test_observed_expiry_survives_explicit_close_and_reopen(); return 0; }
        if (selected == "observed-loss-close") { test_observed_loss_remains_expired_while_closed_and_reopening(); return 0; }
        if (selected == "observed-expiry") { test_observed_expiry_survives_failed_cleanup_and_clock_rollback(); return 0; }
        if (selected == "observed-loss") { test_observed_loss_expiry_cannot_return_after_failed_cleanup(); return 0; }
        if (selected == "auto-failure") { test_automatic_expiry_persistence_failure_is_closed(); return 0; }
#endif
    }
    std::cout << "Running exam_telemetry_outbox_tests...\n";
    test_outbox_open_and_enqueue();
    test_outbox_ack_handling();
    test_outbox_restart_and_persistence();
    test_outbox_30day_expiry();
    test_outbox_tamper_and_torn_tail();
    test_outbox_lost_ack_and_inflight();
    test_outbox_purge_and_clear();
    test_outbox_storage_unavailable();
    test_outbox_quota_limits();
    test_outbox_byte_flip_tamper();
    test_outbox_ack_extended();
    test_outbox_retry_backoff_cap_and_clock_jump();
    test_outbox_context_separation();
    test_outbox_completed_contexts_release_empty_loss_slots();
    test_outbox_future_stamp_expiry();
    test_outbox_future_loaded_stamp_clamps_once();
    test_outbox_quarantine_retention();
#ifdef _WIN32
    test_outbox_transient_read_failure();
    test_outbox_purge_delete_failure();
#endif
    test_outbox_move_safety();
    test_outbox_persist_failure_rollback();
    test_outbox_poll_automatically_expires_records();
    test_outbox_open_automatically_expires_individual_records();
    test_outbox_poll_expires_loss_without_records();
    test_automatic_expiry_exact_boundary_uses_controlled_clock();
    test_nonzero_loss_without_first_stamp_is_refused();
    test_expired_enqueue_cannot_race_a_send_offer();
#ifdef _WIN32
    test_mixed_expiry_payload_replacement_failure_preserves_state_and_path();
    test_mixed_expiry_barrier_deletion_failure_preserves_committed_cleanup_and_path();
    test_mixed_expiry_accepted_ack_retires_exact_batch_while_barrier_deletion_is_blocked();
    test_mixed_expiry_duplicate_ack_retires_exact_batch_while_barrier_deletion_is_blocked();
    test_mixed_expiry_purge_succeeds_after_committed_cleanup_while_barrier_deletion_is_blocked();
    test_mixed_expiry_clear_succeeds_after_committed_cleanup_while_barrier_deletion_is_blocked();
    test_protected_seven_byte_expiry_barrier_is_refused();
    test_protected_nine_byte_expiry_barrier_is_refused();
    test_protected_zero_expiry_barrier_is_refused();
    test_tampered_protected_expiry_barrier_is_refused();
    test_automatic_expiry_persistence_failure_is_closed();
    test_loss_expiry_observation_union_is_bounded();
    test_observed_expiry_survives_same_instance_reopen_failure();
    test_observed_expiry_survives_explicit_close_and_reopen();
    test_observed_loss_remains_expired_while_closed_and_reopening();
    test_observed_expiry_survives_failed_cleanup_and_clock_rollback();
    test_observed_loss_expiry_cannot_return_after_failed_cleanup();
#endif

    // Clean up temporary test files (L6)
    std::error_code ec;
    std::filesystem::remove_all(
        std::filesystem::temp_directory_path() / "nstu_telemetry_outbox_tests", ec);

    std::cout << "All exam_telemetry_outbox_tests passed successfully.\n";
    return 0;
}
