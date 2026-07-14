// `Sturdy.Text` — CPU-side text shaping, glyph outline extraction, and SDF/MSDF rasterization.
//
// Wraps HarfBuzz for shaping ((font, UTF-8 text, script/language) -> positioned glyph indices) and
// glyph outline extraction via HarfBuzz's hb-draw API — no FreeType dependency; HarfBuzz decodes
// glyf/CFF/CFF2 outlines natively. A later partition turns an extracted outline into a signed
// distance field (single-channel SDF or multi-channel MSDF, via msdfgen) ready for GPU atlasing.
// `:Error` is the exception-free TextResult/TextExpected shape shared by every partition.
//
// Deliberately depends on nothing but Foundation, Async, and GLM — no RHI, no Core, no GPU
// dependency at all. The GPU-facing half (atlas, instancing, shaders) lives in Renderer, which
// depends on this package.
#pragma once

#include <Foundation/Foundation.hpp>

#include "Error.hpp"
#include "Font.hpp"
#include "Shape.hpp"
#include "Outline.hpp"
#include "Raster.hpp"
#include "FontDatabase.hpp"
#include "ColorGlyph.hpp"
#include "FontFallback.hpp"
#include "Features.hpp"
