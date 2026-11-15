// Setting an environment variable for this process (M16): POSIX setenv, Windows _putenv_s (both update what getenv,
// which the SDK reads its REX_* options with, returns).
#pragma once

#include <cstdlib>

namespace kuf {

inline void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

}  // namespace kuf
