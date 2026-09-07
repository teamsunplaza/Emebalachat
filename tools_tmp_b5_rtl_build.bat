@echo off
REM P4 Batch B-5 (session 260907_0002) proof build - VP-designated log names.
REM Same idiom as tools_tmp_b4_rtl_build.bat: source vcvars64 first (fresh cmd
REM has no MSVC INCLUDE env), then build -> run_tests -> ctest, logging to root.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cmake --build build > build_b5_rtl.log 2>&1
set BUILD_RC=%ERRORLEVEL%
echo BUILD_RC=%BUILD_RC%
if %BUILD_RC% NEQ 0 goto :fail

REM NOTE: this tree uses the Ninja single-config generator (Release via
REM CMAKE_BUILD_TYPE), so the binary is build\run_tests.exe, not
REM build\Release\run_tests.exe (same idiom as tools_tmp_b4_rtl_build.bat).
build\run_tests.exe > run_tests_b5_rtl.log 2>&1
set TESTS_RC=%ERRORLEVEL%
echo TESTS_RC=%TESTS_RC%
if %TESTS_RC% NEQ 0 goto :fail

ctest --test-dir build -C Release -R CoreTests > ctest_b5_rtl.log 2>&1
set CTEST_RC=%ERRORLEVEL%
echo CTEST_RC=%CTEST_RC%
goto :done

:fail
echo STUCK_AT_RC_BUILD=%BUILD_RC%_TESTS=%TESTS_RC%

:done
echo DONE
