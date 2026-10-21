// tests_main.cpp — plat_tests entry point: the harness counters and the
// run order (components first, then the engine suites built on them).

#include <cstdio>

#include "plat/core/crash.hpp"  // a crashing suite prints a named stack

int g_checks = 0;
int g_fails = 0;

// tests_components.cpp
void test_conv();
void test_config();
void test_fut_book();
void test_oms();
void test_riskgate();
void test_risk_desk();
void test_pnl();
void test_journal();
void test_ring();
void test_ring_broadcast();
void test_instrument_table();
void test_tick_codec();
void test_tick_file();
void test_tick_depth_l1_l2();
void test_tick_compare();
void test_backtest_replay();
void test_positions_file();
// tests_signals.cpp
void test_kbar();
void test_kbar_snapshot();
void test_indicators();
// tests_engine.cpp
void test_site_loop();
void test_momentum_multi_instrument();
void test_three_strategies();
// tests_strats.cpp
void test_two_momentum_instances();
void test_strategy_config();
void test_phase2_strategies_compile();
void test_crash_resume();
void test_async_journal();
// tests_ops.cpp
void test_ops_param_update();
void test_ops_manual_order_cancel();
void test_ops_position_transfer();
void test_ops_mirror();
void test_ops_journal_tailer();

int main() {
  plat::install_crash_handlers("plat_tests");
  test_conv();
  test_config();
  test_fut_book();
  test_oms();
  test_riskgate();
  test_risk_desk();
  test_pnl();
  test_journal();
  test_ring();
  test_ring_broadcast();
  test_instrument_table();
  test_tick_codec();
  test_tick_file();
  test_tick_depth_l1_l2();
  test_tick_compare();
  test_backtest_replay();
  test_positions_file();
  test_kbar();
  test_kbar_snapshot();
  test_indicators();
  test_site_loop();
  test_momentum_multi_instrument();
  test_three_strategies();
  test_two_momentum_instances();
  test_strategy_config();
  test_phase2_strategies_compile();
  test_crash_resume();
  test_async_journal();
  test_ops_param_update();
  test_ops_manual_order_cancel();
  test_ops_position_transfer();
  test_ops_mirror();
  test_ops_journal_tailer();
  std::printf("plat_tests: %d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
