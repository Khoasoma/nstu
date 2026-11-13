#include "nstu/programming.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nstu::exam::programming::Operation;
using nstu::exam::programming::Request;
using nstu::exam::programming::decode_request;
using nstu::exam::programming::encode_request;
using nstu::exam::programming::validate_request;

Request make_request(Operation operation = Operation::draft) {
    Request request;
    request.context.package_id = "cs-package";
    request.context.candidate_id = "candidate-01";
    request.context.package_digest[0] = std::byte{1};
    request.context.client_id[0] = std::byte{2};
    request.context.session_id[0] = std::byte{3};
    request.operation = operation;
    request.request_id = "request-01";
    if (operation != Operation::finalize && operation != Operation::ready) {
        request.problem_id = "sum-01";
        request.problem_revision = 3;
        request.language = "cpp20";
    }
    if (operation == Operation::draft || operation == Operation::submit ||
        operation == Operation::run) {
        request.draft_revision = 7;
        request.source = "int main() {\n\treturn 0;\r\n}\n";
    }
    if (operation == Operation::run) request.sample_id = "sample-01";
    if (operation == Operation::cancel) request.target_run_id = "earlier-run-01";
    return request;
}

void assert_round_trip(const Request& original) {
    assert(validate_request(original));
    const auto wire = encode_request(original);
    assert(!wire.empty());
    assert(wire.size() <= nstu::exam::kMaximumExamPayloadBytes);
    const auto decoded = decode_request(wire);
    assert(decoded.has_value());
    assert(nstu::exam::encode_state_request(decoded->context) ==
           nstu::exam::encode_state_request(original.context));
    assert(decoded->operation == original.operation);
    assert(decoded->request_id == original.request_id);
    assert(decoded->problem_id == original.problem_id);
    assert(decoded->problem_revision == original.problem_revision);
    assert(decoded->language == original.language);
    assert(decoded->draft_revision == original.draft_revision);
    assert(decoded->source == original.source);
    assert(decoded->sample_id == original.sample_id);
    assert(decoded->target_run_id == original.target_run_id);
    assert(decoded->receipt_ids == original.receipt_ids);
    assert(decoded->no_source_problem_ids == original.no_source_problem_ids);
    assert(decoded->workstation_policy_ready == original.workstation_policy_ready);
    assert(encode_request(*decoded) == wire);
}

void assert_invalid(const Request& request) {
    assert(!validate_request(request));
    assert(encode_request(request).empty());
}

void test_round_trips_and_limits() {
    auto attested_ready = make_request(Operation::ready);
    attested_ready.workstation_policy_ready = true;
    assert_round_trip(attested_ready);
    auto forged_draft = make_request(Operation::draft);
    forged_draft.workstation_policy_ready = true;
    assert(!validate_request(forged_draft));
    using namespace nstu::exam::programming;
    for (const auto operation : {Operation::draft, Operation::submit,
                                 Operation::run, Operation::cancel,
                                 Operation::recover, Operation::finalize,
                                 Operation::ready}) {
        assert_round_trip(make_request(operation));
    }
    for (const std::string_view language : {"cpp20", "python3"}) {
        auto request = make_request(Operation::run);
        request.language = language;
        request.request_id.assign(kMaximumIdBytes, 'r');
        request.problem_id.assign(kMaximumIdBytes, 'p');
        request.sample_id.assign(kMaximumIdBytes, 's');
        request.context.package_id.assign(nstu::exam::kMaximumPackageIdBytes,
                                          'p');
        request.context.candidate_id.assign(
            nstu::exam::kMaximumCandidateIdBytes, 'c');
        request.problem_revision = std::numeric_limits<std::uint32_t>::max();
        request.draft_revision = kMaximumDraftRevision;
        // Include every UTF-8 width and permitted ASCII control character.
        const std::string unicode = "\t\r\n\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80";
        request.source.assign(kMaximumSourceBytes - unicode.size(), 'x');
        request.source += unicode;
        assert_round_trip(request);
        request.source.push_back('x');
        assert_invalid(request);
    }
    auto cleared_draft = make_request();
    cleared_draft.source.clear();
    assert_round_trip(cleared_draft);
    auto finalize = make_request(Operation::finalize);
    for (std::size_t index = 0; index < kMaximumInventoryEntries; ++index) {
        const auto suffix = std::to_string(index);
        finalize.receipt_ids.push_back(
            std::string(kMaximumIdBytes - suffix.size(), 'r') + suffix);
        finalize.no_source_problem_ids.push_back(
            std::string(kMaximumIdBytes - suffix.size(), 'p') + suffix);
    }
    assert_round_trip(finalize);
    assert(operation_name(Operation::draft) == "draft");
    assert(operation_name(Operation::submit) == "submit");
    assert(operation_name(Operation::run) == "run");
    assert(operation_name(Operation::cancel) == "cancel");
    assert(operation_name(Operation::recover) == "recover");
    assert(operation_name(Operation::finalize) == "finalize");
    assert(operation_name(Operation::ready) == "ready");
    assert(operation_name(static_cast<Operation>(0)).empty());
}

