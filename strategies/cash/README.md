# cash — the cash-equity team's strategy library

`strat_cash` owns the stock-inventory intraday strategy:

| Strategy | Type name | Idea |
|---|---|---|
| `T0Alpha` | (not yet in the house Pack) | The Chinese-market shape of T+0: T+1 means a fresh buy cannot round-trip today, so the strategy works a carried 底仓 — sell first out of sellable inventory on ask-pressure signals (EMA of L1 imbalance), buy back on reversion or a take/stop trigger, and restore inventory before the close. |

## Status: Phase 2

Phase 1 trades commodity futures through CTP; there is no stock venue yet, so
the house desk does not field this team (`desk/pack.hpp` does not include its
type). The library and its suite keep the T+1 ledger, IOC path and deny
handling compiled and tested while it waits.

## Ownership and dependencies

* Owned by the cash team; header-only, depends on `strat_sdk` only.
* The engine appears in the test suite solely as a host.

## Build and test

```pwsh
cmake -S strategies/cash -B build/cash
cmake --build build/cash --config Release --target strat_cash_tests
build/cash/Release/strat_cash_tests.exe
```

The suite asserts the channel's most important safety property: ask pressure
with **no** sellable inventory must not create a naked short (T+1), while the
same pressure after a carried 底仓 is granted through the PnL ledger's
carry-in sells exactly the configured clip; after the fill is consumed, the
strategy waits for a buy-back instead of selling again.
