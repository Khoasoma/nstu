#include "nstu/exam_telemetry.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename Array>
void fill_bytes(Array& bytes, std::uint8_t first) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(first + index);
    }
}

nstu::exam::TelemetrySample make_sample(std::uint64_t collector_id,
                                       nstu::exam::TelemetrySampleKind kind) {
    nstu::exam::TelemetrySample sample;
    sample.kind = kind;
    sample.source = nstu::exam::TelemetrySource::browser;
    sample.collector_id = collector_id;
    sample.question_id = "q-01";
    sample.question_revision = 1;
    sample.monotonic_elapsed_milliseconds = 12500;
    sample.duration_milliseconds = 5000;
    sample.client_time_unix_milliseconds = 1'700'000'000'000ULL;
    sample.numeric_value = 42;
    sample.count = 3;
    sample.reason = nstu::exam::TelemetryReason::collector_initial;
    return sample;
}

nstu::exam::TelemetryBatch make_batch(std::uint64_t sequence) {
    nstu::exam::TelemetryBatch batch;
    batch.package_id = "exam-package-01";
    fill_bytes(batch.package_digest, 0x11);
    fill_bytes(batch.client_id, 0x22);
    fill_bytes(batch.session_id, 0x33);
    batch.candidate_id = "candidate-42";
    fill_bytes(batch.producer_id, 0x44);
    batch.sequence = sequence;
    batch.first_enqueued_at_unix_milliseconds = 1'700'000'000'100ULL;
    batch.samples.push_back(
        make_sample(1001, nstu::exam::TelemetrySampleKind::question_first_view));
    batch.samples.push_back(
        make_sample(1001, nstu::exam::TelemetrySampleKind::question_dwell));
    return batch;
}

void test_sample_round_trip() {
    using namespace nstu::exam;
    const auto sample = make_sample(1234, TelemetrySampleKind::question_dwell);
    const auto encoded = encode_telemetry_sample(sample);
    assert(!encoded.empty());

    std::string error;
    const auto decoded = decode_telemetry_sample(encoded, &error);
    assert(decoded.has_value());
    assert(error.empty());
    assert(decoded->kind == sample.kind);
    assert(decoded->source == sample.source);
    assert(decoded->collector_id == sample.collector_id);
    assert(decoded->question_id == sample.question_id);
    assert(decoded->question_revision == sample.question_revision);
    assert(decoded->monotonic_elapsed_milliseconds ==
           sample.monotonic_elapsed_milliseconds);
    assert(decoded->duration_milliseconds == sample.duration_milliseconds);
    assert(decoded->client_time_unix_milliseconds ==
           sample.client_time_unix_milliseconds);
    assert(decoded->numeric_value == sample.numeric_value);
    assert(decoded->count == sample.count);
    assert(decoded->reason == sample.reason);
}

void test_sample_rejections() {
    using namespace nstu::exam;
    auto sample = make_sample(1234, TelemetrySampleKind::question_dwell);

    // Zero collector_id rejected
    sample.collector_id = 0;
    assert(encode_telemetry_sample(sample).empty());
    sample.collector_id = 1234;

    // Zero client time rejected
    sample.client_time_unix_milliseconds = 0;
    assert(encode_telemetry_sample(sample).empty());
    sample.client_time_unix_milliseconds = 1'700'000'000'000ULL;

    // Nonempty question_id with question_revision == 0 rejected
    sample.question_id = "q-01";
    sample.question_revision = 0;
    assert(encode_telemetry_sample(sample).empty());
    sample.question_revision = 1;

    // Empty question_id with question_revision != 0 rejected
    sample.question_id = "";
    sample.question_revision = 1;
    assert(encode_telemetry_sample(sample).empty());
    // Empty question_id with question_revision == 0 allowed
    sample.question_revision = 0;
    assert(!encode_telemetry_sample(sample).empty());
    sample.question_id = "valid-q";
    sample.question_revision = 1;

    // Unknown reason value rejected on encode
    sample.reason = static_cast<TelemetryReason>(999);
    assert(encode_telemetry_sample(sample).empty());
    sample.reason = TelemetryReason::collector_initial;

    // Unknown reason value rejected on decode
    auto valid_wire = encode_telemetry_sample(sample);
    assert(!valid_wire.empty());
    // Reason is at offset 46 (4 bytes)
    valid_wire[46] = std::byte{0xff};
    valid_wire[47] = std::byte{0xff};
    assert(!decode_telemetry_sample(valid_wire).has_value());

    // Oversized question_id rejected
    sample.question_id = std::string(kMaximumQuestionIdBytes + 1, 'a');
    assert(encode_telemetry_sample(sample).empty());
    sample.question_id = "valid-q";

    // Invalid UTF-8 / embedded null in question_id rejected
    sample.question_id = std::string("bad\0id", 6);
    assert(encode_telemetry_sample(sample).empty());
    sample.question_id = "valid-q";

    // Trailing bytes rejected on decode
    auto encoded = encode_telemetry_sample(sample);
    encoded.push_back(std::byte{0x00});
    assert(!decode_telemetry_sample(encoded).has_value());
}

