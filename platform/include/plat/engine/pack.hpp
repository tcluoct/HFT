#pragma once
// plat/engine/pack.hpp — the compile-time strategy registry: compose a Site
// and its config-driven instance banks from a type list of strategy Defs, so
// the platform's strategy lineup is declared in exactly ONE place (the Pack
// alias in strat_params.hpp) and everything else — `type`-name parsing,
// legacy defaults, bank building, Site instantiation — is derived from it.
//
// Each strategy header carries its own Def next to the class:
//
//   struct MyStratDef {
//     using Strat = MyStrat;                     // the CRTP strategy class
//     static constexpr const char* name = "mystrat";  // config `type` value
//     static constexpr StrategyId legacy_id = 0; // fixed id in the legacy
//                                                // no-`strategies`-key lineup
//                                                // (0 = not part of it)
//     static MyStratParams params(const Config&, const std::string& n);
//   };
//
// and Strat must be constructible as (id, account, params, table_size,
// universe). Everything in this header is cold-path composition code: the
// hot path is untouched, because StratPack expands to the same
// Site<Gateway, Strats...> instantiation, tuple-of-banks and fold dispatch
// as spelling the pack by hand (§9.1 inline mode — no virtuals, no type
// erasure, every handler statically resolved).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "plat/core/config.hpp"
#include "plat/core/log.hpp"
#include "plat/core/types.hpp"
#include "plat/engine/site.hpp"
#include "plat/ref/instrument.hpp"

namespace plat {

// Everything the platform needs to host a strategy TYPE, colocated with the
// type in its own header (see the recipe above).
template <typename D>
concept StrategyDef = requires(const Config& c, const std::string& n) {
  typename D::Strat;
  { std::string_view{D::name} };
  { D::params(c, n) };
  requires AnyStrategy<typename D::Strat>;
  requires std::convertible_to<decltype(D::legacy_id), StrategyId>;
};

// One configured strategy instance: the durable id, the type (an index into
// the pack's def list), and the config prefix its parameters live under.
struct StratSpec {
  std::string name;       // instance name == config key prefix
  std::uint8_t kind = 0;  // index into the pack's def list
  StrategyId id = 0;
  AccountId account = 0;  // resolved (instance override or site default)
};

// `<name>.instruments` selects the contracts an instance trades, falling back
// to the site-wide `instruments` list; every name must resolve in the table
// (unresolved names are logged and skipped).
inline std::vector<InstrumentId> universe_from(const Config& c, const InstrumentTable& table,
                                               const std::string& name) {
  const std::string key = name + ".instruments";
  const std::string used = c.has(key) ? key : std::string("instruments");
  std::vector<InstrumentId> out;
  for (const auto& n : c.list(used)) {
    const InstrumentId id = table.find(n);
    if (id == kNoInstrument)
      logf("[cfg] %s: `%s` is not hosted — skipped\n", used.c_str(), n.c_str());
    else
      out.push_back(id);
  }
  return out;
}

template <StrategyDef... Defs>
struct StratPack {
  static_assert(sizeof...(Defs) > 0);
  static_assert(sizeof...(Defs) < 256, "StratSpec::kind is a uint8_t index");
  static_assert((!std::is_polymorphic_v<typename Defs::Strat> && ...),
                "strategies are CRTP: no vtable on the hot path");

  static constexpr std::size_t kCount = sizeof...(Defs);
  static constexpr std::array<const char*, kCount> kNames{Defs::name...};
  static constexpr std::array<StrategyId, kCount> kLegacyIds{Defs::legacy_id...};

  // The pack composed over a gateway: each type is a BANK in the Site (zero
  // or more configured instances of that type).
  template <typename Gateway>
  using SiteFor = Site<Gateway, typename Defs::Strat...>;

  // What SiteFor's constructor takes: one instance vector per type, in pack
  // order — the order banks are dispatched, saved and hashed (site_state.hpp),
  // which is why the def list is append-only.
  using Banks = std::tuple<std::vector<typename Defs::Strat>...>;

