#pragma once

#include <string_view>

// The build's version (cmake/kuf_version.cmake): "<base>-dev.<n>+g<commit>[.dirty]", or the release version given
// to a release build. The updater (tools/updater) and Check for Updates compare release versions.
namespace kuf::version {

std::string_view String();
std::string_view Base();
std::string_view Commit();
// When this build was made, UTC, "YYYY-MM-DD HH:MM" (cmake/kuf_version.cmake: SOURCE_DATE_EPOCH, else the commit's
// time for a clean tree, else the build's own time).
std::string_view BuildTimeUtc();

}  // namespace kuf::version
