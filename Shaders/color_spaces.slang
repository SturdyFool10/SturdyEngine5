// CSS colour spaces on the GPU: the same conversions and the same interpolation as Renderer/src/Renderer/UI/UiColor.hpp (CSS Color Module
// Level 4), so a shader and the CPU agree on what a colour in `oklch` or `display-p3` is and how two of them mix. Import it:
//
//     import color_spaces;
//     float4 c = cssMix(a, b, t, CSS_SRGB_LINEAR, CSS_OKLAB, CSS_HUE_SHORTER, CSS_SRGB_LINEAR);
//
// The space ids are the values of `UI::ColorSpace`, and the hue methods those of `UI::HueInterpolation`; keep them in step.
// Component conventions are the CSS reference ranges: hsl/hwb S, L, W, B in 0..100, hue in degrees, lab L 0..100, oklab L 0..1.
// Nothing is clamped; only the write to a render target clips.

static const uint CSS_SRGB = 0u;
static const uint CSS_SRGB_LINEAR = 1u;
static const uint CSS_DISPLAY_P3 = 2u;
static const uint CSS_DISPLAY_P3_LINEAR = 3u;
static const uint CSS_A98_RGB = 4u;
static const uint CSS_PROPHOTO_RGB = 5u;
static const uint CSS_REC2020 = 6u;
static const uint CSS_XYZ_D50 = 7u;
static const uint CSS_XYZ_D65 = 8u;
static const uint CSS_LAB = 9u;
static const uint CSS_OKLAB = 10u;
static const uint CSS_HSL = 11u;
static const uint CSS_HWB = 12u;
static const uint CSS_LCH = 13u;
static const uint CSS_OKLCH = 14u;

static const uint CSS_HUE_SHORTER = 0u;
static const uint CSS_HUE_LONGER = 1u;
static const uint CSS_HUE_INCREASING = 2u;
static const uint CSS_HUE_DECREASING = 3u;

static const float CSS_PI = 3.14159265358979;

// Matrices exactly as CSS Color 4 section 19 writes them, row by row (mul(M, v) is M times a column vector).
static const float3x3 CSS_LIN_SRGB_TO_XYZ = float3x3(
    506752.0 / 1228815.0, 87881.0 / 245763.0, 12673.0 / 70218.0,
    87098.0 / 409605.0, 175762.0 / 245763.0, 12673.0 / 175545.0,
    7918.0 / 409605.0, 87881.0 / 737289.0, 1001167.0 / 1053270.0);
static const float3x3 CSS_XYZ_TO_LIN_SRGB = float3x3(
    12831.0 / 3959.0, -329.0 / 214.0, -1974.0 / 3959.0,
    -851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0,
    705.0 / 12673.0, -2585.0 / 12673.0, 705.0 / 667.0);
static const float3x3 CSS_LIN_P3_TO_XYZ = float3x3(
    608311.0 / 1250200.0, 189793.0 / 714400.0, 198249.0 / 1000160.0,
    35783.0 / 156275.0, 247089.0 / 357200.0, 198249.0 / 2500400.0,
    0.0, 32229.0 / 714400.0, 5220557.0 / 5000800.0);
static const float3x3 CSS_XYZ_TO_LIN_P3 = float3x3(
    446124.0 / 178915.0, -333277.0 / 357830.0, -72051.0 / 178915.0,
    -14852.0 / 17905.0, 63121.0 / 35810.0, 423.0 / 17905.0,
    11844.0 / 330415.0, -50337.0 / 660830.0, 316169.0 / 330415.0);
static const float3x3 CSS_LIN_A98_TO_XYZ = float3x3(
    573536.0 / 994567.0, 263643.0 / 1420810.0, 187206.0 / 994567.0,
    591459.0 / 1989134.0, 6239551.0 / 9945670.0, 374412.0 / 4972835.0,
    53769.0 / 1989134.0, 351524.0 / 4972835.0, 4929758.0 / 4972835.0);
static const float3x3 CSS_XYZ_TO_LIN_A98 = float3x3(
    1829569.0 / 896150.0, -506331.0 / 896150.0, -308931.0 / 896150.0,
    -851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0,
    16779.0 / 1248040.0, -147721.0 / 1248040.0, 1266979.0 / 1248040.0);
static const float3x3 CSS_LIN_PROPHOTO_TO_XYZ = float3x3(
    0.79776664490064230, 0.13518129740053308, 0.03134773412839220,
    0.28807482881940130, 0.71183523424187300, 0.00008993693872564,
    0.0, 0.0, 0.82510460251046020);
