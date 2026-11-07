#include "../server/app/exam_digest.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void rejected_input_preserves_output() {
    const std::string valid =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::vector<std::pair<std::string, std::string>> cases{
        {"empty input", ""},
        {"63 characters", valid.substr(0, 63)},
        {"65 characters", valid + "0"},
        {"all-zero digest", std::string(64, '0')},
        {"invalid first high nibble", "g" + valid.substr(1)},
        {"invalid first low nibble", "e!" + valid.substr(2)},
        {"invalid final high nibble", valid.substr(0, 62) + "g5"},
        {"invalid final low nibble", valid.substr(0, 63) + "x"},
        {"leading whitespace", " " + valid.substr(1)},
        {"trailing whitespace", valid.substr(0, 63) + " "},
    };
    auto non_ascii = valid;
    non_ascii.back() = static_cast<char>(0xe9);
    cases.emplace_back("non-ASCII byte", std::move(non_ascii));
    auto embedded_null = valid;
    embedded_null[31] = '\0';
    cases.emplace_back("embedded NUL", std::move(embedded_null));

    nstu::security::Sha256Digest sentinel{};
    sentinel.fill(std::byte{0xa5});
    for (const auto& [name, input] : cases) {
        auto output = sentinel;
        const bool accepted = nstu::ui::parse_sha256_hex(input, output);
        expect(!accepted, name + " must be rejected");
        expect(output == sentinel, name + " must preserve every output byte");
    }
}

void valid_case_variants_decode_known_bytes() {
    // SHA-256 of the empty message; expected bytes are independently pinned.
    constexpr nstu::security::Sha256Digest expected{
        std::byte{0xe3}, std::byte{0xb0}, std::byte{0xc4}, std::byte{0x42},
        std::byte{0x98}, std::byte{0xfc}, std::byte{0x1c}, std::byte{0x14},
        std::byte{0x9a}, std::byte{0xfb}, std::byte{0xf4}, std::byte{0xc8},
        std::byte{0x99}, std::byte{0x6f}, std::byte{0xb9}, std::byte{0x24},
        std::byte{0x27}, std::byte{0xae}, std::byte{0x41}, std::byte{0xe4},
        std::byte{0x64}, std::byte{0x9b}, std::byte{0x93}, std::byte{0x4c},
        std::byte{0xa4}, std::byte{0x95}, std::byte{0x99}, std::byte{0x1b},
        std::byte{0x78}, std::byte{0x52}, std::byte{0xb8}, std::byte{0x55},
    };
    constexpr std::array<std::string_view, 3> inputs{
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855",
        "E3b0C44298fC1c149aFBf4C8996fB92427aE41e4649B934cA495991B7852b855",
    };
    for (const auto input : inputs) {
        nstu::security::Sha256Digest output{};
        expect(nstu::ui::parse_sha256_hex(input, output),
               "lower/upper/mixed-case digest must be accepted");
        expect(output == expected, "case variants must decode to known bytes");
    }
}

void individual_zero_digits_are_allowed() {
    nstu::security::Sha256Digest output{};
    output.fill(std::byte{0xa5});
    nstu::security::Sha256Digest expected{};
    expected.back() = std::byte{1};
    expect(nstu::ui::parse_sha256_hex(
               "0000000000000000000000000000000000000000000000000000000000000001",
               output),
           "only final byte nonzero must be accepted");
    expect(output == expected, "zero digits must decode to zero bytes");
}

} // namespace

int main() {
    rejected_input_preserves_output();
    valid_case_variants_decode_known_bytes();
    individual_zero_digits_are_allowed();
    if (failures != 0) {
        std::cerr << failures << " exam digest checks failed\n";
        return 1;
    }
    std::cout << "Exam digest tests passed (32 behavioral checks).\n";
}
