// Import audit hooks (see import_audit.cpp). Without KUFCOD_IMPORT_AUDIT the functions are no-ops.
#pragma once

#include <ostream>

namespace kuf::audit {

// True when the executable was built with the import audit.
bool Enabled();

// Appends the `"imports": [...]` member (calls per import) to run-summary.json.
void WriteJson(std::ostream& out);

}  // namespace kuf::audit
