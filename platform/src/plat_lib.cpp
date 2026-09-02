// plat_lib.cpp — anchor translation unit for the header-only platform library.
//
// The engine, strategies, books and OMS/PnL/risk are all header-only. This
// one (near-empty) TU exists so those headers live in ONE library target —
// `platform_lib` (layered on `plat_core`, see plat_core.cpp) — that every
// engine binary links and reuses. No executable re-lists the platform
// headers, so nothing is duplicated across the platd / platbt / plat_tests /
// plat_bench projects.
namespace plat {
int platform_lib_anchor = 0;
}
