#include "nstu/programming.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <type_traits>
#include <utility>

namespace nstu::exam::programming {
namespace {

constexpr std::uint32_t kRequestMagic = 0x31305343u; // "CS01"
constexpr std::uint16_t kRequestVersion = 1;
constexpr std::size_t kFixedHeaderBytes = 40;

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || sizeof(T) > input.size() - offset) return false;
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

bool ascii_alphanumeric(unsigned char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9');
}

bool valid_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumIdBytes) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!ascii_alphanumeric(first) && first != '_') return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return ascii_alphanumeric(byte) || byte == '_' || byte == '.' ||
               byte == '-';
    });
}

bool valid_source(std::string_view value, std::size_t maximum = kMaximumSourceBytes) noexcept {
    if (value.size() > maximum) return false;
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t width = 0;
        std::uint32_t codepoint = 0;
        if (lead <= 0x7fu) {
            if ((lead < 0x20u && lead != '\t' && lead != '\n' && lead != '\r') ||
                lead == 0x7fu) return false;
            width = 1;
            codepoint = lead;
        } else if (lead >= 0xc2u && lead <= 0xdfu) {
            width = 2;
            codepoint = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            width = 3;
            codepoint = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            width = 4;
            codepoint = lead & 0x07u;
        } else {
            return false;
        }
        if (width > value.size() - index) return false;
        for (std::size_t continuation = 1; continuation < width; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (byte & 0x3fu);
        }
        if ((width == 3 && codepoint < 0x800u) ||
            (width == 4 && codepoint < 0x10000u) || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return false;
        index += width;
    }
    return true;
}

bool valid_inventory(const std::vector<std::string>& ids) noexcept {
    if (ids.size() > kMaximumInventoryEntries) return false;
    for (auto entry = ids.begin(); entry != ids.end(); ++entry) {
        if (!valid_id(*entry) || std::find(ids.begin(), entry, *entry) != entry) {
            return false;
        }
    }
    return true;
}

void append_text(std::vector<std::byte>& output, std::string_view value) {
    if (value.empty()) return;
    output.insert(output.end(), reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data()) + value.size());
}

bool read_text(std::span<const std::byte> input, std::size_t& offset,
               std::size_t length, std::size_t maximum, std::string& output) {
    if (length > maximum || offset > input.size() || length > input.size() - offset) {
        return false;
    }
    output.assign(reinterpret_cast<const char*>(input.data() + offset), length);
    offset += length;
    return true;
}

void append_inventory(std::vector<std::byte>& output,
                      const std::vector<std::string>& ids) {
    for (const auto& id : ids) {
        append_le(output, static_cast<std::uint16_t>(id.size()));
        append_text(output, id);
    }
}

bool read_inventory(std::span<const std::byte> input, std::size_t& offset,
                    std::uint16_t count, std::vector<std::string>& ids) {
    if (count > kMaximumInventoryEntries) return false;
    ids.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint16_t length = 0;
        std::string id;
        if (!read_le(input, offset, length) || length == 0 ||
            !read_text(input, offset, length, kMaximumIdBytes, id)) return false;
        ids.push_back(std::move(id));
    }
    return true;
}

} // namespace

std::string_view operation_name(Operation operation) noexcept {
    switch (operation) {
    case Operation::draft: return "draft";
    case Operation::submit: return "submit";
    case Operation::run: return "run";
    case Operation::cancel: return "cancel";
    case Operation::recover: return "recover";
    case Operation::finalize: return "finalize";
    case Operation::ready: return "ready";
    }
    return {};
}

