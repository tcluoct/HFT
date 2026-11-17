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
void test_latency_measurements();
void test_latency_boundaries();
void test_crash_resume();
void test_flatten_closeout();
void test_flatten_replay();
void test_async_journal();
void test_journal_fault_unsealed();
void test_session_fingerprint();
void test_risk_limits_reload();
void test_snapshot_resume();
void test_secret_resolution();
// tests_ops.cpp
void test_ops_http_auth();
void test_ops_param_update();
void test_ops_manual_order_cancel();
void test_ops_position_transfer();
void test_ops_mirror();
void test_ops_journal_tailer();
// tests_safety.cpp
void test_query_fail_closed();
void test_safety_interlocks();
void test_recovery_counter_audit();
void test_session_marker();

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
  test_latency_measurements();
  test_latency_boundaries();
  test_crash_resume();
  test_flatten_closeout();
  test_flatten_replay();
  test_async_journal();
  test_journal_fault_unsealed();
  test_session_fingerprint();
  test_risk_limits_reload();
  test_snapshot_resume();
  test_secret_resolution();
  test_ops_http_auth();
  test_ops_param_update();
  test_ops_manual_order_cancel();
  test_ops_position_transfer();
  test_ops_mirror();
  test_ops_journal_tailer();
  test_query_fail_closed();
  test_safety_interlocks();
  test_recovery_counter_audit();
  test_session_marker();
  std::printf("plat_tests: %d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
