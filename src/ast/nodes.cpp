#include "vestra/ast/nodes.hpp"

// Out-of-line definitions live here so that the destructor virtual tables are
// pinned to a single translation unit (avoids weak-vtable warnings under
// -Wweak-vtables).

namespace vestra::ast {

// Intentionally empty — all node members are inline/default-defined. We still
// want at least one .cpp so the library has a TU per header.

}  // namespace vestra::ast
