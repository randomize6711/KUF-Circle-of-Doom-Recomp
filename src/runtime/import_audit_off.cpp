// Builds without KUFCOD_IMPORT_AUDIT: the generated code calls the runtime's imports directly.
#include "runtime/import_audit.h"

namespace kuf::audit {

bool Enabled() { return false; }

void WriteJson(std::ostream& out) { out << "  \"imports\": null,\n"; }

}  // namespace kuf::audit
