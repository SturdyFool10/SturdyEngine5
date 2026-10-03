#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/// A small read-only HDF5 reader: enough of the format to load numeric datasets and string attributes from the files audio
/// work uses (SOFA, which is netCDF-4 over HDF5). Supported: superblock versions 0-3, object headers v1 and v2, old-style groups
/// (symbol tables) and new-style groups (link messages, dense link storage in a fractal heap), contiguous, compact and chunked
/// layouts (B-tree v1 index) with deflate, shuffle and fletcher32 filters, and integer/float element types of either byte order.
/// Every read is bounds-checked; damaged or unsupported files fail with a message.
namespace SFT::Audio {

    struct Hdf5Array {
        std::vector<u64> shape;
        std::vector<f64> values; ///< row-major, whatever the stored element type
        [[nodiscard]] usize element_count() const noexcept {
            usize n = 1;
            for (const u64 d : shape) n *= static_cast<usize>(d);
            return n;
        }
    };

    class Hdf5File {
      public:
        /// `file` must outlive the reader.
        [[nodiscard]] static std::expected<std::unique_ptr<Hdf5File>, UString> open(std::span<const std::byte> file);
        ~Hdf5File();

        /// Whether a dataset or group exists at `path` ("/Data.IR", "a/b").
        [[nodiscard]] bool exists(std::string_view path) const;
        /// A numeric dataset converted to doubles.
        [[nodiscard]] std::expected<Hdf5Array, UString> read(std::string_view path) const;
        /// A string attribute of the group or dataset at `path` ("/" for the root); nothing when absent or not a string.
        [[nodiscard]] std::optional<UString> string_attribute(std::string_view path, std::string_view name) const;
        /// A numeric scalar/array attribute.
        [[nodiscard]] std::optional<std::vector<f64>> number_attribute(std::string_view path, std::string_view name) const;

      private:
        struct Impl;
        explicit Hdf5File(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Audio
