#include "Features.hpp"

namespace SFT::RHI {

FeatureSet &FeatureSet::set(Feature feature, bool enabled) noexcept {
            bits_.set(static_cast<usize>(feature), enabled);
            return *this;
        }

FeatureSet &FeatureSet::unset(Feature feature) noexcept {
            bits_.reset(static_cast<usize>(feature));
            return *this;
        }

[[nodiscard]] bool FeatureSet::has(Feature feature) const noexcept {
            return bits_.test(static_cast<usize>(feature));
        }

[[nodiscard]] bool FeatureSet::contains_all(const FeatureSet &required) const noexcept {
            return (required.bits_ & ~bits_).none();
        }

[[nodiscard]] FeatureSet FeatureSet::missing(const FeatureSet &required) const noexcept {
            FeatureSet result;
            result.bits_ = required.bits_ & ~bits_;
            return result;
        }

[[nodiscard]] FeatureSet FeatureSet::intersection(const FeatureSet &other) const noexcept {
            FeatureSet result;
            result.bits_ = bits_ & other.bits_;
            return result;
        }

[[nodiscard]] FeatureSet FeatureSet::difference(const FeatureSet &other) const noexcept {
            FeatureSet result;
            result.bits_ = bits_ & ~other.bits_;
            return result;
        }

[[nodiscard]] bool FeatureSet::any() const noexcept { return bits_.any(); }

[[nodiscard]] bool FeatureSet::none() const noexcept { return bits_.none(); }

[[nodiscard]] usize FeatureSet::count() const noexcept { return bits_.count(); }

FeatureSet &FeatureSet::operator|=(const FeatureSet &other) noexcept {
            bits_ |= other.bits_;
            return *this;
        }

[[nodiscard]] bool FeatureNegotiationReport::required_satisfied() const noexcept { return missing_required_features.none(); }

[[nodiscard]] bool FeatureNegotiationReport::optional_fully_enabled() const noexcept { return unavailable_optional_features.none(); }

[[nodiscard]] FeatureSet FeatureNegotiationReport::enabled_features() const noexcept { return enabled_required_features | enabled_optional_features; }

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

FeatureSet features_of(std::initializer_list<Feature> features) noexcept {
        FeatureSet set;
        for (Feature feature : features) {
            set.set(feature);
        }
        return set;
    }

} // namespace SFT::RHI
