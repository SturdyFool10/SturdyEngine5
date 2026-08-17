#include <RHI/src/RHI/Features.hpp>
#include "Features.hpp"

namespace SFT::RHI {

/// Performs the set operation for `RHI` using the supplied arguments.
///
/// @param feature `feature` value used by the operation.
/// @param enabled Whether the associated behavior is enabled.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
FeatureSet &FeatureSet::set(Feature feature, bool enabled) noexcept {
            bits_.set(static_cast<usize>(feature), enabled);
            return *this;
        }

/// Performs the unset operation for `RHI` using the supplied arguments.
///
/// @param feature `feature` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
FeatureSet &FeatureSet::unset(Feature feature) noexcept {
            bits_.reset(static_cast<usize>(feature));
            return *this;
        }

/// Performs the has operation for `RHI` using the supplied arguments.
///
/// @param feature `feature` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool FeatureSet::has(Feature feature) const noexcept {
            return bits_.test(static_cast<usize>(feature));
        }

/// Reports whether all holds for this `RHI`.
///
/// @param required `required` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool FeatureSet::contains_all(const FeatureSet &required) const noexcept {
            return (required.bits_ & ~bits_).none();
        }

/// Performs the missing operation for `RHI` using the supplied arguments.
///
/// @param required `required` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] FeatureSet FeatureSet::missing(const FeatureSet &required) const noexcept {
            FeatureSet result;
            result.bits_ = required.bits_ & ~bits_;
            return result;
        }

/// Performs the intersection operation for `RHI` using the supplied arguments.
///
/// @param other Other object used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] FeatureSet FeatureSet::intersection(const FeatureSet &other) const noexcept {
            FeatureSet result;
            result.bits_ = bits_ & other.bits_;
            return result;
        }

/// Performs the difference operation for `RHI` using the supplied arguments.
///
/// @param other Other object used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] FeatureSet FeatureSet::difference(const FeatureSet &other) const noexcept {
            FeatureSet result;
            result.bits_ = bits_ & ~other.bits_;
            return result;
        }

/// Returns the current or globally available any value.
///
/// @return Returns the current any value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool FeatureSet::any() const noexcept { return bits_.any(); }

/// Returns the current or globally available none value.
///
/// @return Returns the current none value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool FeatureSet::none() const noexcept { return bits_.none(); }

/// Returns the count for this `RHI`.
///
/// @return Returns the current count value.
/// @note This function does not throw exceptions.
[[nodiscard]] usize FeatureSet::count() const noexcept { return bits_.count(); }

/// Combines this object with the right-hand operand using bitwise OR.
///
/// @param other Other object used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
FeatureSet &FeatureSet::operator|=(const FeatureSet &other) noexcept {
            bits_ |= other.bits_;
            return *this;
        }

/// Returns the current or globally available required satisfied value.
///
/// @return Returns the current required satisfied value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool FeatureNegotiationReport::required_satisfied() const noexcept { return missing_required_features.none(); }

/// Returns the current or globally available optional fully enabled value.
///
/// @return Returns the current optional fully enabled value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool FeatureNegotiationReport::optional_fully_enabled() const noexcept { return unavailable_optional_features.none(); }

/// Returns the current or globally available enabled features value.
///
/// @return Returns the current enabled features value.
/// @note This function does not throw exceptions.
[[nodiscard]] FeatureSet FeatureNegotiationReport::enabled_features() const noexcept { return enabled_required_features | enabled_optional_features; }

/// Performs the negotiate features operation for `RHI` using the supplied arguments.
///
/// @param supported `supported` value used by the operation.
/// @param required `required` value used by the operation.
/// @param optional `optional` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
FeatureNegotiationReport negotiate_features(
        const FeatureSet &supported,
        const FeatureSet &required,
        const FeatureSet &optional) noexcept {
        FeatureNegotiationReport report{};
        report.supported_features = supported;
        report.requested_required_features = required;
        report.requested_optional_features = optional;
        report.enabled_required_features = required.intersection(supported);
        report.enabled_optional_features = optional.intersection(supported).difference(required);
        report.missing_required_features = supported.missing(required);
        report.unavailable_optional_features = supported.missing(optional).difference(required);
        return report;
    }

/// Performs the features of operation for `RHI` using the supplied arguments.
///
/// @param features `features` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
FeatureSet features_of(std::initializer_list<Feature> features) noexcept {
        FeatureSet set;
        for (Feature feature : features) {
            set.set(feature);
        }
        return set;
    }

} // namespace SFT::RHI

namespace SFT::RHI {

    /// Combines the operands with bitwise OR.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    FeatureSet operator|(FeatureSet a, const FeatureSet &b) noexcept {
        a |= b;
        return a;
    }

} // namespace SFT::RHI

