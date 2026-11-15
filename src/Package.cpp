#include "Package.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <unzip.h>
#include <iowin32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <share.h>
#include <string_view>

namespace {

using View = std::wstring_view;
using Bytes = std::vector<unsigned char>;

bool Equal(View a, View b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring DecodeBytes(std::string_view data, UINT encoding, DWORD flags = 0) {
    if (data.empty()) return {};
    const auto length = static_cast<int>(data.size());
    const int needed = MultiByteToWideChar(encoding, flags, data.data(), length, nullptr, 0);
    if (!needed) return {};
    std::wstring text(needed, L'\0');
    MultiByteToWideChar(encoding, flags, data.data(), length, text.data(), needed);
    return text;
}

Footer ReadFooter(FILE* file) {
    std::array<unsigned char, 16> tail{};
    if (_fseeki64(file, 0, SEEK_END) != 0) return Footer::Unreadable;
    const auto size = _ftelli64(file);
    if (size < 0) return Footer::Unreadable;
    if (size < static_cast<__int64>(tail.size())) return Footer::Missing;
    const auto offset = size - static_cast<__int64>(tail.size());
    if (_fseeki64(file, offset, SEEK_SET) != 0 ||
        fread(tail.data(), 1, tail.size(), file) != tail.size()) return Footer::Unreadable;
    if (memcmp(tail.data() + 9, "RMSKIN", 6) != 0) return Footer::Missing;
    std::uint64_t recorded = 0;
    memcpy(&recorded, tail.data(), sizeof(recorded));
    return tail.back() == 0 && recorded == static_cast<std::uint64_t>(offset) ? Footer::Valid : Footer::Invalid;
}

enum class ReadResult { Good, Bad, Limit };

ReadResult ReadEntry(unzFile zip, std::uint64_t limit, Bytes* output = nullptr) {
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) return ReadResult::Bad;
    if (info.uncompressed_size > limit) return ReadResult::Limit;
    if (unzOpenCurrentFile(zip) != UNZ_OK) return ReadResult::Bad;
    std::array<unsigned char, 16384> block{};
    std::uint64_t total = 0;
    int count = 0;
    while ((count = unzReadCurrentFile(zip, block.data(), static_cast<unsigned>(block.size()))) > 0) {
        total += count;
        if (total > limit) break;
        if (output) output->insert(output->end(), block.begin(), block.begin() + count);
    }
    const int closed = unzCloseCurrentFile(zip);
    if (total > limit) return ReadResult::Limit;
    return count == 0 && total == info.uncompressed_size && closed == UNZ_OK ? ReadResult::Good : ReadResult::Bad;
}

std::wstring DecodeIni(const Bytes& bytes) {
    if (bytes.empty()) return {};
    auto starts = [&](unsigned a, unsigned b) { return bytes.size() >= 2 && bytes[0] == a && bytes[1] == b; };
    if (starts(0xfe, 0xff)) return {};
    const bool utf16Bom = starts(0xff, 0xfe);
    int tests = IS_TEXT_UNICODE_ASCII16 | IS_TEXT_UNICODE_CONTROLS;
    const bool utf8Bom = bytes.size() >= 3 && starts(0xef, 0xbb) && bytes[2] == 0xbf;
    if (utf16Bom || (!utf8Bom && bytes.size() >= 2 && IsTextUnicode(bytes.data(), static_cast<int>(bytes.size()), &tests))) {
        if (bytes.size() % 2) return {};
        const size_t offset = utf16Bom ? 2 : 0;
        std::wstring text((bytes.size() - offset) / sizeof(wchar_t), L'\0');
        memcpy(text.data(), bytes.data() + offset, bytes.size() - offset);
        return text;
    }
    const size_t offset = utf8Bom ? 3 : 0;
    const std::string_view data(reinterpret_cast<const char*>(bytes.data() + offset), bytes.size() - offset);
    auto text = DecodeBytes(data, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (text.empty() && !utf8Bom) text = DecodeBytes(data, CP_ACP);
    return text;
}

View Trim(View value) {
    while (!value.empty() && value.front() <= L' ') value.remove_prefix(1);
    while (!value.empty() && value.back() <= L' ') value.remove_suffix(1);
    return value;
}

std::vector<Section> ReadSections(View text) {
    std::vector<Section> sections;
    // Keep the first unnamed section too, so the viewer can show keys outside a section.
    while (!text.empty()) {
        const auto end = text.find_first_of(L"\r\n");
        auto line = Trim(text.substr(0, end));
        text = end == View::npos ? View{} : text.substr(end + 1);
        if (line.empty() || line.front() == L';') continue;
        if (line.front() == L'[') {
            line.remove_prefix(1);
            sections.push_back({std::wstring(Trim(line.substr(0, line.find(L']')))), {}});
            continue;
        }
        const auto equals = line.find(L'=');
        if (equals == View::npos) continue;
        const auto key = Trim(line.substr(0, equals));
        if (key.empty() || key.find(L'\0') != View::npos) continue;
        auto value = Trim(line.substr(equals + 1));
        if (const auto null = value.find(L'\0'); null != View::npos) value = value.substr(0, null);
        if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front()) {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        if (sections.empty()) sections.push_back({});
        sections.back().fields.push_back({std::wstring(key), std::wstring(value)});
    }
    return sections;
}

View GetField(const Section* section, View key) {
    if (section) {
        for (const auto& field : section->fields) {
            if (Equal(field.key, key)) return field.value;
        }
    }
    return {};
}

int NextVersionPart(View& version) {
    const auto start = version.find_first_not_of(L'.');
    if (start == View::npos) {
        version = {};
        return 0;
    }
    version.remove_prefix(start);
    const auto end = version.find(L'.');
    const int part = _wtoi(std::wstring(version.substr(0, end)).c_str());
    version = end == View::npos ? View{} : version.substr(end + 1);
    return part;
}

bool Newer(View required, View current) {
    while (!required.empty() || !current.empty()) {
        const int a = NextVersionPart(required);
        const int b = NextVersionPart(current);
        if (a != b) return a > b;
    }
    return false;
}

std::wstring DllVersion(const std::filesystem::path& file) {
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), nullptr);
    if (!size) return {};
    Bytes buffer(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, buffer.data())) return {};
    void* translation = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", &translation, &length) ||
        length < sizeof(WORD) * 2) return {};
    const auto* language = static_cast<WORD*>(translation);
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileVersion", language[0], language[1]);
    void* value = nullptr;
    if (!VerQueryValueW(buffer.data(), key, &value, &length) || !length) return {};
    return static_cast<wchar_t*>(value);
}

