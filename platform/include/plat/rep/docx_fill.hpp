#pragma once
// plat/rep/docx_fill.hpp — fills the text fields of the CSRC/exchange
// program-trading "external system access report"
// (附件95-3：外部系统接入报告（程序化）.docx): software name/version, test
// account, APPID, local IP/MAC, the CTP counter, the feature-scope
// checkboxes, and a computed MD5 line for the software package. It does NOT
// touch the ~20 screenshot placeholders (login proof, order/cancel/close,
// threshold-alert triggers, error handling, logs, ...) — those are evidence
// of a live test session and have to be pasted in by whoever ran one.
//
// A .docx is a zip (OPC package). word/document.xml is plain XML with one
// very regular shape for every one-line answer: a table row of
// `<w:tc>label</w:tc><w:tc>EMPTY paragraph</w:tc>` — no run at all in the
// value cell. Filling a field means finding the label's own `<w:t>` run,
// walking to the end of that cell, then inserting a new `<w:r>` into the
// (still-empty) paragraph of the very next cell. See docx_detail below.
//
// Repacking the zip goes through plat/rep/winproc.hpp (Expand-Archive /
// Compress-Archive) rather than a vendored DEFLATE codec — see that header
// for why.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "plat/core/log.hpp"
#include "plat/rep/win_paths.hpp"
#include "plat/rep/winproc.hpp"

namespace plat::rep {

struct AccessReportFields {
  std::string software_name;
  std::string software_version;
  std::string test_account;
  std::string test_time;  // pre-formatted, e.g. "2026-09-01 14:30:00"
  std::string app_id;
  std::string local_ip;
  std::string local_mac;
  std::string relay_app_id;    // left blank unless relay mode was used
  std::string relay_proxy_ip;  // left blank unless relay mode was used
  std::string trading_system;  // e.g. "CTP"

  // OFF by default in the caller (accrep_main.cpp) — see there for why.
  bool scope_penetration = false;      // 穿透式监管
  bool scope_interface = false;        // 接口适应性
  bool scope_program_trading = false;  // 程序化交易系统（异常监测...）
  bool scope_access_security = false;  // 接入安全性
  bool scope_sub_account = false;      // 是否具备分账户功能

