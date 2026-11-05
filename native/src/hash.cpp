#include "vortex/core/hash.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace vortex::core {

namespace {

constexpr std::array<std::uint32_t, 64> MD5_K = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
    0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
    0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
    0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
    0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
    0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
    0xeb86d391,
};

constexpr std::array<std::uint32_t, 64> MD5_S = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

std::uint32_t loadLe32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

void storeLe32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8);
    data[2] = static_cast<std::uint8_t>(value >> 16);
    data[3] = static_cast<std::uint8_t>(value >> 24);
}

class Md5 {
public:
    void update(const std::uint8_t* data, std::size_t length) {
        total_ += length;
        while (length > 0) {
            const auto copy = std::min<std::size_t>(length, buffer_.size() - bufferLength_);
            std::memcpy(buffer_.data() + bufferLength_, data, copy);
            bufferLength_ += copy;
            data += copy;
            length -= copy;
            if (bufferLength_ == buffer_.size()) {
                transform(buffer_.data());
                bufferLength_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 16> finalize() {
        const auto bitCount = total_ * 8;
        std::uint8_t padding[72] = {0x80};
        const auto paddingLength =
            bufferLength_ < 56 ? 56 - bufferLength_ : 120 - bufferLength_;
        update(padding, paddingLength);
        std::uint8_t lengthBytes[8];
        for (std::size_t i = 0; i < 8; ++i) {
            lengthBytes[i] = static_cast<std::uint8_t>(bitCount >> (8 * i));
        }
        update(lengthBytes, sizeof(lengthBytes));

        std::array<std::uint8_t, 16> digest{};
        storeLe32(digest.data(), state_[0]);
        storeLe32(digest.data() + 4, state_[1]);
        storeLe32(digest.data() + 8, state_[2]);
        storeLe32(digest.data() + 12, state_[3]);
        return digest;
    }

private:
    void transform(const std::uint8_t* block) {
        std::uint32_t words[16];
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = loadLe32(block + i * 4);
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];

        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }

            const auto sum = f + a + MD5_K[i] + words[g];
            a = d;
            d = c;
            c = b;
            b += std::rotl(sum, static_cast<int>(MD5_S[i]));
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
    }

    std::array<std::uint32_t, 4> state_ = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferLength_ = 0;
    std::uint64_t total_ = 0;
};

constexpr std::uint64_t XXH_P1 = 11400714785074694791ULL;
constexpr std::uint64_t XXH_P2 = 14029467366897019727ULL;
constexpr std::uint64_t XXH_P3 = 1609587929392839161ULL;
constexpr std::uint64_t XXH_P4 = 9650029242287828579ULL;
constexpr std::uint64_t XXH_P5 = 2870177450012600261ULL;

std::uint64_t loadLe64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
    }
    return value;
}

std::uint32_t loadLe32(const std::uint8_t* data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

class Xxh64 {
public:
    Xxh64() { reset(); }

    void update(const std::uint8_t* data, std::size_t length) {
        total_ += length;
        if (bufferLength_ + length < buffer_.size()) {
            std::memcpy(buffer_.data() + bufferLength_, data, length);
            bufferLength_ += length;
            return;
        }

        if (bufferLength_ > 0) {
            const auto fill = buffer_.size() - bufferLength_;
            std::memcpy(buffer_.data() + bufferLength_, data, fill);
            processStripe(buffer_.data());
            data += fill;
            length -= fill;
            bufferLength_ = 0;
        }

        while (length >= 32) {
            processStripe(data);
            data += 32;
            length -= 32;
        }

        if (length > 0) {
            std::memcpy(buffer_.data(), data, length);
            bufferLength_ = length;
        }
    }

    std::uint64_t finalize() {
        std::uint64_t hash = 0;
        if (total_ >= 32) {
            hash = std::rotl(accumulators_[0], 1) + std::rotl(accumulators_[1], 7) +
                   std::rotl(accumulators_[2], 12) + std::rotl(accumulators_[3], 18);
            hash = merge(hash, accumulators_[0]);
            hash = merge(hash, accumulators_[1]);
            hash = merge(hash, accumulators_[2]);
            hash = merge(hash, accumulators_[3]);
        } else {
            hash = XXH_P5;
        }

        hash += total_;

        const auto* cursor = buffer_.data();
        auto remaining = bufferLength_;
        while (remaining >= 8) {
            auto lane = round(0, loadLe64(cursor));
            hash ^= lane;
            hash = std::rotl(hash, 27) * XXH_P1 + XXH_P4;
            cursor += 8;
            remaining -= 8;
        }
        if (remaining >= 4) {
            hash ^= static_cast<std::uint64_t>(loadLe32(cursor, 0)) * XXH_P1;
            hash = std::rotl(hash, 23) * XXH_P2 + XXH_P3;
            cursor += 4;
            remaining -= 4;
        }
        while (remaining > 0) {
            hash ^= static_cast<std::uint64_t>(*cursor) * XXH_P5;
            hash = std::rotl(hash, 11) * XXH_P1;
            ++cursor;
            --remaining;
        }

        hash ^= hash >> 33;
        hash *= XXH_P2;
        hash ^= hash >> 29;
        hash *= XXH_P3;
        hash ^= hash >> 32;
        return hash;
    }

private:
    void reset() {
        accumulators_[0] = XXH_P1 + XXH_P2;
        accumulators_[1] = XXH_P2;
        accumulators_[2] = 0;
        accumulators_[3] = 0 - XXH_P1;
        bufferLength_ = 0;
        total_ = 0;
    }

    static std::uint64_t round(std::uint64_t accumulator, std::uint64_t input) {
        accumulator += input * XXH_P2;
        accumulator = std::rotl(accumulator, 31);
        accumulator *= XXH_P1;
        return accumulator;
    }

    static std::uint64_t merge(std::uint64_t hash, std::uint64_t accumulator) {
        hash ^= round(0, accumulator);
        hash = hash * XXH_P1 + XXH_P4;
        return hash;
    }

    void processStripe(const std::uint8_t* stripe) {
        accumulators_[0] = round(accumulators_[0], loadLe64(stripe));
        accumulators_[1] = round(accumulators_[1], loadLe64(stripe + 8));
        accumulators_[2] = round(accumulators_[2], loadLe64(stripe + 16));
        accumulators_[3] = round(accumulators_[3], loadLe64(stripe + 24));
    }

    std::array<std::uint64_t, 4> accumulators_{};
    std::array<std::uint8_t, 32> buffer_{};
    std::size_t bufferLength_ = 0;
    std::uint64_t total_ = 0;
};

std::string toHex(const std::uint8_t* data, std::size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 0x0f]);
    }
    return result;
}

