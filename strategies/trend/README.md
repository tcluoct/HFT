# trend — the trend team's strategy library

`strat_trend` owns the two directional futures strategies:

| Strategy | Type name | Idea |
|---|---|---|
| `SnapMomentum` | `momentum` | Chinese futures disclose 250–500 ms snapshots; act on the snapshot delta when the mid move and the L1 imbalance agree. |
| `Breakout` | `breakout` | Act only at fresh session extremes: the mid must clear the session high/low by N ticks with the imbalance confirming. |

Both are multi-instrument by construction: one configured instance runs any
number of contracts, with per-contract state in dense-id slot tables. Exits
are the shared take/stop/timeout cycle (`plat/sdk/cycle.hpp`) closed with
`Offset::CloseToday`, so the fee model and the closeable-bucket risk check are
exercised end to end.

## Ownership and dependencies

* Owned by the trend team; header-only, compiled into the site by the desk
  that fields it (`desk/pack.hpp`).
* Depends on the strategy SDK only (`strat_sdk`: `plat/sdk/*`,
  `plat/md/*`, `plat/ref/*`, `plat/codec/*`). No engine dependency.
* The engine appears in the team's test suite solely as a host
  (`Site<FakeGateway, ...>`).

## Build and test

```pwsh
cmake -S strategies/trend -B build/trend            # standalone (libraries only)
cmake --build build/trend --config Release --target strat_trend_tests
build/trend/Release/strat_trend_tests.exe
```

From the solution root, the target is `strat_trend_tests` and the exe lands in
`build/vs2026/strategies/trend/Release/`.

The suite covers momentum's per-instrument slots (one instance, two contracts,
independent states), two instances of one type with different gates and
universes (separate ids/PnL books, snapshot round-trip), and breakout's
session-extreme trigger.
