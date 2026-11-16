# desk — the deployed strategy lineup (composition root)

The platform engine knows no strategy. A **desk** is where a lineup is
declared: it includes the strategy-team headers it fields, appends their
`Def`s to a `StratPack`, and exports the machinery every platform binary
uses.

```cpp
// desk/include/desk/pack.hpp
using Pack = StratPack<MomentumDef, MeanRevDef, BreakoutDef>;
template <typename Gateway> using PackSite = Pack::SiteFor<Gateway>;
```

The house desk fields three production futures strategies from two teams:

| Bank | Team | Type name | Legacy id |
|---|---|---|---|
| 0 | trend | `momentum` | 1 |
| 1 | fade | `meanrev` | 2 |
| 2 | trend | `breakout` | 3 |

The Phase-2 teams (`basis`, `cash`) are not in this pack — no federation or
stock venue is wired in Phase 1.

## The contract

* **Append only, never reorder.** Banks are dispatched, saved in snapshots
  and hashed in pack order (`plat/engine/site_state.hpp`); reordering breaks
  replay and snapshot compatibility. Appending a new type at the end is safe:
  its bank starts empty on existing snapshots.
* The `StrategyId` in config is the durable identity (risk scopes, kill
  targets, PnL, overnight position carry) — never reuse one.
* A desk links every team library whose types it names. The platform apps
  (`platd`, `platbt`, `platops`, `plat_bench`, `plat_tests`) link this desk
  and nothing else strategy-specific.

## Building a different desk

A futures-only paper book, a single team's research build, or a desk that also
fields Phase-2 teams is a **new header in this project** plus a target that
links the team libraries it fields — no platform change:

```cmake
add_library(desk_futures STATIC src/anchor.cpp include/desk/futures.hpp)
target_link_libraries(desk_futures PUBLIC platform_lib strat_trend strat_fade)
```

and point the apps at it. The full build is a desk per deployment; the
platform is built once.

## Build and test

```pwsh
cmake -S desk -B build/desk
cmake --build build/desk --config Release --target desk_tests
build/desk/Release/desk_tests.exe
```

From the solution root: target `desk_tests`; the exe lands in
`build/vs2026/desk/Release/`.

`desk_tests` covers the two concerns of a desk: the cross-team composition
(trend + fade + breakout instances on one site, same dispatch pass, separate
ids, no interference) and the config-facing lineup machinery (`strategies =
...` parsing, the legacy trio, fail-closed validation, per-bank instance
order).
