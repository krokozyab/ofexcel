#include "ofexcel/version.hpp"

namespace ofexcel {

const char *const PRODUCT_VERSION = "0.1.0-dev";
const char *const API_VERSION = "1.0";
const char *const API_MIN_CLIENT = "1.0";

std::string VersionWithBuildStamp() {
	return std::string(PRODUCT_VERSION) + " (built " __DATE__ " " __TIME__ ")";
}

} // namespace ofexcel
