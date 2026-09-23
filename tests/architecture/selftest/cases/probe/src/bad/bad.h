#pragma once
// Two defects a public header must not have, and neither shows up while the
// library builds itself:
//   1. PrivateHandle is declared in a private header this one does not include
//      and consumers cannot reach.
//   2. std::string is used without <string>, so it only compiles when some
//      earlier include happened to pull it in.
namespace synth_bad_ns {
struct Value { std::string name; PrivateHandle handle; };
Value make();
}
