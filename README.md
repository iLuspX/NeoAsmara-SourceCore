# NeoAsmara Core : Source Code

C++ source for the NeoAsmara Windows server services. This folder contains build inputs and test sources. It has no backups, player databases, passwords, signing keys, client packages or extracted game assets.

## Programs to install

### To run the compiled Core

Use the matching **NeoAsmara server-files package** on Windows x64. It supplies the server configuration, clean SQLite databases, authenticated catalogs and runtime DLLs. Keep `libc++.dll` and `libunwind.dll` beside the server executables in `Bin`.

The C++ server uses Windows' built-in SQLite runtime. **A separate SQLite server, SQL Server, Python or .NET is not required to run the compiled C++ services.** Windows 11 IoT is the locally tested system.

### To edit, update or compile the source

| Program | What you use it for | Official download |
|---|---|---|
| **Python 3.11 or newer (64-bit)** | Required by the automatic compiler-download helper and Python analysis tools. Make sure `python --version` works in PowerShell. | [Download Python for Windows](https://www.python.org/downloads/windows/) |
| **LLVM-MinGW — UCRT x86_64** | The C++ compiler, linker and Windows import libraries used by `BUILD.cmd`. The build helper downloads and verifies the pinned release automatically when absent. | [Pinned LLVM-MinGW 20261020 download](https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip) · [Project releases](https://github.com/mstorsjo/llvm-mingw/releases) |
| **Windows PowerShell 5.1 or newer** | Runs `BUILD.ps1`. Windows PowerShell is included with Windows; PowerShell 7 is an optional alternative. | [PowerShell installation instructions](https://learn.microsoft.com/en-us/powershell/scripting/install/installing-powershell-on-windows) |
| **Visual Studio Code** | Optional editor for C++ files, JSON configuration, scripts and documentation. Install the Microsoft C/C++ extension for C++ editing support. | [Download Visual Studio Code](https://code.visualstudio.com/download) · [Microsoft C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) |
| **Git for Windows** | Optional version control for cloning the repository, reviewing changes, pulling updates and submitting contributions. | [Download Git for Windows](https://git-scm.com/downloads/win) |
| **DB Browser for SQLite** | Optional interface for inspecting a disposable database copy during development. Stop the relevant server before manually editing its database. | [Download DB Browser for SQLite](https://sqlitebrowser.org/dl/) |

**For the normal build, install Python and run `BUILD.cmd`.** The compiler is fetched automatically; Visual Studio Code, Git and DB Browser are optional tools.

### Optional tools for other workflows

| Program | When it is needed | Official download |
|---|---|---|
| **CMake 3.20 or newer** | Alternative CMake-based builds. `BUILD.cmd` does not use it. | [Download CMake](https://cmake.org/download/) |
| **Visual Studio / Build Tools for Visual Studio**, with **Desktop development with C++** and a **Windows SDK** | Alternative MSVC/CMake development. The native CMake configuration needs the Windows SDK `winsqlite3.lib`. The portable LLVM-MinGW build does not need this installation. | [Visual Studio and Build Tools downloads](https://visualstudio.microsoft.com/downloads/) · [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/) |
| **.NET Framework 4.8** | The separately distributed launcher and desktop GM Tool; it is not a dependency of the C++ Core. | [Download .NET Framework 4.8](https://dotnet.microsoft.com/en-us/download/dotnet-framework/net48) |
| **Node.js** | Optional acceleration for the KPK resource-reading helper. Its Python fallback is available without Node.js. | [Download Node.js](https://nodejs.org/en/download) |

The portable LLVM-MinGW build is the path compiled and tested for this export. The alternative MSVC/CMake workflow has not been validated here.

## Build on Windows

Double-click **BUILD.cmd** from a writable folder. Folder names and locations can change.

The first build needs Python 3.11 or newer to download the pinned LLVM-MinGW compiler. The download is checked against the SHA-256 in `tools/native_toolchain.json`. The compiler is not bundled with this source copy. Windows SDK, Visual Studio and SQL Server are not required by this build script.

If LLVM-MinGW is installed outside this source folder, run the command from the source folder and provide the installation root. It must contain `bin/x86_64-w64-mingw32-clang++.exe`:

```powershell
$toolchainRoot = Read-Host 'Path to the LLVM-MinGW installation folder'
.\BUILD.ps1 -ToolchainRoot $toolchainRoot
```

Output goes to `build/native`: KarosRuntime.dll, the four server executables, KarosDataTool, the native GM/account services, ServerControl, runtime DLLs and tests. Compilation does not deploy files or restart any server.

## Dependencies

| Dependency | Source |
|---|---|
| LLVM-MinGW/UCRT compiler | Pinned version and SHA-256 in `tools/native_toolchain.json`; fetched on first build |
| nlohmann/json 3.12.0 | Included under `third_party/nlohmann`, with its MIT license |
| SQLite C API header | Included as `include/sqlite3.h` |
| SQLite runtime | Windows `winsqlite3.dll`; the script generates its import library |
| Win32, Winsock, BCrypt, DPAPI, GDI and common controls | Windows / compiler SDK headers and import libraries |
| Python 3.11+ | Compiler setup and optional resource-analysis tests |

CMake 3.20+ is an alternative for developers with a Windows SDK. Configure with `ROSH_NATIVE_WINDOWS=ON`; set `ROSH_SQLITE_IMPORT_LIBRARY` if automatic SDK detection fails. `ROSH_NATIVE_WINDOWS=OFF` builds the older offline lab and requires a separate SQLite development package.

## Run and test

Use the matching server-files package for configuration, clean databases and authenticated game catalogs. This copy includes reviewed dungeon-data overrides; the complete authenticated game catalogs and client assets are supplied separately. Test changes in a separate server installation before replacing a working runtime.

`BUILD.ps1` runs the foundation tests. It also builds dungeon domain and packet-flow tests. Dungeon tests require authenticated content and an isolated fixture marked `GM_TEST_FIXTURE`; never point those tests at live player databases. The other tests under `tests` cover additional systems and have their own fixture requirements.

The distribution helpers are included under `tools/distribution`. `initialize.cpp` additionally requires an owner-generated `release_content_key.hpp`; private release keys are intentionally absent. The normal build does not compile that release-specific initializer.

## Install a Core patch

`BUILD.cmd` compiles the Core. It does not deploy the result. To update a server installation, use a **matching NeoAsmara server-files package** and copy the build outputs into that installation's `Bin` folder.

1. Shut down that installation with its own `STOP.cmd` and wait for the services to exit.
2. Keep the package's `Config`, `Data`, `Database`, `Bootstrap` and `Updates` folders together. Do not replace them with files from a different release.
3. Open PowerShell in this source folder and run:

```powershell
$serverRoot = Read-Host 'Full path to the matching server-files folder'
$sourceBin = Join-Path (Get-Location) 'build\native'
$targetBin = Join-Path $serverRoot 'Bin'
$files = @(
    'KarosRuntime.dll', 'LoginServer.exe', 'MainServer.exe',
    'LobbyServer.exe', 'WorldServer.exe', 'KarosDataTool.exe',
    'RoshGmService.exe', 'RoshAccountService.exe', 'ServerControl.exe',
    'libc++.dll', 'libunwind.dll'
)

if (-not (Test-Path -LiteralPath $targetBin -PathType Container)) {
    throw 'The selected folder is not a server-files installation with a Bin directory.'
}
foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceBin $file) -PathType Leaf)) {
        throw "Build output is missing: $file. Run BUILD.cmd first."
    }
}
foreach ($file in $files) {
    Copy-Item -LiteralPath (Join-Path $sourceBin $file) -Destination $targetBin -Force
}
```

4. Start that installation with its normal `START.cmd` and check `Logs` if a service fails to start.

This replaces only the listed Core binaries and runtime libraries. It does not alter player databases, configuration, encrypted catalogs, client files, launcher hashes or signatures. Save a copy of the existing `Bin` files before replacing them if you need to roll back.

### Prepare another server-files package

The source repository is not a complete server-files generator: it intentionally does not contain the encrypted `Data` catalogs, release content key, clean database seeds or the release-specific `Initialize.exe`. Those files are part of the matching server-files distribution and cannot be recreated by running `BUILD.cmd`.

To prepare another package, start from an **untouched, matching server-files release**, extract it to a new folder, and apply the patch steps above. Do not use a live server folder as the package template: its `Database` may contain player accounts and progress. Keep the original `Bootstrap\Database` seeds and the release's `Initialize.exe`; do not distribute a generated `Config\Keys\content.key.dpapi` from an initialized machine.

To make a ZIP of the prepared, clean folder before its first start:

```powershell
$packageRoot = (Resolve-Path (Read-Host 'Path to the prepared server-files folder')).Path
if (Test-Path -LiteralPath (Join-Path $packageRoot 'Config\Keys\content.key.dpapi')) {
    throw 'This folder has already been initialized. Do not publish its machine-protected content key.'
}
$zip = Join-Path (Split-Path $packageRoot -Parent) ((Split-Path $packageRoot -Leaf) + '.zip')
if (Test-Path -LiteralPath $zip) { throw "Archive already exists: $zip" }
Compress-Archive -Path (Join-Path $packageRoot '*') -DestinationPath $zip -CompressionLevel Optimal
```

Recipients extract the ZIP and run `START.cmd`; initialization creates the local protected key and preserves existing databases on later starts.

The standard build only updates Core binaries. Changes under `ReferenceOverrides` or other authenticated server data need a separate data-staging and signing step using the matching release content key; copying the DLLs alone does not install those changes. Client or launcher patches also need their own protected-file hashes and signed update publication.

There is no supported command in this source repository to build a complete server-files release from an empty folder. Use the matching server-files release as the template; do not substitute another build's catalogs, keys or database seeds.

## Dungeon review

See `docs/DUNGEON_REVIEW.md` for the Peltrok entrance correction, regression checks and remaining work. This source release does not claim complete dungeon parity or production readiness.

## In-game commands

See [IN_GAME_COMMANDS.md](docs/IN_GAME_COMMANDS.md) for command syntax, access levels, staff authentication, aliases and known command limitations. The guide reflects this local source revision; verify the installed server build before relying on commands in a published binary.

[Support NeoAsmara on Ko-fi](https://ko-fi.com/neoasmara). Donations are optional.

## License

Copyright © 2026 LuspX_

This source distribution is released under the **NeoAsmara Project License v1.1 (NAPL)**. You may use, modify, redistribute and commercially operate NeoAsmara under its terms.

- Public or commercial servers must display **`: NeoAsmara`** in their official title, launcher and primary public listings.
- Distributed source-code modifications must remain publicly available under **NAPL v1.1**.
- Preserve copyright notices, attribution headers and permanent credits in [CONTRIBUTORS.md](CONTRIBUTORS.md).
- Operators are responsible for third-party trademarks, clients and game assets.

See [LICENSE.md](LICENSE.md) for the complete terms. Third-party dependencies retain their own licenses.

**NeoAsmara is an independent, unofficial project.**