struct Target {
    std::wstring version;
    std::filesystem::path installer;
    bool x86 = false;
};

Target FindRainmeter() {
    auto fromInstaller = [](const std::filesystem::path& exe) -> Target {
        DWORD binaryType = 0;
        if (!GetBinaryTypeW(exe.c_str(), &binaryType)) return {};
        return {DllVersion(exe.parent_path() / L"Rainmeter.dll"), exe, binaryType == SCS_32BIT_BINARY};
    };
    DWORD length = 0;
    AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".rmskin", L"open", nullptr, &length);
    if (length) {
        std::wstring file(length, L'\0');
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".rmskin", L"open", file.data(), &length))) {
            file.resize(wcslen(file.c_str()));
            const std::filesystem::path exe(file);
            if (Equal(exe.filename().native(), L"SkinInstaller.exe")) {
                auto target = fromInstaller(exe);
                if (!target.version.empty()) return target;
            }
        }
    }
    for (const auto* folder : {&FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86}) {
        PWSTR path = nullptr;
        const auto status = SHGetKnownFolderPath(*folder, KF_FLAG_DEFAULT, nullptr, &path);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> cleanup(path, CoTaskMemFree);
        if (FAILED(status)) continue;
        auto target = fromInstaller(std::filesystem::path(path) / L"Rainmeter" / L"SkinInstaller.exe");
        if (!target.version.empty()) return target;
    }
    return {};
}

std::wstring WindowsVersion() {
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
#pragma warning(suppress: 4996)
    if (!GetVersionExW(&info)) return {};
    return std::to_wstring(info.dwMajorVersion) + L"." + std::to_wstring(info.dwMinorVersion) +
        L"." + std::to_wstring(info.dwBuildNumber);
}

template <size_t Size>
bool IsOneOf(View text, const View (&choices)[Size]) {
    return std::ranges::any_of(choices, [&](View item) { return Equal(text, item); });
}

struct Components {
    bool independent = false;
    bool x86 = false;
    bool x64 = false;
};

