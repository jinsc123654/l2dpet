@echo off
rem ============================================================================
rem  windows_build.bat - build l2dpet (the Live2D desktop pet) and assemble a
rem  ready-to-run folder. Windows / MSVC.
rem
rem  Counterpart of scripts/linux_build.sh: that one produces tar.gz/deb/AppImage,
rem  this one produces dist\windows-x64\ (two exes + assets + icons + the bundled
rem  sample model). Copy the folder anywhere and double-click l2dtest.exe to run - nothing is
rem  written to the registry.
rem
rem  What it does:
rem    1) Assemble the Cubism SDK build tree (Core + Framework, inject the software
rem       renderer + stb_image). The Windows Core libs live under per-toolset
rem       subdirectories, so pick the 143 static-CRT one and lay it out at the flat path the
rem       player CMakeLists expects.
rem    2) Build l2dtest.exe (the pet, with the built-in agent; static libcurl is
rem       fetched and built once so the LLM works out of the box).
rem    3) Build l2dpet-ui.exe (the panel: tray / context menu / settings, EUI-NEO).
rem    4) Assemble and self-check dist\windows-x64\.
rem
rem  Usage (from cmd.exe; no "x64 Native Tools" prompt needed - the script uses the
rem  Visual Studio generator, which finds the toolchain itself):
rem      scripts\windows_build.bat                  build + assemble
rem      scripts\windows_build.bat --skip-build     assemble from existing exes
rem      scripts\windows_build.bat --clean          wipe build dirs and dist first
rem      scripts\windows_build.bat --help
rem
rem  Requirements:
rem    - Visual Studio (MSVC + Windows SDK) and CMake 3.14 or newer.
rem    - Cubism SDK unpacked: set the CSM_SDK env var, or put it under
rem        %USERPROFILE%\l2d-build\live2d-sdk\
rem      (download from live2d.com/en/sdk/download/native/; unpack to CubismSdkForNative-*)
rem    - Nothing else: EUI-NEO and the GLFW/glad/freetype sources ship inside this
rem      repo (live2d\3rd\EUI-NEO), already carrying the project's local changes.
rem      No git required; first player build downloads curl sources once.
rem
rem  Batch gotcha, for whoever edits this next: inside an if/for parenthesised block, an
rem  unescaped ( or ) in echoed text breaks cmd's paren counting - the block then runs
rem  unconditionally and its output disappears. Keep block messages paren-free.
rem
rem  Env vars: CSM_SDK (SDK unpack dir), L2D_WORK (build root, default %USERPROFILE%\l2d-build)
rem ============================================================================
setlocal EnableExtensions DisableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "WORK=%USERPROFILE%\l2d-build"
if defined L2D_WORK set "WORK=%L2D_WORK%"
set "TREE=%WORK%\sdk-tree"
set "OUT=%ROOT%\dist\windows-x64"
set "SKIP_BUILD=0"
set "FORCE_SYNC=0"
set "PET_EXE=%WORK%\build-win\Release\l2dtest.exe"
set "UI_EXE=%WORK%\build-panel-win\Release\l2dpet-ui.exe"

:parse
if "%~1"=="" goto parsed
if /I "%~1"=="--skip-build" ( set "SKIP_BUILD=1" & shift & goto parse )
if /I "%~1"=="--clean"      ( set "FORCE_SYNC=1" & shift & goto parse )
if /I "%~1"=="-h"           goto usage
if /I "%~1"=="--help"       goto usage
echo [x] unknown argument: %~1  (use --help)
exit /b 2

:usage
findstr /b "rem" "%~f0"
exit /b 0

:parsed
if "%SKIP_BUILD%:%FORCE_SYNC%"=="1:1" (
  echo [x] --clean and --skip-build cannot be combined.
  exit /b 2
)
where cmake >nul 2>nul || goto fail
if "%FORCE_SYNC%"=="1" (
  if exist "%WORK%\build-win" (rmdir /S /Q "%WORK%\build-win" || goto fail)
  if exist "%WORK%\build-panel-win" (rmdir /S /Q "%WORK%\build-panel-win" || goto fail)
)
echo.
echo == options ==
echo   repo      : "%ROOT%"
echo   build dir : "%WORK%"
echo   output    : "%OUT%"

