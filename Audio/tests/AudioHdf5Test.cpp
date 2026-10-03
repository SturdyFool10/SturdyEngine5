#include <Audio/Hdf5.hpp>
#include <Audio/Inflate.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace SFT::Audio;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }
} // namespace

int main(int argc, char **argv) {
    // zlib streams made with Python's zlib: stored, fixed Huffman and dynamic Huffman blocks.
    {
        const unsigned char fixed[] = {0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01, 0x00, 0x1a, 0x0b, 0x04, 0x5d}; // "hello world"
        auto out = inflate(std::span<const std::byte>(reinterpret_cast<const std::byte *>(fixed), sizeof(fixed)), true, 11);
        check(out && out->size() == 11 && std::memcmp(out->data(), "hello world", 11) == 0, "a fixed-Huffman zlib stream inflates");
        const unsigned char stored[] = {0x78, 0x01, 0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c', 0x02, 0x4d, 0x01, 0x27};
        auto s = inflate(std::span<const std::byte>(reinterpret_cast<const std::byte *>(stored), sizeof(stored)), true);
        check(s && s->size() == 3 && std::memcmp(s->data(), "abc", 3) == 0, "a stored block inflates");
        auto bad = inflate(std::span<const std::byte>(reinterpret_cast<const std::byte *>(fixed), 8), true);
        check(!bad.has_value(), "a truncated stream is an error, not a crash");
    }
    const char *path = argc > 1 ? argv[1] : std::getenv("STURDY_TEST_SOFA");
    if (path == nullptr) {
        std::cout << "  (set STURDY_TEST_SOFA to a .sofa file to test the HDF5 reader against real data)\n";
        return failures == 0 ? 0 : 1;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<std::byte> bytes;
    {
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bytes.resize(raw.size());
        std::memcpy(bytes.data(), raw.data(), raw.size());
    }
    auto file = Hdf5File::open(bytes);
    check(file.has_value(), "the file opens");
    if (!file) return 1;
    for (const char *name : {"/Data.IR", "/Data.SamplingRate", "/SourcePosition", "/ListenerPosition", "/Data.Delay"}) {
        const auto array = (*file)->read(name);
        std::cout << name << ": ";
        if (!array) { std::cout << array.error() << "\n"; ++failures; continue; }
        for (auto d : array->shape) std::cout << d << " ";
        double sum = 0, abs_sum = 0;
        for (double v : array->values) { sum += v; abs_sum += std::fabs(v); }
        std::cout << " first=" << (array->values.empty() ? 0.0 : array->values[0]) << " n=" << array->values.size() << " sum=" << sum << " abs=" << abs_sum << "\n";
    }
    for (const char *attr : {"Conventions", "SOFAConventions", "DataType", "APIName"}) {
        const auto value = (*file)->string_attribute("/", attr);
        std::cout << attr << "=" << (value ? value->cpp_string() : "?") << "\n";
    }
    return failures == 0 ? 0 : 1;
}