  std::optional<std::string> md5_note;  // pre-formatted line to insert under §4, if any
};

namespace docx_detail {

inline std::string xml_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

inline std::string read_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline bool write_file(const std::filesystem::path& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(f);
}

// Finds the label run `<w:t[...]>LABEL</w:t>` (exact tag-content match; both
// xml:space variants seen in this template are tried) and inserts `value`
// as a new run into the NEXT table cell's (still-empty) paragraph — the
// template's convention for every one-line answer field. Copies that
// paragraph's default `<w:rPr>` onto the new run so the inserted text
// matches the surrounding font/size instead of falling back to Word's
// document default. Returns false (and logs) if the label isn't found, so a
// template change is never silently ignored — a script that quietly skips a
// field is worse than one that stops and says so.
inline bool fill_field_after_label(std::string& xml, const std::string& label,
                                    const std::string& value) {
  if (value.empty()) return true;  // nothing to fill; not an error
  const std::string open1 = "<w:t>" + label + "</w:t>";
  const std::string open2 = "<w:t xml:space=\"preserve\">" + label + "</w:t>";
  std::size_t pos = xml.find(open1);
  if (pos == std::string::npos) pos = xml.find(open2);
  if (pos == std::string::npos) {
    logf("[accrep] template field not found: \"%s\" (template may have changed)\n",
         label.c_str());
    return false;
  }

  const std::size_t label_cell_end = xml.find("</w:tc>", pos);
  if (label_cell_end == std::string::npos) return false;
  const std::size_t value_cell_start = xml.find("<w:tc", label_cell_end);
  if (value_cell_start == std::string::npos) return false;
  const std::size_t value_cell_end = xml.find("</w:tc>", value_cell_start);
  if (value_cell_end == std::string::npos) return false;

  const std::size_t ppr_end = xml.find("</w:pPr>", value_cell_start);
  if (ppr_end == std::string::npos || ppr_end > value_cell_end) {
    logf("[accrep] value cell for \"%s\" has no <w:pPr> — template shape changed\n",
         label.c_str());
    return false;
  }
  // Reuse the paragraph's default run properties (font/size) on the new run.
  std::string rpr;
  const std::size_t rpr_start = xml.rfind("<w:rPr>", ppr_end);
  const std::size_t ppr_start = xml.rfind("<w:pPr>", ppr_end);
  if (rpr_start != std::string::npos && (ppr_start == std::string::npos || rpr_start > ppr_start)) {
    const std::size_t rpr_end = xml.find("</w:rPr>", rpr_start);
    if (rpr_end != std::string::npos && rpr_end < ppr_end)
      rpr = xml.substr(rpr_start, rpr_end + std::strlen("</w:rPr>") - rpr_start);
  }

  const std::string run =
      "<w:r>" + rpr + "<w:t xml:space=\"preserve\">" + xml_escape(value) + "</w:t></w:r>";
  xml.insert(ppr_end + std::strlen("</w:pPr>"), run);
  return true;
}

// Marks a scope checkbox: `<w:t>\xE2\x96\xA1LABEL</w:t>` (□LABEL) becomes
// `<w:t>\xE2\x98\x91LABEL</w:t>` (☑LABEL) — a 3-byte-for-3-byte glyph swap
// inside the existing run, so it can't disturb surrounding formatting.
inline bool set_checkbox(std::string& xml, const std::string& label, bool checked) {
  if (!checked) return true;  // the template already ships every box unchecked (□)
  const std::string unchecked = "\xE2\x96\xA1" + label;  // U+25A1
  const std::string open1 = "<w:t>" + unchecked + "</w:t>";
  const std::string open2 = "<w:t xml:space=\"preserve\">" + unchecked + "</w:t>";
  std::size_t pos = xml.find(open1);
  if (pos == std::string::npos) pos = xml.find(open2);
  if (pos == std::string::npos) {
    logf("[accrep] checkbox not found: \"%s\" (template may have changed)\n", label.c_str());
    return false;
  }
  const std::size_t glyph_pos = xml.find(unchecked, pos);
  xml.replace(glyph_pos, 3, "\xE2\x98\x91");  // U+2611, same 3-byte width
  return true;
}

// Inserts a new one-line paragraph right after the paragraph containing
// `anchor_text` — used to drop the computed MD5 line under the "software
// package MD5" heading, ahead of its screenshot-placeholder paragraphs.
inline bool insert_paragraph_after(std::string& xml, const std::string& anchor_text,
                                    const std::string& text) {
  const std::size_t pos = xml.find(anchor_text);
  if (pos == std::string::npos) {
    logf("[accrep] MD5 anchor text not found: \"%s\"\n", anchor_text.c_str());
    return false;
  }
  const std::size_t p_end = xml.find("</w:p>", pos);
  if (p_end == std::string::npos) return false;
  const std::string para =
      "<w:p><w:pPr><w:rPr><w:rFonts w:ascii=\"\xE4\xBB\xBF\xE5\xAE\x8B\" "
      "w:hAnsi=\"\xE4\xBB\xBF\xE5\xAE\x8B\" w:eastAsia=\"\xE4\xBB\xBF\xE5\xAE\x8B\"/>"
      "<w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/></w:rPr></w:pPr><w:r><w:rPr>"
      "<w:rFonts w:ascii=\"\xE4\xBB\xBF\xE5\xAE\x8B\" w:hAnsi=\"\xE4\xBB\xBF\xE5\xAE\x8B\" "
      "w:eastAsia=\"\xE4\xBB\xBF\xE5\xAE\x8B\"/><w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/>"
      "</w:rPr><w:t xml:space=\"preserve\">" +
      xml_escape(text) + "</w:t></w:r></w:p>";
  xml.insert(p_end + std::strlen("</w:p>"), para);
  return true;
}

}  // namespace docx_detail

#ifdef _WIN32

// Fills `template_path` with `f`, writing the result to `output_path`.
// Returns false (with a logged reason) on any I/O or template-shape
// failure — this never produces a silently half-filled report.
inline bool fill_access_report(const std::filesystem::path& template_path,
                                const std::filesystem::path& output_path,
                                const AccessReportFields& f) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(template_path, ec)) {
    logf("[accrep] template not found: %s\n", wide_to_utf8(template_path.wstring()).c_str());
    return false;
  }
  fs::create_directories(output_path.parent_path(), ec);

  // Work in a fresh temp dir next to the output file (same volume, so the
  // eventual repack write is cheap and never crosses a permissions
  // boundary).
  fs::path work = output_path;
  work.remove_filename();
  work /= (output_path.stem().wstring() + L"_accrep_tmp");
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  if (ec) {
    logf("[accrep] cannot create work dir\n");
    return false;
  }
  // NOT pre-created: ZipFile::ExtractToDirectory makes it, and the .NET
  // FileSystem module only accepts a destination that doesn't already exist.
  const fs::path extract_dir = work / L"x";

  // Expand-Archive/Compress-Archive would be the obvious choice here, but
  // Expand-Archive rejects any source whose extension isn't literally
  // ".zip" — it won't open a .docx even though it's the same container
  // format. Driving System.IO.Compression.ZipFile directly (still the exact
  // same .NET implementation underneath) has no such extension gate.
  const int unzip_rc = run_powershell(
      L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
      L"[System.IO.Compression.ZipFile]::ExtractToDirectory(" +
      ps_quote_w(template_path.wstring()) + L", " + ps_quote_w(extract_dir.wstring()) + L")");
  if (unzip_rc != 0) {
    logf("[accrep] could not open the template as a zip container (exit %d)\n", unzip_rc);
    fs::remove_all(work, ec);
    return false;
  }

  const fs::path doc_xml = extract_dir / L"word" / L"document.xml";
  std::string xml = docx_detail::read_file(doc_xml);
  if (xml.empty()) {
    logf("[accrep] could not read word/document.xml from the template\n");
    fs::remove_all(work, ec);
    return false;
  }

  bool ok = true;
  ok &= docx_detail::fill_field_after_label(xml, "\xE6\x8E\xA5\xE5\x85\xA5\xE8\xBD\xAF\xE4\xBB"
                                                  "\xB6\xE5\x90\x8D\xE7\xA7\xB0",
                                             f.software_name);  // 接入软件名称
  ok &= docx_detail::fill_field_after_label(
      xml, "\xE8\xBD\xAF\xE4\xBB\xB6\xE7\x89\x88\xE6\x9C\xAC", f.software_version);  // 软件版本
  ok &= docx_detail::fill_field_after_label(
      xml, "\xE6\xB5\x8B\xE8\xAF\x95\xE8\xB4\xA6\xE5\x8F\xB7", f.test_account);  // 测试账号
  ok &= docx_detail::fill_field_after_label(
      xml, "\xE6\xB5\x8B\xE8\xAF\x95\xE6\x97\xB6\xE9\x97\xB4", f.test_time);  // 测试时间
  ok &= docx_detail::fill_field_after_label(xml, "APPID", f.app_id);
  ok &= docx_detail::fill_field_after_label(
      xml,
      "\xE6\xB5\x8B\xE8\xAF\x95\xE7\xBB\x88\xE7\xAB\xAF\xE6\x9C\xAC\xE6\x9C\xBA"
      "IP\xE5\x9C\xB0\xE5\x9D\x80",
      f.local_ip);  // 测试终端本机IP地址
  ok &= docx_detail::fill_field_after_label(
      xml,
      "\xE6\xB5\x8B\xE8\xAF\x95\xE7\xBB\x88\xE7\xAB\xAF\xE6\x9C\xAC\xE6\x9C\xBA"
      "MAC\xE5\x9C\xB0\xE5\x9D\x80",
      f.local_mac);  // 测试终端本机MAC地址
  ok &= docx_detail::fill_field_after_label(
      xml, "\xE4\xB8\xAD\xE7\xBB\xA7RELAYAPPID", f.relay_app_id);  // 中继RELAYAPPID
  ok &= docx_detail::fill_field_after_label(
      xml, "\xE4\xB8\xAD\xE7\xBB\xA7\xE4\xBB\xA3\xE7\x90\x86IP\xEF\xBC\x9A",
      f.relay_proxy_ip);  // 中继代理IP：
  ok &= docx_detail::fill_field_after_label(
      xml, "\xE4\xBA\xA4\xE6\x98\x93\xE4\xBF\xA1\xE6\x81\xAF\xE7\xB3\xBB\xE7\xBB\x9F",
      f.trading_system);  // 交易信息系统

  ok &= docx_detail::set_checkbox(
      xml, "\xE7\xA9\xBF\xE9\x80\x8F\xE5\xBC\x8F\xE7\x9B\x91\xE7\xAE\xA1",
      f.scope_penetration);  // 穿透式监管
  ok &= docx_detail::set_checkbox(
      xml, "\xE6\x8E\xA5\xE5\x8F\xA3\xE9\x80\x82\xE5\xBA\x94\xE6\x80\xA7",
      f.scope_interface);  // 接口适应性
  ok &= docx_detail::set_checkbox(
      xml,
      "\xE7\xA8\x8B\xE5\xBA\x8F\xE5\x8C\x96\xE4\xBA\xA4\xE6\x98\x93\xE7\xB3\xBB\xE7\xBB\x9F"
      "\xEF\xBC\x88\xE5\xBC\x82\xE5\xB8\xB8\xE7\x9B\x91\xE6\xB5\x8B\xE3\x80\x81\xE9\x98\x88"
      "\xE5\x80\xBC\xE7\xAE\xA1\xE7\x90\x86\xE3\x80\x81\xE9\x94\x99\xE8\xAF\xAF\xE9\x98\xB2"
      "\xE8\x8C\x83\xE3\x80\x81\xE5\xBA\x94\xE6\x80\xA5\xE5\xA4\x84\xE7\xBD\xAE\xE3\x80\x81"
      "\xE6\x97\xA5\xE5\xBF\x97\xE8\xAE\xB0\xE5\xBD\x95\xEF\xBC\x89",
      f.scope_program_trading);  // 程序化交易系统（异常监测、阈值管理、错误防范、应急处置、日志记录）
  ok &= docx_detail::set_checkbox(
      xml, "\xE6\x8E\xA5\xE5\x85\xA5\xE5\xAE\x89\xE5\x85\xA8\xE6\x80\xA7",
      f.scope_access_security);  // 接入安全性
  ok &= docx_detail::set_checkbox(
      xml,
      "\xE6\x98\xAF\xE5\x90\xA6\xE5\x85\xB7\xE5\xA4\x87\xE5\x88\x86\xE8\xB4\xA6\xE6\x88\xB7"
      "\xE5\x8A\x9F\xE8\x83\xBD",
      f.scope_sub_account);  // 是否具备分账户功能

  if (f.md5_note)
    ok &= docx_detail::insert_paragraph_after(
        xml,
        "\xE5\x8F\xAF\xE9\x80\x9A\xE8\xBF\x87\xE5\xAE\x98\xE7\xBD\x91\xE6\x8F\x90\xE4\xBE\x9B"
        "\xE7\x9A\x84MD5\xE6\xA0\xA1\xE9\xAA\x8C\xE5\xB7\xA5\xE5\x85\xB7\xE6\x9D\xA5\xE6\x9F"
        "\xA5\xE7\x9C\x8B",  // 可通过官网提供的MD5校验工具来查看
        *f.md5_note);

  if (!ok) logf("[accrep] one or more fields could not be filled — see warnings above\n");

  if (!docx_detail::write_file(doc_xml, xml)) {
    logf("[accrep] could not write the edited document.xml\n");
    fs::remove_all(work, ec);
    return false;
  }

  fs::remove(output_path, ec);
  // Compress-Archive has the same .docx-extension gate as Expand-Archive, so
  // this repacks by hand with ZipArchive directly — one entry per file,
  // named with a forward slash (Get-ChildItem -Name, not -FullName: a
  // FullName/prefix substring approach silently breaks on any ancestor
  // directory that has an 8.3 short name, since Get-ChildItem's FullName
  // and Resolve-Path's output don't always agree on which form they use).
  // OOXML/Word requires '/' in zip entry names; CreateFromDirectory (the
  // one-line alternative) emits the OS separator ('\' on Windows) instead,
  // which Word will not parse as "word/document.xml" — so this builds the
  // archive entry-by-entry instead of trusting that helper.
  const int zip_rc = run_powershell(
      L"Add-Type -AssemblyName System.IO.Compression; "
      L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
      L"$fs = New-Object System.IO.FileStream(" +
      ps_quote_w(output_path.wstring()) +
      L", [System.IO.FileMode]::Create); "
      L"$ar = New-Object System.IO.Compression.ZipArchive($fs, "
      L"[System.IO.Compression.ZipArchiveMode]::Create); "
      L"$bs=[char]92; $fwd=[char]47; "
      L"Get-ChildItem -LiteralPath " +
      ps_quote_w(extract_dir.wstring()) +
      L" -Recurse -File -Name | ForEach-Object { "
      L"$rel = $_.Replace($bs,$fwd); $full = Join-Path " +
      ps_quote_w(extract_dir.wstring()) +
      L" $_; "
      L"[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($ar, $full, $rel, "
      L"[System.IO.Compression.CompressionLevel]::Optimal) | Out-Null }; "
      L"$ar.Dispose(); $fs.Dispose()");
  fs::remove_all(work, ec);
  if (zip_rc != 0) {
    logf("[accrep] repacking the output .docx failed (exit %d)\n", zip_rc);
    return false;
  }
  logf("[accrep] wrote %s\n", wide_to_utf8(output_path.wstring()).c_str());
  return ok;
}

#endif  // _WIN32

}  // namespace plat::rep
