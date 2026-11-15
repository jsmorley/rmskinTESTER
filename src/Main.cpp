#include "CommandLine.h"
#include "Package.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr auto Title = L"rmskinTESTER";
constexpr auto WindowTitle = L"rmskinTESTER " RMSKINTESTER_VERSION;
constexpr auto WindowClass = L"RmskinTesterWindow";
constexpr auto FooterTitle = L"Security Assessment";
constexpr auto EvaluationTitle = L"Skin Installer Evaluation";
constexpr int LogoSize = 72;
constexpr int BrowseId = 100;
constexpr int TreeId = 101;
constexpr int MetadataId = 102;
constexpr int InstallId = 103;
constexpr int MoreOptionsId = 104;
constexpr int DarkModeId = 105;
constexpr int ReleasesId = 106;
constexpr int SupportId = 107;
constexpr auto SettingsKey = L"Software\\rmskinTESTER";
constexpr UINT_PTR InspectTimer = 1;

int CloseForSetup(const std::filesystem::path& executable) {
    struct Request {
        const std::filesystem::path& executable;
        bool blocked = false;
    } request{executable};
    const auto visit = [](HWND window, LPARAM data) -> BOOL {
        auto& match = *reinterpret_cast<Request*>(data);
        wchar_t name[32]{};
        if (!GetClassNameW(window, name, ARRAYSIZE(name)) || std::wstring_view(name) != WindowClass) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        std::unique_ptr<void, decltype(&CloseHandle)> process(
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid), CloseHandle);
        if (!process) return TRUE;

        wchar_t image[32768];
        DWORD length = ARRAYSIZE(image);
        if (!QueryFullProcessImageNameW(process.get(), 0, image, &length)) return TRUE;
        const auto& target = match.executable.native();
        if (CompareStringOrdinal(image, static_cast<int>(length), target.c_str(), -1, TRUE) != CSTR_EQUAL) {
            std::error_code error;
            if (!std::filesystem::equivalent(image, match.executable, error)) return TRUE;
        }

        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == pid) PostMessageW(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0) match.blocked = true;
        return TRUE;
    };
    return !EnumWindows(visit, reinterpret_cast<LPARAM>(&request)) || request.blocked ? 1 : 0;
}

struct Palette {
    COLORREF background{}, panel{}, text{}, quiet{}, border{}, accent{}, onAccent{}, pressed{};
    COLORREF good{}, bad{}, neutral{}, onResult{};
    HBRUSH canvas{}, surface{};
    bool dark{}, highContrast{};

    ~Palette() {
        DeleteObject(canvas);
        DeleteObject(surface);
    }

    void Load(HWND window, std::optional<bool> darkPreference) {
        DWORD light = 1, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        HIGHCONTRASTW contrast{sizeof(contrast), 0, nullptr};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        highContrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        dark = darkPreference.value_or(light == 0) && !highContrast;
        background = dark ? RGB(25, 30, 39) : RGB(241, 245, 249);
        panel = dark ? RGB(33, 40, 52) : RGB(255, 255, 255);
        text = dark ? RGB(232, 239, 250) : RGB(27, 42, 65);
        quiet = dark ? RGB(163, 179, 200) : RGB(77, 98, 123);
        border = dark ? RGB(65, 79, 98) : RGB(203, 215, 230);
        accent = dark ? RGB(116, 192, 255) : RGB(16, 95, 166);
        onAccent = dark ? RGB(12, 30, 46) : RGB(255, 255, 255);
        pressed = dark ? RGB(81, 155, 217) : RGB(14, 74, 137);
        good = RGB(24, 121, 72);
        bad = RGB(183, 46, 53);
        neutral = RGB(88, 102, 122);
        onResult = RGB(255, 255, 255);
        if (highContrast) {
            background = panel = GetSysColor(COLOR_WINDOW);
            text = quiet = border = GetSysColor(COLOR_WINDOWTEXT);
            accent = pressed = GetSysColor(COLOR_HIGHLIGHT);
            onAccent = GetSysColor(COLOR_HIGHLIGHTTEXT);
            good = bad = neutral = accent;
            onResult = onAccent;
        }
        DeleteObject(canvas);
        DeleteObject(surface);
        canvas = CreateSolidBrush(background);
        surface = CreateSolidBrush(panel);
        const BOOL immersive = dark;
        DwmSetWindowAttribute(window, 20, &immersive, sizeof(immersive));
    }
};

