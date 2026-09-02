#pragma once
// backtest_report.hpp — turns a series of per-day backtest results into a
// professional performance report: the standard metrics (total / annualised
// return, Sharpe, volatility, max drawdown, Calmar, win rate, trade count,
// fees) plus an equity curve, an underwater (drawdown) plot and a daily-PnL
// bar chart, rendered as a SELF-CONTAINED HTML file (inline CSS + inline SVG,
// no external assets) and a console summary. platbt-only.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace plat::rec {

// One trading day's realised outcome, as platbt accumulates it. Per-strategy
// PnL is positional: strat_pnl[i] belongs to the report's strat_names[i] —
// one entry per configured strategy instance, whatever the lineup is.
struct DayResult {
  std::uint32_t day = 0;  // YYYYMMDD
  std::vector<double> strat_pnl;  // realised CNY per configured instance
  double fees = 0;
  std::uint64_t fills = 0;
  double net() const {
    double n = 0;
    for (double v : strat_pnl) n += v;
    return n;
  }
};

class BacktestReport {
 public:
  BacktestReport(double capital_cny, std::vector<std::string> strat_names)
      : capital_(capital_cny > 0 ? capital_cny : 1.0), names_(std::move(strat_names)) {}
  void add(const DayResult& d) { days_.push_back(d); }
  bool empty() const { return days_.empty(); }
  std::size_t size() const { return days_.size(); }

  struct Metrics {
    double capital = 0, net_pnl = 0, fees = 0, equity_end = 0;
    double total_return = 0, ann_return = 0, sharpe = 0, vol_ann = 0;
    double max_dd = 0, calmar = 0, best_day = 0, worst_day = 0, win_rate = 0;
    std::vector<double> by_strat;  // realised totals, aligned with strat names
    std::uint64_t trades = 0;
    int days = 0, trading_days = 0, win_days = 0;
  };

  Metrics compute() const {
    Metrics m;
    m.capital = capital_;
    m.days = static_cast<int>(days_.size());
    m.by_strat.assign(names_.size(), 0.0);
    double eq = capital_, peak = capital_;
    std::vector<double> rets;
    rets.reserve(days_.size());
    for (const auto& d : days_) {
      const double net = d.net(), prev = eq;
      m.net_pnl += net;
      m.fees += d.fees;
      m.trades += d.fills;
      for (std::size_t i = 0; i < m.by_strat.size() && i < d.strat_pnl.size(); ++i)
        m.by_strat[i] += d.strat_pnl[i];
      if (net != 0 || d.fills) {
        ++m.trading_days;
        if (net > 0) ++m.win_days;
      }
      if (net > m.best_day) m.best_day = net;
      if (net < m.worst_day) m.worst_day = net;
      rets.push_back(prev > 0 ? net / prev : 0.0);
      eq += net;
      if (eq > peak) peak = eq;
      const double dd = peak > 0 ? (eq - peak) / peak : 0.0;
      if (dd < m.max_dd) m.max_dd = dd;
    }
    m.equity_end = eq;
    m.total_return = m.net_pnl / capital_;
    m.win_rate = m.trading_days ? static_cast<double>(m.win_days) / m.trading_days : 0.0;
    double mean = 0;
    for (double r : rets) mean += r;
    mean /= rets.empty() ? 1.0 : static_cast<double>(rets.size());
    double var = 0;
    for (double r : rets) var += (r - mean) * (r - mean);
    var = rets.size() > 1 ? var / static_cast<double>(rets.size() - 1) : 0.0;
    const double sd = std::sqrt(var);
    m.vol_ann = sd * std::sqrt(252.0);
    m.sharpe = sd > 0 ? mean / sd * std::sqrt(252.0) : 0.0;
    if (m.days > 0 && m.equity_end > 0)
      m.ann_return = std::pow(m.equity_end / capital_, 252.0 / m.days) - 1.0;
    m.calmar = m.max_dd < 0 ? m.ann_return / (-m.max_dd) : 0.0;
    return m;
  }

