# Third-party licenses

This document records the licensing analysis for components that
betl redistributes (via `deps/`) or that get statically linked into
end-user-shipped artifacts (via NativeAOT).

The audit underlying this document was performed on 2026-05-12
against released artifact versions. **Re-audit before each major
betl release** — Microsoft can change distribution terms between
.NET LTS releases, and the audit is only valid for the pinned
versions listed.

## Summary

| Component | Version audited | Source license | Distribution license | Verdict |
|---|---|---|---|---|
| `dotnet/runtime` source | v8.0.11 | MIT | n/a (source only) | ✓ Redistributable |
| `dotnet/sdk` source | v8.0.404 | MIT | n/a (source only) | ✓ Redistributable |
| .NET 8 SDK Linux x64 tarball | 8.0.404 | n/a | **MIT** | ✓ Redistributable |
| NativeAOT compiler (`Microsoft.DotNet.ILCompiler`) | 8.0.11 | MIT | MIT (NuGet) | ✓ Redistributable |
| NativeAOT output binaries | n/a | n/a | MIT (statically linked) | ✓ Redistributable |
| `ICSharpCode.CodeConverter` (NuGet) | 9.2.7.792 | MIT | MIT (NuGet) | ✓ Redistributable (conversion-time only) |
| `cJSON` (vendored under `vendor/cjson/`) | 1.7.15 | MIT | MIT (statically compiled into `betl_core`) | ✓ Redistributable |

**Net**: every artifact betl needs to ship or that flows downstream
into end-user pipelines is permissively licensed (MIT, with some
embedded BSD / Apache 2.0 / ISC / Zlib third-party components — all
permissive). No GPL/LGPL/AGPL/MPL code is present in shipped binaries.

## Source-of-truth references

