#include <Foundation/Color.hpp>

#include <cmath>
#include <iostream>

namespace {

    using namespace SFT::Foundation::Color;

    bool check(const char *space, const Linear &in, const Linear &out, double tolerance) {
        const double error = std::max({std::abs(in.r - out.r), std::abs(in.g - out.g), std::abs(in.b - out.b)});
        if (error > tolerance) {
            std::cerr << space << " round trip error " << error << " for (" << in.r << ", " << in.g << ", " << in.b << ") -> (" << out.r << ", "
                      << out.g << ", " << out.b << ")\n";
            return false;
        }
        return true;
    }

    template <class Space>
    bool round_trips(const char *space, const Linear &in, double tolerance = 1.0e-5) {
        return check(space, in, Space::from_linear(in).to_linear(), tolerance);
    }

} // namespace

int main() {
    bool ok = true;
    const Linear samples[] = {
        Srgb{0.8, 0.35, 0.1, 1.0}.to_linear(), Srgb{0.2, 0.6, 0.9, 1.0}.to_linear(), Srgb{0.5, 0.5, 0.5, 1.0}.to_linear(),
        Srgb{0.05, 0.9, 0.3, 1.0}.to_linear(), Linear{1.0, 1.0, 1.0, 1.0},            Linear{0.0, 0.0, 0.0, 1.0},
    };
    for (const Linear &c : samples) {
        ok &= round_trips<Srgb>("Srgb", c);
        ok &= round_trips<Xyz>("Xyz", c);
        ok &= round_trips<AdobeRgb>("AdobeRgb", c);
        ok &= round_trips<DisplayP3>("DisplayP3", c);
        ok &= round_trips<Rec2020>("Rec2020", c);
        ok &= round_trips<Hsl>("Hsl", c);
        ok &= round_trips<Hsv>("Hsv", c);
        ok &= round_trips<Hwb>("Hwb", c);
        ok &= round_trips<Lab>("Lab", c);
        ok &= round_trips<Lch>("Lch", c);
        ok &= round_trips<Luv>("Luv", c);
        ok &= round_trips<Oklab>("Oklab", c);
        ok &= round_trips<Oklch>("Oklch", c);
    }

    // An sRGB colour is inside the P3 gamut, so it must come out with a *smaller* red than in sRGB.
    const DisplayP3 p3 = DisplayP3::from_linear(Srgb{0.8, 0.35, 0.1, 1.0}.to_linear());
    if (std::abs(p3.r - 0.745) > 0.005 || std::abs(p3.g - 0.377) > 0.005 || std::abs(p3.b - 0.178) > 0.005) {
        std::cerr << "DisplayP3 of sRGB (0.8, 0.35, 0.1) is (" << p3.r << ", " << p3.g << ", " << p3.b << ")\n";
        ok = false;
    }
    return ok ? 0 : 1;
}