rem Repack existing exes without requiring or modifying the SDK tree.
if "%SKIP_BUILD%"=="1" goto skipbuild

rem -------------------------------------------------------------- find SDK
set "CSM="
if defined CSM_SDK if exist "%CSM_SDK%\Core" set "CSM=%CSM_SDK%"
if not defined CSM for /d %%D in ("%WORK%\live2d-sdk\CubismSdkForNative-*") do if exist "%%D\Core" set "CSM=%%D"
if not defined CSM (
  echo [x] Cubism SDK not found. Set CSM_SDK, or unpack it under "%WORK%\live2d-sdk\"
  exit /b 1
)
echo   Cubism SDK: "%CSM%"

rem -------------------------------------------------------------- 1. build tree
echo.
echo == assemble build tree ==
if not exist "%TREE%" mkdir "%TREE%"

rem Skip the SDK sync when the tree is already usable. xcopy refreshes the mtime of every
rem copied file, so re-syncing makes MSBuild rebuild the whole Framework each run (minutes
rem of nothing). Everything the sync produces lives under this one block; --clean forces it.
set "NEED_SYNC=0"
if not exist "%TREE%\Framework\src" set "NEED_SYNC=1"
rem .crt-mt marks "the Core step already ran on this tree" - trees synced before the /MT
rem switch must run it once more, otherwise the flat lib is the _MD one (no longer fatal:
rem CMake prefers the Core vendored in the repo, but keep the tree consistent anyway).
if not exist "%TREE%\Core\lib\windows\x86_64\.crt-mt" set "NEED_SYNC=1"
if defined L2D_FORCE_SYNC set "NEED_SYNC=1"
if "%FORCE_SYNC%"=="1" set "NEED_SYNC=1"

if "%NEED_SYNC%"=="0" (
  echo [i] build tree already in place - skipping SDK sync [--clean forces a re-sync]
  goto :tree_ready
)

rem NOTE: these sync steps stay at top level on purpose. Wrapping them in an if-block made
rem cmd expand the loop body at parse time, so CORESRC came out empty and the Core lib copy
rem ran as: copy /Y "" "...Live2DCubismCore.lib" (the build failed right there).
echo [i] sync Core + Framework
rem The repo ships its own Core (live2d\3rd\CubismCore) and CMake prefers that one, so the
rem Core copy below is only a fallback for trees built from an external SDK without one.
if exist "%TREE%\Framework" (rmdir /S /Q "%TREE%\Framework" || goto fail)
xcopy "%CSM%\Framework" "%TREE%\Framework" /E /I /Y /Q || goto fail
if not exist "%CSM%\Core" if exist "%ROOT%\live2d\3rd\CubismCore\lib\windows\x86_64\Live2DCubismCore_MT.lib" goto :core_done
if exist "%TREE%\Core" (rmdir /S /Q "%TREE%\Core" || goto fail)
xcopy "%CSM%\Core" "%TREE%\Core" /E /I /Y /Q || goto fail

