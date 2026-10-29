#include "ofexcel/utf8.hpp"

namespace ofexcel {

bool IsValidUtf8(const std::string &text) {
	const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
	const size_t size = text.size();
	size_t i = 0;
	while (i < size) {
		const unsigned char lead = bytes[i];
		size_t length;
		unsigned char min_second = 0x80;
		unsigned char max_second = 0xBF;
		if (lead < 0x80) {
			i++;
			continue;
		} else if (lead >= 0xC2 && lead <= 0xDF) {
			length = 2;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			length = 3;
			if (lead == 0xE0) {
				min_second = 0xA0; // overlong
			} else if (lead == 0xED) {
				max_second = 0x9F; // surrogates
			}
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			length = 4;
			if (lead == 0xF0) {
				min_second = 0x90; // overlong
			} else if (lead == 0xF4) {
				max_second = 0x8F; // above U+10FFFF
			}
		} else {
			return false;
		}
		if (i + length > size || bytes[i + 1] < min_second || bytes[i + 1] > max_second) {
			return false;
		}
		for (size_t k = 2; k < length; k++) {
			if (bytes[i + k] < 0x80 || bytes[i + k] > 0xBF) {
				return false;
			}
		}
		i += length;
	}
	return true;
}

size_t Utf8Length(const std::string &text) {
	size_t count = 0;
	for (const char c : text) {
		// Every byte but a continuation byte starts a code point.
		count += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
	}
	return count;
}

} // namespace ofexcel