bool validate_request(const Request& request) {
    if ((request.workstation_policy_ready && request.operation != Operation::ready) ||
        operation_name(request.operation).empty() || !valid_id(request.request_id) ||
        !valid_source(request.source) || encode_state_request(request.context).empty()) {
        return false;
    }
    if (request.operation == Operation::finalize || request.operation == Operation::ready) {
        return request.problem_id.empty() && request.problem_revision == 1 &&
               request.language.empty() && request.draft_revision == 0 &&
               request.source.empty() && request.sample_id.empty() &&
               request.target_run_id.empty() &&
               (request.operation == Operation::finalize
                    ? valid_inventory(request.receipt_ids) &&
                          valid_inventory(request.no_source_problem_ids)
                    : request.receipt_ids.empty() && request.no_source_problem_ids.empty());
    }
    if (!valid_id(request.problem_id) || request.problem_revision == 0 ||
        (request.language != "cpp20" && request.language != "python3") ||
        !request.receipt_ids.empty() || !request.no_source_problem_ids.empty()) return false;
    switch (request.operation) {
    case Operation::draft:
    case Operation::submit:
    case Operation::run:
        return request.draft_revision != 0 && request.draft_revision <= kMaximumDraftRevision &&
               request.target_run_id.empty() &&
               (request.operation == Operation::draft || !request.source.empty()) &&
               (request.operation == Operation::run ? valid_id(request.sample_id)
                                                    : request.sample_id.empty());
    case Operation::cancel:
        return request.draft_revision == 0 && request.source.empty() &&
               request.sample_id.empty() && valid_id(request.target_run_id) &&
               request.target_run_id != request.request_id;
    case Operation::recover:
        return request.draft_revision == 0 && request.source.empty() &&
               request.sample_id.empty() && request.target_run_id.empty();
    case Operation::finalize:
    case Operation::ready:
        return false;
    }
    return false;
}

std::vector<std::byte> encode_request(const Request& request) {
    if (!validate_request(request)) return {};
    const auto context = encode_state_request(request.context);
    auto total = kFixedHeaderBytes + context.size() + request.request_id.size() +
                 request.problem_id.size() + request.language.size() +
                 request.sample_id.size() + request.target_run_id.size() + request.source.size();
    for (const auto& id : request.receipt_ids) total += sizeof(std::uint16_t) + id.size();
    for (const auto& id : request.no_source_problem_ids) total += sizeof(std::uint16_t) + id.size();
    if (total > kMaximumExamPayloadBytes) return {};

    std::vector<std::byte> output;
    output.reserve(total);
    append_le(output, kRequestMagic);
    append_le(output, kRequestVersion);
    append_le(output, static_cast<std::uint8_t>(request.operation));
    append_le(output, static_cast<std::uint8_t>(request.workstation_policy_ready));
    append_le(output, static_cast<std::uint16_t>(context.size()));
    append_le(output, static_cast<std::uint16_t>(request.request_id.size()));
    append_le(output, static_cast<std::uint16_t>(request.problem_id.size()));
    append_le(output, static_cast<std::uint16_t>(request.language.size()));
    append_le(output, static_cast<std::uint16_t>(request.sample_id.size()));
    append_le(output, static_cast<std::uint32_t>(request.source.size()));
    append_le(output, request.problem_revision);
    append_le(output, request.draft_revision);
    append_le(output, static_cast<std::uint16_t>(request.receipt_ids.size()));
    append_le(output, static_cast<std::uint16_t>(request.no_source_problem_ids.size()));
    append_le(output, static_cast<std::uint16_t>(request.target_run_id.size()));
    output.insert(output.end(), context.begin(), context.end());
    append_text(output, request.request_id);
    append_text(output, request.problem_id);
    append_text(output, request.language);
    append_text(output, request.sample_id);
    append_text(output, request.target_run_id);
    append_text(output, request.source);
    append_inventory(output, request.receipt_ids);
    append_inventory(output, request.no_source_problem_ids);
    return output;
}

bool validate_service_request(const Request& request, const StateRequest& issued_context) {
    return !request.workstation_policy_ready && validate_request(request) &&
        request.context.package_id == issued_context.package_id &&
        request.context.package_digest == issued_context.package_digest &&
        request.context.client_id == issued_context.client_id &&
        request.context.session_id == issued_context.session_id &&
        request.context.candidate_id == issued_context.candidate_id;
}