  void print_summary() const {
    const Metrics m = compute();
    std::printf("\n===== backtest summary (%d day%s) =====\n", m.days, m.days == 1 ? "" : "s");
    std::printf("  capital:            %14s CNY\n", money(m.capital).c_str());
    std::printf("  net realised:       %14s CNY   (%.2f%%)\n", money(m.net_pnl).c_str(),
                m.total_return * 100.0);
    std::printf("  annualised return:  %13.2f%%\n", m.ann_return * 100.0);
    std::printf("  Sharpe (ann.):      %14.2f\n", m.sharpe);
    std::printf("  volatility (ann.):  %13.2f%%\n", m.vol_ann * 100.0);
    std::printf("  max drawdown:       %13.2f%%\n", m.max_dd * 100.0);
    std::printf("  Calmar:             %14.2f\n", m.calmar);
    std::printf("  win rate (days):    %13.2f%%   (%d/%d)\n", m.win_rate * 100.0, m.win_days,
                m.trading_days);
    std::printf("  best / worst day:   %14s / %s CNY\n", money(m.best_day).c_str(),
                money(m.worst_day).c_str());
    std::printf("  ------------------------------------\n");
    for (std::size_t i = 0; i < names_.size(); ++i)
      std::printf("  %-18s %14s CNY\n", (names_[i] + ":").c_str(), money(m.by_strat[i]).c_str());
    std::printf("  trades: %llu     fees: %s CNY\n",
                static_cast<unsigned long long>(m.trades), money(m.fees).c_str());
  }

