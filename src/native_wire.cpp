#include "native/core.hpp"
#include <cstring>

namespace rosh {
static void size_check(const Bytes &bytes, std::size_t offset, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset)
        throw std::runtime_error("Truncated native packet");
}
void put16(Bytes &bytes, std::size_t offset, std::uint16_t value, bool big) {
    size_check(bytes, offset, 2);
    bytes[offset] = static_cast<unsigned char>(big ? value >> 8 : value);
    bytes[offset + 1] = static_cast<unsigned char>(big ? value : value >> 8);
}
void put32(Bytes &bytes, std::size_t offset, std::uint32_t value, bool big) {
    size_check(bytes, offset, 4);
    for (int i = 0; i < 4; i++)
        bytes[offset + i] = static_cast<unsigned char>(value >> (big ? (3 - i) * 8 : i * 8));
}
std::uint16_t get16(const Bytes &bytes, std::size_t offset, bool big) {
    size_check(bytes, offset, 2);
    return static_cast<std::uint16_t>(big ? (bytes[offset] << 8) | bytes[offset + 1]
                                          : bytes[offset] | (bytes[offset + 1] << 8));
}
std::uint32_t get32(const Bytes &bytes, std::size_t offset, bool big) {
    size_check(bytes, offset, 4);
    std::uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value |= std::uint32_t(bytes[offset + i]) << (big ? (3 - i) * 8 : i * 8);
    return value;
}
void putfloat(Bytes &bytes, std::size_t offset, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    put32(bytes, offset, bits);
}
float getfloat(const Bytes &bytes, std::size_t offset) {
    auto bits = get32(bytes, offset);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
Bytes make_frame(unsigned char category, unsigned char sub, const Bytes &body, std::uint16_t sequence) {
    if (body.size() > 0x100000 - 6)
        throw std::runtime_error("Native payload too large");
    Bytes frame(10 + body.size());
    put32(frame, 0, static_cast<std::uint32_t>(body.size() + 6), true);
    frame[4] = category;
    frame[5] = sub;
    put16(frame, 8, sequence, true);
    std::copy(body.begin(), body.end(), frame.begin() + 10);
    return frame;
}
std::vector<Bytes> extract_frames(Bytes &buffer) {
    if (buffer.size() > 2 * 1024 * 1024)
        throw std::runtime_error("Receive buffer limit");
    std::vector<Bytes> frames;
    std::size_t used = 0;
    while (buffer.size() - used >= 4) {
        auto size = get32(buffer, used, true);
        if (size < 6 || size > 0x100000)
            throw std::runtime_error("Invalid native frame length");
        if (buffer.size() - used < size + 4)
            break;
        frames.emplace_back(buffer.begin() + used + 4, buffer.begin() + used + 4 + size);
        used += size + 4;
    }
    buffer.erase(buffer.begin(), buffer.begin() + used);
    return frames;
}
LoginFields parse_login(const Bytes &frame) {
    if (frame.size() < 282 || frame.size() > 4096 || frame[0] != 2 || frame[1] != 0)
        throw std::runtime_error("Invalid native login frame");
    std::vector<std::string> fields;
    std::string current;
    for (std::size_t i = 277; i < frame.size(); i++) {
        auto c = frame[i];
        if (c == 0) {
            fields.push_back(current);
            current.clear();
        } else {
            current += static_cast<char>(c);
            if (current.size() > 255)
                throw std::runtime_error("Native login field limit");
        }
    }
    if (!current.empty() || frame.back() != 0 || fields.size() != 5)
        throw std::runtime_error("Invalid native login fields");
    for (unsigned char ch : fields[0] + fields[1])
        if (ch > 127)
            throw std::runtime_error("Native identification encoding");
    std::string opaque = fields[2];
    opaque += '\0';
    opaque += fields[3];
    opaque += '\0';
    opaque += fields[4];
    return {fields[0], fields[1], sha256(opaque)};
}
Bytes notice(const std::string &value) {
    auto text = wide(value);
    std::vector<std::uint32_t> codes;
    for (std::size_t i = 0; i < text.size(); i++) {
        auto code = static_cast<std::uint32_t>(text[i]);
        if (code >= 0xd800 && code <= 0xdbff) {
            if (i + 1 >= text.size())
                throw std::runtime_error("Invalid text");
            auto low = static_cast<std::uint32_t>(text[++i]);
            if (low < 0xdc00 || low > 0xdfff)
                throw std::runtime_error("Invalid text");
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        }
        codes.push_back(code);
    }
    if (codes.size() > 1024)
        throw std::runtime_error("Notice too long");
    Bytes body(3 + 4 * (codes.size() + 1));
    put16(body, 0, static_cast<std::uint16_t>(codes.size()), true);
    for (std::size_t i = 0; i < codes.size(); i++)
        put32(body, 3 + i * 4, codes[i]);
    return body;
}
} // namespace rosh