void CheckComponent(const std::wstring& name, Components& found) {
    const View path(name);
    const auto firstSlash = path.find(L'\\');
    if (firstSlash == View::npos) return;
    const auto category = path.substr(0, firstSlash);
    const auto rest = path.substr(firstSlash + 1);
    const auto secondSlash = rest.find(L'\\');
    if (secondSlash == View::npos) return;
    const auto item = rest.substr(0, secondSlash);
    const auto child = rest.substr(secondSlash + 1);
    if (item.empty()) return;
    constexpr View skippedSkins[] = {L"Backup", L"@Backup", L"@Vault"};
    constexpr View skippedLayouts[] = {L"Backup", L"@Backup"};
    constexpr View builtInPlugins[] = {
        L"ActionTimer.dll", L"AdvancedCPU.dll", L"AudioLevel.dll", L"CoreTemp.dll",
        L"FileView.dll", L"FolderInfo.dll", L"InputText.dll", L"iTunesPlugin.dll",
        L"MediaKey.dll", L"NowPlaying.dll", L"PerfMon.dll", L"PingPlugin.dll",
        L"PowerPlugin.dll", L"Process.dll", L"QuotePlugin.dll", L"RecycleManager.dll",
        L"ResMon.dll", L"RunCommand.dll", L"SpeedFanPlugin.dll", L"SysInfo.dll",
        L"UsageMonitor.dll", L"WebParser.dll", L"WifiStatus.dll", L"Win7AudioPlugin.dll",
        L"WindowMessagePlugin.dll"
    };

    if ((Equal(category, L"Skins") && !IsOneOf(item, skippedSkins)) ||
        (Equal(category, L"Layouts") &&
            Equal(PathFindExtensionW(name.c_str() + firstSlash + 1 + secondSlash), L".ini") &&
            !IsOneOf(item, skippedLayouts))) {
        found.independent = true;
    }
    if (Equal(category, L"Plugins") && child.find(L'\\') == View::npos &&
        Equal(PathFindExtensionW(name.c_str() + firstSlash + 2 + secondSlash), L".dll") &&
        !IsOneOf(child, builtInPlugins)) {
        found.x86 |= Equal(item, L"32bit");
        found.x64 |= Equal(item, L"64bit");
    }
}

} // namespace