void RoundedBox(HDC dc, RECT bounds, int radius, COLORREF fill, COLORREF edge) {
    const HBRUSH brush = CreateSolidBrush(fill);
    const HPEN pen = CreatePen(PS_SOLID, 1, edge);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

enum class ResultMark { Pass, Fail, Unknown };

void DrawResultMark(Gdiplus::Graphics& graphics, int x, int y, int size, ResultMark result, const Palette& palette) {
    using namespace Gdiplus;
    auto color = [](COLORREF rgb) { return Color(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)); };
    const auto fill = result == ResultMark::Pass ? palette.good :
        result == ResultMark::Fail ? palette.bad : palette.neutral;
    const SolidBrush brush(color(fill));
    graphics.FillEllipse(&brush, x, y, size, size);

    Pen pen(color(palette.onResult), size * 0.095f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    auto point = [&](float h, float v) { return PointF(x + size * h, y + size * v); };
    if (result == ResultMark::Pass) {
        const PointF check[] = {point(0.27f, 0.52f), point(0.43f, 0.68f), point(0.73f, 0.35f)};
        graphics.DrawLines(&pen, check, 3);
    } else if (result == ResultMark::Fail) {
        graphics.DrawLine(&pen, point(0.33f, 0.33f), point(0.67f, 0.67f));
        graphics.DrawLine(&pen, point(0.67f, 0.33f), point(0.33f, 0.67f));
    } else {
        graphics.DrawLine(&pen, point(0.29f, 0.5f), point(0.71f, 0.5f));
    }
}

const wchar_t* FooterDetail(Footer footer) {
    switch (footer) {
    case Footer::Valid: return L"Package format verified.";
    case Footer::Missing:
    case Footer::Invalid: return InvalidPackageMessage;
    default: return L"Package format could not be verified.";
    }
}

struct Group {
    size_t first;
    size_t end;
    size_t prefix;
};

struct App {
    HWND window{}, title{}, moreOptions{}, package{}, browse{}, install{}, footer{}, evaluation{}, footerDetail{}, evaluationDetail{};
    HWND treeTitle{}, infoTitle{}, tree{}, info{}, count{};
    HINSTANCE instance{};
    HFONT body{}, heading{}, brand{};
    HICON logo{};
    HIMAGELIST images{};
    ULONG_PTR graphicsToken{};
    IFileOpenDialog* activeFileDialog{};
    UINT dpi = 96;
    Palette palette;
    std::optional<bool> darkPreference;
    RECT openCard{}, validationCard{}, treeCard{}, infoCard{};
    int dividerX{};
    std::future<Report> inspection;
    std::filesystem::path currentPackage;
    std::filesystem::path queued;
    Report report;
    std::vector<Group> groups;
    bool hasResult = false;
    bool openInstallerWhenReady = false;

    ~App() {
        DeleteObject(body);
        DeleteObject(heading);
        DeleteObject(brand);
        if (logo) DestroyIcon(logo);
        if (images) ImageList_Destroy(images);
        if (graphicsToken) Gdiplus::GdiplusShutdown(graphicsToken);
    }

    int Px(int value) const { return MulDiv(value, dpi, 96); }

    HWND Control(const wchar_t* kind, const wchar_t* label, DWORD style, int id = 0) const {
        return CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    }

    void Theme() {
        palette.Load(window, darkPreference);
        SetWindowTheme(tree, palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SetWindowTheme(info, palette.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        TreeView_SetBkColor(tree, palette.panel);
        TreeView_SetTextColor(tree, palette.text);
        TreeView_SetLineColor(tree, palette.border);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void Scale() {
        auto font = [&](int size, int weight) {
            return CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        const auto newBody = font(16, FW_NORMAL);
        const auto newHeading = font(17, FW_SEMIBOLD);
        const auto newBrand = font(32, FW_SEMIBOLD);
        for (HWND control : {package, browse, install, footerDetail, evaluationDetail, count, tree, info})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(newBody), FALSE);
        for (HWND control : {footer, evaluation, treeTitle, infoTitle})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(newHeading), FALSE);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(newBrand), FALSE);
        DeleteObject(body);
        DeleteObject(heading);
        DeleteObject(brand);
        body = newBody;
        heading = newHeading;
        brand = newBrand;
        if (logo) DestroyIcon(logo);
        logo = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, Px(LogoSize), Px(LogoSize), 0));
        const auto newImages = ImageList_Create(Px(16), Px(16), ILC_COLOR32, 2, 0);
        for (const auto kind : {SIID_DOCNOASSOC, SIID_FOLDER}) {
            SHSTOCKICONINFO icon{};
            icon.cbSize = sizeof(icon);
            if (SUCCEEDED(SHGetStockIconInfo(kind, SHGSI_ICON | SHGSI_SMALLICON, &icon))) {
                ImageList_AddIcon(newImages, icon.hIcon);
                DestroyIcon(icon.hIcon);
            } else {
                ImageList_AddIcon(newImages, LoadIconW(nullptr, IDI_APPLICATION));
            }
        }
        TreeView_SetImageList(tree, newImages, TVSIL_NORMAL);
        if (images) ImageList_Destroy(images);
        images = newImages;
        TreeView_SetItemHeight(tree, Px(25));
        TreeView_SetIndent(tree, Px(19));
        SendMessageW(info, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Px(2), Px(8)));
    }

    void Init() {
        dpi = GetDpiForWindow(window);
        Gdiplus::GdiplusStartupInput graphicsOptions;
        Gdiplus::GdiplusStartup(&graphicsToken, &graphicsOptions, nullptr);
        title = Control(L"STATIC", Title, SS_CENTERIMAGE | SS_NOPREFIX);
        moreOptions = Control(L"BUTTON", L"More options", BS_OWNERDRAW | WS_TABSTOP, MoreOptionsId);
        package = Control(L"STATIC", L"Drop a .rmskin here or browse for a package.", SS_CENTERIMAGE | SS_PATHELLIPSIS | SS_NOPREFIX);
        browse = Control(L"BUTTON", L"&Browse", BS_OWNERDRAW | WS_TABSTOP, BrowseId);
        install = Control(L"BUTTON", L"&Install", BS_OWNERDRAW | WS_TABSTOP, InstallId);
        EnableWindow(install, FALSE);
        footer = Control(L"STATIC", FooterTitle, SS_CENTERIMAGE | SS_NOPREFIX);
        evaluation = Control(L"STATIC", EvaluationTitle, SS_CENTERIMAGE | SS_NOPREFIX);
        footerDetail = Control(L"STATIC", L"Open a package to run both checks.", SS_NOPREFIX);
        evaluationDetail = Control(L"STATIC", L"", SS_NOPREFIX);
        treeTitle = Control(L"STATIC", L"Package contents", SS_CENTERIMAGE | SS_NOPREFIX);
        count = Control(L"STATIC", L"", SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
        tree = Control(WC_TREEVIEWW, L"Package contents",
            WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_TRACKSELECT, TreeId);
        infoTitle = Control(L"STATIC", L"RMSKIN.ini", SS_CENTERIMAGE | SS_NOPREFIX);
        info = Control(L"EDIT", L"Package metadata appears here.",
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, MetadataId);
        SendMessageW(info, EM_SETLIMITTEXT, 0x7ffffffd, 0);
        DragAcceptFiles(window, TRUE);
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, SettingsKey, L"DarkMode", RRF_RT_REG_DWORD,
            nullptr, &value, &size) == ERROR_SUCCESS && value <= 1) {
            darkPreference = value != 0;
        }
        Scale();
        Theme();
        SetFocus(browse);
    }

    void Layout() {
        RECT rect{};
        GetClientRect(window, &rect);
        const int left = Px(24), right = rect.right - Px(24), gap = Px(14);
        const int width = std::max(0, right - left);
        const int split = left + (width - gap) * 55 / 100;
        const int bottom = rect.bottom - Px(76);
        const int extraResultHeight = width < Px(820) ? Px(14) : 0;
        openCard = {left, Px(86), right, Px(144)};
        validationCard = {left, Px(156), right, Px(258) + extraResultHeight};
        dividerX = left + width / 2;
        const int panelsTop = validationCard.bottom + Px(14);
        treeCard = {left, panelsTop, split, bottom};
        infoCard = {split + gap, panelsTop, right, bottom};
        auto put = [](HWND control, int x, int y, int w, int h) {
            MoveWindow(control, x, y, std::max(0, w), std::max(0, h), TRUE);
        };
        const int titleOffset = Px(LogoSize + 11);
        put(title, left + titleOffset, Px(16), width - titleOffset - Px(54), Px(54));
        put(moreOptions, right - Px(42), Px(24), Px(42), Px(38));
        put(package, left + Px(16), Px(97), width - Px(158), Px(36));
        put(browse, right - Px(126), Px(96), Px(110), Px(38));
        put(install, right - Px(110), rect.bottom - Px(62), Px(110), Px(38));
        const int titleInset = Px(hasResult ? 44 : 16);
        put(footer, left + titleInset, Px(168), dividerX - left - titleInset - Px(16), Px(25));
        put(evaluation, dividerX + titleInset, Px(168), right - dividerX - titleInset - Px(16), Px(25));
        const int detailHeight = validationCard.bottom - Px(204);
        put(footerDetail, left + Px(16), Px(199), dividerX - left - Px(32), detailHeight);
        put(evaluationDetail, dividerX + Px(16), Px(199), right - dividerX - Px(32), detailHeight);
        const int panelTitleTop = panelsTop + Px(10);
        const int treeTop = panelsTop + Px(45), infoTop = panelsTop + Px(47);
        put(treeTitle, left + Px(16), panelTitleTop, split - left - Px(145), Px(25));
        put(count, split - Px(123), panelTitleTop, Px(107), Px(25));
        put(tree, left + Px(12), treeTop, split - left - Px(24), bottom - treeTop - Px(12));
        put(infoTitle, infoCard.left + Px(16), panelTitleTop, right - infoCard.left - Px(32), Px(25));
        put(info, infoCard.left + Px(14), infoTop, right - infoCard.left - Px(26), bottom - infoTop - Px(12));
        InvalidateRect(window, nullptr, FALSE);
    }

    void Paint() const {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, palette.canvas);
        for (const auto& card : {openCard, validationCard, treeCard, infoCard}) {
            RoundedBox(dc, card, Px(16), palette.panel, palette.border);
        }
        const RECT divider{dividerX, validationCard.top + Px(12), dividerX + 1, validationCard.bottom - Px(12)};
        const HBRUSH rule = CreateSolidBrush(palette.border);
        FillRect(dc, &divider, rule);
        DeleteObject(rule);
        if (logo) DrawIconEx(dc, Px(24), Px(7), logo, Px(LogoSize), Px(LogoSize), 0, nullptr, DI_NORMAL);
        if (hasResult && graphicsToken) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const auto footerResult = report.footer == Footer::Valid ? ResultMark::Pass :
                report.footer == Footer::Unreadable ? ResultMark::Unknown : ResultMark::Fail;
            const auto evaluationResult = report.preflight == Preflight::Passed ? ResultMark::Pass :
                report.preflight == Preflight::Unverified ? ResultMark::Unknown : ResultMark::Fail;
            DrawResultMark(graphics, validationCard.left + Px(16), Px(170), Px(21), footerResult, palette);
            DrawResultMark(graphics, dividerX + Px(16), Px(170), Px(21), evaluationResult, palette);
        }
        EndPaint(window, &paint);
    }

    INT_PTR Color(HDC dc, HWND control) const {
        const bool onCanvas = control == title;
        const auto bg = onCanvas ? palette.background : palette.panel;
        auto fg = palette.text;
        if (control == package || control == footerDetail || control == evaluationDetail || control == count)
            fg = palette.quiet;
        SetTextColor(dc, fg);
        SetBkColor(dc, bg);
        return reinterpret_cast<INT_PTR>(onCanvas ? palette.canvas : palette.surface);
    }

    void DrawButton(const DRAWITEMSTRUCT& item) const {
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool menuButton = item.CtlID == MoreOptionsId;
        FillRect(item.hDC, &item.rcItem, menuButton || disabled || item.CtlID == InstallId ? palette.canvas : palette.surface);
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const auto fill = menuButton ? (pressed ? palette.background : palette.panel) :
            disabled ? palette.background : pressed ? palette.pressed : palette.accent;
        RoundedBox(item.hDC, item.rcItem, Px(10), fill, disabled || menuButton ? palette.border : palette.accent);
        RECT bounds = item.rcItem;
        if (menuButton) {
            const int x = (bounds.left + bounds.right) / 2;
            const int y = (bounds.top + bounds.bottom) / 2;
            const int thickness = std::max(1, Px(2));
            const auto ink = CreateSolidBrush(palette.text);
            for (const int offset : {-5, 0, 5}) {
                const int top = y + Px(offset) - thickness / 2;
                const RECT bar{x - Px(8), top, x + Px(8), top + thickness};
                FillRect(item.hDC, &bar, ink);
            }
            DeleteObject(ink);
        } else {
            SetBkMode(item.hDC, TRANSPARENT);
            SetTextColor(item.hDC, disabled ? palette.quiet : palette.onAccent);
            const auto prev = SelectObject(item.hDC, heading);
            DrawTextW(item.hDC, item.CtlID == InstallId ? L"&Install" : L"&Browse", -1, &bounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((item.itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
            SelectObject(item.hDC, prev);
        }
        if (item.itemState & ODS_FOCUS) {
            InflateRect(&bounds, -Px(4), -Px(4));
            DrawFocusRect(item.hDC, &bounds);
        }
    }

    void ShowMoreOptions() {
        HMENU menu = CreatePopupMenu();
        HMENU settings = CreatePopupMenu();
        if (!menu || !settings || !AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), L"&Settings")) {
            if (settings) DestroyMenu(settings);
            if (menu) DestroyMenu(menu);
            return;
        }
        const UINT flags = MF_STRING | (palette.dark ? MF_CHECKED : MF_UNCHECKED) |
            (palette.highContrast ? MF_GRAYED : MF_ENABLED);
        AppendMenuW(settings, flags, DarkModeId, L"&Dark Mode");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ReleasesId, L"&Releases");
        AppendMenuW(menu, MF_STRING, SupportId, L"Su&pport");
        RECT anchor{};
        GetWindowRect(moreOptions, &anchor);
        const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN,
            anchor.right, anchor.bottom, 0, window, nullptr);
        DestroyMenu(menu);
        if (choice == DarkModeId) {
            const DWORD value = palette.dark ? 0 : 1;
            const auto saved = RegSetKeyValueW(HKEY_CURRENT_USER, SettingsKey, L"DarkMode", REG_DWORD, &value, sizeof(value));
            darkPreference = value != 0;
            Theme();
            if (saved != ERROR_SUCCESS) {
                MessageBoxW(window, L"Could not save the dark mode setting for the next launch.", Title, MB_OK | MB_ICONWARNING);
            }
        } else if (choice == ReleasesId || choice == SupportId) {
            const wchar_t* url = choice == ReleasesId ? L"https://github.com/jsmorley/rmskinTESTER/releases" :
                L"https://forum.rainmeter.net/viewtopic.php?p=247068#p247068";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
                MessageBoxW(window, L"Could not open the page in the default browser.", Title, MB_OK | MB_ICONERROR);
            }
        }
    }

    void Browse() {
        IFileOpenDialog* raw = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) return;
        const auto release = [](auto* item) { item->Release(); };
        std::unique_ptr<IFileOpenDialog, decltype(release)> dialog(raw, release);
        const COMDLG_FILTERSPEC types[] = {{L"Rainmeter skin packages (*.rmskin)", L"*.rmskin"}};
        dialog->SetFileTypes(1, types);
        dialog->SetTitle(L"Open a Rainmeter skin package");
        dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        activeFileDialog = dialog.get();
        const HRESULT shown = dialog->Show(window);
        activeFileDialog = nullptr;
        if (FAILED(shown)) return;
        IShellItem* selected = nullptr;
        if (FAILED(dialog->GetResult(&selected))) return;
        std::unique_ptr<IShellItem, decltype(release)> result(selected, release);
        PWSTR file = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &file))) {
            std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(file, CoTaskMemFree);
            Open(path.get());
        }
    }

    void Drop(HDROP drop) {
        const auto countDropped = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (countDropped == 1) {
            const auto length = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(drop, 0, path.data(), length + 1)) {
                path.resize(length);
                Open(path);
            }
        } else if (countDropped > 1) {
            MessageBoxW(window, L"Drop one .rmskin package at a time.", Title, MB_OK | MB_ICONINFORMATION);
        }
        DragFinish(drop);
    }

    bool CanInstall() const {
        return hasResult && report.footer == Footer::Valid &&
            report.preflight == Preflight::Passed && !report.installerPath.empty();
    }

    void LaunchInstaller() const {
        const auto arguments = L"\"" + currentPackage.native() + L"\"";
        const auto directory = report.installerPath.parent_path();
        SHELLEXECUTEINFOW launch{};
        launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_FLAG_NO_UI;
        launch.hwnd = window;
        launch.lpFile = report.installerPath.c_str();
        launch.lpParameters = arguments.c_str();
        launch.lpDirectory = directory.c_str();
        launch.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&launch)) {
            MessageBoxW(window, L"Windows could not start Rainmeter Skin Installer.", Title, MB_OK | MB_ICONERROR);
        }
    }

    void Open(std::filesystem::path path, bool thenInstall = false) {
        if (inspection.valid()) {
            queued = std::move(path);
            openInstallerWhenReady = false;
            return;
        }
        std::error_code error;
        auto absolute = std::filesystem::absolute(path, error);
        currentPackage = error ? std::move(path) : std::move(absolute);
        openInstallerWhenReady = thenInstall;
        hasResult = false;
        if (GetFocus() == install) SetFocus(browse);
        EnableWindow(install, FALSE);
        Layout();
        SetWindowTextW(package, currentPackage.c_str());
        SetWindowTextW(footerDetail, L"Checking the package format…");
        SetWindowTextW(evaluationDetail, thenInstall ? L"Rechecking before opening Skin Installer…" : L"Evaluating the package…");
        SetWindowTextW(count, L"");
        SetWindowTextW(infoTitle, L"RMSKIN.ini");
        SetWindowTextW(info, L"");
        TreeView_DeleteAllItems(tree);
        groups.clear();
        report = {};
        try {
            std::packaged_task<Report()> work([path = currentPackage] {
                auto result = InspectPackage(path);
                std::ranges::sort(result.entries);
                return result;
            });
            auto future = work.get_future();
            std::thread(std::move(work)).detach();
            inspection = std::move(future);
            if (!SetTimer(window, InspectTimer, 40, nullptr)) {
                report = inspection.get();
                Present();
            }
        } catch (...) {
            report.preflight = Preflight::Unverified;
            report.reason = L"The package could not be inspected.";
            Present();
        }
    }

    void Poll() {
        if (!inspection.valid() ||
            inspection.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        KillTimer(window, InspectTimer);
        try {
            report = inspection.get();
        } catch (...) {
            report.preflight = Preflight::Unverified;
            report.reason = L"The package could not be inspected.";
        }
        if (!queued.empty()) Open(std::exchange(queued, {}));
        else Present();
    }

    void AddNodes(HTREEITEM parent, Group group) {
        // Walk immediate siblings in reverse so TVI_FIRST can insert in constant time.
        for (size_t i = group.end; i > group.first;) {
            const auto& path = report.entries[--i];
            const auto slash = path.find(L'\\', group.prefix);
            bool folder = slash != std::wstring::npos;
            size_t id = 0;
            bool children = false;
            if (folder) {
                const auto end = i + 1;
                const auto prefix = std::wstring_view(path).substr(0, slash + 1);
                const auto first = std::lower_bound(report.entries.begin() + group.first, report.entries.begin() + i, prefix);
                i = static_cast<size_t>(first - report.entries.begin());
                const auto hasEntry = std::ranges::upper_bound(
                    report.entries.begin() + i, report.entries.begin() + end, prefix);
                const auto start = static_cast<size_t>(hasEntry - report.entries.begin());
                children = start < end;
                if (children) {
                    groups.push_back({start, end, slash + 1});
                    id = groups.size();
                }
            }
            auto name = path.substr(group.prefix, slash == std::wstring::npos ? slash : slash - group.prefix);
            if (name.empty()) name = L"(empty name)";
            TVINSERTSTRUCTW node{};
            node.hParent = parent;
            node.hInsertAfter = TVI_FIRST;
            node.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN | TVIF_PARAM;
            node.item.pszText = name.data();
            node.item.iImage = node.item.iSelectedImage = folder ? 1 : 0;
            node.item.cChildren = children ? 1 : 0;
            node.item.lParam = static_cast<LPARAM>(id);
            TreeView_InsertItem(tree, &node);
        }
    }

    void Expand(const NMTREEVIEWW& event) {
        if (event.action != TVE_EXPAND || !event.itemNew.lParam || TreeView_GetChild(tree, event.itemNew.hItem)) return;
        AddNodes(event.itemNew.hItem, groups[static_cast<size_t>(event.itemNew.lParam) - 1]);
    }

    void Present() {
        hasResult = true;
        EnableWindow(install, CanInstall());
        Layout();
        SetWindowTextW(footerDetail, FooterDetail(report.footer));
        SetWindowTextW(evaluationDetail, report.reason == InvalidPackageMessage ? L"" : report.reason.c_str());
        SetWindowTextW(count, (std::to_wstring(report.entries.size()) +
            (report.entries.size() == 1 ? L" entry" : L" entries")).c_str());

        std::wstring bullets;
        for (const auto& section : report.sections) {
            if (!bullets.empty()) bullets += L"\r\n";
            if (!section.name.empty()) bullets += L"[" + section.name + L"]\r\n";
            for (const auto& field : section.fields) {
                bullets += L"•  " + field.key + L" = " + field.value + L"\r\n";
            }
        }
        if (bullets.empty()) bullets = report.metadataProblem.empty() ? L"Package metadata is unavailable." : report.metadataProblem;
        SetWindowTextW(info, bullets.c_str());
        SendMessageW(info, EM_SETSEL, 0, 0);
        SendMessageW(info, EM_SCROLLCARET, 0, 0);
        SendMessageW(tree, WM_SETREDRAW, FALSE, 0);
        AddNodes(TVI_ROOT, {0, report.entries.size(), 0});
        SendMessageW(tree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tree, nullptr, TRUE);
        if (std::exchange(openInstallerWhenReady, false) && CanInstall()) LaunchInstaller();
    }
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE: app->Init(); return 0;
    case WM_SIZE: app->Layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* bounds = reinterpret_cast<MINMAXINFO*>(lParam);
        bounds->ptMinTrackSize = {app->Px(740), app->Px(550)};
        return 0;
    }
    case WM_DPICHANGED: {
        app->dpi = HIWORD(wParam);
        app->Scale();
        const auto& rect = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        app->Layout();
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE: app->Theme(); return 0;
    case WM_PAINT: app->Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return app->Color(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
    case WM_DRAWITEM:
        if (wParam == BrowseId || wParam == InstallId || wParam == MoreOptionsId) {
            app->DrawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == MoreOptionsId) {
            app->ShowMoreOptions();
            return 0;
        }
        if (LOWORD(wParam) == BrowseId) {
            app->Browse();
            return 0;
        }
        if (LOWORD(wParam) == InstallId && app->CanInstall()) {
            app->Open(app->currentPackage, true);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->idFrom == TreeId && header->code == TVN_ITEMEXPANDINGW)
            app->Expand(*reinterpret_cast<const NMTREEVIEWW*>(lParam));
        break;
    }
    case WM_DROPFILES: app->Drop(reinterpret_cast<HDROP>(wParam)); return 0;
    case WM_TIMER:
        if (wParam == InspectTimer) app->Poll();
        return 0;
    case WM_CLOSE:
        if (app->activeFileDialog) app->activeFileDialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, InspectTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> freeArgs(args, LocalFree);
    if (count >= 2 && std::wstring_view(args[1]) == L"--close-for-setup")
        return count == 3 ? CloseForSetup(args[2]) : 64;
    if (count >= 2 && (std::wstring_view(args[1]) == L"--check" ||
        std::wstring_view(args[1]) == L"--help" || std::wstring_view(args[1]) == L"-h" ||
        std::wstring_view(args[1]) == L"/?")) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        return CheckCommandLine(count, args, std::wstring_view(args[1]) == L"--check");
    }
    if (count > 2) {
        MessageBoxW(nullptr, L"Pass one .rmskin file to open it, or use --check to validate it in the console.",
            Title, MB_OK | MB_ICONINFORMATION);
        return 64;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return 2;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    App app;
    app.instance = instance;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = WindowProcedure;
    cls.hInstance = instance;
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    cls.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = WindowClass;
    RegisterClassExW(&cls);
    const UINT dpi = GetDpiForSystem();
    const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, WindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1020, dpi, 96),
        MulDiv(690, dpi, 96), nullptr, nullptr, instance, &app);
    if (!window) {
        CoUninitialize();
        return 2;
    }
    ShowWindow(window, show);
    if (count == 2) app.Open(args[1]);
    MSG msg{};
    int result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == 'O' && (GetKeyState(VK_CONTROL) < 0)) {
            app.Browse();
            continue;
        }
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return result < 0 ? 2 : static_cast<int>(msg.wParam);
}