std::optional<Request> decode_request(std::span<const std::byte> payload) {
    if (payload.size() < kFixedHeaderBytes || payload.size() > kMaximumExamPayloadBytes) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t operation = 0;
    std::uint8_t reserved = 0;
    std::uint16_t context_bytes = 0;
    std::uint16_t request_bytes = 0;
    std::uint16_t problem_bytes = 0;
    std::uint16_t language_bytes = 0;
    std::uint16_t sample_bytes = 0;
    std::uint32_t source_bytes = 0;
    std::uint16_t receipt_count = 0;
    std::uint16_t no_source_count = 0;
    std::uint16_t target_bytes = 0;
    Request request;
    if (!read_le(payload, offset, magic) || magic != kRequestMagic ||
        !read_le(payload, offset, version) || version != kRequestVersion ||
        !read_le(payload, offset, operation) ||
        operation_name(static_cast<Operation>(operation)).empty() ||
        !read_le(payload, offset, reserved) || reserved > 1 ||
        !read_le(payload, offset, context_bytes) ||
        !read_le(payload, offset, request_bytes) ||
        !read_le(payload, offset, problem_bytes) ||
        !read_le(payload, offset, language_bytes) ||
        !read_le(payload, offset, sample_bytes) ||
        !read_le(payload, offset, source_bytes) ||
        !read_le(payload, offset, request.problem_revision) ||
        !read_le(payload, offset, request.draft_revision) ||
        !read_le(payload, offset, receipt_count) ||
        !read_le(payload, offset, no_source_count) ||
        !read_le(payload, offset, target_bytes) || context_bytes > payload.size() - offset) {
        return std::nullopt;
    }
    auto context = decode_state_request(payload.subspan(offset, context_bytes));
    if (!context) return std::nullopt;
    request.context = std::move(*context);
    request.operation = static_cast<Operation>(operation);
    request.workstation_policy_ready = reserved != 0;
    offset += context_bytes;
    if (!read_text(payload, offset, request_bytes, kMaximumIdBytes, request.request_id) ||
        !read_text(payload, offset, problem_bytes, kMaximumIdBytes, request.problem_id) ||
        !read_text(payload, offset, language_bytes, 7, request.language) ||
        !read_text(payload, offset, sample_bytes, kMaximumIdBytes, request.sample_id) ||
        !read_text(payload, offset, target_bytes, kMaximumIdBytes, request.target_run_id) ||
        !read_text(payload, offset, source_bytes, kMaximumSourceBytes, request.source) ||
        !read_inventory(payload, offset, receipt_count, request.receipt_ids) ||
        !read_inventory(payload, offset, no_source_count, request.no_source_problem_ids) ||
        offset != payload.size() || !validate_request(request)) return std::nullopt;
    return request;
}

namespace {

struct JsonScalar {
    enum class Kind { text, number, boolean } kind = Kind::text;
    std::string value;
};
using JsonObject = std::map<std::string, JsonScalar>;

void append_codepoint(std::string& text, std::uint32_t point) {
    if (point <= 0x7fu) text.push_back(static_cast<char>(point));
    else if (point <= 0x7ffu) {
        text.push_back(static_cast<char>(0xc0u | (point >> 6u)));
        text.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
    } else if (point <= 0xffffu) {
        text.push_back(static_cast<char>(0xe0u | (point >> 12u)));
        text.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3fu)));
        text.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
    } else {
        text.push_back(static_cast<char>(0xf0u | (point >> 18u)));
        text.push_back(static_cast<char>(0x80u | ((point >> 12u) & 0x3fu)));
        text.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3fu)));
        text.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
    }
}

