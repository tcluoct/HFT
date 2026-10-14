#pragma once
// plat/rep/shot_render.hpp — renders a captured text excerpt (platd log
// lines, `certutil` output, a directory listing, ...) as a terminal-style
// PNG, for the screenshot placeholders in the 附件95-3 access report that a
// live test session actually produces evidence for.
//
// This is NOT a substitute for a real screen capture of an actual test
// session — it's a faithful re-rendering of real captured output (accrep
// never fabricates the text itself), used only because a live test here runs
// headless. Same PowerShell/GDI+ shell-out approach as the docx repacking in
// docx_fill.hpp, for the same reason: battle-tested platform code, not a
// hand-rolled image encoder, for a report generated a few times a year.

#ifdef _WIN32

#include <filesystem>
#include <optional>
#include <string>

#include "plat/core/log.hpp"
#include "plat/rep/win_paths.hpp"
#include "plat/rep/winproc.hpp"

namespace plat::rep {

struct ShotDims {
  int width_px = 0;
  int height_px = 0;
};

namespace shot_detail {

inline bool write_utf8(const std::filesystem::path& p, const std::string& text) {
  std::FILE* f = nullptr;
#ifdef _WIN32
  f = _wfopen(p.wstring().c_str(), L"wb");
#else
  f = std::fopen(p.string().c_str(), "wb");
#endif
  if (!f) return false;
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  return true;
}

inline bool read_dims(const std::filesystem::path& p, ShotDims& out) {
  std::FILE* f = nullptr;
#ifdef _WIN32
  f = _wfopen(p.wstring().c_str(), L"rb");
#else
  f = std::fopen(p.string().c_str(), "rb");
#endif
  if (!f) return false;
  char buf[64] = {};
  const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = '\0';
  int w = 0, h = 0;
  if (std::sscanf(buf, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) return false;
  out.width_px = w;
  out.height_px = h;
  return true;
}

}  // namespace shot_detail

// `title` is drawn as a header line above `body` (both plain UTF-8 text,
// `body` may be multi-line). Writes a PNG to `out_png`. Returns the pixel
// dimensions on success; nullopt (logged) on failure.
inline std::optional<ShotDims> render_text_shot(const std::string& title,
                                                 const std::string& body,
                                                 const std::filesystem::path& out_png) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(out_png.parent_path(), ec);

  const fs::path txt_path = fs::path(out_png).replace_extension(".src.txt");
  const fs::path dims_path = fs::path(out_png).replace_extension(".dims.txt");
  std::error_code rm_ec;
  fs::remove(dims_path, rm_ec);

  std::string full = title;
  full += "\n\n";
  full += body;
  if (!shot_detail::write_utf8(txt_path, full)) {
    logf("[accrep] could not write screenshot source text: %s\n",
         wide_to_utf8(txt_path.wstring()).c_str());
    return std::nullopt;
  }

  const std::wstring script =
      L"Add-Type -AssemblyName System.Drawing; "
      L"$text = [System.IO.File]::ReadAllText(" + ps_quote_w(txt_path.wstring()) + L", [System.Text.Encoding]::UTF8); "
      L"$lines = $text -split '\\r?\\n'; "
      L"$fontTitle = New-Object System.Drawing.Font('Microsoft YaHei', 15, [System.Drawing.FontStyle]::Bold); "
      L"$fontBody = New-Object System.Drawing.Font('Microsoft YaHei', 14, [System.Drawing.FontStyle]::Regular); "
      L"$bmpTmp = New-Object System.Drawing.Bitmap(1,1); "
      L"$g = [System.Drawing.Graphics]::FromImage($bmpTmp); "
      L"$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit; "
      L"$pad = 22; "
      L"$lineHeight = [int]([Math]::Ceiling($fontBody.GetHeight($g))) + 6; "
      L"$titleHeight = [int]([Math]::Ceiling($fontTitle.GetHeight($g))) + 14; "
      L"$maxWidth = 0.0; "
      L"foreach ($l in $lines) { $f = if ($l -eq $lines[0]) { $fontTitle } else { $fontBody }; "
      L"  $sz = $g.MeasureString($l, $f); if ($sz.Width -gt $maxWidth) { $maxWidth = $sz.Width } }; "
      L"$g.Dispose(); $bmpTmp.Dispose(); "
      L"$width = [int]([Math]::Ceiling($maxWidth)) + $pad*2; "
      L"$height = $titleHeight + (($lines.Count - 1) * $lineHeight) + $pad*2; "
      L"$bmp = New-Object System.Drawing.Bitmap($width, $height); "
      L"$g = [System.Drawing.Graphics]::FromImage($bmp); "
      L"$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit; "
      L"$g.Clear([System.Drawing.Color]::FromArgb(255,30,30,30)); "
      L"$brushTitle = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,120,200,255)); "
      L"$brushBody = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,220,220,220)); "
      L"$y = [float]$pad; "
      L"$g.DrawString($lines[0], $fontTitle, $brushTitle, [float]$pad, $y); "
      L"$y += $titleHeight; "
      L"for ($i = 1; $i -lt $lines.Count; $i++) { $g.DrawString($lines[$i], $fontBody, $brushBody, [float]$pad, $y); $y += $lineHeight }; "
      L"$g.Dispose(); "
      L"$bmp.Save(" + ps_quote_w(out_png.wstring()) + L", [System.Drawing.Imaging.ImageFormat]::Png); "
      L"$bmp.Dispose(); "
      L"[System.IO.File]::WriteAllText(" + ps_quote_w(dims_path.wstring()) +
      L", ([string]$width + 'x' + [string]$height))";

  const int rc = run_powershell(script);
  fs::remove(txt_path, rm_ec);
  if (rc != 0) {
    logf("[accrep] screenshot render failed (exit %d): %s\n", rc,
         wide_to_utf8(out_png.wstring()).c_str());
    fs::remove(dims_path, rm_ec);
    return std::nullopt;
  }

  ShotDims dims;
  if (!shot_detail::read_dims(dims_path, dims)) {
    logf("[accrep] screenshot rendered but dims missing: %s\n",
         wide_to_utf8(out_png.wstring()).c_str());
    fs::remove(dims_path, rm_ec);
    return std::nullopt;
  }
  fs::remove(dims_path, rm_ec);
  return dims;
}

}  // namespace plat::rep

#endif  // _WIN32
