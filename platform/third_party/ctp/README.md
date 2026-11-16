# CTP SDK 6.7.13 (Windows x64)

Installed from the user-supplied `v6.7.13_20260225_trader.zip` archive.
Archive SHA-256: `A15958DA251FE8D375FC66F56721EF067B2F300BC7979572E4EAF95D53CA8AE5`.

The matching trading and market-data packages contain the Windows x64
`20260225_traderapi64_se_windows` and `20260225_mduserapi64_se_windows`
directories. Their shared data-type and structure headers were verified identical.

- `include/`: the four SDK headers, converted from GBK to UTF-8 for MSVC `/utf-8`.
  The existing `.h.u8` companion copies are kept synchronized.
- `lib/`: original Windows x64 import libraries.
- `bin/`: original Windows x64 runtime DLLs, copied beside executables by CMake.
- `error.xml`, `error.dtd`: original SDK error definitions.
- `RELEASE_NOTES.txt`: vendor release notes converted from GBK to UTF-8.

This update installs the Windows SDK only. The separate legacy `ctp_cp`
SDK is unchanged. All consumers must be rebuilt against these headers and
libraries; do not mix runtime DLLs from an older SDK with this version.