class FlatJsonReader {
public:
    explicit FlatJsonReader(std::string_view text) : text_(text) {}
    bool parse(JsonObject& result) {
        if (text_.empty() || text_.size() > kMaximumExamPayloadBytes || !take('{')) return false;
        if (take('}')) return finished();
        do {
            std::string key;
            if (result.size() >= 40 || !string(key) || key.size() > 64 || !take(':')) return false;
            JsonScalar scalar;
            whitespace();
            if (position_ < text_.size() && text_[position_] == '"') {
                if (!string(scalar.value)) return false;
            } else if (text_.substr(position_, 4) == "true") {
                scalar.kind = JsonScalar::Kind::boolean;
                scalar.value = "true";
                position_ += 4;
            } else if (text_.substr(position_, 5) == "false") {
                scalar.kind = JsonScalar::Kind::boolean;
                scalar.value = "false";
                position_ += 5;
            } else {
                scalar.kind = JsonScalar::Kind::number;
                const auto begin = position_;
                while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
                if (begin == position_ || (text_[begin] == '0' && position_ != begin + 1)) return false;
                scalar.value = text_.substr(begin, position_ - begin);
            }
            if (!result.emplace(std::move(key), std::move(scalar)).second) return false;
            if (take('}')) return finished();
        } while (take(','));
        return false;
    }
private:
    void whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
               text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
    }
    bool take(char character) {
        whitespace();
        if (position_ == text_.size() || text_[position_] != character) return false;
        ++position_;
        return true;
    }
    bool finished() { whitespace(); return position_ == text_.size(); }
    bool hex4(std::uint32_t& point) {
        if (text_.size() - position_ < 4) return false;
        point = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const char digit = text_[position_++];
            point <<= 4u;
            if (digit >= '0' && digit <= '9') point |= static_cast<unsigned>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') point |= static_cast<unsigned>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') point |= static_cast<unsigned>(digit - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& result) {
        if (!take('"')) return false;
        while (position_ < text_.size()) {
            const auto character = static_cast<unsigned char>(text_[position_++]);
            if (character == '"') return valid_source(result, 64u * 1024u);
            if (character < 0x20u) return false;
            if (character != '\\') { result.push_back(static_cast<char>(character)); continue; }
            if (position_ == text_.size()) return false;
            switch (text_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t point = 0;
                if (!hex4(point)) return false;
                if (point >= 0xd800u && point <= 0xdbffu) {
                    if (text_.substr(position_, 2) != "\\u") return false;
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!hex4(low) || low < 0xdc00u || low > 0xdfffu) return false;
                    point = 0x10000u + ((point - 0xd800u) << 10u) + low - 0xdc00u;
                } else if (point >= 0xdc00u && point <= 0xdfffu) return false;
                append_codepoint(result, point);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    std::string_view text_;
    std::size_t position_ = 0;
};

const JsonScalar* scalar(const JsonObject& object, std::string_view key) {
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}
bool text_field(const JsonObject& object, std::string_view key, std::string& result) {
    const auto* value = scalar(object, key);
    if (!value || value->kind != JsonScalar::Kind::text) return false;
    result = value->value;
    return true;
}
bool number_field(const JsonObject& object, std::string_view key, std::uint64_t& result) {
    const auto* value = scalar(object, key);
    if (!value || value->kind != JsonScalar::Kind::number) return false;
    const auto conversion = std::from_chars(value->value.data(), value->value.data() + value->value.size(), result);
    return conversion.ec == std::errc{} && conversion.ptr == value->value.data() + value->value.size() &&
           result <= kMaximumDraftRevision;
}
bool boolean_field(const JsonObject& object, std::string_view key, bool& result) {
    const auto* value = scalar(object, key);
    if (!value || value->kind != JsonScalar::Kind::boolean) return false;
    result = value->value == "true";
    return true;
}
bool fields_allowed(const JsonObject& object, std::string_view keys) {
    return std::all_of(object.begin(), object.end(), [&](const auto& entry) {
        return keys.find("|" + entry.first + "|") != std::string_view::npos;
    });
}
template <typename Array>
std::string hex_string(const Array& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : value) {
        const auto digit = std::to_integer<unsigned>(byte);
        result.push_back(digits[digit >> 4u]);
        result.push_back(digits[digit & 15u]);
    }
    return result;
}
template <typename Array>
bool parse_hex(std::string_view value, Array& result) {
    if (value.size() != result.size() * 2) return false;
    for (std::size_t index = 0; index < result.size(); ++index) {
        unsigned byte = 0;
        for (std::size_t part = 0; part < 2; ++part) {
            const char digit = value[index * 2 + part];
            if (digit >= '0' && digit <= '9') byte = (byte << 4u) | static_cast<unsigned>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') byte = (byte << 4u) | static_cast<unsigned>(digit - 'a' + 10);
            else return false;
        }
        result[index] = static_cast<std::byte>(byte);
    }
    return true;
}
bool parse_context(const JsonObject& object, StateRequest& result) {
    std::string digest, client, session;
    return text_field(object, "packageId", result.package_id) &&
           text_field(object, "candidateId", result.candidate_id) &&
           text_field(object, "packageDigestHex", digest) && parse_hex(digest, result.package_digest) &&
           text_field(object, "clientIdHex", client) && parse_hex(client, result.client_id) &&
           text_field(object, "sessionIdHex", session) && parse_hex(session, result.session_id) &&
           !encode_state_request(result).empty();
}
bool same_context(const StateRequest& left, const StateRequest& right) {
    return left.package_id == right.package_id && left.package_digest == right.package_digest &&
           left.client_id == right.client_id && left.session_id == right.session_id &&
           left.candidate_id == right.candidate_id;
}
std::string quote(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char byte : value) {
        if (byte == '"') result += "\\\"";
        else if (byte == '\\') result += "\\\\";
        else if (byte == '\n') result += "\\n";
        else if (byte == '\r') result += "\\r";
        else if (byte == '\t') result += "\\t";
        else if (byte < 32u) {
            constexpr char digits[] = "0123456789abcdef";
            result += "\\u00";
            result.push_back(digits[byte >> 4u]);
            result.push_back(digits[byte & 15u]);
        } else result.push_back(static_cast<char>(byte));
    }
    return result + '"';
}
std::string array_json(const std::vector<std::string>& ids) {
    std::string result = "[";
    for (const auto& id : ids) {
        if (result.size() != 1) result += ',';
        result += quote(id);
    }
    return result + ']';
}
std::string serialize(const JsonObject& object) {
    std::string result = "{";
    for (const auto& [key, value] : object) {
        if (result.size() != 1) result += ',';
        result += quote(key) + ':' + (value.kind == JsonScalar::Kind::text ? quote(value.value) : value.value);
    }
    return result + '}';
}
std::string source_digest(std::string_view source) {
    const auto bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(source.data()), source.size()};
    const auto digest = security::sha256(bytes);
    return digest ? hex_string(*digest) : std::string{};
}
bool decode_base64(std::string_view value, std::string& result) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (value.size() > ((kMaximumSourceBytes + 2) / 3) * 4 || value.size() % 4 != 0) return false;
    for (std::size_t offset = 0; offset < value.size(); offset += 4) {
        std::uint32_t bits = 0;
        unsigned padding = 0;
        for (unsigned part = 0; part < 4; ++part) {
            const auto character = value[offset + part];
            if (character == '=') {
                if (part < 2 || offset + 4 != value.size()) return false;
                ++padding;
                bits <<= 6u;
            } else {
                const auto index = alphabet.find(character);
                if (padding != 0 || index == std::string_view::npos) return false;
                bits = (bits << 6u) | static_cast<std::uint32_t>(index);
            }
        }
        if (padding > 2 || (padding == 2 && (bits & 0xffffu) != 0) ||
            (padding == 1 && (bits & 0xffu) != 0)) return false;
        result.push_back(static_cast<char>((bits >> 16u) & 0xffu));
        if (padding < 2) result.push_back(static_cast<char>((bits >> 8u) & 0xffu));
        if (padding == 0) result.push_back(static_cast<char>(bits & 0xffu));
    }
    return valid_source(result);
}