Report InspectPackage(const std::filesystem::path& path) {
    Report report;
    auto finish = [&](std::wstring reason, Preflight status = Preflight::Failed) {
        report.preflight = status;
        report.reason = std::move(reason);
        return std::move(report);
    };
    if (!Equal(path.extension().native(), L".rmskin")) return finish(L"Choose a file with the .rmskin extension.");

    // Deny concurrent writes while the footer and ZIP reader inspect the same file.
    std::unique_ptr<FILE, decltype(&fclose)> file(_wfsopen(path.c_str(), L"rb", _SH_DENYWR), fclose);
    if (!file) return finish(L"The package could not be opened.", Preflight::Unverified);
    report.footer = ReadFooter(file.get());
    zlib_filefunc64_def fileFunctions{};
    fill_win32_filefunc64W(&fileFunctions);
    std::unique_ptr<void, decltype(&unzClose)> zip(unzOpen2_64(path.c_str(), &fileFunctions), unzClose);
    if (!zip) {
        report.metadataProblem = L"The ZIP directory could not be opened.";
        return finish(report.footer == Footer::Missing || report.footer == Footer::Invalid ?
            InvalidPackageMessage : report.metadataProblem.c_str());
    }

    // Read modern metadata even when the footer is wrong, so a failed package can still be inspected.
    Bytes ini;
    ReadResult metadataRead = ReadResult::Bad, bannerRead = ReadResult::Good;
    bool hasMetadata = false;
    bool longNames = false, catalogLimit = false;
    std::array<char, 65536> filename{};
    size_t totalNameLength = 0;
    int next = unzGoToFirstFile(zip.get());
    while (next == UNZ_OK) {
        if (report.entries.size() == 150000) {
            catalogLimit = true;
            break;
        }
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(zip.get(), &info, filename.data(), static_cast<uLong>(filename.size()),
            nullptr, 0, nullptr, 0) != UNZ_OK || info.size_filename >= filename.size()) {
            next = UNZ_BADZIPFILE;
            break;
        }
        const auto length = static_cast<size_t>(std::find(filename.data(), filename.data() + info.size_filename, '\0') - filename.data());
        auto entry = DecodeBytes({filename.data(), length}, (info.flag & (1u << 11)) ? CP_UTF8 : CP_ACP);
        totalNameLength += entry.size();
        if (totalNameLength > 8 * 1024 * 1024) {
            catalogLimit = true;
            break;
        }
        std::ranges::replace(entry, L'/', L'\\');
        longNames |= entry.size() >= MAX_PATH || length >= MAX_PATH * 3 || length != info.size_filename || entry.empty();
        if (!hasMetadata && Equal(entry, L"RMSKIN.ini")) {
            hasMetadata = true;
            metadataRead = ReadEntry(zip.get(), 4 * 1024 * 1024, &ini);
        } else if (report.footer == Footer::Valid && bannerRead == ReadResult::Good && Equal(entry, L"RMSKIN.bmp")) {
            bannerRead = ReadEntry(zip.get(), 64 * 1024 * 1024);
        }
        report.entries.push_back(std::move(entry));
        next = unzGoToNextFile(zip.get());
    }

    if (hasMetadata && metadataRead == ReadResult::Good) {
        report.sections = ReadSections(DecodeIni(ini));
        if (report.sections.empty()) report.metadataProblem = L"No INI sections or fields could be read.";
    } else if (hasMetadata) {
        report.metadataProblem = metadataRead == ReadResult::Limit ? L"The metadata exceeds the tester’s 4 MiB inspection limit." :
            L"The metadata entry could not be read.";
    } else {
        report.metadataProblem = L"No RMSKIN.ini at the root of the package.";
    }

    if (report.footer == Footer::Invalid || report.footer == Footer::Missing) return finish(InvalidPackageMessage);
    if (report.footer == Footer::Unreadable) return finish(L"The package format could not be verified.", Preflight::Unverified);
    if (catalogLimit) return finish(L"The archive listing exceeds the tester’s inspection limit. A partial tree is shown.", Preflight::Unverified);
    if (next != UNZ_END_OF_LIST_OF_FILE) return finish(L"The ZIP directory contains an unreadable entry.");
    if (!hasMetadata) return finish(L"Skin Installer could not find the required metadata.");
    if (metadataRead == ReadResult::Limit) return finish(L"The package metadata exceeds the tester’s inspection limit.", Preflight::Unverified);
    if (metadataRead != ReadResult::Good) return finish(L"Skin Installer could not read the package metadata.");
    if (longNames) return finish(L"An archive path exceeds Skin Installer’s name buffer or could not be decoded.", Preflight::Unverified);

    const auto options = std::ranges::find_if(report.sections, [](const Section& section) {
        return Equal(section.name, L"rmskin");
    });
    const auto* section = options != report.sections.end() ? &*options : nullptr;
    if (GetField(section, L"Name").empty()) return finish(L"The package metadata is missing a Name.");

    const auto target = FindRainmeter();
    const auto minimumRainmeter = GetField(section, L"MinimumRainmeter");
    if (!minimumRainmeter.empty() && !target.version.empty() && Newer(minimumRainmeter, target.version)) {
        return finish(L"Requires Rainmeter " + std::wstring(minimumRainmeter) + L". Found " + target.version + L".");
    }
    const auto minimumWindows = GetField(section, L"MinimumWindows");
    const auto windows = WindowsVersion();
    if (!minimumWindows.empty() && !windows.empty() && Newer(minimumWindows, windows)) {
        return finish(L"Requires Windows " + std::wstring(minimumWindows) + L". Found " + windows + L".");
    }

    if (bannerRead == ReadResult::Limit) return finish(L"The installer banner exceeds the tester’s inspection limit.", Preflight::Unverified);
    if (bannerRead == ReadResult::Bad) return finish(L"Skin Installer could not read the installer banner.");

    Components components;
    for (const auto& name : report.entries) CheckComponent(name, components);
    if (!components.independent && !(target.version.empty() ? (components.x86 || components.x64) :
        (target.x86 ? components.x86 : components.x64))) {
        return finish(L"No installable components were found for Skin Installer.");
    }
    if (target.version.empty())
        return finish(L"Could not locate SkinInstaller.exe and Rainmeter.dll to verify this package on this PC.", Preflight::Unverified);
    if (!minimumWindows.empty() && windows.empty()) {
        return finish(L"Could not determine the Windows version.", Preflight::Unverified);
    }
    report.installerPath = target.installer;
    return finish(L"Skin Installer should open this package. Checked against Rainmeter " + target.version + L".",
        Preflight::Passed);
}

const wchar_t* FooterLabel(Footer footer) {
    switch (footer) {
    case Footer::Valid: return L"Valid";
    case Footer::Missing: return L"Missing";
    case Footer::Invalid: return L"Invalid";
    default: return L"Unreadable";
    }
}

const wchar_t* PreflightLabel(Preflight preflight) {
    switch (preflight) {
    case Preflight::Passed: return L"Passed";
    case Preflight::Failed: return L"Failed";
    default: return L"Unverified";
    }
}
