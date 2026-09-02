// plat_core.cpp — anchor translation unit for the standalone-safe base
// layer (plat/core/*, ctp/conv, the .mdr codec/file). All header-only; this
// one (near-empty) TU exists so those headers live in ONE library target —
// `plat_core` — shared by platform_lib and by tools like platrec that must
// not link the trading engine.
namespace plat {
int plat_core_anchor = 0;
}
