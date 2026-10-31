#include "runtime/version.h"

#include "kuf_version_generated.h"

namespace kuf::version {

std::string_view String() { return KUFCOD_VERSION; }
std::string_view Base() { return KUFCOD_BASE_VERSION; }
std::string_view Commit() { return KUFCOD_GIT_COMMIT; }
std::string_view BuildTimeUtc() { return KUFCOD_BUILD_TIME_UTC; }

}  // namespace kuf::version