static const float3x3 CSS_XYZ_TO_LIN_PROPHOTO = float3x3(
    1.34578688164715830, -0.25557208737979464, -0.05110186497554526,
    -0.54463070512490190, 1.50824774284514680, 0.02052744743642139,
    0.0, 0.0, 1.21196754563894520);
static const float3x3 CSS_LIN_2020_TO_XYZ = float3x3(
    63426534.0 / 99577255.0, 20160776.0 / 139408157.0, 47086771.0 / 278816314.0,
    26158966.0 / 99577255.0, 472592308.0 / 697040785.0, 8267143.0 / 139408157.0,
    0.0, 19567812.0 / 697040785.0, 295819943.0 / 278816314.0);
static const float3x3 CSS_XYZ_TO_LIN_2020 = float3x3(
    30757411.0 / 17917100.0, -6372589.0 / 17917100.0, -4539589.0 / 17917100.0,
    -19765991.0 / 29648200.0, 47925759.0 / 29648200.0, 467509.0 / 29648200.0,
    792561.0 / 44930125.0, -1921689.0 / 44930125.0, 42328811.0 / 44930125.0);
static const float3x3 CSS_D65_TO_D50 = float3x3(
    1.0479297925449969, 0.022946870601609652, -0.05019226628920524,
    0.02962780877005599, 0.9904344267538799, -0.017073799063418826,
    -0.009243040646204504, 0.015055191490298152, 0.7518742814281371);
static const float3x3 CSS_D50_TO_D65 = float3x3(
    0.955473421488075, -0.02309845494876471, 0.06325924320057072,
    -0.0283697093338637, 1.0099953980813041, 0.021041441191917323,
    0.012314014864481998, -0.020507649298898964, 1.330365926242124);
static const float3x3 CSS_XYZ_TO_LMS = float3x3(
    0.8190224379967030, 0.3619062600528904, -0.1288737815209879,
    0.0329836539323885, 0.9292868615863434, 0.0361446663506424,
    0.0481771893596242, 0.2642395317527308, 0.6335478284694309);
static const float3x3 CSS_LMS_TO_OKLAB = float3x3(
    0.2104542683093140, 0.7936177747023054, -0.0040720430116193,
    1.9779985324311684, -2.4285922420485799, 0.4505937096174110,
    0.0259040424655478, 0.7827717124575296, -0.8086757549230774);
static const float3x3 CSS_LMS_TO_XYZ = float3x3(
    1.2268798758459243, -0.5578149944602171, 0.2813910456659647,
    -0.0405757452148008, 1.1122868032803170, -0.0717110580655164,
    -0.0763729366746601, -0.4214933324022432, 1.5869240198367816);
static const float3x3 CSS_OKLAB_TO_LMS = float3x3(
    1.0000000000000000, 0.3963377773761749, 0.2158037573099136,
    1.0000000000000000, -0.1055613458156586, -0.0638541728258133,
    1.0000000000000000, -0.0894841775298119, -1.2914855480194092);

static const float3 CSS_D50 = float3(0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585);
static const float CSS_LAB_EPSILON = 216.0 / 24389.0;
static const float CSS_LAB_KAPPA = 24389.0 / 27.0;

float cssSign(float v) { return v < 0.0 ? -1.0 : 1.0; }

float cssDecodeSrgb(float v) {
    float a = abs(v);
    return a <= 0.04045 ? v / 12.92 : cssSign(v) * pow((a + 0.055) / 1.055, 2.4);
}
float cssEncodeSrgb(float v) {
    float a = abs(v);
    return a > 0.0031308 ? cssSign(v) * (1.055 * pow(a, 1.0 / 2.4) - 0.055) : 12.92 * v;
}
float cssPower(float v, float gamma) { return cssSign(v) * pow(abs(v), gamma); }
float cssDecodeProphoto(float v) {
    float a = abs(v);
    return a <= 16.0 / 512.0 ? v / 16.0 : cssSign(v) * pow(a, 1.8);
}
float cssEncodeProphoto(float v) {
    float a = abs(v);
    return a >= 1.0 / 512.0 ? cssSign(v) * pow(a, 1.0 / 1.8) : 16.0 * v;
}