void test_operation_fields() {
    for (const auto operation : {Operation::draft, Operation::submit,
                                 Operation::run}) {
        auto request = make_request(operation);
        request.draft_revision = 0;
        assert_invalid(request);
        request.draft_revision = nstu::exam::programming::kMaximumDraftRevision + 1;
        assert_invalid(request);
        if (operation != Operation::draft) {
            request = make_request(operation);
            request.source.clear();
            assert_invalid(request);
        }
    }
    auto missing_sample = make_request(Operation::run);
    missing_sample.sample_id.clear();
    assert_invalid(missing_sample);
    auto missing_target = make_request(Operation::cancel);
    missing_target.target_run_id.clear();
    assert_invalid(missing_target);
    auto self_target = make_request(Operation::cancel);
    self_target.target_run_id = self_target.request_id;
    assert_invalid(self_target);
    for (const auto operation : {Operation::draft, Operation::submit,
                                 Operation::run, Operation::recover,
                                 Operation::finalize, Operation::ready}) {
        auto request = make_request(operation);
        request.target_run_id = "earlier-run-01";
        assert_invalid(request);
    }
    for (const auto operation : {Operation::draft, Operation::submit,
                                 Operation::cancel, Operation::recover,
                                 Operation::finalize, Operation::ready}) {
        auto request = make_request(operation);
        request.sample_id = "sample-01";
        assert_invalid(request);
    }
    for (const auto operation : {Operation::cancel, Operation::recover,
                                 Operation::finalize, Operation::ready}) {
        auto request = make_request(operation);
        request.source = "unexpected";
        assert_invalid(request);
        request = make_request(operation);
        request.draft_revision = 1;
        assert_invalid(request);
    }
    for (const auto operation : {Operation::draft, Operation::submit,
                                 Operation::run, Operation::cancel,
                                 Operation::recover}) {
        auto request = make_request(operation);
        request.problem_id.clear();
        assert_invalid(request);
        request = make_request(operation);
        request.problem_revision = 0;
        assert_invalid(request);
        request = make_request(operation);
        request.language.clear();
        assert_invalid(request);
    }
    for (const auto operation : {Operation::finalize, Operation::ready}) {
        auto request = make_request(operation);
        request.problem_id = "sum-01";
        assert_invalid(request);
        request = make_request(operation);
        request.language = "cpp20";
        assert_invalid(request);
        request = make_request(operation);
        request.problem_revision = 2;
        assert_invalid(request);
    }
}