void test_batch_round_trip() {
    using namespace nstu::exam;
    const auto batch = make_batch(1);
    const auto encoded = encode_telemetry_batch(batch);
    assert(!encoded.empty());
    assert(encoded.size() <= kMaximumBatchBytes);

    std::string error;
    const auto decoded = decode_telemetry_batch(encoded, &error);
    assert(decoded.has_value());
    assert(error.empty());
    assert(decoded->package_id == batch.package_id);
    assert(decoded->package_digest == batch.package_digest);
    assert(decoded->client_id == batch.client_id);
    assert(decoded->session_id == batch.session_id);
    assert(decoded->candidate_id == batch.candidate_id);
    assert(decoded->producer_id == batch.producer_id);
    assert(decoded->sequence == batch.sequence);
    assert(decoded->first_enqueued_at_unix_milliseconds ==
           batch.first_enqueued_at_unix_milliseconds);
    assert(decoded->samples.size() == 2);
    assert(decoded->samples[0].kind == batch.samples[0].kind);
    assert(decoded->samples[1].kind == batch.samples[1].kind);

    // Canonical batch hash matches computation
    const auto expected_hash = compute_telemetry_batch_hash(batch);
    const auto actual_hash = compute_telemetry_batch_hash(*decoded);
    assert(expected_hash.has_value());
    assert(actual_hash.has_value());
    assert(*expected_hash == *actual_hash);
}

void test_batch_shortest_valid_ids() {
    using namespace nstu::exam;
    TelemetryBatch batch;
    batch.package_id = "p"; // 1 char
    fill_bytes(batch.package_digest, 0x11);
    fill_bytes(batch.client_id, 0x22);
    fill_bytes(batch.session_id, 0x33);
    batch.candidate_id = "c"; // 1 char
    fill_bytes(batch.producer_id, 0x44);
    batch.sequence = 1;
    batch.first_enqueued_at_unix_milliseconds = 1'700'000'000'100ULL;

    TelemetrySample sample = make_sample(101, TelemetrySampleKind::collector_started);
    sample.question_id = "";
    sample.question_revision = 0;
    batch.samples.push_back(sample);

    const auto encoded = encode_telemetry_batch(batch);
    assert(!encoded.empty());

    std::string error;
    const auto decoded = decode_telemetry_batch(encoded, &error);
    assert(decoded.has_value());
    assert(error.empty());
    assert(decoded->package_id == "p");
    assert(decoded->candidate_id == "c");
    assert(decoded->samples.size() == 1);
    assert(decoded->samples[0].question_id.empty());
    assert(decoded->samples[0].question_revision == 0);
}

