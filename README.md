# rmskinTESTER

rmskinTESTER is a C++ utility for Windows 10 and 11 x64. Use **Browse**, press **Ctrl+O**, or drop a Rainmeter `.rmskin` file onto the window. The file tree lists archive entries. The INI panel displays fields in package order with one field per line. Scroll right to read long values.

**Security Assessment** checks the RMSKIN marker and recorded archive length. **Skin Installer Evaluation** also checks the metadata, minimum Windows and Rainmeter versions, installer banner, and recognized components. The app looks for the installer registered for `.rmskin`, then checks the standard `Program Files` locations.

**Install…** is enabled when both checks pass. It rechecks the selected package and opens it in `SkinInstaller.exe` for confirmation. Inspection reads metadata and archive names in memory. It is a format and compatibility check. It does not authenticate the publisher, scan for malware, or verify every payload’s CRC or installation outcome.

## Install or upgrade

Download `rmskinTESTER-Setup.exe` for Windows x64. Setup defaults to `C:\Program Files\rmskinTESTER` and remembers a custom location for the next upgrade. The **Create Start menu shortcut** option is selected on a fresh install and cleared on upgrades. Clearing it leaves any existing shortcut in place. For portable use, extract the versioned ZIP and open `rmskinTESTER.exe`. [GitHub Releases](https://github.com/jsmorley/rmskinTESTER/releases)

Before upgrading, Setup closes running copies from the selected install folder. If a command-line check is still using the installed executable, let it finish and click **Retry**.

## Command line

```powershell
# Open the GUI with a package selected
.\rmskinTESTER.exe "C:\Skins\Example.rmskin"

# Validate and print the result. The console executable waits in interactive shells.
.\rmskinTESTER-cli.exe "C:\Skins\Example.rmskin"
```

The GUI executable also accepts `--check "C:\Skins\Example.rmskin"`. For scripts and redirected output, use the console executable.

| Exit code | Meaning |
| --- | --- |
| 0 | Both checks passed |
| 1 | A check failed |
| 2 | Validation could not be completed |
| 64 | Invalid command-line arguments |

## Build

Use a Visual Studio C++ toolchain and CMake 3.21 or later. Build the Windows x64 Release configuration from a developer shell.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build produces two executables in `build\Release`. Both link their C++ runtime and ZIP reader statically. To build Setup, compile `installer\rmskinTESTER.nsi` with NSIS after building the Release configuration.

The package checks follow Rainmeter’s Skin Installer source. The logo incorporates the Rainmeter droplet. The download includes the Rainmeter, zlib, and MiniZip notices in `Licenses`, and the source tree keeps them in `third_party`. [Rainmeter Skin Installer source](https://github.com/rainmeter/rainmeter/blob/master/Library/DialogInstall.cpp)
