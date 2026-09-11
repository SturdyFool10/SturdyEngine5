#include <Reflection/EnumInfo.hpp>

namespace SFT::Reflection {

    const EnumeratorInfo *EnumInfo::find_enumerator(std::string_view enumerator_name) const noexcept {
        for (const EnumeratorInfo &enumerator : enumerators) {
            if (enumerator.name.cpp_string_view() == enumerator_name) {
                return &enumerator;
            }
        }
        return nullptr;
    }

    const EnumeratorInfo *EnumInfo::find_enumerator(i64 value) const noexcept {
        for (const EnumeratorInfo &enumerator : enumerators) {
            if (enumerator.value == value) {
                return &enumerator;
            }
        }
        return nullptr;
    }

} // namespace SFT::Reflection