void test_batch_rejections() {
    using namespace nstu::exam;
    auto batch = make_batch(1);

    // Sequence 0 rejected
    batch.sequence = 0;
    assert(encode_telemetry_batch(batch).empty());
    batch.sequence = 1;

    // Zero producer ID rejected
    batch.producer_id.fill(std::byte{0});
    assert(encode_telemetry_batch(batch).empty());
    fill_bytes(batch.producer_id, 0x44);

    // Zero session ID rejected
    batch.session_id.fill(std::byte{0});
    assert(encode_telemetry_batch(batch).empty());
    fill_bytes(batch.session_id, 0x33);

    // Empty samples rejected
    batch.samples.clear();
    assert(encode_telemetry_batch(batch).empty());
    batch.samples.push_back(
        make_sample(1001, TelemetrySampleKind::collector_started));

    // >32 samples rejected
    for (int i = 0; i < 35; ++i) {
        batch.samples.push_back(
            make_sample(1001, TelemetrySampleKind::question_dwell));
    }
    assert(encode_telemetry_batch(batch).empty());
    batch = make_batch(1);

    // Corrupted / truncated payload rejected
    auto encoded = encode_telemetry_batch(batch);
    encoded.resize(encoded.size() - 5);
    assert(!decode_telemetry_batch(encoded).has_value());

    // Trailing bytes rejected
    encoded = encode_telemetry_batch(batch);
    encoded.push_back(std::byte{0xff});
    assert(!decode_telemetry_batch(encoded).has_value());
}

void test_ack_round_trip() {
    using namespace nstu::exam;
    for (const auto status : {
             TelemetryAckStatus::accepted,
             TelemetryAckStatus::duplicate,
             TelemetryAckStatus::rejected,
             TelemetryAckStatus::conflict,
             TelemetryAckStatus::unavailable,
         }) {
        TelemetryAck ack;
        fill_bytes(ack.session_id, 0x12);
        fill_bytes(ack.producer_id, 0x34);
        ack.sequence = 5;
        fill_bytes(ack.batch_hash, 0x56);
        ack.server_time_unix_milliseconds = 1'700'000'000'200ULL;
        ack.status = status;

        const auto encoded = encode_telemetry_ack(ack);
        assert(!encoded.empty());

        std::string error;
        const auto decoded = decode_telemetry_ack(encoded, &error);
        assert(decoded.has_value());
        assert(error.empty());
        assert(decoded->session_id == ack.session_id);
        assert(decoded->producer_id == ack.producer_id);
        assert(decoded->sequence == ack.sequence);
        assert(decoded->batch_hash == ack.batch_hash);
        assert(decoded->server_time_unix_milliseconds ==
               ack.server_time_unix_milliseconds);
        assert(decoded->status == status);
    }
}

void test_ack_rejections() {
    using namespace nstu::exam;
    TelemetryAck ack;
    fill_bytes(ack.session_id, 0x12);
    fill_bytes(ack.producer_id, 0x34);
    ack.sequence = 5;
    fill_bytes(ack.batch_hash, 0x56);
    ack.server_time_unix_milliseconds = 1'700'000'000'200ULL;
    ack.status = TelemetryAckStatus::accepted;

    // Sequence 0 rejected
    ack.sequence = 0;
    assert(encode_telemetry_ack(ack).empty());
    ack.sequence = 5;

    // Zero session ID rejected
    ack.session_id.fill(std::byte{0});
    assert(encode_telemetry_ack(ack).empty());
    fill_bytes(ack.session_id, 0x12);

    // Zero producer ID rejected
    ack.producer_id.fill(std::byte{0});
    assert(encode_telemetry_ack(ack).empty());
    fill_bytes(ack.producer_id, 0x34);

    // Trailing bytes rejected
    auto encoded = encode_telemetry_ack(ack);
    encoded.push_back(std::byte{0x00});
    assert(!decode_telemetry_ack(encoded).has_value());
}

void test_enable_round_trip() {
    using namespace nstu::exam;
    TelemetryEnable enable;
    enable.package_id = "exam-package-01";
    fill_bytes(enable.package_digest, 0x11);
    fill_bytes(enable.client_id, 0x22);
    fill_bytes(enable.session_id, 0x33);
    enable.candidate_id = "candidate-42";
    enable.server_time_unix_milliseconds = 1'700'000'000'050ULL;

    const auto encoded = encode_telemetry_enable(enable);
    assert(!encoded.empty());

    std::string error;
    const auto decoded = decode_telemetry_enable(encoded, &error);
    assert(decoded.has_value());
    assert(error.empty());
    assert(decoded->package_id == enable.package_id);
    assert(decoded->package_digest == enable.package_digest);
    assert(decoded->client_id == enable.client_id);
    assert(decoded->session_id == enable.session_id);
    assert(decoded->candidate_id == enable.candidate_id);
    assert(decoded->server_time_unix_milliseconds ==
           enable.server_time_unix_milliseconds);
}

