// The setup without its window (unattended.cpp): --install-to / --remove.
#pragma once

#include "core/util.h"

namespace kuf::setup {

bool IsUnattended(int argc, char** argv);
int RunUnattended(int argc, char** argv, const fs::path& exe);

}  // namespace kuf::setup
