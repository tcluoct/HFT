// strat_sdk.cpp — anchor translation unit for the strategy SDK: the types and
// contract a strategy-team library is allowed to include (plat/core,
// plat/codec, plat/md, plat/ref, plat/pnl, plat/sdk). All header-only; this
// near-empty TU exists so those headers live in ONE library target —
// `strat_sdk` — that team libraries link instead of the engine, and that
// platform_lib layers on top.
namespace plat {
int strat_sdk_anchor = 0;
}
