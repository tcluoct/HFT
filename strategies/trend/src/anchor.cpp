// strategies/trend/src/anchor.cpp — anchor translation unit for the trend
// team's header-only strategy library (momentum + breakout). The library's
// headers are compiled where they are used; this near-empty TU gives the
// static_lib project one object of its own (and a stable target for the IDE,
// IPO and the linker).
namespace plat {
int strat_trend_anchor = 0;
}