void test_untrusted_fields_and_context() {
    using namespace nstu::exam::programming;
    for (const std::string& id : {std::string{}, std::string(kMaximumIdBytes + 1, 'x'),
                                 std::string{"../problem"}, std::string{"C:\\source"},
                                 std::string{"bad id"}, std::string{"bad\n"},
                                 std::string{"bad\0id", 6}, std::string{"\xc3\xa9"},
                                 std::string{"-bad"}, std::string{"."},
                                 std::string{".."}, std::string{"bad:id"}}) {
        auto request = make_request();
        request.request_id = id;
        assert_invalid(request);
        request = make_request();
        request.problem_id = id;
        assert_invalid(request);
        request = make_request(Operation::run);
        request.sample_id = id;
        assert_invalid(request);
        request = make_request(Operation::cancel);
        request.target_run_id = id;
        assert_invalid(request);
    }
    for (const std::string_view language : {"c++", "python", "cpp20 -O2"}) {
        auto request = make_request();
        request.language = language;
        assert_invalid(request);
    }
    for (const unsigned int byte : {0u, 1u, 8u, 11u, 12u, 31u, 127u}) {
        auto request = make_request();
        request.source.push_back(static_cast<char>(byte));
        assert_invalid(request);
    }
    for (const std::string& source : {std::string{"\x80"}, std::string{"\xc0\xaf"},
                                     std::string{"\xe0\x80\x80"},
                                     std::string{"\xed\xa0\x80"},
                                     std::string{"\xf0\x80\x80\x80"},
                                     std::string{"\xf4\x90\x80\x80"},
                                     std::string{"\xf5\x80\x80\x80"},
                                     std::string{"\xe2\x82"},
                                     std::string{"\xe2\x28\xa1"}}) {
        auto request = make_request();
        request.source = source;
        assert_invalid(request);
    }
    auto request = make_request();
    request.context.package_digest.fill(std::byte{0});
    assert_invalid(request);
    request = make_request();
    request.context.client_id.fill(std::byte{0});
    assert_invalid(request);
    request = make_request();
    request.context.session_id.fill(std::byte{0});
    assert_invalid(request);
    request = make_request();
    request.context.package_id.clear();
    assert_invalid(request);
    request = make_request();
    request.context.candidate_id.assign(nstu::exam::kMaximumCandidateIdBytes + 1,
                                        'c');
    assert_invalid(request);
    request = make_request();
    request.context.package_id = std::string{"bad\0package", 11};
    assert_invalid(request);
    for (const unsigned int operation : {0u, 8u, 255u}) {
        request = make_request();
        request.operation = static_cast<Operation>(operation);
        assert_invalid(request);
    }
}

void test_finalize_inventory() {
    using namespace nstu::exam::programming;
    auto finalize = make_request(Operation::finalize);
    finalize.receipt_ids = {"receipt-01", "receipt_02"};
    finalize.no_source_problem_ids = {"problem-03"};
    assert_round_trip(finalize);
    for (const auto operation : {Operation::draft, Operation::submit,
                                 Operation::run, Operation::cancel,
                                 Operation::recover, Operation::ready}) {
        auto request = make_request(operation);
        request.receipt_ids = {"receipt-01"};
        assert_invalid(request);
        request = make_request(operation);
        request.no_source_problem_ids = {"problem-01"};
        assert_invalid(request);
    }
    for (const std::vector<std::string>& ids : {
             std::vector<std::string>{""}, std::vector<std::string>{"../problem"},
             std::vector<std::string>{"same", "same"},
             std::vector<std::string>{std::string(kMaximumIdBytes + 1, 'x')},
             std::vector<std::string>(kMaximumInventoryEntries + 1, "id")}) {
        auto request = make_request(Operation::finalize);
        request.receipt_ids = ids;
        assert_invalid(request);
        request = make_request(Operation::finalize);
        request.no_source_problem_ids = ids;
        assert_invalid(request);
    }
    const auto wire = encode_request(finalize);
    auto malformed = wire;
    // Inventory counts follow draft revision in the fixed header.
    malformed[34] = std::byte{0xff};
    malformed[35] = std::byte{0xff};
    assert(!decode_request(malformed));
    malformed = wire;
    const auto first_entry = 40 +
        nstu::exam::encode_state_request(finalize.context).size() +
        finalize.request_id.size();
    malformed[first_entry] = std::byte{0};
    malformed[first_entry + 1] = std::byte{0};
    assert(!decode_request(malformed));
}

void test_malformed_wire() {
    const auto original = make_request(Operation::run);
    const auto wire = encode_request(original);
    assert(!wire.empty());
    assert(wire[0] == std::byte{'C'} && wire[1] == std::byte{'S'} &&
           wire[2] == std::byte{'0'} && wire[3] == std::byte{'1'});
    for (std::size_t length = 0; length < wire.size(); ++length) {
        assert(!decode_request(std::span<const std::byte>{wire}.first(length)));
    }
    auto malformed = wire;
    malformed.push_back(std::byte{0});
    assert(!decode_request(malformed));
    // Header: magic(4), version(2), operation(1), reserved(1), five u16
    // lengths, source length(4), problem revision(4), draft revision(8),
    // receipt count(2), no-source count(2), target-run length(2), then context.
    for (const std::size_t offset : {std::size_t{0}, std::size_t{4},
                                     std::size_t{6}, std::size_t{7},
                                     std::size_t{8}, std::size_t{10},
                                     std::size_t{18}, std::size_t{40}}) {
        malformed = wire;
        malformed[offset] = std::byte{0xff};
        assert(!decode_request(malformed));
    }
    malformed = wire;
    malformed.back() = std::byte{0xff};
    assert(!decode_request(malformed));
    malformed = wire;
    const auto context_bytes = nstu::exam::encode_state_request(original.context);
    malformed[40 + context_bytes.size()] = std::byte{'/'};
    assert(!decode_request(malformed));
    malformed.assign(nstu::exam::kMaximumExamPayloadBytes + 1, std::byte{0});
    assert(!decode_request(malformed));
}

