// libFuzzer target for the JSON parser (MSVC: built with /fsanitize=fuzzer,address via CADENCE_BUILD_FUZZERS).
// Run: fuzz_json.exe -max_total_time=60

#include "core/JsonParser.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    cadence::JsonValue root;
    cadence::JsonParser parser(std::string_view(reinterpret_cast<const char*>(data), size));
    (void)parser.Parse(root);
    return 0;
}
