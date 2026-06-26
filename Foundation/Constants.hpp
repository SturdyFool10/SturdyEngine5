#pragma once

#include "Types.hpp"
#include "Wide.hpp"

#include <concepts>

// Typed mathematical constants for engine math, graphics, simulation, procedural art, and
// general-purpose numeric code.
//
// Calls without an explicit type preserve the old API and return f64:
//     auto p64 = SFT::Foundation::pi();
//
// Wide callers can request constants at the precision of the type they are computing with:
//     auto p128 = SFT::Foundation::pi<SFT::f128>();
//     auto p256 = SFT::Foundation::pi<SFT::f256>();
//
// Each constant is stored as a four-component binary64 expansion (c0..c3). A request selects how
// many components are used: f32/f64 take c0 (narrowed for f32), f128 takes c0..c1 (double-double),
// f256 takes c0..c3 (quad-double). Hex float literals are intentional: they preserve the exact
// component bits and avoid decimal-parser differences across compilers.
//
// Precision budget (storage bits -> decimal significant figures, + 3 guard digits for generation):
//     f128 double-double: 2 x 53 = 106 bits -> 31.9 -> 32 sig figs (generate to 35, then round).
//     f256 quad-double:   4 x 53 = 212 bits -> 63.8 -> 64 sig figs (generate to 67, then round).
// The stored components are correctly-rounded expansions at that full width; constants_precision_
// smoke_test() below locks the guarantee in at compile time by cross-checking the algebraic ones.

using std::same_as;

namespace SFT::Foundation {

    namespace Detail {

        template <class T>
        concept ConstantScalar = same_as<T, f32> || same_as<T, f64> || same_as<T, f128> || same_as<T, f256>;

        template <ConstantScalar T>
        [[nodiscard]] constexpr T constant(f64 c0, f64 c1 = 0.0, f64 c2 = 0.0, f64 c3 = 0.0) noexcept {
            if constexpr (same_as<T, f32>) {
                return static_cast<f32>(c0);
            } else if constexpr (same_as<T, f64>) {
                return c0;
            } else if constexpr (same_as<T, f128>) {
                return f128(c0, c1);
            } else {
                return f256(c0, c1, c2, c3);
            }
        }

    } // namespace Detail

