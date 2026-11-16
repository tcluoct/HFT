# basis — the basis-arbitrage team's strategy library

`strat_basis` owns the futures–spot arbitrage pair (sord graduation, §9.3).
It is **two cooperating strategies, one per site**, coordinating only through
the cross-site messages a real private line would carry:

| Leg | Role |
|---|---|
| `ArbHedgeLeg` (futures site) | Exports `BasisSummary` on every snapshot; executes inbound `HedgeIntent`s aggressively within the intent's limit and reports `HedgeDone` either way. |
| `ArbCashLeg` (cash site) | Maintains the pair view (local ETF book + the deliberately stale remote leg), lifts the ETF leg IOC when the basis edge clears costs, and fires a hedge intent when unhedged notional reaches one futures lot. |

Per-pair state lives in slot arrays; instrument-keyed events resolve their
pair through dense-id index tables, remote messages through their `pair_id`.
The §9.3 opportunity ledger tracks detected → legged → hedged with realized
hedge slippage.

## Status: Phase 2

No federation venue is wired yet, so the house desk does **not** field this
team: `desk/pack.hpp` does not include its types. The library and its suite
keep the code compiling and covered while it waits, exactly as the old
Phase-2 compile test did — but now in the team's own project.

## Ownership and dependencies

* Owned by the basis team; header-only, depends on `strat_sdk` only.
* The engine appears in the test suite solely as a host
  (`Site<FakeGateway, ArbCashLeg, ArbHedgeLeg>`).

## Build and test

```pwsh
cmake -S strategies/basis -B build/basis
cmake --build build/basis --config Release --target strat_basis_tests
build/basis/Release/strat_basis_tests.exe
```
