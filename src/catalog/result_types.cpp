#include "ofexcel/result_types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace ofexcel {

namespace {

//! Excel keeps 15 significant digits; a longer integer is an identifier.
constexpr size_t MAX_INTEGER_DIGITS = 15;
constexpr size_t MAX_DECIMAL_DIGITS = 38;

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool AllDigits(const std::string &text, size_t from, size_t to) {
	if (from >= to || to > text.size()) {
		return false;
	}
	for (size_t i = from; i < to; i++) {
		if (!IsDigit(text[i])) {
			return false;
		}
	}
	return true;
}

int Number(const std::string &text, size_t from, size_t length) {
	return std::stoi(text.substr(from, length));
}

std::string Lower(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

//! A timestamp as Oracle and DuckDB print it: YYYY-MM-DD[ T]HH:MM:SS, up to
//! nine fraction digits, and for TSTZ an offset ±HH:MM, with or without a
//! space before it.
struct ParsedTimestamp {
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	std::string fraction; //!< as written, up to 9 digits
	bool has_offset = false;
	int offset_minutes = 0; //!< east of UTC
};

bool ParseTimestamp(const std::string &text, ParsedTimestamp &parsed) {
	if (text.size() < 19 || !IsDateAt(text, 0) || (text[10] != ' ' && text[10] != 'T') || text[13] != ':' ||
	    text[16] != ':' || !AllDigits(text, 11, 13) || !AllDigits(text, 14, 16) || !AllDigits(text, 17, 19)) {
		return false;
	}
	parsed.year = Number(text, 0, 4);
	parsed.month = Number(text, 5, 2);
	parsed.day = Number(text, 8, 2);
	parsed.hour = Number(text, 11, 2);
	parsed.minute = Number(text, 14, 2);
	parsed.second = Number(text, 17, 2);
	if (parsed.hour > 23 || parsed.minute > 59 || parsed.second > 59) {
		return false;
	}
	size_t at = 19;
	if (at < text.size() && text[at] == '.') {
		const size_t start = ++at;
		while (at < text.size() && IsDigit(text[at])) {
			at++;
		}
		if (at == start || at - start > 9) {
			return false;
		}
		parsed.fraction = text.substr(start, at - start);
	}
	if (at == text.size()) {
		return true;
	}
	if (text[at] == ' ') {
		at++;
	}
	if (at + 6 != text.size() || (text[at] != '+' && text[at] != '-') || text[at + 3] != ':' ||
	    !AllDigits(text, at + 1, at + 3) || !AllDigits(text, at + 4, at + 6)) {
		return false;
	}
	const int hours = Number(text, at + 1, 2);
	const int minutes = Number(text, at + 4, 2);
	if (hours > 23 || minutes > 59) {
		return false;
	}
	parsed.has_offset = true;
	parsed.offset_minutes = (text[at] == '-' ? -1 : 1) * (hours * 60 + minutes);
	return true;
}

bool IsZeroFraction(const std::string &fraction) {
	return fraction.find_first_not_of('0') == std::string::npos;
}

std::string TwoDigits(int value) {
	return std::string(1, static_cast<char>('0' + value / 10)) + static_cast<char>('0' + value % 10);
}

//! Days since 1970-01-01 (H. Hinnant's days_from_civil).
int64_t DaysFromCivil(int64_t year, int64_t month, int64_t day) {
	year -= month <= 2;
	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const int64_t year_of_era = year - era * 400;
	const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	return era * 146097 + day_of_era - 719468;
}

void CivilFromDays(int64_t days, int &year, int &month, int &day) {
	days += 719468;
	const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	const int64_t day_of_era = days - era * 146097;
	const int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	const int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	const int64_t shifted_month = (5 * day_of_year + 2) / 153;
	day = static_cast<int>(day_of_year - (153 * shifted_month + 2) / 5 + 1);
	month = static_cast<int>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
	year = static_cast<int>(year_of_era + era * 400 + (month <= 2));
}

//! The offset applied: the same instant at +00:00.
ParsedTimestamp ToUtc(ParsedTimestamp parsed) {
	const int64_t seconds = DaysFromCivil(parsed.year, parsed.month, parsed.day) * 86400 + parsed.hour * 3600 +
	                        parsed.minute * 60 + parsed.second - parsed.offset_minutes * 60;
	int64_t days = seconds / 86400;
	int64_t rest = seconds % 86400;
	if (rest < 0) {
		rest += 86400;
		days--;
	}
	CivilFromDays(days, parsed.year, parsed.month, parsed.day);
	parsed.hour = static_cast<int>(rest / 3600);
	parsed.minute = static_cast<int>(rest % 3600 / 60);
	parsed.second = static_cast<int>(rest % 60);
	parsed.offset_minutes = 0;
	return parsed;
}

//! YYYY-MM-DD HH:MM:SS[.ffffff]: the fraction cut (not rounded) to
//! microseconds, trailing zeros dropped. `separator` is ' ' or 'T'.
std::string FormatTimestamp(const ParsedTimestamp &parsed, char separator) {
	std::string year = std::to_string(parsed.year);
	year = std::string(year.size() < 4 ? 4 - year.size() : 0, '0') + year;
	auto fraction = parsed.fraction.substr(0, 6);
	while (!fraction.empty() && fraction.back() == '0') {
		fraction.pop_back();
	}
	return year + "-" + TwoDigits(parsed.month) + "-" + TwoDigits(parsed.day) + separator + TwoDigits(parsed.hour) +
	       ":" + TwoDigits(parsed.minute) + ":" + TwoDigits(parsed.second) + (fraction.empty() ? "" : "." + fraction);
}

//! What one value looks like, for every type at once.
struct Shape {
	bool integer = false;
	size_t integer_digits = 0;
	bool decimal = false;
	size_t whole_digits = 0;
	size_t scale = 0;
	bool number = false;
	bool date = false;
	bool timestamp = false;
	bool midnight = false;
	bool timestamptz = false;
	bool boolean = false;
};

//! ^-?(0|[1-9][0-9]*)?(\.[0-9]+)?$ with at least one digit; also sets the
//! digit counts.
bool IsOracleDecimal(const std::string &text, Shape &shape) {
	size_t i = text.empty() || text[0] != '-' ? 0 : 1;
	const size_t whole_start = i;
	while (i < text.size() && IsDigit(text[i])) {
		i++;
	}
	const auto whole = text.substr(whole_start, i - whole_start);
	if (whole.size() > 1 && whole[0] == '0') {
		return false;
	}
	std::string fraction;
	if (i < text.size() && text[i] == '.') {
		const size_t start = ++i;
		while (i < text.size() && IsDigit(text[i])) {
			i++;
		}
		if (i == start) {
			return false;
		}
		fraction = text.substr(start, i - start);
	}
	if (i != text.size() || whole.size() + fraction.size() == 0) {
		return false;
	}
	while (!fraction.empty() && fraction.back() == '0') {
		fraction.pop_back();
	}
	shape.whole_digits = whole == "0" ? 0 : whole.size();
	shape.scale = fraction.size();
	return true;
}

Shape ShapeOf(const std::string &text) {
	Shape shape;
	shape.decimal = IsOracleDecimal(text, shape);
	if (shape.decimal && text.find('.') == std::string::npos) {
		shape.integer = true;
		shape.integer_digits = text.size() - (text[0] == '-' ? 1 : 0);
	}
	if (shape.decimal) {
		shape.number = true;
	} else {
		const auto lower = Lower(text);
		if (lower == "inf" || lower == "-inf" || lower == "nan" || lower == "infinity" || lower == "-infinity") {
			shape.number = true;
		} else if (!text.empty() && text.find_first_of("eE") != std::string::npos &&
		           text.find_first_of("xXpP") == std::string::npos) {
			// strtod also reads hexadecimal; "0x1e5" is not a number Oracle sends.
			char *end = nullptr;
			std::strtod(text.c_str(), &end);
			shape.number = end == text.c_str() + text.size() && (IsDigit(text[0]) || text[0] == '-' || text[0] == '.');
		}
	}
	shape.date = text.size() == 10 && IsDateAt(text, 0);
	ParsedTimestamp parsed;
	if (ParseTimestamp(text, parsed)) {
		shape.timestamp = !parsed.has_offset;
		shape.timestamptz = parsed.has_offset;
		shape.midnight = shape.timestamp && parsed.hour == 0 && parsed.minute == 0 && parsed.second == 0 &&
		                 IsZeroFraction(parsed.fraction);
	}
	const auto lower = Lower(text);
	shape.boolean = lower == "y" || lower == "n" || lower == "true" || lower == "false" || lower == "1" || lower == "0";
	return shape;
}

//! Whether a column of `type` holds a value of `shape`. `declared` admits
//! the wider spellings an author may mean by number: exponents, Inf, NaN.
bool Holds(const ColumnType &type, const Shape &shape, bool declared) {
	switch (type.kind) {
	case ColumnKind::STRING:
	case ColumnKind::TEXT:
		return true;
	case ColumnKind::ID:
		return shape.integer;
	case ColumnKind::INTEGER:
		return shape.integer && shape.integer_digits <= MAX_INTEGER_DIGITS;
	case ColumnKind::DECIMAL:
		return shape.decimal && shape.scale <= static_cast<size_t>(type.scale) &&
		       shape.whole_digits <= static_cast<size_t>(type.precision - type.scale);
	case ColumnKind::NUMBER:
		return declared ? shape.number : shape.decimal;
	case ColumnKind::DATE:
		return shape.date || shape.midnight;
	case ColumnKind::TIMESTAMP:
		return shape.date || shape.timestamp;
	case ColumnKind::TIMESTAMPTZ:
		return shape.timestamptz;
	case ColumnKind::BOOLEAN:
		return shape.boolean;
	}
	return false;
}

//! The same question for every value at once.
bool HoldsAll(const ColumnType &type, const OracleColumnInference::Summary &all, bool declared) {
	if (all.values == 0) {
		return true;
	}
	Shape widest;
	widest.integer = all.all_integer;
	widest.integer_digits = all.max_integer_digits;
	widest.decimal = all.all_decimal;
	widest.whole_digits = all.max_whole_digits;
	widest.scale = all.max_scale;
	widest.number = declared ? all.all_number : all.all_decimal;
	widest.boolean = all.all_boolean;
	widest.timestamptz = all.all_timestamptz;
	switch (type.kind) {
	case ColumnKind::DATE:
		return all.all_date_or_midnight;
	case ColumnKind::TIMESTAMP:
		return all.all_date_or_timestamp;
	default:
		return Holds(type, widest, declared);
	}
}

ColumnType Decimal(int scale) {
	ColumnType type;
	type.kind = ColumnKind::DECIMAL;
	type.precision = static_cast<int>(MAX_DECIMAL_DIGITS);
	type.scale = scale;
	return type;
}

ColumnType OfKind(ColumnKind kind) {
	ColumnType type;
	type.kind = kind;
	return type;
}

bool EndsWithId(const std::string &name) {
	return name.size() >= 3 && Lower(name.substr(name.size() - 3)) == "_id";
}

} // namespace

OracleColumnInference::OracleColumnInference(std::string name_p, const ColumnType *declared_p, const ColumnType *start_p)
    : name(std::move(name_p)), has_declared(declared_p != nullptr), has_start(start_p != nullptr) {
	if (declared_p) {
		declared = *declared_p;
	}
	if (start_p) {
		start = *start_p;
	}
}

void OracleColumnInference::Add(const std::string &value) {
	rows++;
	if (value.empty()) {
		return;
	}
	const auto shape = ShapeOf(value);
	if (has_declared && mismatch_row == 0 && !Holds(declared, shape, true)) {
		mismatch_row = rows;
		mismatch_value = value;
	}
	auto &all = summary;
	all.values++;
	all.all_integer = all.all_integer && shape.integer;
	all.max_integer_digits = std::max(all.max_integer_digits, shape.integer_digits);
	all.all_decimal = all.all_decimal && shape.decimal;
	all.max_whole_digits = std::max(all.max_whole_digits, shape.whole_digits);
	all.max_scale = std::max(all.max_scale, shape.scale);
	all.all_number = all.all_number && shape.number;
	// Inference asks "all dates" and "all timestamps" separately (§3.3 п. 3: a
	// mix is a string); a column that already is a date or a timestamp holds
	// either form.
	all.all_date = all.all_date && shape.date;
	all.all_timestamp = all.all_timestamp && shape.timestamp;
	all.all_midnight = all.all_midnight && shape.midnight;
	all.all_date_or_midnight = all.all_date_or_midnight && (shape.date || shape.midnight);
	all.all_date_or_timestamp = all.all_date_or_timestamp && (shape.date || shape.timestamp);
	all.all_timestamptz = all.all_timestamptz && shape.timestamptz;
	all.all_boolean = all.all_boolean && shape.boolean;
}

ColumnInference OracleColumnInference::Finish() const {
	ColumnInference result;
	const auto &all = summary;

	if (has_declared) {
		result.type = declared;
		if (mismatch_row) {
			result.ok = false;
			result.code = "E_TYPE_DECLARED_MISMATCH";
			result.row = mismatch_row;
			result.value = mismatch_value;
			result.message = "колонка " + name + " объявлена как " + declared.ToString() + ", но в строке " +
			                 std::to_string(mismatch_row) + " значение «" + mismatch_value + "»";
		}
		return result;
	}

	if (has_start) {
		result.type = start;
		if (HoldsAll(start, all, false)) {
			return result;
		}
		// Only this run's values need to fit: each run stores its own dataset
		// instance, and the start type only keeps the schema steady between runs.
		std::vector<ColumnType> chain;
		switch (start.kind) {
		case ColumnKind::INTEGER:
			chain = {Decimal(static_cast<int>(all.max_scale)), OfKind(ColumnKind::NUMBER)};
			break;
		case ColumnKind::DECIMAL:
			chain = {Decimal(std::max(start.scale, static_cast<int>(all.max_scale))), OfKind(ColumnKind::NUMBER)};
			break;
		case ColumnKind::DATE:
			chain = {OfKind(ColumnKind::TIMESTAMP)};
			break;
		default:
			break;
		}
		ColumnType widened = OfKind(ColumnKind::STRING);
		for (const auto &candidate : chain) {
			if (HoldsAll(candidate, all, false)) {
				widened = candidate;
				break;
			}
		}
		result.type = widened;
		result.widened = true;
		result.warnings.push_back("колонка " + name + ": тип расширен с " + start.ToString() + " до " + widened.ToString());
		return result;
	}

	// §3.3 п. 3, in the table's order.
	if (all.values == 0) {
		result.type = OfKind(ColumnKind::STRING);
	} else if (all.all_integer) {
		result.type = OfKind(EndsWithId(name) || all.max_integer_digits > MAX_INTEGER_DIGITS ? ColumnKind::ID
		                                                                                     : ColumnKind::INTEGER);
	} else if (all.all_decimal) {
		if (all.max_whole_digits + all.max_scale <= MAX_DECIMAL_DIGITS) {
			result.type = Decimal(static_cast<int>(all.max_scale));
		} else {
			result.type = OfKind(ColumnKind::NUMBER);
			result.warnings.push_back("колонка " + name + ": больше 38 значащих цифр, тип number — точность ограничена double");
		}
	} else if (all.all_date) {
		result.type = OfKind(ColumnKind::DATE);
	} else if (all.all_timestamp && all.all_midnight) {
		result.type = OfKind(ColumnKind::DATE);
	} else if (all.all_timestamp) {
		result.type = OfKind(ColumnKind::TIMESTAMP);
	} else if (all.all_timestamptz) {
		result.type = OfKind(ColumnKind::TIMESTAMPTZ);
	} else {
		result.type = OfKind(ColumnKind::STRING);
	}
	return result;
}

StoredValue ConvertOracleValue(const ColumnType &type, const std::string &text, std::string &stored) {
	if (text.empty()) {
		return StoredValue::NULL_VALUE;
	}
	const auto shape = ShapeOf(text);
	if (!Holds(type, shape, true)) {
		return StoredValue::MISMATCH;
	}
	ParsedTimestamp parsed;
	switch (type.kind) {
	case ColumnKind::DECIMAL:
		stored = CanonicalDecimalText(text);
		break;
	case ColumnKind::NUMBER: {
		const double value = std::strtod(text.c_str(), nullptr);
		stored = std::isnan(value) ? "nan" : std::isinf(value) ? (value < 0 ? "-inf" : "inf") : ShortestDouble(value);
		break;
	}
	case ColumnKind::DATE:
		stored = text.substr(0, 10);
		break;
	case ColumnKind::TIMESTAMP:
		if (shape.date) {
			stored = text + " 00:00:00";
		} else {
			ParseTimestamp(text, parsed);
			stored = FormatTimestamp(parsed, ' ');
		}
		break;
	case ColumnKind::TIMESTAMPTZ:
		ParseTimestamp(text, parsed);
		stored = FormatTimestamp(ToUtc(parsed), ' ');
		break;
	case ColumnKind::BOOLEAN: {
		const auto lower = Lower(text);
		stored = lower == "y" || lower == "true" || lower == "1" ? "true" : "false";
		break;
	}
	case ColumnKind::ID:
	case ColumnKind::INTEGER:
	case ColumnKind::STRING:
	case ColumnKind::TEXT:
		stored = text;
		break;
	}
	return StoredValue::VALUE;
}

JsonCell EncodeJsonValue(const ColumnType &type, const std::string *text) {
	JsonCell cell;
	if (!text) {
		return cell;
	}
	const auto as_string = [&](std::string value) {
		cell.kind = JsonCell::Kind::STRING;
		cell.text = std::move(value);
		return cell;
	};
	ParsedTimestamp parsed;
	switch (type.kind) {
	case ColumnKind::ID:
	case ColumnKind::STRING:
	case ColumnKind::TEXT:
	case ColumnKind::DATE:
		return as_string(*text);
	case ColumnKind::INTEGER: {
		char *end = nullptr;
		const long long value = std::strtoll(text->c_str(), &end, 10);
		if (text->empty() || end != text->c_str() + text->size()) {
			return as_string(*text);
		}
		cell.kind = JsonCell::Kind::INTEGER;
		cell.integer = value;
		return cell;
	}
	case ColumnKind::DECIMAL:
		// A string, so that no digit is lost to a JSON reader's double.
		return as_string(IsPlainDecimal(*text) ? CanonicalDecimalText(*text) : *text);
	case ColumnKind::NUMBER: {
		const double value = std::strtod(text->c_str(), nullptr);
		if (!std::isfinite(value)) {
			cell.non_finite = true;
			return cell;
		}
		cell.kind = JsonCell::Kind::NUMBER;
		cell.number = value;
		return cell;
	}
	case ColumnKind::TIMESTAMP:
		return as_string(ParseTimestamp(*text, parsed) ? FormatTimestamp(parsed, ' ') : *text);
	case ColumnKind::TIMESTAMPTZ:
		// Stored at UTC (§3.2), so the offset, if DuckDB printed one, is zero.
		return as_string(ParseTimestamp(*text, parsed) ? FormatTimestamp(ToUtc(parsed), 'T') + "Z" : *text);
	case ColumnKind::BOOLEAN:
		cell.kind = JsonCell::Kind::BOOLEAN;
		cell.boolean = *text == "true" || *text == "1";
		return cell;
	}
	return cell;
}

} // namespace ofexcel