rem Windows Core static libs are under per-toolset subdirs; CMake wants the flat path.
rem Prefer the _MT (static CRT) variant: both exes link with /MT so they carry no VC++
rem runtime dependency - the _MD lib would conflict with a /MT target (LNK2038).
set "CORESRC="
for %%T in (143 142 141) do (
  if not defined CORESRC if exist "%CSM%\Core\lib\windows\x86_64\%%T\Live2DCubismCore_MT.lib" set "CORESRC=%CSM%\Core\lib\windows\x86_64\%%T\Live2DCubismCore_MT.lib"
)
if not defined CORESRC ( echo [x] Missing Windows x64 static-CRT Core library: Live2DCubismCore_MT.lib & exit /b 1 )
if not exist "%TREE%\Core\lib\windows\x86_64" mkdir "%TREE%\Core\lib\windows\x86_64"
copy /Y "%CORESRC%" "%TREE%\Core\lib\windows\x86_64\Live2DCubismCore.lib" >nul || goto fail
echo [i] Core lib: "%CORESRC%"
:core_done
if not exist "%TREE%\Core\lib\windows\x86_64" mkdir "%TREE%\Core\lib\windows\x86_64"
echo mt> "%TREE%\Core\lib\windows\x86_64\.crt-mt"

if not exist "%TREE%\third_party" mkdir "%TREE%\third_party"
if not exist "%TREE%\third_party\stb_image.h" for /r "%CSM%\Samples" %%F in (stb_image.h) do if not exist "%TREE%\third_party\stb_image.h" copy /Y "%%F" "%TREE%\third_party\stb_image.h" >nul
if not exist "%TREE%\third_party\stb_image.h" (
  echo [x] could not extract stb_image.h from the SDK Samples
  exit /b 1
)

:tree_ready
rem Always refresh the software renderer from the repo (it is project code, and only 5 files)
echo [i] inject software renderer
if exist "%TREE%\Framework\src\Rendering\Software" (rmdir /S /Q "%TREE%\Framework\src\Rendering\Software" || goto fail)
xcopy "%ROOT%\live2d\player\soft-renderer" "%TREE%\Framework\src\Rendering\Software" /E /I /Y /Q >nul || goto fail

rem -------------------------------------------------------------- 2. build
set "PET_EXE=%WORK%\build-win\Release\l2dtest.exe"
set "UI_EXE=%WORK%\build-panel-win\Release\l2dpet-ui.exe"

echo.
echo == build the pet: l2dtest (fetches+builds static libcurl on first run) ==
cmake -S "%ROOT%\live2d\player" -B "%WORK%\build-win" -A x64 ^
      -DCSM_BUILD_ROOT="%TREE%" ^
      -DL2D_3RD_DIR="%ROOT%\live2d\3rd\EUI-NEO\3rd" ^
      -DCMAKE_BUILD_TYPE=Release || goto fail
rem --parallel N: project-level parallelism (MSBuild /m). /p:MultiProcessorCompilation=true also lets cl
rem compile the files *within* one project in parallel - the vendored libs (curl/EUI) are one big project each.
cmake --build "%WORK%\build-win" --config Release --parallel %NUMBER_OF_PROCESSORS% -- /p:MultiProcessorCompilation=true || goto fail

echo.
echo == build the panel: l2dpet-ui (EUI-NEO) ==
cmake -S "%ROOT%\live2d\panel" -B "%WORK%\build-panel-win" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release -DEUI_DEPS_MODE=auto ^
      -DEUI_NEO_DIR="%ROOT%\live2d\3rd\EUI-NEO" || goto fail
cmake --build "%WORK%\build-panel-win" --config Release --target l2dpet-ui --parallel %NUMBER_OF_PROCESSORS% -- /p:MultiProcessorCompilation=true || goto fail
goto haveexe

:skipbuild
echo.
echo == skipping build (--skip-build) ==

:haveexe
if not exist "%PET_EXE%" ( echo [x] missing "%PET_EXE%" & exit /b 1 )
if not exist "%UI_EXE%"  ( echo [x] missing "%UI_EXE%"  & exit /b 1 )

