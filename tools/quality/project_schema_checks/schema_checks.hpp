#pragma once

#include "strict_json.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace bloom::quality {

class SchemaCheckError final : public std::runtime_error {
  public:
    explicit SchemaCheckError(const std::string& message);
};

void validateManifestSchema(const json::Value& schema);
void validateDocumentSchema(const json::Value& schema);
void checkProjectSchemas(const std::filesystem::path& repositoryRoot);

} // namespace bloom::quality
