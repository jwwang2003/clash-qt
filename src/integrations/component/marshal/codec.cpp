#include "integrations/component/marshal/codec.h"

#include <cstring>
#include <limits>

namespace clashqt::integration::marshal {
namespace {

// The largest string or blob any single field may carry. A controller response
// or a log line is orders of magnitude below this; the limit exists so a
// corrupted length is rejected rather than turned into an allocation.
constexpr std::uint32_t kMaxFieldBytes = 64u * 1024u * 1024u;

}  // namespace

void ByteWriter::u8(std::uint8_t value) { bytes_.push_back(value); }

void ByteWriter::u16(std::uint16_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void ByteWriter::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

void ByteWriter::f64(double value) {
    // Bit pattern, not a decimal rendering: a rate must survive the boundary
    // exactly, and printf/parse round-tripping is where a delay of 0.1 becomes
    // 0.09999999999999999.
    static_assert(sizeof(double) == sizeof(std::uint64_t), "IEEE-754 binary64 expected");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
}

void ByteWriter::raw(const void *data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    const auto *first = static_cast<const std::uint8_t *>(data);
    bytes_.insert(bytes_.end(), first, first + size);
}

void ByteWriter::text(const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    u32(static_cast<std::uint32_t>(utf8.size()));
    raw(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

void ByteWriter::textList(const QStringList &values) {
    u32(static_cast<std::uint32_t>(values.size()));
    for (const QString &value : values) {
        text(value);
    }
}

const std::uint8_t *ByteReader::take(std::size_t bytes) noexcept {
    if (!ok_ || bytes > size_ - cursor_) {
        ok_ = false;
        return nullptr;
    }
    const std::uint8_t *at = data_ + cursor_;
    cursor_ += bytes;
    return at;
}

std::uint8_t ByteReader::u8() noexcept {
    const std::uint8_t *at = take(1);
    return at == nullptr ? 0 : *at;
}

std::uint16_t ByteReader::u16() noexcept {
    const std::uint8_t *at = take(2);
    if (at == nullptr) {
        return 0;
    }
    return static_cast<std::uint16_t>(at[0] | (static_cast<std::uint16_t>(at[1]) << 8));
}

std::uint32_t ByteReader::u32() noexcept {
    const std::uint8_t *at = take(4);
    if (at == nullptr) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(at[index]) << (index * 8);
    }
    return value;
}

std::uint64_t ByteReader::u64() noexcept {
    const std::uint8_t *at = take(8);
    if (at == nullptr) {
        return 0;
    }
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(at[index]) << (index * 8);
    }
    return value;
}

double ByteReader::f64() noexcept {
    const std::uint64_t bits = u64();
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

QString ByteReader::text() {
    const std::uint32_t length = u32();
    if (!ok_) {
        return {};
    }
    if (length > kMaxFieldBytes || length > remaining()) {
        ok_ = false;
        return {};
    }
    const std::uint8_t *at = take(length);
    if (at == nullptr) {
        return {};
    }
    // fromUtf8 replaces invalid sequences rather than rejecting them, which is
    // the right answer here: a log line from an engine is not always valid
    // UTF-8, and a replacement character is a better outcome than dropping the
    // whole snapshot the line arrived in.
    return QString::fromUtf8(reinterpret_cast<const char *>(at), static_cast<qsizetype>(length));
}

std::uint32_t ByteReader::count(std::size_t minimumBytesEach) noexcept {
    const std::uint32_t value = u32();
    if (!ok_) {
        return 0;
    }
    const std::size_t floor = minimumBytesEach == 0 ? 1 : minimumBytesEach;
    if (static_cast<std::size_t>(value) > remaining() / floor) {
        ok_ = false;
        return 0;
    }
    return value;
}

QStringList ByteReader::textList() {
    const std::uint32_t size = count(sizeof(std::uint32_t));
    QStringList values;
    values.reserve(static_cast<qsizetype>(size));
    for (std::uint32_t index = 0; index < size && ok_; ++index) {
        values.append(text());
    }
    return ok_ ? values : QStringList{};
}

}  // namespace clashqt::integration::marshal
