#include <Foundation/src/Types.hpp>


namespace SFT::Foundation {

    void assert_type_assumptions() noexcept {
        assert(sizeof(i8) == 1);
        assert(sizeof(i16) == 2);
        assert(sizeof(i32) == 4);
        assert(sizeof(i64) == 8);

        assert(sizeof(u8) == 1);
        assert(sizeof(u16) == 2);
        assert(sizeof(u32) == 4);
        assert(sizeof(u64) == 8);

        assert(sizeof(f32) == 4);
        assert(sizeof(f64) == 8);

        assert(sizeof(byte) == 1);
        assert(sizeof(b8) == 1);

        assert(numeric_limits<f32>::is_iec559);
        assert(numeric_limits<f64>::is_iec559);
        assert(is_trivially_copyable_v<b8>);
        assert(is_standard_layout_v<b8>);
    }

} // namespace SFT::Foundation