  // Self-contained HTML: metric cards, equity curve, underwater plot, daily
  // PnL bars, and a per-day table. Returns false if the file can't be written.
  bool write_html(const std::string& path, const std::string& title) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const Metrics m = compute();
    std::string h;
    h.reserve(16384);
    h += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>";
    h += title;
    h += "</title>";
    h += css();
    h += "</head><body><div class=\"wrap\"><h1>" + title + "</h1>";
    h += "<div class=\"sub\">" + std::to_string(m.days) + " trading days &middot; " +
         (days_.empty() ? "" : (std::to_string(days_.front().day) + " \xE2\x86\x92 " +
                                std::to_string(days_.back().day))) +
         "</div>";
    // metric cards
    h += "<div class=\"cards\">";
    h += card("Net P&amp;L", money(m.net_pnl) + " <span class=\"u\">CNY</span>",
              m.net_pnl >= 0 ? "pos" : "neg");
    h += card("Total return", pct(m.total_return), m.total_return >= 0 ? "pos" : "neg");
    h += card("Annualised", pct(m.ann_return), m.ann_return >= 0 ? "pos" : "neg");
    h += card("Sharpe (ann.)", f2(m.sharpe), m.sharpe >= 1 ? "pos" : (m.sharpe < 0 ? "neg" : ""));
    h += card("Max drawdown", pct(m.max_dd), "neg");
    h += card("Volatility", pct(m.vol_ann), "");
    h += card("Calmar", f2(m.calmar), m.calmar >= 0 ? "pos" : "neg");
    h += card("Win rate", pct(m.win_rate), "");
    h += card("Trades", grp(m.trades), "");
    h += card("Fees", money(m.fees) + " <span class=\"u\">CNY</span>", "neg");
    h += "</div>";
    // charts
    h += "<h2>Equity curve</h2>" + equity_svg(m);
    h += "<h2>Drawdown (underwater)</h2>" + drawdown_svg(m);
    h += "<h2>Daily P&amp;L</h2>" + dailypnl_svg();
    // per-strategy split: one card per configured instance
    h += "<h2>By strategy</h2><div class=\"cards\">";
    for (std::size_t i = 0; i < names_.size(); ++i)
      h += card(names_[i], money(m.by_strat[i]) + " <span class=\"u\">CNY</span>",
                m.by_strat[i] >= 0 ? "pos" : "neg");
    h += "</div>";
    // table
    h += table();
    h += "<div class=\"foot\">Generated by platbt &middot; the strategies, riskgate and "
         "PnL are the same code platd runs live.</div>";
    h += "</div></body></html>";
    std::fwrite(h.data(), 1, h.size(), f);
    std::fclose(f);
    return true;
  }

 private:
  double capital_;
  std::vector<std::string> names_;  // configured instance names, column order
  std::vector<DayResult> days_;

  // ---- number formatting ----
  static std::string grp(std::uint64_t v) {
    std::string s = std::to_string(v), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
      if (c && c % 3 == 0) out.push_back(',');
      out.push_back(*it);
      ++c;
    }
    return std::string(out.rbegin(), out.rend());
  }
  static std::string money(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", v < 0 ? -v : v);
    std::string s(buf);
    const auto dot = s.find('.');
    std::string ip = s.substr(0, dot), fp = s.substr(dot);
    std::string g;
    int c = 0;
    for (auto it = ip.rbegin(); it != ip.rend(); ++it) {
      if (c && c % 3 == 0) g.push_back(',');
      g.push_back(*it);
      ++c;
    }
    return (v < 0 ? "-" : "") + std::string(g.rbegin(), g.rend()) + fp;
  }
  static std::string f2(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
  }
  static std::string pct(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f%%", v * 100.0);
    return b;
  }
  static std::string card(const std::string& label, const std::string& val, const char* cls) {
    return "<div class=\"card\"><div class=\"lab\">" + label + "</div><div class=\"val " +
           cls + "\">" + val + "</div></div>";
  }

  // ---- series ----
  std::vector<double> equity() const {
    std::vector<double> e;
    e.reserve(days_.size() + 1);
    double eq = capital_;
    e.push_back(eq);
    for (const auto& d : days_) {
      eq += d.net();
      e.push_back(eq);
    }
    return e;
  }

  // ---- SVG charts (viewBox 0..W x 0..H, scaled by CSS to container width) ----
  static constexpr double kW = 960, kH = 300, kPadL = 70, kPadR = 20, kPadT = 16, kPadB = 28;
  static double xat(std::size_t i, std::size_t n) {
    return n <= 1 ? kPadL : kPadL + (kW - kPadL - kPadR) * (double)i / (double)(n - 1);
  }
  static double yat(double v, double lo, double hi) {
    if (hi <= lo) return kH - kPadB;
    return kH - kPadB - (kH - kPadT - kPadB) * (v - lo) / (hi - lo);
  }

  std::string equity_svg(const Metrics& m) const {
    const std::vector<double> e = equity();
    double lo = e[0], hi = e[0];
    for (double v : e) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    const double pad = (hi - lo) * 0.08 + 1.0;
    lo -= pad;
    hi += pad;
    std::string s = svg_open();
    s += grid(lo, hi, money);
    // capital baseline
    const double yc = yat(m.capital, lo, hi);
    s += line(kPadL, yc, kW - kPadR, yc, "base");
    // area + line
    std::string pts, area = "M";
    for (std::size_t i = 0; i < e.size(); ++i) {
      char b[64];
      std::snprintf(b, sizeof b, "%.1f,%.1f ", xat(i, e.size()), yat(e[i], lo, hi));
      pts += b;
      area += b;
    }
    char tail[64];
    std::snprintf(tail, sizeof tail, "L%.1f,%.1f L%.1f,%.1f Z", xat(e.size() - 1, e.size()),
                  kH - kPadB, xat(0, e.size()), kH - kPadB);
    area += tail;
    s += "<path d=\"" + area + "\" class=\"eqfill\"/>";
    s += "<polyline points=\"" + pts + "\" class=\"eqline\"/>";
    s += "</svg>";
    return s;
  }

  std::string drawdown_svg(const Metrics& m) const {
    const std::vector<double> e = equity();
    std::vector<double> dd(e.size());
    double peak = e[0], lo = 0;
    for (std::size_t i = 0; i < e.size(); ++i) {
      peak = std::max(peak, e[i]);
      dd[i] = peak > 0 ? (e[i] - peak) / peak : 0.0;
      lo = std::min(lo, dd[i]);
    }
    lo = std::min(lo * 1.1, -1e-9);
    std::string s = svg_open();
    s += grid(lo, 0, [](double v) { return pct(v); });
    std::string area = "M";
    for (std::size_t i = 0; i < dd.size(); ++i) {
      char b[64];
      std::snprintf(b, sizeof b, "%.1f,%.1f ", xat(i, dd.size()), yat(dd[i], lo, 0));
      area += b;
    }
    char tail[96];
    std::snprintf(tail, sizeof tail, "L%.1f,%.1f L%.1f,%.1f Z", xat(dd.size() - 1, dd.size()),
                  yat(0, lo, 0), xat(0, dd.size()), yat(0, lo, 0));
    area += tail;
    s += "<path d=\"" + area + "\" class=\"ddfill\"/></svg>";
    (void)m;
    return s;
  }

  std::string dailypnl_svg() const {
    if (days_.empty()) return svg_open() + "</svg>";
    double hi = 0, lo = 0;
    for (const auto& d : days_) {
      hi = std::max(hi, d.net());
      lo = std::min(lo, d.net());
    }
    const double span = std::max(hi, -lo) * 1.1 + 1.0;
    lo = -span;
    hi = span;
    std::string s = svg_open();
    s += grid(lo, hi, money);
    const double y0 = yat(0, lo, hi);
    const std::size_t n = days_.size();
    const double bw = std::max(1.0, (kW - kPadL - kPadR) / (double)n * 0.7);
    for (std::size_t i = 0; i < n; ++i) {
      const double net = days_[i].net();
      const double x = xat(i, n) - bw / 2, y = yat(net, lo, hi);
      char b[160];
      std::snprintf(b, sizeof b, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" class=\"%s\"/>",
                    x, std::min(y, y0), bw, std::abs(y - y0), net >= 0 ? "barpos" : "barneg");
      s += b;
    }
    s += "</svg>";
    return s;
  }

  static std::string svg_open() {
    char b[128];
    std::snprintf(b, sizeof b, "<svg viewBox=\"0 0 %.0f %.0f\" class=\"chart\" preserveAspectRatio=\"none\">", kW, kH);
    return b;
  }
  static std::string line(double x1, double y1, double x2, double y2, const char* cls) {
    char b[160];
    std::snprintf(b, sizeof b, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" class=\"%s\"/>",
                  x1, y1, x2, y2, cls);
    return b;
  }
  // Horizontal gridlines + y-axis labels at 5 levels, labelled by `fmt`.
  template <typename Fmt>
  static std::string grid(double lo, double hi, Fmt fmt) {
    std::string s;
    for (int k = 0; k <= 4; ++k) {
      const double v = lo + (hi - lo) * k / 4.0;
      const double y = yat(v, lo, hi);
      s += line(kPadL, y, kW - kPadR, y, "grid");
      char b[160];
      std::snprintf(b, sizeof b, "<text x=\"%.1f\" y=\"%.1f\" class=\"yl\">%s</text>",
                    kPadL - 6, y + 4, fmt(v).c_str());
      s += b;
    }
    return s;
  }

  std::string table() const {
    std::string s = "<h2>Daily detail</h2><div class=\"tw\"><table><thead><tr><th>Day</th>";
    for (const auto& n : names_) s += "<th>" + n + "</th>";
    s += "<th>Net</th><th>Fees</th><th>Fills</th><th>Equity</th></tr></thead><tbody>";
    double eq = capital_;
    for (const auto& d : days_) {
      eq += d.net();
      const char* nc = d.net() >= 0 ? "pos" : "neg";
      s += "<tr><td>" + std::to_string(d.day) + "</td>";
      for (std::size_t i = 0; i < names_.size(); ++i)
        s += "<td>" + money(i < d.strat_pnl.size() ? d.strat_pnl[i] : 0.0) + "</td>";
      s += "<td class=\"" + std::string(nc) + "\">" + money(d.net()) + "</td><td>" +
           money(d.fees) + "</td><td>" + grp(d.fills) + "</td><td>" + money(eq) + "</td></tr>";
    }
    s += "</tbody></table></div>";
    return s;
  }

  static std::string css() {
    return
        "<style>"
        ":root{--bg:#0b1020;--pnl:#e2e8f0;--card:#141b2e;--mut:#8391a8;--pos:#22c55e;"
        "--neg:#ef4444;--acc:#3b82f6;--grid:#26304a;--line:#60a5fa}"
        "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--pnl);"
        "font:14px/1.5 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}"
        ".wrap{max-width:1040px;margin:0 auto;padding:32px 20px 64px}"
        "h1{font-size:24px;margin:0 0 2px}h2{font-size:16px;margin:34px 0 10px;color:var(--mut);"
        "font-weight:600;letter-spacing:.02em}.sub{color:var(--mut);margin-bottom:24px}"
        ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}"
        ".card{background:var(--card);border:1px solid var(--grid);border-radius:10px;padding:14px 16px}"
        ".lab{color:var(--mut);font-size:12px;margin-bottom:6px}"
        ".val{font-size:20px;font-weight:600;font-variant-numeric:tabular-nums}"
        ".val .u{font-size:12px;color:var(--mut);font-weight:400}"
        ".pos{color:var(--pos)}.neg{color:var(--neg)}"
        ".chart{width:100%;height:auto;background:var(--card);border:1px solid var(--grid);"
        "border-radius:10px;display:block}"
        ".grid{stroke:var(--grid);stroke-width:1}.base{stroke:#5b6b8c;stroke-width:1;stroke-dasharray:4 4}"
        ".yl{fill:var(--mut);font-size:11px;text-anchor:end;font-family:ui-monospace,monospace}"
        ".eqline{fill:none;stroke:var(--line);stroke-width:2}"
        ".eqfill{fill:rgba(96,165,250,.12);stroke:none}"
        ".ddfill{fill:rgba(239,68,68,.22);stroke:#ef4444;stroke-width:1.5}"
        ".barpos{fill:var(--pos)}.barneg{fill:var(--neg)}"
        ".tw{overflow-x:auto;border:1px solid var(--grid);border-radius:10px;margin-top:6px}"
        "table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}"
        "th,td{padding:8px 12px;text-align:right;border-bottom:1px solid var(--grid);white-space:nowrap}"
        "th{color:var(--mut);font-weight:600;font-size:12px;position:sticky;top:0;background:var(--card)}"
        "th:first-child,td:first-child{text-align:left}"
        "tbody tr:hover{background:#1b2440}"
        ".foot{color:var(--mut);font-size:12px;margin-top:28px;text-align:center}"
        "</style>";
  }
};

}  // namespace plat::rec
