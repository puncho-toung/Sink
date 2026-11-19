@echo off
rem //puncho - SB0: builds Steam Audio 4.8.1's phonon.dll without Intel IPP for the acoustic bake (WP-SB0).
rem
rem Usage:  build_phonon.cmd [all ^| release ^| measure ^| p0001 ^| control ^| deps]
rem   all      (default) deps, control, p0001, measure, release; the outputs are copied here at the very end
rem   release  patches 0001 + 0002; every check; phonon_test; then the outputs are copied into this folder
rem   measure  0001 + 0002 configured with -DIPL_LOCAL_MEASURE=1: phonon_measure.dll (seed export, finalizeJob
rem            timer, patch level with bit 31). Used by TOOLS/sb0/sb0bake only. NEVER copied into this folder.
rem   p0001    0001 only: the control whose patch level is 0x1
rem   control  no patch: the control with no iplLocalPatchLevel
rem   deps     fetch the five dependencies once (network) and archive them, so later builds need no network
rem
rem Paths: the pristine tree claudespace/steam-audio-4.8.1 is never edited (R-SB0-1). Each variant is a fresh copy
rem of its core/ in claudespace/steam-audio-build/<variant>/core, patched there, built in <variant>/build; the DLLs
rem end in <variant>/bin. The dependencies are fetched by Steam Audio's own get_dependencies.py, at the commits its
rem core/build/dependencies.json pins, into steam-audio-build/deps-archive.
rem
rem On any stop this folder keeps its previous contents: the copy into it runs only after every check passed.
rem
rem THE PATCH-LEVEL RULE (R-SB0-13). The engine's loader (tools/compilers/dmap/acoustic_phonon.cpp) accepts only a
rem DLL whose iplLocalPatchLevel() returns exactly PHONON_PATCH_LEVEL, 0x3 (bit 0 = 0001, bit 1 = 0002). A change
rem to a patch, or a rebuilt DLL that changes bake output (another compiler, a dependency, a Steam Audio update),
rem raises SND_BAKER_REVISION_B in the same commit; toolVersion stays Steam Audio's version. A library update: new
rem pristine tree, new SOURCE.txt, re-apply, rebuild, phonon_test, M-SB0-3 and M-SB0-6; it lands only when both pass.
rem
rem CMake 4.x refuses the cmake_minimum_required of two pinned dependencies (flatbuffers, pffft: below 3.5). For
rem those only, the fetch sets CMAKE_POLICY_VERSION_MINIMUM (CMake's environment form of
rem -DCMAKE_POLICY_VERSION_MINIMUM); see :deps. flatbuffers (a host tool, flatc) takes 3.5. pffft and mysofa take
rem 3.15: below it policy CMP0091 is OLD, CMake ignores the CMAKE_MSVC_RUNTIME_LIBRARY that dependencies.json passes,
rem the library is built /MD, and the /MT phonon.dll fails to link (mysofa: __imp_fgetc and four more). zlib and
rem catch need nothing. Steam Audio's own core/ needs no such setting.

setlocal EnableExtensions EnableDelayedExpansion

for %%I in ("%~dp0.") do set "LIBS=%%~fI"
for %%I in ("%~dp0..\..\..\..") do set "ROOT=%%~fI"
set "PRISTINE=%ROOT%\steam-audio-4.8.1"
set "WORK=%ROOT%\steam-audio-build"
set "ARCHIVE=%WORK%\deps-archive"
set "PY=%ROOT%\TOOLS\python\python.exe"
set "HELPER=%LIBS%\tools\phonon_build.py"
set "PATCHES=%LIBS%\patches"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"

if not exist "%PRISTINE%\core\CMakeLists.txt" ( echo build_phonon: no pristine tree at %PRISTINE% & exit /b 1 )
if not exist "%PY%" ( echo build_phonon: no Python at %PY% & exit /b 1 )

