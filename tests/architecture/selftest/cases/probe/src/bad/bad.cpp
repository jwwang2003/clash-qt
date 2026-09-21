// The accidental include order that hides the defect.
#include <string>
#include "private_detail.h"
#include "bad/bad.h"
namespace synth_bad_ns { Value make() { return Value{"bad", PrivateHandle{-1}}; } }
