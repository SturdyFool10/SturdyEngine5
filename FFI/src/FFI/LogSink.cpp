/// C ABI implementation of the log-sink registration surface.
///
/// Thin: all the real work (the fanout sink itself, the callback registry) lives in
/// `Foundation::add_log_sink`/`remove_log_sink`. This layer only translates between the C
/// function-pointer-plus-`user_data` shape a foreign caller can express and the `std::function`
/// `Foundation::LogSinkCallback` shape the C++ API wants.

#include <Foundation/Foundation.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::set_error;

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_log_add_sink(SturdyLogCallback callback, void *user_data,
                                                  SturdyLogSink *out_sink) {
    return guarded([&]() -> SturdyResult {
        if (callback == nullptr || out_sink == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "callback and output pointer must not be null");
        }
        const SFT::Foundation::LogSinkId id = SFT::Foundation::add_log_sink(
            [callback, user_data](SFT::Foundation::LogLevel level, std::string_view message) {
                callback(static_cast<SturdyLogLevel>(level), message.data(), message.size(), user_data);
            });
        out_sink->id = id.value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_log_remove_sink(SturdyLogSink sink) {
    return guarded([&]() -> SturdyResult {
        SFT::Foundation::remove_log_sink(SFT::Foundation::LogSinkId{sink.id});
        return STURDY_OK;
    });
}

} // extern "C"
