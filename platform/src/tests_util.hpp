#pragma once
// tests_util.hpp — the platform engine suite's composition: the shared
// scenario harness (plat/testing/harness.hpp) plus the strategy the
// production desk runs, used as the end-to-end stimulus for engine tests
// (site loop, crash resume, async journal, ops console).
//
// The engine LIBRARY has no dependency on any strategy; this header is the
// test executable's choice of stimulus, and it gets that strategy through
// the desk composition the production binaries link (see desk/pack.hpp).

#include <type_traits>

#include "plat/engine/site.hpp"
#include "plat/testing/harness.hpp"
#include "strat/trend/momentum.hpp"

using namespace plat;

using TestSite = Site<FakeGateway, SnapMomentum>;
// The whole hot path is compile-time composed: no vtables anywhere.
static_assert(!std::is_polymorphic_v<SnapMomentum>);
static_assert(!std::is_polymorphic_v<TestSite>);
static_assert(!std::is_polymorphic_v<FutBook>);
static_assert(!std::is_polymorphic_v<RiskGate>);
