# strategies — the strategy-team static libraries

A hedge fund runs many strategy teams; the platform is one execution system
they all trade on. This tree is that organisation in code: **one static
library project per team**, each with its own headers, tests, README and
build, and each depending only on the platform's *strategy SDK* — never on
the engine.

| Team | Project | Target | Strategies | Fielded in the house desk |
|---|---|---|---|---|
| [trend](trend/) | `strategies/trend` | `strat_trend` | snapshot momentum, session breakout | yes |
| [fade](fade/) | `strategies/fade` | `strat_fade` | snapshot mean-reversion | yes |
| [basis](basis/) | `strategies/basis` | `strat_basis` | futures–spot arb (hedge leg + cash leg) | no (Phase 2) |
| [cash](cash/) | `strategies/cash` | `strat_cash` | T+0 L2 alpha on carried stock inventory | no (Phase 2) |

## The dependency rule

```
plat_core                      (base layer: types, config, clocks, logging)
   ▲
strat_sdk                      (strategy SDK: plat/sdk/* + codec, books,
   ▲                            reference data, PnL types)
   ├────────────── strategies/<team>     static libs, this tree
   │                    ▲
platform_lib (engine)    │
   ▲                     │
   └────────────── desk  ─┘             the deployed lineup: Pack + PackSite
                        ▲
        platd / platbt / platops / plat_bench / plat_tests
```

* A team library includes `plat/sdk/framework.hpp` (the CRTP `Strategy<D>`
  contract), the shared primitives (`cycle.hpp`, `signals.hpp`, `kbar.hpp`,
  `indicators.hpp`) and the SDK's market-data/reference/accounting types.
* A team library **never** includes `plat/engine/site.hpp`. It does not know
  the gateway, the risk gate or the OMS exist — the engine is the host, the
  SDK is the contract.
* `desk/pack.hpp` is the only place that names team types. It appends their
  `Def`s to a `StratPack`, which derives parsing, bank building and the
  `Site` type the platforms link.

## Build and test

```pwsh
# from the solution root (configures everything once)
cmake --preset vs2026
cmake --build --preset vs2026-release --target strat_trend_tests strat_fade_tests `
      strat_basis_tests strat_cash_tests desk_tests
build\vs2026\strategies\trend\Release\strat_trend_tests.exe
build\vs2026\strategies\fade\Release\strat_fade_tests.exe
build\vs2026\strategies\basis\Release\strat_basis_tests.exe
build\vs2026\strategies\cash\Release\strat_cash_tests.exe
build\vs2026\desk\Release\desk_tests.exe
```

Each team also configures standalone — it pulls in the platform for the SDK
and engine with `HFT_PLATFORM_APPS=OFF`, so only libraries build:

```pwsh
cmake -S strategies/trend -B build/trend
cmake --build build/trend --config Release --target strat_trend_tests
```

## Adding a strategy type

1. Write the strategy + its params struct + its `Def` traits in the owning
   team's `include/strat/<team>/<name>.hpp` (see the recipe in
   `platform/include/plat/engine/pack.hpp`).
2. Add the header to that team's `CMakeLists.txt` and cover it in the team's
   `src/tests.cpp`.
3. To deploy it, append its `Def` to `desk/include/desk/pack.hpp` (append
   only, never reorder) and link the team from `desk/CMakeLists.txt` if it is
   a new team.

The platform is not touched in any of those steps. That is the point.

## Why static libraries, not a runtime plugin

Every strategy is a template on the site's context, so a handler compiles to a
direct, inlinable call — no virtual hop between a snapshot arriving and an
order leaving. A runtime plugin API was evaluated and rejected: its per-event
indirection is a latency tax on exactly the path that matters. The library
boundary gives teams ownership and isolation without putting a vtable on the
tick path.
