#include "CommandLine.h"
#include "Package.h"

#include <windows.h>
#include <string_view>

namespace {

void Print(DWORD stream, std::wstring_view message) {
    const HANDLE output = GetStdHandle(stream);
    if (!output || output == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    DWORD consoleMode = 0;
    if (GetConsoleMode(output, &consoleMode)) {
        WriteConsoleW(output, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        return;
    }
    const auto size = static_cast<int>(message.size());
    const auto count = WideCharToMultiByte(CP_UTF8, 0, message.data(), size, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return;
    std::string bytes(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, message.data(), size, bytes.data(), count, nullptr, nullptr);
    WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

} // namespace

int CheckCommandLine(int count, wchar_t** args, bool hasSwitch) {
    const auto usage = L"Usage: rmskinTESTER-cli.exe <package.rmskin>\r\n"
        L"       rmskinTESTER.exe [--check] <package.rmskin>\r\n"
        L"Exit codes: 0 = valid modern package, 1 = failed, 2 = unverified, 64 = usage error.\r\n";
    const int firstPath = hasSwitch ? 2 : 1;
    if (count == firstPath + 1 && (std::wstring_view(args[firstPath]) == L"--help" ||
        std::wstring_view(args[firstPath]) == L"-h" || std::wstring_view(args[firstPath]) == L"/?")) {
        Print(STD_OUTPUT_HANDLE, usage);
        return 0;
    }
    if (count != firstPath + 1) {
        Print(STD_ERROR_HANDLE, usage);
        return 64;
    }
    try {
        const auto report = InspectPackage(args[firstPath]);
        Print(STD_OUTPUT_HANDLE, std::wstring(L"Security Assessment: ") + FooterLabel(report.footer) +
            L"\r\nSkin Installer Evaluation: " + PreflightLabel(report.preflight) +
            L"\r\n" + report.reason + L"\r\n");
        if (report.preflight == Preflight::Unverified) return 2;
        return report.footer == Footer::Valid && report.preflight == Preflight::Passed ? 0 : 1;
    } catch (...) {
        Print(STD_ERROR_HANDLE, L"The package could not be inspected.\r\n");
        return 2;
    }
}