- [dotnet/core license-information.md](https://github.com/dotnet/core/blob/main/license-information.md)
  — Microsoft's authoritative summary. **Critical text**:
  > "On Linux and macOS: MIT license"
  > "Binaries produced by .NET SDK compilers (C#, F#, VB) can be
  > redistributed without additional restrictions. The only restrictions
  > are based on the license of the compiler inputs used to produce
  > the binary."
- [dotnet/core LICENSE.TXT](https://github.com/dotnet/core/blob/main/LICENSE.TXT)
  — the MIT text referenced for Linux/macOS product distributions.
- [dotnet/runtime LICENSE.TXT](https://github.com/dotnet/runtime/blob/v8.0.11/LICENSE.TXT)
  — runtime source MIT.
- [dotnet/sdk LICENSE.TXT](https://github.com/dotnet/sdk/blob/v8.0.404/LICENSE.TXT)
  — SDK source MIT.

## Platform scope

**Linux x64 only** for v0.2. The redistribution license is platform-
specific:

- **Linux + macOS**: MIT (per Microsoft's `license-information.md`).
- **Windows**: ".NET Library License" — different terms, more
  restrictive, **explicitly not redistributable** in the same way.
  See [`license-information-windows.md`](https://github.com/dotnet/core/blob/main/license-information-windows.md).

When we add Windows / macOS support later we MUST audit each platform
separately. For Windows in particular, the betl distribution model
will likely have to be "user installs .NET themselves" rather than
"betl ships .NET in deps/".

## Artifact-level verification (2026-05-12)

The `dotnet-sdk-8.0.404-linux-x64.tar.gz` tarball was pulled from
`https://dotnetcli.azureedge.net/dotnet/Sdk/8.0.404/` and inspected:

```
sha256 of tarball:    5bf340ba6acb314c703c2492a3a6e1530d7cdbfd3b5bf86788ee8b4afefd3573
tarball size:         204 MB
file count:           5467 entries
license-bearing files (4):
  ./LICENSE.txt                                                 → MIT
  ./ThirdPartyNotices.txt                                       → permissive (see below)
  ./shared/Microsoft.AspNetCore.App/8.0.11/THIRD-PARTY-NOTICES.txt
  ./sdk/8.0.404/Sdks/Microsoft.NET.Sdk.WindowsDesktop/LICENSE.TXT → MIT
  ./sdk/8.0.404/Sdks/Microsoft.NET.Sdk.WindowsDesktop/THIRD-PARTY-NOTICES.TXT
  ./sdk/8.0.404/Sdks/NuGet.Build.Tasks.Pack/NOTICES.txt
```

### Third-party license families in shipped notices

Audited by grepping for license-family names across all four notice
files. License families that appear:

| Family | Mentions (SDK root + AspNetCore + NuGet) | Permissive? |
|---|---|---|
| MIT | 406 | ✓ |
| Apache 2.0 | 47 | ✓ (requires attribution) |
| BSD (2-clause + 3-clause) | 13 | ✓ |
| ISC | 113 | ✓ |
| Zlib | 5 | ✓ |
| Public Domain | 6 | n/a |

### Copyleft hits — investigated and cleared

A regex scan for `GPL|LGPL|AGPL|GNU General|GNU Lesser|GNU Affero|
Mozilla Public License|Common Development|Sun Public` returned hits
in two files. Both were investigated and confirmed to be **attribution-
chain disclosures, not shipped GPL/LGPL code**:

1. `shared/Microsoft.AspNetCore.App/8.0.11/THIRD-PARTY-NOTICES.txt:420`
   — context is the Valgrind `memcheck.h` header, **itself BSD-licensed**.
   The notices file mentions that the rest of Valgrind is GPL only as
   context; that other Valgrind code is **not shipped** in the SDK.
   Only the BSD-licensed `memcheck.h` is included for debug builds.

2. `sdk/8.0.404/Sdks/NuGet.Build.Tasks.Pack/NOTICES.txt:473`
   — context is the "Morfologik" upstream project that uses LGPL and
   Creative Commons ShareAlike for its source dictionary data. The
   actual data shipped in NuGet is from a different source (Morfeusz
   / SGJP) and is **BSD-licensed**. The LGPL mention is upstream-
   attribution transparency, not code that ships.

## NativeAOT (build-time compiler) licensing

The base SDK tarball does *not* include the NativeAOT compiler (`ilc`).
Instead, the SDK installs it on-demand at first AOT publish via the
`Microsoft.DotNet.ILCompiler` NuGet package.

This means betl's compile-cache build flow will:
1. Invoke `dotnet publish -p:PublishAot=true`.
2. SDK transparently downloads `Microsoft.DotNet.ILCompiler` to the
   user's NuGet cache.
3. Compiles the script to a self-contained `.so` artifact.

`Microsoft.DotNet.ILCompiler` 8.0.11 `nuspec` was inspected:

```xml
<license type="expression">MIT</license>
<licenseUrl>https://licenses.nuget.org/MIT</licenseUrl>
<requireLicenseAcceptance>false</requireLicenseAcceptance>
```

→ MIT, no acceptance gate. The NuGet package and the IL→native compiler
inside it are MIT-licensed.

## NativeAOT output binaries (downstream user concern)

NativeAOT output is a **self-contained native binary** that statically
includes parts of the .NET runtime. The redistribution status of these
binaries is governed by:

> "Binaries produced by .NET SDK compilers (C#, F#, VB) can be
> redistributed without additional restrictions."
> — Microsoft, `license-information.md`

So when end users ship betl pipelines that include `dotnet.script`
or `dotnet.task` steps:

- The compiled `.so` artifact (`~/.cache/betl/dotnet/<hash>.so`) can
  be redistributed by the user freely.
- The runtime bits inside that artifact carry the MIT license.
- We do not need to obtain any special license for end users.

## Required attribution

Because the SDK tarball is MIT-licensed and we redistribute it via
`deps/`, betl needs to:

1. Include `LICENSE.txt` (the MIT text) **alongside** the redistributed
   tarball in `deps/dotnet/`.
2. Include `ThirdPartyNotices.txt` (Microsoft's notices document).
3. Reference both in betl's top-level `NOTICE` (TBD — create this file
   in v0.2 alongside the dotnet provider work).

A `deps/dotnet/LICENSE.txt` symlink/copy preserved through any
re-bundling is sufficient.

## NOT redistributable from Microsoft

Things we explicitly **do not** ship and that would change the analysis
if we did:

- The **Windows** .NET binary distribution (covered by the .NET
  Library License, not MIT).
- **Visual Studio** and Visual-Studio-specific tooling (separate
  Microsoft commercial license).
- **WPF / Windows Forms** runtime packs beyond what the WindowsDesktop
  SDK source MIT covers — we should never need these for ETL.
- **Power BI / SSRS / SSAS** assemblies — separate Microsoft commercial.
- Any **NuGet package** we choose to bundle in `deps/` must be
  individually audited. The default `Microsoft.*` packages are MIT
  but third-party NuGet packages span every license family.

## Audit checklist for v0.3 and beyond

Before each release that pins a new .NET version, re-run this
checklist:

- [ ] Pull the new SDK Linux x64 tarball, record SHA256.
- [ ] Extract `LICENSE.txt`. Confirm verbatim MIT text.
- [ ] Extract `ThirdPartyNotices.txt`. Grep for `GPL|LGPL|AGPL|MPL|EPL|
      Mozilla|proprietary`. Investigate any hits with surrounding
      context.
- [ ] Inspect `dotnet/core/license-information.md` on the corresponding
      release branch. Confirm Linux distribution license is still MIT.
- [ ] Inspect `Microsoft.DotNet.ILCompiler` nuspec for the matching
      version. Confirm `<license type="expression">MIT</license>`.
- [ ] Update this document with the new version pins.

## ICSharpCode.CodeConverter (VB.NET → C# translator)

Used at **conversion time** by `betl-dtsx2yaml` to rewrite SSIS
Script Task / Script Component VB.NET source into C# the dotnet.task
/ dotnet.script runtime can compile. The library is a NuGet
dependency of `tools/betl-dtsx2yaml`, published with the converter
binary via `dotnet publish` (framework-dependent, so the DLLs ship
alongside `Betl.Dtsx2Yaml.dll`).

**Not a runtime dependency.** End-user pipelines never load this code
— it runs once at DTSX→YAML conversion time on the operator's
machine. Generated YAML and downstream NativeAOT artifacts have no
trace of it.

### Versions and licenses (audited 2026-05-12)

`ICSharpCode.CodeConverter 9.2.7.792` (last 9.x; pinned for net8
compatibility — the 10.x line replaced `System.Linq.Async` with
`System.Linq.AsyncEnumerable`, which pulls a .NET-10-only assembly):

| Source | License | Verified via |
|---|---|---|
| GitHub `icsharpcode/CodeConverter` LICENSE | MIT | Verbatim MIT text, "Copyright (c) 2017-2020 AlphaSierraPapa for the CodeConverter team" |
| NuGet package | MIT (`licenses.nuget.org/MIT`) | NuGet flat-container nuspec |

### Transitive dependencies (declared in 9.2.7.792 .nuspec, .NETStandard2.0)

All MIT-licensed; verified individually on nuget.org:

- `Microsoft.CSharp` 4.7.0 — MIT (.NET runtime)
- `Microsoft.CodeAnalysis.CSharp.Features` 4.1.0 — MIT (Roslyn)
- `Microsoft.CodeAnalysis.CSharp.Workspaces` 4.1.0 — MIT (Roslyn)
- `Microsoft.CodeAnalysis.VisualBasic.Workspaces` 4.1.0 — MIT (Roslyn)
- `Microsoft.VisualBasic` 10.3.0 — MIT (.NET runtime)
- `Microsoft.VisualStudio.Composition` 16.9.20 — MIT (MEFv3, Microsoft)
- `Microsoft.VisualStudio.Threading` 16.10.56 — MIT (Microsoft)
- `System.Data.DataSetExtensions` 4.5.0 — MIT (.NET runtime)
- `System.Globalization.Extensions` 4.3.0 — MIT (.NET runtime)
- `System.IO.Abstractions` 13.2.33 — MIT (TestableIO)
- `System.Linq.Async` 4.0.0 — MIT (.NET Foundation / Reactive.NET team)
- `System.Text.Encodings.Web` 8.0.0 — MIT (.NET runtime)
- `System.Threading.Tasks.Dataflow` 5.0.0 — MIT (.NET runtime)

The only **non-Microsoft, non-.NET-runtime** packages in the closure
are `ICSharpCode.CodeConverter` itself (AlphaSierraPapa / SharpDevelop),
`System.IO.Abstractions` (TestableIO), and `System.Linq.Async`
(Reactive Extensions / .NET Foundation). All MIT.

### Explicit Roslyn pin in `Betl.Dtsx2Yaml.csproj`

CodeConverter 9.2.7.792 declares `>= 4.1.0` on the Roslyn packages.
NuGet's default resolution floats this up to the highest version at
restore time (currently 4.14.x), which has hard refs to BCL
assemblies (`System.Collections.Immutable 9.0.0`,
`System.Reflection.Metadata 9.0.0`) that aren't in the .NET 8 BCL —
the publish output ends up missing them and Roslyn throws
`FileNotFoundException` at converter startup. The csproj pins
Roslyn at `4.8.0` (the version that shipped with .NET 8) to stay
within the .NET 8 BCL world:

- `Microsoft.CodeAnalysis.CSharp.Features` 4.8.0 — MIT
- `Microsoft.CodeAnalysis.CSharp.Workspaces` 4.8.0 — MIT
- `Microsoft.CodeAnalysis.VisualBasic.Workspaces` 4.8.0 — MIT

Same MIT license; the version pin only affects which point release
ships, not the license terms.

### Attribution requirement

MIT requires the copyright notice + permission text to travel with
binary copies. When publishing the dtsx2yaml tool, the relevant
LICENSE files must appear in the published `out/` directory. The
NuGet restore stage already populates a `runtimes/.../licenses` /
`/legal` directory alongside each DLL; we just ensure those aren't
stripped at packaging time.

When we add a `NOTICE` file to the betl distribution (v0.2 deliverable),
it should reference:
- `ICSharpCode.CodeConverter` © 2017-2020 AlphaSierraPapa for the CodeConverter team
- `System.IO.Abstractions` © Tatham Oddie / TestableIO

## Audit log

| Date | Auditor | Versions pinned | Result |
|---|---|---|---|
| 2026-05-12 | Initial v0.2 audit | .NET 8.0.404 SDK / 8.0.11 runtime / 8.0.11 ILCompiler | ✓ Clear to redistribute on Linux |
| 2026-05-12 | v0.2 VB.NET converter audit | `ICSharpCode.CodeConverter 10.0.1.923` + transitive deps | ✓ Clear (all MIT, conversion-time use) |