constexpr std::string_view kContextFields =
    "|packageId||packageDigestHex||clientIdHex||sessionIdHex||candidateId||programmingVersion|";

} // namespace

std::string context_json(const StateRequest& context) {
    if (encode_state_request(context).empty()) return {};
    return "{\"packageId\":" + quote(context.package_id) + ",\"packageDigestHex\":" + quote(hex_string(context.package_digest)) +
           ",\"clientIdHex\":" + quote(hex_string(context.client_id)) + ",\"sessionIdHex\":" + quote(hex_string(context.session_id)) +
           ",\"candidateId\":" + quote(context.candidate_id) + '}';
}

std::string request_json(const Request& request) {
    if (!validate_request(request)) return {};
    auto result = context_json(request.context);
    result.pop_back();
    result += ",\"programmingVersion\":1,\"requestId\":" + quote(request.request_id) +
              ",\"operation\":" + quote(operation_name(request.operation));
    if (request.operation != Operation::ready && request.operation != Operation::finalize) {
        result += ",\"problemId\":" + quote(request.problem_id) + ",\"problemRevision\":" +
                  std::to_string(request.problem_revision) + ",\"language\":" + quote(request.language);
    }
    if (request.operation == Operation::draft || request.operation == Operation::submit || request.operation == Operation::run) {
        result += ",\"draftRevision\":" + std::to_string(request.draft_revision) + ",\"source\":" + quote(request.source);
    }
    if (request.operation == Operation::run) result += ",\"sampleId\":" + quote(request.sample_id);
    if (request.operation == Operation::cancel) result += ",\"targetRunId\":" + quote(request.target_run_id);
    if (request.operation == Operation::finalize) result += ",\"receiptIds\":" + array_json(request.receipt_ids) +
                                                          ",\"noSourceProblemIds\":" + array_json(request.no_source_problem_ids);
    return result + '}';
}