  static constexpr const char* kind_name(std::uint8_t k) noexcept {
    return k < kCount ? kNames[k] : "?";
  }
  // Index of a config `type` value in the pack; kCount when unknown.
  static constexpr std::size_t kind_of(std::string_view type) noexcept {
    for (std::size_t i = 0; i < kCount; ++i)
      if (type == kNames[i]) return i;
    return kCount;
  }

  // Parse + validate the `strategies = ...` instance list. Returns false on
  // any config error — callers must refuse to trade on a config that doesn't
  // say what it means (fail closed). Disabled instances are validated,
  // logged and dropped. Without a `strategies` key the legacy lineup (every
  // def with a nonzero legacy_id, under its own fixed id) is assumed, so
  // existing configs keep working unchanged.
  static bool parse(const Config& c, AccountId default_account, std::vector<StratSpec>& out) {
    out.clear();
    std::vector<std::string> names = c.list("strategies");
    const bool legacy = names.empty();
    if (legacy)
      for (std::size_t i = 0; i < kCount; ++i)
        if (kLegacyIds[i] != 0) names.emplace_back(kNames[i]);

    bool ok = true;
    std::vector<StratSpec> all;
    for (const auto& n : names) {
      StratSpec s;
      s.name = n;
      const std::string type = c.str(n + ".type", n);
      const std::size_t k = kind_of(type);
      if (k == kCount) {
        logf("[cfg] strategy `%s`: unknown type `%s` (%s)\n", n.c_str(), type.c_str(),
             joined_names().c_str());
        ok = false;
        continue;
      }
      s.kind = static_cast<std::uint8_t>(k);
      // The legacy lineup keeps its fixed ids; explicit instances must
      // declare one — the id is what PnL, carry files and kill targets
      // attribute to, so it must be a deliberate, stable choice.
      s.id = static_cast<StrategyId>(c.i64(n + ".id", legacy ? kLegacyIds[k] : 0));
      if (s.id == 0) {
        logf("[cfg] strategy `%s`: missing `%s.id` (unique nonzero StrategyId)\n", n.c_str(),
             n.c_str());
        ok = false;
        continue;
      }
      s.account = static_cast<AccountId>(c.i64(n + ".account", default_account));
      for (const auto& prev : all)
        if (prev.id == s.id) {
          logf("[cfg] strategy `%s`: id %u already used by `%s`\n", n.c_str(), s.id,
               prev.name.c_str());
          ok = false;
        }
      all.push_back(s);
    }
    if (!ok) return false;
    for (auto& s : all) {
      if (!c.flag(s.name + ".enabled", true)) {
        logf("[cfg] strategy `%s` (%s, id %u) is DISABLED — not constructed; its id stays "
             "reserved and carried positions keep flowing through the .positions file\n",
             s.name.c_str(), kind_name(s.kind), s.id);
        continue;
      }
      out.push_back(std::move(s));
    }
    return true;
  }

  // Build every enabled instance, grouped per type in config order. Both
  // platd and platbt construct their Site from this one call, so live and
  // backtest are configured identically.
  static Banks banks(const Config& c, const InstrumentTable& t,
                     const std::vector<StratSpec>& specs) {
    Banks out;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (fill_bank<Defs, I>(std::get<I>(out), c, t, specs), ...);
    }(std::index_sequence_for<Defs...>{});
    return out;
  }

 private:
  template <StrategyDef D, std::size_t I>
  static void fill_bank(std::vector<typename D::Strat>& bank, const Config& c,
                        const InstrumentTable& t, const std::vector<StratSpec>& specs) {
    for (const auto& s : specs)
      if (s.kind == I)
        bank.emplace_back(s.id, s.account, D::params(c, s.name), t.size(),
                          universe_from(c, t, s.name));
  }

  static std::string joined_names() {
    std::string s;
    for (std::size_t i = 0; i < kCount; ++i) {
      if (i) s += '|';
      s += kNames[i];
    }
    return s;
  }
};

}  // namespace plat
