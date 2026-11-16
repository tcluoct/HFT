# fade — the fade team's strategy library

`strat_fade` owns the futures mean-reversion strategy:

| Strategy | Type name | Idea |
|---|---|---|
| `MeanRevert` | `meanrev` | An integer EMA of the snapshot mid is the anchor; fade a stretch of ≥ N ticks only when the L1 imbalance **opposes** the move's continuation (the move is running out of size). |

It complements the trend team by construction: momentum needs a move with
agreeing imbalance, the fade needs a stretch with opposing imbalance — the two
cannot fire on the same tick in the same direction, so running both
concurrently diversifies rather than doubles exposure. Exits are the shared
take/stop/timeout cycle (`plat/sdk/cycle.hpp`).

## Ownership and dependencies

* Owned by the fade team; header-only, fielded by `desk/pack.hpp`.
* Depends on the strategy SDK only (`strat_sdk`). No engine dependency.
* The engine appears in the team's test suite solely as a host.

## Build and test

```pwsh
cmake -S strategies/fade -B build/fade
cmake --build build/fade --config Release --target strat_fade_tests
build/fade/Release/strat_fade_tests.exe
```

The suite asserts the gate in both directions: a stretch with the imbalance
agreeing with the move must not fade, and the same stretch ask-heavy must
sell rich at the bid; the filled entry moves the cycle to Holding and the
state hash changes.
