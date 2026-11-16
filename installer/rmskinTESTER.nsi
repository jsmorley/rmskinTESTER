Unicode True
!include "MUI2.nsh"
!include "x64.nsh"
!include "Sections.nsh"

!define PRODUCT "rmskinTESTER"
!define VERSION "1.2.0"
!define APP_KEY "Software\${PRODUCT}"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"

Name "${PRODUCT}"
OutFile "..\build\rmskinTESTER-Setup.exe"
InstallDir "$PROGRAMFILES64\${PRODUCT}"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT} Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright rmskinTESTER contributors"

!define MUI_ICON "..\src\RmskinTester.ico"
!define MUI_UNICON "..\src\RmskinTester.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckDestination
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${PRODUCT}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchTester
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var LastDestination

Section "${PRODUCT}"
    SectionIn RO
    SetShellVarContext current
    Call CheckExecutables

    ClearErrors
    SetOutPath "$INSTDIR"
    File "..\build\Release\rmskinTESTER.exe"
    File "..\build\Release\rmskinTESTER-cli.exe"
    File "..\README.md"
    File "..\Manual.md"
    SetOutPath "$INSTDIR\Licenses"
    File "..\third_party\Rainmeter-LICENSE.txt"
    File /oname=zlib-LICENSE.txt "..\third_party\zlib\LICENSE"
    File /oname=MiniZip-LICENSE.Info-Zip.txt "..\third_party\zlib\minizip\LICENSE.Info-Zip"
    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    IfErrors installFailed

    WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\rmskinTESTER.exe"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    IfErrors 0 installed

installFailed:
    MessageBox MB_OK|MB_ICONSTOP "Setup could not finish installing ${PRODUCT}." /SD IDOK
    SetErrorLevel 1
    Quit
installed:
SectionEnd

Section "Create Start menu shortcut" StartMenuShortcut
    ClearErrors
    CreateShortcut "$SMPROGRAMS\${PRODUCT}.lnk" "$INSTDIR\rmskinTESTER.exe"
    IfErrors 0 created
    MessageBox MB_OK|MB_ICONEXCLAMATION "${PRODUCT} was installed, but Setup could not create the Start menu shortcut." /SD IDOK
    SetErrorLevel 1
created:
SectionEnd

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "${PRODUCT} requires 64-bit Windows." /SD IDOK
        Abort
    ${EndIf}
    SetShellVarContext current
    Call CheckDestination
FunctionEnd

Function CheckDestination
    ; Reevaluate when the install path changes. Going Back keeps the user's selection.
    StrCmp $LastDestination $INSTDIR done
    StrCpy $LastDestination $INSTDIR
    IfFileExists "$INSTDIR\rmskinTESTER.exe" upgrade
    ReadRegStr $0 HKCU "${APP_KEY}" "InstallDir"
    StrCmp $0 "" 0 upgrade
    !insertmacro SelectSection ${StartMenuShortcut}
    Goto done
upgrade:
    !insertmacro UnselectSection ${StartMenuShortcut}
done:
FunctionEnd

Function LaunchTester
    ; Start through the desktop shell so the tester follows the signed-in user's session.
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\rmskinTESTER.exe"'
FunctionEnd

Function CloseRunningTester
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    ClearErrors
    File /oname=rmskinTESTER-SetupCheck.exe "..\build\Release\rmskinTESTER.exe"
    IfErrors done
    ExecWait '"$PLUGINSDIR\rmskinTESTER-SetupCheck.exe" --close-for-setup "$INSTDIR\rmskinTESTER.exe"' $0
done:
FunctionEnd

!macro CheckWritable prefix
Function ${prefix}CheckExecutables
retry:
    IfFileExists "$INSTDIR\rmskinTESTER.exe" 0 checkConsole
    !if "${prefix}" == ""
        Call CloseRunningTester
    !endif
    System::Call 'kernel32::CreateFileW(w "$INSTDIR\rmskinTESTER.exe", i 0x40000000, i 7, p 0, i 3, i 0, p 0) p .r0'
    StrCmp $0 -1 blocked
    System::Call 'kernel32::CloseHandle(p r0)'
checkConsole:
    IfFileExists "$INSTDIR\rmskinTESTER-cli.exe" 0 done
    System::Call 'kernel32::CreateFileW(w "$INSTDIR\rmskinTESTER-cli.exe", i 0x40000000, i 7, p 0, i 3, i 0, p 0) p .r0'
    StrCmp $0 -1 blocked
    System::Call 'kernel32::CloseHandle(p r0)'
    Goto done
blocked:
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Close ${PRODUCT} and let any command-line checks finish, then click Retry." /SD IDCANCEL IDRETRY retry
    SetErrorLevel 1
    Quit
done:
FunctionEnd
!macroend

!insertmacro CheckWritable ""
!insertmacro CheckWritable "un."

Section "Uninstall"
    SetShellVarContext current
    Call un.CheckExecutables
    Delete "$SMPROGRAMS\${PRODUCT}.lnk"
    Delete "$INSTDIR\rmskinTESTER.exe"
    Delete "$INSTDIR\rmskinTESTER-cli.exe"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\Manual.md"
    Delete "$INSTDIR\Licenses\Rainmeter-LICENSE.txt"
    Delete "$INSTDIR\Licenses\zlib-LICENSE.txt"
    Delete "$INSTDIR\Licenses\MiniZip-LICENSE.Info-Zip.txt"
    RMDir "$INSTDIR\Licenses"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    DeleteRegKey HKCU "${APP_KEY}"
SectionEnd