void test_broker_context_contract() {
    using namespace nstu::exam::programming;
    const auto request = make_request(Operation::ready);
    auto prefix = context_json(request.context);
    prefix.pop_back();
    const auto known = prefix + R"(,"programmingVersion":1,"status":"known","serverTime":1700000000000})";
    assert(validate_known_context_json(known, request.context));
    auto other = request.context;
    other.session_id[0] = std::byte{7};
    assert(!validate_known_context_json(known, other));
    assert(!validate_known_context_json(prefix + R"(,"programmingVersion":1,"status":"unknown","serverTime":1700000000000})", request.context));
    assert(!validate_known_context_json(prefix + R"(,"programmingVersion":1,"status":"known","serverTime":0})", request.context));
    assert(!validate_known_context_json(prefix + R"(,"programmingVersion":1,"status":"known","serverTime":1700000000000,"score":100})", request.context));
    assert(!validate_known_context_json(prefix + R"(,"programmingVersion":1,"status":"known","serverTime":1700000000000,"status":"known"})", request.context));
    assert(!validate_known_context_json(prefix + R"(,"programmingVersion":2,"status":"known","serverTime":1700000000000})", request.context));
    assert(validate_prepared_json(prefix + R"(,"programmingVersion":1,"status":"prepared"})", request.context));
    assert(!validate_prepared_json(known, request.context));
    assert(!validate_prepared_json(prefix + R"(,"programmingVersion":1,"status":"prepared","gradingDigestHex":"private"})", request.context));
    auto attested = request;
    attested.workstation_policy_ready = true;
    assert(request_json(attested) == request_json(request));
    assert(request_json(attested).find("workstation") == std::string::npos);
    assert(request_json(attested).find("recoveryOnly") == std::string::npos);
    const auto begun = prefix + R"(,"programmingVersion":1,"status":"begun","requestId":"begin-01","serverStartUnixMilliseconds":1700000000000,"deadlineUnixMilliseconds":1700000060000})";
    const auto lifecycle = parse_lifecycle_json(begun, request.context, "begin-01", true);
    assert(lifecycle && lifecycle->deadline - lifecycle->server_start == 60000);
    assert(!parse_lifecycle_json(begun, other, "begin-01", true));
    assert(!parse_lifecycle_json(begun, request.context, "another", true));
    assert(!parse_lifecycle_json(prefix + R"(,"programmingVersion":1,"status":"begun","requestId":"begin-01","serverStartUnixMilliseconds":1700000000000,"deadlineUnixMilliseconds":1700000000000})", request.context, "begin-01", true));
}

void test_service_issued_context() {
    using namespace nstu::exam::programming;
    const auto request = make_request();
    assert(validate_service_request(request, request.context));
    for (unsigned field = 0; field < 5; ++field) {
        auto issued = request.context;
        if (field == 0) issued.package_id = "different-package";
        if (field == 1) issued.package_digest[0] = std::byte{9};
        if (field == 2) issued.client_id[0] = std::byte{9};
        if (field == 3) issued.session_id[0] = std::byte{9};
        if (field == 4) issued.candidate_id = "different-candidate";
        assert(!validate_service_request(request, issued));
    }
    auto ready = make_request(Operation::ready);
    assert(validate_service_request(ready, ready.context));
    ready.workstation_policy_ready = true;
    assert(validate_request(ready));
    assert(!validate_service_request(ready, ready.context));
    auto invalid = request;
    invalid.draft_revision = 0;
    assert(!validate_service_request(invalid, invalid.context));
}

} // namespace

int main() {
    test_round_trips_and_limits();
    test_operation_fields();
    test_untrusted_fields_and_context();
    test_finalize_inventory();
    test_malformed_wire();
    test_broker_context_contract();
    test_service_issued_context();
    return 0;
}