std::string unavailable_response_json(const Request& request, std::string_view reason) {
    if (!validate_request(request) || !valid_id(reason)) return {};
    auto result = context_json(request.context);
    result.pop_back();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result += ",\"programmingVersion\":1,\"type\":\"exam_programming_response\",\"requestId\":" + quote(request.request_id) +
              ",\"operation\":" + quote(operation_name(request.operation)) + ",\"status\":\"unavailable\",\"reason\":" +
              quote(reason) + ",\"serverTime\":" + std::to_string(now);
    if (request.operation != Operation::ready && request.operation != Operation::finalize) {
        result += ",\"problemId\":" + quote(request.problem_id) + ",\"problemRevision\":" + std::to_string(request.problem_revision) +
                  ",\"language\":" + quote(request.language);
    }
    if (request.operation == Operation::draft || request.operation == Operation::submit || request.operation == Operation::run ||
        request.operation == Operation::recover) {
        result += ",\"draftRevision\":" + std::to_string(request.draft_revision);
    }
    if (request.operation == Operation::ready) result += ",\"runnerReady\":false";
    return result + '}';
}

std::optional<Response> parse_response_json(std::string_view json, const Request* expected) {
    JsonObject object;
    if (!FlatJsonReader(json).parse(object)) return std::nullopt;
    Response response;
    std::string type, operation, status;
    std::uint64_t version = 0;
    if (!parse_context(object, response.context) || !number_field(object, "programmingVersion", version) || version != 1 ||
        !text_field(object, "type", type) || type != "exam_programming_response" ||
        !text_field(object, "requestId", response.request_id) || !valid_id(response.request_id) ||
        !text_field(object, "operation", operation) || !text_field(object, "status", status) ||
        !number_field(object, "serverTime", response.server_time) || response.server_time == 0) return std::nullopt;
    bool known = false;
    for (const auto op : {Operation::draft, Operation::submit, Operation::run, Operation::cancel,
                          Operation::recover, Operation::finalize, Operation::ready}) {
        if (operation_name(op) == operation) { response.operation = op; known = true; break; }
    }
    if (!known || (expected && (!same_context(expected->context, response.context) ||
        expected->operation != response.operation || expected->request_id != response.request_id))) return std::nullopt;
    auto allowed = std::string(kContextFields) + "|type||requestId||operation||status||serverTime||reason|";
    if (const auto* reason = scalar(object, "reason"); reason &&
        (reason->kind != JsonScalar::Kind::text || reason->value.size() > 512)) return std::nullopt;
    const bool failed = status == "unavailable" || status == "rejected" || status == "conflict" || status == "closed" || status == "late";
    bool status_valid = failed;
    switch (response.operation) {
    case Operation::draft: status_valid = status_valid || status == "saved"; break;
    case Operation::submit: status_valid = status_valid || status == "received"; break;
    case Operation::run: status_valid = status_valid || status == "completed" || status == "compilation_error" ||
        status == "runtime_error" || status == "timeout" || status == "cancelled"; break;
    case Operation::cancel: status_valid = status_valid || status == "cancelled"; break;
    case Operation::recover: status_valid = status_valid || status == "recovered"; break;
    case Operation::finalize: status_valid = status_valid || status == "finalized"; break;
    case Operation::ready: status_valid = status_valid || status == "ready"; break;
    }
    if (!status_valid) return std::nullopt;
    response.status = status;
    const bool durable_late = response.operation == Operation::submit && status == "late";
    if (response.operation != Operation::ready && response.operation != Operation::finalize) {
        allowed += "|problemId||problemRevision||language|";
        std::string problem, language;
        std::uint64_t revision = 0;
        if (!text_field(object, "problemId", problem) || !valid_id(problem) ||
            !text_field(object, "language", language) || (language != "cpp20" && language != "python3") ||
            !number_field(object, "problemRevision", revision) || revision == 0 || revision > UINT32_MAX ||
            (expected && (expected->problem_id != problem || expected->problem_revision != revision || expected->language != language))) return std::nullopt;
    }
    if (response.operation == Operation::draft || response.operation == Operation::submit || response.operation == Operation::run ||
        response.operation == Operation::recover) {
        allowed += "|draftRevision||sourceDigest|";
        std::uint64_t revision = 0;
        if (!number_field(object, "draftRevision", revision) ||
            (expected && response.operation != Operation::recover && revision != expected->draft_revision)) return std::nullopt;
        if (!failed || durable_late || scalar(object, "sourceDigest")) {
            std::string digest;
            security::Sha256Digest parsed{};
            if (!text_field(object, "sourceDigest", digest) || !parse_hex(digest, parsed) ||
                (expected && response.operation != Operation::recover && digest != source_digest(expected->source))) return std::nullopt;
        }
    }
    if (response.operation == Operation::submit || response.operation == Operation::finalize) {
        allowed += "|receiptId|";
        if (!failed || durable_late || scalar(object, "receiptId")) {
            std::string receipt;
            if (!text_field(object, "receiptId", receipt) || !valid_id(receipt)) return std::nullopt;
        }
    }
    if (response.operation == Operation::run) {
        allowed += "|sampleId||sampleMatched||stdout||stderr||diagnostics||truncated|";
        std::size_t size = 0;
        for (const auto key : {"stdout", "stderr", "diagnostics"}) {
            std::string stream;
            if ((!failed || scalar(object, key)) && (!text_field(object, key, stream) || !valid_source(stream, 24u * 1024u))) return std::nullopt;
            size += stream.size();
        }
        bool flag = false;
        if (size > 24u * 1024u || ((!failed || scalar(object, "truncated")) && !boolean_field(object, "truncated", flag))) return std::nullopt;
        if (scalar(object, "sampleMatched") && !boolean_field(object, "sampleMatched", flag)) return std::nullopt;
        if (scalar(object, "sampleId")) {
            std::string sample;
            if (!text_field(object, "sampleId", sample) || !valid_id(sample) || (expected && sample != expected->sample_id)) return std::nullopt;
        }
    }
    if (response.operation == Operation::ready) {
        allowed += "|runnerReady|";
        if (!boolean_field(object, "runnerReady", response.runner_ready) || (status == "ready" && !response.runner_ready)) return std::nullopt;
    }
    if (response.operation == Operation::recover) {
        allowed += "|sourceBase64||latestReceiptId||receiptDigest||finalized||serverStartUnixMilliseconds||deadlineUnixMilliseconds|";
        if (!failed) {
            std::string encoded, digest, source, receipt, receipt_digest;
            bool finalized = false;
            if (!text_field(object, "sourceBase64", encoded) || !decode_base64(encoded, source) ||
                !text_field(object, "sourceDigest", digest) || digest != source_digest(source) ||
                !text_field(object, "latestReceiptId", receipt) || (!receipt.empty() && !valid_id(receipt)) ||
                !text_field(object, "receiptDigest", receipt_digest) || receipt.empty() != receipt_digest.empty() ||
                !boolean_field(object, "finalized", finalized)) return std::nullopt;
            security::Sha256Digest parsed{};
            if (!receipt_digest.empty() && !parse_hex(receipt_digest, parsed)) return std::nullopt;
        }
        for (const auto key : {"serverStartUnixMilliseconds", "deadlineUnixMilliseconds"}) {
            std::uint64_t value = 0;
            if (scalar(object, key) && !number_field(object, key, value)) return std::nullopt;
        }
    }
    if (!fields_allowed(object, allowed)) return std::nullopt;
    response.json = serialize(object);
    return response;
}