float3 cssDecodeSrgb3(float3 v) { return float3(cssDecodeSrgb(v.x), cssDecodeSrgb(v.y), cssDecodeSrgb(v.z)); }
float3 cssEncodeSrgb3(float3 v) { return float3(cssEncodeSrgb(v.x), cssEncodeSrgb(v.y), cssEncodeSrgb(v.z)); }
float3 cssPower3(float3 v, float gamma) { return float3(cssPower(v.x, gamma), cssPower(v.y, gamma), cssPower(v.z, gamma)); }
float3 cssCbrt3(float3 v) { return float3(cssSign(v.x) * pow(abs(v.x), 1.0 / 3.0), cssSign(v.y) * pow(abs(v.y), 1.0 / 3.0), cssSign(v.z) * pow(abs(v.z), 1.0 / 3.0)); }

float3 cssHslToSrgb(float3 hsl) {
    float hue = hsl.x - 360.0 * floor(hsl.x / 360.0);
    float sat = hsl.y / 100.0;
    float light = hsl.z / 100.0;
    float a = sat * min(light, 1.0 - light);
    float3 n = float3(0.0, 8.0, 4.0);
    float3 k = n + hue / 30.0;
    k = k - 12.0 * floor(k / 12.0);
    float3 channel = light - a * max(float3(-1.0, -1.0, -1.0), min(min(k - 3.0, 9.0 - k), float3(1.0, 1.0, 1.0)));
    return channel;
}

float3 cssSrgbToHsl(float3 rgb) {
    float mx = max(rgb.x, max(rgb.y, rgb.z));
    float mn = min(rgb.x, min(rgb.y, rgb.z));
    float hue = 0.0;
    float sat = 0.0;
    float light = (mn + mx) * 0.5;
    float d = mx - mn;
    float epsilon = 1.0e-5;
    if (d != 0.0) {
        sat = (abs(light) <= epsilon || abs(1.0 - light) < epsilon) ? 0.0 : (mx - light) / min(light, 1.0 - light);
        if (mx == rgb.x) {
            hue = (rgb.y - rgb.z) / d + (rgb.y < rgb.z ? 6.0 : 0.0);
        } else if (mx == rgb.y) {
            hue = (rgb.z - rgb.x) / d + 2.0;
        } else {
            hue = (rgb.x - rgb.y) / d + 4.0;
        }
        hue *= 60.0;
    }
    if (sat < 0.0) {
        hue += 180.0;
        sat = abs(sat);
    }
    if (hue >= 360.0) {
        hue -= 360.0;
    }
    if (sat <= epsilon) {
        sat = 0.0; // a powerless hue stays 0 here; cssHuePowerless() tells the mixer it carries no information
    }
    return float3(hue, sat * 100.0, light * 100.0);
}

float3 cssHwbToSrgb(float3 hwb) {
    float w = hwb.y / 100.0;
    float b = hwb.z / 100.0;
    if (w + b >= 1.0) {
        float grey = w / (w + b);
        return float3(grey, grey, grey);
    }
    return cssHslToSrgb(float3(hwb.x, 100.0, 50.0)) * (1.0 - w - b) + float3(w, w, w);
}

float3 cssSrgbToHwb(float3 rgb) {
    float3 hsl = cssSrgbToHsl(rgb);
    return float3(hsl.x, min(rgb.x, min(rgb.y, rgb.z)) * 100.0, (1.0 - max(rgb.x, max(rgb.y, rgb.z))) * 100.0);
}

float3 cssPolarToRect(float3 lch) {
    float r = lch.z * CSS_PI / 180.0;
    return float3(lch.x, lch.y * cos(r), lch.y * sin(r));
}

float3 cssRectToPolar(float3 lab) {
    float chroma = sqrt(lab.y * lab.y + lab.z * lab.z);
    float hue = atan2(lab.z, lab.y) * 180.0 / CSS_PI;
    if (hue < 0.0) {
        hue += 360.0;
    }
    return float3(lab.x, chroma, hue);
}

float3 cssXyzD50ToLab(float3 xyz) {
    float3 scaled = xyz / CSS_D50;
    float3 f;
    f.x = scaled.x > CSS_LAB_EPSILON ? pow(scaled.x, 1.0 / 3.0) : (CSS_LAB_KAPPA * scaled.x + 16.0) / 116.0;
    f.y = scaled.y > CSS_LAB_EPSILON ? pow(scaled.y, 1.0 / 3.0) : (CSS_LAB_KAPPA * scaled.y + 16.0) / 116.0;
    f.z = scaled.z > CSS_LAB_EPSILON ? pow(scaled.z, 1.0 / 3.0) : (CSS_LAB_KAPPA * scaled.z + 16.0) / 116.0;
    return float3(116.0 * f.y - 16.0, 500.0 * (f.x - f.y), 200.0 * (f.y - f.z));
}

