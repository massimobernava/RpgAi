!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "RpgAi"
OutFile "RpgAi-Setup.exe"
InstallDir "$PROGRAMFILES64\RpgAi"
InstallDirRegKey HKLM "Software\RpgAi" "Install_Dir"
RequestExecutionLevel admin

; Icon (place rpgai.ico in installer/windows/)
!define MUI_ICON "rpgai.ico"
!define MUI_UNICON "rpgai.ico"

; Welcome/Finish page
!define MUI_WELCOMEPAGE_TITLE "Welcome to RpgAi"
!define MUI_WELCOMEPAGE_TEXT "This wizard will install RpgAi and set up your story workspace in Documents\RpgAi."
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open RpgAi workspace folder"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION OpenWorkspace

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ── Main install ─────────────────────────────────────────────────────────────
Section "RpgAi" SecMain
    SectionIn RO

    ; Install executable and launcher
    SetOutPath "$INSTDIR"
    File "rpgai.exe"
    File /nonfatal "*.dll"
    File "rpgai-launcher.bat"
    File "getting_started.html"

    ; Bundle scripts and servers inside install dir (read-only reference copy)
    SetOutPath "$INSTDIR\scripts"
    File /r "scripts\*"

    SetOutPath "$INSTDIR\servers\t2i_locale"
    File /r "t2i_locale\*"

    SetOutPath "$INSTDIR\servers\faceswap_locale"
    File /r "faceswap_locale\*"

    SetOutPath "$INSTDIR\servers\qwen_locale"
    File /r "qwen_locale\*"

    ; Create user workspace in Documents
    CreateDirectory "$DOCUMENTS\RpgAi"
    CreateDirectory "$DOCUMENTS\RpgAi\scripts"
    CreateDirectory "$DOCUMENTS\RpgAi\scripts\lib"
    CreateDirectory "$DOCUMENTS\RpgAi\servers\t2i_locale"
    CreateDirectory "$DOCUMENTS\RpgAi\servers\faceswap_locale"
    CreateDirectory "$DOCUMENTS\RpgAi\servers\qwen_locale"
    CreateDirectory "$DOCUMENTS\RpgAi\output"
    CreateDirectory "$DOCUMENTS\RpgAi\saves"

    ; Copy default scripts (only if workspace is fresh)
    IfFileExists "$DOCUMENTS\RpgAi\scripts\fantasy_demo.lua" skip_scripts 0
        CopyFiles /SILENT "$INSTDIR\scripts\*" "$DOCUMENTS\RpgAi\scripts\"
    skip_scripts:

    ; Copy Python servers (only if workspace is fresh)
    IfFileExists "$DOCUMENTS\RpgAi\servers\t2i_locale\server.py" skip_servers 0
        CopyFiles /SILENT "$INSTDIR\servers\*" "$DOCUMENTS\RpgAi\servers\"
    skip_servers:

    ; Start Menu shortcut
    CreateDirectory "$SMPROGRAMS\RpgAi"
    CreateShortcut "$SMPROGRAMS\RpgAi\RpgAi.lnk" "$INSTDIR\rpgai-launcher.bat" \
        "" "$INSTDIR\rpgai.exe" 0
    CreateShortcut "$SMPROGRAMS\RpgAi\Workspace.lnk" "$DOCUMENTS\RpgAi"
    CreateShortcut "$SMPROGRAMS\RpgAi\Uninstall.lnk" "$INSTDIR\Uninstall.exe"

    ; Desktop shortcut
    CreateShortcut "$DESKTOP\RpgAi.lnk" "$INSTDIR\rpgai-launcher.bat" \
        "" "$INSTDIR\rpgai.exe" 0

    ; Registry (for Add/Remove Programs)
    WriteRegStr HKLM "Software\RpgAi" "Install_Dir" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RpgAi" \
        "DisplayName" "RpgAi"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RpgAi" \
        "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RpgAi" \
        "DisplayVersion" "RPGAI_VERSION"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RpgAi" \
        "Publisher" "RpgAi"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RpgAi" \
        "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RpgAi" \
        "NoRepair" 1

    WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

; ── Uninstall ────────────────────────────────────────────────────────────────
Section "Uninstall"
    Delete "$INSTDIR\rpgai.exe"
    Delete "$INSTDIR\*.dll"
    Delete "$INSTDIR\rpgai-launcher.bat"
    Delete "$INSTDIR\getting_started.html"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir /r "$INSTDIR\scripts"
    RMDir /r "$INSTDIR\servers"
    RMDir "$INSTDIR"

    Delete "$DESKTOP\RpgAi.lnk"
    RMDir /r "$SMPROGRAMS\RpgAi"

    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RpgAi"
    DeleteRegKey HKLM "Software\RpgAi"

    ; Note: Documents\RpgAi is intentionally NOT deleted (user data)
SectionEnd

; ── Helpers ──────────────────────────────────────────────────────────────────
Function OpenWorkspace
    ExecShell "open" "$DOCUMENTS\RpgAi"
FunctionEnd
