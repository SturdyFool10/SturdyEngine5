// SEClay -- Sturdy Engine Clay
// VERSION: derived from Clay 0.14, modified
//
// THIS IS NOT THE ORIGINAL CLAY. SEClay is a derivative work of Clay (https://github.com/nicbarker/clay,
// Copyright (c) 2024 Nic Barker, zlib/libpng license), forked from tag v0.14 and developed inside Sturdy
// Engine, where it now lives with the rest of the UI (Renderer/src/Renderer/UI/SEClay/). It is a
// different, altered library: every public identifier carries the SEClay_/SECLAY_ prefix instead of
// Clay_/CLAY_ so it can never be mistaken for, or collide with, upstream Clay, and it grows features
// upstream does not have. Bugs in it are Sturdy Engine's, not Nic Barker's.
//
// The original license applies to this file and is reproduced in LICENSE.md next to it: clause 1 (do not
// claim to have written the original), clause 2 (altered versions must be plainly marked as such --
// this notice) and clause 3 (this notice, and the license, must not be removed or altered). Sturdy
// Engine's changes from upstream v0.14, all marked inline where they occur, are listed below.
//
// Sturdy Engine's own changes from upstream v0.14:
//   - scrollContainerDatas' fixed capacity of 10 (independent of SEClay_SetMaxElementCount) is now
//     scaled with maxElementCount, and grows further via SEClay__GrowArrayStorage (malloc + copy,
//     doubling) instead of silently overflowing when a caller has more than that many distinct
//     clipped elements alive in one frame.
//   - warnings' fixed capacity of 100 is likewise scaled with maxElementCount.
//   - A floating element's SCISSOR_START render command now carries its real clip-element id
//     (previously a throwaway derived id, per upstream's own TODO comment there) so a renderer can
//     resolve the correct ancestor-intersected scissor for content whose clip target is nested
//     inside further containers (a scroll area, a dock panel) that a floating root — appended after
//     the main layout tree finishes — otherwise has no way to know about.
//   - A clip element's SCISSOR_START is now always emitted, even when Clay's own offscreen-culling
//     heuristic judges that element offscreen (tested against the whole application window, not any
//     nearer ancestor's smaller visible region) — its matching SCISSOR_END was already unconditional,
//     so skipping SCISSOR_START left an unbalanced pair that corrupted scissor-stack state for every
//     sibling declared after it.
//
//   - Memory-safety hardening pass (bounds/overflow audit). Every change is marked "HARDENING" inline:
//       * Arena allocation is overflow-checked; a failed allocation yields a zero-capacity array (never a
//         NULL array with a non-zero capacity) and makes SEClay_Initialize return NULL.
//       * The warnings array was allocated with sizeof(SEClay_String) per item instead of
//         sizeof(SEClay__Warning) (half the needed size) -- a real arena overflow.
//       * Array accessors reject negative lengths/NULL storage, and the shared "_DEFAULT" sentinels handed
//         back on failure are re-zeroed first (callers write through them) and are now thread-local.
//       * SEClay__currentContext is thread-local, so contexts driven from different threads cannot clobber
//         each other's "current context".
//       * elementConfigs/wrappedTextLines/renderCommands get larger capacities (they legitimately hold
//         several entries per element), and a full array no longer inflates per-element slice lengths past
//         the real storage (previously an out-of-bounds read into neighbouring arena arrays).
//       * Wrapped-line construction bounds-checks measured-word offsets against the text and no longer
//         reads text[-1]; the measure cache verifies text length (hash-collision safety), no longer leaks
//         slots when a measure fails part way, derives its bucket count from the real table size, and
//         can reclaim stale items on demand.
//       * Unbalanced SEClay__CloseElement, negative stack lengths, >20 configs per element, unchecked DFS
//         pushes, dynamic string buffers and pointer-state clip lookups are all bounds-checked.
//       * SEClay_ReleaseContextResources() frees the heap storage a context may own.
//
// Do not silently pull a fresh copy of upstream clay.h over this file — these fixes are load-bearing for this
// engine's UI (real, reproduced correctness bugs, not speculative). Re-apply them (or re-verify they
// still apply) if upstream is ever re-merged.

/*
    NOTE: In order to use this library you must define
    the following macro in exactly one file, _before_ including clay.h:

    #define SECLAY_IMPLEMENTATION
    #include "clay.h"

    See the examples folder for details.
*/

#include <stdint.h>
#include <string_view>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// SIMD includes on supported platforms
#if !defined(SECLAY_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64))
#include <emmintrin.h>
#elif !defined(SECLAY_DISABLE_SIMD) && defined(__aarch64__)
#include <arm_neon.h>
#endif

// -----------------------------------------
// HEADER DECLARATIONS ---------------------
// -----------------------------------------

#ifndef SECLAY_HEADER
#define SECLAY_HEADER

#if !( \
    (defined(__cplusplus) && __cplusplus >= 202002L) || \
    (defined(__STDC__) && __STDC__ == 1 && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || \
    defined(_MSC_VER) \
)
#error "Clay requires C99, C++20, or MSVC"
#endif

#ifdef SECLAY_WASM
#define SECLAY_WASM_EXPORT(name) __attribute__((export_name(name)))
#else
#define SECLAY_WASM_EXPORT(null)
#endif

#ifdef SECLAY_DLL
#define SECLAY_DLL_EXPORT __declspec(dllexport) __stdcall
#else
#define SECLAY_DLL_EXPORT
#endif

// Public Macro API ------------------------

#define SECLAY__MAX(x, y) (((x) > (y)) ? (x) : (y))
#define SECLAY__MIN(x, y) (((x) < (y)) ? (x) : (y))

#define SECLAY_TEXT_CONFIG(...) SEClay__StoreTextElementConfig(SECLAY__CONFIG_WRAPPER(SEClay_TextElementConfig, __VA_ARGS__))

#define SECLAY_BORDER_OUTSIDE(widthValue) {widthValue, widthValue, widthValue, widthValue, 0}

#define SECLAY_BORDER_ALL(widthValue) {widthValue, widthValue, widthValue, widthValue, widthValue}

#define SECLAY_CORNER_RADIUS(radius) (SEClay_CornerRadius { radius, radius, radius, radius })

#define SECLAY_PADDING_ALL(padding) SECLAY__CONFIG_WRAPPER(SEClay_Padding, { padding, padding, padding, padding })





// Note: If a compile error led you here, you might be trying to use SECLAY_ID with something other than a string literal. To construct an ID with a dynamic string, use SECLAY_SID instead.
#define SECLAY_ID(label) SECLAY_IDI(label, 0)

#define SECLAY_SID(label) SECLAY_SIDI(label, 0)

// Note: If a compile error led you here, you might be trying to use SECLAY_IDI with something other than a string literal. To construct an ID with a dynamic string, use SECLAY_SIDI instead.
#define SECLAY_IDI(label, index) SECLAY_SIDI(SECLAY_STRING(label), index)

#define SECLAY_SIDI(label, index) SEClay__HashString(label, index, 0)

// Note: If a compile error led you here, you might be trying to use SECLAY_ID_LOCAL with something other than a string literal. To construct an ID with a dynamic string, use SECLAY_SID_LOCAL instead.
#define SECLAY_ID_LOCAL(label) SECLAY_IDI_LOCAL(label, 0)

#define SECLAY_SID_LOCAL(label) SECLAY_SIDI_LOCAL(label, 0)

// Note: If a compile error led you here, you might be trying to use SECLAY_IDI_LOCAL with something other than a string literal. To construct an ID with a dynamic string, use SECLAY_SIDI_LOCAL instead.
#define SECLAY_IDI_LOCAL(label, index) SECLAY_SIDI_LOCAL(SECLAY_STRING(label), index)

#define SECLAY_SIDI_LOCAL(label, index) SEClay__HashString(label, index, SEClay__GetParentElementId())

#define SECLAY__STRING_LENGTH(s) ((sizeof(s) / sizeof((s)[0])) - sizeof((s)[0]))

#define SECLAY__ENSURE_STRING_LITERAL(x) ("" x "")

// Note: If an error led you here, it's because SECLAY_STRING can only be used with string literals, i.e. SECLAY_STRING("SomeString") and not SECLAY_STRING(yourString)
#define SECLAY_STRING(string) (SEClay_String { .isStaticallyAllocated = true, .length = SECLAY__STRING_LENGTH(SECLAY__ENSURE_STRING_LITERAL(string)), .chars = (string) })

#define SECLAY_STRING_CONST(string) { .isStaticallyAllocated = true, .length = SECLAY__STRING_LENGTH(SECLAY__ENSURE_STRING_LITERAL(string)), .chars = (string) }

// Publicly visible layout element macros -----------------------------------------------------

/* This macro looks scary on the surface, but is actually quite simple.
  It turns a macro call like this:

  SECLAY({
    .id = SECLAY_ID("Container"),
    .backgroundColor = { 255, 200, 200, 255 }
  }) {
      ...children declared here
  }

  Into calls like this:

  SEClay_OpenElement();
  SEClay_ConfigureOpenElement((SEClay_ElementDeclaration) {
    .id = SECLAY_ID("Container"),
    .backgroundColor = { 255, 200, 200, 255 }
  });
  ...children declared here
  SEClay_CloseElement();

  The RAII scope object (SEClay_ElementScope) closes the element after the body - where the children are declared - so you cannot forget
  to call SEClay_CloseElement().
*/
#define SECLAY__CAT_(a, b) a##b
#define SECLAY__CAT(a, b) SECLAY__CAT_(a, b)
// `SECLAY({ ... }) { children }` opens an element, runs the block, and closes it again: an `SEClay_ElementScope` (RAII) lives for exactly the block,
// so the close cannot be forgotten, and it also runs if the block is left early or by an exception.
#define SECLAY(...)                                                                                                                              \
    if (SEClay_ElementScope SECLAY__CAT(seclayScope_, __COUNTER__){SECLAY__CONFIG_WRAPPER(SEClay_ElementDeclaration, __VA_ARGS__)}; true)

// These macros exist to allow the SECLAY() macro to be called both with an inline struct definition, such as
// SECLAY({ .id = something... });
// As well as by passing a predefined declaration struct
// SEClay_ElementDeclaration declarationStruct = ...
// SECLAY(declarationStruct);
#define SECLAY__WRAPPER_TYPE(type) SEClay__##type##Wrapper
#define SECLAY__WRAPPER_STRUCT(type) typedef struct { type wrapped; } SECLAY__WRAPPER_TYPE(type)
#define SECLAY__CONFIG_WRAPPER(type, ...) (SECLAY__WRAPPER_TYPE(type) { __VA_ARGS__ }).wrapped

#define SECLAY_TEXT(text, textConfig) SEClay__OpenTextElement(text, textConfig)

// SEClay is C++-only: no `(type)` cast spelling for compound literals, no packed-enum macro (the enums are `enum class : uint8_t`).
#define SECLAY__DEFAULT_STRUCT {}

#ifdef __cplusplus
extern "C" {
#endif

// Utility Structs -------------------------

// Note: SEClay_String is not guaranteed to be null terminated. It may be if created from a literal C string,
// but it is also used to represent slices.
typedef struct SEClay_String {
    // Set this boolean to true if the char* data underlying this string will live for the entire lifetime of the program.
    // This will automatically be set for strings created with SECLAY_STRING, as the macro requires a string literal.
    bool isStaticallyAllocated;
    int32_t length;
    // The underlying character memory. Note: this will not be copied and will not extend the lifetime of the underlying memory.
    const char *chars;

    // Type-safe construction and access (C++): a view in, a view out, with the length checked against the int32 field.
    // `is_static` promises the characters live for the whole program (a string literal); otherwise SEClay copies what it keeps.
    [[nodiscard]] static constexpr SEClay_String from_view(std::string_view text, bool is_static = false) noexcept {
        return SEClay_String{.isStaticallyAllocated = is_static,
                             .length = text.size() > static_cast<size_t>(INT32_MAX) ? INT32_MAX : static_cast<int32_t>(text.size()),
                             .chars = text.data()};
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return chars == nullptr || length <= 0 ? std::string_view{} : std::string_view{chars, static_cast<size_t>(length)};
    }
} SEClay_String;

// SEClay_StringSlice is used to represent non owning string slices, and includes
// a baseChars field which points to the string this slice is derived from.
typedef struct SEClay_StringSlice {
    int32_t length;
    const char *chars;
    const char *baseChars; // The source string / char* that this slice was derived from
} SEClay_StringSlice;

typedef struct SEClay_Context SEClay_Context;

// SEClay_Arena is a memory arena structure that is used by clay to manage its internal allocations.
// Rather than creating it by hand, it's easier to use SEClay_CreateArenaWithCapacityAndMemory()
typedef struct SEClay_Arena {
    uintptr_t nextAllocation;
    size_t capacity;
    char *memory;
} SEClay_Arena;

typedef struct SEClay_Dimensions {
    float width, height;
} SEClay_Dimensions;

typedef struct SEClay_Vector2 {
    float x, y;
} SEClay_Vector2;

// Internally clay conventionally represents colors as 0-255, but interpretation is up to the renderer.
typedef struct SEClay_Color {
    float r, g, b, a;
} SEClay_Color;

typedef struct SEClay_BoundingBox {
    float x, y, width, height;
} SEClay_BoundingBox;

// Primarily created via the SECLAY_ID(), SECLAY_IDI(), SECLAY_ID_LOCAL() and SECLAY_IDI_LOCAL() macros.
// Represents a hashed string ID used for identifying and finding specific clay UI elements, required
// by functions such as SEClay_PointerOver() and SEClay_GetElementData().
typedef struct SEClay_ElementId {
    uint32_t id; // The resulting hash generated from the other fields.
    uint32_t offset; // A numerical offset applied after computing the hash from stringId.
    uint32_t baseId; // A base hash value to start from, for example the parent element ID is used when calculating SECLAY_ID_LOCAL().
    SEClay_String stringId; // The string id to hash.
} SEClay_ElementId;

// A sized array of SEClay_ElementId.
typedef struct
{
    int32_t capacity;
    int32_t length;
    SEClay_ElementId *internalArray;
} SEClay_ElementIdArray;

// Controls the "radius", or corner rounding of elements, including rectangles, borders and images.
// The rounding is determined by drawing a circle inset into the element corner by (radius, radius) pixels.
//
// SEClay addition (not in upstream Clay): `shapeCodes` carries the CSS `corner-shape` of each corner, one byte per corner
// (bits 0-7 topLeft, 8-15 topRight, 16-23 bottomLeft, 24-31 bottomRight). SEClay does not interpret the bytes; they ride along
// with the radius into the RECTANGLE / BORDER / IMAGE render data for the renderer to shape the corner with. Zero in every byte
// means "round" (a circular arc, upstream Clay's only corner), so a zero-initialised or SECLAY_CORNER_RADIUS()-built value
// behaves exactly like upstream. The engine packs and unpacks the bytes (Renderer/UI/Style.hpp, pack_corner_shapes).
typedef struct SEClay_CornerRadius {
    float topLeft;
    float topRight;
    float bottomLeft;
    float bottomRight;
    uint32_t shapeCodes;
} SEClay_CornerRadius;

// Element Configs ---------------------------

// Controls the direction in which child elements will be automatically laid out.
enum class SEClay_LayoutDirection : uint8_t {
    // (Default) Lays out child elements from left to right with increasing x.
    LeftToRight,
    // Lays out child elements from top to bottom with increasing y.
    TopToBottom,
};

// Controls the alignment along the x axis (horizontal) of child elements.
enum class SEClay_LayoutAlignmentX : uint8_t {
    // (Default) Aligns child elements to the left hand side of this element, offset by padding.width.left
    Left,
    // Aligns child elements to the right hand side of this element, offset by padding.width.right
    Right,
    // Aligns child elements horizontally to the center of this element
    Center,
};

// Controls the alignment along the y axis (vertical) of child elements.
enum class SEClay_LayoutAlignmentY : uint8_t {
    // (Default) Aligns child elements to the top of this element, offset by padding.width.top
    Top,
    // Aligns child elements to the bottom of this element, offset by padding.width.bottom
    Bottom,
    // Aligns child elements vertically to the center of this element
    Center,
};

// Controls how the element takes up space inside its parent container.
enum class SEClay__SizingType : uint8_t {
    // (default) Wraps tightly to the size of the element's contents.
    Fit,
    // Expands along this axis to fill available space in the parent element, sharing it with other GROW elements.
    Grow,
    // Expects 0-1 range. Clamps the axis size to a percent of the parent container's axis size minus padding and child gaps.
    Percent,
    // Clamps the axis size to an exact size in pixels.
    Fixed,
};

// Controls how child elements are aligned on each axis.
typedef struct SEClay_ChildAlignment {
    SEClay_LayoutAlignmentX x; // Controls alignment of children along the x axis.
    SEClay_LayoutAlignmentY y; // Controls alignment of children along the y axis.
} SEClay_ChildAlignment;

// Controls the minimum and maximum size in pixels that this element is allowed to grow or shrink to,
// overriding sizing types such as FIT or GROW.
typedef struct SEClay_SizingMinMax {
    float min; // The smallest final size of the element on this axis will be this value in pixels.
    float max; // The largest final size of the element on this axis will be this value in pixels.
} SEClay_SizingMinMax;

// Controls the sizing of this element along one axis inside its parent container.
typedef struct SEClay_SizingAxis {
    union {
        SEClay_SizingMinMax minMax; // Controls the minimum and maximum size in pixels that this element is allowed to grow or shrink to, overriding sizing types such as FIT or GROW.
        float percent; // Expects 0-1 range. Clamps the axis size to a percent of the parent container's axis size minus padding and child gaps.
    } size;
    SEClay__SizingType type; // Controls how the element takes up space inside its parent container.
} SEClay_SizingAxis;

// Sizing helpers: fit to content, grow to fill, a fixed size, or a fraction (0-1) of the parent. `min`/`max` clamp Fit and Grow
// (0 for either means no limit).
[[nodiscard]] constexpr SEClay_SizingAxis SEClay_SizingFit(float min = 0, float max = 0) noexcept {
    return SEClay_SizingAxis{.size = {.minMax = {min, max}}, .type = SEClay__SizingType::Fit};
}
[[nodiscard]] constexpr SEClay_SizingAxis SEClay_SizingGrow(float min = 0, float max = 0) noexcept {
    return SEClay_SizingAxis{.size = {.minMax = {min, max}}, .type = SEClay__SizingType::Grow};
}
[[nodiscard]] constexpr SEClay_SizingAxis SEClay_SizingFixed(float size) noexcept {
    return SEClay_SizingAxis{.size = {.minMax = {size, size}}, .type = SEClay__SizingType::Fixed};
}
[[nodiscard]] constexpr SEClay_SizingAxis SEClay_SizingPercent(float fractionOfParent) noexcept {
    return SEClay_SizingAxis{.size = {.percent = fractionOfParent}, .type = SEClay__SizingType::Percent};
}

// Controls the sizing of this element along one axis inside its parent container.
typedef struct SEClay_Sizing {
    SEClay_SizingAxis width; // Controls the width sizing of the element, along the x axis.
    SEClay_SizingAxis height;  // Controls the height sizing of the element, along the y axis.
} SEClay_Sizing;

// Controls "padding" in pixels, which is a gap between the bounding box of this element and where its children
// will be placed.
typedef struct SEClay_Padding {
    uint16_t left;
    uint16_t right;
    uint16_t top;
    uint16_t bottom;
} SEClay_Padding;

SECLAY__WRAPPER_STRUCT(SEClay_Padding);

// Controls various settings that affect the size and position of an element, as well as the sizes and positions
// of any child elements.
typedef struct SEClay_LayoutConfig {
    SEClay_Sizing sizing; // Controls the sizing of this element inside it's parent container, including FIT, GROW, PERCENT and FIXED sizing.
    SEClay_Padding padding; // Controls "padding" in pixels, which is a gap between the bounding box of this element and where its children will be placed.
    uint16_t childGap; // Controls the gap in pixels between child elements along the layout axis (horizontal gap for LEFT_TO_RIGHT, vertical gap for TOP_TO_BOTTOM).
    SEClay_ChildAlignment childAlignment; // Controls how child elements are aligned on each axis.
    SEClay_LayoutDirection layoutDirection; // Controls the direction in which child elements will be automatically laid out.
} SEClay_LayoutConfig;

SECLAY__WRAPPER_STRUCT(SEClay_LayoutConfig);

extern SEClay_LayoutConfig SECLAY_LAYOUT_DEFAULT;

// Controls how text "wraps", that is how it is broken into multiple lines when there is insufficient horizontal space.
enum class SEClay_TextElementConfigWrapMode : uint8_t {
    // (default) breaks on whitespace characters.
    Words,
    // Don't break on space characters, only on newlines.
    Newlines,
    // Disable text wrapping entirely.
    None,
};

// Controls how wrapped lines of text are horizontally aligned within the outer text bounding box.
enum class SEClay_TextAlignment : uint8_t {
    // (default) Horizontally aligns wrapped lines of text to the left hand side of their bounding box.
    Left,
    // Horizontally aligns wrapped lines of text to the center of their bounding box.
    Center,
    // Horizontally aligns wrapped lines of text to the right hand side of their bounding box.
    Right,
};

// Controls various functionality related to text elements.
typedef struct SEClay_TextElementConfig {
    // A pointer that will be transparently passed through to the resulting render command.
    void *userData;
    // The RGBA color of the font to render, conventionally specified as 0-255.
    SEClay_Color textColor;
    // An integer transparently passed to SEClay_MeasureText to identify the font to use.
    // The debug view will pass fontId = 0 for its internal text.
    uint16_t fontId;
    // Controls the size of the font. Handled by the function provided to SEClay_MeasureText.
    uint16_t fontSize;
    // Controls extra horizontal spacing between characters. Handled by the function provided to SEClay_MeasureText.
    uint16_t letterSpacing;
    // Controls additional vertical space between wrapped lines of text.
    uint16_t lineHeight;
    // Controls how text "wraps", that is how it is broken into multiple lines when there is insufficient horizontal space.
    // SEClay_TextElementConfigWrapMode::Words (default) breaks on whitespace characters.
    // SEClay_TextElementConfigWrapMode::Newlines doesn't break on space characters, only on newlines.
    // SEClay_TextElementConfigWrapMode::None disables wrapping entirely.
    SEClay_TextElementConfigWrapMode wrapMode;
    // Controls how wrapped lines of text are horizontally aligned within the outer text bounding box.
    // SEClay_TextAlignment::Left (default) - Horizontally aligns wrapped lines of text to the left hand side of their bounding box.
    // SEClay_TextAlignment::Center - Horizontally aligns wrapped lines of text to the center of their bounding box.
    // SEClay_TextAlignment::Right - Horizontally aligns wrapped lines of text to the right hand side of their bounding box.
    SEClay_TextAlignment textAlignment;
} SEClay_TextElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_TextElementConfig);

// Aspect Ratio --------------------------------

// Controls various settings related to aspect ratio scaling element.
typedef struct SEClay_AspectRatioElementConfig {
    float aspectRatio; // A float representing the target "Aspect ratio" for an element, which is its final width divided by its final height.
} SEClay_AspectRatioElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_AspectRatioElementConfig);

// Image --------------------------------

// Controls various settings related to image elements.
typedef struct SEClay_ImageElementConfig {
    void* imageData; // A transparent pointer used to pass image data through to the renderer.
} SEClay_ImageElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_ImageElementConfig);

// Floating -----------------------------

// Controls where a floating element is offset relative to its parent element.
// Note: see https://github.com/user-attachments/assets/b8c6dfaa-c1b1-41a4-be55-013473e4a6ce for a visual explanation.
enum class SEClay_FloatingAttachPointType : uint8_t {
    LeftTop,
    LeftCenter,
    LeftBottom,
    CenterTop,
    CenterCenter,
    CenterBottom,
    RightTop,
    RightCenter,
    RightBottom,
};

// Controls where a floating element is offset relative to its parent element.
typedef struct SEClay_FloatingAttachPoints {
    SEClay_FloatingAttachPointType element; // Controls the origin point on a floating element that attaches to its parent.
    SEClay_FloatingAttachPointType parent; // Controls the origin point on the parent element that the floating element attaches to.
} SEClay_FloatingAttachPoints;

// Controls how mouse pointer events like hover and click are captured or passed through to elements underneath a floating element.
enum class SEClay_PointerCaptureMode : uint8_t {
    // (default) "Capture" the pointer event and don't allow events like hover and click to pass through to elements underneath.
    Capture,
    //    SECLAY_POINTER_CAPTURE_MODE_PARENT, TODO pass pointer through to attached parent

    // Transparently pass through pointer events like hover and click to elements underneath the floating element.
    Passthrough,
};

// Controls which element a floating element is "attached" to (i.e. relative offset from).
enum class SEClay_FloatingAttachToElement : uint8_t {
    // (default) Disables floating for this element.
    None,
    // Attaches this floating element to its parent, positioned based on the .attachPoints and .offset fields.
    Parent,
    // Attaches this floating element to an element with a specific ID, specified with the .parentId field. positioned based on the .attachPoints and .offset fields.
    ElementWithId,
    // Attaches this floating element to the root of the layout, which combined with the .offset field provides functionality similar to "absolute positioning".
    Root,
};

// Controls whether or not a floating element is clipped to the same clipping rectangle as the element it's attached to.
enum class SEClay_FloatingClipToElement : uint8_t {
    // (default) - The floating element does not inherit clipping.
    None,
    // The floating element is clipped to the same clipping rectangle as the element it's attached to.
    AttachedParent
};

// Controls various settings related to "floating" elements, which are elements that "float" above other elements, potentially overlapping their boundaries,
// and not affecting the layout of sibling or parent elements.
typedef struct SEClay_FloatingElementConfig {
    // Offsets this floating element by the provided x,y coordinates from its attachPoints.
    SEClay_Vector2 offset;
    // Expands the boundaries of the outer floating element without affecting its children.
    SEClay_Dimensions expand;
    // When used in conjunction with .attachTo = SEClay_FloatingAttachToElement::ElementWithId, attaches this floating element to the element in the hierarchy with the provided ID.
    // Hint: attach the ID to the other element with .id = SECLAY_ID("yourId"), and specify the id the same way, with .parentId = SECLAY_ID("yourId").id
    uint32_t parentId;
    // Controls the z index of this floating element and all its children. Floating elements are sorted in ascending z order before output.
    // zIndex is also passed to the renderer for all elements contained within this floating element.
    int16_t zIndex;
    // Controls how mouse pointer events like hover and click are captured or passed through to elements underneath / behind a floating element.
    // Enum is of the form SECLAY_ATTACH_POINT_foo_bar. See SEClay_FloatingAttachPoints for more details.
    // Note: see <img src="https://github.com/user-attachments/assets/b8c6dfaa-c1b1-41a4-be55-013473e4a6ce />
    // and <img src="https://github.com/user-attachments/assets/ebe75e0d-1904-46b0-982d-418f929d1516 /> for a visual explanation.
    SEClay_FloatingAttachPoints attachPoints;
    // Controls how mouse pointer events like hover and click are captured or passed through to elements underneath a floating element.
    // SEClay_PointerCaptureMode::Capture (default) - "Capture" the pointer event and don't allow events like hover and click to pass through to elements underneath.
    // SEClay_PointerCaptureMode::Passthrough - Transparently pass through pointer events like hover and click to elements underneath the floating element.
    SEClay_PointerCaptureMode pointerCaptureMode;
    // Controls which element a floating element is "attached" to (i.e. relative offset from).
    // SEClay_FloatingAttachToElement::None (default) - Disables floating for this element.
    // SEClay_FloatingAttachToElement::Parent - Attaches this floating element to its parent, positioned based on the .attachPoints and .offset fields.
    // SEClay_FloatingAttachToElement::ElementWithId - Attaches this floating element to an element with a specific ID, specified with the .parentId field. positioned based on the .attachPoints and .offset fields.
    // SEClay_FloatingAttachToElement::Root - Attaches this floating element to the root of the layout, which combined with the .offset field provides functionality similar to "absolute positioning".
    SEClay_FloatingAttachToElement attachTo;
    // Controls whether or not a floating element is clipped to the same clipping rectangle as the element it's attached to.
    // SEClay_FloatingClipToElement::None (default) - The floating element does not inherit clipping.
    // SEClay_FloatingClipToElement::AttachedParent - The floating element is clipped to the same clipping rectangle as the element it's attached to.
    SEClay_FloatingClipToElement clipTo;
} SEClay_FloatingElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_FloatingElementConfig);

// Custom -----------------------------

// Controls various settings related to custom elements.
typedef struct SEClay_CustomElementConfig {
    // A transparent pointer through which you can pass custom data to the renderer.
    // Generates CUSTOM render commands.
    void* customData;
    // SEClay addition (not in upstream Clay): a transparent pointer describing an *effect* applied to this element as a whole --
    // the element's own rectangle/border/image/text AND everything inside it. Generates a balanced pair of render commands around
    // the element's content: EFFECT_START before the first command the element or any descendant emits, and EFFECT_END after the
    // last one (after its borders and its SCISSOR_END). The pair is emitted even when the element is judged offscreen, so a
    // renderer can always rely on it nesting properly with other EFFECT and SCISSOR pairs. Effects may be nested. SEClay never
    // dereferences the pointer; both commands carry it in renderData.effect.effectData.
    void* effectData;
} SEClay_CustomElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_CustomElementConfig);

// Scroll -----------------------------

// Controls the axis on which an element switches to "scrolling", which clips the contents and allows scrolling in that direction.
typedef struct SEClay_ClipElementConfig {
    bool horizontal; // Clip overflowing elements on the X axis.
    bool vertical; // Clip overflowing elements on the Y axis.
    SEClay_Vector2 childOffset; // Offsets the x,y positions of all child elements. Used primarily for scrolling containers.
} SEClay_ClipElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_ClipElementConfig);

// Border -----------------------------

// Controls the widths of individual element borders.
typedef struct SEClay_BorderWidth {
    uint16_t left;
    uint16_t right;
    uint16_t top;
    uint16_t bottom;
    // Creates borders between each child element, depending on the .layoutDirection.
    // e.g. for LEFT_TO_RIGHT, borders will be vertical lines, and for TOP_TO_BOTTOM borders will be horizontal lines.
    // .betweenChildren borders will result in individual RECTANGLE render commands being generated.
    uint16_t betweenChildren;
} SEClay_BorderWidth;

// Controls settings related to element borders.
typedef struct SEClay_BorderElementConfig {
    SEClay_Color color; // Controls the color of all borders with width > 0. Conventionally represented as 0-255, but interpretation is up to the renderer.
    SEClay_BorderWidth width; // Controls the widths of individual borders. At least one of these should be > 0 for a BORDER render command to be generated.
    const void *edgeData; // SEClay addition: opaque pointer handed through to the Border render command (per-edge colours and styles in the host).
} SEClay_BorderElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_BorderElementConfig);

// Render Command Data -----------------------------

// Render command data when commandType == SEClay_RenderCommandType::Text
typedef struct SEClay_TextRenderData {
    // A string slice containing the text to be rendered.
    // Note: this is not guaranteed to be null terminated.
    SEClay_StringSlice stringContents;
    // Conventionally represented as 0-255 for each channel, but interpretation is up to the renderer.
    SEClay_Color textColor;
    // An integer representing the font to use to render this text, transparently passed through from the text declaration.
    uint16_t fontId;
    uint16_t fontSize;
    // Specifies the extra whitespace gap in pixels between each character.
    uint16_t letterSpacing;
    // The height of the bounding box for this line of text.
    uint16_t lineHeight;
} SEClay_TextRenderData;

// Render command data when commandType == SEClay_RenderCommandType::Rectangle
typedef struct SEClay_RectangleRenderData {
    // The solid background color to fill this rectangle with. Conventionally represented as 0-255 for each channel, but interpretation is up to the renderer.
    SEClay_Color backgroundColor;
    // Controls the "radius", or corner rounding of elements, including rectangles, borders and images.
    // The rounding is determined by drawing a circle inset into the element corner by (radius, radius) pixels.
    SEClay_CornerRadius cornerRadius;
} SEClay_RectangleRenderData;

// Render command data when commandType == SEClay_RenderCommandType::Image
typedef struct SEClay_ImageRenderData {
    // The tint color for this image. Note that the default value is 0,0,0,0 and should likely be interpreted
    // as "untinted".
    // Conventionally represented as 0-255 for each channel, but interpretation is up to the renderer.
    SEClay_Color backgroundColor;
    // Controls the "radius", or corner rounding of this image.
    // The rounding is determined by drawing a circle inset into the element corner by (radius, radius) pixels.
    SEClay_CornerRadius cornerRadius;
    // A pointer transparently passed through from the original element definition, typically used to represent image data.
    void* imageData;
} SEClay_ImageRenderData;

// Render command data when commandType == SEClay_RenderCommandType::Custom
typedef struct SEClay_CustomRenderData {
    // Passed through from .backgroundColor in the original element declaration.
    // Conventionally represented as 0-255 for each channel, but interpretation is up to the renderer.
    SEClay_Color backgroundColor;
    // Controls the "radius", or corner rounding of this custom element.
    // The rounding is determined by drawing a circle inset into the element corner by (radius, radius) pixels.
    SEClay_CornerRadius cornerRadius;
    // A pointer transparently passed through from the original element definition.
    void* customData;
} SEClay_CustomRenderData;

// Render command data when commandType == SEClay_RenderCommandType::ScissorStart || commandType == SEClay_RenderCommandType::ScissorEnd
typedef struct SEClay_ScrollRenderData {
    bool horizontal;
    bool vertical;
} SEClay_ClipRenderData;

// Render command data when commandType == SEClay_RenderCommandType::Border
typedef struct SEClay_BorderRenderData {
    // Controls a shared color for all this element's borders.
    // Conventionally represented as 0-255 for each channel, but interpretation is up to the renderer.
    SEClay_Color color;
    // Specifies the "radius", or corner rounding of this border element.
    // The rounding is determined by drawing a circle inset into the element corner by (radius, radius) pixels.
    SEClay_CornerRadius cornerRadius;
    // Controls individual border side widths.
    SEClay_BorderWidth width;
    // SEClay addition: `SEClay_BorderElementConfig::edgeData` of the element, unchanged.
    const void *edgeData;
} SEClay_BorderRenderData;

// Render command data when commandType == SEClay_RenderCommandType::EffectStart || commandType == SEClay_RenderCommandType::EffectEnd
// (SEClay addition, see SEClay_CustomElementConfig::effectData)
typedef struct SEClay_EffectRenderData {
    // The pointer from the element declaration's .custom.effectData; the same value on the START and its matching END.
    void* effectData;
} SEClay_EffectRenderData;

// A struct union containing data specific to this command's .commandType
typedef union SEClay_RenderData {
    // Render command data when commandType == SEClay_RenderCommandType::Rectangle
    SEClay_RectangleRenderData rectangle;
    // Render command data when commandType == SEClay_RenderCommandType::Text
    SEClay_TextRenderData text;
    // Render command data when commandType == SEClay_RenderCommandType::Image
    SEClay_ImageRenderData image;
    // Render command data when commandType == SEClay_RenderCommandType::Custom
    SEClay_CustomRenderData custom;
    // Render command data when commandType == SEClay_RenderCommandType::Border
    SEClay_BorderRenderData border;
    // Render command data when commandType == SEClay_RenderCommandType::ScissorStart|END
    SEClay_ClipRenderData clip;
    // Render command data when commandType == SEClay_RenderCommandType::EffectStart|END
    SEClay_EffectRenderData effect;
} SEClay_RenderData;

// Miscellaneous Structs & Enums ---------------------------------

// Data representing the current internal state of a scrolling element.
typedef struct SEClay_ScrollContainerData {
    // Note: This is a pointer to the real internal scroll position, mutating it may cause a change in final layout.
    // Intended for use with external functionality that modifies scroll position, such as scroll bars or auto scrolling.
    SEClay_Vector2 *scrollPosition;
    // The bounding box of the scroll element.
    SEClay_Dimensions scrollContainerDimensions;
    // The outer dimensions of the inner scroll container content, including the padding of the parent scroll container.
    SEClay_Dimensions contentDimensions;
    // The config that was originally passed to the clip element.
    SEClay_ClipElementConfig config;
    // Indicates whether an actual scroll container matched the provided ID or if the default struct was returned.
    bool found;
} SEClay_ScrollContainerData;

// Bounding box and other data for a specific UI element.
typedef struct SEClay_ElementData {
    // The rectangle that encloses this UI element, with the position relative to the root of the layout.
    SEClay_BoundingBox boundingBox;
    // Indicates whether an actual Element matched the provided ID or if the default struct was returned.
    bool found;
} SEClay_ElementData;

// Used by renderers to determine specific handling for each render command.
enum class SEClay_RenderCommandType : uint8_t {
    // This command type should be skipped.
    None,
    // The renderer should draw a solid color rectangle.
    Rectangle,
    // The renderer should draw a colored border inset into the bounding box.
    Border,
    // The renderer should draw text.
    Text,
    // The renderer should draw an image.
    Image,
    // The renderer should begin clipping all future draw commands, only rendering content that falls within the provided boundingBox.
    ScissorStart,
    // The renderer should finish any previously active clipping, and begin rendering elements in full again.
    ScissorEnd,
    // The renderer should provide a custom implementation for handling this render command based on its .customData
    Custom,
    // SEClay addition: every command up to the matching EFFECT_END belongs to one effect group (see SEClay_CustomElementConfig::effectData).
    EffectStart,
    // SEClay addition: closes the innermost open EFFECT_START.
    EffectEnd,
};

typedef struct SEClay_RenderCommand {
    // A rectangular box that fully encloses this UI element, with the position relative to the root of the layout.
    SEClay_BoundingBox boundingBox;
    // A struct union containing data specific to this command's commandType.
    SEClay_RenderData renderData;
    // A pointer transparently passed through from the original element declaration.
    void *userData;
    // The id of this element, transparently passed through from the original element declaration.
    uint32_t id;
    // The z order required for drawing this command correctly.
    // Note: the render command array is already sorted in ascending order, and will produce correct results if drawn in naive order.
    // This field is intended for use in batching renderers for improved performance.
    int16_t zIndex;
    // Specifies how to handle rendering of this command.
    // SEClay_RenderCommandType::Rectangle - The renderer should draw a solid color rectangle.
    // SEClay_RenderCommandType::Border - The renderer should draw a colored border inset into the bounding box.
    // SEClay_RenderCommandType::Text - The renderer should draw text.
    // SEClay_RenderCommandType::Image - The renderer should draw an image.
    // SEClay_RenderCommandType::ScissorStart - The renderer should begin clipping all future draw commands, only rendering content that falls within the provided boundingBox.
    // SEClay_RenderCommandType::ScissorEnd - The renderer should finish any previously active clipping, and begin rendering elements in full again.
    // SEClay_RenderCommandType::Custom - The renderer should provide a custom implementation for handling this render command based on its .customData
    SEClay_RenderCommandType commandType;
} SEClay_RenderCommand;

// A sized array of render commands.
typedef struct SEClay_RenderCommandArray {
    // The underlying max capacity of the array, not necessarily all initialized.
    int32_t capacity;
    // The number of initialized elements in this array. Used for loops and iteration.
    int32_t length;
    // A pointer to the first element in the internal array.
    SEClay_RenderCommand* internalArray;
} SEClay_RenderCommandArray;

// Represents the current state of interaction with clay this frame.
enum class SEClay_PointerDataInteractionState : uint8_t {
    // A left mouse click, or touch occurred this frame.
    PressedThisFrame,
    // The left mouse button click or touch happened at some point in the past, and is still currently held down this frame.
    Pressed,
    // The left mouse button click or touch was released this frame.
    ReleasedThisFrame,
    // The left mouse button click or touch is not currently down / was released at some point in the past.
    Released,
};

// Information on the current state of pointer interactions this frame.
typedef struct SEClay_PointerData {
    // The position of the mouse / touch / pointer relative to the root of the layout.
    SEClay_Vector2 position;
    // Represents the current state of interaction with clay this frame.
    // SEClay_PointerDataInteractionState::PressedThisFrame - A left mouse click, or touch occurred this frame.
    // SEClay_PointerDataInteractionState::Pressed - The left mouse button click or touch happened at some point in the past, and is still currently held down this frame.
    // SEClay_PointerDataInteractionState::ReleasedThisFrame - The left mouse button click or touch was released this frame.
    // SEClay_PointerDataInteractionState::Released - The left mouse button click or touch is not currently down / was released at some point in the past.
    SEClay_PointerDataInteractionState state;
} SEClay_PointerData;

typedef struct SEClay_ElementDeclaration {
    // Primarily created via the SECLAY_ID(), SECLAY_IDI(), SECLAY_ID_LOCAL() and SECLAY_IDI_LOCAL() macros.
    // Represents a hashed string ID used for identifying and finding specific clay UI elements, required by functions such as SEClay_PointerOver() and SEClay_GetElementData().
    SEClay_ElementId id;
    // Controls various settings that affect the size and position of an element, as well as the sizes and positions of any child elements.
    SEClay_LayoutConfig layout;
    // Controls the background color of the resulting element.
    // By convention specified as 0-255, but interpretation is up to the renderer.
    // If no other config is specified, .backgroundColor will generate a RECTANGLE render command, otherwise it will be passed as a property to IMAGE or CUSTOM render commands.
    SEClay_Color backgroundColor;
    // Controls the "radius", or corner rounding of elements, including rectangles, borders and images.
    SEClay_CornerRadius cornerRadius;
    // Controls settings related to aspect ratio scaling.
    SEClay_AspectRatioElementConfig aspectRatio;
    // Controls settings related to image elements.
    SEClay_ImageElementConfig image;
    // Controls whether and how an element "floats", which means it layers over the top of other elements in z order, and doesn't affect the position and size of siblings or parent elements.
    // Note: in order to activate floating, .floating.attachTo must be set to something other than the default value.
    SEClay_FloatingElementConfig floating;
    // Used to create CUSTOM render commands, usually to render element types not supported by Clay.
    SEClay_CustomElementConfig custom;
    // Controls whether an element should clip its contents, as well as providing child x,y offset configuration for scrolling.
    SEClay_ClipElementConfig clip;
    // Controls settings related to element borders, and will generate BORDER render commands.
    SEClay_BorderElementConfig border;
    // A pointer that will be transparently passed through to resulting render commands.
    void *userData;
} SEClay_ElementDeclaration;

SECLAY__WRAPPER_STRUCT(SEClay_ElementDeclaration);

// Represents the type of error clay encountered while computing layout.
enum class SEClay_ErrorType : uint8_t {
    // A text measurement function wasn't provided using SEClay_SetMeasureTextFunction(), or the provided function was null.
    TextMeasurementFunctionNotProvided,
    // Clay attempted to allocate its internal data structures but ran out of space.
    // The arena passed to SEClay_Initialize was created with a capacity smaller than that required by SEClay_MinMemorySize().
    ArenaCapacityExceeded,
    // Clay ran out of capacity in its internal array for storing elements. This limit can be increased with SEClay_SetMaxElementCount().
    ElementsCapacityExceeded,
    // Clay ran out of capacity in its internal array for storing elements. This limit can be increased with SEClay_SetMaxMeasureTextCacheWordCount().
    TextMeasurementCapacityExceeded,
    // Two elements were declared with exactly the same ID within one layout.
    DuplicateId,
    // A floating element was declared using SECLAY_ATTACH_TO_ELEMENT_ID and either an invalid .parentId was provided or no element with the provided .parentId was found.
    FloatingContainerParentNotFound,
    // An element was declared that using SEClay_SizingPercent but the percentage value was over 1. Percentage values are expected to be in the 0-1 range.
    PercentageOver1,
    // Clay encountered an internal error. It would be wonderful if you could report this so we can fix it!
    InternalError,
    // HARDENING+: an argument or call sequence was invalid (a close with nothing open, text with a negative length, ...). The call was ignored.
    InvalidUsage,
    // HARDENING+: a growable context reached its configured memory limit, so it can no longer grow to fit the layout. The layout is truncated.
    MemoryLimitReached,
    // HARDENING+: the allocator refused a request. The context keeps running at its previous size.
    AllocationFailed,
    // HARDENING+: not a fault: the context resized itself. Reported so a host can log or tune initial sizes.
    Resized,
};

// HARDENING+: the precise condition behind an error, stable across versions, for hosts that want to react or map it onto their own codes.
enum class SEClay_ErrorDetail : uint8_t {
    None = 0,
    Elements,
    ElementConfigs,
    WrappedTextLines,
    RenderCommands,
    TextMeasureItems,
    TextMeasureWords,
    ElementIdMap,
    DynamicStrings,
    TraversalStack,
    UnbalancedClose,
    BadTextArgument,
    OutOfBoundsAccess,
    ArenaTooSmall,
    SettingIgnored,
    MemoryLimit,
    Allocation,
    Grew,
    Shrank,
    MeasureFunction,
    DuplicateId,
    FloatingParent,
    Percentage,
};

// Data to identify the error that clay has encountered.
typedef struct SEClay_ErrorData {
    // Represents the type of error clay encountered while computing layout.
    // SEClay_ErrorType::TextMeasurementFunctionNotProvided - A text measurement function wasn't provided using SEClay_SetMeasureTextFunction(), or the provided function was null.
    // SEClay_ErrorType::ArenaCapacityExceeded - Clay attempted to allocate its internal data structures but ran out of space. The arena passed to SEClay_Initialize was created with a capacity smaller than that required by SEClay_MinMemorySize().
    // SEClay_ErrorType::ElementsCapacityExceeded - Clay ran out of capacity in its internal array for storing elements. This limit can be increased with SEClay_SetMaxElementCount().
    // SEClay_ErrorType::TextMeasurementCapacityExceeded - Clay ran out of capacity in its internal array for storing elements. This limit can be increased with SEClay_SetMaxMeasureTextCacheWordCount().
    // SEClay_ErrorType::DuplicateId - Two elements were declared with exactly the same ID within one layout.
    // SEClay_ErrorType::FloatingContainerParentNotFound - A floating element was declared using SECLAY_ATTACH_TO_ELEMENT_ID and either an invalid .parentId was provided or no element with the provided .parentId was found.
    // SEClay_ErrorType::PercentageOver1 - An element was declared that using SEClay_SizingPercent but the percentage value was over 1. Percentage values are expected to be in the 0-1 range.
    // SEClay_ErrorType::InternalError - Clay encountered an internal error. It would be wonderful if you could report this so we can fix it!
    SEClay_ErrorType errorType;
    // A string containing human-readable error text that explains the error in more detail.
    SEClay_String errorText;
    // A transparent pointer passed through from when the error handler was first provided.
    void *userData;
    // HARDENING+ (everything below is zero for errors raised by code that predates these fields) ---------------------------------
    // The precise condition (see SEClay_ErrorDetail).
    SEClay_ErrorDetail detail;
    // For capacity errors: how many entries the layout wanted, and how many the buffer holds. Otherwise 0.
    int64_t value;
    int64_t limit;
    // Where inside Clay the check that failed lives (static strings, valid forever).
    const char *sourceFile;
    const char *sourceFunction;
    int32_t sourceLine;
    // The layout generation (frame counter) the error happened in.
    uint32_t frame;
} SEClay_ErrorData;

// HARDENING+: one open element on the path from the layout root to the innermost open element.
typedef struct SEClay_ElementPathEntry {
    // Hashed id of the element, 0 while it has none yet (anonymous elements get theirs when first queried).
    uint32_t id;
    // The numeric offset used with the id (SECLAY_IDI index).
    uint32_t offset;
    // Its position among the parent's children declared so far.
    int32_t childIndex;
    // 1 when the element is a text element or another kind of leaf.
    int32_t isText;
} SEClay_ElementPathEntry;

// A wrapper struct around Clay's error handler function.
typedef struct {
    // A user provided function to call when Clay encounters an error during layout.
    void (*errorHandlerFunction)(SEClay_ErrorData errorText);
    // A pointer that will be transparently passed through to the error handler when it is called.
    void *userData;
} SEClay_ErrorHandler;

// Function Forward Declarations ---------------------------------

// Public API functions ------------------------------------------

// Returns the size, in bytes, of the minimum amount of memory Clay requires to operate at its current settings.
SECLAY_DLL_EXPORT uint32_t SEClay_MinMemorySize(void);
// Creates an arena for clay to use for its internal allocations, given a certain capacity in bytes and a pointer to an allocation of at least that size.
// Intended to be used with SEClay_MinMemorySize in the following way:
// uint32_t minMemoryRequired = SEClay_MinMemorySize();
// SEClay_Arena clayMemory = SEClay_CreateArenaWithCapacityAndMemory(minMemoryRequired, malloc(minMemoryRequired));
SECLAY_DLL_EXPORT SEClay_Arena SEClay_CreateArenaWithCapacityAndMemory(size_t capacity, void *memory);
// Sets the state of the "pointer" (i.e. the mouse or touch) in Clay's internal data. Used for detecting and responding to mouse events in the debug view,
// as well as for SEClay_Hovered() and scroll element handling.
SECLAY_DLL_EXPORT void SEClay_SetPointerState(SEClay_Vector2 position, bool pointerDown);
// Initialize Clay's internal arena and setup required data before layout can begin. Only needs to be called once.
// - arena can be created using SEClay_CreateArenaWithCapacityAndMemory()
// - layoutDimensions are the initial bounding dimensions of the layout (i.e. the screen width and height for a full screen layout)
// - errorHandler is used by Clay to inform you if something has gone wrong in configuration or layout.
SECLAY_DLL_EXPORT SEClay_Context* SEClay_Initialize(SEClay_Arena arena, SEClay_Dimensions layoutDimensions, SEClay_ErrorHandler errorHandler);

// HARDENING+: growable contexts -------------------------------------------------------------------------------------------------
// A fixed-arena context (SEClay_Initialize) has hard limits sized up front. A growable context owns its memory and resizes itself
// between frames to fit what the layout actually needs, so there is no per-layout element/command limit, only a memory limit the host
// chooses. Growth happens in SEClay_BeginLayout (never mid-layout, which would invalidate pointers the layout holds); a layout that
// outgrows the current size is truncated for that one frame -- every element past the limit is ignored, the rest is laid out and
// rendered normally, and an error with the exact numbers is reported -- and the next frame has room.

// Hooks for the memory a growable context uses. Leave all members NULL for malloc/free. The allocator must return memory aligned to at
// least 16 bytes (malloc does) and may return NULL to refuse; Clay then keeps running at its previous size and reports the refusal.
typedef struct SEClay_Allocator {
    void *(*allocate)(size_t size, void *userData);
    void (*release)(void *memory, size_t size, void *userData);
    void *userData;
} SEClay_Allocator;

typedef struct SEClay_GrowableConfig {
    SEClay_Dimensions layoutDimensions;
    SEClay_ErrorHandler errorHandler;
    SEClay_Allocator allocator;
    // Starting size, in elements. 0 selects a default. The context grows (and optionally shrinks) from here.
    int32_t initialElementCount;
    // Starting size of the measured-word cache. 0 selects a default.
    int32_t initialMeasureWordCount;
    // Upper bound on the memory the context may hold at once. 0 selects 1 GiB. At the limit the layout truncates rather than growing.
    size_t memoryLimitBytes;
    // Give memory back after the layout has needed far less than it holds for a long time.
    bool allowShrink;
} SEClay_GrowableConfig;

typedef struct SEClay_MemoryStats {
    bool growable;
    // The layout generation (frame counter) of the most recent finished layout.
    uint32_t frame;
    // True when the most recent layout had to leave elements, configs, text lines or commands out because it ran out of room.
    bool truncatedLastLayout;
    size_t bytesReserved;
    size_t bytesLimit;
    int32_t elementBudget;
    int32_t measureWordBudget;
    // What the most recent layout asked for (counting what was rejected), against what a buffer holds.
    int32_t usedElements, capacityElements;
    int32_t usedElementConfigs, capacityElementConfigs;
    int32_t usedWrappedTextLines, capacityWrappedTextLines;
    int32_t usedRenderCommands, capacityRenderCommands;
    uint32_t growCount;
    uint32_t shrinkCount;
} SEClay_MemoryStats;

// Creates a context that owns and manages its own memory. Returns NULL if the first allocation fails (after reporting it).
SECLAY_DLL_EXPORT SEClay_Context* SEClay_InitializeGrowable(const SEClay_GrowableConfig *config);
// Releases a context made by SEClay_InitializeGrowable (including the context itself). For a fixed-arena context this only releases the
// few heap buffers such a context can own; its arena stays the caller's to free. Safe with NULL. The context must not be current on
// another thread.
SECLAY_DLL_EXPORT void SEClay_DestroyContext(SEClay_Context* context);
// Replaces the error handler (function and user data) of the current context. For hosts that move a context's ownership around and need
// errors to keep reaching the right place.
SECLAY_DLL_EXPORT void SEClay_SetErrorHandler(SEClay_ErrorHandler errorHandler);
// Memory and utilisation figures for the current context.
[[nodiscard]] SECLAY_DLL_EXPORT SEClay_MemoryStats SEClay_GetMemoryStats(void);
// Copies the chain of currently open elements, outermost first, into `out` (up to `capacity` entries) and returns how many are open.
// Meant for error handlers: it describes where in the layout tree the call that failed was made.
SECLAY_DLL_EXPORT int32_t SEClay_GetOpenElementPath(SEClay_ElementPathEntry *out, int32_t capacity);
// Returns the Context that clay is currently using. Used when using multiple instances of clay simultaneously.
[[nodiscard]] SECLAY_DLL_EXPORT SEClay_Context* SEClay_GetCurrentContext(void);
// Sets the context that clay will use to compute the layout.
// Used to restore a context saved from SEClay_GetCurrentContext when using multiple instances of clay simultaneously.
SECLAY_DLL_EXPORT void SEClay_SetCurrentContext(SEClay_Context* context);
// Updates the state of Clay's internal scroll data, updating scroll content positions if scrollDelta is non zero, and progressing momentum scrolling.
// - enableDragScrolling when set to true will enable mobile device like "touch drag" scroll of scroll containers, including momentum scrolling after the touch has ended.
// - scrollDelta is the amount to scroll this frame on each axis in pixels.
// - deltaTime is the time in seconds since the last "frame" (scroll update)
SECLAY_DLL_EXPORT void SEClay_UpdateScrollContainers(bool enableDragScrolling, SEClay_Vector2 scrollDelta, float deltaTime);
// Returns the internally stored scroll offset for the currently open element.
// Generally intended for use with clip elements to create scrolling containers.
SECLAY_DLL_EXPORT SEClay_Vector2 SEClay_GetScrollOffset(void);
// Updates the layout dimensions in response to the window or outer container being resized.
SECLAY_DLL_EXPORT void SEClay_SetLayoutDimensions(SEClay_Dimensions dimensions);
// Called before starting any layout declarations.
SECLAY_DLL_EXPORT void SEClay_BeginLayout(void);
// Called when all layout declarations are finished.
// Computes the layout and generates and returns the array of render commands to draw.
SECLAY_DLL_EXPORT SEClay_RenderCommandArray SEClay_EndLayout(void);
// Calculates a hash ID from the given idString.
// Generally only used for dynamic strings when SECLAY_ID("stringLiteral") can't be used.
[[nodiscard]] SECLAY_DLL_EXPORT SEClay_ElementId SEClay_GetElementId(SEClay_String idString);
// Calculates a hash ID from the given idString and index.
// - index is used to avoid constructing dynamic ID strings in loops.
// Generally only used for dynamic strings when SECLAY_IDI("stringLiteral", index) can't be used.
[[nodiscard]] SECLAY_DLL_EXPORT SEClay_ElementId SEClay_GetElementIdWithIndex(SEClay_String idString, uint32_t index);
// Returns layout data such as the final calculated bounding box for an element with a given ID.
// The returned SEClay_ElementData contains a `found` bool that will be true if an element with the provided ID was found.
// This ID can be calculated either with SECLAY_ID() for string literal IDs, or SEClay_GetElementId for dynamic strings.
[[nodiscard]] SECLAY_DLL_EXPORT SEClay_ElementData SEClay_GetElementData(SEClay_ElementId id);
// Returns true if the pointer position provided by SEClay_SetPointerState is within the current element's bounding box.
// Works during element declaration, e.g. SECLAY({ .backgroundColor = SEClay_Hovered() ? BLUE : RED });
SECLAY_DLL_EXPORT bool SEClay_Hovered(void);
// Bind a callback that will be called when the pointer position provided by SEClay_SetPointerState is within the current element's bounding box.
// - onHoverFunction is a function pointer to a user defined function.
// - userData is a pointer that will be transparently passed through when the onHoverFunction is called.
SECLAY_DLL_EXPORT void SEClay_OnHover(void (*onHoverFunction)(SEClay_ElementId elementId, SEClay_PointerData pointerData, intptr_t userData), intptr_t userData);
// An imperative function that returns true if the pointer position provided by SEClay_SetPointerState is within the element with the provided ID's bounding box.
// This ID can be calculated either with SECLAY_ID() for string literal IDs, or SEClay_GetElementId for dynamic strings.
[[nodiscard]] SECLAY_DLL_EXPORT bool SEClay_PointerOver(SEClay_ElementId elementId);
// Returns the array of element IDs that the pointer is currently over.
[[nodiscard]] SECLAY_DLL_EXPORT SEClay_ElementIdArray SEClay_GetPointerOverIds(void);
// Returns data representing the state of the scrolling element with the provided ID.
// The returned SEClay_ScrollContainerData contains a `found` bool that will be true if a scroll element was found with the provided ID.
// An imperative function that returns true if the pointer position provided by SEClay_SetPointerState is within the element with the provided ID's bounding box.
// This ID can be calculated either with SECLAY_ID() for string literal IDs, or SEClay_GetElementId for dynamic strings.
[[nodiscard]] SECLAY_DLL_EXPORT SEClay_ScrollContainerData SEClay_GetScrollContainerData(SEClay_ElementId id);
// Binds a callback function that Clay will call to determine the dimensions of a given string slice.
// - measureTextFunction is a user provided function that adheres to the interface SEClay_Dimensions (SEClay_StringSlice text, SEClay_TextElementConfig *config, void *userData);
// - userData is a pointer that will be transparently passed through when the measureTextFunction is called.
SECLAY_DLL_EXPORT void SEClay_SetMeasureTextFunction(SEClay_Dimensions (*measureTextFunction)(SEClay_StringSlice text, SEClay_TextElementConfig *config, void *userData), void *userData);
// Experimental - Used in cases where Clay needs to integrate with a system that manages its own scrolling containers externally.
// Please reach out if you plan to use this function, as it may be subject to change.
SECLAY_DLL_EXPORT void SEClay_SetQueryScrollOffsetFunction(SEClay_Vector2 (*queryScrollOffsetFunction)(uint32_t elementId, void *userData), void *userData);
// A bounds-checked "get" function for the SEClay_RenderCommandArray returned from SEClay_EndLayout().
SECLAY_DLL_EXPORT SEClay_RenderCommand * SEClay_RenderCommandArray_Get(SEClay_RenderCommandArray* array, int32_t index);
// Enables and disables Clay's internal debug tools.
// This state is retained and does not need to be set each frame.
SECLAY_DLL_EXPORT void SEClay_SetDebugModeEnabled(bool enabled);
// Returns true if Clay's internal debug tools are currently enabled.
[[nodiscard]] SECLAY_DLL_EXPORT bool SEClay_IsDebugModeEnabled(void);
// Enables and disables visibility culling. By default, Clay will not generate render commands for elements whose bounding box is entirely outside the screen.
SECLAY_DLL_EXPORT void SEClay_SetCullingEnabled(bool enabled);
// Returns the maximum number of UI elements supported by Clay's current configuration.
SECLAY_DLL_EXPORT int32_t SEClay_GetMaxElementCount(void);
// Modifies the maximum number of UI elements supported by Clay's current configuration.
// This may require reallocating additional memory, and re-calling SEClay_Initialize();
SECLAY_DLL_EXPORT void SEClay_SetMaxElementCount(int32_t maxElementCount);
// Returns the maximum number of measured "words" (whitespace seperated runs of characters) that Clay can store in its internal text measurement cache.
SECLAY_DLL_EXPORT int32_t SEClay_GetMaxMeasureTextCacheWordCount(void);
// Modifies the maximum number of measured "words" (whitespace seperated runs of characters) that Clay can store in its internal text measurement cache.
// This may require reallocating additional memory, and re-calling SEClay_Initialize();
SECLAY_DLL_EXPORT void SEClay_SetMaxMeasureTextCacheWordCount(int32_t maxMeasureTextCacheWordCount);
// Resets Clay's internal text measurement cache. Useful if font mappings have changed or fonts have been reloaded.
SECLAY_DLL_EXPORT void SEClay_ResetMeasureTextCache(void);

// HARDENING: frees the few buffers a context can own on the heap outside its arena (scroll container storage that
// outgrew its arena slice, and the scroll id index). Call once before releasing the arena memory of a context that is
// no longer used. Safe to call with NULL.
SECLAY_DLL_EXPORT void SEClay_ReleaseContextResources(SEClay_Context* context);

// Internal API functions required by macros ----------------------

SECLAY_DLL_EXPORT void SEClay__OpenElement(void);
SECLAY_DLL_EXPORT void SEClay__ConfigureOpenElement(const SEClay_ElementDeclaration config);
SECLAY_DLL_EXPORT void SEClay__ConfigureOpenElementPtr(const SEClay_ElementDeclaration *config);
SECLAY_DLL_EXPORT void SEClay__CloseElement(void);

// An open layout element, closed when this goes out of scope. Opening and configuring happen in the constructor, so a scope can only exist for
// an element that is really open, and cannot outlive it.
class SEClay_ElementScope {
  public:
    explicit SEClay_ElementScope(const SEClay_ElementDeclaration &declaration) {
        SEClay__OpenElement();
        SEClay__ConfigureOpenElement(declaration);
    }
    ~SEClay_ElementScope() { SEClay__CloseElement(); }
    SEClay_ElementScope(const SEClay_ElementScope &) = delete;
    SEClay_ElementScope &operator=(const SEClay_ElementScope &) = delete;
    SEClay_ElementScope(SEClay_ElementScope &&) = delete;
    SEClay_ElementScope &operator=(SEClay_ElementScope &&) = delete;
};
SECLAY_DLL_EXPORT SEClay_ElementId SEClay__HashString(SEClay_String key, uint32_t offset, uint32_t seed);
SECLAY_DLL_EXPORT void SEClay__OpenTextElement(SEClay_String text, SEClay_TextElementConfig *textConfig);
SECLAY_DLL_EXPORT SEClay_TextElementConfig *SEClay__StoreTextElementConfig(SEClay_TextElementConfig config);
SECLAY_DLL_EXPORT uint32_t SEClay__GetParentElementId(void);

extern SEClay_Color SEClay__debugViewHighlightColor;
extern uint32_t SEClay__debugViewWidth;

#ifdef __cplusplus
}
#endif

#endif // SECLAY_HEADER

// -----------------------------------------
// IMPLEMENTATION --------------------------
// -----------------------------------------
#ifdef SECLAY_IMPLEMENTATION
#undef SECLAY_IMPLEMENTATION

#ifndef SECLAY__NULL
#define SECLAY__NULL 0
#endif

#ifndef SECLAY__MAXFLOAT
#define SECLAY__MAXFLOAT 3.40282346638528859812e+38F
#endif

// HARDENING: the "current context" and the failure sentinels below are per-thread, so UI contexts driven from
// different threads (one per window) cannot clobber each other's state.
#ifndef SECLAY__THREAD_LOCAL
    #if defined(__cplusplus)
        #define SECLAY__THREAD_LOCAL thread_local
    #elif defined(_MSC_VER)
        #define SECLAY__THREAD_LOCAL __declspec(thread)
    #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
        #define SECLAY__THREAD_LOCAL _Thread_local
    #elif defined(__GNUC__) || defined(__clang__)
        #define SECLAY__THREAD_LOCAL __thread
    #else
        #define SECLAY__THREAD_LOCAL
    #endif
#endif

SEClay_LayoutConfig SECLAY_LAYOUT_DEFAULT = SECLAY__DEFAULT_STRUCT;

SEClay_Color SEClay__Color_DEFAULT = SECLAY__DEFAULT_STRUCT;
SEClay_CornerRadius SEClay__CornerRadius_DEFAULT = SECLAY__DEFAULT_STRUCT;
SEClay_BorderWidth SEClay__BorderWidth_DEFAULT = SECLAY__DEFAULT_STRUCT;

// The below functions define array bounds checking and convenience functions for a provided type.
#define SECLAY__ARRAY_DEFINE_FUNCTIONS(typeName, arrayName)                                                       \
                                                                                                                \
typedef struct                                                                                                  \
{                                                                                                               \
    int32_t length;                                                                                             \
    typeName *internalArray;                                                                                    \
} arrayName##Slice;                                                                                             \
                                                                                                                \
SECLAY__THREAD_LOCAL typeName typeName##_DEFAULT = SECLAY__DEFAULT_STRUCT;                                          \
                                                                                                                \
/* HARDENING: callers write through the pointers returned on failure, so re-zero the sentinel before use. */    \
typeName *typeName##_FailureSentinel(void) {                                                                    \
    memset((void *)&typeName##_DEFAULT, 0, sizeof(typeName));                                                   \
    return &typeName##_DEFAULT;                                                                                 \
}                                                                                                               \
                                                                                                                \
arrayName arrayName##_Allocate_Arena(int32_t capacity, SEClay_Arena *arena) {                                     \
    typeName *storage = (typeName *)SEClay__Array_Allocate_Arena(capacity, sizeof(typeName), arena);              \
    /* HARDENING: a failed allocation must not leave a non-zero capacity over NULL storage. */                  \
    return arrayName{.capacity = storage ? capacity : 0, .length = 0, .internalArray = storage};    \
}                                                                                                               \
                                                                                                                \
typeName *arrayName##_Get(arrayName *array, int32_t index) {                                                    \
    return (array->internalArray && SEClay__Array_RangeCheck(index, array->length))                               \
        ? &array->internalArray[index] : typeName##_FailureSentinel();                                          \
}                                                                                                               \
                                                                                                                \
typeName arrayName##_GetValue(arrayName *array, int32_t index) {                                                \
    return (array->internalArray && SEClay__Array_RangeCheck(index, array->length))                               \
        ? array->internalArray[index] : *typeName##_FailureSentinel();                                          \
}                                                                                                               \
                                                                                                                \
typeName *arrayName##_Add(arrayName *array, typeName item) {                                                    \
    if (array->internalArray && SEClay__Array_AddCapacityCheck(array->length, array->capacity)) {                 \
        array->internalArray[array->length++] = item;                                                           \
        return &array->internalArray[array->length - 1];                                                        \
    }                                                                                                           \
    return typeName##_FailureSentinel();                                                                        \
}                                                                                                               \
                                                                                                                \
typeName *arrayName##Slice_Get(arrayName##Slice *slice, int32_t index) {                                        \
    return (slice->internalArray && SEClay__Array_RangeCheck(index, slice->length))                               \
        ? &slice->internalArray[index] : typeName##_FailureSentinel();                                          \
}                                                                                                               \
                                                                                                                \
typeName arrayName##_RemoveSwapback(arrayName *array, int32_t index) {                                          \
	if (array->internalArray && SEClay__Array_RangeCheck(index, array->length)) {                                 \
		array->length--;                                                                                        \
		typeName removed = array->internalArray[index];                                                         \
		array->internalArray[index] = array->internalArray[array->length];                                      \
		return removed;                                                                                         \
	}                                                                                                           \
	return *typeName##_FailureSentinel();                                                                       \
}                                                                                                               \
                                                                                                                \
void arrayName##_Set(arrayName *array, int32_t index, typeName value) {                                         \
	if (array->internalArray && SEClay__Array_RangeCheck(index, array->capacity)) {                               \
		array->internalArray[index] = value;                                                                    \
		array->length = index < array->length ? array->length : index + 1;                                      \
	}                                                                                                           \
}                                                                                                               \

#define SECLAY__ARRAY_DEFINE(typeName, arrayName)     \
typedef struct                                      \
{                                                   \
    int32_t capacity;                               \
    int32_t length;                                 \
    typeName *internalArray;                        \
} arrayName;                                        \
                                                    \
SECLAY__ARRAY_DEFINE_FUNCTIONS(typeName, arrayName)   \

SECLAY__THREAD_LOCAL SEClay_Context *SEClay__currentContext;
int32_t SEClay__defaultMaxElementCount = 8192;
int32_t SEClay__defaultMaxMeasureTextWordCacheCount = 16384;

void SEClay__ErrorHandlerFunctionDefault(SEClay_ErrorData errorText) {
    (void) errorText;
}

SEClay_String SECLAY__SPACECHAR = { .length = 1, .chars = " " };
SEClay_String SECLAY__STRING_DEFAULT = { .length = 0, .chars = NULL };

typedef struct {
    bool maxElementsExceeded;
    bool maxRenderCommandsExceeded;
    bool maxTextMeasureCacheExceeded;
    bool textMeasurementFunctionNotSet;
    bool maxWrappedLinesExceeded;
    bool maxElementConfigsExceeded;
    bool unbalancedClose;
} SEClay_BooleanWarnings;

typedef struct {
    SEClay_String baseMessage;
    SEClay_String dynamicMessage;
} SEClay__Warning;

SEClay__Warning SECLAY__WARNING_DEFAULT = SECLAY__DEFAULT_STRUCT;

typedef struct {
    int32_t capacity;
    int32_t length;
    SEClay__Warning *internalArray;
} SEClay__WarningArray;

typedef struct {
    SEClay_Color backgroundColor;
    SEClay_CornerRadius cornerRadius;
    void* userData;
} SEClay_SharedElementConfig;

SECLAY__WRAPPER_STRUCT(SEClay_SharedElementConfig);

SEClay__WarningArray SEClay__WarningArray_Allocate_Arena(int32_t capacity, SEClay_Arena *arena);
SEClay__Warning *SEClay__WarningArray_Add(SEClay__WarningArray *array, SEClay__Warning item);
void* SEClay__Array_Allocate_Arena(int32_t capacity, uint32_t itemSize, SEClay_Arena *arena);
bool SEClay__Array_RangeCheck(int32_t index, int32_t length);
bool SEClay__Array_AddCapacityCheck(int32_t length, int32_t capacity);
// HARDENING+: every error carries where inside Clay it was raised, a precise detail code and the numbers involved. `SEClay__ReportError`
// is the short form for sites with no extra numbers; `SECLAY__REPORT` is the full one.
static void SEClay__ReportErrorEx(SEClay_ErrorType errorType, SEClay_ErrorDetail detail, SEClay_String errorText, int64_t value, int64_t limit,
                                const char *sourceFile, int32_t sourceLine, const char *sourceFunction);
#define SECLAY__REPORT(type, detail, text, value, limit) \
    SEClay__ReportErrorEx((type), (detail), SECLAY_STRING(text), (int64_t)(value), (int64_t)(limit), __FILE__, (int32_t)__LINE__, __func__)
#define SEClay__ReportError(type, text) \
    SEClay__ReportErrorEx((type), SEClay_ErrorDetail::None, (text), 0, 0, __FILE__, (int32_t)__LINE__, __func__)

SECLAY__ARRAY_DEFINE(bool, SEClay__boolArray)
SECLAY__ARRAY_DEFINE(int32_t, SEClay__int32_tArray)
SECLAY__ARRAY_DEFINE(char, SEClay__charArray)
SECLAY__ARRAY_DEFINE_FUNCTIONS(SEClay_ElementId, SEClay_ElementIdArray)
SECLAY__ARRAY_DEFINE(SEClay_LayoutConfig, SEClay__LayoutConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_TextElementConfig, SEClay__TextElementConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_AspectRatioElementConfig, SEClay__AspectRatioElementConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_ImageElementConfig, SEClay__ImageElementConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_FloatingElementConfig, SEClay__FloatingElementConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_CustomElementConfig, SEClay__CustomElementConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_ClipElementConfig, SEClay__ClipElementConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_BorderElementConfig, SEClay__BorderElementConfigArray)
SECLAY__ARRAY_DEFINE(SEClay_String, SEClay__StringArray)
SECLAY__ARRAY_DEFINE(SEClay_SharedElementConfig, SEClay__SharedElementConfigArray)
SECLAY__ARRAY_DEFINE_FUNCTIONS(SEClay_RenderCommand, SEClay_RenderCommandArray)

enum class SEClay__ElementConfigType : uint8_t {
    None,
    Border,
    Floating,
    Clip,
    Aspect,
    Image,
    Text,
    Custom,
    Shared,
};

typedef union {
    SEClay_TextElementConfig *textElementConfig;
    SEClay_AspectRatioElementConfig *aspectRatioElementConfig;
    SEClay_ImageElementConfig *imageElementConfig;
    SEClay_FloatingElementConfig *floatingElementConfig;
    SEClay_CustomElementConfig *customElementConfig;
    SEClay_ClipElementConfig *clipElementConfig;
    SEClay_BorderElementConfig *borderElementConfig;
    SEClay_SharedElementConfig *sharedElementConfig;
} SEClay_ElementConfigUnion;

typedef struct {
    SEClay__ElementConfigType type;
    SEClay_ElementConfigUnion config;
} SEClay_ElementConfig;

SECLAY__ARRAY_DEFINE(SEClay_ElementConfig, SEClay__ElementConfigArray)

typedef struct {
    SEClay_Dimensions dimensions;
    SEClay_String line;
} SEClay__WrappedTextLine;

SECLAY__ARRAY_DEFINE(SEClay__WrappedTextLine, SEClay__WrappedTextLineArray)

typedef struct {
    SEClay_String text;
    SEClay_Dimensions preferredDimensions;
    int32_t elementIndex;
    SEClay__WrappedTextLineArraySlice wrappedLines;
} SEClay__TextElementData;

SECLAY__ARRAY_DEFINE(SEClay__TextElementData, SEClay__TextElementDataArray)

typedef struct {
    int32_t *elements;
    uint16_t length;
} SEClay__LayoutElementChildren;

typedef struct {
    union {
        SEClay__LayoutElementChildren children;
        SEClay__TextElementData *textElementData;
    } childrenOrTextContent;
    SEClay_Dimensions dimensions;
    SEClay_Dimensions minDimensions;
    SEClay_LayoutConfig *layoutConfig;
    SEClay__ElementConfigArraySlice elementConfigs;
    uint32_t id;
} SEClay_LayoutElement;

SECLAY__ARRAY_DEFINE(SEClay_LayoutElement, SEClay_LayoutElementArray)

// HARDENING: when a layout element lookup fails the zeroed sentinel comes back, whose layoutConfig is NULL -- and
// nearly every consumer dereferences layoutConfig straight away. Give the sentinel a valid (default) config.
static SEClay_LayoutElement *SEClay__LayoutElementWithConfig(SEClay_LayoutElement *element) {
    if (element == &SEClay_LayoutElement_DEFAULT) {
        element->layoutConfig = &SECLAY_LAYOUT_DEFAULT;
    }
    return element;
}
#define SEClay_LayoutElementArray_Get(array, index) SEClay__LayoutElementWithConfig((SEClay_LayoutElementArray_Get)((array), (index)))
#define SEClay_LayoutElementArray_Add(array, item) SEClay__LayoutElementWithConfig((SEClay_LayoutElementArray_Add)((array), (item)))

typedef struct {
    SEClay_LayoutElement *layoutElement;
    SEClay_BoundingBox boundingBox;
    SEClay_Dimensions contentSize;
    SEClay_Vector2 scrollOrigin;
    SEClay_Vector2 pointerOrigin;
    SEClay_Vector2 scrollMomentum;
    SEClay_Vector2 scrollPosition;
    SEClay_Vector2 previousDelta;
    float momentumTime;
    uint32_t elementId;
    bool openThisFrame;
    bool pointerScrollActive;
} SEClay__ScrollContainerDataInternal;

SECLAY__ARRAY_DEFINE(SEClay__ScrollContainerDataInternal, SEClay__ScrollContainerDataInternalArray)

typedef struct {
    bool collision;
    bool collapsed;
} SEClay__DebugElementData;

SECLAY__ARRAY_DEFINE(SEClay__DebugElementData, SEClay__DebugElementDataArray)

typedef struct { // todo get this struct into a single cache line
    SEClay_BoundingBox boundingBox;
    SEClay_ElementId elementId;
    SEClay_LayoutElement* layoutElement;
    void (*onHoverFunction)(SEClay_ElementId elementId, SEClay_PointerData pointerInfo, intptr_t userData);
    intptr_t hoverFunctionUserData;
    int32_t nextIndex;
    uint32_t generation;
    uint32_t idAlias;
    SEClay__DebugElementData *debugData;
    // HARDENING+: the entry's own copy of (the start of) its id text. The text a caller declared an element with is usually gone by the
    // next frame, so nothing stored here may point at it; `elementId.stringId` points at this buffer instead (re-pointed whenever
    // entries move, see SEClay__RebuildElementHashMap).
    char nameBuffer[48];
} SEClay_LayoutElementHashMapItem;

SECLAY__ARRAY_DEFINE(SEClay_LayoutElementHashMapItem, SEClay__LayoutElementHashMapItemArray)

typedef struct {
    int32_t startOffset;
    int32_t length;
    float width;
    int32_t next;
} SEClay__MeasuredWord;

SECLAY__ARRAY_DEFINE(SEClay__MeasuredWord, SEClay__MeasuredWordArray)

typedef struct {
    SEClay_Dimensions unwrappedDimensions;
    int32_t measuredWordsStartIndex;
    float minWidth;
    bool containsNewlines;
    // Hash map data
    uint32_t id;
    int32_t textLength; // HARDENING: second key besides the content hash (see SEClay__MeasureTextCached)
    int32_t nextIndex;
    uint32_t generation;
} SEClay__MeasureTextCacheItem;

SECLAY__ARRAY_DEFINE(SEClay__MeasureTextCacheItem, SEClay__MeasureTextCacheItemArray)

typedef struct {
    SEClay_LayoutElement *layoutElement;
    SEClay_Vector2 position;
    SEClay_Vector2 nextChildOffset;
} SEClay__LayoutElementTreeNode;

SECLAY__ARRAY_DEFINE(SEClay__LayoutElementTreeNode, SEClay__LayoutElementTreeNodeArray)

typedef struct {
    int32_t layoutElementIndex;
    uint32_t parentId; // This can be zero in the case of the root layout tree
    uint32_t clipElementId; // This can be zero if there is no clip element
    int16_t zIndex;
    SEClay_Vector2 pointerOffset; // Only used when scroll containers are managed externally
} SEClay__LayoutElementTreeRoot;

SECLAY__ARRAY_DEFINE(SEClay__LayoutElementTreeRoot, SEClay__LayoutElementTreeRootArray)

struct SEClay_Context {
    int32_t maxElementCount;
    int32_t maxMeasureTextCacheWordCount;
    bool warningsEnabled;
    // HARDENING: set when any arena allocation for this context failed (see SEClay__Array_Allocate_Arena).
    bool arenaAllocationFailed;
    // HARDENING+: while an element past the capacity limit is open, every element/text declared inside it is ignored. This counts the
    // open ignored elements so their closes can be matched.
    int32_t ghostDepth;
    // HARDENING+: growable-context state (all zero for a fixed-arena context).
    bool growable;
    bool allowShrink;
    bool growthFailedRecently;
    bool memoryLimitReported;
    int32_t initialElements;
    int32_t initialWords;
    uint32_t growthRetryAtGeneration;
    SEClay_Allocator allocator;
    size_t memoryLimitBytes;
    size_t persistentBlockSize;
    size_t ephemeralBlockSize;
    char *persistentBlock;
    char *ephemeralBlock;
    // The previous ephemeral block, kept alive for one extra frame because pointers into it (scroll containers, the id map) are still
    // read until the next layout re-registers them.
    char *retiredBlock;
    size_t retiredBlockSize;
    uint32_t retiredAtGeneration;
    SEClay_Arena persistentArena;
    SEClay_Arena ephemeralArena;
    uint32_t growCount;
    uint32_t shrinkCount;
    uint32_t lowUsageFrames;
    // What the layout in progress (and, after SEClay_EndLayout, the one just finished) asked for -- counting what was rejected.
    int32_t usageElements;
    int32_t usageElementConfigs;
    int32_t usageWrappedLines;
    int32_t usageRenderCommands;
    int32_t usageMeasureItemFailures;
    int32_t usageMeasureWordFailures;
    int32_t usageMeasureWordShortfall;
    int32_t usageDynamicChars;
    // Snapshot of the last finished layout, for SEClay_GetMemoryStats.
    int32_t lastUsageElements;
    int32_t lastUsageElementConfigs;
    int32_t lastUsageWrappedLines;
    int32_t lastUsageRenderCommands;
    bool lastLayoutTruncated;
    SEClay_ErrorHandler errorHandler;
    SEClay_BooleanWarnings booleanWarnings;
    SEClay__WarningArray warnings;

    SEClay_PointerData pointerInfo;
    SEClay_Dimensions layoutDimensions;
    SEClay_ElementId dynamicElementIndexBaseHash;
    uint32_t dynamicElementIndex;
    bool debugModeEnabled;
    bool disableCulling;
    bool externalScrollHandlingEnabled;
    uint32_t debugSelectedElementId;
    uint32_t generation;
    uintptr_t arenaResetOffset;
    void *measureTextUserData;
    void *queryScrollOffsetUserData;
    SEClay_Arena internalArena;
    // Layout Elements / Render Commands
    SEClay_LayoutElementArray layoutElements;
    SEClay_RenderCommandArray renderCommands;
    SEClay__int32_tArray openLayoutElementStack;
    SEClay__int32_tArray layoutElementChildren;
    SEClay__int32_tArray layoutElementChildrenBuffer;
    SEClay__TextElementDataArray textElementData;
    SEClay__int32_tArray aspectRatioElementIndexes;
    SEClay__int32_tArray reusableElementIndexBuffer;
    SEClay__int32_tArray layoutElementClipElementIds;
    // Configs
    SEClay__LayoutConfigArray layoutConfigs;
    SEClay__ElementConfigArray elementConfigs;
    SEClay__TextElementConfigArray textElementConfigs;
    SEClay__AspectRatioElementConfigArray aspectRatioElementConfigs;
    SEClay__ImageElementConfigArray imageElementConfigs;
    SEClay__FloatingElementConfigArray floatingElementConfigs;
    SEClay__ClipElementConfigArray clipElementConfigs;
    SEClay__CustomElementConfigArray customElementConfigs;
    SEClay__BorderElementConfigArray borderElementConfigs;
    SEClay__SharedElementConfigArray sharedElementConfigs;
    // Misc Data Structures
    SEClay__StringArray layoutElementIdStrings;
    SEClay__WrappedTextLineArray wrappedTextLines;
    SEClay__LayoutElementTreeNodeArray layoutElementTreeNodeArray1;
    SEClay__LayoutElementTreeRootArray layoutElementTreeRoots;
    SEClay__LayoutElementHashMapItemArray layoutElementsHashMapInternal;
    SEClay__int32_tArray layoutElementsHashMap;
    SEClay__MeasureTextCacheItemArray measureTextHashMapInternal;
    SEClay__int32_tArray measureTextHashMapInternalFreeList;
    SEClay__int32_tArray measureTextHashMap;
    SEClay__MeasuredWordArray measuredWords;
    SEClay__int32_tArray measuredWordsFreeList;
    // Next hash bucket of the measured-text cache the background sweep will clean (see SEClay__SweepMeasureTextCache).
    int32_t measureSweepBucket;
    uint32_t measureFullSweepGeneration;
    SEClay__int32_tArray openClipElementStack;
    SEClay_ElementIdArray pointerOverIds;
    SEClay__ScrollContainerDataInternalArray scrollContainerDatas;
    // Open-addressing index over scrollContainerDatas by element id (slot -> array index + 1, 0 = empty), so the many
    // per-element lookups below are O(1) instead of a scan of every scroll container: a UI where most cards, lists and
    // text boxes clip makes that scan quadratic. Rebuilt when entries are removed (`scrollIndexDirty`).
    int32_t *scrollIndex;
    uint32_t scrollIndexMask;
    bool scrollIndexDirty;
    SEClay__boolArray treeNodeVisited;
    SEClay__charArray dynamicStringData;
    SEClay__DebugElementDataArray debugElementData;
};

SEClay_Context* SEClay__Context_Allocate_Arena(SEClay_Arena *arena) {
    size_t totalSizeBytes = sizeof(SEClay_Context);
    uintptr_t memoryAddress = (uintptr_t)arena->memory;
    // Make sure the memory address passed in for clay to use is cache line aligned
    uintptr_t nextAllocOffset = (memoryAddress % 64);
    if (nextAllocOffset > arena->capacity || totalSizeBytes > arena->capacity - nextAllocOffset)
    {
        return NULL;
    }
    arena->nextAllocation = nextAllocOffset + totalSizeBytes;
    return (SEClay_Context*)(memoryAddress + nextAllocOffset);
}

SEClay_String SEClay__WriteStringToCharBuffer(SEClay__charArray *buffer, SEClay_String string) {
    // HARDENING: the buffer holds maxElementCount chars in total and was written without any bound.
    if (buffer->internalArray == NULL || string.chars == NULL || string.length <= 0 || buffer->length < 0 ||
        string.length > buffer->capacity - buffer->length) {
        SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::DynamicStrings,
                     "Clay ran out of space for dynamically generated strings; the string was dropped.",
                     (int64_t)buffer->length + (int64_t)string.length, buffer->capacity);
        return SEClay_String { .length = 0, .chars = "" };
    }
    for (int32_t i = 0; i < string.length; i++) {
        buffer->internalArray[buffer->length + i] = string.chars[i];
    }
    buffer->length += string.length;
    return SEClay_String { .length = string.length, .chars = (const char *)(buffer->internalArray + buffer->length - string.length) };
}

#ifdef SECLAY_WASM
    __attribute__((import_module("clay"), import_name("measureTextFunction"))) SEClay_Dimensions SEClay__MeasureText(SEClay_StringSlice text, SEClay_TextElementConfig *config, void *userData);
    __attribute__((import_module("clay"), import_name("queryScrollOffsetFunction"))) SEClay_Vector2 SEClay__QueryScrollOffset(uint32_t elementId, void *userData);
#else
    SEClay_Dimensions (*SEClay__MeasureText)(SEClay_StringSlice text, SEClay_TextElementConfig *config, void *userData);
    SEClay_Vector2 (*SEClay__QueryScrollOffset)(uint32_t elementId, void *userData);
#endif

SEClay_LayoutElement* SEClay__GetOpenLayoutElement(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    return SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&context->openLayoutElementStack, context->openLayoutElementStack.length - 1));
}

uint32_t SEClay__GetParentElementId(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    return SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&context->openLayoutElementStack, context->openLayoutElementStack.length - 2))->id;
}

SEClay_LayoutConfig * SEClay__StoreLayoutConfig(SEClay_LayoutConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SECLAY_LAYOUT_DEFAULT : SEClay__LayoutConfigArray_Add(&SEClay_GetCurrentContext()->layoutConfigs, config); }
SEClay_TextElementConfig * SEClay__StoreTextElementConfig(SEClay_TextElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_TextElementConfig_DEFAULT : SEClay__TextElementConfigArray_Add(&SEClay_GetCurrentContext()->textElementConfigs, config); }
SEClay_AspectRatioElementConfig * SEClay__StoreAspectRatioElementConfig(SEClay_AspectRatioElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_AspectRatioElementConfig_DEFAULT : SEClay__AspectRatioElementConfigArray_Add(&SEClay_GetCurrentContext()->aspectRatioElementConfigs, config); }
SEClay_ImageElementConfig * SEClay__StoreImageElementConfig(SEClay_ImageElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_ImageElementConfig_DEFAULT : SEClay__ImageElementConfigArray_Add(&SEClay_GetCurrentContext()->imageElementConfigs, config); }
SEClay_FloatingElementConfig * SEClay__StoreFloatingElementConfig(SEClay_FloatingElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_FloatingElementConfig_DEFAULT : SEClay__FloatingElementConfigArray_Add(&SEClay_GetCurrentContext()->floatingElementConfigs, config); }
SEClay_CustomElementConfig * SEClay__StoreCustomElementConfig(SEClay_CustomElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_CustomElementConfig_DEFAULT : SEClay__CustomElementConfigArray_Add(&SEClay_GetCurrentContext()->customElementConfigs, config); }
SEClay_ClipElementConfig * SEClay__StoreClipElementConfig(SEClay_ClipElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_ClipElementConfig_DEFAULT : SEClay__ClipElementConfigArray_Add(&SEClay_GetCurrentContext()->clipElementConfigs, config); }
SEClay_BorderElementConfig * SEClay__StoreBorderElementConfig(SEClay_BorderElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_BorderElementConfig_DEFAULT : SEClay__BorderElementConfigArray_Add(&SEClay_GetCurrentContext()->borderElementConfigs, config); }
SEClay_SharedElementConfig * SEClay__StoreSharedElementConfig(SEClay_SharedElementConfig config) {  return SEClay_GetCurrentContext()->booleanWarnings.maxElementsExceeded ? &SEClay_SharedElementConfig_DEFAULT : SEClay__SharedElementConfigArray_Add(&SEClay_GetCurrentContext()->sharedElementConfigs, config); }

SEClay_ElementConfig SEClay__AttachElementConfig(SEClay_ElementConfigUnion config, SEClay__ElementConfigType type) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->ghostDepth > 0 || context->booleanWarnings.maxElementsExceeded) {
        return SEClay_ElementConfig SECLAY__DEFAULT_STRUCT;
    }
    // HARDENING: the element's config slice length used to be bumped before (and regardless of whether) the config
    // was actually stored. With the shared config array full the slice then claimed entries beyond the end of the
    // array, so later reads walked into the neighbouring arena allocation. Only count configs that were stored. A config that does
    // not fit is dropped (the element renders without, e.g., its border) and counted, so a growable context sizes up for next frame.
    context->usageElementConfigs++;
    if (context->elementConfigs.length >= context->elementConfigs.capacity) {
        if (!context->booleanWarnings.maxElementConfigsExceeded) {
            context->booleanWarnings.maxElementConfigsExceeded = true;
            SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::ElementConfigs,
                         "Clay ran out of room for element configurations (backgrounds, borders, clips, ...); this element is missing some of them for this frame.",
                         context->usageElementConfigs, context->elementConfigs.capacity);
        }
        return SEClay_ElementConfig SECLAY__DEFAULT_STRUCT;
    }
    SEClay_LayoutElement *openLayoutElement = SEClay__GetOpenLayoutElement();
    SEClay_ElementConfig stored = *SEClay__ElementConfigArray_Add(&context->elementConfigs, SEClay_ElementConfig { .type = type, .config = config });
    openLayoutElement->elementConfigs.length++;
    return stored;
}

SEClay_ElementConfigUnion SEClay__FindElementConfigWithType(SEClay_LayoutElement *element, SEClay__ElementConfigType type) {
    for (int32_t i = 0; i < element->elementConfigs.length; i++) {
        SEClay_ElementConfig *config = SEClay__ElementConfigArraySlice_Get(&element->elementConfigs, i);
        if (config->type == type) {
            return config->config;
        }
    }
    return SEClay_ElementConfigUnion { NULL };
}

SEClay_ElementId SEClay__HashNumber(const uint32_t offset, const uint32_t seed) {
    uint32_t hash = seed;
    hash += (offset + 48);
    hash += (hash << 10);
    hash ^= (hash >> 6);

    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return SEClay_ElementId { .id = hash + 1, .offset = offset, .baseId = seed, .stringId = SECLAY__STRING_DEFAULT }; // Reserve the hash result of zero as "null id"
}

SEClay_ElementId SEClay__HashString(SEClay_String key, const uint32_t offset, const uint32_t seed) {
    uint32_t hash = 0;
    uint32_t base = seed;

    for (int32_t i = 0; i < key.length; i++) {
        base += key.chars[i];
        base += (base << 10);
        base ^= (base >> 6);
    }
    hash = base;
    hash += offset;
    hash += (hash << 10);
    hash ^= (hash >> 6);

    hash += (hash << 3);
    base += (base << 3);
    hash ^= (hash >> 11);
    base ^= (base >> 11);
    hash += (hash << 15);
    base += (base << 15);
    return SEClay_ElementId { .id = hash + 1, .offset = offset, .baseId = base + 1, .stringId = key }; // Reserve the hash result of zero as "null id"
}

#if !defined(SECLAY_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64))
static inline __m128i SEClay__SIMDRotateLeft(__m128i x, int r) {
    return _mm_or_si128(_mm_slli_epi64(x, r), _mm_srli_epi64(x, 64 - r));
}

static inline void SEClay__SIMDARXMix(__m128i* a, __m128i* b) {
    *a = _mm_add_epi64(*a, *b);
    *b = _mm_xor_si128(SEClay__SIMDRotateLeft(*b, 17), *a);
}

uint64_t SEClay__HashData(const uint8_t* data, size_t length) {
    // Pinched these constants from the BLAKE implementation
    __m128i v0 = _mm_set1_epi64x(0x6a09e667f3bcc908ULL);
    __m128i v1 = _mm_set1_epi64x(0xbb67ae8584caa73bULL);
    __m128i v2 = _mm_set1_epi64x(0x3c6ef372fe94f82bULL);
    __m128i v3 = _mm_set1_epi64x(0xa54ff53a5f1d36f1ULL);

    uint8_t overflowBuffer[16] = { 0 };  // Temporary buffer for small inputs

    while (length > 0) {
        __m128i msg;
        if (length >= 16) {
            msg = _mm_loadu_si128((const __m128i*)data);
            data += 16;
            length -= 16;
        }
        else {
            for (size_t i = 0; i < length; i++) {
                overflowBuffer[i] = data[i];
            }
            msg = _mm_loadu_si128((const __m128i*)overflowBuffer);
            length = 0;
        }

        v0 = _mm_xor_si128(v0, msg);
        SEClay__SIMDARXMix(&v0, &v1);
        SEClay__SIMDARXMix(&v2, &v3);

        v0 = _mm_add_epi64(v0, v2);
        v1 = _mm_add_epi64(v1, v3);
    }

    SEClay__SIMDARXMix(&v0, &v1);
    SEClay__SIMDARXMix(&v2, &v3);
    v0 = _mm_add_epi64(v0, v2);
    v1 = _mm_add_epi64(v1, v3);
    v0 = _mm_add_epi64(v0, v1);

    uint64_t result[2];
    _mm_storeu_si128((__m128i*)result, v0);

    return result[0] ^ result[1];
}
#elif !defined(SECLAY_DISABLE_SIMD) && defined(__aarch64__)
static inline void SEClay__SIMDARXMix(uint64x2_t* a, uint64x2_t* b) {
    *a = vaddq_u64(*a, *b);
    *b = veorq_u64(vorrq_u64(vshlq_n_u64(*b, 17), vshrq_n_u64(*b, 64 - 17)), *a);
}

uint64_t SEClay__HashData(const uint8_t* data, size_t length) {
    // Pinched these constants from the BLAKE implementation
    uint64x2_t v0 = vdupq_n_u64(0x6a09e667f3bcc908ULL);
    uint64x2_t v1 = vdupq_n_u64(0xbb67ae8584caa73bULL);
    uint64x2_t v2 = vdupq_n_u64(0x3c6ef372fe94f82bULL);
    uint64x2_t v3 = vdupq_n_u64(0xa54ff53a5f1d36f1ULL);

    uint8_t overflowBuffer[8] = { 0 };

    while (length > 0) {
        uint64x2_t msg;
        if (length > 16) {
            msg = vld1q_u64((const uint64_t*)data);
            data += 16;
            length -= 16;
        }
        else if (length > 8) {
            msg = vcombine_u64(vld1_u64((const uint64_t*)data), vdup_n_u64(0));
            data += 8;
            length -= 8;
        }
        else {
            for (size_t i = 0; i < length; i++) {
                overflowBuffer[i] = data[i];
            }
            uint8x8_t lower = vld1_u8(overflowBuffer);
            msg = vreinterpretq_u64_u8(vcombine_u8(lower, vdup_n_u8(0)));
            length = 0;
        }
        v0 = veorq_u64(v0, msg);
        SEClay__SIMDARXMix(&v0, &v1);
        SEClay__SIMDARXMix(&v2, &v3);

        v0 = vaddq_u64(v0, v2);
        v1 = vaddq_u64(v1, v3);
    }

    SEClay__SIMDARXMix(&v0, &v1);
    SEClay__SIMDARXMix(&v2, &v3);
    v0 = vaddq_u64(v0, v2);
    v1 = vaddq_u64(v1, v3);
    v0 = vaddq_u64(v0, v1);

    uint64_t result[2];
    vst1q_u64(result, v0);

    return result[0] ^ result[1];
}
#else
uint64_t SEClay__HashData(const uint8_t* data, size_t length) {
    uint64_t hash = 0;

    for (size_t i = 0; i < length; i++) {
        hash += data[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    return hash;
}
#endif

uint32_t SEClay__HashStringContentsWithConfig(SEClay_String *text, SEClay_TextElementConfig *config) {
    uint32_t hash = 0;
    if (text->isStaticallyAllocated) {
        hash += (uintptr_t)text->chars;
        hash += (hash << 10);
        hash ^= (hash >> 6);
        hash += text->length;
        hash += (hash << 10);
        hash ^= (hash >> 6);
    } else {
        hash = SEClay__HashData((const uint8_t *)text->chars, text->length) % UINT32_MAX;
    }

    hash += config->fontId;
    hash += (hash << 10);
    hash ^= (hash >> 6);

    hash += config->fontSize;
    hash += (hash << 10);
    hash ^= (hash >> 6);

    hash += config->letterSpacing;
    hash += (hash << 10);
    hash ^= (hash >> 6);

    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash + 1; // Reserve the hash result of zero as "null id"
}

// HARDENING: returns NULL (leaving previousWord->next untouched) when no word slot is available, instead of linking
// the chain to a slot that does not exist. Callers abort the measurement and release what they linked so far.
SEClay__MeasuredWord *SEClay__AddMeasuredWord(SEClay__MeasuredWord word, SEClay__MeasuredWord *previousWord) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->measuredWordsFreeList.length > 0) {
        int32_t newItemIndex = SEClay__int32_tArray_GetValue(&context->measuredWordsFreeList, (int)context->measuredWordsFreeList.length - 1);
        context->measuredWordsFreeList.length--;
        if (newItemIndex < 0 || newItemIndex >= context->measuredWords.capacity) {
            return NULL;
        }
        SEClay__MeasuredWordArray_Set(&context->measuredWords, newItemIndex, word);
        previousWord->next = newItemIndex;
        return SEClay__MeasuredWordArray_Get(&context->measuredWords, newItemIndex);
    } else {
        if (context->measuredWords.length >= context->measuredWords.capacity) {
            return NULL;
        }
        previousWord->next = (int32_t)context->measuredWords.length;
        return SEClay__MeasuredWordArray_Add(&context->measuredWords, word);
    }
}

// HARDENING: bounds-checked lookup of a measured word by chain index (the chains are walked with `while (i != -1)`,
// so a bad link used to read a sentinel whose `next` of 0 looped forever). NULL for any index outside the used range.
static SEClay__MeasuredWord *SEClay__GetMeasuredWordChecked(SEClay_Context *context, int32_t index) {
    if (index < 0 || index >= context->measuredWords.length || context->measuredWords.internalArray == NULL) {
        return NULL;
    }
    return &context->measuredWords.internalArray[index];
}

// Returns every word of a chain to the free list. The walk is capped at the number of words that can exist so that a
// corrupt (cyclic) chain cannot hang the frame.
static void SEClay__FreeMeasuredWordChain(SEClay_Context *context, int32_t firstWordIndex) {
    int32_t remaining = context->measuredWords.capacity;
    int32_t nextWordIndex = firstWordIndex;
    while (nextWordIndex != -1 && remaining-- > 0) {
        SEClay__MeasuredWord *measuredWord = SEClay__GetMeasuredWordChecked(context, nextWordIndex);
        if (measuredWord == NULL) {
            break;
        }
        SEClay__int32_tArray_Add(&context->measuredWordsFreeList, nextWordIndex);
        nextWordIndex = measuredWord->next;
    }
}

// HARDENING: the hash bucket count used to be maxMeasureTextCacheWordCount / 32 even when the bucket table
// (measureTextHashMap, sized by maxElementCount) was smaller, indexing past its end. Never exceed the real table.
static int32_t SEClay__MeasureBucketCount(SEClay_Context *context) {
    int32_t buckets = context->maxMeasureTextCacheWordCount / 32;
    if (buckets > context->measureTextHashMap.capacity) {
        buckets = context->measureTextHashMap.capacity;
    }
    return buckets;
}

// Frees the entries of one hash bucket that have not been asked for in a few frames (and the words they own). The
// cache used to do this only while *inserting into* the same bucket, so a bucket nobody measured into again kept its stale
// entries forever; an app that shows many different strings over time (a log, long lists) eventually filled the whole
// cache with dead ones and every new string failed to measure.
static void SEClay__EvictStaleMeasureEntries(SEClay_Context *context, uint32_t hashBucket) {
    int32_t elementIndexPrevious = 0;
    if (context->measureTextHashMap.internalArray == NULL || hashBucket >= (uint32_t)context->measureTextHashMap.capacity) {
        return;
    }
    int32_t elementIndex = context->measureTextHashMap.internalArray[hashBucket];
    // HARDENING: bounded walk -- a corrupt (cyclic) bucket chain must not hang the frame.
    int32_t remainingSteps = context->measureTextHashMapInternal.length;
    while (elementIndex != 0 && remainingSteps-- > 0) {
        if (elementIndex < 0 || elementIndex >= context->measureTextHashMapInternal.length) {
            // Broken link: cut the chain here rather than following it.
            if (elementIndexPrevious == 0) {
                context->measureTextHashMap.internalArray[hashBucket] = 0;
            } else {
                SEClay__MeasureTextCacheItemArray_Get(&context->measureTextHashMapInternal, elementIndexPrevious)->nextIndex = 0;
            }
            break;
        }
        SEClay__MeasureTextCacheItem *hashEntry = SEClay__MeasureTextCacheItemArray_Get(&context->measureTextHashMapInternal, elementIndex);
        if (context->generation - hashEntry->generation > 2) {
            SEClay__FreeMeasuredWordChain(context, hashEntry->measuredWordsStartIndex);
            int32_t nextIndex = hashEntry->nextIndex;
            SEClay__MeasureTextCacheItemArray_Set(&context->measureTextHashMapInternal, elementIndex, SEClay__MeasureTextCacheItem { .measuredWordsStartIndex = -1 });
            SEClay__int32_tArray_Add(&context->measureTextHashMapInternalFreeList, elementIndex);
            if (elementIndexPrevious == 0) {
                context->measureTextHashMap.internalArray[hashBucket] = nextIndex;
            } else {
                SEClay__MeasureTextCacheItemArray_Get(&context->measureTextHashMapInternal, elementIndexPrevious)->nextIndex = nextIndex;
            }
            elementIndex = nextIndex;
        } else {
            elementIndexPrevious = elementIndex;
            elementIndex = hashEntry->nextIndex;
        }
    }
}

// Cleans `bucketCount` buckets (wrapping round), continuing from where the last call stopped.
static void SEClay__SweepMeasureTextCache(SEClay_Context *context, int32_t bucketCount) {
    int32_t buckets = SEClay__MeasureBucketCount(context);
    if (buckets <= 0 || context->measureTextHashMap.internalArray == NULL) {
        return;
    }
    for (int32_t i = 0; i < bucketCount && i < buckets; i++) {
        context->measureSweepBucket = (context->measureSweepBucket + 1) % buckets;
        SEClay__EvictStaleMeasureEntries(context, (uint32_t)context->measureSweepBucket);
    }
}

SEClay__MeasureTextCacheItem *SEClay__MeasureTextCached(SEClay_String *text, SEClay_TextElementConfig *config) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    #ifndef SECLAY_WASM
    if (!SEClay__MeasureText) {
        if (!context->booleanWarnings.textMeasurementFunctionNotSet) {
            context->booleanWarnings.textMeasurementFunctionNotSet = true;
            SECLAY__REPORT(SEClay_ErrorType::TextMeasurementFunctionNotProvided, SEClay_ErrorDetail::MeasureFunction,
                         "Clay's text measure function is null, so all text measures as empty. Call SEClay_SetMeasureTextFunction() for this context (it must be set on the thread that lays out).",
                         0, 0);
        }
        return &SEClay__MeasureTextCacheItem_DEFAULT;
    }
    #endif
    int32_t bucketCount = SEClay__MeasureBucketCount(context);
    if (bucketCount <= 0 || context->measureTextHashMap.internalArray == NULL) {
        return SEClay__MeasureTextCacheItem_FailureSentinel();
    }
    // Nearly out of word or item slots with nothing on the matching free list: reclaim everything stale right now
    // (at most once per frame, it walks the whole cache) before deciding the cache is really full.
    // HARDENING: the item slots (measureTextHashMapInternal) used to have no such emergency sweep, so a UI with lots
    // of changing strings could fill them with dead entries and then fail to measure every new string.
    if (context->generation != context->measureFullSweepGeneration &&
        ((context->measuredWordsFreeList.length == 0 &&
          context->measuredWords.length >= context->measuredWords.capacity - 256) ||
         (context->measureTextHashMapInternalFreeList.length == 0 &&
          context->measureTextHashMapInternal.length >= context->measureTextHashMapInternal.capacity - 16))) {
        context->measureFullSweepGeneration = context->generation;
        SEClay__SweepMeasureTextCache(context, bucketCount);
    }
    uint32_t id = SEClay__HashStringContentsWithConfig(text, config);
    uint32_t hashBucket = id % (uint32_t)bucketCount;
    int32_t elementIndex = context->measureTextHashMap.internalArray[hashBucket];
    // HARDENING: bounded, range-checked walk of the bucket chain.
    int32_t remainingSteps = context->measureTextHashMapInternal.length;
    while (elementIndex > 0 && elementIndex < context->measureTextHashMapInternal.length && remainingSteps-- > 0) {
        SEClay__MeasureTextCacheItem *hashEntry = &context->measureTextHashMapInternal.internalArray[elementIndex];
        // HARDENING: also compare the text length. Two different strings can share a 32-bit content hash; reusing the
        // other string's measured word offsets could then point past the end of this one.
        if (hashEntry->id == id && hashEntry->textLength == text->length) {
            hashEntry->generation = context->generation;
            return hashEntry;
        }
        elementIndex = hashEntry->nextIndex;
    }
    // Not cached: drop whatever in this bucket hasn't been seen in a few frames before adding to it.
    SEClay__EvictStaleMeasureEntries(context, hashBucket);

    int32_t newItemIndex = 0;
    SEClay__MeasureTextCacheItem newCacheItem = { .measuredWordsStartIndex = -1, .id = id, .textLength = text->length, .generation = context->generation };
    SEClay__MeasureTextCacheItem *measured = NULL;
    if (context->measureTextHashMapInternalFreeList.length > 0) {
        newItemIndex = SEClay__int32_tArray_GetValue(&context->measureTextHashMapInternalFreeList, context->measureTextHashMapInternalFreeList.length - 1);
        context->measureTextHashMapInternalFreeList.length--;
        if (newItemIndex <= 0 || newItemIndex >= context->measureTextHashMapInternal.capacity) {
            return SEClay__MeasureTextCacheItem_FailureSentinel();
        }
        SEClay__MeasureTextCacheItemArray_Set(&context->measureTextHashMapInternal, newItemIndex, newCacheItem);
        measured = SEClay__MeasureTextCacheItemArray_Get(&context->measureTextHashMapInternal, newItemIndex);
    } else {
        if (context->measureTextHashMapInternal.length >= context->measureTextHashMapInternal.capacity - 1) {
            context->usageMeasureItemFailures++;
            if (!context->booleanWarnings.maxTextMeasureCacheExceeded) {
                SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::TextMeasureItems,
                             "The text measure cache has no free entry for another distinct string; text that is not cached measures as empty this frame.",
                             context->measureTextHashMapInternal.length, context->measureTextHashMapInternal.capacity);
                context->booleanWarnings.maxTextMeasureCacheExceeded = true;
            }
            return SEClay__MeasureTextCacheItem_FailureSentinel();
        }
        measured = SEClay__MeasureTextCacheItemArray_Add(&context->measureTextHashMapInternal, newCacheItem);
        newItemIndex = context->measureTextHashMapInternal.length - 1;
    }

    int32_t start = 0;
    int32_t end = 0;
    float lineWidth = 0;
    float measuredWidth = 0;
    float measuredHeight = 0;
    float spaceWidth = SEClay__MeasureText(SEClay_StringSlice { .length = 1, .chars = SECLAY__SPACECHAR.chars, .baseChars = SECLAY__SPACECHAR.chars }, config, context->measureTextUserData).width;
    SEClay__MeasuredWord tempWord = { .next = -1 };
    SEClay__MeasuredWord *previousWord = &tempWord;
    bool measureFailed = false;
    while (end < text->length && !measureFailed) {
        char current = text->chars[end];
        if (current == ' ' || current == '\n') {
            int32_t length = end - start;
            SEClay_Dimensions dimensions = SEClay__MeasureText(SEClay_StringSlice { .length = length, .chars = &text->chars[start], .baseChars = text->chars }, config, context->measureTextUserData);
            measured->minWidth = SECLAY__MAX(dimensions.width, measured->minWidth);
            measuredHeight = SECLAY__MAX(measuredHeight, dimensions.height);
            if (current == ' ') {
                dimensions.width += spaceWidth;
                SEClay__MeasuredWord *added = SEClay__AddMeasuredWord(SEClay__MeasuredWord { .startOffset = start, .length = length + 1, .width = dimensions.width, .next = -1 }, previousWord);
                if (added == NULL) { measureFailed = true; break; }
                previousWord = added;
                lineWidth += dimensions.width;
            }
            if (current == '\n') {
                if (length > 0) {
                    SEClay__MeasuredWord *added = SEClay__AddMeasuredWord(SEClay__MeasuredWord { .startOffset = start, .length = length, .width = dimensions.width, .next = -1 }, previousWord);
                    if (added == NULL) { measureFailed = true; break; }
                    previousWord = added;
                }
                SEClay__MeasuredWord *addedBreak = SEClay__AddMeasuredWord(SEClay__MeasuredWord { .startOffset = end + 1, .length = 0, .width = 0, .next = -1 }, previousWord);
                if (addedBreak == NULL) { measureFailed = true; break; }
                previousWord = addedBreak;
                lineWidth += dimensions.width;
                measuredWidth = SECLAY__MAX(lineWidth, measuredWidth);
                measured->containsNewlines = true;
                lineWidth = 0;
            }
            start = end + 1;
        }
        end++;
    }
    if (!measureFailed && end - start > 0) {
        SEClay_Dimensions dimensions = SEClay__MeasureText(SEClay_StringSlice { .length = end - start, .chars = &text->chars[start], .baseChars = text->chars }, config, context->measureTextUserData);
        if (SEClay__AddMeasuredWord(SEClay__MeasuredWord { .startOffset = start, .length = end - start, .width = dimensions.width, .next = -1 }, previousWord) == NULL) {
            measureFailed = true;
        }
        lineWidth += dimensions.width;
        measuredHeight = SECLAY__MAX(measuredHeight, dimensions.height);
        measured->minWidth = SECLAY__MAX(dimensions.width, measured->minWidth);
    }
    if (measureFailed) {
        // HARDENING: this used to return from the middle of the loop, leaking the cache item and every word linked so
        // far (never reachable from a bucket, so never evicted). Retrying the same text every frame then drained the
        // cache for good -- the "ran out of space in its text measurement cache" errors that never went away.
        // Count how many words this string still needed, so a growable context can size the word cache in one step instead of
        // doubling over many frames. (Words already stored for it were released by the rollback below, so count them too.)
        context->usageMeasureWordFailures++;
        {
            int32_t wordsWanted = 1;
            for (int32_t i = 0; i < text->length; i++) {
                if (text->chars[i] == ' ' || text->chars[i] == '\n') { wordsWanted++; }
            }
            context->usageMeasureWordShortfall += wordsWanted;
        }
        if (!context->booleanWarnings.maxTextMeasureCacheExceeded) {
            SECLAY__REPORT(SEClay_ErrorType::TextMeasurementCapacityExceeded, SEClay_ErrorDetail::TextMeasureWords,
                         "The text measure cache has no room for the words of this string; it measures as empty this frame.",
                         context->measuredWords.length, context->measuredWords.capacity);
            context->booleanWarnings.maxTextMeasureCacheExceeded = true;
        }
        SEClay__FreeMeasuredWordChain(context, tempWord.next);
        SEClay__MeasureTextCacheItemArray_Set(&context->measureTextHashMapInternal, newItemIndex, SEClay__MeasureTextCacheItem { .measuredWordsStartIndex = -1 });
        SEClay__int32_tArray_Add(&context->measureTextHashMapInternalFreeList, newItemIndex);
        return SEClay__MeasureTextCacheItem_FailureSentinel();
    }
    measuredWidth = SECLAY__MAX(lineWidth, measuredWidth) - config->letterSpacing;

    measured->measuredWordsStartIndex = tempWord.next;
    measured->unwrappedDimensions.width = measuredWidth;
    measured->unwrappedDimensions.height = measuredHeight;

    // Link the new item at the head of its bucket.
    measured->nextIndex = context->measureTextHashMap.internalArray[hashBucket];
    context->measureTextHashMap.internalArray[hashBucket] = newItemIndex;
    return measured;
}

bool SEClay__PointIsInsideRect(SEClay_Vector2 point, SEClay_BoundingBox rect) {
    return point.x >= rect.x && point.x <= rect.x + rect.width && point.y >= rect.y && point.y <= rect.y + rect.height;
}

// HARDENING: the element id -> element map lives in persistent memory and entries were never removed, so every
// distinct id an app ever declared (ids with a counter or a row key in them, tooltips, drag previews, ...) kept a slot
// for the lifetime of the context. Once maxElementCount distinct ids had been seen, new elements stopped getting map
// entries (no hover, bounds or scroll state) -- an "out of capacity" failure that no layout budget could prevent.
// Called at the start of each frame: drops the entries that were not declared last frame (or since) and rebuilds the
// bucket chains. Entries' `generation` is the frame (+1) in which the element was last declared.
// Re-derives everything that is a function of the id map's item list: each item's debug slot and the bucket chains.
static void SEClay__RebuildElementHashMap(SEClay_Context *context) {
    SEClay__LayoutElementHashMapItemArray *items = &context->layoutElementsHashMapInternal;
    const int32_t kept = items->length;
    // Per-item debug data is a parallel persistent array: hand each surviving item a fresh slot in the same order.
    if (context->debugElementData.internalArray != NULL) {
        int32_t debugKept = kept < context->debugElementData.capacity ? kept : context->debugElementData.capacity;
        memset((void *)context->debugElementData.internalArray, 0, (size_t)debugKept * sizeof(SEClay__DebugElementData));
        context->debugElementData.length = debugKept;
        for (int32_t i = 0; i < kept; i++) {
            items->internalArray[i].debugData = i < debugKept ? &context->debugElementData.internalArray[i] : &SEClay__DebugElementData_DEFAULT;
        }
    }
    for (int32_t i = 0; i < context->layoutElementsHashMap.capacity; i++) {
        context->layoutElementsHashMap.internalArray[i] = -1;
    }
    for (int32_t i = 0; i < kept; i++) {
        // Entries may have moved: their stored id text lives inside the entry itself, so re-point at the new location.
        if (items->internalArray[i].elementId.stringId.length > 0) {
            items->internalArray[i].elementId.stringId.chars = items->internalArray[i].nameBuffer;
        }
        uint32_t bucket = items->internalArray[i].elementId.id % (uint32_t)context->layoutElementsHashMap.capacity;
        items->internalArray[i].nextIndex = context->layoutElementsHashMap.internalArray[bucket];
        context->layoutElementsHashMap.internalArray[bucket] = i;
    }
}

static void SEClay__CompactElementHashMap(SEClay_Context *context, bool force) {
    SEClay__LayoutElementHashMapItemArray *items = &context->layoutElementsHashMapInternal;
    if (items->internalArray == NULL || context->layoutElementsHashMap.internalArray == NULL ||
        context->layoutElementsHashMap.capacity <= 0) {
        return;
    }
    // Only pay for the pass once the map is getting full (or when a growable context that may shrink asks for a true live count).
    if (!force && items->length < items->capacity - items->capacity / 4) {
        return;
    }
    int32_t kept = 0;
    for (int32_t i = 0; i < items->length; i++) {
        if (items->internalArray[i].generation >= context->generation) {
            if (kept != i) {
                items->internalArray[kept] = items->internalArray[i];
            }
            kept++;
        }
    }
    items->length = kept;
    SEClay__RebuildElementHashMap(context);
}

// Copies (the start of) `name` into the entry's own buffer and points the entry's stored id at it.
static void SEClay__StoreItemName(SEClay_LayoutElementHashMapItem *item, SEClay_String name) {
    if (name.chars == NULL || name.length <= 0) {
        item->elementId.stringId = SECLAY__STRING_DEFAULT;
        return;
    }
    const int32_t maxChars = (int32_t)sizeof(item->nameBuffer) - 1;
    const int32_t length = name.length < maxChars ? name.length : maxChars;
    memcpy(item->nameBuffer, name.chars, (size_t)length);
    item->nameBuffer[length] = '\0';
    item->elementId.stringId.isStaticallyAllocated = false;
    item->elementId.stringId.length = length;
    item->elementId.stringId.chars = item->nameBuffer;
}

SEClay_LayoutElementHashMapItem* SEClay__AddHashMapItem(SEClay_ElementId elementId, SEClay_LayoutElement* layoutElement, uint32_t idAlias) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->layoutElementsHashMapInternal.length >= context->layoutElementsHashMapInternal.capacity - 1 ||
        context->layoutElementsHashMap.capacity <= 0) {
        return NULL;
    }
    // HARDENING+: the map outlives the frame, but the id's text belongs to whoever declared the element and is usually gone by the
    // next frame. The entry keeps its own copy of the start of the text (SEClay__StoreItemName) and the id passed on to anything
    // else carries no string, so nothing that reads a stored id later can follow a dangling pointer. Per-frame name lookups
    // use layoutElementIdStrings, which is rebuilt every layout.
    const SEClay_String declaredName = elementId.stringId;
    elementId.stringId = SECLAY__STRING_DEFAULT;
    SEClay_LayoutElementHashMapItem item = { .elementId = elementId, .layoutElement = layoutElement, .nextIndex = -1, .generation = context->generation + 1, .idAlias = idAlias };
    uint32_t hashBucket = elementId.id % context->layoutElementsHashMap.capacity;
    int32_t hashItemPrevious = -1;
    int32_t hashItemIndex = context->layoutElementsHashMap.internalArray[hashBucket];
    while (hashItemIndex != -1) { // Just replace collision, not a big deal - leave it up to the end user
        SEClay_LayoutElementHashMapItem *hashItem = SEClay__LayoutElementHashMapItemArray_Get(&context->layoutElementsHashMapInternal, hashItemIndex);
        if (hashItem->elementId.id == elementId.id) { // Collision - resolve based on generation
            item.nextIndex = hashItem->nextIndex;
            if (hashItem->generation <= context->generation) { // First collision - assume this is the "same" element
                hashItem->elementId = elementId; // Make sure to copy this across. If the stringId reference has changed, we should update the hash item to use the new one.
                SEClay__StoreItemName(hashItem, declaredName);
                hashItem->idAlias = idAlias;
                hashItem->generation = context->generation + 1;
                hashItem->layoutElement = layoutElement;
                hashItem->debugData->collision = false;
                hashItem->onHoverFunction = NULL;
                hashItem->hoverFunctionUserData = 0;
            } else { // Multiple collisions this frame - two elements have the same ID
                SECLAY__REPORT(SEClay_ErrorType::DuplicateId, SEClay_ErrorDetail::DuplicateId,
                             "Two elements were declared with the same ID in one layout; hover, bounds and scroll state will follow only one of them. 'value' is the ID hash.",
                             elementId.id, 0);
                if (context->debugModeEnabled) {
                    hashItem->debugData->collision = true;
                }
            }
            return hashItem;
        }
        hashItemPrevious = hashItemIndex;
        hashItemIndex = hashItem->nextIndex;
    }
    SEClay_LayoutElementHashMapItem *hashItem = SEClay__LayoutElementHashMapItemArray_Add(&context->layoutElementsHashMapInternal, item);
    SEClay__StoreItemName(hashItem, declaredName);
    hashItem->debugData = SEClay__DebugElementDataArray_Add(&context->debugElementData, SEClay__DebugElementData SECLAY__DEFAULT_STRUCT);
    if (hashItemPrevious != -1) {
        SEClay__LayoutElementHashMapItemArray_Get(&context->layoutElementsHashMapInternal, hashItemPrevious)->nextIndex = (int32_t)context->layoutElementsHashMapInternal.length - 1;
    } else {
        context->layoutElementsHashMap.internalArray[hashBucket] = (int32_t)context->layoutElementsHashMapInternal.length - 1;
    }
    return hashItem;
}

SEClay_LayoutElementHashMapItem *SEClay__GetHashMapItem(uint32_t id) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->layoutElementsHashMap.capacity <= 0 || context->layoutElementsHashMap.internalArray == NULL) {
        return SEClay_LayoutElementHashMapItem_FailureSentinel();
    }
    uint32_t hashBucket = id % (uint32_t)context->layoutElementsHashMap.capacity;
    int32_t elementIndex = context->layoutElementsHashMap.internalArray[hashBucket];
    // HARDENING: bounded, range-checked chain walk (a bad link used to read the sentinel, whose nextIndex of 0 can loop).
    int32_t remainingSteps = context->layoutElementsHashMapInternal.length;
    while (elementIndex >= 0 && elementIndex < context->layoutElementsHashMapInternal.length && remainingSteps-- > 0) {
        SEClay_LayoutElementHashMapItem *hashEntry = &context->layoutElementsHashMapInternal.internalArray[elementIndex];
        if (hashEntry->elementId.id == id) {
            return hashEntry;
        }
        elementIndex = hashEntry->nextIndex;
    }
    return SEClay_LayoutElementHashMapItem_FailureSentinel();
}

SEClay_ElementId SEClay__GenerateIdForAnonymousElement(SEClay_LayoutElement *openLayoutElement) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    SEClay_LayoutElement *parentElement = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&context->openLayoutElementStack, context->openLayoutElementStack.length - 2));
    SEClay_ElementId elementId = SEClay__HashNumber(parentElement->childrenOrTextContent.children.length, parentElement->id);
    openLayoutElement->id = elementId.id;
    SEClay__AddHashMapItem(elementId, openLayoutElement, 0);
    SEClay__StringArray_Add(&context->layoutElementIdStrings, elementId.stringId);
    return elementId;
}

bool SEClay__ElementHasConfig(SEClay_LayoutElement *layoutElement, SEClay__ElementConfigType type) {
    for (int32_t i = 0; i < layoutElement->elementConfigs.length; i++) {
        if (SEClay__ElementConfigArraySlice_Get(&layoutElement->elementConfigs, i)->type == type) {
            return true;
        }
    }
    return false;
}

void SEClay__UpdateAspectRatioBox(SEClay_LayoutElement *layoutElement) {
    for (int32_t j = 0; j < layoutElement->elementConfigs.length; j++) {
        SEClay_ElementConfig *config = SEClay__ElementConfigArraySlice_Get(&layoutElement->elementConfigs, j);
        if (config->type == SEClay__ElementConfigType::Aspect) {
            SEClay_AspectRatioElementConfig *aspectConfig = config->config.aspectRatioElementConfig;
            if (aspectConfig->aspectRatio == 0) {
                break;
            }
            if (layoutElement->dimensions.width == 0 && layoutElement->dimensions.height != 0) {
                layoutElement->dimensions.width = layoutElement->dimensions.height * aspectConfig->aspectRatio;
            } else if (layoutElement->dimensions.width != 0 && layoutElement->dimensions.height == 0) {
                layoutElement->dimensions.height = layoutElement->dimensions.width * (1 / aspectConfig->aspectRatio);
            }
            break;
        }
    }
}

void SEClay__CloseElement(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    // HARDENING+: closing an element that was ignored for lack of room just balances its open.
    if (context->ghostDepth > 0) {
        context->ghostDepth--;
        return;
    }
    // HARDENING: more closes than opens used to resolve to the zeroed sentinel element, whose layoutConfig is NULL.
    if (context->openLayoutElementStack.length <= 0) {
        if (!context->booleanWarnings.unbalancedClose) {
            context->booleanWarnings.unbalancedClose = true;
            SECLAY__REPORT(SEClay_ErrorType::InvalidUsage, SEClay_ErrorDetail::UnbalancedClose,
                         "An element was closed when none was open (more closes than opens). The extra close was ignored; check that every opened element is closed exactly once.",
                         0, 0);
        }
        return;
    }
    SEClay_LayoutElement *openLayoutElement = SEClay__GetOpenLayoutElement();
    SEClay_LayoutConfig *layoutConfig = openLayoutElement->layoutConfig ? openLayoutElement->layoutConfig : &SECLAY_LAYOUT_DEFAULT;
    bool elementHasScrollHorizontal = false;
    bool elementHasScrollVertical = false;
    for (int32_t i = 0; i < openLayoutElement->elementConfigs.length; i++) {
        SEClay_ElementConfig *config = SEClay__ElementConfigArraySlice_Get(&openLayoutElement->elementConfigs, i);
        if (config->type == SEClay__ElementConfigType::Clip) {
            if (config->config.clipElementConfig != NULL) {
                elementHasScrollHorizontal = config->config.clipElementConfig->horizontal;
                elementHasScrollVertical = config->config.clipElementConfig->vertical;
            }
            // HARDENING: never let the stack length go negative (a negative length passed the Add capacity check).
            if (context->openClipElementStack.length > 0) { context->openClipElementStack.length--; }
            break;
        } else if (config->type == SEClay__ElementConfigType::Floating) {
            if (context->openClipElementStack.length > 0) { context->openClipElementStack.length--; }
        }
    }

    float leftRightPadding = (float)(layoutConfig->padding.left + layoutConfig->padding.right);
    float topBottomPadding = (float)(layoutConfig->padding.top + layoutConfig->padding.bottom);

    // Attach children to the current open element
    openLayoutElement->childrenOrTextContent.children.elements = &context->layoutElementChildren.internalArray[context->layoutElementChildren.length];
    if (layoutConfig->layoutDirection == SEClay_LayoutDirection::LeftToRight) {
        openLayoutElement->dimensions.width = leftRightPadding;
        openLayoutElement->minDimensions.width = leftRightPadding;
        for (int32_t i = 0; i < openLayoutElement->childrenOrTextContent.children.length; i++) {
            int32_t childIndex = SEClay__int32_tArray_GetValue(&context->layoutElementChildrenBuffer, (int)context->layoutElementChildrenBuffer.length - openLayoutElement->childrenOrTextContent.children.length + i);
            SEClay_LayoutElement *child = SEClay_LayoutElementArray_Get(&context->layoutElements, childIndex);
            openLayoutElement->dimensions.width += child->dimensions.width;
            openLayoutElement->dimensions.height = SECLAY__MAX(openLayoutElement->dimensions.height, child->dimensions.height + topBottomPadding);
            // Minimum size of child elements doesn't matter to scroll containers as they can shrink and hide their contents
            if (!elementHasScrollHorizontal) {
                openLayoutElement->minDimensions.width += child->minDimensions.width;
            }
            if (!elementHasScrollVertical) {
                openLayoutElement->minDimensions.height = SECLAY__MAX(openLayoutElement->minDimensions.height, child->minDimensions.height + topBottomPadding);
            }
            SEClay__int32_tArray_Add(&context->layoutElementChildren, childIndex);
        }
        float childGap = (float)(SECLAY__MAX(openLayoutElement->childrenOrTextContent.children.length - 1, 0) * layoutConfig->childGap);
        openLayoutElement->dimensions.width += childGap; // TODO this is technically a bug with childgap and scroll containers
        openLayoutElement->minDimensions.width += childGap;
    }
    else if (layoutConfig->layoutDirection == SEClay_LayoutDirection::TopToBottom) {
        openLayoutElement->dimensions.height = topBottomPadding;
        openLayoutElement->minDimensions.height = topBottomPadding;
        for (int32_t i = 0; i < openLayoutElement->childrenOrTextContent.children.length; i++) {
            int32_t childIndex = SEClay__int32_tArray_GetValue(&context->layoutElementChildrenBuffer, (int)context->layoutElementChildrenBuffer.length - openLayoutElement->childrenOrTextContent.children.length + i);
            SEClay_LayoutElement *child = SEClay_LayoutElementArray_Get(&context->layoutElements, childIndex);
            openLayoutElement->dimensions.height += child->dimensions.height;
            openLayoutElement->dimensions.width = SECLAY__MAX(openLayoutElement->dimensions.width, child->dimensions.width + leftRightPadding);
            // Minimum size of child elements doesn't matter to scroll containers as they can shrink and hide their contents
            if (!elementHasScrollVertical) {
                openLayoutElement->minDimensions.height += child->minDimensions.height;
            }
            if (!elementHasScrollHorizontal) {
                openLayoutElement->minDimensions.width = SECLAY__MAX(openLayoutElement->minDimensions.width, child->minDimensions.width + leftRightPadding);
            }
            SEClay__int32_tArray_Add(&context->layoutElementChildren, childIndex);
        }
        float childGap = (float)(SECLAY__MAX(openLayoutElement->childrenOrTextContent.children.length - 1, 0) * layoutConfig->childGap);
        openLayoutElement->dimensions.height += childGap; // TODO this is technically a bug with childgap and scroll containers
        openLayoutElement->minDimensions.height += childGap;
    }

    context->layoutElementChildrenBuffer.length -= openLayoutElement->childrenOrTextContent.children.length;
    if (context->layoutElementChildrenBuffer.length < 0) { context->layoutElementChildrenBuffer.length = 0; }

    // Clamp element min and max width to the values configured in the layout
    if (layoutConfig->sizing.width.type != SEClay__SizingType::Percent) {
        if (layoutConfig->sizing.width.size.minMax.max <= 0) { // Set the max size if the user didn't specify, makes calculations easier
            layoutConfig->sizing.width.size.minMax.max = SECLAY__MAXFLOAT;
        }
        openLayoutElement->dimensions.width = SECLAY__MIN(SECLAY__MAX(openLayoutElement->dimensions.width, layoutConfig->sizing.width.size.minMax.min), layoutConfig->sizing.width.size.minMax.max);
        openLayoutElement->minDimensions.width = SECLAY__MIN(SECLAY__MAX(openLayoutElement->minDimensions.width, layoutConfig->sizing.width.size.minMax.min), layoutConfig->sizing.width.size.minMax.max);
    } else {
        openLayoutElement->dimensions.width = 0;
    }

    // Clamp element min and max height to the values configured in the layout
    if (layoutConfig->sizing.height.type != SEClay__SizingType::Percent) {
        if (layoutConfig->sizing.height.size.minMax.max <= 0) { // Set the max size if the user didn't specify, makes calculations easier
            layoutConfig->sizing.height.size.minMax.max = SECLAY__MAXFLOAT;
        }
        openLayoutElement->dimensions.height = SECLAY__MIN(SECLAY__MAX(openLayoutElement->dimensions.height, layoutConfig->sizing.height.size.minMax.min), layoutConfig->sizing.height.size.minMax.max);
        openLayoutElement->minDimensions.height = SECLAY__MIN(SECLAY__MAX(openLayoutElement->minDimensions.height, layoutConfig->sizing.height.size.minMax.min), layoutConfig->sizing.height.size.minMax.max);
    } else {
        openLayoutElement->dimensions.height = 0;
    }

    SEClay__UpdateAspectRatioBox(openLayoutElement);

    bool elementIsFloating = SEClay__ElementHasConfig(openLayoutElement, SEClay__ElementConfigType::Floating);

    // Close the currently open element
    int32_t closingElementIndex = SEClay__int32_tArray_RemoveSwapback(&context->openLayoutElementStack, (int)context->openLayoutElementStack.length - 1);
    openLayoutElement = SEClay__GetOpenLayoutElement();

    if (!elementIsFloating && context->openLayoutElementStack.length > 1) {
        openLayoutElement->childrenOrTextContent.children.length++;
        SEClay__int32_tArray_Add(&context->layoutElementChildrenBuffer, closingElementIndex);
    }
}

bool SEClay__MemCmp(const char *s1, const char *s2, int32_t length);
#if !defined(SECLAY_DISABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64))
    bool SEClay__MemCmp(const char *s1, const char *s2, int32_t length) {
        while (length >= 16) {
            __m128i v1 = _mm_loadu_si128((const __m128i *)s1);
            __m128i v2 = _mm_loadu_si128((const __m128i *)s2);

            if (_mm_movemask_epi8(_mm_cmpeq_epi8(v1, v2)) != 0xFFFF) { // If any byte differs
                return false;
            }

            s1 += 16;
            s2 += 16;
            length -= 16;
        }

        // Handle remaining bytes
        while (length--) {
            if (*s1 != *s2) {
                return false;
            }
            s1++;
            s2++;
        }

        return true;
    }
#elif !defined(SECLAY_DISABLE_SIMD) && defined(__aarch64__)
    bool SEClay__MemCmp(const char *s1, const char *s2, int32_t length) {
        while (length >= 16) {
            uint8x16_t v1 = vld1q_u8((const uint8_t *)s1);
            uint8x16_t v2 = vld1q_u8((const uint8_t *)s2);

            // Compare vectors
            if (vminvq_u32(vreinterpretq_u32_u8(vceqq_u8(v1, v2))) != 0xFFFFFFFF) { // If there's a difference
                return false;
            }

            s1 += 16;
            s2 += 16;
            length -= 16;
        }

        // Handle remaining bytes
        while (length--) {
            if (*s1 != *s2) {
                return false;
            }
            s1++;
            s2++;
        }

        return true;
    }
#else
    bool SEClay__MemCmp(const char *s1, const char *s2, int32_t length) {
        for (int32_t i = 0; i < length; i++) {
            if (s1[i] != s2[i]) {
                return false;
            }
        }
        return true;
    }
#endif

// HARDENING+: the layout has no room for another element. Instead of abandoning the whole frame, the element (and everything declared
// inside it) is ignored while the rest of the layout carries on, so the frame is a valid partial layout. `usageElements` keeps counting
// so a growable context learns how many elements the frame really wanted.
static void SEClay__RejectElementForLackOfRoom(SEClay_Context *context) {
    context->ghostDepth++;
    if (!context->booleanWarnings.maxElementsExceeded) {
        context->booleanWarnings.maxElementsExceeded = true;
        SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::Elements,
                     "Clay ran out of room for layout elements; the elements declared after this point are left out of this frame. A growable context fits itself to the layout on the next frame.",
                     context->usageElements, context->layoutElements.capacity);
    }
}

void SEClay__OpenElement(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->usageElements++;
    if (context->ghostDepth > 0 || context->layoutElements.length >= context->layoutElements.capacity - 1 || context->booleanWarnings.maxElementsExceeded) {
        SEClay__RejectElementForLackOfRoom(context);
        return;
    }
    SEClay_LayoutElement layoutElement = SECLAY__DEFAULT_STRUCT;
    SEClay_LayoutElementArray_Add(&context->layoutElements, layoutElement);
    SEClay__int32_tArray_Add(&context->openLayoutElementStack, context->layoutElements.length - 1);
    if (context->openClipElementStack.length > 0) {
        SEClay__int32_tArray_Set(&context->layoutElementClipElementIds, context->layoutElements.length - 1, SEClay__int32_tArray_GetValue(&context->openClipElementStack, (int)context->openClipElementStack.length - 1));
    } else {
        SEClay__int32_tArray_Set(&context->layoutElementClipElementIds, context->layoutElements.length - 1, 0);
    }
}

void SEClay__OpenTextElement(SEClay_String text, SEClay_TextElementConfig *textConfig) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->usageElements++;
    context->usageElementConfigs++;
    if (context->ghostDepth > 0) {
        return; // inside an ignored element
    }
    if (context->layoutElements.length >= context->layoutElements.capacity - 1 || context->booleanWarnings.maxElementsExceeded ||
        context->elementConfigs.length >= context->elementConfigs.capacity || context->textElementData.length >= context->textElementData.capacity) {
        // A text leaf has no matching close, so it is simply left out (rather than counted in ghostDepth).
        if (!context->booleanWarnings.maxElementsExceeded) {
            context->booleanWarnings.maxElementsExceeded = true;
            SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::Elements,
                         "Clay ran out of room for layout elements; the elements declared after this point are left out of this frame. A growable context fits itself to the layout on the next frame.",
                         context->usageElements, context->layoutElements.capacity);
        }
        return;
    }
    // HARDENING: text outside any open element would mutate the shared sentinel element; a missing text config or a
    // negative/NULL-backed string would reach the measure function and the hasher as-is.
    if (context->openLayoutElementStack.length <= 0 || textConfig == NULL) {
        SECLAY__REPORT(SEClay_ErrorType::InvalidUsage, SEClay_ErrorDetail::BadTextArgument,
                     "Text was declared with no open parent element or without a text configuration. The text was ignored.",
                     0, 0);
        return;
    }
    if (text.length < 0 || (text.length > 0 && text.chars == NULL)) {
        text.length = 0;
        text.chars = "";
    }
    SEClay_LayoutElement *parentElement = SEClay__GetOpenLayoutElement();

    SEClay_LayoutElement layoutElement = SECLAY__DEFAULT_STRUCT;
    SEClay_LayoutElement *textElement = SEClay_LayoutElementArray_Add(&context->layoutElements, layoutElement);
    if (context->openClipElementStack.length > 0) {
        SEClay__int32_tArray_Set(&context->layoutElementClipElementIds, context->layoutElements.length - 1, SEClay__int32_tArray_GetValue(&context->openClipElementStack, (int)context->openClipElementStack.length - 1));
    } else {
        SEClay__int32_tArray_Set(&context->layoutElementClipElementIds, context->layoutElements.length - 1, 0);
    }

    SEClay__int32_tArray_Add(&context->layoutElementChildrenBuffer, context->layoutElements.length - 1);
    SEClay__MeasureTextCacheItem *textMeasured = SEClay__MeasureTextCached(&text, textConfig);
    SEClay_ElementId elementId = SEClay__HashNumber(parentElement->childrenOrTextContent.children.length, parentElement->id);
    textElement->id = elementId.id;
    SEClay__AddHashMapItem(elementId, textElement, 0);
    SEClay__StringArray_Add(&context->layoutElementIdStrings, elementId.stringId);
    SEClay_Dimensions textDimensions = { .width = textMeasured->unwrappedDimensions.width, .height = textConfig->lineHeight > 0 ? (float)textConfig->lineHeight : textMeasured->unwrappedDimensions.height };
    textElement->dimensions = textDimensions;
    textElement->minDimensions = SEClay_Dimensions { .width = textMeasured->minWidth, .height = textDimensions.height };
    textElement->childrenOrTextContent.textElementData = SEClay__TextElementDataArray_Add(&context->textElementData, SEClay__TextElementData { .text = text, .preferredDimensions = textMeasured->unwrappedDimensions, .elementIndex = context->layoutElements.length - 1 });
    textElement->elementConfigs = SEClay__ElementConfigArraySlice {
            .length = 1,
            .internalArray = SEClay__ElementConfigArray_Add(&context->elementConfigs, SEClay_ElementConfig { .type = SEClay__ElementConfigType::Text, .config = { .textElementConfig = textConfig }})
    };
    textElement->layoutConfig = &SECLAY_LAYOUT_DEFAULT;
    parentElement->childrenOrTextContent.children.length++;
}

SEClay_ElementId SEClay__AttachId(SEClay_ElementId elementId) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->ghostDepth > 0 || context->openLayoutElementStack.length <= 0) {
        return SEClay_ElementId_DEFAULT;
    }
    SEClay_LayoutElement *openLayoutElement = SEClay__GetOpenLayoutElement();
    uint32_t idAlias = openLayoutElement->id;
    openLayoutElement->id = elementId.id;
    SEClay_LayoutElementHashMapItem *mapItem = SEClay__AddHashMapItem(elementId, openLayoutElement, idAlias);
    // HARDENING+: the per-frame id text points at the map entry's own copy (valid for the rest of this layout), never at the caller's
    // string, which is usually a temporary that is gone by the time the debug view reads it at the end of the layout.
    SEClay__StringArray_Set(&context->layoutElementIdStrings, context->layoutElements.length - 1, mapItem != NULL ? mapItem->elementId.stringId : SECLAY__STRING_DEFAULT);
    return elementId;
}

// Grows an array's backing storage onto the heap once it outgrows its original arena allocation,
// doubling capacity each time -- unlike the arena itself (a fixed-size bump allocator sized once at
// SEClay_Initialize, with no room to resize any one sub-allocation in place), a small number of arrays
// need genuinely unbounded growth: e.g. scrollContainerDatas holds one entry per *distinct clipped
// element* alive across the whole app, not something scaled by any per-frame layout budget the caller
// controls (maxElementCount). Safe to call unconditionally before an _Add call -- it's a no-op if
// there's still room. Detects whether `internalArray` is still the original arena pointer (vs. an
// already-heap-owned buffer from a previous growth) by checking whether it falls inside the arena's
// own memory range, so it correctly avoids freeing arena memory (not ours to free) while still
// freeing/growing a previously-malloc'd buffer via realloc-equivalent copy+free semantics.
void *SEClay__GrowArrayStorage(void *internalArray, int32_t *capacity, int32_t length, size_t itemSize, SEClay_Arena *arena) {
    if (length < *capacity) {
        return internalArray;
    }
    // HARDENING: `*capacity * 2` overflowed int32 for huge capacities; cap the growth instead.
    if (*capacity >= (1 << 24) || length < 0) {
        return internalArray;
    }
    int32_t newCapacity = *capacity > 0 ? *capacity * 2 : 8;
    void *newArray = malloc((size_t)newCapacity * itemSize);
    if (newArray == NULL) {
        return internalArray; // Allocation failed -- the caller's existing capacity check reports this the same way it always has.
    }
    if (length > 0 && internalArray != NULL) {
        memcpy(newArray, internalArray, (size_t)length * itemSize);
    }
    const char *arenaBegin = arena->memory;
    const char *arenaEnd = arena->memory + arena->capacity;
    const bool isHeapOwned = !(arenaBegin != NULL && (const char *)internalArray >= arenaBegin && (const char *)internalArray < arenaEnd);
    if (isHeapOwned) {
        free(internalArray);
    }
    *capacity = newCapacity;
    return newArray;
}

static inline uint32_t SEClay__ScrollIdHash(uint32_t id) {
    id ^= id >> 16;
    id *= 0x7feb352dU;
    id ^= id >> 15;
    id *= 0x846ca68bU;
    id ^= id >> 16;
    return id;
}

// Rebuilds the element id -> scrollContainerDatas index (load factor <= 0.5, linear probing).
static void SEClay__RebuildScrollIndex(SEClay_Context *context) {
    uint32_t wanted = 64;
    while (wanted < (uint32_t)context->scrollContainerDatas.length * 2u + 2u) {
        wanted <<= 1;
    }
    if (!context->scrollIndex || context->scrollIndexMask + 1u != wanted) {
        int32_t *table = (int32_t *)malloc((size_t)wanted * sizeof(int32_t));
        if (table == NULL) {
            free(context->scrollIndex);
            context->scrollIndex = NULL;
            context->scrollIndexMask = 0;
            context->scrollIndexDirty = false;
            return; // lookups fall back to the linear scan
        }
        free(context->scrollIndex);
        context->scrollIndex = table;
        context->scrollIndexMask = wanted - 1u;
    }
    memset(context->scrollIndex, 0, ((size_t)context->scrollIndexMask + 1u) * sizeof(int32_t));
    for (int32_t i = 0; i < context->scrollContainerDatas.length; i++) {
        uint32_t slot = SEClay__ScrollIdHash(SEClay__ScrollContainerDataInternalArray_Get(&context->scrollContainerDatas, i)->elementId) & context->scrollIndexMask;
        while (context->scrollIndex[slot]) {
            slot = (slot + 1u) & context->scrollIndexMask;
        }
        context->scrollIndex[slot] = i + 1;
    }
    context->scrollIndexDirty = false;
}

// The scroll container data of element `elementId`, or NULL.
static SEClay__ScrollContainerDataInternal *SEClay__FindScrollContainer(SEClay_Context *context, uint32_t elementId) {
    if (context->scrollIndexDirty || context->scrollIndex == NULL) {
        SEClay__RebuildScrollIndex(context);
    }
    if (context->scrollIndex == NULL) {
        for (int32_t i = 0; i < context->scrollContainerDatas.length; i++) {
            SEClay__ScrollContainerDataInternal *mapping = SEClay__ScrollContainerDataInternalArray_Get(&context->scrollContainerDatas, i);
            if (mapping->elementId == elementId) {
                return mapping;
            }
        }
        return SECLAY__NULL;
    }
    uint32_t slot = SEClay__ScrollIdHash(elementId) & context->scrollIndexMask;
    for (;;) {
        int32_t entry = context->scrollIndex[slot];
        if (entry == 0) {
            return SECLAY__NULL;
        }
        SEClay__ScrollContainerDataInternal *mapping = SEClay__ScrollContainerDataInternalArray_Get(&context->scrollContainerDatas, entry - 1);
        if (mapping->elementId == elementId) {
            return mapping;
        }
        slot = (slot + 1u) & context->scrollIndexMask;
    }
}


void SEClay__ConfigureOpenElementPtr(const SEClay_ElementDeclaration *declaration) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    // HARDENING: once the element budget is blown the open element is no longer the one this declaration is for (the
    // matching open was skipped), so configuring it would clobber an unrelated parent. The frame is discarded anyway.
    if (context->ghostDepth > 0 || context->openLayoutElementStack.length <= 0) {
        return;
    }
    SEClay_LayoutElement *openLayoutElement = SEClay__GetOpenLayoutElement();
    openLayoutElement->layoutConfig = SEClay__StoreLayoutConfig(declaration->layout);
    if ((declaration->layout.sizing.width.type == SEClay__SizingType::Percent && declaration->layout.sizing.width.size.percent > 1) || (declaration->layout.sizing.height.type == SEClay__SizingType::Percent && declaration->layout.sizing.height.size.percent > 1)) {
        SECLAY__REPORT(SEClay_ErrorType::PercentageOver1, SEClay_ErrorDetail::Percentage,
                     "An element was configured with SEClay_SizingPercent, but the percentage was over 1.0. Clay expects a value between 0 and 1, i.e. 20% is 0.2.",
                     0, 0);
    }

    SEClay_ElementId openLayoutElementId = declaration->id;

    // HARDENING: this element's configs must be one contiguous run starting here. Reset the run so configuring an
    // element a second time cannot leave a slice length longer than the run that actually follows this pointer.
    openLayoutElement->elementConfigs.internalArray = context->elementConfigs.internalArray ? &context->elementConfigs.internalArray[context->elementConfigs.length] : NULL;
    openLayoutElement->elementConfigs.length = 0;
    SEClay_SharedElementConfig *sharedConfig = NULL;
    if (declaration->backgroundColor.a > 0) {
        sharedConfig = SEClay__StoreSharedElementConfig(SEClay_SharedElementConfig { .backgroundColor = declaration->backgroundColor });
        SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .sharedElementConfig = sharedConfig }, SEClay__ElementConfigType::Shared);
    }
    if (!SEClay__MemCmp((char *)(&declaration->cornerRadius), (char *)(&SEClay__CornerRadius_DEFAULT), sizeof(SEClay_CornerRadius))) {
        if (sharedConfig) {
            sharedConfig->cornerRadius = declaration->cornerRadius;
        } else {
            sharedConfig = SEClay__StoreSharedElementConfig(SEClay_SharedElementConfig { .cornerRadius = declaration->cornerRadius });
            SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .sharedElementConfig = sharedConfig }, SEClay__ElementConfigType::Shared);
        }
    }
    if (declaration->userData != 0) {
        if (sharedConfig) {
            sharedConfig->userData = declaration->userData;
        } else {
            sharedConfig = SEClay__StoreSharedElementConfig(SEClay_SharedElementConfig { .userData = declaration->userData });
            SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .sharedElementConfig = sharedConfig }, SEClay__ElementConfigType::Shared);
        }
    }
    if (declaration->image.imageData) {
        SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .imageElementConfig = SEClay__StoreImageElementConfig(declaration->image) }, SEClay__ElementConfigType::Image);
    }
    if (declaration->aspectRatio.aspectRatio > 0) {
        SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .aspectRatioElementConfig = SEClay__StoreAspectRatioElementConfig(declaration->aspectRatio) }, SEClay__ElementConfigType::Aspect);
        SEClay__int32_tArray_Add(&context->aspectRatioElementIndexes, context->layoutElements.length - 1);
    }
    if (declaration->floating.attachTo != SEClay_FloatingAttachToElement::None) {
        SEClay_FloatingElementConfig floatingConfig = declaration->floating;
        // This looks dodgy but because of the auto generated root element the depth of the tree will always be at least 2 here
        SEClay_LayoutElement *hierarchicalParent = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&context->openLayoutElementStack, context->openLayoutElementStack.length - 2));
        if (hierarchicalParent) {
            uint32_t clipElementId = 0;
            if (declaration->floating.attachTo == SEClay_FloatingAttachToElement::Parent) {
                // Attach to the element's direct hierarchical parent
                floatingConfig.parentId = hierarchicalParent->id;
                if (context->openClipElementStack.length > 0) {
                    clipElementId = SEClay__int32_tArray_GetValue(&context->openClipElementStack, (int)context->openClipElementStack.length - 1);
                }
            } else if (declaration->floating.attachTo == SEClay_FloatingAttachToElement::ElementWithId) {
                SEClay_LayoutElementHashMapItem *parentItem = SEClay__GetHashMapItem(floatingConfig.parentId);
                if (parentItem == &SEClay_LayoutElementHashMapItem_DEFAULT) {
                    SECLAY__REPORT(SEClay_ErrorType::FloatingContainerParentNotFound, SEClay_ErrorDetail::FloatingParent,
                                 "A floating element was attached to an element ID that has not been declared (yet) in this layout. 'value' is the missing ID hash; the parent must be declared before the floating element.",
                                 floatingConfig.parentId, 0);
                } else {
                    clipElementId = SEClay__int32_tArray_GetValue(&context->layoutElementClipElementIds, (int32_t)(parentItem->layoutElement - context->layoutElements.internalArray));
                }
            } else if (declaration->floating.attachTo == SEClay_FloatingAttachToElement::Root) {
                floatingConfig.parentId = SEClay__HashString(SECLAY_STRING("SEClay__RootContainer"), 0, 0).id;
            }
            if (!openLayoutElementId.id) {
                openLayoutElementId = SEClay__HashString(SECLAY_STRING("SEClay__FloatingContainer"), context->layoutElementTreeRoots.length, 0);
            }
            if (declaration->floating.clipTo == SEClay_FloatingClipToElement::None) {
                clipElementId = 0;
            }
            int32_t currentElementIndex = SEClay__int32_tArray_GetValue(&context->openLayoutElementStack, context->openLayoutElementStack.length - 1);
            SEClay__int32_tArray_Set(&context->layoutElementClipElementIds, currentElementIndex, clipElementId);
            SEClay__int32_tArray_Add(&context->openClipElementStack, clipElementId);
            SEClay__LayoutElementTreeRootArray_Add(&context->layoutElementTreeRoots, SEClay__LayoutElementTreeRoot {
                    .layoutElementIndex = SEClay__int32_tArray_GetValue(&context->openLayoutElementStack, context->openLayoutElementStack.length - 1),
                    .parentId = floatingConfig.parentId,
                    .clipElementId = clipElementId,
                    .zIndex = floatingConfig.zIndex,
            });
            SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .floatingElementConfig = SEClay__StoreFloatingElementConfig(floatingConfig) }, SEClay__ElementConfigType::Floating);
        }
    }
    if (declaration->custom.customData || declaration->custom.effectData) {
        SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .customElementConfig = SEClay__StoreCustomElementConfig(declaration->custom) }, SEClay__ElementConfigType::Custom);
    }

    if (openLayoutElementId.id != 0) {
        SEClay__AttachId(openLayoutElementId);
    } else if (openLayoutElement->id == 0) {
        openLayoutElementId = SEClay__GenerateIdForAnonymousElement(openLayoutElement);
    }

    if (declaration->clip.horizontal | declaration->clip.vertical) {
        SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .clipElementConfig = SEClay__StoreClipElementConfig(declaration->clip) }, SEClay__ElementConfigType::Clip);
        SEClay__int32_tArray_Add(&context->openClipElementStack, (int)openLayoutElement->id);
        // Retrieve or create cached data to track scroll position across frames
        SEClay__ScrollContainerDataInternal *scrollOffset = SEClay__FindScrollContainer(context, openLayoutElement->id);
        if (scrollOffset) {
            scrollOffset->layoutElement = openLayoutElement;
            scrollOffset->openThisFrame = true;
        }
        if (!scrollOffset) {
            context->scrollContainerDatas.internalArray = (SEClay__ScrollContainerDataInternal *)SEClay__GrowArrayStorage(
                context->scrollContainerDatas.internalArray, &context->scrollContainerDatas.capacity,
                context->scrollContainerDatas.length, sizeof(SEClay__ScrollContainerDataInternal), &context->internalArena);
            scrollOffset = SEClay__ScrollContainerDataInternalArray_Add(&context->scrollContainerDatas, SEClay__ScrollContainerDataInternal{.layoutElement = openLayoutElement, .scrollOrigin = {-1,-1}, .elementId = openLayoutElement->id, .openThisFrame = true});
            context->scrollIndexDirty = true; // an entry was added (the array may also have moved): reindex before the next lookup
        }
        if (context->externalScrollHandlingEnabled) {
            scrollOffset->scrollPosition = SEClay__QueryScrollOffset(scrollOffset->elementId, context->queryScrollOffsetUserData);
        }
    }
    if (!SEClay__MemCmp((char *)(&declaration->border.width), (char *)(&SEClay__BorderWidth_DEFAULT), sizeof(SEClay_BorderWidth))) {
        SEClay__AttachElementConfig(SEClay_ElementConfigUnion { .borderElementConfig = SEClay__StoreBorderElementConfig(declaration->border) }, SEClay__ElementConfigType::Border);
    }
}

void SEClay__ConfigureOpenElement(const SEClay_ElementDeclaration declaration) {
    SEClay__ConfigureOpenElementPtr(&declaration);
}

// HARDENING: arrays that legitimately hold several entries per element (configs, wrapped text lines, render commands,
// measure cache items) are sized as a multiple of maxElementCount. Saturates instead of overflowing int32.
static int32_t SEClay__ScaledCapacity(int32_t maxElementCount, int32_t factor) {
    int64_t scaled = (int64_t)maxElementCount * (int64_t)factor;
    return scaled > (int64_t)(1 << 26) ? (int32_t)(1 << 26) : (int32_t)scaled;
}

// HARDENING: bounds for the two user-settable sizes. Too small and bucket tables end up with zero buckets (a modulo by
// zero); too large and the size math / int32 capacities overflow.
static int32_t SEClay__ClampMaxElementCount(int32_t value) {
    return value < 16 ? 16 : (value > (1 << 20) ? (1 << 20) : value);
}

static int32_t SEClay__ClampMaxMeasureTextWordCount(int32_t value) {
    return value < 64 ? 64 : (value > (1 << 24) ? (1 << 24) : value);
}

void SEClay__InitializeEphemeralMemory(SEClay_Context* context) {
    int32_t maxElementCount = context->maxElementCount;
    // Ephemeral Memory - reset every frame
    // HARDENING+: a growable context keeps its per-frame arrays in a block of their own (so they can be replaced between frames
    // without moving anything persistent); a fixed-arena context carves them after the persistent ones as before.
    SEClay_Arena *arena = context->growable ? &context->ephemeralArena : &context->internalArena;
    arena->nextAllocation = context->growable ? 0 : context->arenaResetOffset;

    context->layoutElementChildrenBuffer = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    context->layoutElements = SEClay_LayoutElementArray_Allocate_Arena(maxElementCount, arena);
    // Was hardcoded to 100 regardless of maxElementCount -- scaled the same way as
    // scrollContainerDatas above (see SEClay__InitializePersistentMemory), with a 100-item floor so
    // small maxElementCount configs don't regress below the original default.
    context->warnings = SEClay__WarningArray_Allocate_Arena(
        maxElementCount > 256 ? 256 : (maxElementCount < 100 ? 100 : maxElementCount), arena);

    context->layoutConfigs = SEClay__LayoutConfigArray_Allocate_Arena(maxElementCount, arena);
    // HARDENING: one entry per attached config (background, border, clip, text, ...), several per element -- with
    // capacity == maxElementCount this overflowed long before the element budget did.
    context->elementConfigs = SEClay__ElementConfigArray_Allocate_Arena(SEClay__ScaledCapacity(maxElementCount, 4), arena);
    context->textElementConfigs = SEClay__TextElementConfigArray_Allocate_Arena(maxElementCount, arena);
    context->aspectRatioElementConfigs = SEClay__AspectRatioElementConfigArray_Allocate_Arena(maxElementCount, arena);
    context->imageElementConfigs = SEClay__ImageElementConfigArray_Allocate_Arena(maxElementCount, arena);
    context->floatingElementConfigs = SEClay__FloatingElementConfigArray_Allocate_Arena(maxElementCount, arena);
    context->clipElementConfigs = SEClay__ClipElementConfigArray_Allocate_Arena(maxElementCount, arena);
    context->customElementConfigs = SEClay__CustomElementConfigArray_Allocate_Arena(maxElementCount, arena);
    context->borderElementConfigs = SEClay__BorderElementConfigArray_Allocate_Arena(maxElementCount, arena);
    context->sharedElementConfigs = SEClay__SharedElementConfigArray_Allocate_Arena(maxElementCount, arena);

    context->layoutElementIdStrings = SEClay__StringArray_Allocate_Arena(maxElementCount, arena);
    // HARDENING: one entry per wrapped *line*, not per element.
    context->wrappedTextLines = SEClay__WrappedTextLineArray_Allocate_Arena(SEClay__ScaledCapacity(maxElementCount, 2), arena);
    context->layoutElementTreeNodeArray1 = SEClay__LayoutElementTreeNodeArray_Allocate_Arena(maxElementCount, arena);
    context->layoutElementTreeRoots = SEClay__LayoutElementTreeRootArray_Allocate_Arena(maxElementCount, arena);
    context->layoutElementChildren = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    context->openLayoutElementStack = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    context->textElementData = SEClay__TextElementDataArray_Allocate_Arena(maxElementCount, arena);
    context->aspectRatioElementIndexes = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    // HARDENING: an element can emit a rectangle, a border, a scissor pair and between-children dividers, and every
    // wrapped text line is a command of its own -- so this holds several commands per element. At capacity ==
    // maxElementCount the buffer filled up on a few thousand styled elements ("ran out of capacity while attempting to
    // create render commands") and the tail of the frame, including scissor ends, was silently dropped.
    context->renderCommands = SEClay_RenderCommandArray_Allocate_Arena(SEClay__ScaledCapacity(maxElementCount, 6), arena);
    context->treeNodeVisited = SEClay__boolArray_Allocate_Arena(maxElementCount, arena);
    context->treeNodeVisited.length = context->treeNodeVisited.capacity; // This array is accessed directly rather than behaving as a list
    context->openClipElementStack = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    context->reusableElementIndexBuffer = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    context->layoutElementClipElementIds = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    context->dynamicStringData = SEClay__charArray_Allocate_Arena(maxElementCount, arena);
}

void SEClay__InitializePersistentMemory(SEClay_Context* context) {
    // Persistent memory - initialized once and not reset
    int32_t maxElementCount = context->maxElementCount;
    int32_t maxMeasureTextCacheWordCount = context->maxMeasureTextCacheWordCount;
    SEClay_Arena *arena = context->growable ? &context->persistentArena : &context->internalArena;

    // Was hardcoded to 10 regardless of maxElementCount, with no public API to configure it -- every
    // element with .clip.horizontal/.vertical set (any UI::graph(), any ScrollArea, not just "real"
    // scrolling containers) claims one slot here, so any app with more than 10 such elements alive in a
    // single frame silently overflowed this array with no diagnosable error (just a generic "out of
    // bounds array access"). Scaled with maxElementCount (capped, since needing this many CLIPPED
    // elements in one frame would be unusual) instead of a bare literal.
    // HARDENING+: a growable context keeps this array on the heap from the start (it already grows there on demand), so replacing the
    // persistent block when the context resizes does not have to move scroll state.
    if (!context->growable) {
        context->scrollContainerDatas = SEClay__ScrollContainerDataInternalArray_Allocate_Arena(
            maxElementCount > 256 ? 256 : (maxElementCount < 10 ? 10 : maxElementCount), arena);
    }
    context->layoutElementsHashMapInternal = SEClay__LayoutElementHashMapItemArray_Allocate_Arena(maxElementCount, arena);
    context->layoutElementsHashMap = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    // HARDENING: entries live ~3 frames before they are evictable, so a UI whose strings all change every frame needs
    // room for several frames' worth of distinct strings.
    context->measureTextHashMapInternal = SEClay__MeasureTextCacheItemArray_Allocate_Arena(SEClay__ScaledCapacity(maxElementCount, 3), arena);
    context->measureTextHashMapInternalFreeList = SEClay__int32_tArray_Allocate_Arena(SEClay__ScaledCapacity(maxElementCount, 3), arena);
    context->measuredWordsFreeList = SEClay__int32_tArray_Allocate_Arena(maxMeasureTextCacheWordCount, arena);
    context->measureTextHashMap = SEClay__int32_tArray_Allocate_Arena(maxElementCount, arena);
    context->measuredWords = SEClay__MeasuredWordArray_Allocate_Arena(maxMeasureTextCacheWordCount, arena);
    context->pointerOverIds = SEClay_ElementIdArray_Allocate_Arena(maxElementCount, arena);
    context->debugElementData = SEClay__DebugElementDataArray_Allocate_Arena(maxElementCount, arena);
    context->arenaResetOffset = arena->nextAllocation;
}

// ---------------------------------------------------------------------------------------------------------------------------
// HARDENING+: growable contexts
// ---------------------------------------------------------------------------------------------------------------------------

static void *SEClay__DefaultAllocate(size_t size, void *userData) {
    (void)userData;
    return malloc(size);
}

static void SEClay__DefaultRelease(void *memory, size_t size, void *userData) {
    (void)size;
    (void)userData;
    free(memory);
}

// Zero-filled block from the context's allocator; NULL if it refuses (or the size is unreasonable).
static char *SEClay__AllocateBlock(const SEClay_Allocator *allocator, size_t size) {
    if (size == 0) {
        return NULL;
    }
    char *block = (char *)allocator->allocate(size, allocator->userData);
    if (block != NULL) {
        memset(block, 0, size);
    }
    return block;
}

static void SEClay__ReleaseBlock(const SEClay_Allocator *allocator, char *block, size_t size) {
    if (block != NULL) {
        allocator->release(block, size, allocator->userData);
    }
}

// How many bytes the two blocks of a growable context need for a given element budget and word budget. Done by running the real carve
// code against an arena with no memory behind it, so it can never disagree with what is actually carved.
static void SEClay__BlockSizesFor(int32_t elements, int32_t words, size_t *persistentBytes, size_t *ephemeralBytes) {
    SEClay_Context fake;
    memset((void *)&fake, 0, sizeof(fake));
    fake.growable = true;
    fake.maxElementCount = elements;
    fake.maxMeasureTextCacheWordCount = words;
    fake.persistentArena.capacity = SIZE_MAX;
    fake.ephemeralArena.capacity = SIZE_MAX;
    SEClay__InitializePersistentMemory(&fake);
    SEClay__InitializeEphemeralMemory(&fake);
    *persistentBytes = (size_t)fake.persistentArena.nextAllocation + 128u;
    *ephemeralBytes = (size_t)fake.ephemeralArena.nextAllocation + 128u;
}

// Replaces both blocks with ones sized for the new budgets, keeping what must survive a resize: the element-id map and the pointer-over
// list (the measured-text cache is dropped and refills; scroll state is on the heap and untouched). Only ever called between layouts.
// Returns false (leaving the context as it was) if the memory limit or the allocator forbids it.
static bool SEClay__ResizeGrowable(SEClay_Context *context, int32_t newElements, int32_t newWords) {
    newElements = SEClay__ClampMaxElementCount(newElements);
    newWords = SEClay__ClampMaxMeasureTextWordCount(newWords);
    size_t newPersistent = 0, newEphemeral = 0;
    SEClay__BlockSizesFor(newElements, newWords, &newPersistent, &newEphemeral);
    if (newPersistent + newEphemeral > context->memoryLimitBytes) {
        // Fit the largest budget the limit allows (scaling both budgets together), if that is still growth.
        const double fraction = (double)context->memoryLimitBytes / (double)(newPersistent + newEphemeral);
        const int32_t fitElements = SEClay__ClampMaxElementCount((int32_t)((double)newElements * fraction));
        const int32_t fitWords = SEClay__ClampMaxMeasureTextWordCount((int32_t)((double)newWords * fraction));
        const bool growsElements = fitElements > context->maxElementCount;
        if (!growsElements && fitWords <= context->maxMeasureTextCacheWordCount) {
            if (!context->memoryLimitReported) {
                context->memoryLimitReported = true;
                SECLAY__REPORT(SEClay_ErrorType::MemoryLimitReached, SEClay_ErrorDetail::MemoryLimit,
                             "The layout needs more memory than this context's limit allows, so it is truncated. Raise memoryLimitBytes (SEClay_GrowableConfig) or declare fewer elements. 'value' is the bytes wanted, 'limit' the limit.",
                             newPersistent + newEphemeral, context->memoryLimitBytes);
            }
            context->growthRetryAtGeneration = context->generation + 600;
            return false;
        }
        newElements = growsElements ? fitElements : context->maxElementCount;
        newWords = fitWords > context->maxMeasureTextCacheWordCount ? fitWords : context->maxMeasureTextCacheWordCount;
        SEClay__BlockSizesFor(newElements, newWords, &newPersistent, &newEphemeral);
        if (!context->memoryLimitReported) {
            context->memoryLimitReported = true;
            SECLAY__REPORT(SEClay_ErrorType::MemoryLimitReached, SEClay_ErrorDetail::MemoryLimit,
                         "The layout wants more memory than this context's limit allows; it grew to the limit and may still be truncated. 'value' is the bytes it now holds, 'limit' the limit.",
                         newPersistent + newEphemeral, context->memoryLimitBytes);
        }
    }
    char *persistent = SEClay__AllocateBlock(&context->allocator, newPersistent);
    char *ephemeral = persistent != NULL ? SEClay__AllocateBlock(&context->allocator, newEphemeral) : NULL;
    if (persistent == NULL || ephemeral == NULL) {
        SEClay__ReleaseBlock(&context->allocator, persistent, newPersistent);
        SEClay__ReleaseBlock(&context->allocator, ephemeral, newEphemeral);
        context->growthRetryAtGeneration = context->generation + 120;
        SECLAY__REPORT(SEClay_ErrorType::AllocationFailed, SEClay_ErrorDetail::Allocation,
                     "The allocator refused the memory a larger layout needs; the context keeps its current size and will retry. 'value' is the bytes requested.",
                     newPersistent + newEphemeral, 0);
        return false;
    }

    const int32_t previousElements = context->maxElementCount;
    const int32_t previousWords = context->maxMeasureTextCacheWordCount;
    const bool growing = newElements > previousElements || newWords > previousWords;

    SEClay__LayoutElementHashMapItemArray oldItems = context->layoutElementsHashMapInternal;
    SEClay_ElementIdArray oldPointerOver = context->pointerOverIds;
    char *oldPersistent = context->persistentBlock;
    const size_t oldPersistentSize = context->persistentBlockSize;

    context->maxElementCount = newElements;
    context->maxMeasureTextCacheWordCount = newWords;
    context->persistentBlock = persistent;
    context->persistentBlockSize = newPersistent;
    context->persistentArena.memory = persistent;
    context->persistentArena.capacity = newPersistent;
    context->persistentArena.nextAllocation = 0;
    context->arenaAllocationFailed = false;
    SEClay__InitializePersistentMemory(context);
    for (int32_t i = 0; i < context->layoutElementsHashMap.capacity; ++i) {
        context->layoutElementsHashMap.internalArray[i] = -1;
    }
    for (int32_t i = 0; i < context->measureTextHashMap.capacity; ++i) {
        context->measureTextHashMap.internalArray[i] = 0;
    }
    context->measureTextHashMapInternal.length = 1; // 0 means "no next entry"
    context->measureSweepBucket = 0;

    int32_t keep = oldItems.length < context->layoutElementsHashMapInternal.capacity - 1 ? oldItems.length : context->layoutElementsHashMapInternal.capacity - 1;
    if (keep > 0 && oldItems.internalArray != NULL) {
        memcpy((void *)context->layoutElementsHashMapInternal.internalArray, oldItems.internalArray, (size_t)keep * sizeof(SEClay_LayoutElementHashMapItem));
        context->layoutElementsHashMapInternal.length = keep;
        SEClay__RebuildElementHashMap(context);
    }
    int32_t keepOver = oldPointerOver.length < context->pointerOverIds.capacity ? oldPointerOver.length : context->pointerOverIds.capacity;
    if (keepOver > 0 && oldPointerOver.internalArray != NULL) {
        memcpy((void *)context->pointerOverIds.internalArray, oldPointerOver.internalArray, (size_t)keepOver * sizeof(SEClay_ElementId));
        context->pointerOverIds.length = keepOver;
    }
    SEClay__ReleaseBlock(&context->allocator, oldPersistent, oldPersistentSize);

    // The old per-frame block stays alive for a couple of frames (scroll containers and the id map still point into it until the
    // next layout re-registers them); anything older is released now.
    SEClay__ReleaseBlock(&context->allocator, context->retiredBlock, context->retiredBlockSize);
    context->retiredBlock = context->ephemeralBlock;
    context->retiredBlockSize = context->ephemeralBlockSize;
    context->retiredAtGeneration = context->generation;
    context->ephemeralBlock = ephemeral;
    context->ephemeralBlockSize = newEphemeral;
    context->ephemeralArena.memory = ephemeral;
    context->ephemeralArena.capacity = newEphemeral;
    context->ephemeralArena.nextAllocation = 0;

    context->lowUsageFrames = 0;
    if (growing) { context->growCount++; } else { context->shrinkCount++; }
    if (growing) {
        SECLAY__REPORT(SEClay_ErrorType::Resized, SEClay_ErrorDetail::Grew,
                     "The layout context grew to fit the layout. 'value' is the new element budget, 'limit' the previous one.",
                     newElements, previousElements);
    } else {
        SECLAY__REPORT(SEClay_ErrorType::Resized, SEClay_ErrorDetail::Shrank,
                     "The layout context gave memory back after sustained low use. 'value' is the new element budget, 'limit' the previous one.",
                     newElements, previousElements);
    }
    return true;
}

// The ratio of what a buffer was asked for to what it holds.
static double SEClay__Utilisation(int64_t used, int64_t capacity) {
    return capacity > 0 ? (double)used / (double)capacity : 1.0;
}

// Runs at the start of every layout of a growable context: decides, from what the previous layout asked for, whether the buffers are
// about to be too small (grow, with room to spare) or have been far too large for a long time (shrink, if allowed).
static void SEClay__AdjustMemoryForNextLayout(SEClay_Context *context) {
    if (!context->growable || context->layoutElements.capacity <= 0) {
        return;
    }
    if (context->retiredBlock != NULL && context->generation >= context->retiredAtGeneration + 3) {
        SEClay__ReleaseBlock(&context->allocator, context->retiredBlock, context->retiredBlockSize);
        context->retiredBlock = NULL;
        context->retiredBlockSize = 0;
    }
    if (context->generation < context->growthRetryAtGeneration) {
        return;
    }
    double elementRatio = SEClay__Utilisation(context->lastUsageElements, context->layoutElements.capacity);
    double ratio = elementRatio;
    double r;
    r = SEClay__Utilisation(context->lastUsageElementConfigs, context->elementConfigs.capacity); if (r > ratio) { ratio = r; }
    r = SEClay__Utilisation(context->lastUsageWrappedLines, context->wrappedTextLines.capacity); if (r > ratio) { ratio = r; }
    r = SEClay__Utilisation(context->lastUsageRenderCommands, context->renderCommands.capacity); if (r > ratio) { ratio = r; }
    // Persistent tables: the id map holds one entry per live id, the measure cache one item per distinct string seen recently.
    r = SEClay__Utilisation(context->layoutElementsHashMapInternal.length, context->layoutElementsHashMapInternal.capacity); if (r > ratio) { ratio = r; }
    r = SEClay__Utilisation(context->measureTextHashMapInternal.length - context->measureTextHashMapInternalFreeList.length, context->measureTextHashMapInternal.capacity); if (r > ratio) { ratio = r; }
    // Strings that did not fit count as demand: the cache wanted that many more items than it holds.
    r = SEClay__Utilisation((int64_t)context->measureTextHashMapInternal.length - context->measureTextHashMapInternalFreeList.length + context->usageMeasureItemFailures,
                          context->measureTextHashMapInternal.capacity);
    if (r > ratio) { ratio = r; }
    // Words are budgeted separately because their count is not proportional to the element count; the words that did not fit are
    // counted too, so one resize is enough.
    double wordRatio = SEClay__Utilisation((int64_t)context->measuredWords.length - context->measuredWordsFreeList.length + context->usageMeasureWordShortfall,
                                         context->measuredWords.capacity);

    int32_t newElements = context->maxElementCount;
    int32_t newWords = context->maxMeasureTextCacheWordCount;
    bool change = false;
    if (ratio > 0.75) {
        // Aim to sit at about half full at this load, never growing by less than half again.
        double scale = ratio / 0.5;
        if (scale < 1.5) { scale = 1.5; }
        newElements = (int32_t)((double)context->maxElementCount * scale + 1.0);
        change = true;
    }
    if (wordRatio > 0.75) {
        double scale = wordRatio / 0.5;
        if (scale < 1.5) { scale = 1.5; }
        newWords = (int32_t)((double)context->maxMeasureTextCacheWordCount * scale + 1.0);
        change = true;
    }
    if (!change && context->allowShrink &&
        (context->maxElementCount > context->initialElements || context->maxMeasureTextCacheWordCount > context->initialWords) &&
        ratio < 0.2 && wordRatio < 0.2) {
        // About 30 seconds at 60 fps of the layout using under a fifth of what is held.
        if (++context->lowUsageFrames > 1800) {
            newElements = context->maxElementCount / 2 > context->initialElements ? context->maxElementCount / 2 : context->initialElements;
            newWords = context->maxMeasureTextCacheWordCount / 2 > context->initialWords ? context->maxMeasureTextCacheWordCount / 2 : context->initialWords;
            change = newElements != context->maxElementCount || newWords != context->maxMeasureTextCacheWordCount;
            context->lowUsageFrames = 0;
        }
    } else if (!change) {
        context->lowUsageFrames = 0;
    }
    if (change) {
        (void)SEClay__ResizeGrowable(context, newElements, newWords);
    }
}

const float SECLAY__EPSILON = 0.01;

bool SEClay__FloatEqual(float left, float right) {
    float subtracted = left - right;
    return subtracted < SECLAY__EPSILON && subtracted > -SECLAY__EPSILON;
}

void SEClay__SizeContainersAlongAxis(bool xAxis) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    SEClay__int32_tArray bfsBuffer = context->layoutElementChildrenBuffer;
    SEClay__int32_tArray resizableContainerBuffer = context->openLayoutElementStack;
    for (int32_t rootIndex = 0; rootIndex < context->layoutElementTreeRoots.length; ++rootIndex) {
        bfsBuffer.length = 0;
        SEClay__LayoutElementTreeRoot *root = SEClay__LayoutElementTreeRootArray_Get(&context->layoutElementTreeRoots, rootIndex);
        SEClay_LayoutElement *rootElement = SEClay_LayoutElementArray_Get(&context->layoutElements, (int)root->layoutElementIndex);
        SEClay__int32_tArray_Add(&bfsBuffer, (int32_t)root->layoutElementIndex);

        // Size floating containers to their parents
        if (SEClay__ElementHasConfig(rootElement, SEClay__ElementConfigType::Floating)) {
            SEClay_FloatingElementConfig *floatingElementConfig = SEClay__FindElementConfigWithType(rootElement, SEClay__ElementConfigType::Floating).floatingElementConfig;
            SEClay_LayoutElementHashMapItem *parentItem = SEClay__GetHashMapItem(floatingElementConfig->parentId);
            if (parentItem && parentItem != &SEClay_LayoutElementHashMapItem_DEFAULT) {
                SEClay_LayoutElement *parentLayoutElement = parentItem->layoutElement;
                if (rootElement->layoutConfig->sizing.width.type == SEClay__SizingType::Grow) {
                    rootElement->dimensions.width = parentLayoutElement->dimensions.width;
                }
                if (rootElement->layoutConfig->sizing.height.type == SEClay__SizingType::Grow) {
                    rootElement->dimensions.height = parentLayoutElement->dimensions.height;
                }
            }
        }

        rootElement->dimensions.width = SECLAY__MIN(SECLAY__MAX(rootElement->dimensions.width, rootElement->layoutConfig->sizing.width.size.minMax.min), rootElement->layoutConfig->sizing.width.size.minMax.max);
        rootElement->dimensions.height = SECLAY__MIN(SECLAY__MAX(rootElement->dimensions.height, rootElement->layoutConfig->sizing.height.size.minMax.min), rootElement->layoutConfig->sizing.height.size.minMax.max);

        for (int32_t i = 0; i < bfsBuffer.length; ++i) {
            int32_t parentIndex = SEClay__int32_tArray_GetValue(&bfsBuffer, i);
            SEClay_LayoutElement *parent = SEClay_LayoutElementArray_Get(&context->layoutElements, parentIndex);
            SEClay_LayoutConfig *parentStyleConfig = parent->layoutConfig;
            int32_t growContainerCount = 0;
            float parentSize = xAxis ? parent->dimensions.width : parent->dimensions.height;
            float parentPadding = (float)(xAxis ? (parent->layoutConfig->padding.left + parent->layoutConfig->padding.right) : (parent->layoutConfig->padding.top + parent->layoutConfig->padding.bottom));
            float innerContentSize = 0, totalPaddingAndChildGaps = parentPadding;
            bool sizingAlongAxis = (xAxis && parentStyleConfig->layoutDirection == SEClay_LayoutDirection::LeftToRight) || (!xAxis && parentStyleConfig->layoutDirection == SEClay_LayoutDirection::TopToBottom);
            resizableContainerBuffer.length = 0;
            float parentChildGap = parentStyleConfig->childGap;

            for (int32_t childOffset = 0; childOffset < parent->childrenOrTextContent.children.length; childOffset++) {
                int32_t childElementIndex = parent->childrenOrTextContent.children.elements[childOffset];
                SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, childElementIndex);
                SEClay_SizingAxis childSizing = xAxis ? childElement->layoutConfig->sizing.width : childElement->layoutConfig->sizing.height;
                float childSize = xAxis ? childElement->dimensions.width : childElement->dimensions.height;

                if (!SEClay__ElementHasConfig(childElement, SEClay__ElementConfigType::Text) && childElement->childrenOrTextContent.children.length > 0) {
                    SEClay__int32_tArray_Add(&bfsBuffer, childElementIndex);
                }

                if (childSizing.type != SEClay__SizingType::Percent
                    && childSizing.type != SEClay__SizingType::Fixed
                    && (!SEClay__ElementHasConfig(childElement, SEClay__ElementConfigType::Text) || (SEClay__FindElementConfigWithType(childElement, SEClay__ElementConfigType::Text).textElementConfig->wrapMode == SEClay_TextElementConfigWrapMode::Words)) // todo too many loops
//                    && (xAxis || !SEClay__ElementHasConfig(childElement, SEClay__ElementConfigType::Aspect))
                ) {
                    SEClay__int32_tArray_Add(&resizableContainerBuffer, childElementIndex);
                }

                if (sizingAlongAxis) {
                    innerContentSize += (childSizing.type == SEClay__SizingType::Percent ? 0 : childSize);
                    if (childSizing.type == SEClay__SizingType::Grow) {
                        growContainerCount++;
                    }
                    if (childOffset > 0) {
                        innerContentSize += parentChildGap; // For children after index 0, the childAxisOffset is the gap from the previous child
                        totalPaddingAndChildGaps += parentChildGap;
                    }
                } else {
                    innerContentSize = SECLAY__MAX(childSize, innerContentSize);
                }
            }

            // Expand percentage containers to size
            for (int32_t childOffset = 0; childOffset < parent->childrenOrTextContent.children.length; childOffset++) {
                int32_t childElementIndex = parent->childrenOrTextContent.children.elements[childOffset];
                SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, childElementIndex);
                SEClay_SizingAxis childSizing = xAxis ? childElement->layoutConfig->sizing.width : childElement->layoutConfig->sizing.height;
                float *childSize = xAxis ? &childElement->dimensions.width : &childElement->dimensions.height;
                if (childSizing.type == SEClay__SizingType::Percent) {
                    *childSize = (parentSize - totalPaddingAndChildGaps) * childSizing.size.percent;
                    if (sizingAlongAxis) {
                        innerContentSize += *childSize;
                    }
                    SEClay__UpdateAspectRatioBox(childElement);
                }
            }

            if (sizingAlongAxis) {
                float sizeToDistribute = parentSize - parentPadding - innerContentSize;
                // The content is too large, compress the children as much as possible
                if (sizeToDistribute < 0) {
                    // If the parent clips content in this axis direction, don't compress children, just leave them alone
                    SEClay_ClipElementConfig *clipElementConfig = SEClay__FindElementConfigWithType(parent, SEClay__ElementConfigType::Clip).clipElementConfig;
                    if (clipElementConfig) {
                        if (((xAxis && clipElementConfig->horizontal) || (!xAxis && clipElementConfig->vertical))) {
                            continue;
                        }
                    }
                    // Scrolling containers preferentially compress before others
                    while (sizeToDistribute < -SECLAY__EPSILON && resizableContainerBuffer.length > 0) {
                        float largest = 0;
                        float secondLargest = 0;
                        float widthToAdd = sizeToDistribute;
                        for (int childIndex = 0; childIndex < resizableContainerBuffer.length; childIndex++) {
                            SEClay_LayoutElement *child = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&resizableContainerBuffer, childIndex));
                            float childSize = xAxis ? child->dimensions.width : child->dimensions.height;
                            if (SEClay__FloatEqual(childSize, largest)) { continue; }
                            if (childSize > largest) {
                                secondLargest = largest;
                                largest = childSize;
                            }
                            if (childSize < largest) {
                                secondLargest = SECLAY__MAX(secondLargest, childSize);
                                widthToAdd = secondLargest - largest;
                            }
                        }

                        widthToAdd = SECLAY__MAX(widthToAdd, sizeToDistribute / resizableContainerBuffer.length);

                        for (int childIndex = 0; childIndex < resizableContainerBuffer.length; childIndex++) {
                            SEClay_LayoutElement *child = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&resizableContainerBuffer, childIndex));
                            float *childSize = xAxis ? &child->dimensions.width : &child->dimensions.height;
                            float minSize = xAxis ? child->minDimensions.width : child->minDimensions.height;
                            float previousWidth = *childSize;
                            if (SEClay__FloatEqual(*childSize, largest)) {
                                *childSize += widthToAdd;
                                if (*childSize <= minSize) {
                                    *childSize = minSize;
                                    SEClay__int32_tArray_RemoveSwapback(&resizableContainerBuffer, childIndex--);
                                }
                                sizeToDistribute -= (*childSize - previousWidth);
                            }
                        }
                    }
                // The content is too small, allow SIZING_GROW containers to expand
                } else if (sizeToDistribute > 0 && growContainerCount > 0) {
                    for (int childIndex = 0; childIndex < resizableContainerBuffer.length; childIndex++) {
                        SEClay_LayoutElement *child = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&resizableContainerBuffer, childIndex));
                        SEClay__SizingType childSizing = xAxis ? child->layoutConfig->sizing.width.type : child->layoutConfig->sizing.height.type;
                        if (childSizing != SEClay__SizingType::Grow) {
                            SEClay__int32_tArray_RemoveSwapback(&resizableContainerBuffer, childIndex--);
                        }
                    }
                    while (sizeToDistribute > SECLAY__EPSILON && resizableContainerBuffer.length > 0) {
                        float smallest = SECLAY__MAXFLOAT;
                        float secondSmallest = SECLAY__MAXFLOAT;
                        float widthToAdd = sizeToDistribute;
                        for (int childIndex = 0; childIndex < resizableContainerBuffer.length; childIndex++) {
                            SEClay_LayoutElement *child = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&resizableContainerBuffer, childIndex));
                            float childSize = xAxis ? child->dimensions.width : child->dimensions.height;
                            if (SEClay__FloatEqual(childSize, smallest)) { continue; }
                            if (childSize < smallest) {
                                secondSmallest = smallest;
                                smallest = childSize;
                            }
                            if (childSize > smallest) {
                                secondSmallest = SECLAY__MIN(secondSmallest, childSize);
                                widthToAdd = secondSmallest - smallest;
                            }
                        }

                        widthToAdd = SECLAY__MIN(widthToAdd, sizeToDistribute / resizableContainerBuffer.length);

                        for (int childIndex = 0; childIndex < resizableContainerBuffer.length; childIndex++) {
                            SEClay_LayoutElement *child = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&resizableContainerBuffer, childIndex));
                            float *childSize = xAxis ? &child->dimensions.width : &child->dimensions.height;
                            float maxSize = xAxis ? child->layoutConfig->sizing.width.size.minMax.max : child->layoutConfig->sizing.height.size.minMax.max;
                            float previousWidth = *childSize;
                            if (SEClay__FloatEqual(*childSize, smallest)) {
                                *childSize += widthToAdd;
                                if (*childSize >= maxSize) {
                                    *childSize = maxSize;
                                    SEClay__int32_tArray_RemoveSwapback(&resizableContainerBuffer, childIndex--);
                                }
                                sizeToDistribute -= (*childSize - previousWidth);
                            }
                        }
                    }
                }
            // Sizing along the non layout axis ("off axis")
            } else {
                for (int32_t childOffset = 0; childOffset < resizableContainerBuffer.length; childOffset++) {
                    SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&resizableContainerBuffer, childOffset));
                    SEClay_SizingAxis childSizing = xAxis ? childElement->layoutConfig->sizing.width : childElement->layoutConfig->sizing.height;
                    float minSize = xAxis ? childElement->minDimensions.width : childElement->minDimensions.height;
                    float *childSize = xAxis ? &childElement->dimensions.width : &childElement->dimensions.height;

                    float maxSize = parentSize - parentPadding;
                    // If we're laying out the children of a scroll panel, grow containers expand to the size of the inner content, not the outer container
                    if (SEClay__ElementHasConfig(parent, SEClay__ElementConfigType::Clip)) {
                        SEClay_ClipElementConfig *clipElementConfig = SEClay__FindElementConfigWithType(parent, SEClay__ElementConfigType::Clip).clipElementConfig;
                        if (((xAxis && clipElementConfig->horizontal) || (!xAxis && clipElementConfig->vertical))) {
                            maxSize = SECLAY__MAX(maxSize, innerContentSize);
                        }
                    }
                    if (childSizing.type == SEClay__SizingType::Grow) {
                        *childSize = SECLAY__MIN(maxSize, childSizing.size.minMax.max);
                    }
                    *childSize = SECLAY__MAX(minSize, SECLAY__MIN(*childSize, maxSize));
                }
            }
        }
    }
}

SEClay_String SEClay__IntToString(int32_t integer) {
    if (integer == 0) {
        return SEClay_String { .length = 1, .chars = "0" };
    }
    SEClay_Context* context = SEClay_GetCurrentContext();
    // HARDENING: an int32 needs at most 11 characters ("-2147483648"). The write used to be unchecked against the
    // dynamic string buffer, and negating INT32_MIN overflowed.
    enum { SECLAY__INT_STRING_MAX_CHARS = 11 };
    if (context->dynamicStringData.internalArray == NULL || context->dynamicStringData.length < 0 ||
        context->dynamicStringData.length > context->dynamicStringData.capacity - SECLAY__INT_STRING_MAX_CHARS) {
        SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::DynamicStrings,
                     "Clay ran out of space for dynamically generated strings (debug view numbers); a '?' was drawn instead.",
                     context->dynamicStringData.length, context->dynamicStringData.capacity);
        return SEClay_String { .length = 1, .chars = "?" };
    }
    char *chars = (char *)(context->dynamicStringData.internalArray + context->dynamicStringData.length);
    int32_t length = 0;
    int64_t sign = integer;
    int64_t magnitude = integer;

    if (magnitude < 0) {
        magnitude = -magnitude;
    }
    while (magnitude > 0) {
        chars[length++] = (char)(magnitude % 10 + '0');
        magnitude /= 10;
    }

    if (sign < 0) {
        chars[length++] = '-';
    }

    // Reverse the string to get the correct order
    for (int32_t j = 0, k = length - 1; j < k; j++, k--) {
        char temp = chars[j];
        chars[j] = chars[k];
        chars[k] = temp;
    }
    context->dynamicStringData.length += length;
    return SEClay_String { .length = length, .chars = chars };
}

void SEClay__AddRenderCommand(SEClay_RenderCommand renderCommand) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->usageRenderCommands++;
    if (context->renderCommands.length < context->renderCommands.capacity - 1) {
        SEClay_RenderCommandArray_Add(&context->renderCommands, renderCommand);
    } else {
        if (!context->booleanWarnings.maxRenderCommandsExceeded) {
            context->booleanWarnings.maxRenderCommandsExceeded = true;
            SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::RenderCommands,
                         "Clay ran out of room for render commands; the end of this frame's draw list was dropped.",
                         context->usageRenderCommands, context->renderCommands.capacity);
        }
    }
}

bool SEClay__ElementIsOffscreen(SEClay_BoundingBox *boundingBox) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->disableCulling) {
        return false;
    }

    return (boundingBox->x > (float)context->layoutDimensions.width) ||
           (boundingBox->y > (float)context->layoutDimensions.height) ||
           (boundingBox->x + boundingBox->width < 0) ||
           (boundingBox->y + boundingBox->height < 0);
}

// HARDENING: pushes onto a layout DFS stack, keeping the parallel `treeNodeVisited` flags in step. The flags were
// indexed by stack depth with no check, so a full stack wrote one past the end of the flag array.
static bool SEClay__PushTreeNode(SEClay_Context *context, SEClay__LayoutElementTreeNodeArray *dfsBuffer, SEClay__LayoutElementTreeNode node) {
    if (dfsBuffer->internalArray == NULL || context->treeNodeVisited.internalArray == NULL ||
        dfsBuffer->length < 0 || dfsBuffer->length >= dfsBuffer->capacity || dfsBuffer->length >= context->treeNodeVisited.capacity) {
        SECLAY__REPORT(SEClay_ErrorType::InternalError, SEClay_ErrorDetail::TraversalStack,
                     "Clay's layout traversal stack overflowed; part of the layout was skipped. This should not happen for a well-formed tree.",
                     dfsBuffer->length, dfsBuffer->capacity);
        return false;
    }
    context->treeNodeVisited.internalArray[dfsBuffer->length] = false;
    SEClay__LayoutElementTreeNodeArray_Add(dfsBuffer, node);
    return true;
}

// HARDENING: appends a wrapped line and counts it on the text element -- but only if it was really stored (the shared
// array has a fixed capacity; the old code counted lines it had dropped, so the element's line slice then ran past the
// end of the array into neighbouring arena memory when the commands were generated). Also validates that the line lies
// inside the element's own text, which is the invariant the renderer relies on when it reads the characters.
static bool SEClay__PushWrappedLine(SEClay_Context *context, SEClay__TextElementData *textElementData, SEClay_Dimensions dimensions, int32_t startOffset, int32_t length) {
    if (startOffset < 0 || length < 0 || (int64_t)startOffset + (int64_t)length > (int64_t)textElementData->text.length) {
        return true; // Not representable; skip the line but keep going.
    }
    context->usageWrappedLines++;
    if (context->wrappedTextLines.internalArray == NULL || context->wrappedTextLines.length >= context->wrappedTextLines.capacity) {
        if (!context->booleanWarnings.maxWrappedLinesExceeded) {
            context->booleanWarnings.maxWrappedLinesExceeded = true;
            SECLAY__REPORT(SEClay_ErrorType::ElementsCapacityExceeded, SEClay_ErrorDetail::WrappedTextLines,
                         "Clay ran out of room for wrapped text lines; the rest of this frame's text is not drawn.",
                         context->usageWrappedLines, context->wrappedTextLines.capacity);
        }
        return false;
    }
    const char *chars = textElementData->text.chars != NULL ? &textElementData->text.chars[startOffset] : "";
    SEClay__WrappedTextLineArray_Add(&context->wrappedTextLines, SEClay__WrappedTextLine { dimensions, { .length = length, .chars = chars } });
    textElementData->wrappedLines.length++;
    return true;
}

void SEClay__CalculateFinalLayout(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    // Calculate sizing along the X axis
    SEClay__SizeContainersAlongAxis(true);

    // Wrap text
    for (int32_t textElementIndex = 0; textElementIndex < context->textElementData.length; ++textElementIndex) {
        SEClay__TextElementData *textElementData = SEClay__TextElementDataArray_Get(&context->textElementData, textElementIndex);
        textElementData->wrappedLines = SEClay__WrappedTextLineArraySlice { .length = 0, .internalArray = context->wrappedTextLines.internalArray ? &context->wrappedTextLines.internalArray[context->wrappedTextLines.length] : NULL };
        SEClay_LayoutElement *containerElement = SEClay_LayoutElementArray_Get(&context->layoutElements, (int)textElementData->elementIndex);
        SEClay_TextElementConfig *textConfig = SEClay__FindElementConfigWithType(containerElement, SEClay__ElementConfigType::Text).textElementConfig;
        if (textConfig == NULL) {
            continue;
        }
        SEClay__MeasureTextCacheItem *measureTextCacheItem = SEClay__MeasureTextCached(&textElementData->text, textConfig);
        float lineWidth = 0;
        float lineHeight = textConfig->lineHeight > 0 ? (float)textConfig->lineHeight : textElementData->preferredDimensions.height;
        int32_t lineLengthChars = 0;
        int32_t lineStartOffset = 0;
        if (!measureTextCacheItem->containsNewlines && textElementData->preferredDimensions.width <= containerElement->dimensions.width) {
            SEClay__PushWrappedLine(context, textElementData, containerElement->dimensions, 0, textElementData->text.length);
            continue;
        }
        float spaceWidth = SEClay__MeasureText(SEClay_StringSlice { .length = 1, .chars = SECLAY__SPACECHAR.chars, .baseChars = SECLAY__SPACECHAR.chars }, textConfig, context->measureTextUserData).width;
        int32_t wordIndex = measureTextCacheItem->measuredWordsStartIndex;
        // HARDENING: every iteration either consumes a word or emits a line, but cap the work anyway so a corrupt
        // word chain can never hang the frame.
        int64_t remainingIterations = (int64_t)context->measuredWords.length * 2 + 16;
        while (wordIndex != -1 && remainingIterations-- > 0) {
            SEClay__MeasuredWord *measuredWord = SEClay__GetMeasuredWordChecked(context, wordIndex);
            if (measuredWord == NULL) {
                break;
            }
            // Only word on the line is too large, just render it anyway
            if (lineLengthChars == 0 && lineWidth + measuredWord->width > containerElement->dimensions.width) {
                // A line that does not fit is dropped but still counted (the loop keeps going), so a growable context learns how many
                // lines this frame really needed instead of only that it needed more than it had.
                (void)SEClay__PushWrappedLine(context, textElementData, SEClay_Dimensions { measuredWord->width, lineHeight }, measuredWord->startOffset, measuredWord->length);
                wordIndex = measuredWord->next;
                lineStartOffset = measuredWord->startOffset + measuredWord->length;
            }
            // measuredWord->length == 0 means a newline character
            else if (measuredWord->length == 0 || lineWidth + measuredWord->width > containerElement->dimensions.width) {
                // HARDENING: with nothing on the line yet (text that starts with a newline) this looked at text[-1].
                int32_t lastCharOffset = lineStartOffset + lineLengthChars - 1;
                bool finalCharIsSpace = lastCharOffset >= 0 && lastCharOffset < textElementData->text.length && textElementData->text.chars[lastCharOffset] == ' ';
                (void)SEClay__PushWrappedLine(context, textElementData, SEClay_Dimensions { lineWidth + (finalCharIsSpace ? -spaceWidth : 0), lineHeight }, lineStartOffset, lineLengthChars + (finalCharIsSpace ? -1 : 0));
                if (lineLengthChars == 0 || measuredWord->length == 0) {
                    wordIndex = measuredWord->next;
                }
                lineWidth = 0;
                lineLengthChars = 0;
                lineStartOffset = measuredWord->startOffset;
            } else {
                lineWidth += measuredWord->width + textConfig->letterSpacing;
                lineLengthChars += measuredWord->length;
                wordIndex = measuredWord->next;
            }
        }
        if (lineLengthChars > 0) {
            SEClay__PushWrappedLine(context, textElementData, SEClay_Dimensions { lineWidth - textConfig->letterSpacing, lineHeight }, lineStartOffset, lineLengthChars);
        }
        containerElement->dimensions.height = lineHeight * (float)textElementData->wrappedLines.length;
    }

    // Scale vertical heights according to aspect ratio
    for (int32_t i = 0; i < context->aspectRatioElementIndexes.length; ++i) {
        SEClay_LayoutElement* aspectElement = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&context->aspectRatioElementIndexes, i));
        SEClay_AspectRatioElementConfig *config = SEClay__FindElementConfigWithType(aspectElement, SEClay__ElementConfigType::Aspect).aspectRatioElementConfig;
        aspectElement->dimensions.height = (1 / config->aspectRatio) * aspectElement->dimensions.width;
        aspectElement->layoutConfig->sizing.height.size.minMax.max = aspectElement->dimensions.height;
    }

    // Propagate effect of text wrapping, aspect scaling etc. on height of parents
    SEClay__LayoutElementTreeNodeArray dfsBuffer = context->layoutElementTreeNodeArray1;
    dfsBuffer.length = 0;
    for (int32_t i = 0; i < context->layoutElementTreeRoots.length; ++i) {
        SEClay__LayoutElementTreeRoot *root = SEClay__LayoutElementTreeRootArray_Get(&context->layoutElementTreeRoots, i);
        SEClay__PushTreeNode(context, &dfsBuffer, SEClay__LayoutElementTreeNode { .layoutElement = SEClay_LayoutElementArray_Get(&context->layoutElements, (int)root->layoutElementIndex) });
    }
    while (dfsBuffer.length > 0) {
        SEClay__LayoutElementTreeNode *currentElementTreeNode = SEClay__LayoutElementTreeNodeArray_Get(&dfsBuffer, (int)dfsBuffer.length - 1);
        SEClay_LayoutElement *currentElement = currentElementTreeNode->layoutElement;
        if (!context->treeNodeVisited.internalArray[dfsBuffer.length - 1]) {
            context->treeNodeVisited.internalArray[dfsBuffer.length - 1] = true;
            // If the element has no children or is the container for a text element, don't bother inspecting it
            if (SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Text) || currentElement->childrenOrTextContent.children.length == 0) {
                dfsBuffer.length--;
                continue;
            }
            // Add the children to the DFS buffer (needs to be pushed in reverse so that stack traversal is in correct layout order)
            for (int32_t i = 0; i < currentElement->childrenOrTextContent.children.length; i++) {
                SEClay__PushTreeNode(context, &dfsBuffer, SEClay__LayoutElementTreeNode { .layoutElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[i]) });
            }
            continue;
        }
        dfsBuffer.length--;

        // DFS node has been visited, this is on the way back up to the root
        SEClay_LayoutConfig *layoutConfig = currentElement->layoutConfig;
        if (layoutConfig->layoutDirection == SEClay_LayoutDirection::LeftToRight) {
            // Resize any parent containers that have grown in height along their non layout axis
            for (int32_t j = 0; j < currentElement->childrenOrTextContent.children.length; ++j) {
                SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[j]);
                float childHeightWithPadding = SECLAY__MAX(childElement->dimensions.height + layoutConfig->padding.top + layoutConfig->padding.bottom, currentElement->dimensions.height);
                currentElement->dimensions.height = SECLAY__MIN(SECLAY__MAX(childHeightWithPadding, layoutConfig->sizing.height.size.minMax.min), layoutConfig->sizing.height.size.minMax.max);
            }
        } else if (layoutConfig->layoutDirection == SEClay_LayoutDirection::TopToBottom) {
            // Resizing along the layout axis
            float contentHeight = (float)(layoutConfig->padding.top + layoutConfig->padding.bottom);
            for (int32_t j = 0; j < currentElement->childrenOrTextContent.children.length; ++j) {
                SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[j]);
                contentHeight += childElement->dimensions.height;
            }
            contentHeight += (float)(SECLAY__MAX(currentElement->childrenOrTextContent.children.length - 1, 0) * layoutConfig->childGap);
            currentElement->dimensions.height = SECLAY__MIN(SECLAY__MAX(contentHeight, layoutConfig->sizing.height.size.minMax.min), layoutConfig->sizing.height.size.minMax.max);
        }
    }

    // Calculate sizing along the Y axis
    SEClay__SizeContainersAlongAxis(false);

    // Scale horizontal widths according to aspect ratio
    for (int32_t i = 0; i < context->aspectRatioElementIndexes.length; ++i) {
        SEClay_LayoutElement* aspectElement = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&context->aspectRatioElementIndexes, i));
        SEClay_AspectRatioElementConfig *config = SEClay__FindElementConfigWithType(aspectElement, SEClay__ElementConfigType::Aspect).aspectRatioElementConfig;
        aspectElement->dimensions.width = config->aspectRatio * aspectElement->dimensions.height;
    }

    // Sort tree roots by z-index
    int32_t sortMax = context->layoutElementTreeRoots.length - 1;
    while (sortMax > 0) { // todo dumb bubble sort
        for (int32_t i = 0; i < sortMax; ++i) {
            SEClay__LayoutElementTreeRoot current = *SEClay__LayoutElementTreeRootArray_Get(&context->layoutElementTreeRoots, i);
            SEClay__LayoutElementTreeRoot next = *SEClay__LayoutElementTreeRootArray_Get(&context->layoutElementTreeRoots, i + 1);
            if (next.zIndex < current.zIndex) {
                SEClay__LayoutElementTreeRootArray_Set(&context->layoutElementTreeRoots, i, next);
                SEClay__LayoutElementTreeRootArray_Set(&context->layoutElementTreeRoots, i + 1, current);
            }
        }
        sortMax--;
    }

    // Calculate final positions and generate render commands
    context->renderCommands.length = 0;
    dfsBuffer.length = 0;
    for (int32_t rootIndex = 0; rootIndex < context->layoutElementTreeRoots.length; ++rootIndex) {
        dfsBuffer.length = 0;
        SEClay__LayoutElementTreeRoot *root = SEClay__LayoutElementTreeRootArray_Get(&context->layoutElementTreeRoots, rootIndex);
        SEClay_LayoutElement *rootElement = SEClay_LayoutElementArray_Get(&context->layoutElements, (int)root->layoutElementIndex);
        SEClay_Vector2 rootPosition = SECLAY__DEFAULT_STRUCT;
        SEClay_LayoutElementHashMapItem *parentHashMapItem = SEClay__GetHashMapItem(root->parentId);
        // Position root floating containers
        if (SEClay__ElementHasConfig(rootElement, SEClay__ElementConfigType::Floating) && parentHashMapItem) {
            SEClay_FloatingElementConfig *config = SEClay__FindElementConfigWithType(rootElement, SEClay__ElementConfigType::Floating).floatingElementConfig;
            SEClay_Dimensions rootDimensions = rootElement->dimensions;
            SEClay_BoundingBox parentBoundingBox = parentHashMapItem->boundingBox;
            // Set X position
            SEClay_Vector2 targetAttachPosition = SECLAY__DEFAULT_STRUCT;
            switch (config->attachPoints.parent) {
                case SEClay_FloatingAttachPointType::LeftTop:
                case SEClay_FloatingAttachPointType::LeftCenter:
                case SEClay_FloatingAttachPointType::LeftBottom: targetAttachPosition.x = parentBoundingBox.x; break;
                case SEClay_FloatingAttachPointType::CenterTop:
                case SEClay_FloatingAttachPointType::CenterCenter:
                case SEClay_FloatingAttachPointType::CenterBottom: targetAttachPosition.x = parentBoundingBox.x + (parentBoundingBox.width / 2); break;
                case SEClay_FloatingAttachPointType::RightTop:
                case SEClay_FloatingAttachPointType::RightCenter:
                case SEClay_FloatingAttachPointType::RightBottom: targetAttachPosition.x = parentBoundingBox.x + parentBoundingBox.width; break;
            }
            switch (config->attachPoints.element) {
                case SEClay_FloatingAttachPointType::LeftTop:
                case SEClay_FloatingAttachPointType::LeftCenter:
                case SEClay_FloatingAttachPointType::LeftBottom: break;
                case SEClay_FloatingAttachPointType::CenterTop:
                case SEClay_FloatingAttachPointType::CenterCenter:
                case SEClay_FloatingAttachPointType::CenterBottom: targetAttachPosition.x -= (rootDimensions.width / 2); break;
                case SEClay_FloatingAttachPointType::RightTop:
                case SEClay_FloatingAttachPointType::RightCenter:
                case SEClay_FloatingAttachPointType::RightBottom: targetAttachPosition.x -= rootDimensions.width; break;
            }
            switch (config->attachPoints.parent) { // I know I could merge the x and y switch statements, but this is easier to read
                case SEClay_FloatingAttachPointType::LeftTop:
                case SEClay_FloatingAttachPointType::RightTop:
                case SEClay_FloatingAttachPointType::CenterTop: targetAttachPosition.y = parentBoundingBox.y; break;
                case SEClay_FloatingAttachPointType::LeftCenter:
                case SEClay_FloatingAttachPointType::CenterCenter:
                case SEClay_FloatingAttachPointType::RightCenter: targetAttachPosition.y = parentBoundingBox.y + (parentBoundingBox.height / 2); break;
                case SEClay_FloatingAttachPointType::LeftBottom:
                case SEClay_FloatingAttachPointType::CenterBottom:
                case SEClay_FloatingAttachPointType::RightBottom: targetAttachPosition.y = parentBoundingBox.y + parentBoundingBox.height; break;
            }
            switch (config->attachPoints.element) {
                case SEClay_FloatingAttachPointType::LeftTop:
                case SEClay_FloatingAttachPointType::RightTop:
                case SEClay_FloatingAttachPointType::CenterTop: break;
                case SEClay_FloatingAttachPointType::LeftCenter:
                case SEClay_FloatingAttachPointType::CenterCenter:
                case SEClay_FloatingAttachPointType::RightCenter: targetAttachPosition.y -= (rootDimensions.height / 2); break;
                case SEClay_FloatingAttachPointType::LeftBottom:
                case SEClay_FloatingAttachPointType::CenterBottom:
                case SEClay_FloatingAttachPointType::RightBottom: targetAttachPosition.y -= rootDimensions.height; break;
            }
            targetAttachPosition.x += config->offset.x;
            targetAttachPosition.y += config->offset.y;
            rootPosition = targetAttachPosition;
        }
        bool rootScissorOpened = false;
        if (root->clipElementId) {
            SEClay_LayoutElementHashMapItem *clipHashMapItem = SEClay__GetHashMapItem(root->clipElementId);
            // HARDENING: a missing lookup returns the sentinel (never NULL), whose layoutElement is NULL.
            if (clipHashMapItem && clipHashMapItem != &SEClay_LayoutElementHashMapItem_DEFAULT) {
                // Floating elements that are attached to scrolling contents won't be correctly positioned if external scroll handling is enabled, fix here
                if (context->externalScrollHandlingEnabled) {
                    SEClay_ClipElementConfig *clipConfig = clipHashMapItem->layoutElement ? SEClay__FindElementConfigWithType(clipHashMapItem->layoutElement, SEClay__ElementConfigType::Clip).clipElementConfig : NULL;
                    if (clipConfig) {
                        if (clipConfig->horizontal) {
                            rootPosition.x += clipConfig->childOffset.x;
                        }
                        if (clipConfig->vertical) {
                            rootPosition.y += clipConfig->childOffset.y;
                        }
                    }
                    break;
                }
                SEClay__AddRenderCommand(SEClay_RenderCommand {
                    .boundingBox = clipHashMapItem->boundingBox,
                    .userData = 0,
                    // Carries the clip element's own id (rather than a derived placeholder id, per the
                    // prior TODO here) so a renderer can look up what ambient scissor was active when
                    // this same element was normally opened elsewhere in the tree, and intersect against
                    // that instead of just this element's own bounding box -- a floating root's clip
                    // target may itself be nested inside further ancestor clips (a scroll area, a dock
                    // panel) that this root, being appended after the main tree finishes, has no other
                    // way to know about.
                    .id = root->clipElementId,
                    .zIndex = root->zIndex,
                    .commandType = SEClay_RenderCommandType::ScissorStart,
                });
                rootScissorOpened = true;
            }
        }
        SEClay__PushTreeNode(context, &dfsBuffer, SEClay__LayoutElementTreeNode { .layoutElement = rootElement, .position = rootPosition, .nextChildOffset = { .x = (float)rootElement->layoutConfig->padding.left, .y = (float)rootElement->layoutConfig->padding.top } });

        while (dfsBuffer.length > 0) {
            SEClay__LayoutElementTreeNode *currentElementTreeNode = SEClay__LayoutElementTreeNodeArray_Get(&dfsBuffer, (int)dfsBuffer.length - 1);
            SEClay_LayoutElement *currentElement = currentElementTreeNode->layoutElement;
            SEClay_LayoutConfig *layoutConfig = currentElement->layoutConfig;
            SEClay_Vector2 scrollOffset = SECLAY__DEFAULT_STRUCT;

            // This will only be run a single time for each element in downwards DFS order
            if (!context->treeNodeVisited.internalArray[dfsBuffer.length - 1]) {
                context->treeNodeVisited.internalArray[dfsBuffer.length - 1] = true;

                SEClay_BoundingBox currentElementBoundingBox = { currentElementTreeNode->position.x, currentElementTreeNode->position.y, currentElement->dimensions.width, currentElement->dimensions.height };
                if (SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Floating)) {
                    SEClay_FloatingElementConfig *floatingElementConfig = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Floating).floatingElementConfig;
                    SEClay_Dimensions expand = floatingElementConfig->expand;
                    currentElementBoundingBox.x -= expand.width;
                    currentElementBoundingBox.width += expand.width * 2;
                    currentElementBoundingBox.y -= expand.height;
                    currentElementBoundingBox.height += expand.height * 2;
                }

                SEClay__ScrollContainerDataInternal *scrollContainerData = SECLAY__NULL;
                // Apply scroll offsets to container
                if (SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Clip)) {
                    SEClay_ClipElementConfig *clipConfig = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Clip).clipElementConfig;

                    // This linear scan could theoretically be slow under very strange conditions, but I can't imagine a real UI with more than a few 10's of scroll containers
                    for (int32_t i = 0; i < 1; i++) {
                        SEClay__ScrollContainerDataInternal *mapping = SEClay__FindScrollContainer(context, currentElement->id);
                        if (mapping && mapping->layoutElement == currentElement) {
                            scrollContainerData = mapping;
                            mapping->boundingBox = currentElementBoundingBox;
                            scrollOffset = clipConfig->childOffset;
                            if (context->externalScrollHandlingEnabled) {
                                scrollOffset = SEClay_Vector2 SECLAY__DEFAULT_STRUCT;
                            }
                            break;
                        }
                    }
                }

                SEClay_LayoutElementHashMapItem *hashMapItem = SEClay__GetHashMapItem(currentElement->id);
                if (hashMapItem) {
                    hashMapItem->boundingBox = currentElementBoundingBox;
                    if (hashMapItem->idAlias) {
                        SEClay_LayoutElementHashMapItem *hashMapItemAlias = SEClay__GetHashMapItem(hashMapItem->idAlias);
                        if (hashMapItemAlias) {
                            hashMapItemAlias->boundingBox = currentElementBoundingBox;
                        }
                    }
                }

                // HARDENING: an element has at most one config of each kind (about 8), but the slice length is not
                // otherwise bounded; never let it index past this fixed buffer.
                enum { SECLAY__MAX_SORTED_CONFIGS = 20 };
                int32_t sortedConfigIndexes[SECLAY__MAX_SORTED_CONFIGS];
                int32_t sortedConfigCount = currentElement->elementConfigs.length < SECLAY__MAX_SORTED_CONFIGS ? currentElement->elementConfigs.length : SECLAY__MAX_SORTED_CONFIGS;
                if (sortedConfigCount < 0) { sortedConfigCount = 0; }
                for (int32_t elementConfigIndex = 0; elementConfigIndex < sortedConfigCount; ++elementConfigIndex) {
                    sortedConfigIndexes[elementConfigIndex] = elementConfigIndex;
                }
                sortMax = sortedConfigCount - 1;
                while (sortMax > 0) { // todo dumb bubble sort
                    for (int32_t i = 0; i < sortMax; ++i) {
                        int32_t current = sortedConfigIndexes[i];
                        int32_t next = sortedConfigIndexes[i + 1];
                        SEClay__ElementConfigType currentType = SEClay__ElementConfigArraySlice_Get(&currentElement->elementConfigs, current)->type;
                        SEClay__ElementConfigType nextType = SEClay__ElementConfigArraySlice_Get(&currentElement->elementConfigs, next)->type;
                        if (nextType == SEClay__ElementConfigType::Clip || currentType == SEClay__ElementConfigType::Border) {
                            sortedConfigIndexes[i] = next;
                            sortedConfigIndexes[i + 1] = current;
                        }
                    }
                    sortMax--;
                }

                // SEClay: open this element's effect group before anything the element or its descendants draw. Always emitted (even
                // offscreen) so it pairs with the EFFECT_END written when the traversal returns to this element.
                {
                    SEClay_CustomElementConfig *effectConfig = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Custom).customElementConfig;
                    if (effectConfig && effectConfig->effectData) {
                        SEClay_SharedElementConfig *effectShared = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Shared).sharedElementConfig;
                        SEClay__AddRenderCommand(SEClay_RenderCommand {
                            .boundingBox = currentElementBoundingBox,
                            .renderData = { .effect = { .effectData = effectConfig->effectData } },
                            .userData = effectShared ? effectShared->userData : 0,
                            .id = currentElement->id,
                            .zIndex = root->zIndex,
                            .commandType = SEClay_RenderCommandType::EffectStart,
                        });
                    }
                }
                bool emitRectangle = false;
                // Create the render commands for this element
                SEClay_SharedElementConfig *sharedConfig = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Shared).sharedElementConfig;
                if (sharedConfig && sharedConfig->backgroundColor.a > 0) {
                   emitRectangle = true;
                }
                else if (!sharedConfig) {
                    emitRectangle = false;
                    sharedConfig = &SEClay_SharedElementConfig_DEFAULT;
                }
                for (int32_t elementConfigIndex = 0; elementConfigIndex < sortedConfigCount; ++elementConfigIndex) {
                    SEClay_ElementConfig *elementConfig = SEClay__ElementConfigArraySlice_Get(&currentElement->elementConfigs, sortedConfigIndexes[elementConfigIndex]);
                    SEClay_RenderCommand renderCommand = {
                        .boundingBox = currentElementBoundingBox,
                        .userData = sharedConfig->userData,
                        .id = currentElement->id,
                    };

                    bool offscreen = SEClay__ElementIsOffscreen(&currentElementBoundingBox);
                    // Culling - Don't bother to generate render commands for rectangles entirely outside the screen - this won't stop their children from being rendered if they overflow
                    bool shouldRender = !offscreen;
                    switch (elementConfig->type) {
                        case SEClay__ElementConfigType::Aspect:
                        case SEClay__ElementConfigType::Floating:
                        case SEClay__ElementConfigType::Shared:
                        case SEClay__ElementConfigType::Border: {
                            shouldRender = false;
                            break;
                        }
                        case SEClay__ElementConfigType::Clip: {
                            // Always emitted, even when this element itself is judged offscreen (tested
                            // against the whole application window, not any nearer ancestor's own
                            // smaller visible region — e.g. a scroll area): DFS traversal still visits
                            // and renders this element's children regardless of `offscreen` (see the
                            // comment a few lines below this switch), so silently dropping SCISSOR_START
                            // here while still emitting the matching SCISSOR_END further down would
                            // leave children completely unclipped instead of clipped-and-culled.
                            shouldRender = true;
                            renderCommand.commandType = SEClay_RenderCommandType::ScissorStart;
                            renderCommand.renderData = SEClay_RenderData {
                                .clip = {
                                    .horizontal = elementConfig->config.clipElementConfig->horizontal,
                                    .vertical = elementConfig->config.clipElementConfig->vertical,
                                }
                            };
                            break;
                        }
                        case SEClay__ElementConfigType::Image: {
                            renderCommand.commandType = SEClay_RenderCommandType::Image;
                            renderCommand.renderData = SEClay_RenderData {
                                .image = {
                                    .backgroundColor = sharedConfig->backgroundColor,
                                    .cornerRadius = sharedConfig->cornerRadius,
                                    .imageData = elementConfig->config.imageElementConfig->imageData,
                               }
                            };
                            emitRectangle = false;
                            break;
                        }
                        case SEClay__ElementConfigType::Text: {
                            if (!shouldRender) {
                                break;
                            }
                            shouldRender = false;
                            SEClay_ElementConfigUnion configUnion = elementConfig->config;
                            SEClay_TextElementConfig *textElementConfig = configUnion.textElementConfig;
                            float naturalLineHeight = currentElement->childrenOrTextContent.textElementData->preferredDimensions.height;
                            float finalLineHeight = textElementConfig->lineHeight > 0 ? (float)textElementConfig->lineHeight : naturalLineHeight;
                            float lineHeightOffset = (finalLineHeight - naturalLineHeight) / 2;
                            float yPosition = lineHeightOffset;
                            for (int32_t lineIndex = 0; lineIndex < currentElement->childrenOrTextContent.textElementData->wrappedLines.length; ++lineIndex) {
                                SEClay__WrappedTextLine *wrappedLine = SEClay__WrappedTextLineArraySlice_Get(&currentElement->childrenOrTextContent.textElementData->wrappedLines, lineIndex);
                                if (wrappedLine->line.length == 0) {
                                    yPosition += finalLineHeight;
                                    continue;
                                }
                                float offset = (currentElementBoundingBox.width - wrappedLine->dimensions.width);
                                if (textElementConfig->textAlignment == SEClay_TextAlignment::Left) {
                                    offset = 0;
                                }
                                if (textElementConfig->textAlignment == SEClay_TextAlignment::Center) {
                                    offset /= 2;
                                }
                                SEClay__AddRenderCommand(SEClay_RenderCommand {
                                    .boundingBox = { currentElementBoundingBox.x + offset, currentElementBoundingBox.y + yPosition, wrappedLine->dimensions.width, wrappedLine->dimensions.height },
                                    .renderData = { .text = {
                                        .stringContents = SEClay_StringSlice { .length = wrappedLine->line.length, .chars = wrappedLine->line.chars, .baseChars = currentElement->childrenOrTextContent.textElementData->text.chars },
                                        .textColor = textElementConfig->textColor,
                                        .fontId = textElementConfig->fontId,
                                        .fontSize = textElementConfig->fontSize,
                                        .letterSpacing = textElementConfig->letterSpacing,
                                        .lineHeight = textElementConfig->lineHeight,
                                    }},
                                    .userData = textElementConfig->userData,
                                    .id = SEClay__HashNumber(lineIndex, currentElement->id).id,
                                    .zIndex = root->zIndex,
                                    .commandType = SEClay_RenderCommandType::Text,
                                });
                                yPosition += finalLineHeight;

                                if (!context->disableCulling && (currentElementBoundingBox.y + yPosition > context->layoutDimensions.height)) {
                                    break;
                                }
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Custom: {
                            if (elementConfig->config.customElementConfig->customData == NULL) {
                                // An effect-only element (effectData set, no customData) draws as an ordinary element; the effect
                                // itself is the EFFECT_START/END pair emitted around its content.
                                shouldRender = false;
                                break;
                            }
                            renderCommand.commandType = SEClay_RenderCommandType::Custom;
                            renderCommand.renderData = SEClay_RenderData {
                                .custom = {
                                    .backgroundColor = sharedConfig->backgroundColor,
                                    .cornerRadius = sharedConfig->cornerRadius,
                                    .customData = elementConfig->config.customElementConfig->customData,
                                }
                            };
                            emitRectangle = false;
                            break;
                        }
                        default: break;
                    }
                    if (shouldRender) {
                        SEClay__AddRenderCommand(renderCommand);
                    }
                    if (offscreen) {
                        // NOTE: You may be tempted to try an early return / continue if an element is off screen. Why bother calculating layout for its children, right?
                        // Unfortunately, a FLOATING_CONTAINER may be defined that attaches to a child or grandchild of this element, which is large enough to still
                        // be on screen, even if this element isn't. That depends on this element and it's children being laid out correctly (even if they are entirely off screen)
                    }
                }

                if (emitRectangle) {
                    SEClay__AddRenderCommand(SEClay_RenderCommand {
                        .boundingBox = currentElementBoundingBox,
                        .renderData = { .rectangle = {
                                .backgroundColor = sharedConfig->backgroundColor,
                                .cornerRadius = sharedConfig->cornerRadius,
                        }},
                        .userData = sharedConfig->userData,
                        .id = currentElement->id,
                        .zIndex = root->zIndex,
                        .commandType = SEClay_RenderCommandType::Rectangle,
                    });
                }

                // Setup initial on-axis alignment
                if (!SEClay__ElementHasConfig(currentElementTreeNode->layoutElement, SEClay__ElementConfigType::Text)) {
                    SEClay_Dimensions contentSize = {0,0};
                    if (layoutConfig->layoutDirection == SEClay_LayoutDirection::LeftToRight) {
                        for (int32_t i = 0; i < currentElement->childrenOrTextContent.children.length; ++i) {
                            SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[i]);
                            contentSize.width += childElement->dimensions.width;
                            contentSize.height = SECLAY__MAX(contentSize.height, childElement->dimensions.height);
                        }
                        contentSize.width += (float)(SECLAY__MAX(currentElement->childrenOrTextContent.children.length - 1, 0) * layoutConfig->childGap);
                        float extraSpace = currentElement->dimensions.width - (float)(layoutConfig->padding.left + layoutConfig->padding.right) - contentSize.width;
                        switch (layoutConfig->childAlignment.x) {
                            case SEClay_LayoutAlignmentX::Left: extraSpace = 0; break;
                            case SEClay_LayoutAlignmentX::Center: extraSpace /= 2; break;
                            default: break;
                        }
                        currentElementTreeNode->nextChildOffset.x += extraSpace;
                    } else {
                        for (int32_t i = 0; i < currentElement->childrenOrTextContent.children.length; ++i) {
                            SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[i]);
                            contentSize.width = SECLAY__MAX(contentSize.width, childElement->dimensions.width);
                            contentSize.height += childElement->dimensions.height;
                        }
                        contentSize.height += (float)(SECLAY__MAX(currentElement->childrenOrTextContent.children.length - 1, 0) * layoutConfig->childGap);
                        float extraSpace = currentElement->dimensions.height - (float)(layoutConfig->padding.top + layoutConfig->padding.bottom) - contentSize.height;
                        switch (layoutConfig->childAlignment.y) {
                            case SEClay_LayoutAlignmentY::Top: extraSpace = 0; break;
                            case SEClay_LayoutAlignmentY::Center: extraSpace /= 2; break;
                            default: break;
                        }
                        currentElementTreeNode->nextChildOffset.y += extraSpace;
                    }

                    if (scrollContainerData) {
                        scrollContainerData->contentSize = SEClay_Dimensions { contentSize.width + (float)(layoutConfig->padding.left + layoutConfig->padding.right), contentSize.height + (float)(layoutConfig->padding.top + layoutConfig->padding.bottom) };
                    }
                }
            }
            else {
                // DFS is returning upwards backwards
                bool closeClipElement = false;
                SEClay_ClipElementConfig *clipConfig = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Clip).clipElementConfig;
                if (clipConfig) {
                    closeClipElement = true;
                    for (int32_t i = 0; i < 1; i++) {
                        SEClay__ScrollContainerDataInternal *mapping = SEClay__FindScrollContainer(context, currentElement->id);
                        if (mapping && mapping->layoutElement == currentElement) {
                            scrollOffset = clipConfig->childOffset;
                            if (context->externalScrollHandlingEnabled) {
                                scrollOffset = SEClay_Vector2 SECLAY__DEFAULT_STRUCT;
                            }
                            break;
                        }
                    }
                }

                if (SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Border)) {
                    SEClay_LayoutElementHashMapItem *currentElementData = SEClay__GetHashMapItem(currentElement->id);
                    SEClay_BoundingBox currentElementBoundingBox = currentElementData->boundingBox;

                    // Culling - Don't bother to generate render commands for rectangles entirely outside the screen - this won't stop their children from being rendered if they overflow
                    if (!SEClay__ElementIsOffscreen(&currentElementBoundingBox)) {
                        SEClay_SharedElementConfig *sharedConfig = SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Shared) ? SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Shared).sharedElementConfig : &SEClay_SharedElementConfig_DEFAULT;
                        SEClay_BorderElementConfig *borderConfig = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Border).borderElementConfig;
                        SEClay_RenderCommand renderCommand = {
                                .boundingBox = currentElementBoundingBox,
                                .renderData = { .border = {
                                    .color = borderConfig->color,
                                    .cornerRadius = sharedConfig->cornerRadius,
                                    .width = borderConfig->width,
                                    .edgeData = borderConfig->edgeData
                                }},
                                .userData = sharedConfig->userData,
                                .id = SEClay__HashNumber(currentElement->id, currentElement->childrenOrTextContent.children.length).id,
                                .commandType = SEClay_RenderCommandType::Border,
                        };
                        SEClay__AddRenderCommand(renderCommand);
                        if (borderConfig->width.betweenChildren > 0 && borderConfig->color.a > 0) {
                            float halfGap = layoutConfig->childGap / 2;
                            SEClay_Vector2 borderOffset = { (float)layoutConfig->padding.left - halfGap, (float)layoutConfig->padding.top - halfGap };
                            if (layoutConfig->layoutDirection == SEClay_LayoutDirection::LeftToRight) {
                                for (int32_t i = 0; i < currentElement->childrenOrTextContent.children.length; ++i) {
                                    SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[i]);
                                    if (i > 0) {
                                        SEClay__AddRenderCommand(SEClay_RenderCommand {
                                            .boundingBox = { currentElementBoundingBox.x + borderOffset.x + scrollOffset.x, currentElementBoundingBox.y + scrollOffset.y, (float)borderConfig->width.betweenChildren, currentElement->dimensions.height },
                                            .renderData = { .rectangle = {
                                                .backgroundColor = borderConfig->color,
                                            } },
                                            .userData = sharedConfig->userData,
                                            .id = SEClay__HashNumber(currentElement->id, currentElement->childrenOrTextContent.children.length + 1 + i).id,
                                            .commandType = SEClay_RenderCommandType::Rectangle,
                                        });
                                    }
                                    borderOffset.x += (childElement->dimensions.width + (float)layoutConfig->childGap);
                                }
                            } else {
                                for (int32_t i = 0; i < currentElement->childrenOrTextContent.children.length; ++i) {
                                    SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[i]);
                                    if (i > 0) {
                                        SEClay__AddRenderCommand(SEClay_RenderCommand {
                                            .boundingBox = { currentElementBoundingBox.x + scrollOffset.x, currentElementBoundingBox.y + borderOffset.y + scrollOffset.y, currentElement->dimensions.width, (float)borderConfig->width.betweenChildren },
                                            .renderData = { .rectangle = {
                                                    .backgroundColor = borderConfig->color,
                                            } },
                                            .userData = sharedConfig->userData,
                                            .id = SEClay__HashNumber(currentElement->id, currentElement->childrenOrTextContent.children.length + 1 + i).id,
                                            .commandType = SEClay_RenderCommandType::Rectangle,
                                        });
                                    }
                                    borderOffset.y += (childElement->dimensions.height + (float)layoutConfig->childGap);
                                }
                            }
                        }
                    }
                }
                // This exists because the scissor needs to end _after_ borders between elements
                if (closeClipElement) {
                    SEClay__AddRenderCommand(SEClay_RenderCommand {
                        .id = SEClay__HashNumber(currentElement->id, rootElement->childrenOrTextContent.children.length + 11).id,
                        .commandType = SEClay_RenderCommandType::ScissorEnd,
                    });
                }
                // SEClay: close this element's effect group, after its borders and scissor end.
                {
                    SEClay_CustomElementConfig *effectConfig = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Custom).customElementConfig;
                    if (effectConfig && effectConfig->effectData) {
                        SEClay_SharedElementConfig *effectShared = SEClay__FindElementConfigWithType(currentElement, SEClay__ElementConfigType::Shared).sharedElementConfig;
                        SEClay__AddRenderCommand(SEClay_RenderCommand {
                            .boundingBox = { currentElementTreeNode->position.x, currentElementTreeNode->position.y, currentElement->dimensions.width, currentElement->dimensions.height },
                            .renderData = { .effect = { .effectData = effectConfig->effectData } },
                            .userData = effectShared ? effectShared->userData : 0,
                            .id = currentElement->id,
                            .zIndex = root->zIndex,
                            .commandType = SEClay_RenderCommandType::EffectEnd,
                        });
                    }
                }

                dfsBuffer.length--;
                continue;
            }

            // Add children to the DFS buffer
            if (!SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Text)) {
                // HARDENING: the children are written straight into the stack at computed indices (and into the
                // parallel visited flags); make sure the whole run fits in both before touching either.
                const int32_t childCount = currentElement->childrenOrTextContent.children.length;
                const int32_t stackLimit = dfsBuffer.capacity < context->treeNodeVisited.capacity ? dfsBuffer.capacity : context->treeNodeVisited.capacity;
                if (childCount < 0 || dfsBuffer.internalArray == NULL || childCount > stackLimit - dfsBuffer.length) {
                    SEClay__ReportError(SEClay_ErrorType::InternalError, SECLAY_STRING("Clay layout traversal stack overflowed; part of the layout was skipped."));
                    continue;
                }
                dfsBuffer.length += currentElement->childrenOrTextContent.children.length;
                for (int32_t i = 0; i < currentElement->childrenOrTextContent.children.length; ++i) {
                    SEClay_LayoutElement *childElement = SEClay_LayoutElementArray_Get(&context->layoutElements, currentElement->childrenOrTextContent.children.elements[i]);
                    // Alignment along non layout axis
                    if (layoutConfig->layoutDirection == SEClay_LayoutDirection::LeftToRight) {
                        currentElementTreeNode->nextChildOffset.y = currentElement->layoutConfig->padding.top;
                        float whiteSpaceAroundChild = currentElement->dimensions.height - (float)(layoutConfig->padding.top + layoutConfig->padding.bottom) - childElement->dimensions.height;
                        switch (layoutConfig->childAlignment.y) {
                            case SEClay_LayoutAlignmentY::Top: break;
                            case SEClay_LayoutAlignmentY::Center: currentElementTreeNode->nextChildOffset.y += whiteSpaceAroundChild / 2; break;
                            case SEClay_LayoutAlignmentY::Bottom: currentElementTreeNode->nextChildOffset.y += whiteSpaceAroundChild; break;
                        }
                    } else {
                        currentElementTreeNode->nextChildOffset.x = currentElement->layoutConfig->padding.left;
                        float whiteSpaceAroundChild = currentElement->dimensions.width - (float)(layoutConfig->padding.left + layoutConfig->padding.right) - childElement->dimensions.width;
                        switch (layoutConfig->childAlignment.x) {
                            case SEClay_LayoutAlignmentX::Left: break;
                            case SEClay_LayoutAlignmentX::Center: currentElementTreeNode->nextChildOffset.x += whiteSpaceAroundChild / 2; break;
                            case SEClay_LayoutAlignmentX::Right: currentElementTreeNode->nextChildOffset.x += whiteSpaceAroundChild; break;
                        }
                    }

                    SEClay_Vector2 childPosition = {
                        currentElementTreeNode->position.x + currentElementTreeNode->nextChildOffset.x + scrollOffset.x,
                        currentElementTreeNode->position.y + currentElementTreeNode->nextChildOffset.y + scrollOffset.y,
                    };

                    // DFS buffer elements need to be added in reverse because stack traversal happens backwards
                    uint32_t newNodeIndex = dfsBuffer.length - 1 - i;
                    dfsBuffer.internalArray[newNodeIndex] = SEClay__LayoutElementTreeNode {
                        .layoutElement = childElement,
                        .position = { childPosition.x, childPosition.y },
                        .nextChildOffset = { .x = (float)childElement->layoutConfig->padding.left, .y = (float)childElement->layoutConfig->padding.top },
                    };
                    context->treeNodeVisited.internalArray[newNodeIndex] = false;

                    // Update parent offsets
                    if (layoutConfig->layoutDirection == SEClay_LayoutDirection::LeftToRight) {
                        currentElementTreeNode->nextChildOffset.x += childElement->dimensions.width + (float)layoutConfig->childGap;
                    } else {
                        currentElementTreeNode->nextChildOffset.y += childElement->dimensions.height + (float)layoutConfig->childGap;
                    }
                }
            }
        }

        if (rootScissorOpened) {
            SEClay__AddRenderCommand(SEClay_RenderCommand { .id = SEClay__HashNumber(rootElement->id, rootElement->childrenOrTextContent.children.length + 11).id, .commandType = SEClay_RenderCommandType::ScissorEnd });
        }
    }
}

SECLAY_WASM_EXPORT("SEClay_GetPointerOverIds")
SECLAY_DLL_EXPORT SEClay_ElementIdArray SEClay_GetPointerOverIds(void) {
    return SEClay_GetCurrentContext()->pointerOverIds;
}

#pragma region DebugTools
SEClay_Color SECLAY__DEBUGVIEW_COLOR_1 = {0.2275f, 0.2196f, 0.2039f, 1.0f};
SEClay_Color SECLAY__DEBUGVIEW_COLOR_2 = {0.2431f, 0.2353f, 0.2275f, 1.0f};
SEClay_Color SECLAY__DEBUGVIEW_COLOR_3 = {0.5529f, 0.5216f, 0.5294f, 1.0f};
SEClay_Color SECLAY__DEBUGVIEW_COLOR_4 = {0.9333f, 0.8863f, 0.9059f, 1.0f};
SEClay_Color SECLAY__DEBUGVIEW_COLOR_SELECTED_ROW = {0.4f, 0.3137f, 0.3059f, 1.0f};
const int32_t SECLAY__DEBUGVIEW_ROW_HEIGHT = 30;
const int32_t SECLAY__DEBUGVIEW_OUTER_PADDING = 10;
const int32_t SECLAY__DEBUGVIEW_INDENT_WIDTH = 16;
SEClay_TextElementConfig SEClay__DebugView_TextNameConfig = {.textColor = {0.9333f, 0.8863f, 0.9059f, 1.0f}, .fontSize = 16, .wrapMode = SEClay_TextElementConfigWrapMode::None };
SEClay_LayoutConfig SEClay__DebugView_ScrollViewItemLayoutConfig = SECLAY__DEFAULT_STRUCT;

typedef struct {
    SEClay_String label;
    SEClay_Color color;
} SEClay__DebugElementConfigTypeLabelConfig;

SEClay__DebugElementConfigTypeLabelConfig SEClay__DebugGetElementConfigTypeLabel(SEClay__ElementConfigType type) {
    switch (type) {
        case SEClay__ElementConfigType::Shared: return SEClay__DebugElementConfigTypeLabelConfig { SECLAY_STRING("Shared"), {0.9529f, 0.5255f, 0.1882f, 1.0f} };
        case SEClay__ElementConfigType::Text: return SEClay__DebugElementConfigTypeLabelConfig { SECLAY_STRING("Text"), {0.4118f, 0.8235f, 0.9059f, 1.0f} };
        case SEClay__ElementConfigType::Aspect: return SEClay__DebugElementConfigTypeLabelConfig { SECLAY_STRING("Aspect"), {0.3961f, 0.5843f, 0.7608f, 1.0f} };
        case SEClay__ElementConfigType::Image: return SEClay__DebugElementConfigTypeLabelConfig { SECLAY_STRING("Image"), {0.4745f, 0.7412f, 0.6039f, 1.0f} };
        case SEClay__ElementConfigType::Floating: return SEClay__DebugElementConfigTypeLabelConfig { SECLAY_STRING("Floating"), {0.9804f, 0.4118f, 0.0f, 1.0f} };
        case SEClay__ElementConfigType::Clip: return SEClay__DebugElementConfigTypeLabelConfig {SECLAY_STRING("Scroll"), {0.949f, 0.7686f, 0.3529f, 1.0f} };
        case SEClay__ElementConfigType::Border: return SEClay__DebugElementConfigTypeLabelConfig {SECLAY_STRING("Border"), {0.4235f, 0.3569f, 0.4824f, 1.0f} };
        case SEClay__ElementConfigType::Custom: return SEClay__DebugElementConfigTypeLabelConfig { SECLAY_STRING("Custom"), {0.0431f, 0.2824f, 0.4196f, 1.0f} };
        default: break;
    }
    return SEClay__DebugElementConfigTypeLabelConfig { SECLAY_STRING("Error"), {0.0f, 0.0f, 0.0f, 1.0f} };
}

typedef struct {
    int32_t rowCount;
    int32_t selectedElementRowIndex;
} SEClay__RenderDebugLayoutData;

// Returns row count
SEClay__RenderDebugLayoutData SEClay__RenderDebugLayoutElementsList(int32_t initialRootsLength, int32_t highlightedRowIndex) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    SEClay__int32_tArray dfsBuffer = context->reusableElementIndexBuffer;
    SEClay__DebugView_ScrollViewItemLayoutConfig = SEClay_LayoutConfig { .sizing = { .height = SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT) }, .childGap = 6, .childAlignment = { .y = SEClay_LayoutAlignmentY::Center }};
    SEClay__RenderDebugLayoutData layoutData = SECLAY__DEFAULT_STRUCT;

    uint32_t highlightedElementId = 0;

    for (int32_t rootIndex = 0; rootIndex < initialRootsLength; ++rootIndex) {
        dfsBuffer.length = 0;
        SEClay__LayoutElementTreeRoot *root = SEClay__LayoutElementTreeRootArray_Get(&context->layoutElementTreeRoots, rootIndex);
        SEClay__int32_tArray_Add(&dfsBuffer, (int32_t)root->layoutElementIndex);
        context->treeNodeVisited.internalArray[0] = false;
        if (rootIndex > 0) {
            SECLAY({ .id = SECLAY_IDI("SEClay__DebugView_EmptyRowOuter", rootIndex), .layout = { .sizing = {.width = SEClay_SizingGrow(0)}, .padding = {SECLAY__DEBUGVIEW_INDENT_WIDTH / 2, 0, 0, 0} } }) {
                SECLAY({ .id = SECLAY_IDI("SEClay__DebugView_EmptyRow", rootIndex), .layout = { .sizing = { .width = SEClay_SizingGrow(0), .height = SEClay_SizingFixed((float)SECLAY__DEBUGVIEW_ROW_HEIGHT) }}, .border = { .color = SECLAY__DEBUGVIEW_COLOR_3, .width = { .top = 1 } } }) {}
            }
            layoutData.rowCount++;
        }
        while (dfsBuffer.length > 0) {
            int32_t currentElementIndex = SEClay__int32_tArray_GetValue(&dfsBuffer, (int)dfsBuffer.length - 1);
            SEClay_LayoutElement *currentElement = SEClay_LayoutElementArray_Get(&context->layoutElements, (int)currentElementIndex);
            if (context->treeNodeVisited.internalArray[dfsBuffer.length - 1]) {
                if (!SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Text) && currentElement->childrenOrTextContent.children.length > 0) {
                    SEClay__CloseElement();
                    SEClay__CloseElement();
                    SEClay__CloseElement();
                }
                dfsBuffer.length--;
                continue;
            }

            if (highlightedRowIndex == layoutData.rowCount) {
                if (context->pointerInfo.state == SEClay_PointerDataInteractionState::PressedThisFrame) {
                    context->debugSelectedElementId = currentElement->id;
                }
                highlightedElementId = currentElement->id;
            }

            context->treeNodeVisited.internalArray[dfsBuffer.length - 1] = true;
            SEClay_LayoutElementHashMapItem *currentElementData = SEClay__GetHashMapItem(currentElement->id);
            bool offscreen = SEClay__ElementIsOffscreen(&currentElementData->boundingBox);
            if (context->debugSelectedElementId == currentElement->id) {
                layoutData.selectedElementRowIndex = layoutData.rowCount;
            }
            SECLAY({ .id = SECLAY_IDI("SEClay__DebugView_ElementOuter", currentElement->id), .layout = SEClay__DebugView_ScrollViewItemLayoutConfig }) {
                // Collapse icon / button
                if (!(SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Text) || currentElement->childrenOrTextContent.children.length == 0)) {
                    SECLAY({
                        .id = SECLAY_IDI("SEClay__DebugView_CollapseElement", currentElement->id),
                        .layout = { .sizing = {SEClay_SizingFixed(16), SEClay_SizingFixed(16)}, .childAlignment = { SEClay_LayoutAlignmentX::Center, SEClay_LayoutAlignmentY::Center} },
                        .cornerRadius = SECLAY_CORNER_RADIUS(4),
                        .border = { .color = SECLAY__DEBUGVIEW_COLOR_3, .width = {1, 1, 1, 1, 0} },
                    }) {
                        SECLAY_TEXT((currentElementData && currentElementData->debugData->collapsed) ? SECLAY_STRING("+") : SECLAY_STRING("-"), SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16 }));
                    }
                } else { // Square dot for empty containers
                    SECLAY({ .layout = { .sizing = {SEClay_SizingFixed(16), SEClay_SizingFixed(16)}, .childAlignment = { SEClay_LayoutAlignmentX::Center, SEClay_LayoutAlignmentY::Center } } }) {
                        SECLAY({ .layout = { .sizing = {SEClay_SizingFixed(8), SEClay_SizingFixed(8)} }, .backgroundColor = SECLAY__DEBUGVIEW_COLOR_3, .cornerRadius = SECLAY_CORNER_RADIUS(2) }) {}
                    }
                }
                // Collisions and offscreen info
                if (currentElementData) {
                    if (currentElementData->debugData->collision) {
                        SECLAY({ .layout = { .padding = { 8, 8, 2, 2 }}, .border = { .color = {0.6941f, 0.5765f, 0.0314f, 1.0f}, .width = {1, 1, 1, 1, 0} } }) {
                            SECLAY_TEXT(SECLAY_STRING("Duplicate ID"), SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_3, .fontSize = 16 }));
                        }
                    }
                    if (offscreen) {
                        SECLAY({ .layout = { .padding = { 8, 8, 2, 2 } }, .border = {  .color = SECLAY__DEBUGVIEW_COLOR_3, .width = { 1, 1, 1, 1, 0} } }) {
                            SECLAY_TEXT(SECLAY_STRING("Offscreen"), SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_3, .fontSize = 16 }));
                        }
                    }
                }
                SEClay_String idString = SEClay__StringArray_GetValue(&context->layoutElementIdStrings, currentElementIndex); // HARDENING: was an unchecked index
                if (idString.length > 0) {
                    SECLAY_TEXT(idString, offscreen ? SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_3, .fontSize = 16 }) : &SEClay__DebugView_TextNameConfig);
                }
                for (int32_t elementConfigIndex = 0; elementConfigIndex < currentElement->elementConfigs.length; ++elementConfigIndex) {
                    SEClay_ElementConfig *elementConfig = SEClay__ElementConfigArraySlice_Get(&currentElement->elementConfigs, elementConfigIndex);
                    if (elementConfig->type == SEClay__ElementConfigType::Shared) {
                        SEClay_Color labelColor = {0.9529f, 0.5255f, 0.1882f, 0.3529f};
                        labelColor.a = 90;
                        SEClay_Color backgroundColor = elementConfig->config.sharedElementConfig->backgroundColor;
                        SEClay_CornerRadius radius = elementConfig->config.sharedElementConfig->cornerRadius;
                        if (backgroundColor.a > 0) {
                            SECLAY({ .layout = { .padding = { 8, 8, 2, 2 } }, .backgroundColor = labelColor, .cornerRadius = SECLAY_CORNER_RADIUS(4), .border = { .color = labelColor, .width = { 1, 1, 1, 1, 0} } }) {
                                SECLAY_TEXT(SECLAY_STRING("Color"), SECLAY_TEXT_CONFIG({ .textColor = offscreen ? SECLAY__DEBUGVIEW_COLOR_3 : SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16 }));
                            }
                        }
                        if (radius.bottomLeft > 0) {
                            SECLAY({ .layout = { .padding = { 8, 8, 2, 2 } }, .backgroundColor = labelColor, .cornerRadius = SECLAY_CORNER_RADIUS(4), .border = { .color = labelColor, .width = { 1, 1, 1, 1, 0 } } }) {
                                SECLAY_TEXT(SECLAY_STRING("Radius"), SECLAY_TEXT_CONFIG({ .textColor = offscreen ? SECLAY__DEBUGVIEW_COLOR_3 : SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16 }));
                            }
                        }
                        continue;
                    }
                    SEClay__DebugElementConfigTypeLabelConfig config = SEClay__DebugGetElementConfigTypeLabel(elementConfig->type);
                    SEClay_Color backgroundColor = config.color;
                    backgroundColor.a = 90;
                    SECLAY({ .layout = { .padding = { 8, 8, 2, 2 } }, .backgroundColor = backgroundColor, .cornerRadius = SECLAY_CORNER_RADIUS(4), .border = { .color = config.color, .width = { 1, 1, 1, 1, 0 } } }) {
                        SECLAY_TEXT(config.label, SECLAY_TEXT_CONFIG({ .textColor = offscreen ? SECLAY__DEBUGVIEW_COLOR_3 : SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16 }));
                    }
                }
            }

            // Render the text contents below the element as a non-interactive row
            if (SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Text)) {
                layoutData.rowCount++;
                SEClay__TextElementData *textElementData = currentElement->childrenOrTextContent.textElementData;
                SEClay_TextElementConfig *rawTextConfig = offscreen ? SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_3, .fontSize = 16 }) : &SEClay__DebugView_TextNameConfig;
                SECLAY({ .layout = { .sizing = { .height = SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT)}, .childAlignment = { .y = SEClay_LayoutAlignmentY::Center } } }) {
                    SECLAY({ .layout = { .sizing = {.width = SEClay_SizingFixed(SECLAY__DEBUGVIEW_INDENT_WIDTH + 16) } } }) {}
                    SECLAY_TEXT(SECLAY_STRING("\""), rawTextConfig);
                    SECLAY_TEXT(textElementData->text.length > 40 ? (SEClay_String { .length = 40, .chars = textElementData->text.chars }) : textElementData->text, rawTextConfig);
                    if (textElementData->text.length > 40) {
                        SECLAY_TEXT(SECLAY_STRING("..."), rawTextConfig);
                    }
                    SECLAY_TEXT(SECLAY_STRING("\""), rawTextConfig);
                }
            } else if (currentElement->childrenOrTextContent.children.length > 0) {
                SEClay__OpenElement();
                SEClay__ConfigureOpenElement(SEClay_ElementDeclaration { .layout = { .padding = { .left = 8 } } });
                SEClay__OpenElement();
                SEClay__ConfigureOpenElement(SEClay_ElementDeclaration { .layout = { .padding = { .left = SECLAY__DEBUGVIEW_INDENT_WIDTH }}, .border = { .color = SECLAY__DEBUGVIEW_COLOR_3, .width = { .left = 1 } }});
                SEClay__OpenElement();
                SEClay__ConfigureOpenElement(SEClay_ElementDeclaration { .layout = { .layoutDirection = SEClay_LayoutDirection::TopToBottom } });
            }

            layoutData.rowCount++;
            if (!(SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Text) || (currentElementData && currentElementData->debugData->collapsed))) {
                for (int32_t i = currentElement->childrenOrTextContent.children.length - 1; i >= 0; --i) {
                    // HARDENING: the visited flag was written at length-1 even when the push was rejected as full.
                    if (dfsBuffer.length < dfsBuffer.capacity && dfsBuffer.length < context->treeNodeVisited.capacity) {
                        SEClay__int32_tArray_Add(&dfsBuffer, currentElement->childrenOrTextContent.children.elements[i]);
                        context->treeNodeVisited.internalArray[dfsBuffer.length - 1] = false;
                    }
                }
            }
        }
    }

    if (context->pointerInfo.state == SEClay_PointerDataInteractionState::PressedThisFrame) {
        SEClay_ElementId collapseButtonId = SEClay__HashString(SECLAY_STRING("SEClay__DebugView_CollapseElement"), 0, 0);
        for (int32_t i = (int)context->pointerOverIds.length - 1; i >= 0; i--) {
            SEClay_ElementId *elementId = SEClay_ElementIdArray_Get(&context->pointerOverIds, i);
            if (elementId->baseId == collapseButtonId.baseId) {
                SEClay_LayoutElementHashMapItem *highlightedItem = SEClay__GetHashMapItem(elementId->offset);
                highlightedItem->debugData->collapsed = !highlightedItem->debugData->collapsed;
                break;
            }
        }
    }

    if (highlightedElementId) {
        SECLAY({ .id = SECLAY_ID("SEClay__DebugView_ElementHighlight"), .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingGrow(0)} }, .floating = { .parentId = highlightedElementId, .zIndex = 32767, .pointerCaptureMode = SEClay_PointerCaptureMode::Passthrough, .attachTo = SEClay_FloatingAttachToElement::ElementWithId } }) {
            SECLAY({ .id = SECLAY_ID("SEClay__DebugView_ElementHighlightRectangle"), .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingGrow(0)} }, .backgroundColor = SEClay__debugViewHighlightColor }) {}
        }
    }
    return layoutData;
}

void SEClay__RenderDebugLayoutSizing(SEClay_SizingAxis sizing, SEClay_TextElementConfig *infoTextConfig) {
    SEClay_String sizingLabel = SECLAY_STRING("GROW");
    if (sizing.type == SEClay__SizingType::Fit) {
        sizingLabel = SECLAY_STRING("FIT");
    } else if (sizing.type == SEClay__SizingType::Percent) {
        sizingLabel = SECLAY_STRING("PERCENT");
    } else if (sizing.type == SEClay__SizingType::Fixed) {
        sizingLabel = SECLAY_STRING("FIXED");
    }
    SECLAY_TEXT(sizingLabel, infoTextConfig);
    if (sizing.type == SEClay__SizingType::Grow || sizing.type == SEClay__SizingType::Fit || sizing.type == SEClay__SizingType::Fixed) {
        SECLAY_TEXT(SECLAY_STRING("("), infoTextConfig);
        if (sizing.size.minMax.min != 0) {
            SECLAY_TEXT(SECLAY_STRING("min: "), infoTextConfig);
            SECLAY_TEXT(SEClay__IntToString(sizing.size.minMax.min), infoTextConfig);
            if (sizing.size.minMax.max != SECLAY__MAXFLOAT) {
                SECLAY_TEXT(SECLAY_STRING(", "), infoTextConfig);
            }
        }
        if (sizing.size.minMax.max != SECLAY__MAXFLOAT) {
            SECLAY_TEXT(SECLAY_STRING("max: "), infoTextConfig);
            SECLAY_TEXT(SEClay__IntToString(sizing.size.minMax.max), infoTextConfig);
        }
        SECLAY_TEXT(SECLAY_STRING(")"), infoTextConfig);
    } else if (sizing.type == SEClay__SizingType::Percent) {
        SECLAY_TEXT(SECLAY_STRING("("), infoTextConfig);
        SECLAY_TEXT(SEClay__IntToString(sizing.size.percent * 100), infoTextConfig);
        SECLAY_TEXT(SECLAY_STRING("%)"), infoTextConfig);
    }
}

void SEClay__RenderDebugViewElementConfigHeader(SEClay_String elementId, SEClay__ElementConfigType type) {
    SEClay__DebugElementConfigTypeLabelConfig config = SEClay__DebugGetElementConfigTypeLabel(type);
    SEClay_Color backgroundColor = config.color;
    backgroundColor.a = 90;
    SECLAY({ .layout = { .sizing = { .width = SEClay_SizingGrow(0) }, .padding = SECLAY_PADDING_ALL(SECLAY__DEBUGVIEW_OUTER_PADDING), .childAlignment = { .y = SEClay_LayoutAlignmentY::Center } } }) {
        SECLAY({ .layout = { .padding = { 8, 8, 2, 2 } }, .backgroundColor = backgroundColor, .cornerRadius = SECLAY_CORNER_RADIUS(4), .border = { .color = config.color, .width = { 1, 1, 1, 1, 0 } } }) {
            SECLAY_TEXT(config.label, SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16 }));
        }
        SECLAY({ .layout = { .sizing = { .width = SEClay_SizingGrow(0) } } }) {}
        SECLAY_TEXT(elementId, SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_3, .fontSize = 16, .wrapMode = SEClay_TextElementConfigWrapMode::None }));
    }
}

void SEClay__RenderDebugViewColor(SEClay_Color color, SEClay_TextElementConfig *textConfig) {
    SECLAY({ .layout = { .childAlignment = {.y = SEClay_LayoutAlignmentY::Center} } }) {
        SECLAY_TEXT(SECLAY_STRING("{ r: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(color.r), textConfig);
        SECLAY_TEXT(SECLAY_STRING(", g: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(color.g), textConfig);
        SECLAY_TEXT(SECLAY_STRING(", b: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(color.b), textConfig);
        SECLAY_TEXT(SECLAY_STRING(", a: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(color.a), textConfig);
        SECLAY_TEXT(SECLAY_STRING(" }"), textConfig);
        SECLAY({ .layout = { .sizing = { .width = SEClay_SizingFixed(10) } } }) {}
        SECLAY({ .layout = { .sizing = { SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT - 8), SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT - 8)} }, .backgroundColor = color, .cornerRadius = SECLAY_CORNER_RADIUS(4), .border = { .color = SECLAY__DEBUGVIEW_COLOR_4, .width = { 1, 1, 1, 1, 0 } } }) {}
    }
}

void SEClay__RenderDebugViewCornerRadius(SEClay_CornerRadius cornerRadius, SEClay_TextElementConfig *textConfig) {
    SECLAY({ .layout = { .childAlignment = {.y = SEClay_LayoutAlignmentY::Center} } }) {
        SECLAY_TEXT(SECLAY_STRING("{ topLeft: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(cornerRadius.topLeft), textConfig);
        SECLAY_TEXT(SECLAY_STRING(", topRight: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(cornerRadius.topRight), textConfig);
        SECLAY_TEXT(SECLAY_STRING(", bottomLeft: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(cornerRadius.bottomLeft), textConfig);
        SECLAY_TEXT(SECLAY_STRING(", bottomRight: "), textConfig);
        SECLAY_TEXT(SEClay__IntToString(cornerRadius.bottomRight), textConfig);
        SECLAY_TEXT(SECLAY_STRING(" }"), textConfig);
    }
}

void HandleDebugViewCloseButtonInteraction(SEClay_ElementId elementId, SEClay_PointerData pointerInfo, intptr_t userData) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    (void) elementId; (void) pointerInfo; (void) userData;
    if (pointerInfo.state == SEClay_PointerDataInteractionState::PressedThisFrame) {
        context->debugModeEnabled = false;
    }
}

void SEClay__RenderDebugView(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    SEClay_ElementId closeButtonId = SEClay__HashString(SECLAY_STRING("SEClay__DebugViewTopHeaderCloseButtonOuter"), 0, 0);
    if (context->pointerInfo.state == SEClay_PointerDataInteractionState::PressedThisFrame) {
        for (int32_t i = 0; i < context->pointerOverIds.length; ++i) {
            SEClay_ElementId *elementId = SEClay_ElementIdArray_Get(&context->pointerOverIds, i);
            if (elementId->id == closeButtonId.id) {
                context->debugModeEnabled = false;
                return;
            }
        }
    }

    uint32_t initialRootsLength = context->layoutElementTreeRoots.length;
    uint32_t initialElementsLength = context->layoutElements.length;
    SEClay_TextElementConfig *infoTextConfig = SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16, .wrapMode = SEClay_TextElementConfigWrapMode::None });
    SEClay_TextElementConfig *infoTitleConfig = SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_3, .fontSize = 16, .wrapMode = SEClay_TextElementConfigWrapMode::None });
    SEClay_ElementId scrollId = SEClay__HashString(SECLAY_STRING("SEClay__DebugViewOuterScrollPane"), 0, 0);
    float scrollYOffset = 0;
    bool pointerInDebugView = context->pointerInfo.position.y < context->layoutDimensions.height - 300;
    for (int32_t i = 0; i < context->scrollContainerDatas.length; ++i) {
        SEClay__ScrollContainerDataInternal *scrollContainerData = SEClay__ScrollContainerDataInternalArray_Get(&context->scrollContainerDatas, i);
        if (scrollContainerData->elementId == scrollId.id) {
            if (!context->externalScrollHandlingEnabled) {
                scrollYOffset = scrollContainerData->scrollPosition.y;
            } else {
                pointerInDebugView = context->pointerInfo.position.y + scrollContainerData->scrollPosition.y < context->layoutDimensions.height - 300;
            }
            break;
        }
    }
    int32_t highlightedRow = pointerInDebugView
            ? (int32_t)((context->pointerInfo.position.y - scrollYOffset) / (float)SECLAY__DEBUGVIEW_ROW_HEIGHT) - 1
            : -1;
    if (context->pointerInfo.position.x < context->layoutDimensions.width - (float)SEClay__debugViewWidth) {
        highlightedRow = -1;
    }
    SEClay__RenderDebugLayoutData layoutData = SECLAY__DEFAULT_STRUCT;
    SECLAY({ .id = SECLAY_ID("SEClay__DebugView"),
         .layout = { .sizing = { SEClay_SizingFixed((float)SEClay__debugViewWidth) , SEClay_SizingFixed(context->layoutDimensions.height) }, .layoutDirection = SEClay_LayoutDirection::TopToBottom },
        .floating = { .zIndex = 32765, .attachPoints = { .element = SEClay_FloatingAttachPointType::LeftCenter, .parent = SEClay_FloatingAttachPointType::RightCenter }, .attachTo = SEClay_FloatingAttachToElement::Root, .clipTo = SEClay_FloatingClipToElement::AttachedParent },
        .border = { .color = SECLAY__DEBUGVIEW_COLOR_3, .width = { .bottom = 1 } }
    }) {
        SECLAY({ .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT)}, .padding = {SECLAY__DEBUGVIEW_OUTER_PADDING, SECLAY__DEBUGVIEW_OUTER_PADDING, 0, 0 }, .childAlignment = {.y = SEClay_LayoutAlignmentY::Center} }, .backgroundColor = SECLAY__DEBUGVIEW_COLOR_2 }) {
            SECLAY_TEXT(SECLAY_STRING("Clay Debug Tools"), infoTextConfig);
            SECLAY({ .layout = { .sizing = { .width = SEClay_SizingGrow(0) } } }) {}
            // Close button
            SECLAY({
                .layout = { .sizing = {SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT - 10), SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT - 10)}, .childAlignment = {SEClay_LayoutAlignmentX::Center, SEClay_LayoutAlignmentY::Center} },
                .backgroundColor = {0.851f, 0.3569f, 0.2627f, 0.3137f},
                .cornerRadius = SECLAY_CORNER_RADIUS(4),
                .border = { .color = {0.851f, 0.3569f, 0.2627f, 1.0f}, .width = { 1, 1, 1, 1, 0 } },
            }) {
                SEClay_OnHover(HandleDebugViewCloseButtonInteraction, 0);
                SECLAY_TEXT(SECLAY_STRING("x"), SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16 }));
            }
        }
        SECLAY({ .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingFixed(1)} }, .backgroundColor = SECLAY__DEBUGVIEW_COLOR_3 } ) {}
        SECLAY({ .id = scrollId, .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingGrow(0)} }, .clip = { .horizontal = true, .vertical = true, .childOffset = SEClay_GetScrollOffset() } }) {
            SECLAY({ .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingGrow(0)}, .layoutDirection = SEClay_LayoutDirection::TopToBottom }, .backgroundColor = ((initialElementsLength + initialRootsLength) & 1) == 0 ? SECLAY__DEBUGVIEW_COLOR_2 : SECLAY__DEBUGVIEW_COLOR_1 }) {
                SEClay_ElementId panelContentsId = SEClay__HashString(SECLAY_STRING("SEClay__DebugViewPaneOuter"), 0, 0);
                // Element list
                SECLAY({ .id = panelContentsId, .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingGrow(0)} }, .floating = { .zIndex = 32766, .pointerCaptureMode = SEClay_PointerCaptureMode::Passthrough, .attachTo = SEClay_FloatingAttachToElement::Parent, .clipTo = SEClay_FloatingClipToElement::AttachedParent } }) {
                    SECLAY({ .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingGrow(0)}, .padding = { SECLAY__DEBUGVIEW_OUTER_PADDING, SECLAY__DEBUGVIEW_OUTER_PADDING, 0, 0 }, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                        layoutData = SEClay__RenderDebugLayoutElementsList((int32_t)initialRootsLength, highlightedRow);
                    }
                }
                float contentWidth = SEClay__GetHashMapItem(panelContentsId.id)->layoutElement->dimensions.width;
                SECLAY({ .layout = { .sizing = {.width = SEClay_SizingFixed(contentWidth) }, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {}
                for (int32_t i = 0; i < layoutData.rowCount; i++) {
                    SEClay_Color rowColor = (i & 1) == 0 ? SECLAY__DEBUGVIEW_COLOR_2 : SECLAY__DEBUGVIEW_COLOR_1;
                    if (i == layoutData.selectedElementRowIndex) {
                        rowColor = SECLAY__DEBUGVIEW_COLOR_SELECTED_ROW;
                    }
                    if (i == highlightedRow) {
                        rowColor.r *= 1.25f;
                        rowColor.g *= 1.25f;
                        rowColor.b *= 1.25f;
                    }
                    SECLAY({ .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT)}, .layoutDirection = SEClay_LayoutDirection::TopToBottom }, .backgroundColor = rowColor } ) {}
                }
            }
        }
        SECLAY({ .layout = { .sizing = {.width = SEClay_SizingGrow(0), .height = SEClay_SizingFixed(1)} }, .backgroundColor = SECLAY__DEBUGVIEW_COLOR_3 }) {}
        if (context->debugSelectedElementId != 0) {
            SEClay_LayoutElementHashMapItem *selectedItem = SEClay__GetHashMapItem(context->debugSelectedElementId);
            SECLAY({
                .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingFixed(300)}, .layoutDirection = SEClay_LayoutDirection::TopToBottom },
                .backgroundColor = SECLAY__DEBUGVIEW_COLOR_2 ,
                .clip = { .vertical = true, .childOffset = SEClay_GetScrollOffset() },
                .border = { .color = SECLAY__DEBUGVIEW_COLOR_3, .width = { .betweenChildren = 1 } }
            }) {
                SECLAY({ .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT + 8)}, .padding = {SECLAY__DEBUGVIEW_OUTER_PADDING, SECLAY__DEBUGVIEW_OUTER_PADDING, 0, 0 }, .childAlignment = {.y = SEClay_LayoutAlignmentY::Center} } }) {
                    SECLAY_TEXT(SECLAY_STRING("Layout Config"), infoTextConfig);
                    SECLAY({ .layout = { .sizing = { .width = SEClay_SizingGrow(0) } } }) {}
                    if (selectedItem->elementId.stringId.length != 0) {
                        SECLAY_TEXT(selectedItem->elementId.stringId, infoTitleConfig);
                        if (selectedItem->elementId.offset != 0) {
                            SECLAY_TEXT(SECLAY_STRING(" ("), infoTitleConfig);
                            SECLAY_TEXT(SEClay__IntToString(selectedItem->elementId.offset), infoTitleConfig);
                            SECLAY_TEXT(SECLAY_STRING(")"), infoTitleConfig);
                        }
                    }
                }
                SEClay_Padding attributeConfigPadding = {SECLAY__DEBUGVIEW_OUTER_PADDING, SECLAY__DEBUGVIEW_OUTER_PADDING, 8, 8};
                // SEClay_LayoutConfig debug info
                SECLAY({ .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                    // .boundingBox
                    SECLAY_TEXT(SECLAY_STRING("Bounding Box"), infoTitleConfig);
                    SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                        SECLAY_TEXT(SECLAY_STRING("{ x: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(selectedItem->boundingBox.x), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(", y: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(selectedItem->boundingBox.y), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(", width: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(selectedItem->boundingBox.width), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(", height: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(selectedItem->boundingBox.height), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(" }"), infoTextConfig);
                    }
                    // .layoutDirection
                    SECLAY_TEXT(SECLAY_STRING("Layout Direction"), infoTitleConfig);
                    SEClay_LayoutConfig *layoutConfig = selectedItem->layoutElement->layoutConfig;
                    SECLAY_TEXT(layoutConfig->layoutDirection == SEClay_LayoutDirection::TopToBottom ? SECLAY_STRING("TOP_TO_BOTTOM") : SECLAY_STRING("LEFT_TO_RIGHT"), infoTextConfig);
                    // .sizing
                    SECLAY_TEXT(SECLAY_STRING("Sizing"), infoTitleConfig);
                    SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                        SECLAY_TEXT(SECLAY_STRING("width: "), infoTextConfig);
                        SEClay__RenderDebugLayoutSizing(layoutConfig->sizing.width, infoTextConfig);
                    }
                    SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                        SECLAY_TEXT(SECLAY_STRING("height: "), infoTextConfig);
                        SEClay__RenderDebugLayoutSizing(layoutConfig->sizing.height, infoTextConfig);
                    }
                    // .padding
                    SECLAY_TEXT(SECLAY_STRING("Padding"), infoTitleConfig);
                    SECLAY({ .id = SECLAY_ID("SEClay__DebugViewElementInfoPadding") }) {
                        SECLAY_TEXT(SECLAY_STRING("{ left: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(layoutConfig->padding.left), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(", right: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(layoutConfig->padding.right), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(", top: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(layoutConfig->padding.top), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(", bottom: "), infoTextConfig);
                        SECLAY_TEXT(SEClay__IntToString(layoutConfig->padding.bottom), infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(" }"), infoTextConfig);
                    }
                    // .childGap
                    SECLAY_TEXT(SECLAY_STRING("Child Gap"), infoTitleConfig);
                    SECLAY_TEXT(SEClay__IntToString(layoutConfig->childGap), infoTextConfig);
                    // .childAlignment
                    SECLAY_TEXT(SECLAY_STRING("Child Alignment"), infoTitleConfig);
                    SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                        SECLAY_TEXT(SECLAY_STRING("{ x: "), infoTextConfig);
                        SEClay_String alignX = SECLAY_STRING("LEFT");
                        if (layoutConfig->childAlignment.x == SEClay_LayoutAlignmentX::Center) {
                            alignX = SECLAY_STRING("CENTER");
                        } else if (layoutConfig->childAlignment.x == SEClay_LayoutAlignmentX::Right) {
                            alignX = SECLAY_STRING("RIGHT");
                        }
                        SECLAY_TEXT(alignX, infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(", y: "), infoTextConfig);
                        SEClay_String alignY = SECLAY_STRING("TOP");
                        if (layoutConfig->childAlignment.y == SEClay_LayoutAlignmentY::Center) {
                            alignY = SECLAY_STRING("CENTER");
                        } else if (layoutConfig->childAlignment.y == SEClay_LayoutAlignmentY::Bottom) {
                            alignY = SECLAY_STRING("BOTTOM");
                        }
                        SECLAY_TEXT(alignY, infoTextConfig);
                        SECLAY_TEXT(SECLAY_STRING(" }"), infoTextConfig);
                    }
                }
                for (int32_t elementConfigIndex = 0; elementConfigIndex < selectedItem->layoutElement->elementConfigs.length; ++elementConfigIndex) {
                    SEClay_ElementConfig *elementConfig = SEClay__ElementConfigArraySlice_Get(&selectedItem->layoutElement->elementConfigs, elementConfigIndex);
                    SEClay__RenderDebugViewElementConfigHeader(selectedItem->elementId.stringId, elementConfig->type);
                    switch (elementConfig->type) {
                        case SEClay__ElementConfigType::Shared: {
                            SEClay_SharedElementConfig *sharedConfig = elementConfig->config.sharedElementConfig;
                            SECLAY({ .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom }}) {
                                // .backgroundColor
                                SECLAY_TEXT(SECLAY_STRING("Background Color"), infoTitleConfig);
                                SEClay__RenderDebugViewColor(sharedConfig->backgroundColor, infoTextConfig);
                                // .cornerRadius
                                SECLAY_TEXT(SECLAY_STRING("Corner Radius"), infoTitleConfig);
                                SEClay__RenderDebugViewCornerRadius(sharedConfig->cornerRadius, infoTextConfig);
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Text: {
                            SEClay_TextElementConfig *textConfig = elementConfig->config.textElementConfig;
                            SECLAY({ .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                                // .fontSize
                                SECLAY_TEXT(SECLAY_STRING("Font Size"), infoTitleConfig);
                                SECLAY_TEXT(SEClay__IntToString(textConfig->fontSize), infoTextConfig);
                                // .fontId
                                SECLAY_TEXT(SECLAY_STRING("Font ID"), infoTitleConfig);
                                SECLAY_TEXT(SEClay__IntToString(textConfig->fontId), infoTextConfig);
                                // .lineHeight
                                SECLAY_TEXT(SECLAY_STRING("Line Height"), infoTitleConfig);
                                SECLAY_TEXT(textConfig->lineHeight == 0 ? SECLAY_STRING("auto") : SEClay__IntToString(textConfig->lineHeight), infoTextConfig);
                                // .letterSpacing
                                SECLAY_TEXT(SECLAY_STRING("Letter Spacing"), infoTitleConfig);
                                SECLAY_TEXT(SEClay__IntToString(textConfig->letterSpacing), infoTextConfig);
                                // .wrapMode
                                SECLAY_TEXT(SECLAY_STRING("Wrap Mode"), infoTitleConfig);
                                SEClay_String wrapMode = SECLAY_STRING("WORDS");
                                if (textConfig->wrapMode == SEClay_TextElementConfigWrapMode::None) {
                                    wrapMode = SECLAY_STRING("NONE");
                                } else if (textConfig->wrapMode == SEClay_TextElementConfigWrapMode::Newlines) {
                                    wrapMode = SECLAY_STRING("NEWLINES");
                                }
                                SECLAY_TEXT(wrapMode, infoTextConfig);
                                // .textAlignment
                                SECLAY_TEXT(SECLAY_STRING("Text Alignment"), infoTitleConfig);
                                SEClay_String textAlignment = SECLAY_STRING("LEFT");
                                if (textConfig->textAlignment == SEClay_TextAlignment::Center) {
                                    textAlignment = SECLAY_STRING("CENTER");
                                } else if (textConfig->textAlignment == SEClay_TextAlignment::Right) {
                                    textAlignment = SECLAY_STRING("RIGHT");
                                }
                                SECLAY_TEXT(textAlignment, infoTextConfig);
                                // .textColor
                                SECLAY_TEXT(SECLAY_STRING("Text Color"), infoTitleConfig);
                                SEClay__RenderDebugViewColor(textConfig->textColor, infoTextConfig);
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Aspect: {
                            SEClay_AspectRatioElementConfig *aspectRatioConfig = elementConfig->config.aspectRatioElementConfig;
                            SECLAY({ .id = SECLAY_ID("SEClay__DebugViewElementInfoAspectRatioBody"), .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                                SECLAY_TEXT(SECLAY_STRING("Aspect Ratio"), infoTitleConfig);
                                // Aspect Ratio
                                SECLAY_TEXT(SEClay__IntToString(aspectRatioConfig->aspectRatio), infoTextConfig);
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Image: {
                            SEClay_ImageElementConfig *imageConfig = elementConfig->config.imageElementConfig;
                            SEClay_AspectRatioElementConfig aspectConfig = { 1 };
                            if (SEClay__ElementHasConfig(selectedItem->layoutElement, SEClay__ElementConfigType::Aspect)) {
                                aspectConfig = *SEClay__FindElementConfigWithType(selectedItem->layoutElement, SEClay__ElementConfigType::Aspect).aspectRatioElementConfig;
                            }
                            SECLAY({ .id = SECLAY_ID("SEClay__DebugViewElementInfoImageBody"), .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                                // Image Preview
                                SECLAY_TEXT(SECLAY_STRING("Preview"), infoTitleConfig);
                                SECLAY({ .layout = { .sizing = { .width = SEClay_SizingGrow(64, 128), .height = SEClay_SizingGrow(64, 128) }}, .aspectRatio = aspectConfig, .image = *imageConfig }) {}
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Clip: {
                            SEClay_ClipElementConfig *clipConfig = elementConfig->config.clipElementConfig;
                            SECLAY({ .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                                // .vertical
                                SECLAY_TEXT(SECLAY_STRING("Vertical"), infoTitleConfig);
                                SECLAY_TEXT(clipConfig->vertical ? SECLAY_STRING("true") : SECLAY_STRING("false") , infoTextConfig);
                                // .horizontal
                                SECLAY_TEXT(SECLAY_STRING("Horizontal"), infoTitleConfig);
                                SECLAY_TEXT(clipConfig->horizontal ? SECLAY_STRING("true") : SECLAY_STRING("false") , infoTextConfig);
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Floating: {
                            SEClay_FloatingElementConfig *floatingConfig = elementConfig->config.floatingElementConfig;
                            SECLAY({ .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                                // .offset
                                SECLAY_TEXT(SECLAY_STRING("Offset"), infoTitleConfig);
                                SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                                    SECLAY_TEXT(SECLAY_STRING("{ x: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(floatingConfig->offset.x), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(", y: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(floatingConfig->offset.y), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(" }"), infoTextConfig);
                                }
                                // .expand
                                SECLAY_TEXT(SECLAY_STRING("Expand"), infoTitleConfig);
                                SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                                    SECLAY_TEXT(SECLAY_STRING("{ width: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(floatingConfig->expand.width), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(", height: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(floatingConfig->expand.height), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(" }"), infoTextConfig);
                                }
                                // .zIndex
                                SECLAY_TEXT(SECLAY_STRING("z-index"), infoTitleConfig);
                                SECLAY_TEXT(SEClay__IntToString(floatingConfig->zIndex), infoTextConfig);
                                // .parentId
                                SECLAY_TEXT(SECLAY_STRING("Parent"), infoTitleConfig);
                                SEClay_LayoutElementHashMapItem *hashItem = SEClay__GetHashMapItem(floatingConfig->parentId);
                                SECLAY_TEXT(hashItem->elementId.stringId, infoTextConfig);
                                // .attachPoints
                                SECLAY_TEXT(SECLAY_STRING("Attach Points"), infoTitleConfig);
                                SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                                    SECLAY_TEXT(SECLAY_STRING("{ element: "), infoTextConfig);
                                    SEClay_String attachPointElement = SECLAY_STRING("LEFT_TOP");
                                    if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::LeftCenter) {
                                        attachPointElement = SECLAY_STRING("LEFT_CENTER");
                                    } else if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::LeftBottom) {
                                        attachPointElement = SECLAY_STRING("LEFT_BOTTOM");
                                    } else if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::CenterTop) {
                                        attachPointElement = SECLAY_STRING("CENTER_TOP");
                                    } else if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::CenterCenter) {
                                        attachPointElement = SECLAY_STRING("CENTER_CENTER");
                                    } else if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::CenterBottom) {
                                        attachPointElement = SECLAY_STRING("CENTER_BOTTOM");
                                    } else if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::RightTop) {
                                        attachPointElement = SECLAY_STRING("RIGHT_TOP");
                                    } else if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::RightCenter) {
                                        attachPointElement = SECLAY_STRING("RIGHT_CENTER");
                                    } else if (floatingConfig->attachPoints.element == SEClay_FloatingAttachPointType::RightBottom) {
                                        attachPointElement = SECLAY_STRING("RIGHT_BOTTOM");
                                    }
                                    SECLAY_TEXT(attachPointElement, infoTextConfig);
                                    SEClay_String attachPointParent = SECLAY_STRING("LEFT_TOP");
                                    if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::LeftCenter) {
                                        attachPointParent = SECLAY_STRING("LEFT_CENTER");
                                    } else if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::LeftBottom) {
                                        attachPointParent = SECLAY_STRING("LEFT_BOTTOM");
                                    } else if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::CenterTop) {
                                        attachPointParent = SECLAY_STRING("CENTER_TOP");
                                    } else if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::CenterCenter) {
                                        attachPointParent = SECLAY_STRING("CENTER_CENTER");
                                    } else if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::CenterBottom) {
                                        attachPointParent = SECLAY_STRING("CENTER_BOTTOM");
                                    } else if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::RightTop) {
                                        attachPointParent = SECLAY_STRING("RIGHT_TOP");
                                    } else if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::RightCenter) {
                                        attachPointParent = SECLAY_STRING("RIGHT_CENTER");
                                    } else if (floatingConfig->attachPoints.parent == SEClay_FloatingAttachPointType::RightBottom) {
                                        attachPointParent = SECLAY_STRING("RIGHT_BOTTOM");
                                    }
                                    SECLAY_TEXT(SECLAY_STRING(", parent: "), infoTextConfig);
                                    SECLAY_TEXT(attachPointParent, infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(" }"), infoTextConfig);
                                }
                                // .pointerCaptureMode
                                SECLAY_TEXT(SECLAY_STRING("Pointer Capture Mode"), infoTitleConfig);
                                SEClay_String pointerCaptureMode = SECLAY_STRING("NONE");
                                if (floatingConfig->pointerCaptureMode == SEClay_PointerCaptureMode::Passthrough) {
                                    pointerCaptureMode = SECLAY_STRING("PASSTHROUGH");
                                }
                                SECLAY_TEXT(pointerCaptureMode, infoTextConfig);
                                // .attachTo
                                SECLAY_TEXT(SECLAY_STRING("Attach To"), infoTitleConfig);
                                SEClay_String attachTo = SECLAY_STRING("NONE");
                                if (floatingConfig->attachTo == SEClay_FloatingAttachToElement::Parent) {
                                    attachTo = SECLAY_STRING("PARENT");
                                } else if (floatingConfig->attachTo == SEClay_FloatingAttachToElement::ElementWithId) {
                                    attachTo = SECLAY_STRING("ELEMENT_WITH_ID");
                                } else if (floatingConfig->attachTo == SEClay_FloatingAttachToElement::Root) {
                                    attachTo = SECLAY_STRING("ROOT");
                                }
                                SECLAY_TEXT(attachTo, infoTextConfig);
                                // .clipTo
                                SECLAY_TEXT(SECLAY_STRING("Clip To"), infoTitleConfig);
                                SEClay_String clipTo = SECLAY_STRING("ATTACHED_PARENT");
                                if (floatingConfig->clipTo == SEClay_FloatingClipToElement::None) {
                                    clipTo = SECLAY_STRING("NONE");
                                }
                                SECLAY_TEXT(clipTo, infoTextConfig);
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Border: {
                            SEClay_BorderElementConfig *borderConfig = elementConfig->config.borderElementConfig;
                            SECLAY({ .id = SECLAY_ID("SEClay__DebugViewElementInfoBorderBody"), .layout = { .padding = attributeConfigPadding, .childGap = 8, .layoutDirection = SEClay_LayoutDirection::TopToBottom } }) {
                                SECLAY_TEXT(SECLAY_STRING("Border Widths"), infoTitleConfig);
                                SECLAY({ .layout = { .layoutDirection = SEClay_LayoutDirection::LeftToRight } }) {
                                    SECLAY_TEXT(SECLAY_STRING("{ left: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(borderConfig->width.left), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(", right: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(borderConfig->width.right), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(", top: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(borderConfig->width.top), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(", bottom: "), infoTextConfig);
                                    SECLAY_TEXT(SEClay__IntToString(borderConfig->width.bottom), infoTextConfig);
                                    SECLAY_TEXT(SECLAY_STRING(" }"), infoTextConfig);
                                }
                                // .textColor
                                SECLAY_TEXT(SECLAY_STRING("Border Color"), infoTitleConfig);
                                SEClay__RenderDebugViewColor(borderConfig->color, infoTextConfig);
                            }
                            break;
                        }
                        case SEClay__ElementConfigType::Custom:
                        default: break;
                    }
                }
            }
        } else {
            SECLAY({ .id = SECLAY_ID("SEClay__DebugViewWarningsScrollPane"), .layout = { .sizing = {SEClay_SizingGrow(0), SEClay_SizingFixed(300)}, .childGap = 6, .layoutDirection = SEClay_LayoutDirection::TopToBottom }, .backgroundColor = SECLAY__DEBUGVIEW_COLOR_2, .clip = { .horizontal = true, .vertical = true, .childOffset = SEClay_GetScrollOffset() } }) {
                SEClay_TextElementConfig *warningConfig = SECLAY_TEXT_CONFIG({ .textColor = SECLAY__DEBUGVIEW_COLOR_4, .fontSize = 16, .wrapMode = SEClay_TextElementConfigWrapMode::None });
                SECLAY({ .id = SECLAY_ID("SEClay__DebugViewWarningItemHeader"), .layout = { .sizing = {.height = SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT)}, .padding = {SECLAY__DEBUGVIEW_OUTER_PADDING, SECLAY__DEBUGVIEW_OUTER_PADDING, 0, 0 }, .childGap = 8, .childAlignment = {.y = SEClay_LayoutAlignmentY::Center} } }) {
                    SECLAY_TEXT(SECLAY_STRING("Warnings"), warningConfig);
                }
                SECLAY({ .id = SECLAY_ID("SEClay__DebugViewWarningsTopBorder"), .layout = { .sizing = { .width = SEClay_SizingGrow(0), .height = SEClay_SizingFixed(1)} }, .backgroundColor = {0.7843f, 0.7843f, 0.7843f, 1.0f} }) {}
                int32_t previousWarningsLength = context->warnings.length;
                for (int32_t i = 0; i < previousWarningsLength; i++) {
                    SEClay__Warning warning = context->warnings.internalArray[i];
                    SECLAY({ .id = SECLAY_IDI("SEClay__DebugViewWarningItem", i), .layout = { .sizing = {.height = SEClay_SizingFixed(SECLAY__DEBUGVIEW_ROW_HEIGHT)}, .padding = {SECLAY__DEBUGVIEW_OUTER_PADDING, SECLAY__DEBUGVIEW_OUTER_PADDING, 0, 0 }, .childGap = 8, .childAlignment = {.y = SEClay_LayoutAlignmentY::Center} } }) {
                        SECLAY_TEXT(warning.baseMessage, warningConfig);
                        if (warning.dynamicMessage.length > 0) {
                            SECLAY_TEXT(warning.dynamicMessage, warningConfig);
                        }
                    }
                }
            }
        }
    }
}
#pragma endregion

uint32_t SEClay__debugViewWidth = 400;
SEClay_Color SEClay__debugViewHighlightColor = {0.6588f, 0.2588f, 0.1098f, 0.3922f};

// HARDENING: reports an error through the current context's handler. Safe when there is no current context (the
// failure paths below can run before one exists, or after it was cleared) and when the handler is somehow unset.
static void SEClay__ReportErrorEx(SEClay_ErrorType errorType, SEClay_ErrorDetail detail, SEClay_String errorText, int64_t value, int64_t limit,
                                const char *sourceFile, int32_t sourceLine, const char *sourceFunction) {
    SEClay_Context *context = SEClay_GetCurrentContext();
    if (context != NULL && context->errorHandler.errorHandlerFunction != NULL) {
        context->errorHandler.errorHandlerFunction(SEClay_ErrorData {
            .errorType = errorType,
            .errorText = errorText,
            .userData = context->errorHandler.userData,
            .detail = detail,
            .value = value,
            .limit = limit,
            .sourceFile = sourceFile,
            .sourceFunction = sourceFunction,
            .sourceLine = sourceLine,
            .frame = context->generation });
    }
}

SEClay__WarningArray SEClay__WarningArray_Allocate_Arena(int32_t capacity, SEClay_Arena *arena) {
    // HARDENING: this used to size the allocation with sizeof(SEClay_String) -- half of sizeof(SEClay__Warning) -- so
    // filling the array wrote past its slice into whatever the arena held next.
    SEClay__Warning *storage = (SEClay__Warning *)SEClay__Array_Allocate_Arena(capacity, (uint32_t)sizeof(SEClay__Warning), arena);
    SEClay__WarningArray array = {.capacity = storage ? capacity : 0, .length = 0, .internalArray = storage};
    return array;
}

SEClay__Warning *SEClay__WarningArray_Add(SEClay__WarningArray *array, SEClay__Warning item)
{
    if (array->internalArray && array->length >= 0 && array->length < array->capacity) {
        array->internalArray[array->length++] = item;
        return &array->internalArray[array->length - 1];
    }
    memset((void *)&SECLAY__WARNING_DEFAULT, 0, sizeof(SECLAY__WARNING_DEFAULT));
    return &SECLAY__WARNING_DEFAULT;
}

void* SEClay__Array_Allocate_Arena(int32_t capacity, uint32_t itemSize, SEClay_Arena *arena)
{
    // HARDENING: do the size math in size_t (the product used to wrap in 32 bits), reject negative capacities and
    // compare without adding (offset + size could wrap), and flag the failure so SEClay_Initialize can refuse to
    // hand out a half-built context.
    if (arena != NULL && capacity >= 0) {
        size_t totalSizeBytes = (size_t)capacity * (size_t)itemSize;
        uintptr_t nextAllocOffset = arena->nextAllocation + (64 - (arena->nextAllocation % 64));
        if (nextAllocOffset <= arena->capacity && totalSizeBytes <= arena->capacity - nextAllocOffset) {
            arena->nextAllocation = nextAllocOffset + totalSizeBytes;
            return (void*)((uintptr_t)arena->memory + (uintptr_t)nextAllocOffset);
        }
    }
    if (SEClay__currentContext != NULL) {
        SEClay__currentContext->arenaAllocationFailed = true;
    }
    SECLAY__REPORT(SEClay_ErrorType::ArenaCapacityExceeded, SEClay_ErrorDetail::ArenaTooSmall,
                 "Clay could not fit its internal buffers in the memory it was given. Give it at least SEClay_MinMemorySize() bytes, or use a growable context (SEClay_InitializeGrowable).",
                 arena != NULL ? (int64_t)arena->nextAllocation + (int64_t)capacity * (int64_t)itemSize : 0, arena != NULL ? (int64_t)arena->capacity : 0);
    return SECLAY__NULL;
}

bool SEClay__Array_RangeCheck(int32_t index, int32_t length)
{
    if (index < length && index >= 0) {
        return true;
    }
    SECLAY__REPORT(SEClay_ErrorType::InternalError, SEClay_ErrorDetail::OutOfBoundsAccess,
                 "Clay caught an out-of-bounds array read and returned a default value instead. This is an internal error and is likely a bug in Clay or in how it was driven; 'value' is the index, 'limit' the length.",
                 index, length);
    return false;
}

bool SEClay__Array_AddCapacityCheck(int32_t length, int32_t capacity)
{
    // HARDENING: a negative length (a decrement that ran past zero) used to pass this check and write at index -1.
    if (length >= 0 && length < capacity) {
        return true;
    }
    SECLAY__REPORT(SEClay_ErrorType::InternalError, SEClay_ErrorDetail::OutOfBoundsAccess,
                 "Clay caught a write past the end of an internal array and dropped it. This is an internal error and is likely a bug in Clay or in how it was driven; 'value' is the length, 'limit' the capacity.",
                 length, capacity);
    return false;
}

// PUBLIC API FROM HERE ---------------------------------------

SECLAY_WASM_EXPORT("SEClay_MinMemorySize")
uint32_t SEClay_MinMemorySize(void) {
    SEClay_Context fakeContext = {
        .maxElementCount = SEClay__defaultMaxElementCount,
        .maxMeasureTextCacheWordCount = SEClay__defaultMaxMeasureTextWordCacheCount,
        .internalArena = {
            .capacity = SIZE_MAX,
            .memory = NULL,
        }
    };
    SEClay_Context* currentContext = SEClay_GetCurrentContext();
    if (currentContext) {
        fakeContext.maxElementCount = currentContext->maxElementCount;
        fakeContext.maxMeasureTextCacheWordCount = currentContext->maxMeasureTextCacheWordCount;
    }
    // Reserve space in the arena for the context, important for calculating min memory size correctly
    fakeContext.maxElementCount = SEClay__ClampMaxElementCount(fakeContext.maxElementCount);
    fakeContext.maxMeasureTextCacheWordCount = SEClay__ClampMaxMeasureTextWordCount(fakeContext.maxMeasureTextCacheWordCount);
    SEClay__Context_Allocate_Arena(&fakeContext.internalArena);
    SEClay__InitializePersistentMemory(&fakeContext);
    SEClay__InitializeEphemeralMemory(&fakeContext);
    // HARDENING: saturate instead of silently truncating a >4GiB requirement into a tiny (wrong) arena size.
    const uint64_t required = (uint64_t)fakeContext.internalArena.nextAllocation + 128u;
    return required > UINT32_MAX ? UINT32_MAX : (uint32_t)required;
}

SECLAY_WASM_EXPORT("SEClay_CreateArenaWithCapacityAndMemory")
SEClay_Arena SEClay_CreateArenaWithCapacityAndMemory(size_t capacity, void *memory) {
    SEClay_Arena arena = {
        .capacity = capacity,
        .memory = (char *)memory
    };
    return arena;
}

#ifndef SECLAY_WASM
void SEClay_SetMeasureTextFunction(SEClay_Dimensions (*measureTextFunction)(SEClay_StringSlice text, SEClay_TextElementConfig *config, void *userData), void *userData) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    SEClay__MeasureText = measureTextFunction;
    context->measureTextUserData = userData;
}
void SEClay_SetQueryScrollOffsetFunction(SEClay_Vector2 (*queryScrollOffsetFunction)(uint32_t elementId, void *userData), void *userData) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    SEClay__QueryScrollOffset = queryScrollOffsetFunction;
    context->queryScrollOffsetUserData = userData;
}
#endif

SECLAY_WASM_EXPORT("SEClay_SetLayoutDimensions")
void SEClay_SetLayoutDimensions(SEClay_Dimensions dimensions) {
    SEClay_GetCurrentContext()->layoutDimensions = dimensions;
}

SECLAY_WASM_EXPORT("SEClay_SetPointerState")
void SEClay_SetPointerState(SEClay_Vector2 position, bool isPointerDown) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    // HARDENING+: the previous layout is always a valid (if possibly partial) tree now, so hit testing no longer has to be skipped
    // after an overflow. It only needs its buffers to exist.
    if (context->treeNodeVisited.internalArray == NULL || context->layoutElementChildrenBuffer.internalArray == NULL) {
        return;
    }
    context->pointerInfo.position = position;
    context->pointerOverIds.length = 0;
    SEClay__int32_tArray dfsBuffer = context->layoutElementChildrenBuffer;
    for (int32_t rootIndex = context->layoutElementTreeRoots.length - 1; rootIndex >= 0; --rootIndex) {
        dfsBuffer.length = 0;
        SEClay__LayoutElementTreeRoot *root = SEClay__LayoutElementTreeRootArray_Get(&context->layoutElementTreeRoots, rootIndex);
        SEClay__int32_tArray_Add(&dfsBuffer, (int32_t)root->layoutElementIndex);
        if (dfsBuffer.length <= 0 || context->treeNodeVisited.internalArray == NULL) {
            continue;
        }
        context->treeNodeVisited.internalArray[0] = false;
        bool found = false;
        while (dfsBuffer.length > 0) {
            if (context->treeNodeVisited.internalArray[dfsBuffer.length - 1]) {
                dfsBuffer.length--;
                continue;
            }
            context->treeNodeVisited.internalArray[dfsBuffer.length - 1] = true;
            SEClay_LayoutElement *currentElement = SEClay_LayoutElementArray_Get(&context->layoutElements, SEClay__int32_tArray_GetValue(&dfsBuffer, (int)dfsBuffer.length - 1));
            SEClay_LayoutElementHashMapItem *mapItem = SEClay__GetHashMapItem(currentElement->id); // TODO think of a way around this, maybe the fact that it's essentially a binary tree limits the cost, but the worst case is not great
            int32_t clipElementId = SEClay__int32_tArray_GetValue(&context->layoutElementClipElementIds, (int32_t)(currentElement - context->layoutElements.internalArray));
            SEClay_LayoutElementHashMapItem *clipItem = SEClay__GetHashMapItem(clipElementId);
            if (mapItem) {
                SEClay_BoundingBox elementBox = mapItem->boundingBox;
                elementBox.x -= root->pointerOffset.x;
                elementBox.y -= root->pointerOffset.y;
                // HARDENING: a lookup miss yields the sentinel item (never NULL), which used to be hit-tested against
                // its all-zero box (and could carry a stale hover callback); a miss is simply "not laid out".
                if (mapItem != &SEClay_LayoutElementHashMapItem_DEFAULT && (SEClay__PointIsInsideRect(position, elementBox)) && (clipElementId == 0 || (clipItem != &SEClay_LayoutElementHashMapItem_DEFAULT && SEClay__PointIsInsideRect(position, clipItem->boundingBox)))) {
                    if (mapItem->onHoverFunction) {
                        mapItem->onHoverFunction(mapItem->elementId, context->pointerInfo, mapItem->hoverFunctionUserData);
                    }
                    // The stored id's text lives inside the map entry, which moves when the map is compacted; hand out the id without it.
                    SEClay_ElementId overId = mapItem->elementId;
                    overId.stringId = SECLAY__STRING_DEFAULT;
                    SEClay_ElementIdArray_Add(&context->pointerOverIds, overId);
                    found = true;

                    if (mapItem->idAlias != 0) {
                        SEClay_ElementIdArray_Add(&context->pointerOverIds, SEClay_ElementId { .id = mapItem->idAlias });
                    }
                }
                if (SEClay__ElementHasConfig(currentElement, SEClay__ElementConfigType::Text)) {
                    dfsBuffer.length--;
                    continue;
                }
                // HARDENING: an element left open by an abandoned frame has a child count but no child list yet, and the
                // visited flag was written at length-1 even when the push was rejected because the stack was full.
                if (currentElement->childrenOrTextContent.children.elements != NULL) {
                    for (int32_t i = currentElement->childrenOrTextContent.children.length - 1; i >= 0; --i) {
                        if (dfsBuffer.length < dfsBuffer.capacity && dfsBuffer.length < context->treeNodeVisited.capacity) {
                            SEClay__int32_tArray_Add(&dfsBuffer, currentElement->childrenOrTextContent.children.elements[i]);
                            context->treeNodeVisited.internalArray[dfsBuffer.length - 1] = false;
                        }
                    }
                }
            } else {
                dfsBuffer.length--;
            }
        }

        SEClay_LayoutElement *rootElement = SEClay_LayoutElementArray_Get(&context->layoutElements, root->layoutElementIndex);
        if (found && SEClay__ElementHasConfig(rootElement, SEClay__ElementConfigType::Floating) &&
                SEClay__FindElementConfigWithType(rootElement, SEClay__ElementConfigType::Floating).floatingElementConfig->pointerCaptureMode == SEClay_PointerCaptureMode::Capture) {
            break;
        }
    }

    if (isPointerDown) {
        if (context->pointerInfo.state == SEClay_PointerDataInteractionState::PressedThisFrame) {
            context->pointerInfo.state = SEClay_PointerDataInteractionState::Pressed;
        } else if (context->pointerInfo.state != SEClay_PointerDataInteractionState::Pressed) {
            context->pointerInfo.state = SEClay_PointerDataInteractionState::PressedThisFrame;
        }
    } else {
        if (context->pointerInfo.state == SEClay_PointerDataInteractionState::ReleasedThisFrame) {
            context->pointerInfo.state = SEClay_PointerDataInteractionState::Released;
        } else if (context->pointerInfo.state != SEClay_PointerDataInteractionState::Released)  {
            context->pointerInfo.state = SEClay_PointerDataInteractionState::ReleasedThisFrame;
        }
    }
}

SECLAY_WASM_EXPORT("SEClay_Initialize")
SEClay_Context* SEClay_Initialize(SEClay_Arena arena, SEClay_Dimensions layoutDimensions, SEClay_ErrorHandler errorHandler) {
    SEClay_Context *context = SEClay__Context_Allocate_Arena(&arena);
    if (context == NULL) return NULL;
    // DEFAULTS
    SEClay_Context *oldContext = SEClay_GetCurrentContext();
    *context = SEClay_Context {
        .maxElementCount = oldContext ? oldContext->maxElementCount : SEClay__defaultMaxElementCount,
        .maxMeasureTextCacheWordCount = oldContext ? oldContext->maxMeasureTextCacheWordCount : SEClay__defaultMaxMeasureTextWordCacheCount,
        .errorHandler = errorHandler.errorHandlerFunction ? errorHandler : SEClay_ErrorHandler { SEClay__ErrorHandlerFunctionDefault, 0 },
        .layoutDimensions = layoutDimensions,
        .internalArena = arena,
    };
    // HARDENING: sanitize the sizes the arrays are derived from before anything is allocated from them.
    context->maxElementCount = SEClay__ClampMaxElementCount(context->maxElementCount);
    context->maxMeasureTextCacheWordCount = SEClay__ClampMaxMeasureTextWordCount(context->maxMeasureTextCacheWordCount);
    SEClay_SetCurrentContext(context);
    SEClay__InitializePersistentMemory(context);
    SEClay__InitializeEphemeralMemory(context);
    if (context->arenaAllocationFailed) {
        // The arena was too small for the requested sizes: some arrays have no storage. Refuse to hand the context out
        // (the old code returned it and the first write through a NULL array crashed), and restore the previous one.
        SEClay_SetCurrentContext(oldContext);
        return NULL;
    }
    for (int32_t i = 0; i < context->layoutElementsHashMap.capacity; ++i) {
        context->layoutElementsHashMap.internalArray[i] = -1;
    }
    for (int32_t i = 0; i < context->measureTextHashMap.capacity; ++i) {
        context->measureTextHashMap.internalArray[i] = 0;
    }
    context->measureTextHashMapInternal.length = 1; // Reserve the 0 value to mean "no next element"
    context->layoutDimensions = layoutDimensions;
    return context;
}

SECLAY_WASM_EXPORT("SEClay_InitializeGrowable")
SEClay_Context* SEClay_InitializeGrowable(const SEClay_GrowableConfig *config) {
    SEClay_Context *previous = SEClay_GetCurrentContext();
    SEClay_GrowableConfig cfg;
    memset((void *)&cfg, 0, sizeof(cfg));
    if (config != NULL) {
        cfg = *config;
    }
    SEClay_Allocator allocator = cfg.allocator;
    if (allocator.allocate == NULL || allocator.release == NULL) {
        allocator.allocate = SEClay__DefaultAllocate;
        allocator.release = SEClay__DefaultRelease;
        allocator.userData = NULL;
    }
    SEClay_Context *context = (SEClay_Context *)allocator.allocate(sizeof(SEClay_Context), allocator.userData);
    if (context == NULL) {
        return NULL;
    }
    memset((void *)context, 0, sizeof(SEClay_Context));
    context->growable = true;
    context->allocator = allocator;
    context->allowShrink = cfg.allowShrink;
    context->memoryLimitBytes = cfg.memoryLimitBytes != 0 ? cfg.memoryLimitBytes : ((size_t)1 << 30);
    context->errorHandler = cfg.errorHandler.errorHandlerFunction ? cfg.errorHandler : SEClay_ErrorHandler { SEClay__ErrorHandlerFunctionDefault, 0 };
    context->layoutDimensions = cfg.layoutDimensions;
    context->initialElements = SEClay__ClampMaxElementCount(cfg.initialElementCount != 0 ? cfg.initialElementCount : 2048);
    context->initialWords = SEClay__ClampMaxMeasureTextWordCount(cfg.initialMeasureWordCount != 0 ? cfg.initialMeasureWordCount : 16384);
    context->maxElementCount = context->initialElements;
    context->maxMeasureTextCacheWordCount = context->initialWords;

    SEClay__BlockSizesFor(context->maxElementCount, context->maxMeasureTextCacheWordCount, &context->persistentBlockSize, &context->ephemeralBlockSize);
    context->persistentBlock = SEClay__AllocateBlock(&allocator, context->persistentBlockSize);
    context->ephemeralBlock = context->persistentBlock != NULL ? SEClay__AllocateBlock(&allocator, context->ephemeralBlockSize) : NULL;
    if (context->persistentBlock == NULL || context->ephemeralBlock == NULL) {
        SEClay__ReleaseBlock(&allocator, context->persistentBlock, context->persistentBlockSize);
        SEClay__ReleaseBlock(&allocator, context->ephemeralBlock, context->ephemeralBlockSize);
        SEClay_SetCurrentContext(context);
        SECLAY__REPORT(SEClay_ErrorType::AllocationFailed, SEClay_ErrorDetail::Allocation,
                     "The allocator refused the memory for a new layout context.", context->persistentBlockSize + context->ephemeralBlockSize, 0);
        SEClay_SetCurrentContext(previous);
        allocator.release(context, sizeof(SEClay_Context), allocator.userData);
        return NULL;
    }
    context->persistentArena.memory = context->persistentBlock;
    context->persistentArena.capacity = context->persistentBlockSize;
    context->ephemeralArena.memory = context->ephemeralBlock;
    context->ephemeralArena.capacity = context->ephemeralBlockSize;
    SEClay_SetCurrentContext(context);
    SEClay__InitializePersistentMemory(context);
    SEClay__InitializeEphemeralMemory(context);
    for (int32_t i = 0; i < context->layoutElementsHashMap.capacity; ++i) {
        context->layoutElementsHashMap.internalArray[i] = -1;
    }
    for (int32_t i = 0; i < context->measureTextHashMap.capacity; ++i) {
        context->measureTextHashMap.internalArray[i] = 0;
    }
    context->measureTextHashMapInternal.length = 1;
    return context;
}

SECLAY_WASM_EXPORT("SEClay_DestroyContext")
void SEClay_DestroyContext(SEClay_Context* context) {
    if (context == NULL) {
        return;
    }
    const bool growable = context->growable;
    const SEClay_Allocator allocator = context->allocator;
    char *persistent = context->persistentBlock;
    char *ephemeral = context->ephemeralBlock;
    char *retired = context->retiredBlock;
    const size_t persistentSize = context->persistentBlockSize;
    const size_t ephemeralSize = context->ephemeralBlockSize;
    const size_t retiredSize = context->retiredBlockSize;
    SEClay_ReleaseContextResources(context);
    if (growable) {
        SEClay__ReleaseBlock(&allocator, persistent, persistentSize);
        SEClay__ReleaseBlock(&allocator, ephemeral, ephemeralSize);
        SEClay__ReleaseBlock(&allocator, retired, retiredSize);
        allocator.release(context, sizeof(SEClay_Context), allocator.userData);
    }
}

SECLAY_WASM_EXPORT("SEClay_SetErrorHandler")
void SEClay_SetErrorHandler(SEClay_ErrorHandler errorHandler) {
    SEClay_Context *context = SEClay_GetCurrentContext();
    if (context != NULL) {
        context->errorHandler = errorHandler.errorHandlerFunction ? errorHandler : SEClay_ErrorHandler { SEClay__ErrorHandlerFunctionDefault, 0 };
    }
}

SECLAY_WASM_EXPORT("SEClay_GetMemoryStats")
SEClay_MemoryStats SEClay_GetMemoryStats(void) {
    SEClay_MemoryStats stats;
    memset((void *)&stats, 0, sizeof(stats));
    SEClay_Context *context = SEClay_GetCurrentContext();
    if (context == NULL) {
        return stats;
    }
    stats.growable = context->growable;
    stats.frame = context->generation;
    stats.truncatedLastLayout = context->lastLayoutTruncated;
    stats.bytesReserved = context->growable
        ? context->persistentBlockSize + context->ephemeralBlockSize + context->retiredBlockSize
        : (size_t)context->internalArena.capacity;
    stats.bytesLimit = context->growable ? context->memoryLimitBytes : (size_t)context->internalArena.capacity;
    stats.elementBudget = context->maxElementCount;
    stats.measureWordBudget = context->maxMeasureTextCacheWordCount;
    stats.usedElements = context->lastUsageElements;
    stats.capacityElements = context->layoutElements.capacity;
    stats.usedElementConfigs = context->lastUsageElementConfigs;
    stats.capacityElementConfigs = context->elementConfigs.capacity;
    stats.usedWrappedTextLines = context->lastUsageWrappedLines;
    stats.capacityWrappedTextLines = context->wrappedTextLines.capacity;
    stats.usedRenderCommands = context->lastUsageRenderCommands;
    stats.capacityRenderCommands = context->renderCommands.capacity;
    stats.growCount = context->growCount;
    stats.shrinkCount = context->shrinkCount;
    return stats;
}

SECLAY_WASM_EXPORT("SEClay_GetOpenElementPath")
int32_t SEClay_GetOpenElementPath(SEClay_ElementPathEntry *out, int32_t capacity) {
    SEClay_Context *context = SEClay_GetCurrentContext();
    if (context == NULL || context->openLayoutElementStack.internalArray == NULL) {
        return 0;
    }
    int32_t count = 0;
    int32_t previousIndex = -1;
    for (int32_t i = 0; i < context->openLayoutElementStack.length; i++) {
        const int32_t elementIndex = context->openLayoutElementStack.internalArray[i];
        if (elementIndex == previousIndex || elementIndex < 0 || elementIndex >= context->layoutElements.length) {
            continue; // the layout root is on the stack twice by construction; skip stale indices defensively
        }
        previousIndex = elementIndex;
        if (out != NULL && count < capacity) {
            const SEClay_LayoutElement *element = &context->layoutElements.internalArray[elementIndex];
            out[count].id = element->id;
            out[count].offset = 0;
            out[count].childIndex = 0;
            out[count].isText = 0;
            if (i > 0) {
                const int32_t parentIndex = context->openLayoutElementStack.internalArray[i - 1];
                if (parentIndex >= 0 && parentIndex < context->layoutElements.length) {
                    out[count].childIndex = context->layoutElements.internalArray[parentIndex].childrenOrTextContent.children.length;
                }
            }
        }
        count++;
    }
    return count;
}

SECLAY_WASM_EXPORT("SEClay_GetCurrentContext")
SEClay_Context* SEClay_GetCurrentContext(void) {
    return SEClay__currentContext;
}

SECLAY_WASM_EXPORT("SEClay_SetCurrentContext")
void SEClay_SetCurrentContext(SEClay_Context* context) {
    SEClay__currentContext = context;
}

SECLAY_WASM_EXPORT("SEClay_GetScrollOffset")
SEClay_Vector2 SEClay_GetScrollOffset(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->ghostDepth > 0 || context->openLayoutElementStack.length <= 0) {
        return SEClay_Vector2 SECLAY__DEFAULT_STRUCT;
    }
    SEClay_LayoutElement *openLayoutElement = SEClay__GetOpenLayoutElement();
    // If the element has no id attached at this point, we need to generate one
    if (openLayoutElement->id == 0) {
        SEClay__GenerateIdForAnonymousElement(openLayoutElement);
    }
    {
        SEClay__ScrollContainerDataInternal *mapping = SEClay__FindScrollContainer(context, openLayoutElement->id);
        if (mapping && mapping->layoutElement == openLayoutElement) {
            return mapping->scrollPosition;
        }
    }
    return SEClay_Vector2 SECLAY__DEFAULT_STRUCT;
}

SECLAY_WASM_EXPORT("SEClay_UpdateScrollContainers")
void SEClay_UpdateScrollContainers(bool enableDragScrolling, SEClay_Vector2 scrollDelta, float deltaTime) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    bool isPointerActive = enableDragScrolling && (context->pointerInfo.state == SEClay_PointerDataInteractionState::Pressed || context->pointerInfo.state == SEClay_PointerDataInteractionState::PressedThisFrame);
    // Every scroll container under the pointer, with its depth in the pointer-over list (deeper = more inner). A wheel
    // step is offered innermost first: each container takes as much as it can still scroll, and what it cannot take
    // moves on to the next one out. A container that cannot scroll at all (a clipped card, a text box that fits) just
    // passes the whole step through, so it no longer swallows scrolling meant for the list around it.
    enum { SECLAY__MAX_SCROLL_CHAIN = 64 };
    SEClay__ScrollContainerDataInternal *chain[SECLAY__MAX_SCROLL_CHAIN];
    int32_t chainDepth[SECLAY__MAX_SCROLL_CHAIN];
    int32_t chainLength = 0;
    for (int32_t i = 0; i < context->scrollContainerDatas.length; i++) {
        SEClay__ScrollContainerDataInternal *scrollData = SEClay__ScrollContainerDataInternalArray_Get(&context->scrollContainerDatas, i);
        if (!scrollData->openThisFrame) {
            SEClay__ScrollContainerDataInternalArray_RemoveSwapback(&context->scrollContainerDatas, i);
            context->scrollIndexDirty = true;
            i--; // HARDENING: the swapped-in last entry now sits at i and has not been visited yet
            continue;
        }
        scrollData->openThisFrame = false;
        SEClay_LayoutElementHashMapItem *hashMapItem = SEClay__GetHashMapItem(scrollData->elementId);
        // Element isn't rendered this frame but scroll offset has been retained
        // HARDENING: a miss returns the sentinel, never NULL, so this removal never happened.
        if (!hashMapItem || hashMapItem == &SEClay_LayoutElementHashMapItem_DEFAULT) {
            SEClay__ScrollContainerDataInternalArray_RemoveSwapback(&context->scrollContainerDatas, i);
            context->scrollIndexDirty = true;
            i--;
            continue;
        }

        // Touch / click is released
        if (!isPointerActive && scrollData->pointerScrollActive) {
            float xDiff = scrollData->scrollPosition.x - scrollData->scrollOrigin.x;
            if (xDiff < -10 || xDiff > 10) {
                scrollData->scrollMomentum.x = (scrollData->scrollPosition.x - scrollData->scrollOrigin.x) / (scrollData->momentumTime * 25);
            }
            float yDiff = scrollData->scrollPosition.y - scrollData->scrollOrigin.y;
            if (yDiff < -10 || yDiff > 10) {
                scrollData->scrollMomentum.y = (scrollData->scrollPosition.y - scrollData->scrollOrigin.y) / (scrollData->momentumTime * 25);
            }
            scrollData->pointerScrollActive = false;

            scrollData->pointerOrigin = SEClay_Vector2{0,0};
            scrollData->scrollOrigin = SEClay_Vector2{0,0};
            scrollData->momentumTime = 0;
        }

        // Apply existing momentum
        scrollData->scrollPosition.x += scrollData->scrollMomentum.x;
        scrollData->scrollMomentum.x *= 0.95f;
        bool scrollOccurred = scrollDelta.x != 0 || scrollDelta.y != 0;
        if ((scrollData->scrollMomentum.x > -0.1f && scrollData->scrollMomentum.x < 0.1f) || scrollOccurred) {
            scrollData->scrollMomentum.x = 0;
        }
        scrollData->scrollPosition.x = SECLAY__MIN(SECLAY__MAX(scrollData->scrollPosition.x, -(SECLAY__MAX(scrollData->contentSize.width - scrollData->layoutElement->dimensions.width, 0))), 0);

        scrollData->scrollPosition.y += scrollData->scrollMomentum.y;
        scrollData->scrollMomentum.y *= 0.95f;
        if ((scrollData->scrollMomentum.y > -0.1f && scrollData->scrollMomentum.y < 0.1f) || scrollOccurred) {
            scrollData->scrollMomentum.y = 0;
        }
        scrollData->scrollPosition.y = SECLAY__MIN(SECLAY__MAX(scrollData->scrollPosition.y, -(SECLAY__MAX(scrollData->contentSize.height - scrollData->layoutElement->dimensions.height, 0))), 0);

        int32_t depth = -1;
        for (int32_t j = 0; j < context->pointerOverIds.length; ++j) {
            if (scrollData->layoutElement->id == SEClay_ElementIdArray_Get(&context->pointerOverIds, j)->id) {
                depth = j;
            }
        }
        if (depth >= 0 && chainLength < SECLAY__MAX_SCROLL_CHAIN) {
            // Insertion sort, deepest first.
            int32_t at = chainLength++;
            while (at > 0 && chainDepth[at - 1] < depth) {
                chain[at] = chain[at - 1];
                chainDepth[at] = chainDepth[at - 1];
                --at;
            }
            chain[at] = scrollData;
            chainDepth[at] = depth;
        }
    }

    // Wheel: propagate the step outwards, each container consuming what it can.
    float remainingX = scrollDelta.x * 10;
    float remainingY = scrollDelta.y * 10;
    for (int32_t c = 0; c < chainLength && (remainingX != 0 || remainingY != 0); ++c) {
        SEClay__ScrollContainerDataInternal *data = chain[c];
        SEClay_LayoutElement *element = data->layoutElement;
        SEClay_ClipElementConfig *clip = SEClay__FindElementConfigWithType(element, SEClay__ElementConfigType::Clip).clipElementConfig;
        if (!clip) {
            continue;
        }
        if (clip->vertical && remainingY != 0) {
            const float minY = -SECLAY__MAX(data->contentSize.height - element->dimensions.height, 0);
            const float before = data->scrollPosition.y;
            const float after = SECLAY__MAX(SECLAY__MIN(before + remainingY, 0), minY);
            data->scrollPosition.y = after;
            remainingY -= after - before;
            if (remainingY > -0.01f && remainingY < 0.01f) {
                remainingY = 0;
            }
        }
        if (clip->horizontal && remainingX != 0) {
            const float minX = -SECLAY__MAX(data->contentSize.width - element->dimensions.width, 0);
            const float before = data->scrollPosition.x;
            const float after = SECLAY__MAX(SECLAY__MIN(before + remainingX, 0), minX);
            data->scrollPosition.x = after;
            remainingX -= after - before;
            if (remainingX > -0.01f && remainingX < 0.01f) {
                remainingX = 0;
            }
        }
    }

    // Click / touch drag scrolls the innermost container that can actually scroll.
    int32_t highestPriorityElementIndex = -1;
    SEClay__ScrollContainerDataInternal *highestPriorityScrollData = SECLAY__NULL;
    for (int32_t c = 0; c < chainLength; ++c) {
        SEClay_LayoutElement *element = chain[c]->layoutElement;
        SEClay_ClipElementConfig *clip = SEClay__FindElementConfigWithType(element, SEClay__ElementConfigType::Clip).clipElementConfig;
        if (clip && ((clip->vertical && chain[c]->contentSize.height > element->dimensions.height) ||
                     (clip->horizontal && chain[c]->contentSize.width > element->dimensions.width))) {
            highestPriorityElementIndex = chainDepth[c];
            highestPriorityScrollData = chain[c];
            break;
        }
    }

    if (highestPriorityElementIndex > -1 && highestPriorityScrollData) {
        SEClay_LayoutElement *scrollElement = highestPriorityScrollData->layoutElement;
        SEClay_ClipElementConfig *clipConfig = SEClay__FindElementConfigWithType(scrollElement, SEClay__ElementConfigType::Clip).clipElementConfig;
        bool canScrollVertically = clipConfig->vertical && highestPriorityScrollData->contentSize.height > scrollElement->dimensions.height;
        bool canScrollHorizontally = clipConfig->horizontal && highestPriorityScrollData->contentSize.width > scrollElement->dimensions.width;
        // (Wheel steps were applied above, along the whole chain.)
        // Handle click / touch scroll
        if (isPointerActive) {
            highestPriorityScrollData->scrollMomentum = SEClay_Vector2 SECLAY__DEFAULT_STRUCT;
            if (!highestPriorityScrollData->pointerScrollActive) {
                highestPriorityScrollData->pointerOrigin = context->pointerInfo.position;
                highestPriorityScrollData->scrollOrigin = highestPriorityScrollData->scrollPosition;
                highestPriorityScrollData->pointerScrollActive = true;
            } else {
                float scrollDeltaX = 0, scrollDeltaY = 0;
                if (canScrollHorizontally) {
                    float oldXScrollPosition = highestPriorityScrollData->scrollPosition.x;
                    highestPriorityScrollData->scrollPosition.x = highestPriorityScrollData->scrollOrigin.x + (context->pointerInfo.position.x - highestPriorityScrollData->pointerOrigin.x);
                    highestPriorityScrollData->scrollPosition.x = SECLAY__MAX(SECLAY__MIN(highestPriorityScrollData->scrollPosition.x, 0), -(highestPriorityScrollData->contentSize.width - highestPriorityScrollData->boundingBox.width));
                    scrollDeltaX = highestPriorityScrollData->scrollPosition.x - oldXScrollPosition;
                }
                if (canScrollVertically) {
                    float oldYScrollPosition = highestPriorityScrollData->scrollPosition.y;
                    highestPriorityScrollData->scrollPosition.y = highestPriorityScrollData->scrollOrigin.y + (context->pointerInfo.position.y - highestPriorityScrollData->pointerOrigin.y);
                    highestPriorityScrollData->scrollPosition.y = SECLAY__MAX(SECLAY__MIN(highestPriorityScrollData->scrollPosition.y, 0), -(highestPriorityScrollData->contentSize.height - highestPriorityScrollData->boundingBox.height));
                    scrollDeltaY = highestPriorityScrollData->scrollPosition.y - oldYScrollPosition;
                }
                if (scrollDeltaX > -0.1f && scrollDeltaX < 0.1f && scrollDeltaY > -0.1f && scrollDeltaY < 0.1f && highestPriorityScrollData->momentumTime > 0.15f) {
                    highestPriorityScrollData->momentumTime = 0;
                    highestPriorityScrollData->pointerOrigin = context->pointerInfo.position;
                    highestPriorityScrollData->scrollOrigin = highestPriorityScrollData->scrollPosition;
                } else {
                     highestPriorityScrollData->momentumTime += deltaTime;
                }
            }
        }
        // Clamp any changes to scroll position to the maximum size of the contents
        if (canScrollVertically) {
            highestPriorityScrollData->scrollPosition.y = SECLAY__MAX(SECLAY__MIN(highestPriorityScrollData->scrollPosition.y, 0), -(highestPriorityScrollData->contentSize.height - scrollElement->dimensions.height));
        }
        if (canScrollHorizontally) {
            highestPriorityScrollData->scrollPosition.x = SECLAY__MAX(SECLAY__MIN(highestPriorityScrollData->scrollPosition.x, 0), -(highestPriorityScrollData->contentSize.width - scrollElement->dimensions.width));
        }
    }
}

SECLAY_WASM_EXPORT("SEClay_BeginLayout")
void SEClay_BeginLayout(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->generation++;
    context->dynamicElementIndex = 0;
    SEClay__SweepMeasureTextCache(context, 64); // a slice of the measured-text cache per frame, so stale words are returned
    // Forget element ids that were not declared last frame (see its comment). A context that may shrink also does it periodically
    // regardless of fullness, so the live-id count the shrink decision reads is real rather than inflated by dead ids.
    SEClay__CompactElementHashMap(context, context->growable && context->allowShrink && (context->generation & 255u) == 0);
    // HARDENING+: a growable context fits its buffers to what the previous layout needed -- here, between layouts, because the
    // layout in progress holds pointers into them. (A fixed-arena context keeps the sizes it was given.)
    SEClay__AdjustMemoryForNextLayout(context);
    SEClay__InitializeEphemeralMemory(context);
    // Set up the root container that covers the entire window
    SEClay_Dimensions rootDimensions = {context->layoutDimensions.width, context->layoutDimensions.height};
    if (context->debugModeEnabled) {
        rootDimensions.width -= (float)SEClay__debugViewWidth;
    }
    context->booleanWarnings = SEClay_BooleanWarnings SECLAY__DEFAULT_STRUCT;
    context->ghostDepth = 0;
    context->usageElements = 0;
    context->usageElementConfigs = 0;
    context->usageWrappedLines = 0;
    context->usageRenderCommands = 0;
    context->usageMeasureItemFailures = 0;
    context->usageMeasureWordFailures = 0;
    context->usageMeasureWordShortfall = 0;
    SEClay__OpenElement();
    SEClay__ConfigureOpenElement(SEClay_ElementDeclaration {
            .id = SECLAY_ID("SEClay__RootContainer"),
            .layout = { .sizing = {SEClay_SizingFixed((rootDimensions.width)), SEClay_SizingFixed(rootDimensions.height)} }
    });
    SEClay__int32_tArray_Add(&context->openLayoutElementStack, 0);
    SEClay__LayoutElementTreeRootArray_Add(&context->layoutElementTreeRoots, SEClay__LayoutElementTreeRoot { .layoutElementIndex = 0 });
}

SECLAY_WASM_EXPORT("SEClay_EndLayout")
SEClay_RenderCommandArray SEClay_EndLayout(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    SEClay__CloseElement();
    // HARDENING+: the debug view is an ordinary part of the layout that a host can switch on and off at runtime
    // (SEClay_SetDebugModeEnabled). It is skipped for a layout that is already truncated, since it would only add to the overflow.
    if (context->debugModeEnabled && !context->booleanWarnings.maxElementsExceeded && context->ghostDepth == 0) {
        context->warningsEnabled = false;
        SEClay__RenderDebugView();
        context->warningsEnabled = true;
    }
    // HARDENING+: an overflow no longer replaces the whole frame with an error message. Elements past the limit were left out as
    // they were declared, so what exists is a valid tree; lay it out and draw it, and let the reported error carry the news.
    SEClay__CalculateFinalLayout();
    context->lastUsageElements = context->usageElements;
    context->lastUsageElementConfigs = context->usageElementConfigs;
    context->lastUsageWrappedLines = context->usageWrappedLines;
    context->lastUsageRenderCommands = context->usageRenderCommands;
    context->lastLayoutTruncated = context->booleanWarnings.maxElementsExceeded || context->booleanWarnings.maxElementConfigsExceeded ||
                                   context->booleanWarnings.maxWrappedLinesExceeded || context->booleanWarnings.maxRenderCommandsExceeded ||
                                   context->booleanWarnings.maxTextMeasureCacheExceeded;
    return context->renderCommands;
}

SECLAY_WASM_EXPORT("SEClay_GetElementId")
SEClay_ElementId SEClay_GetElementId(SEClay_String idString) {
    return SEClay__HashString(idString, 0, 0);
}

SECLAY_WASM_EXPORT("SEClay_GetElementIdWithIndex")
SEClay_ElementId SEClay_GetElementIdWithIndex(SEClay_String idString, uint32_t index) {
    return SEClay__HashString(idString, index, 0);
}

bool SEClay_Hovered(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->ghostDepth > 0 || context->openLayoutElementStack.length <= 0) {
        return false;
    }
    SEClay_LayoutElement *openLayoutElement = SEClay__GetOpenLayoutElement();
    // If the element has no id attached at this point, we need to generate one
    if (openLayoutElement->id == 0) {
        SEClay__GenerateIdForAnonymousElement(openLayoutElement);
    }
    for (int32_t i = 0; i < context->pointerOverIds.length; ++i) {
        if (SEClay_ElementIdArray_Get(&context->pointerOverIds, i)->id == openLayoutElement->id) {
            return true;
        }
    }
    return false;
}

void SEClay_OnHover(void (*onHoverFunction)(SEClay_ElementId elementId, SEClay_PointerData pointerInfo, intptr_t userData), intptr_t userData) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    if (context->ghostDepth > 0 || context->openLayoutElementStack.length <= 0) {
        return;
    }
    SEClay_LayoutElement *openLayoutElement = SEClay__GetOpenLayoutElement();
    if (openLayoutElement->id == 0) {
        SEClay__GenerateIdForAnonymousElement(openLayoutElement);
    }
    SEClay_LayoutElementHashMapItem *hashMapItem = SEClay__GetHashMapItem(openLayoutElement->id);
    // HARDENING: a miss returns the shared sentinel; registering the callback on it would attach it to every later miss.
    if (hashMapItem == &SEClay_LayoutElementHashMapItem_DEFAULT) {
        return;
    }
    hashMapItem->onHoverFunction = onHoverFunction;
    hashMapItem->hoverFunctionUserData = userData;
}

SECLAY_WASM_EXPORT("SEClay_PointerOver")
bool SEClay_PointerOver(SEClay_ElementId elementId) { // TODO return priority for separating multiple results
    SEClay_Context* context = SEClay_GetCurrentContext();
    for (int32_t i = 0; i < context->pointerOverIds.length; ++i) {
        if (SEClay_ElementIdArray_Get(&context->pointerOverIds, i)->id == elementId.id) {
            return true;
        }
    }
    return false;
}

SECLAY_WASM_EXPORT("SEClay_GetScrollContainerData")
SEClay_ScrollContainerData SEClay_GetScrollContainerData(SEClay_ElementId id) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    for (int32_t i = 0; i < context->scrollContainerDatas.length; ++i) {
        SEClay__ScrollContainerDataInternal *scrollContainerData = SEClay__ScrollContainerDataInternalArray_Get(&context->scrollContainerDatas, i);
        if (scrollContainerData->elementId == id.id) {
            SEClay_ClipElementConfig *clipElementConfig = scrollContainerData->layoutElement ? SEClay__FindElementConfigWithType(scrollContainerData->layoutElement, SEClay__ElementConfigType::Clip).clipElementConfig : NULL;
            if (!clipElementConfig) { // This can happen on the first frame before a scroll container is declared
                return SEClay_ScrollContainerData SECLAY__DEFAULT_STRUCT;
            }
            return SEClay_ScrollContainerData {
                .scrollPosition = &scrollContainerData->scrollPosition,
                .scrollContainerDimensions = { scrollContainerData->boundingBox.width, scrollContainerData->boundingBox.height },
                .contentDimensions = scrollContainerData->contentSize,
                .config = *clipElementConfig,
                .found = true
            };
        }
    }
    return SEClay_ScrollContainerData SECLAY__DEFAULT_STRUCT;
}

SECLAY_WASM_EXPORT("SEClay_GetElementData")
SEClay_ElementData SEClay_GetElementData(SEClay_ElementId id){
    SEClay_LayoutElementHashMapItem * item = SEClay__GetHashMapItem(id.id);
    if(item == &SEClay_LayoutElementHashMapItem_DEFAULT) {
        return SEClay_ElementData SECLAY__DEFAULT_STRUCT;
    }

    return SEClay_ElementData{
        .boundingBox = item->boundingBox,
        .found = true
    };
}

SECLAY_WASM_EXPORT("SEClay_SetDebugModeEnabled")
void SEClay_SetDebugModeEnabled(bool enabled) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->debugModeEnabled = enabled;
}

SECLAY_WASM_EXPORT("SEClay_IsDebugModeEnabled")
bool SEClay_IsDebugModeEnabled(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    return context->debugModeEnabled;
}

SECLAY_WASM_EXPORT("SEClay_SetCullingEnabled")
void SEClay_SetCullingEnabled(bool enabled) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->disableCulling = !enabled;
}

SECLAY_WASM_EXPORT("SEClay_SetExternalScrollHandlingEnabled")
void SEClay_SetExternalScrollHandlingEnabled(bool enabled) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->externalScrollHandlingEnabled = enabled;
}

SECLAY_WASM_EXPORT("SEClay_GetMaxElementCount")
int32_t SEClay_GetMaxElementCount(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    return context->maxElementCount;
}

SECLAY_WASM_EXPORT("SEClay_SetMaxElementCount")
void SEClay_SetMaxElementCount(int32_t maxElementCount) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    maxElementCount = SEClay__ClampMaxElementCount(maxElementCount);
    if (context) {
        // HARDENING: every array was already carved out of the arena for the old count; changing it afterwards made the
        // next frame's per-frame allocations (and every capacity check) disagree with the persistent ones. Re-create the
        // context (SEClay_Initialize) to resize.
        if (context->arenaResetOffset != 0) {
            SECLAY__REPORT(SEClay_ErrorType::InvalidUsage, SEClay_ErrorDetail::SettingIgnored,
                         "SEClay_SetMaxElementCount was called on an initialized context and was ignored; set it before SEClay_Initialize(), or use a growable context, which resizes itself.",
                         maxElementCount, context->maxElementCount);
            return;
        }
        context->maxElementCount = maxElementCount;
    } else {
        SEClay__defaultMaxElementCount = maxElementCount; // TODO: Fix this
        SEClay__defaultMaxMeasureTextWordCacheCount = SEClay__ClampMaxMeasureTextWordCount(maxElementCount * 2);
    }
}

SECLAY_WASM_EXPORT("SEClay_GetMaxMeasureTextCacheWordCount")
int32_t SEClay_GetMaxMeasureTextCacheWordCount(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    return context->maxMeasureTextCacheWordCount;
}

SECLAY_WASM_EXPORT("SEClay_SetMaxMeasureTextCacheWordCount")
void SEClay_SetMaxMeasureTextCacheWordCount(int32_t maxMeasureTextCacheWordCount) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    maxMeasureTextCacheWordCount = SEClay__ClampMaxMeasureTextWordCount(maxMeasureTextCacheWordCount);
    if (context) {
        if (context->arenaResetOffset != 0) {
            SECLAY__REPORT(SEClay_ErrorType::InvalidUsage, SEClay_ErrorDetail::SettingIgnored,
                         "SEClay_SetMaxMeasureTextCacheWordCount was called on an initialized context and was ignored; set it before SEClay_Initialize(), or use a growable context, which resizes itself.",
                         maxMeasureTextCacheWordCount, context->maxMeasureTextCacheWordCount);
            return;
        }
        context->maxMeasureTextCacheWordCount = maxMeasureTextCacheWordCount;
    } else {
        SEClay__defaultMaxMeasureTextWordCacheCount = maxMeasureTextCacheWordCount; // TODO: Fix this
    }
}

SECLAY_WASM_EXPORT("SEClay_ReleaseContextResources")
void SEClay_ReleaseContextResources(SEClay_Context* context) {
    if (context == NULL) {
        return;
    }
    // scrollContainerDatas may have outgrown its arena slice (SEClay__GrowArrayStorage) and now live on the heap; the
    // arena owner cannot free that. Same "is it outside the arena" test the grow path uses.
    const char *arenaBegin = context->internalArena.memory;
    const char *arenaEnd = context->internalArena.memory + context->internalArena.capacity;
    const char *scrollData = (const char *)context->scrollContainerDatas.internalArray;
    if (scrollData != NULL && !(arenaBegin != NULL && scrollData >= arenaBegin && scrollData < arenaEnd)) {
        free(context->scrollContainerDatas.internalArray);
    }
    context->scrollContainerDatas.internalArray = NULL;
    context->scrollContainerDatas.capacity = 0;
    context->scrollContainerDatas.length = 0;
    free(context->scrollIndex);
    context->scrollIndex = NULL;
    context->scrollIndexMask = 0;
    context->scrollIndexDirty = false;
    if (SEClay_GetCurrentContext() == context) {
        SEClay_SetCurrentContext(NULL);
    }
}

SECLAY_WASM_EXPORT("SEClay_ResetMeasureTextCache")
void SEClay_ResetMeasureTextCache(void) {
    SEClay_Context* context = SEClay_GetCurrentContext();
    context->measureTextHashMapInternal.length = 0;
    context->measureTextHashMapInternalFreeList.length = 0;
    context->measureTextHashMap.length = 0;
    context->measuredWords.length = 0;
    context->measuredWordsFreeList.length = 0;
    
    for (int32_t i = 0; i < context->measureTextHashMap.capacity; ++i) {
        context->measureTextHashMap.internalArray[i] = 0;
    }
    context->measureTextHashMapInternal.length = 1; // Reserve the 0 value to mean "no next element"
}

#endif // SECLAY_IMPLEMENTATION

/*
LICENSE
zlib/libpng license

Copyright (c) 2024 Nic Barker

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software in a
    product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not
    be misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
*/
