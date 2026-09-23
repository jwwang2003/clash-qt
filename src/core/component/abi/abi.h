#ifndef CLASHQT_CORE_COMPONENT_ABI_ABI_H
#define CLASHQT_CORE_COMPONENT_ABI_ABI_H

// Umbrella for the backend module ABI. A consumer may
// include this or any single header below it.
//
// These headers are the ONLY thing a foreign module has to compile against.
// Nothing here includes a Qt header, allocates, throws, or names a type whose
// layout depends on a standard library build: that is what makes the set
// publishable. The private marshalling that turns backend-r4's Qt values into
// the byte encodings of wire.h lives on each side of the boundary, not here.

#include "core/component/abi/backend_abi.h"
#include "core/component/abi/module_entry.h"
#include "core/component/abi/target_abi.h"
#include "core/component/abi/wire.h"
#include "core/component/component.h"

#endif  // CLASHQT_CORE_COMPONENT_ABI_ABI_H
