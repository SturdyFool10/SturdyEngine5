#include <graphicsPlatform/GraphicsPlatform.hpp>

namespace SFT::GraphicsPlatform {

    QueryResult<std::vector<DisplayInfo>> query_displays() {
        return QueryResult<std::vector<DisplayInfo>>{
            .value = {},
            .message = QueryMessage{
                .status = QueryStatus::NotAvailable,
                .message = "Web HDR display enumeration is not implemented yet; future implementation should use browser APIs when standardized and available.",
            },
        };
    }

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

} // namespace SFT::GraphicsPlatform
