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
void validateManifestSchemaV1_1(const json::Value& schema);
void validateDocumentSchemaV1_1(const json::Value& schema);
void validateManifestSchemaV1_2(const json::Value& schema);
void validateDocumentSchemaV1_2(const json::Value& schema);
void validateManifestSchemaV1_3(const json::Value& schema);
void validateDocumentSchemaV1_3(const json::Value& schema);
void validateManifestSchemaV1_4(const json::Value& schema);
void validateDocumentSchemaV1_4(const json::Value& schema);
void validateDocumentSchemaV1_5(const json::Value& schema);
void validateDocumentSchemaV1_6(const json::Value& schema);
void validateManifestSchemaV1_6(const json::Value& schema);
void validateManifestSchemaV1_5(const json::Value& schema);
void validateDocumentSchemaV1_7(const json::Value& schema);
void validateManifestSchemaV1_7(const json::Value& schema);
void validateDocumentSchemaV1_8(const json::Value& schema);
void validateManifestSchemaV1_8(const json::Value& schema);
void validateDocumentSchemaV1_17(const json::Value& schema);
void validateManifestSchemaV1_17(const json::Value& schema);
void validateDocumentSchemaV1_18(const json::Value& schema);
void validateManifestSchemaV1_18(const json::Value& schema);
void validateDocumentSchemaV1_19(const json::Value& schema);
void validateManifestSchemaV1_19(const json::Value& schema);
void validateDocumentSchemaV1_20(const json::Value& schema);
void validateManifestSchemaV1_20(const json::Value& schema);
void validateDocumentSchemaV1_16(const json::Value& schema);
void validateManifestSchemaV1_16(const json::Value& schema);
void validateDocumentSchemaV1_15(const json::Value& schema);
void validateManifestSchemaV1_15(const json::Value& schema);
void validateDocumentSchemaV1_14(const json::Value& schema);
void validateManifestSchemaV1_14(const json::Value& schema);
void validateDocumentSchemaV1_13(const json::Value& schema);
void validateManifestSchemaV1_13(const json::Value& schema);
void validateDocumentSchemaV1_12(const json::Value& schema);
void validateManifestSchemaV1_12(const json::Value& schema);
void validateDocumentSchemaV1_11(const json::Value& schema);
void validateManifestSchemaV1_11(const json::Value& schema);
void validateDocumentSchemaV1_9(const json::Value& schema);
void validateManifestSchemaV1_9(const json::Value& schema);
void checkProjectSchemas(const std::filesystem::path& repositoryRoot);

} // namespace bloom::quality
