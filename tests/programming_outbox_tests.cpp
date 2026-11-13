#include "nstu/programming_outbox.hpp"
#include "nstu/auth.hpp"
#include <windows.h>
#include <sddl.h>

#include <cassert>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
using namespace nstu::exam::programming;
std::string hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto value : bytes) {
        const auto byte = std::to_integer<unsigned>(value);
        result += digits[byte >> 4u]; result += digits[byte & 15u];
    }
    return result;
}
Request draft() {
    Request request;
    request.context.package_id = "cs-test"; request.context.candidate_id = "candidate";
    request.context.package_digest[0] = std::byte{1}; request.context.client_id[0] = std::byte{2};
    request.context.session_id[0] = std::byte{3}; request.request_id = "draft-1";
    request.problem_id = "sum"; request.language = "cpp20"; request.draft_revision = 1;
    request.source = "// PRIVATE_SOURCE_CANARY_777777777777777777777777777777\nint main() {}\n";
    return request;
}
Response receipt(const Request& request, std::string_view status) {
    const auto digest = nstu::security::sha256(std::as_bytes(std::span(request.source.data(), request.source.size())));
    assert(digest);
    auto json = context_json(request.context); json.pop_back();
    json += ",\"programmingVersion\":1,\"type\":\"exam_programming_response\",\"requestId\":\"" + request.request_id +
        "\",\"operation\":\"" + std::string(operation_name(request.operation)) + "\",\"status\":\"" + std::string(status) +
        "\",\"problemId\":\"" + request.problem_id + "\",\"problemRevision\":1,\"language\":\"" + request.language +
        "\",\"draftRevision\":" + std::to_string(request.draft_revision) + ",\"sourceDigest\":\"" + hex(*digest) +
        "\",\"serverTime\":1000";
    if (request.operation == Operation::submit) json += ",\"receiptId\":\"receipt-1\"";
    json += '}';
    const auto parsed = parse_response_json(json, &request); assert(parsed); return *parsed;
}
}
int main() {
    std::array<std::byte, 16> nonce{}; assert(nstu::security::generate_random(nonce));
    const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto root = parent / ("nstu-cs-outbox-test-" + hex(nonce));
    std::filesystem::create_directory(root);
    std::ofstream(root / "test-owner") << hex(nonce);
    const auto path = root / "programming-outbox";
    const auto insecure_parent = root / "insecure";
    std::filesystem::create_directory(insecure_parent);
    const auto insecure_path = insecure_parent / "programming-outbox";
    PSECURITY_DESCRIPTOR permissive = nullptr;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &permissive, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), permissive, FALSE};
    assert(CreateDirectoryW(insecure_path.c_str(), &attributes));
    LocalFree(permissive);
    {
        nstu::client::ProgrammingOutbox outbox;
        assert(!outbox.open(insecure_path));
        assert(std::filesystem::is_directory(insecure_path));
    }
    auto request = draft();
    {
        nstu::client::ProgrammingOutbox outbox;
        assert(outbox.open(path)); assert(outbox.enqueue(request)); assert(outbox.enqueue(request));
        nstu::client::ProgrammingOutbox competing; assert(!competing.open(path));
        auto changed = request; changed.source += "changed"; assert(!outbox.enqueue(changed));
        assert(outbox.pending_for_client(request.context.client_id).size() == 1);
        auto newer = request; newer.request_id = "draft-2"; newer.draft_revision = 2; newer.source += "// newer\n";
        assert(outbox.enqueue(newer));
        assert(outbox.pending_for_client(request.context.client_id).size() == 1);
        assert(outbox.acknowledge(receipt(request, "saved")));
        assert(outbox.pending_for_client(request.context.client_id).front().request_id == "draft-2");
        request = newer;
    }
    {
        nstu::client::ProgrammingOutbox outbox; assert(outbox.open(path));
        const auto pending = outbox.pending_for_client(request.context.client_id);
        assert(pending.size() == 1 && pending.front().source == request.source);
        auto unavailable = parse_response_json(unavailable_response_json(request, "offline"), &request); assert(unavailable);
        assert(!outbox.acknowledge(*unavailable));
        assert(outbox.pending_for_client(request.context.client_id).size() == 1);
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.path().extension() != ".csb") continue;
            std::ifstream input(entry.path(), std::ios::binary);
            const std::string blob{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            assert(blob.find("PRIVATE_SOURCE_CANARY") == std::string::npos);
        }
        assert(outbox.acknowledge(receipt(request, "saved")));
        assert(outbox.pending_for_client(request.context.client_id).empty());
        request.operation = Operation::submit; request.request_id = "submit-1";
        assert(outbox.enqueue(request)); assert(outbox.acknowledge(receipt(request, "received")));
        assert(outbox.pending_for_client(request.context.client_id).empty());
    }
    {
        nstu::client::ProgrammingOutbox outbox; assert(outbox.open(path));
        for (unsigned index = 0; index < 17; ++index) {
            auto retained = request; retained.request_id = "retained-" + std::to_string(index);
            assert(outbox.enqueue(retained));
        }
        const auto pending = outbox.pending_for_client(request.context.client_id);
        assert(pending.size() == 17);
        for (const auto& retained : pending) assert(outbox.acknowledge(receipt(retained, "received")));
        assert(outbox.pending_for_client(request.context.client_id).empty());
    }
    assert(std::filesystem::canonical(root).parent_path() == parent);
    assert(root.filename().string().starts_with("nstu-cs-outbox-test-"));
    std::ifstream owner(root / "test-owner"); std::string token; owner >> token; owner.close(); assert(token == hex(nonce));
    std::filesystem::remove_all(root);
}