    // --- Basic values -------------------------------------------------------------------------
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T zero() noexcept {
        return Detail::constant<T>(0x0.0p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T one() noexcept {
        return Detail::constant<T>(0x1.0000000000000p+0, 0x0.0p+0, 0x0.0p+0, 0x0.0p+0);
    }

    // --- Circle / angles ----------------------------------------------------------------------
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T pi() noexcept {
        return Detail::constant<T>(0x1.921fb54442d18p+1, 0x1.1a62633145c07p-53, -0x1.f1976b7ed8fbcp-109, 0x1.4cf98e804177dp-163);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T tau() noexcept {
        return Detail::constant<T>(0x1.921fb54442d18p+2, 0x1.1a62633145c07p-52, -0x1.f1976b7ed8fbcp-108, 0x1.4cf98e804177dp-162);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T two_pi() noexcept {
        return tau<T>();
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T three_pi() noexcept {
        return Detail::constant<T>(0x1.2d97c7f3321d2p+3, 0x1.a79394c9e8a0ap-52, 0x1.456737b06ea1ap-106, -0x1.83226a8fe7731p-160);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T four_pi() noexcept {
        return Detail::constant<T>(0x1.921fb54442d18p+3, 0x1.1a62633145c07p-51, -0x1.f1976b7ed8fbcp-107, 0x1.4cf98e804177dp-161);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T half_pi() noexcept {
        return Detail::constant<T>(0x1.921fb54442d18p+0, 0x1.1a62633145c07p-54, -0x1.f1976b7ed8fbcp-110, 0x1.4cf98e804177dp-164);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T third_pi() noexcept {
        return Detail::constant<T>(0x1.0c152382d7366p+0, -0x1.ee6913347c2a6p-54, -0x1.4bba47a9e5fd2p-110, -0x1.ccaef65529b02p-164);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T quarter_pi() noexcept {
        return Detail::constant<T>(0x1.921fb54442d18p-1, 0x1.1a62633145c07p-55, -0x1.f1976b7ed8fbcp-111, 0x1.4cf98e804177dp-165);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T inv_pi() noexcept {
        return Detail::constant<T>(0x1.45f306dc9c883p-2, -0x1.6b01ec5417056p-56, -0x1.6447e493ad4cep-110, 0x1.e21c820ff28b2p-164);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T inv_tau() noexcept {
        return Detail::constant<T>(0x1.45f306dc9c883p-3, -0x1.6b01ec5417056p-57, -0x1.6447e493ad4cep-111, 0x1.e21c820ff28b2p-165);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T two_over_pi() noexcept {
        return Detail::constant<T>(0x1.45f306dc9c883p-1, -0x1.6b01ec5417056p-55, -0x1.6447e493ad4cep-109, 0x1.e21c820ff28b2p-163);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T four_over_pi() noexcept {
        return Detail::constant<T>(0x1.45f306dc9c883p+0, -0x1.6b01ec5417056p-54, -0x1.6447e493ad4cep-108, 0x1.e21c820ff28b2p-162);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T pi_squared() noexcept {
        return Detail::constant<T>(0x1.3bd3cc9be45dep+3, 0x1.692b71366cc04p-51, 0x1.8358e10acd480p-105, -0x1.f2f5dd7997ddfp-160);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T pi_cubed() noexcept {
        return Detail::constant<T>(0x1.f019b59389d7cp+4, 0x1.e019558e5380dp-52, 0x1.b61ccd40f1292p-106, 0x1.6cc40e155075cp-160);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T sqrt_pi() noexcept {
        return Detail::constant<T>(0x1.c5bf891b4ef6bp+0, -0x1.618f13eb7ca89p-54, -0x1.b1f0071b7aae4p-110, -0x1.389b5a46bdfe8p-165);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T inv_sqrt_pi() noexcept {
        return Detail::constant<T>(0x1.20dd750429b6dp-1, 0x1.1ae3a914fed80p-57, -0x1.3cbbebf65f145p-112, -0x1.e0c574632f53ep-167);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T sqrt_tau() noexcept {
        return Detail::constant<T>(0x1.40d931ff62706p+1, -0x1.a6a0d6f814637p-53, -0x1.311d073060acep-107, 0x1.6000b50dc2f41p-164);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T sqrt_half_pi() noexcept {
        return Detail::constant<T>(0x1.40d931ff62706p+0, -0x1.a6a0d6f814637p-54, -0x1.311d073060acep-108, 0x1.6000b50dc2f41p-165);
    }

    // Angle conversion factors: radians = angle * *_to_rad, angle = radians * rad_to_*.
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T deg_to_rad() noexcept {
        return Detail::constant<T>(0x1.1df46a2529d39p-6, 0x1.5c1d8becdd291p-62, -0x1.1d937fa428858p-116, 0x1.b5e6b8e502a9bp-173);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T rad_to_deg() noexcept {
        return Detail::constant<T>(0x1.ca5dc1a63c1f8p+5, -0x1.1e7ab456405f9p-49, -0x1.b505196fabb41p-103, -0x1.a07e91992ec5fp-161);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T grad_to_rad() noexcept {
        return Detail::constant<T>(0x1.015bf9217271ap-6, -0x1.c9bf81089c7a5p-61, -0x1.1a1bf970456f4p-115, -0x1.80f7d26651734p-169);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T rad_to_grad() noexcept {
        return Detail::constant<T>(0x1.fd4bbab8b494cp+5, 0x1.1199fd79380f2p-50, 0x1.4d3eab6504dfbp-105, 0x1.c5322ce3abe57p-159);
    }

    // --- e and logarithms ---------------------------------------------------------------------
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T e() noexcept {
        return Detail::constant<T>(0x1.5bf0a8b145769p+1, 0x1.4d57ee2b1013ap-53, -0x1.618713a31d3e2p-109, 0x1.c5a6d2b53c26dp-163);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T inv_e() noexcept {
        return Detail::constant<T>(0x1.78b56362cef38p-2, -0x1.ca8a4270fadf5p-57, -0x1.837912b3fd2aap-111, -0x1.52711999fb68cp-165);
    }

    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T log2_e() noexcept {
        return Detail::constant<T>(0x1.71547652b82fep+0, 0x1.777d0ffda0d24p-56, -0x1.60bb8a5442ab9p-110, -0x1.4b52d3ba6d74dp-166);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T log10_e() noexcept {
        return Detail::constant<T>(0x1.bcb7b1526e50ep-2, 0x1.95355baaafad3p-57, 0x1.ee191f71a3012p-112, 0x1.7268808e8fcb5p-167);
    }

    // --- Ratios -------------------------------------------------------------------------------
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T phi() noexcept {
        return Detail::constant<T>(0x1.9e3779b97f4a8p+0, -0x1.f506319fcfd19p-55, 0x1.b906821044ed8p-109, -0x1.8bb1b5c0f272cp-165);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T inv_phi() noexcept {
        return Detail::constant<T>(0x1.3c6ef372fe950p-1, -0x1.f506319fcfd19p-55, 0x1.b906821044ed8p-109, -0x1.8bb1b5c0f272cp-165);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T phi_squared() noexcept {
        return Detail::constant<T>(0x1.4f1bbcdcbfa54p+1, -0x1.f506319fcfd19p-55, 0x1.b906821044ed8p-109, -0x1.8bb1b5c0f272cp-165);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T silver_ratio() noexcept {
        return Detail::constant<T>(0x1.3504f333f9de6p+1, 0x1.21165f626cdd5p-53, 0x1.57d3e3adec175p-108, 0x1.2775099da2f59p-164);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T plastic() noexcept {
        return Detail::constant<T>(0x1.5320b74eca44bp+0, -0x1.29f43bb41df5dp-55, 0x1.334737f8172f2p-112, -0x1.828953cce60e3p-167);
    }

    // --- Named constants ----------------------------------------------------------------------
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T euler_mascheroni() noexcept {
        return Detail::constant<T>(0x1.2788cfc6fb619p-1, -0x1.6cb90701fbfabp-58, -0x1.34a95e3133c51p-112, 0x1.9730064300f7dp-166);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T catalan() noexcept {
        return Detail::constant<T>(0x1.d4f9713e8135dp-1, 0x1.1485608b8df4dp-58, -0x1.2f39c13bc1ec8p-112, 0x1.c2ff8094a263ep-168);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T apery() noexcept {
        return Detail::constant<T>(0x1.33ba004f00621p+0, 0x1.c1b8b8ae2cf35p-55, -0x1.f01c9cfe9049dp-109, -0x1.6d97f445d6c71p-165);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T feigenbaum_delta() noexcept {
        return Detail::constant<T>(0x1.2ad432fc61c97p+2, 0x1.1d1c04ed45ae2p-52, 0x1.90fda022180f1p-106, 0x1.80989caa8f452p-160);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T feigenbaum_alpha() noexcept {
        return Detail::constant<T>(0x1.405f49063806fp+1, 0x1.b832a1bf563fap-53, 0x1.2b4c73d972bb4p-112, -0x1.e64b08de46419p-166);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T khinchin() noexcept {
        return Detail::constant<T>(0x1.57bce423c6d0dp+1, 0x1.de5fbe7c728cdp-53, -0x1.8f31b47f7867ap-114, 0x1.4eefd26c3831ep-171);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T glaisher_kinkelin() noexcept {
        return Detail::constant<T>(0x1.484d24f2fd873p+0, 0x1.313ed56e343dap-56, 0x1.dd5e51da3832bp-110, -0x1.72d404df3d0bcp-167);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T conway() noexcept {
        return Detail::constant<T>(0x1.4db73d6a4fb12p+0, 0x1.55f152aaf3ab1p-57, 0x1.677cfb6e27707p-111, 0x1.2b9d251d89975p-167);
    }
    template <Detail::ConstantScalar T = f64>
    [[nodiscard]] constexpr T omega() noexcept {
        return Detail::constant<T>(0x1.22609af8e9657p-1, 0x1.2f57eed531437p-55, -0x1.eb8f48bc898bdp-111, -0x1.1caefe6d811f9p-165);
    }

    namespace Detail {

        [[nodiscard]] constexpr f256 qd_abs(f256 v) noexcept { return v < f256(0.0) ? -v : v; }

        // Relative agreement to 2^-205 (~61.7 decimal digits): far past f64 (15.9) and f128 (31.9).
        // The reconstructed relationships hold to ~2^-214 (the quad-double arithmetic noise floor,
        // measured), so this leaves ~9 bits of slack against spurious failure while still flagging
        // any realistic transcription error in components c0..c2 and the high-order bits of c3. The
        // lowest ~1-2 bits of a quad-double sit below this self-consistency floor and cannot be
        // cross-checked here; they are pinned by the out-of-band arbitrary-precision generation.
        // Evaluation is consteval, so it always takes the deterministic non-FMA two_prod path and
        // the margin does not vary across compilers. Every reference below is well away from zero.
        [[nodiscard]] constexpr bool qd_close(f256 a, f256 b) noexcept {
            const f256 tol(0x1p-205);
            return qd_abs(a - b) <= tol * qd_abs(b);
        }

        // Compile-time guard on the stored quad-double components: each algebraically-defined
        // constant must equal its definition to within quad-double rounding. This catches a
        // mistyped or transcribed component (the failure mode of hand-edited expansions). Leaf
        // transcendentals with no closed-form relation here (e, euler_mascheroni, catalan, apery,
        // feigenbaum_*, khinchin, glaisher_kinkelin, conway, omega) are generated and verified
        // out-of-band against an arbitrary-precision reference; e is still pinned via inv_e.
        [[nodiscard]] consteval bool constants_precision_smoke_test() noexcept {
            const f256 one_(1.0);

            // Multiples and fractions of pi.
            if (!qd_close(tau<f256>(), pi<f256>() * f256(2.0)))
                return false;
            if (!qd_close(three_pi<f256>(), pi<f256>() * f256(3.0)))
                return false;
            if (!qd_close(four_pi<f256>(), pi<f256>() * f256(4.0)))
                return false;
            if (!qd_close(half_pi<f256>(), pi<f256>() / f256(2.0)))
                return false;
            if (!qd_close(third_pi<f256>(), pi<f256>() / f256(3.0)))
                return false;
            if (!qd_close(quarter_pi<f256>(), pi<f256>() / f256(4.0)))
                return false;

            // Reciprocals.
            if (!qd_close(inv_pi<f256>(), one_ / pi<f256>()))
                return false;
            if (!qd_close(inv_tau<f256>(), one_ / tau<f256>()))
                return false;
            if (!qd_close(two_over_pi<f256>(), f256(2.0) / pi<f256>()))
                return false;
            if (!qd_close(four_over_pi<f256>(), f256(4.0) / pi<f256>()))
                return false;
            if (!qd_close(inv_e<f256>(), one_ / e<f256>()))
                return false;

            // Powers and roots (roots checked by squaring back).
            if (!qd_close(pi_squared<f256>(), pi<f256>() * pi<f256>()))
                return false;
            if (!qd_close(pi_cubed<f256>(), pi<f256>() * pi<f256>() * pi<f256>()))
                return false;
            if (!qd_close(sqrt_pi<f256>() * sqrt_pi<f256>(), pi<f256>()))
                return false;
            if (!qd_close(sqrt_tau<f256>() * sqrt_tau<f256>(), tau<f256>()))
                return false;
            if (!qd_close(sqrt_half_pi<f256>() * sqrt_half_pi<f256>(), half_pi<f256>()))
                return false;
            if (!qd_close(inv_sqrt_pi<f256>(), one_ / sqrt_pi<f256>()))
                return false;

            // Angle conversion factors.
            if (!qd_close(deg_to_rad<f256>(), pi<f256>() / f256(180.0)))
                return false;
            if (!qd_close(rad_to_deg<f256>(), f256(180.0) / pi<f256>()))
                return false;
            if (!qd_close(grad_to_rad<f256>(), pi<f256>() / f256(200.0)))
                return false;
            if (!qd_close(rad_to_grad<f256>(), f256(200.0) / pi<f256>()))
                return false;

            // Golden ratio identities: phi^2 = phi + 1, 1/phi = phi - 1.
            if (!qd_close(phi_squared<f256>(), phi<f256>() * phi<f256>()))
                return false;
            if (!qd_close(phi_squared<f256>(), phi<f256>() + one_))
                return false;
            if (!qd_close(inv_phi<f256>(), one_ / phi<f256>()))
                return false;
            if (!qd_close(inv_phi<f256>(), phi<f256>() - one_))
                return false;

            // Silver ratio: (silver - 1)^2 = 2. Plastic number: plastic^3 = plastic + 1.
            const f256 silver_minus_one = silver_ratio<f256>() - one_;
            if (!qd_close(silver_minus_one * silver_minus_one, f256(2.0)))
                return false;
            const f256 rho = plastic<f256>();
            if (!qd_close(rho * rho * rho, rho + one_))
                return false;

            return true;
        }

        static_assert(constants_precision_smoke_test(),
                      "Constants.hpp: a constant disagrees with its definition beyond quad-double rounding.");

    } // namespace Detail

} // namespace SFT::Foundation
