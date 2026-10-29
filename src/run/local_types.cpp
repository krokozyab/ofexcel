#include "ofexcel/local_types.hpp"

#include <cctype>

namespace ofexcel {

namespace {

ColumnType OfKind(ColumnKind kind) {
	ColumnType type;
	type.kind = kind;
	return type;
}

bool EndsWithId(const std::string &name) {
	if (name.size() < 3) {
		return false;
	}
	const auto tail = name.substr(name.size() - 3);
	return tail[0] == '_' && std::tolower(static_cast<unsigned char>(tail[1])) == 'i' &&
	       std::tolower(static_cast<unsigned char>(tail[2])) == 'd';
}

} // namespace

LocalColumnMapping MapLocalType(const duckdb::LogicalType &type, const std::string &name, const LocalColumnStats &stats) {
	using duckdb::LogicalTypeId;
	LocalColumnMapping mapping;
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		mapping.type = OfKind(ColumnKind::STRING);
		break;
	case LogicalTypeId::BOOLEAN:
		mapping.type = OfKind(ColumnKind::BOOLEAN);
		break;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		mapping.type = OfKind(EndsWithId(name) || stats.has_value_from_1e15 ? ColumnKind::ID : ColumnKind::INTEGER);
		break;
	case LogicalTypeId::DECIMAL:
		mapping.type.kind = ColumnKind::DECIMAL;
		mapping.type.precision = duckdb::DecimalType::GetWidth(type);
		mapping.type.scale = duckdb::DecimalType::GetScale(type);
		break;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		mapping.type = OfKind(ColumnKind::NUMBER);
		break;
	case LogicalTypeId::DATE:
		mapping.type = OfKind(ColumnKind::DATE);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		mapping.type = OfKind(ColumnKind::TIMESTAMP);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		mapping.type = OfKind(ColumnKind::TIMESTAMPTZ);
		break;
	default:
		mapping.type = OfKind(ColumnKind::STRING);
		mapping.cast_to_varchar = true;
		mapping.warning = "колонка " + name + ": тип " + type.ToString() + " передаётся строкой (CAST AS VARCHAR)";
		break;
	}
	return mapping;
}

} // namespace ofexcel