rem --- the compiler's environment (dumpbin, cl) ---
rem (a shell started from MSYS bash drops the variable ProgramFiles(x86), hence the fixed fallback)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%I in (`call "!VSWHERE!" -version 17 -latest -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR ( echo build_phonon: Visual Studio 2022 not found & exit /b 1 )
rem (vcvars64 looks for vswhere.exe on PATH and says so when it is not there; harmless, so its output is dropped)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where dumpbin >nul 2>&1 || ( echo build_phonon: vcvars64 failed, no dumpbin & exit /b 1 )

if not exist "%WORK%" mkdir "%WORK%"
echo build_phonon: %MODE% start %DATE% %TIME%

if /i "%MODE%"=="deps" ( call :deps & goto :end )
call :deps || goto :fail
if /i "%MODE%"=="control" ( call :variant control "" 0 & goto :end )
if /i "%MODE%"=="p0001"   ( call :variant p0001 "0001-repeatable-bake.patch" 0 & goto :end )
if /i "%MODE%"=="measure" ( call :variant measure "0001-repeatable-bake.patch 0002-probe-stats.patch" 1 & goto :end )
if /i "%MODE%"=="release" ( call :release & goto :end )
if /i "%MODE%"=="all" (
	call :variant control "" 0 || goto :fail
	call :variant p0001 "0001-repeatable-bake.patch" 0 || goto :fail
	call :variant measure "0001-repeatable-bake.patch 0002-probe-stats.patch" 1 || goto :fail
	call :release
	goto :end
)
echo build_phonon: unknown mode %MODE%
exit /b 1

:end
if errorlevel 1 goto :fail
echo build_phonon: %MODE% done %DATE% %TIME%
exit /b 0

:fail
if defined DELIVERING ( echo build_phonon: STOPPED during delivery; see the line above & exit /b 1 )
echo build_phonon: STOPPED; %LIBS% keeps its previous contents
exit /b 1


rem ==================================================================================================================
rem :deps - get_dependencies.py for exactly flatbuffers, pffft, zlib, mysofa, catch (R-SB0-2); archived once
rem ==================================================================================================================
:deps
if exist "%ARCHIVE%\complete.txt" ( echo build_phonon: dependencies from %ARCHIVE% & exit /b 0 )
set "FETCH=%WORK%\fetch"
robocopy "%PRISTINE%\core\build" "%FETCH%\core\build" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 ( echo build_phonon: copy of core\build failed & exit /b 1 )
rem zlib before mysofa: mysofa builds against the zlib in deps\ (dependencies.json "depends")
call :fetch_one zlib "" || exit /b 1
call :fetch_one mysofa 3.15 || exit /b 1
call :fetch_one flatbuffers 3.5 || exit /b 1
call :fetch_one pffft 3.15 || exit /b 1
call :fetch_one catch "" || exit /b 1
if exist "%FETCH%\core\deps\ipp" ( echo build_phonon: CHECK FAILED: the fetch made deps\ipp & exit /b 1 )
robocopy "%FETCH%\core\deps" "%ARCHIVE%\deps" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 ( echo build_phonon: archive copy failed & exit /b 1 )
if not exist "%ARCHIVE%\licenses" mkdir "%ARCHIVE%\licenses"
copy /y "%FETCH%\core\deps-build\flatbuffers\src\flatbuffers\LICENSE.txt" "%ARCHIVE%\licenses\flatbuffers-LICENSE" >nul || ( echo build_phonon: no FlatBuffers LICENSE & exit /b 1 )
copy /y "%FETCH%\core\deps-build\zlib\src\zlib\LICENSE" "%ARCHIVE%\licenses\zlib-LICENSE" >nul || ( echo build_phonon: no zlib LICENSE & exit /b 1 )
> "%ARCHIVE%\complete.txt" echo the five dependencies of build_phonon.cmd, fetched %DATE%
exit /b 0

rem :fetch_one <name> <policy minimum or "">
:fetch_one
set "CMAKE_POLICY_VERSION_MINIMUM=%~2"
pushd "%FETCH%\core\build"
"%PY%" get_dependencies.py -p windows -a x64 -t vs2022 --dependency %1
set "EXITCODE=%errorlevel%"
popd
set "CMAKE_POLICY_VERSION_MINIMUM="
if not "%EXITCODE%"=="0" ( echo build_phonon: fetch of %1 failed & exit /b 1 )
exit /b 0


rem ==================================================================================================================
rem :variant <name> "<patches in order>" <measure 0/1> - copy, patch, configure, build, check, phonon_test
rem ==================================================================================================================
:variant
set "VNAME=%~1"
set "VPATCHES=%~2"
set "VMEASURE=%~3"
set "VDIR=%WORK%\%VNAME%"
echo build_phonon: variant %VNAME% start %TIME%

if exist "%VDIR%" rmdir /s /q "%VDIR%"
rem a folder another process holds open as its current directory cannot be removed; empty folders left behind are
rem harmless, a file left behind is not
if exist "%VDIR%" (
	dir /s /b /a-d "%VDIR%" >nul 2>&1 && ( echo build_phonon: cannot clear %VDIR% & exit /b 1 )
	echo build_phonon: %VDIR% kept only empty folders held open by another process; continuing
)
rem core\ and the empty siblings that take its post-build copies (src/core/CMakeLists.txt: ../fmod, ../wwise, ...)
robocopy "%PRISTINE%\core" "%VDIR%\core" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 ( echo build_phonon: copy of core failed & exit /b 1 )
for %%S in (fmod unity unreal wwise) do mkdir "%VDIR%\%%S"
robocopy "%ARCHIVE%\deps" "%VDIR%\core\deps" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 ( echo build_phonon: copy of deps failed & exit /b 1 )

rem patches in number order: --check first, so a failing hunk stops before anything is written (R-SB0-12).
rem Inside a git work tree, git apply run from a subdirectory skips the paths outside it and still exits 0. The
rem ceiling stops git's repository search at the variant folder, so core\ is never taken as part of an outer
rem repository and every path of a patch is applied or reported; GIT_DIR and GIT_WORK_TREE must not point elsewhere.
set "GIT_CEILING_DIRECTORIES=%VDIR%"
set "GIT_DIR="
set "GIT_WORK_TREE="
pushd "%VDIR%\core"
for %%P in (%VPATCHES%) do (
	if not exist "%PATCHES%\%%P" ( echo build_phonon: missing patch %PATCHES%\%%P & popd & exit /b 1 )
	git apply --check -p1 "%PATCHES%\%%P"
	if errorlevel 1 ( echo build_phonon: patch %%P does not apply; the hunk is named above & popd & exit /b 1 )
	git apply -p1 "%PATCHES%\%%P"
	if errorlevel 1 ( echo build_phonon: patch %%P failed & popd & exit /b 1 )
	echo build_phonon: applied %%P
)
popd

set "MEASUREFLAG="
if "%VMEASURE%"=="1" set "MEASUREFLAG=-DIPL_LOCAL_MEASURE=1"

rem every option pinned (R-SB0-3): core/CMakeLists.txt turns a missing package off silently, so nothing is left to it
cmake -G "Visual Studio 17 2022" -A x64 -S "%VDIR%\core" -B "%VDIR%\build" ^
	-DBUILD_SHARED_LIBS=ON -DSTEAMAUDIO_STATIC_RUNTIME=ON ^
	-DSTEAMAUDIO_ENABLE_IPP=OFF -DSTEAMAUDIO_ENABLE_MKL=OFF -DSTEAMAUDIO_ENABLE_FFTS=OFF ^
	-DSTEAMAUDIO_ENABLE_EMBREE=OFF -DSTEAMAUDIO_ENABLE_RADEONRAYS=OFF -DSTEAMAUDIO_ENABLE_TRUEAUDIONEXT=OFF ^
	-DSTEAMAUDIO_ENABLE_AVX=ON -DSTEAMAUDIO_ENABLE_OCTAVE_BANDS=OFF ^
	-DSTEAMAUDIO_BUILD_TESTS=ON -DSTEAMAUDIO_BUILD_BENCHMARKS=OFF -DSTEAMAUDIO_BUILD_SAMPLES=OFF ^
	-DSTEAMAUDIO_BUILD_ITESTS=OFF -DSTEAMAUDIO_BUILD_DOCS=OFF ^
	%MEASUREFLAG% > "%WORK%\%VNAME%-configure.log" 2>&1
if errorlevel 1 ( echo build_phonon: configure failed, see %WORK%\%VNAME%-configure.log & exit /b 1 )

echo build_phonon: variant %VNAME% build start %TIME%
cmake --build "%VDIR%\build" --config Release --target phonon phonon_test --parallel > "%WORK%\%VNAME%-build.log" 2>&1
if errorlevel 1 ( echo build_phonon: build failed, see %WORK%\%VNAME%-build.log & exit /b 1 )
echo build_phonon: variant %VNAME% build end %TIME%

rem the IPP checks (R-SB0-4); each one stops the script
"%PY%" -I "%HELPER%" ippcheck "%VDIR%"
if errorlevel 1 exit /b 1

rem the unit tests, as the library's own add_test runs them (src/test/CMakeLists.txt)
pushd "%VDIR%\core\src\test"
"%VDIR%\build\src\test\Release\phonon_test.exe" "~[ReflectionSimulator]" > "%WORK%\%VNAME%-phonon_test.log" 2>&1
set "EXITCODE=%errorlevel%"
popd
if not "%EXITCODE%"=="0" ( echo build_phonon: phonon_test failed, see %WORK%\%VNAME%-phonon_test.log & exit /b 1 )
echo build_phonon: phonon_test passed

rem the filter above leaves out the only tests that build the class the patches change; with a patch, run the
rem multi-threaded one (more than one source: the job * maxSources + source indexing)
if not "%VPATCHES%"=="" (
	pushd "%VDIR%\core\src\test"
	"%VDIR%\build\src\test\Release\phonon_test.exe" "Multi-threaded implementations of CPUReflectionSimulator produce comparable results." > "%WORK%\%VNAME%-phonon_test_simulator.log" 2>&1
	set "EXITCODE=!errorlevel!"
	popd
	if not "!EXITCODE!"=="0" ( echo build_phonon: CHECK FAILED: the multi-threaded CPUReflectionSimulator test failed, see %WORK%\%VNAME%-phonon_test_simulator.log & exit /b 1 )
	echo build_phonon: multi-threaded CPUReflectionSimulator test passed
)

rem the patch level
if not exist "%WORK%\tools" mkdir "%WORK%\tools"
if not exist "%WORK%\tools\patch_level.exe" (
	cl /nologo /O2 "%LIBS%\tools\patch_level.c" /Fo"%WORK%\tools\patch_level.obj" /Fe"%WORK%\tools\patch_level.exe" >nul
	if errorlevel 1 ( echo build_phonon: patch_level.exe failed to build & exit /b 1 )
)
set "WANT=missing"
if "%VNAME%"=="p0001" set "WANT=0x1"
if "%VNAME%"=="measure" set "WANT=0x80000003"
if "%VNAME%"=="release" set "WANT=0x3"
set "LEVEL="
for /f "usebackq delims=" %%L in (`call "%WORK%\tools\patch_level.exe" "%VDIR%\build\src\core\Release\phonon.dll"`) do set "LEVEL=%%L"
if not "%LEVEL%"=="%WANT%" ( echo build_phonon: CHECK FAILED: iplLocalPatchLevel %LEVEL%, want %WANT% & exit /b 1 )
echo build_phonon: iplLocalPatchLevel %LEVEL%

mkdir "%VDIR%\bin"
set "DLLNAME=phonon"
if "%VMEASURE%"=="1" set "DLLNAME=phonon_measure"
copy /y "%VDIR%\build\src\core\Release\phonon.dll" "%VDIR%\bin\%DLLNAME%.dll" >nul || exit /b 1
copy /y "%VDIR%\build\src\core\Release\phonon.pdb" "%VDIR%\bin\%DLLNAME%.pdb" >nul || exit /b 1
copy /y "%VDIR%\build\src\core\Release\phonon.lib" "%VDIR%\bin\%DLLNAME%.lib" >nul || exit /b 1
echo build_phonon: variant %VNAME% done %TIME%
exit /b 0


rem ==================================================================================================================
rem :release - build 0001 + 0002, then stage and copy the outputs into this folder (R-SB0-5); no import library
rem ==================================================================================================================
:release
call :variant release "0001-repeatable-bake.patch 0002-probe-stats.patch" 0 || exit /b 1
set "REL=%WORK%\release"
set "STAGE=%WORK%\stage"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\include"
mkdir "%STAGE%\bin\win64"
copy /y "%REL%\core\src\core\phonon.h" "%STAGE%\include\phonon.h" >nul || exit /b 1
copy /y "%REL%\build\src\core\phonon_version.h" "%STAGE%\include\phonon_version.h" >nul || exit /b 1
copy /y "%REL%\bin\phonon.dll" "%STAGE%\bin\win64\phonon.dll" >nul || exit /b 1
copy /y "%REL%\bin\phonon.pdb" "%STAGE%\bin\win64\phonon.pdb" >nul || exit /b 1
copy /y "%PRISTINE%\LICENSE.md" "%STAGE%\LICENSE.md" >nul || exit /b 1
"%PY%" -I "%HELPER%" thirdparty "%PRISTINE%\core\THIRDPARTY.md" "%ARCHIVE%\licenses" "%STAGE%\THIRDPARTY.md" || exit /b 1
"%PY%" -I "%HELPER%" source "%PRISTINE%\core" "%PATCHES%" "%REL%" "%STAGE%\SOURCE.txt" || exit /b 1

rem every check passed: only now is this folder written. Everything but phonon.dll first (two retries, not
rem robocopy's million); then the DLL under a temporary name, moved into place last, so a DLL is never half written.
set "DELIVERING=1"
set "MIXED=build_phonon: DELIVERY FAILED: %LIBS% may be mixed (old and new files); fix the cause and re-run build_phonon.cmd release"
robocopy "%STAGE%" "%LIBS%" /E /XF phonon.dll /R:2 /W:2 /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 ( echo !MIXED! & exit /b 1 )
copy /y "%STAGE%\bin\win64\phonon.dll" "%LIBS%\bin\win64\phonon.dll.new" >nul || ( echo !MIXED! & exit /b 1 )
move /y "%LIBS%\bin\win64\phonon.dll.new" "%LIBS%\bin\win64\phonon.dll" >nul || ( echo !MIXED! & exit /b 1 )
echo build_phonon: delivered phonon.h, phonon_version.h, phonon.dll, phonon.pdb, LICENSE.md, THIRDPARTY.md, SOURCE.txt
exit /b 0
