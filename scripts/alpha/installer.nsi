Unicode True
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "WordFunc.nsh"
!include "generated.nsh"
Name "CoreVideo Pro ${RELEASE_ID}"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\CoreVideoPro\${RELEASE_ID}"
RequestExecutionLevel user
SetCompressor /SOLID lzma
Icon "${PAYLOAD}\Assets\AppIcon.ico"
UninstallIcon "${PAYLOAD}\Assets\AppIcon.ico"
VIProductVersion "0.1.0.0"
VIAddVersionKey /LANG=1033 "ProductName" "CoreVideo Pro Alpha"
VIAddVersionKey /LANG=1033 "FileDescription" "CoreVideo Pro ${RELEASE_ID} Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${RELEASE_ID}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "CoreVideo Pro"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideoPro-${RELEASE_ID}"
!define SHORTCUT_NAME "CoreVideo Pro ${RELEASE_ID}"
!define MUI_WELCOMEPAGE_TEXT "Install this experimental Windows alpha for your account.$\r$\n$\r$\nThe app is unsigned. Every-frame 60 fps and video-flashing fixes are still under investigation.$\r$\n$\r$\nSetup may request administrator approval for Microsoft's VC runtime. First app launch downloads its verified media runtime. Close this version before uninstalling."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "CoreVideo Pro is installed. Open its desktop or Start menu shortcut to begin.$\r$\n$\r$\nFirst launch requires internet access to download the verified media runtime. This alpha is for testing and feedback.$\r$\n$\r$\nUninstall removes delivered files and shortcuts, while preserving settings, recordings, and downloaded media."
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
Var RuntimeReady
Var DeleteFailed

!macro DeleteOwned relative
    ${If} ${FileExists} "$INSTDIR\${relative}"
        ClearErrors
        Delete "$INSTDIR\${relative}"
        ${If} ${Errors}
            StrCpy $DeleteFailed 1
        ${EndIf}
    ${EndIf}
!macroend

Function .onInit
    SetShellVarContext current
    SetRegView 64
    ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $0 != ""
        MessageBox MB_OK|MB_ICONSTOP "This alpha version is already registered. Uninstall it before installing again." /SD IDOK
        SetErrorLevel 1638
        Quit
    ${EndIf}
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "This alpha requires Windows x64." /SD IDOK
        SetErrorLevel 1633
        Quit
    ${EndIf}
    ${IfNot} ${AtLeastWin10}
        MessageBox MB_OK|MB_ICONSTOP "This alpha requires Windows 10 or later." /SD IDOK
        SetErrorLevel 1633
        Quit
    ${EndIf}
    IfFileExists "$INSTDIR\*.*" 0 empty_destination
        MessageBox MB_OK|MB_ICONSTOP "This version's destination already contains files. Uninstall it first, or use a new destination. Setup will not overwrite an existing installation." /SD IDOK
        SetErrorLevel 1638
        Quit
    empty_destination:
FunctionEnd

Function CheckRuntime
    StrCpy $RuntimeReady 0
    ; VC registration can be in either registry view depending on redist version.
    SetRegView 64
    ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
    ReadRegStr $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Version"
    Call CompareRuntime
    ${If} $RuntimeReady == 0
        SetRegView 32
        ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
        ReadRegStr $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Version"
        Call CompareRuntime
    ${EndIf}
    SetRegView 64
FunctionEnd

Function CompareRuntime
    ${If} $0 == 1
        StrCpy $2 $1 1
        ${If} $2 == "v"
            StrCpy $1 $1 "" 1
        ${EndIf}
        ${VersionCompare} $1 "${VC_MINIMUM}" $2
        ${If} $2 != 2
            StrCpy $RuntimeReady 1
        ${EndIf}
    ${EndIf}
FunctionEnd

