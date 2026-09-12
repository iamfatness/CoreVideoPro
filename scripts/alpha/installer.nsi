Unicode True
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "WordFunc.nsh"
!include "TextFunc.nsh"
; Uninstaller-side copies of the macros un.RemoveMediaRuntime uses.
!insertmacro un.TrimNewLines
!insertmacro un.WordFind
!include "generated.nsh"
Name "CoreVideo Pro ${RELEASE_ID}"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\CoreVideoPro\${RELEASE_ID}"
RequestExecutionLevel user
SetCompressor /SOLID lzma
Icon "${PAYLOAD}\Assets\AppIcon.ico"
UninstallIcon "${PAYLOAD}\Assets\AppIcon.ico"
VIProductVersion "0.1.0.0"
VIAddVersionKey /LANG=1033 "ProductName" "CoreVideo Pro ${CHANNEL_TITLE}"
VIAddVersionKey /LANG=1033 "FileDescription" "CoreVideo Pro ${RELEASE_ID} Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${RELEASE_ID}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "CoreVideo Pro"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideoPro-${RELEASE_ID}"
!define SHORTCUT_NAME "CoreVideo Pro ${RELEASE_ID}"
; T2.7 / #474. ONE entry point that always opens the newest install. The owner
; accumulated five version-named desktop shortcuts, launched a build from three
; days earlier by mistake, and reported a defect against code that predated its
; fix. Beta testers will do exactly the same. The version-named shortcut stays,
; but only in the Start menu.
!define STABLE_SHORTCUT_NAME "CoreVideo Pro"
!define MUI_WELCOMEPAGE_TEXT "Install this Windows ${CHANNEL} for your account.$\r$\n$\r$\nThe app is unsigned. Use it for rehearsals and testing before an irreplaceable production.$\r$\n$\r$\nSetup may request administrator approval for Microsoft's VC runtime. First app launch downloads its verified media runtime. Close this version before uninstalling."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "CoreVideo Pro is installed. Open its desktop or Start menu shortcut to begin.$\r$\n$\r$\nFirst launch requires internet access to download the verified media runtime. This ${CHANNEL} is for testing and feedback.$\r$\n$\r$\nUninstall removes delivered files and shortcuts, while preserving settings, recordings, and downloaded media."
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
        MessageBox MB_OK|MB_ICONSTOP "This ${CHANNEL} version is already registered. Uninstall it before installing again." /SD IDOK
        SetErrorLevel 1638
        Quit
    ${EndIf}
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "This ${CHANNEL} requires Windows x64." /SD IDOK
        SetErrorLevel 1633
        Quit
    ${EndIf}
    ${IfNot} ${AtLeastWin10}
        MessageBox MB_OK|MB_ICONSTOP "This ${CHANNEL} requires Windows 10 or later." /SD IDOK
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
    FileOpen $0 "$INSTDIR\.corevideo-prerelease-install" w
    IfErrors install_failed
    FileWrite $0 "${RELEASE_ID}"
    FileClose $0
    IfErrors install_failed
    CreateDirectory "$SMPROGRAMS\${SHORTCUT_NAME}"
    CreateShortcut "$SMPROGRAMS\${SHORTCUT_NAME}\CoreVideo Pro.lnk" "$INSTDIR\StartCoreVideo.cmd" "" "$INSTDIR\Assets\AppIcon.ico"
    CreateShortcut "$SMPROGRAMS\${SHORTCUT_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
    ; T2.7: the stable pair, repointed at THIS install because it is the newest.
    CreateShortcut "$DESKTOP\${STABLE_SHORTCUT_NAME}.lnk" "$INSTDIR\StartCoreVideo.cmd" "" "$INSTDIR\Assets\AppIcon.ico"
    CreateShortcut "$SMPROGRAMS\${STABLE_SHORTCUT_NAME}.lnk" "$INSTDIR\StartCoreVideo.cmd" "" "$INSTDIR\Assets\AppIcon.ico"
    ; Retire the version-named DESKTOP shortcuts this installer created in the
    ; past. Matched on our own exact naming ("CoreVideo Pro alpha-*" /
    ; "CoreVideo Pro beta-*"), never a broad "CoreVideo Pro *" glob, which would
    ; sweep up a file the operator made and named themselves.
    Delete "$DESKTOP\CoreVideo Pro alpha-*.lnk"
    Delete "$DESKTOP\CoreVideo Pro beta-*.lnk"
    ; T2.1 / #433. Register the virtual camera for this user (HKCU, no admin)
    ; so "CoreVideo Pro Camera" exists in Zoom/Teams/OBS without a manual step.
    ; Best effort: a tester whose machine refuses this still gets a working app,
    ; and Register-VirtualCamera.cmd remains in the install folder to retry.
    ; regsvr32 WITHOUT /s opens a modal result dialog, and Register-VirtualCamera.cmd
    ; deliberately omits it so a tester who double-clicks that file gets feedback.
    ; Calling the .cmd here hung a silent install forever (measured: the 180 s
    ; Test-AlphaInstaller timeout, two regsvr32 processes waiting on a dialog
    ; nobody could see). Call regsvr32 directly with /s instead. NEVER run an
    ; interactive helper from a silent installer.
    ClearErrors
    ExecWait '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\corevideo-virtualcam.dll"' $1
    ClearErrors
    ; T2.1: create the recording folder so the first Record has somewhere to go.
    ; Must match RecordingFolderPolicy.Resolve's default (Videos\CoreVideo Pro).
    CreateDirectory "$PROFILE\Videos\${STABLE_SHORTCUT_NAME}"
    ClearErrors
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
        Delete "$DESKTOP\${STABLE_SHORTCUT_NAME}.lnk"
        Delete "$SMPROGRAMS\${STABLE_SHORTCUT_NAME}.lnk"
        DeleteRegKey HKCU "${UNINSTALL_KEY}"
        Delete "$INSTDIR\.corevideo-prerelease-install"
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
        ; #474 item 3. Refusing is correct — we will not delete a folder we
        ; cannot prove we installed — but refusing SILENTLY left an operator
        ; with no Apps & Features entry and an uninstaller that appeared to do
        ; nothing. Say what is wrong and what to do about it.
        MessageBox MB_OK|MB_ICONSTOP "This folder is not registered as a CoreVideo Pro installation, so Setup will not delete it.$\r$\n$\r$\nExpected registration: $0$\r$\nThis folder: $INSTDIR$\r$\n$\r$\nIf this install is not listed in Apps & Features, close CoreVideo Pro and delete the folder by hand. Your settings and recordings are stored elsewhere." /SD IDOK
        SetErrorLevel 1603
        Abort "Installation registration does not match this folder."
    ${EndIf}
    ClearErrors
    FileOpen $0 "$INSTDIR\.corevideo-prerelease-install" r
    IfErrors invalid_install
    FileRead $0 $1
    FileClose $0
    ${If} $1 == "${RELEASE_ID}"
        Return
    ${EndIf}
    invalid_install:
        MessageBox MB_OK|MB_ICONSTOP "This folder does not carry a CoreVideo Pro ${RELEASE_ID} install marker, so Setup will not delete it.$\r$\n$\r$\nFolder: $INSTDIR$\r$\n$\r$\nIf this install is not listed in Apps & Features, close CoreVideo Pro and delete the folder by hand. Your settings and recordings are stored elsewhere." /SD IDOK
        SetErrorLevel 1603
        Abort "Installation marker is missing or incorrect."
