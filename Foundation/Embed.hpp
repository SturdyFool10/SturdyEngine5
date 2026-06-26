#pragma once

#include <string_view>

using std::string_view;

// Compile-time text embedding helper.
//
// Usage:
//     struct ShaderSource {
//         static constexpr string_view module_name = "triangle";
//
//         SFT_EMBED_TEXT_BEGIN(source)
// #embed SFT_EMBED_TEXT_FILE("Shaders/Triangle.slang")
//         SFT_EMBED_TEXT_END(source)
//     };
//
// C/C++ preprocessing does not allow a macro to expand into a new preprocessing directive, so the
// #embed line must remain a real directive. The macros around it provide the constexpr storage and
// string_view while preserving quotes, backslashes, newlines, NUL bytes, shader # lines, and empty
// files. SFT_HAS_EMBED(path) mirrors __has_embed and is only valid in #if/#elif expressions.

#if defined(__has_embed)
#define SFT_HAS_EMBED(path) (__has_embed(path) != __STDC_EMBED_NOT_FOUND__)
#else
#define SFT_HAS_EMBED(path) 0
#endif

#define SFT_EMBED_TEXT_FILE(path) path suffix(, )

#define SFT_DETAIL_EMBED_PRAGMA(value) _Pragma(#value)

#if defined(__clang__)
#define SFT_DETAIL_EMBED_DIAGNOSTIC_PUSH           \
    SFT_DETAIL_EMBED_PRAGMA(clang diagnostic push) \
    SFT_DETAIL_EMBED_PRAGMA(clang diagnostic ignored "-Wc23-extensions")
#define SFT_DETAIL_EMBED_DIAGNOSTIC_POP SFT_DETAIL_EMBED_PRAGMA(clang diagnostic pop)
#else
#define SFT_DETAIL_EMBED_DIAGNOSTIC_PUSH
#define SFT_DETAIL_EMBED_DIAGNOSTIC_POP
#endif

#define SFT_EMBED_TEXT_BEGIN(name)   \
    SFT_DETAIL_EMBED_DIAGNOSTIC_PUSH \
    [[maybe_unused]] static inline constexpr char name##_embedded_text_data[] = {

#define SFT_EMBED_TEXT_END(name)                                                                                                   \
    '\0'                                                                                                                           \
    }                                                                                                                              \
    ;                                                                                                                              \
    [[maybe_unused]] static inline constexpr ::string_view name{name##_embedded_text_data, sizeof(name##_embedded_text_data) - 1}; \
    SFT_DETAIL_EMBED_DIAGNOSTIC_POP

#define SFT_COMPILE_TIME_FILE(path) SFT_EMBED_TEXT_FILE(path)
#define SFT_COMPILE_TIME_FILE_BEGIN(name) SFT_EMBED_TEXT_BEGIN(name)
#define SFT_COMPILE_TIME_FILE_END(name) SFT_EMBED_TEXT_END(name)