Section "CoreVideo Pro" SEC_APP
    Call CheckRuntime
    ${If} $RuntimeReady == 0
        IfSilent prerequisite_failed
        InitPluginsDir
        SetOutPath "$PLUGINSDIR"
        File /oname=vc_redist.x64.exe "${VC_REDIST}"
        ClearErrors
        ExecShellWait "runas" "$PLUGINSDIR\vc_redist.x64.exe" "/install /passive /norestart" SW_SHOWNORMAL
        IfErrors prerequisite_failed
        ; Waiting is not proof of success. Require a sufficient installed runtime.
        Call CheckRuntime
        ${If} $RuntimeReady == 0
            Goto prerequisite_failed
        ${EndIf}
    ${EndIf}
    SetOverwrite off
    ClearErrors
    !include "install-files.nsh"
    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    IfErrors install_failed
    FileOpen $0 "$INSTDIR\.corevideo-alpha-install" w
    IfErrors install_failed
    FileWrite $0 "${RELEASE_ID}"
    FileClose $0
    IfErrors install_failed
    CreateDirectory "$SMPROGRAMS\${SHORTCUT_NAME}"
    CreateShortcut "$SMPROGRAMS\${SHORTCUT_NAME}\CoreVideo Pro.lnk" "$INSTDIR\StartCoreVideo.cmd" "" "$INSTDIR\Assets\AppIcon.ico"
    CreateShortcut "$SMPROGRAMS\${SHORTCUT_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
    CreateShortcut "$DESKTOP\${SHORTCUT_NAME}.lnk" "$INSTDIR\StartCoreVideo.cmd" "" "$INSTDIR\Assets\AppIcon.ico"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "CoreVideo Pro ${RELEASE_ID}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${RELEASE_ID}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "CoreVideo Pro"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\CoreVideoPro.WinUI.exe"
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" ${INSTALL_KB}
    IfErrors install_failed
    Goto installed
    install_failed:
        ; This was an empty, unregistered destination. Roll back only our exact
        ; payload/metadata, never recursively remove files created by the user.
        !include "uninstall-files.nsh"
        Delete "$SMPROGRAMS\${SHORTCUT_NAME}\CoreVideo Pro.lnk"
        Delete "$SMPROGRAMS\${SHORTCUT_NAME}\Uninstall.lnk"
        RMDir "$SMPROGRAMS\${SHORTCUT_NAME}"
        Delete "$DESKTOP\${SHORTCUT_NAME}.lnk"
        DeleteRegKey HKCU "${UNINSTALL_KEY}"
        Delete "$INSTDIR\.corevideo-alpha-install"
        Delete "$INSTDIR\Uninstall.exe"
        RMDir "$INSTDIR"
        MessageBox MB_OK|MB_ICONSTOP "Setup could not complete. Check free disk space and folder permissions before trying again. Any files that could not be removed remain in $INSTDIR." /SD IDOK
        SetErrorLevel 1603
        Abort
    prerequisite_failed:
        MessageBox MB_OK|MB_ICONSTOP "Microsoft Visual C++ x64 runtime ${VC_MINIMUM} or newer is required. Install it with administrator approval, then run Setup again. Silent installation requires the runtime to be installed beforehand." /SD IDOK
        SetErrorLevel 1603
        Abort
    installed:
SectionEnd

Function un.onInit
    SetShellVarContext current
    SetRegView 64
    ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $0 != $INSTDIR
        SetErrorLevel 1603
        Abort "Installation registration does not match this folder."
    ${EndIf}
    ClearErrors
    FileOpen $0 "$INSTDIR\.corevideo-alpha-install" r
    IfErrors invalid_install
    FileRead $0 $1
    FileClose $0
    ${If} $1 == "${RELEASE_ID}"
        Return
    ${EndIf}
    invalid_install:
        SetErrorLevel 1603
        Abort "Installation marker is missing or incorrect."
FunctionEnd

Section "Uninstall"
    StrCpy $DeleteFailed 0
    !include "uninstall-files.nsh"
    ${If} $DeleteFailed == 1
        MessageBox MB_OK|MB_ICONSTOP "Some files are in use. Close CoreVideo Pro and run this uninstaller again. Registration and your files have been retained." /SD IDOK
        SetErrorLevel 1603
        Abort
    ${EndIf}
    Delete "$SMPROGRAMS\${SHORTCUT_NAME}\CoreVideo Pro.lnk"
    Delete "$SMPROGRAMS\${SHORTCUT_NAME}\Uninstall.lnk"
    RMDir "$SMPROGRAMS\${SHORTCUT_NAME}"
    Delete "$DESKTOP\${SHORTCUT_NAME}.lnk"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    Delete "$INSTDIR\.corevideo-alpha-install"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