FunctionEnd

; T2.7 / #474 item 2. First launch downloads ~195 MB of FFmpeg runtime into
; $INSTDIR, and none of it was in uninstall-files.nsh. RMDir below is
; non-recursive, so it silently failed and the whole runtime stayed behind:
; measured on the owner's machine 2026-09-12, TWO uninstalled versions were
; still holding 10 files and 195 MB each.
;
; Install-MediaRuntime.ps1 records exactly what it wrote to
; notices\ffmpeg\installed-files.txt, one relative path per line, and this
; deletes that set and nothing else. A manifest rather than a hard-coded list
; because the file set follows the pinned upstream build; a manifest rather
; than RMDir /r because $INSTDIR is an app folder that an operator's files can
; end up in, and a recursive delete there is how a show gets destroyed.
Function un.RemoveMediaRuntime
    ClearErrors
    FileOpen $0 "$INSTDIR\notices\ffmpeg\installed-files.txt" r
    IfErrors done   ; never installed, or already removed
    next:
        ClearErrors
        FileRead $0 $1
        IfErrors close
        ; Trim the trailing newline NSIS hands back with each line.
        ${un.TrimNewLines} $1 $1
        StrCmp $1 "" next
        ; Refuse anything that is not a plain relative path under $INSTDIR. The
        ; manifest is ours, but it is a FILE on disk, and a file that decides
        ; what an uninstaller deletes is worth validating.
        StrCpy $2 $1 1
        StrCmp $2 "\" next
        StrCmp $2 "/" next
        StrCpy $2 $1 2 1
        StrCmp $2 ":\" next
        ${un.WordFind} $1 ".." "E+1{" $2
        IfErrors 0 next
        Delete "$INSTDIR\$1"
        Goto next
    close:
        FileClose $0
        Delete "$INSTDIR\notices\ffmpeg\installed-files.txt"
        RMDir "$INSTDIR\notices\ffmpeg"
        RMDir "$INSTDIR\notices"
    done:
        ClearErrors
FunctionEnd

Section "Uninstall"
    ; T2.1 / #474. Install registers the virtual camera, so uninstall MUST
    ; unregister it - otherwise "CoreVideo Pro Camera" survives as a COM
    ; registration pointing at a DLL this uninstaller is about to delete, and
    ; every app that enumerates cameras inherits a broken device. Runs BEFORE
    ; the payload delete, because regsvr32 needs the DLL it is unregistering.
    ; /s for the same reason as install: an uninstaller must never wait on a
    ; dialog. Runs BEFORE the payload delete, because regsvr32 needs the DLL.
    ClearErrors
    ExecWait '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\corevideo-virtualcam.dll"' $1
    ClearErrors
    StrCpy $DeleteFailed 0
    !include "uninstall-files.nsh"
    ${If} $DeleteFailed == 1
        MessageBox MB_OK|MB_ICONSTOP "Some files are in use. Close CoreVideo Pro and run this uninstaller again. Registration and your files have been retained." /SD IDOK
        SetErrorLevel 1603
        Abort
    ${EndIf}
    Call un.RemoveMediaRuntime
    Delete "$SMPROGRAMS\${SHORTCUT_NAME}\CoreVideo Pro.lnk"
    Delete "$SMPROGRAMS\${SHORTCUT_NAME}\Uninstall.lnk"
    RMDir "$SMPROGRAMS\${SHORTCUT_NAME}"
    Delete "$DESKTOP\${SHORTCUT_NAME}.lnk"
    ; The stable shortcuts point at the install being removed, so they would
    ; become dead links. Removed, not repointed: choosing "the newest remaining
    ; release" means ranking sibling folders, and a wrong guess silently opens
    ; the wrong build — the exact failure #474 exists to stop. Installing any
    ; version recreates them.
    Delete "$DESKTOP\${STABLE_SHORTCUT_NAME}.lnk"
    Delete "$SMPROGRAMS\${STABLE_SHORTCUT_NAME}.lnk"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    Delete "$INSTDIR\.corevideo-prerelease-install"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
