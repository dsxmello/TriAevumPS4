// SHA-256 for the console Forge (FIPS 180-4). Every input identity in the
// revision recipes is a SHA-256; the console has no hashing library.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace triaevum::forge {

class Sha256 final {
  public:
    Sha256() noexcept;
    void Update(const void* data, size_t size) noexcept;
    std::array<uint8_t, 32> Finish() noexcept;
    // Lowercase hex of Finish(), the recipes' notation.
    std::string FinishHex();

    static std::string Hex(const void* data, size_t size);
    static std::string Of(const void* data, size_t size);

  private:
    void Block(const uint8_t* block) noexcept;

    std::array<uint32_t, 8> mState{};
    std::array<uint8_t, 64> mBuffer{};
    uint64_t mLength = 0;
    size_t mBuffered = 0;
};

} // namespace triaevum::forge