std::string toBase64(const std::uint8_t* data, std::size_t length) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((length + 2) / 3) * 4);
    for (std::size_t i = 0; i < length; i += 3) {
        const auto remaining = length - i;
        const auto a = static_cast<std::uint32_t>(data[i]);
        const auto b = remaining > 1 ? static_cast<std::uint32_t>(data[i + 1]) : 0;
        const auto c = remaining > 2 ? static_cast<std::uint32_t>(data[i + 2]) : 0;
        result.push_back(alphabet[a >> 2]);
        result.push_back(alphabet[((a & 0x03) << 4) | (b >> 4)]);
        result.push_back(remaining > 1 ? alphabet[((b & 0x0f) << 2) | (c >> 6)] : '=');
        result.push_back(remaining > 2 ? alphabet[c & 0x3f] : '=');
    }
    return result;
}

struct FileHash {
    std::string hash;
    std::uint64_t bytes = 0;
};

FileHash hashFile(const std::filesystem::path& path, HashAlgorithm algorithm) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open file");
    }

    Md5 md5;
    Xxh64 xxh64;
    std::array<std::uint8_t, 1 << 20> buffer{};
    FileHash result;

    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read <= 0) {
            break;
        }
        const auto count = static_cast<std::size_t>(read);
        if (algorithm == HashAlgorithm::Md5) {
            md5.update(buffer.data(), count);
        } else {
            xxh64.update(buffer.data(), count);
        }
        result.bytes += count;
    }

    if (input.bad()) {
        throw std::runtime_error("failed to read file");
    }

    if (algorithm == HashAlgorithm::Md5) {
        const auto digest = md5.finalize();
        result.hash = toHex(digest.data(), digest.size());
    } else {
        const auto digest = xxh64.finalize();
        std::uint8_t bytes[8];
        for (std::size_t i = 0; i < 8; ++i) {
            bytes[i] = static_cast<std::uint8_t>(digest >> (56 - 8 * i));
        }
        result.hash = toBase64(bytes, sizeof(bytes));
    }
    return result;
}

} // namespace

HashAlgorithm parseHashAlgorithm(const std::string& value) {
    if (value == "md5") {
        return HashAlgorithm::Md5;
    }
    if (value == "xxh64" || value == "xxhash64") {
        return HashAlgorithm::Xxh64;
    }
    throw std::runtime_error("unsupported hash algorithm: " + value);
}

const char* hashAlgorithmName(HashAlgorithm algorithm) {
    return algorithm == HashAlgorithm::Md5 ? "md5" : "xxh64";
}

HashFilesResult hashFiles(const std::vector<std::string>& paths, HashAlgorithm algorithm,
                          std::size_t concurrency) {
    HashFilesResult result;
    result.results.resize(paths.size());
    if (paths.empty()) {
        return result;
    }

    std::vector<HashFailure> failures;
    std::mutex failuresMutex;
    std::atomic<std::size_t> next{0};

    const auto hardware = std::thread::hardware_concurrency();
    const auto requested = concurrency == 0 ? std::size_t{1} : concurrency;
    const auto workers = std::clamp<std::size_t>(
        std::min<std::size_t>(requested, hardware == 0 ? 1 : hardware), 1, 8);
    const auto workerCount = std::min<std::size_t>(workers, paths.size());

    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    try {
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            threads.emplace_back([&] {
                for (;;) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= paths.size()) {
                        return;
                    }
                    try {
                        const auto hash = hashFile(paths[index], algorithm);
                        result.results[index] = {paths[index], hash.hash, hash.bytes};
                    } catch (const std::exception& err) {
                        std::lock_guard<std::mutex> lock(failuresMutex);
                        failures.push_back({paths[index], err.what()});
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(failuresMutex);
                        failures.push_back({paths[index], "unknown hashing error"});
                    }
                }
            });
        }
    } catch (...) {
        for (auto& thread : threads) {
            thread.join();
        }
        throw;
    }
    for (auto& thread : threads) {
        thread.join();
    }

    result.failures = std::move(failures);
    return result;
}

} // namespace vortex::core
