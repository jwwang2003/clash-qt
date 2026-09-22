#pragma once

// The packed connections snapshot: one buffer per snapshot, fixed-layout
// records indexing a shared UTF-8 blob.
//
// WHY THIS IS NOT THE GENERAL ENCODING. A connections snapshot can be hundreds
// of entries, and naive per-string allocation is the one plausible regression
// this boundary introduces. So: one buffer per snapshot with fixed-layout
// structs indexing into it, and a case in the benchmark lane before any
// performance claim is made. Every other payload on this boundary is a handful
// of fields at human rate; this one is 13 strings times hundreds of rows at the
// engine's emission rate.
//
// Three things make it cheap, and all three are measured rather than asserted:
//   1. one allocation for the whole snapshot on each side, not 13 per row;
//   2. identical strings are stored once - "tcp", "udp", a rule name and a
//      process path repeat across nearly every row of a real snapshot;
//   3. the decoder validates the whole buffer before it constructs anything,
//      so a malformed packet costs a scan rather than a partial snapshot.
//
// The layout itself is published in core/component/abi/wire.h, because a
// foreign module has to be able to produce it.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QString>
#include <QVector>

#include "core/backend/types.h"

namespace clashqt::integration::marshal {

namespace cb = ::core::backend;

struct ConnectionSnapshot {
    cb::Generation generation = cb::Generation::Initial;
    quint64 uploadTotal = 0;
    quint64 downloadTotal = 0;
    QVector<cb::Connection> connections;
};

/// Packs a snapshot. Never fails: every input is representable.
std::vector<std::uint8_t> packConnectionSnapshot(cb::Generation generation,
                                                 cb::Span<cb::Connection> connections,
                                                 quint64 uploadTotal, quint64 downloadTotal);

/// Unpacks one. Returns false and leaves `out` untouched when the buffer is
/// malformed in ANY way - wrong magic, wrong version, a record size this build
/// does not know, an offset outside the blob, a chain run outside the chain
/// section, or trailing bytes that belong to nothing. `reason` names which,
/// because "malformed packet" with no detail is how a producer bug survives.
bool unpackConnectionSnapshot(const void *data, std::size_t size, ConnectionSnapshot *out,
                              QString *reason);

}  // namespace clashqt::integration::marshal
