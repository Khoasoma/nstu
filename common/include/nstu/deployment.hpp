#pragma once

#include <filesystem>
#include <string>

namespace nstu::deployment {

[[nodiscard]] std::filesystem::path data_root(
    std::string* error = nullptr);
[[nodiscard]] bool ensure_data_root(const std::filesystem::path& path,
                                    std::string* error = nullptr);

} // namespace nstu::deployment
