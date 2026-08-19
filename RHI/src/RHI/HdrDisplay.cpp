#include <RHI/HdrDisplay.hpp>

namespace SFT::RHI {

[[nodiscard]] PlatformQueryMessage::operator bool() const noexcept {
    return status == PlatformQueryStatus::Ok;
}

[[nodiscard]] SurfaceHdrCapabilityQuery::operator bool() const noexcept {
    return static_cast<bool>(message);
}

[[nodiscard]] DisplayQuery::operator bool() const noexcept {
    return static_cast<bool>(message);
}

} // namespace SFT::RHI