void test_enable_shortest_valid_ids() {
    using namespace nstu::exam;
    TelemetryEnable enable;
    enable.package_id = "p"; // 1 char shortest valid
    fill_bytes(enable.package_digest, 0x11);
    fill_bytes(enable.client_id, 0x22);
    fill_bytes(enable.session_id, 0x33);
    enable.candidate_id = "c"; // 1 char shortest valid
    enable.server_time_unix_milliseconds = 1'700'000'000'050ULL;

    const auto encoded = encode_telemetry_enable(enable);
    assert(!encoded.empty());
    // 84 fixed bytes + 1 + 1 = 86 bytes
    assert(encoded.size() == kTelemetryEnableFixedBytes + 2);
    assert(encoded.size() == 86);

    std::string error;
    const auto decoded = decode_telemetry_enable(encoded, &error);
    assert(decoded.has_value());
    assert(error.empty());
    assert(decoded->package_id == "p");
    assert(decoded->candidate_id == "c");

    // 85 bytes (truncated by 1 byte) must be rejected
    auto truncated = encoded;
    truncated.pop_back();
    assert(truncated.size() == 85);
    assert(!decode_telemetry_enable(truncated).has_value());
}

void test_enable_rejections() {
    using namespace nstu::exam;
    TelemetryEnable enable;
    enable.package_id = "exam-package-01";
    fill_bytes(enable.package_digest, 0x11);
    fill_bytes(enable.client_id, 0x22);
    fill_bytes(enable.session_id, 0x33);
    enable.candidate_id = "candidate-42";
    enable.server_time_unix_milliseconds = 1'700'000'000'050ULL;

    // Zero session ID rejected
    enable.session_id.fill(std::byte{0});
    assert(encode_telemetry_enable(enable).empty());
    fill_bytes(enable.session_id, 0x33);

    // Trailing bytes rejected
    auto encoded = encode_telemetry_enable(enable);
    encoded.push_back(std::byte{0x00});
    assert(!decode_telemetry_enable(encoded).has_value());
}

void test_canonical_hash_identity() {
    using namespace nstu::exam;
    auto batch1 = make_batch(1);
    auto batch2 = make_batch(1);
    const auto hash1 = compute_telemetry_batch_hash(batch1);
    const auto hash2 = compute_telemetry_batch_hash(batch2);
    assert(hash1.has_value());
    assert(hash2.has_value());
    assert(*hash1 == *hash2);

    // Changing sequence changes hash
    batch2.sequence = 2;
    const auto hash_seq2 = compute_telemetry_batch_hash(batch2);
    assert(hash_seq2.has_value());
    assert(*hash1 != *hash_seq2);
    batch2.sequence = 1;

    // Changing producer changes hash
    batch2.producer_id[0] = std::byte{0x99};
    const auto hash_prod2 = compute_telemetry_batch_hash(batch2);
    assert(hash_prod2.has_value());
    assert(*hash1 != *hash_prod2);
    batch2.producer_id = batch1.producer_id;

    // Changing sample changes hash
    batch2.samples[0].numeric_value = 999;
    const auto hash_sample2 = compute_telemetry_batch_hash(batch2);
    assert(hash_sample2.has_value());
    assert(*hash1 != *hash_sample2);

    // Failure preservation: invalid batch returns std::nullopt
    TelemetryBatch invalid_batch;
    invalid_batch.sequence = 0;
    assert(!compute_telemetry_batch_hash(invalid_batch).has_value());
}

} // namespace

int main() {
    test_sample_round_trip();
    test_sample_rejections();
    test_batch_round_trip();
    test_batch_shortest_valid_ids();
    test_batch_rejections();
    test_ack_round_trip();
    test_ack_rejections();
    test_enable_round_trip();
    test_enable_shortest_valid_ids();
    test_enable_rejections();
    test_canonical_hash_identity();

    std::cout << "All telemetry codec tests passed!\n";
    return 0;
}
