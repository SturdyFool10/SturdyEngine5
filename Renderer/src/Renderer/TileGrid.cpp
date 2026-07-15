#include "TileGrid.hpp"

namespace SFT::Renderer {

Core::GraphicsBackendError graphics_error_from_rhi(const RHI::RhiError &error, const char *operation) {
        Core::GraphicsBackendErrorCode code = Core::GraphicsBackendErrorCode::OperationFailed;
        switch (error.code) {
            case RHI::RhiErrorCode::Unsupported: code = Core::GraphicsBackendErrorCode::Unsupported; break;
            case RHI::RhiErrorCode::OutOfMemory: code = Core::GraphicsBackendErrorCode::OutOfMemory; break;
            case RHI::RhiErrorCode::DeviceLost: code = Core::GraphicsBackendErrorCode::DeviceLost; break;
            case RHI::RhiErrorCode::SurfaceLost: code = Core::GraphicsBackendErrorCode::SurfaceLost; break;
            case RHI::RhiErrorCode::InvalidArgument:
            case RHI::RhiErrorCode::NotReady:
            case RHI::RhiErrorCode::OperationFailed:
                code = Core::GraphicsBackendErrorCode::OperationFailed;
                break;
        }
        return Core::GraphicsBackendError{
            .code = code,
            .message = string(operation) + " failed through RHI: " + error.message,
        };
    }

u32 clamp_tile_size(u32 desired, const RHI::DeviceLimits &limits) noexcept {
        const u32 device_max = limits.max_texture_dimension_2d;
        if (device_max == 0 || desired < device_max) {
            return desired;
        }
        return device_max;
    }

[[nodiscard]] usize TileCoordHash::operator()(TileCoord coord) const noexcept {
            const u64 packed = (static_cast<u64>(static_cast<u32>(coord.x)) << 32) | static_cast<u32>(coord.y);
            u64 hashed = packed;
            hashed ^= hashed >> 33;
            hashed *= 0xff51afd7ed558ccdULL;
            hashed ^= hashed >> 33;
            hashed *= 0xc4ceb9fe1a85ec53ULL;
            hashed ^= hashed >> 33;
            return static_cast<usize>(hashed);
        }

TileAddress locate_in_grid(i32 logical_x, i32 logical_y, u32 tile_size) noexcept {
        const i32 size = static_cast<i32>(tile_size);
        const i32 tile_x = logical_x >= 0 ? logical_x / size : -(((-logical_x) + size - 1) / size);
        const i32 tile_y = logical_y >= 0 ? logical_y / size : -(((-logical_y) + size - 1) / size);
        return TileAddress{
            .tile = TileCoord{tile_x, tile_y},
            .local_x = static_cast<u32>(logical_x - tile_x * size),
            .local_y = static_cast<u32>(logical_y - tile_y * size),
        };
    }

vector<TileCoord> tiles_overlapping(i32 x, i32 y, u32 width, u32 height, u32 tile_size) noexcept {
        vector<TileCoord> result;
        if (width == 0 || height == 0) {
            return result;
        }
        const TileAddress min_addr = locate_in_grid(x, y, tile_size);
        const TileAddress max_addr = locate_in_grid(x + static_cast<i32>(width) - 1, y + static_cast<i32>(height) - 1, tile_size);
        result.reserve(static_cast<usize>(max_addr.tile.y - min_addr.tile.y + 1) *
                       static_cast<usize>(max_addr.tile.x - min_addr.tile.x + 1));
        for (i32 ty = min_addr.tile.y; ty <= max_addr.tile.y; ++ty) {
            for (i32 tx = min_addr.tile.x; tx <= max_addr.tile.x; ++tx) {
                result.push_back(TileCoord{tx, ty});
            }
        }
        return result;
    }

} // namespace SFT::Renderer