rem -------------------------------------------------------------- 3. assemble
echo.
echo == assemble "%OUT%" ==
rem The exes are locked while the pet is running, and a mid-build "access denied" at the very
rem end is confusing (everything compiled fine). Detect it up front and say what to do.
tasklist /FI "IMAGENAME eq l2dtest.exe" 2>nul | findstr /I /C:"l2dtest.exe" >nul && (
  echo [x] l2dtest.exe is running - quit the pet first, then re-run this script.
  echo     The exes in the output folder are locked while it runs, so they cannot be replaced.
  exit /b 1
)
tasklist /FI "IMAGENAME eq l2dpet-ui.exe" 2>nul | findstr /I /C:"l2dpet-ui.exe" >nul && (
  echo [x] l2dpet-ui.exe is running - quit the pet/panel, then re-run this script.
  exit /b 1
)
if exist "%OUT%" (rmdir /S /Q "%OUT%" || goto fail)
mkdir "%OUT%" || goto fail
copy /Y "%PET_EXE%" "%OUT%\l2dtest.exe" >nul || goto fail
copy /Y "%UI_EXE%"  "%OUT%\l2dpet-ui.exe" >nul || goto fail
copy /Y "%ROOT%\live2d\panel\l2dpet.png" "%OUT%\l2dpet.png" >nul || goto fail
copy /Y "%ROOT%\live2d\panel\l2dpet.ico" "%OUT%\l2dpet.ico" >nul || goto fail
rem developer logo shown on the About page (the panel finds it next to the exe)
copy /Y "%ROOT%\live2d\panel\l2dpet-logo.png" "%OUT%\l2dpet-logo.png" >nul || goto fail
xcopy "%WORK%\build-panel-win\Release\assets" "%OUT%\assets" /E /I /Y /Q >nul || goto fail

rem external Hook / MCP adapter (Python 3.9+ required only for integrations)
if not exist "%OUT%\tools" mkdir "%OUT%\tools"
copy /Y "%ROOT%\live2d\tools\pet_bridge.py" "%OUT%\tools\pet_bridge.py" >nul || goto fail
rem the doc goes BOTH next to the exe (that is where the pet looks for it and publishes
rem ~/.config/l2dpet/integration/README.md from) and into tools\ (human-readable copy)
copy /Y "%ROOT%\EXTERNAL-INTEGRATION.md" "%OUT%\EXTERNAL-INTEGRATION.md" >nul || goto fail
copy /Y "%ROOT%\EXTERNAL-INTEGRATION.md" "%OUT%\tools\EXTERNAL-INTEGRATION.md" >nul || goto fail

rem bundled sample model (Live2D Hiyori, Free Material License) so an empty model library still starts
if exist "%ROOT%\live2d\bundled-models" (
  xcopy "%ROOT%\live2d\bundled-models" "%OUT%\models" /E /I /Y /Q >nul || goto fail
) else (
  echo [warn] live2d\bundled-models missing - no sample model in the package
)

rem launcher

> "%OUT%\README.txt" echo l2dpet - Live2D desktop pet (Windows portable)
>>"%OUT%\README.txt" echo.
>>"%OUT%\README.txt" echo   l2dtest.exe      double-click to start the pet (also launches the panel l2dpet-ui.exe)
>>"%OUT%\README.txt" echo   l2dtest.exe      the pet itself: l2dtest.exe MODEL 0 0.0333 --pet --ctl 23333
>>"%OUT%\README.txt" echo   l2dpet-ui.exe    the panel: tray / context menu / settings
>>"%OUT%\README.txt" echo   models\          model library (one folder per model, containing x.model3.json); ships with Hiyori
>>"%OUT%\README.txt" echo.
>>"%OUT%\README.txt" echo   Config and memory live in %%USERPROFILE%%\.config\l2dpet\ and %%USERPROFILE%%\.local\share\l2dpet\
>>"%OUT%\README.txt" echo   LLM is optional: fill in an API key in the panel (Status and Settings - Chat)
>>"%OUT%\README.txt" echo.
>>"%OUT%\README.txt" echo   No install, no dependencies: both exes ship with the static CRT (/MT) and the
>>"%OUT%\README.txt" echo   Cubism Core linked in - they only use DLLs that Windows itself provides
>>"%OUT%\README.txt" echo   (kernel32/user32/gdi32/opengl32/ws2_32...). Nothing to install, nothing in the registry.