float3 cssLabToXyzD50(float3 lab) {
    float l116 = lab.x / 116.0;
    float a500 = lab.y / 500.0;
    float b200 = lab.z / 200.0;
    float fy = l116 + 16.0 / 116.0;
    float fx = fy + a500;
    float fz = fy - b200;
    float x = fx * fx * fx > CSS_LAB_EPSILON ? fx * fx * fx : (l116 + a500) * (116.0 / CSS_LAB_KAPPA);
    float y = lab.x > CSS_LAB_KAPPA * CSS_LAB_EPSILON ? pow((lab.x + 16.0) / 116.0, 3.0) : lab.x / CSS_LAB_KAPPA;
    float z = fz * fz * fz > CSS_LAB_EPSILON ? fz * fz * fz : (l116 - b200) * (116.0 / CSS_LAB_KAPPA);
    return float3(x, y, z) * CSS_D50;
}

float3 cssToXyzD65(float3 c, uint space) {
    switch (space) {
        case CSS_SRGB: return mul(CSS_LIN_SRGB_TO_XYZ, cssDecodeSrgb3(c));
        case CSS_SRGB_LINEAR: return mul(CSS_LIN_SRGB_TO_XYZ, c);
        case CSS_DISPLAY_P3: return mul(CSS_LIN_P3_TO_XYZ, cssDecodeSrgb3(c));
        case CSS_DISPLAY_P3_LINEAR: return mul(CSS_LIN_P3_TO_XYZ, c);
        case CSS_A98_RGB: return mul(CSS_LIN_A98_TO_XYZ, cssPower3(c, 563.0 / 256.0));
        case CSS_PROPHOTO_RGB: {
            float3 lin = float3(cssDecodeProphoto(c.x), cssDecodeProphoto(c.y), cssDecodeProphoto(c.z));
            return mul(CSS_D50_TO_D65, mul(CSS_LIN_PROPHOTO_TO_XYZ, lin));
        }
        case CSS_REC2020: return mul(CSS_LIN_2020_TO_XYZ, cssPower3(c, 2.4));
        case CSS_XYZ_D50: return mul(CSS_D50_TO_D65, c);
        case CSS_XYZ_D65: return c;
        case CSS_LAB: return mul(CSS_D50_TO_D65, cssLabToXyzD50(c));
        case CSS_OKLAB: {
            float3 lms = mul(CSS_OKLAB_TO_LMS, c);
            return mul(CSS_LMS_TO_XYZ, lms * lms * lms);
        }
        case CSS_HSL: return mul(CSS_LIN_SRGB_TO_XYZ, cssDecodeSrgb3(cssHslToSrgb(c)));
        case CSS_HWB: return mul(CSS_LIN_SRGB_TO_XYZ, cssDecodeSrgb3(cssHwbToSrgb(c)));
        case CSS_LCH: return mul(CSS_D50_TO_D65, cssLabToXyzD50(cssPolarToRect(c)));
        default: {
            float3 lms = mul(CSS_OKLAB_TO_LMS, cssPolarToRect(c)); // CSS_OKLCH
            return mul(CSS_LMS_TO_XYZ, lms * lms * lms);
        }
    }
}

float3 cssFromXyzD65(float3 xyz, uint space) {
    switch (space) {
        case CSS_SRGB: return cssEncodeSrgb3(mul(CSS_XYZ_TO_LIN_SRGB, xyz));
        case CSS_SRGB_LINEAR: return mul(CSS_XYZ_TO_LIN_SRGB, xyz);
        case CSS_DISPLAY_P3: return cssEncodeSrgb3(mul(CSS_XYZ_TO_LIN_P3, xyz));
        case CSS_DISPLAY_P3_LINEAR: return mul(CSS_XYZ_TO_LIN_P3, xyz);
        case CSS_A98_RGB: return cssPower3(mul(CSS_XYZ_TO_LIN_A98, xyz), 256.0 / 563.0);
        case CSS_PROPHOTO_RGB: {
            float3 lin = mul(CSS_XYZ_TO_LIN_PROPHOTO, mul(CSS_D65_TO_D50, xyz));
            return float3(cssEncodeProphoto(lin.x), cssEncodeProphoto(lin.y), cssEncodeProphoto(lin.z));
        }
        case CSS_REC2020: return cssPower3(mul(CSS_XYZ_TO_LIN_2020, xyz), 1.0 / 2.4);
        case CSS_XYZ_D50: return mul(CSS_D65_TO_D50, xyz);
        case CSS_XYZ_D65: return xyz;
        case CSS_LAB: return cssXyzD50ToLab(mul(CSS_D65_TO_D50, xyz));
        case CSS_OKLAB: return mul(CSS_LMS_TO_OKLAB, cssCbrt3(mul(CSS_XYZ_TO_LMS, xyz)));
        case CSS_HSL: return cssSrgbToHsl(cssEncodeSrgb3(mul(CSS_XYZ_TO_LIN_SRGB, xyz)));
        case CSS_HWB: return cssSrgbToHwb(cssEncodeSrgb3(mul(CSS_XYZ_TO_LIN_SRGB, xyz)));
        case CSS_LCH: return cssRectToPolar(cssXyzD50ToLab(mul(CSS_D65_TO_D50, xyz)));
        default: return cssRectToPolar(mul(CSS_LMS_TO_OKLAB, cssCbrt3(mul(CSS_XYZ_TO_LMS, xyz)))); // CSS_OKLCH
    }
}

