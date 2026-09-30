#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct SchemaField { std::string name; uint32_t offset; std::string type; };

std::vector<SchemaField> SchemaDumpClass(void* scope, const char* className);
bool SchemaDoDump();
