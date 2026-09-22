#pragma once

// The byte codec both sides of the module boundary compile.
// Private: nothing here appears in a published ABI header, and it is free to
// use Qt and the standard library precisely because it never crosses anything.
//
// ONE IMPLEMENTATION, COMPILED TWICE. The host shim and the module link the
// same sources, so an encoder and its decoder cannot drift. That is a
// deliberate trade: it means a module built from a different revision of this
// file is incompatible, which is exactly what wire.h's revision number and the
// handshake are for.
//
// DEFENSIVE BY CONSTRUCTION
//   * Every read is bounds-checked; the first overrun latches an error and
//     every later read returns a default. A decoder never reads uninitialised
//     memory and never returns half a record as if it were whole.
//   * A length or count is validated against what REMAINS, before anything is
//     reserved, so a malformed packet claiming four billion elements costs one
//     comparison rather than an allocation.
//   * Integers are written a byte at a time, little-endian, so the encoding
//     does not depend on the host's byte order or on struct padding.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace clashqt::integration::marshal {

class ByteWriter {
  public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserveBytes) { bytes_.reserve(reserveBytes); }

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void f64(double value);
    void boolean(bool value) { u8(value ? 1 : 0); }
    void raw(const void *data, std::size_t size);

    /// u32 byte length followed by that many UTF-8 bytes. Never NUL-terminated.
    void text(const QString &value);
    void textList(const QStringList &values);

    const std::vector<std::uint8_t> &data() const noexcept { return bytes_; }
    std::size_t size() const noexcept { return bytes_.size(); }
    bool isEmpty() const noexcept { return bytes_.empty(); }

  private:
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
  public:
    ByteReader(const void *data, std::size_t size) noexcept
        : data_(static_cast<const std::uint8_t *>(data)), size_(data == nullptr ? 0 : size) {}

    std::uint8_t u8() noexcept;
    std::uint16_t u16() noexcept;
    std::uint32_t u32() noexcept;
    std::uint64_t u64() noexcept;
    std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }
    double f64() noexcept;
    bool boolean() noexcept { return u8() != 0; }

    QString text();
    QStringList textList();

    /// A count that is about to drive a loop. Fails the reader when the claimed
    /// count cannot possibly fit in what remains, `minimumBytesEach` being the
    /// smallest encoding one element can have.
    std::uint32_t count(std::size_t minimumBytesEach) noexcept;

    /// Sticky: false once any read has overrun or any validation has failed.
    bool ok() const noexcept { return ok_; }
    /// Every byte consumed and nothing went wrong. A decoder asserts this so a
    /// producer that appends a field an older peer ignores is caught here
    /// rather than by the next release.
    bool finished() const noexcept { return ok_ && cursor_ == size_; }
    std::size_t remaining() const noexcept { return ok_ ? size_ - cursor_ : 0; }
    void fail() noexcept { ok_ = false; }

  private:
    const std::uint8_t *take(std::size_t bytes) noexcept;

    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
    bool ok_ = true;
};

}  // namespace clashqt::integration::marshal
