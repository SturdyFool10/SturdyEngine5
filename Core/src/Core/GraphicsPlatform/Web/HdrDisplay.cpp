#include <Foundation/Foundation.hpp>
#include <Core/GraphicsPlatform/GraphicsPlatform.hpp>

namespace SFT::Core::GraphicsPlatform {

    /// Queries displays from the active backend or runtime state.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::NotAvailable`.
    QueryResult<std::vector<DisplayInfo>> query_displays() {
        return QueryResult<std::vector<DisplayInfo>>{
            .value = {},
            .message = QueryMessage{
                .status = QueryStatus::NotAvailable,
                .message = "Web HDR display enumeration is not implemented yet; future implementation should use browser APIs when standardized and available.",
            },
        };
    }

    /// Queries HDR display capabilities from the active backend or runtime state.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::NotAvailable`.
    QueryResult<HdrDisplayCapabilities> query_hdr_display_capabilities(const NativeSurfaceHandle &surface) {
        (void)surface;
        return QueryResult<HdrDisplayCapabilities>{
            .value = HdrDisplayCapabilities{},
            .message = QueryMessage{
                .status = QueryStatus::NotAvailable,
                .message = "Web HDR display capability querying is not implemented yet; future implementation should use browser APIs when standardized and available.",
            },
        };
    }

} // namespace SFT::Core::GraphicsPlatform