rem -------------------------------------------------------------- 4. self-check
echo.
echo == self-check ==
set "BAD=0"
if exist "%OUT%\l2dtest.exe"   (echo   [ok] l2dtest.exe)   else (echo   [x] l2dtest.exe missing & set "BAD=1")
if exist "%OUT%\l2dpet-ui.exe" (echo   [ok] l2dpet-ui.exe) else (echo   [x] l2dpet-ui.exe missing & set "BAD=1")
if exist "%OUT%\assets\Font Awesome 7 Free-Solid-900.otf" (echo   [ok] panel icon font) else (echo   [x] panel icon font missing & set "BAD=1")
if exist "%OUT%\l2dpet.png" (echo   [ok] window icon l2dpet.png) else (echo   [x] window icon missing & set "BAD=1")
if exist "%OUT%\l2dpet.ico"    (echo   [ok] tray icon l2dpet.ico) else (echo   [x] l2dpet.ico missing & set "BAD=1")
if exist "%OUT%\l2dpet-logo.png" (echo   [ok] about-page logo) else (echo   [x] l2dpet-logo.png missing & set "BAD=1")
rem next to the exe = the copy the pet publishes as integration\README.md (tools\ is just for humans)
if exist "%OUT%\EXTERNAL-INTEGRATION.md" (echo   [ok] external-integration doc) else (echo   [x] EXTERNAL-INTEGRATION.md missing next to the exe & set "BAD=1")
if exist "%OUT%\models\Hiyori\Hiyori.model3.json" (echo   [ok] bundled sample model Hiyori) else (echo   [x] sample model missing & set "BAD=1")
if not "%BAD%"=="0" ( echo [x] self-check failed & exit /b 1 )

rem ------------------------------------------------- 5. portable single-file exe
rem Pack the folder above into ONE self-extracting exe: double-click -> unpacks to
rem %LOCALAPPDATA%\l2dpet\ -> launches the pet. On a blank Windows that single file is
rem all you need: no VC++ runtime (static CRT), nothing written to the registry.
rem (Keep this block ASCII-only: cmd parses .bat bytes in the OEM codepage and mangles
rem  UTF-8 text, sometimes splitting a line into a bogus "command".)
set "SINGLE=%ROOT%\dist\l2dpet-portable-x64.exe"
set "PVER="
if exist "%ROOT%\version.txt" for /f "usebackq delims=" %%V in ("%ROOT%\version.txt") do set "PVER=%%V"
if not defined PVER set "PVER=0"
echo.
echo == portable single-file exe ==
if exist "%WORK%\build-portable" (rmdir /S /Q "%WORK%\build-portable" || goto fail)
cmake -S "%ROOT%\live2d\tools\portable" -B "%WORK%\build-portable" -A x64 ^
      -DPORTABLE_PAYLOAD_DIR="%OUT%" -DPORTABLE_APP_VERSION="%PVER%" ^
      -DPORTABLE_ICON="%OUT%\l2dpet.ico" -DPORTABLE_OUT="%SINGLE%" || goto fail
cmake --build "%WORK%\build-portable" --config Release --target l2dpet_portable || goto fail
if not exist "%SINGLE%" ( echo [x] portable exe not produced: "%SINGLE%" & exit /b 1 )
for %%F in ("%SINGLE%") do echo   [ok] %%~nxF  %%~zF bytes
echo   [i] first run unpacks to %%LOCALAPPDATA%%\l2dpet\portable-* ; later runs are instant

echo.
echo == output ==
dir /b "%OUT%"
echo.
echo [ok] ready-to-run folder: "%OUT%"
echo      double-click "%OUT%\l2dtest.exe" to start  [no flags needed]
echo [ok] single-file exe    : "%SINGLE%"
echo      (one file, no runtime deps, no install - copy it anywhere and double-click)
exit /b 0

:fail
echo.
echo [x] build failed (see the error above)
exit /b 1