bool validate_prepared_json(std::string_view json, const StateRequest& context) {
    JsonObject object;
    StateRequest returned;
    std::string status;
    std::uint64_t version = 0;
    return FlatJsonReader(json).parse(object) && fields_allowed(object, std::string(kContextFields) + "|status|") &&
           parse_context(object, returned) && same_context(returned, context) &&
           number_field(object, "programmingVersion", version) && version == 1 &&
           text_field(object, "status", status) && status == "prepared";
}

bool validate_known_context_json(std::string_view json, const StateRequest& context) {
    JsonObject object;
    StateRequest returned;
    std::string status;
    std::uint64_t version = 0, server_time = 0;
    return FlatJsonReader(json).parse(object) &&
           fields_allowed(object, std::string(kContextFields) + "|status||serverTime|") &&
           parse_context(object, returned) && same_context(returned, context) &&
           number_field(object, "programmingVersion", version) && version == 1 &&
           text_field(object, "status", status) && status == "known" &&
           number_field(object, "serverTime", server_time) && server_time != 0;
}

std::optional<LifecycleReply> parse_lifecycle_json(std::string_view json, const StateRequest& context,
                                                std::string_view request_id, bool begin) {
    JsonObject object;
    StateRequest returned;
    std::string status, id;
    std::uint64_t version = 0;
    LifecycleReply result;
    const auto allowed = std::string(kContextFields) + "|status||requestId|" +
        (begin ? "|serverStartUnixMilliseconds||deadlineUnixMilliseconds||serverTime|" : "|serverTime|");
    if (!FlatJsonReader(json).parse(object) || !fields_allowed(object, allowed) || !parse_context(object, returned) ||
        !same_context(returned, context) || !number_field(object, "programmingVersion", version) || version != 1 ||
        !text_field(object, "requestId", id) || id != request_id || !text_field(object, "status", status) ||
        status != (begin ? "begun" : "stopped")) return std::nullopt;
    if (begin) {
        if (!number_field(object, "serverStartUnixMilliseconds", result.server_start) || result.server_start == 0 ||
            !number_field(object, "deadlineUnixMilliseconds", result.deadline) || result.deadline <= result.server_start ||
            result.deadline - result.server_start > 86400ULL * 1000ULL) return std::nullopt;
        if (scalar(object, "serverTime") && (!number_field(object, "serverTime", result.server_time) || result.server_time == 0)) return std::nullopt;
    } else if (!number_field(object, "serverTime", result.server_time) || result.server_time == 0) return std::nullopt;
    return result;
}

} // namespace nstu::exam::programming
