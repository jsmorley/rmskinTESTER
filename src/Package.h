#pragma once

#include <filesystem>
#include <string>
#include <vector>

enum class Footer { Unreadable, Missing, Invalid, Valid };
enum class Preflight { Failed, Passed, Unverified };
inline constexpr auto InvalidPackageMessage =
    L"Invalid .rmskin package. It may be corrupted or may not have been created by Rainmeter Skin Packager.";

struct Field {
    std::wstring key;
    std::wstring value;
};

struct Section {
    std::wstring name;
    std::vector<Field> fields;
};

struct Report {
    Footer footer = Footer::Unreadable;
    Preflight preflight = Preflight::Failed;
    std::wstring reason;
    std::wstring metadataProblem;
    std::filesystem::path installerPath;
    std::vector<std::wstring> entries;
    std::vector<Section> sections;
};

Report InspectPackage(const std::filesystem::path& path);
const wchar_t* FooterLabel(Footer footer);
const wchar_t* PreflightLabel(Preflight preflight);