float3 cssConvert(float3 c, uint fromSpace, uint toSpace) {
    if (fromSpace == toSpace) {
        return c;
    }
    return cssFromXyzD65(cssToXyzD65(c, fromSpace), toSpace);
}

bool cssIsPolar(uint space) { return space >= CSS_HSL; }

// Which component of a polar space is the hue: 0 for hsl and hwb, 2 for lch and oklch.
uint cssHueComponent(uint space) { return (space == CSS_LCH || space == CSS_OKLCH) ? 2u : 0u; }

// A polar colour's hue is powerless (CSS: "missing") when its colourfulness is negligible.
bool cssHuePowerless(float3 c, uint space) {
    switch (space) {
        case CSS_HSL: return c.y <= 0.001;
        case CSS_HWB: return c.y + c.z >= 99.999;
        case CSS_LCH: return c.y <= 0.0015;
        case CSS_OKLCH: return c.y <= 0.000004;
        default: return false;
    }
}

float cssWrapDegrees(float h) { return h - 360.0 * floor(h / 360.0); }

// CSS interpolation of `a` and `b` (straight alpha, components in `sourceSpace`) in `space` with `hueMethod`, `t` the weight of `b`;
// the result is in `resultSpace`. Premultiplied, with the hue fix-up of CSS Color 4 section 13.6 and a powerless hue taking the other
// colour's.
float4 cssMix(float4 a, float4 b, float t, uint sourceSpace, uint space, uint hueMethod, uint resultSpace) {
    float3 ca = cssConvert(a.xyz, sourceSpace, space);
    float3 cb = cssConvert(b.xyz, sourceSpace, space);
    bool polar = cssIsPolar(space);
    uint hueIndex = cssHueComponent(space);
    if (polar) {
        bool missingA = cssHuePowerless(ca, space);
        bool missingB = cssHuePowerless(cb, space);
        float ha = ca[hueIndex];
        float hb = cb[hueIndex];
        if (missingA && !missingB) {
            ha = hb;
        } else if (missingB && !missingA) {
            hb = ha;
        }
        float h1 = cssWrapDegrees(ha);
        float h2 = cssWrapDegrees(hb);
        float d = h2 - h1;
        if (hueMethod == CSS_HUE_SHORTER) {
            if (d > 180.0) {
                h1 += 360.0;
            } else if (d < -180.0) {
                h2 += 360.0;
            }
        } else if (hueMethod == CSS_HUE_LONGER) {
            if (d > 0.0 && d < 180.0) {
                h1 += 360.0;
            } else if (d > -180.0 && d <= 0.0) {
                h2 += 360.0;
            }
        } else if (hueMethod == CSS_HUE_INCREASING) {
            if (h2 < h1) {
                h2 += 360.0;
            }
        } else {
            if (h1 < h2) {
                h1 += 360.0;
            }
        }
        ca[hueIndex] = h1;
        cb[hueIndex] = h2;
    }

    float3 pa = ca * a.w;
    float3 pb = cb * b.w;
    if (polar) {
        pa[hueIndex] = ca[hueIndex];
        pb[hueIndex] = cb[hueIndex];
    }
    float3 mixed = lerp(pa, pb, t);
    float alpha = lerp(a.w, b.w, t);
    if (alpha != 0.0) {
        float hue = mixed[hueIndex];
        mixed = mixed / alpha;
        if (polar) {
            mixed[hueIndex] = hue;
        }
    }
    if (polar) {
        mixed[hueIndex] = cssWrapDegrees(mixed[hueIndex]);
    }
    return float4(cssConvert(mixed, space, resultSpace), alpha);
}
