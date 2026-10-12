@echo off
REM ==================================================================
REM  push-to-github.cmd - push this project to GitHub from a git bundle
REM
REM  WHY: this machine cannot reach github.com:443 (connection blocked),
REM       so the push could not be done where the repo was created.
REM       Run this on a machine that CAN reach GitHub.
REM
REM  NOTE: all text in this file is deliberately ASCII-only.
REM        cmd.exe uses an OEM code page and renders UTF-8 Chinese as
REM        "????", so non-ASCII text here would be unreadable.
REM
REM  PREREQUISITES
REM    1. git.exe available on PATH
REM    2. GitHub credentials: a Personal Access Token (classic, scope
REM       "repo") or an SSH key. GitHub no longer accepts passwords.
REM
REM  WHAT IT DOES: clone from the bundle, set origin, push.
REM                It never modifies source files.
REM ==================================================================

setlocal
set BUNDLE=pet_lamp.bundle
set REPO_URL=https://github.com/shark11634/pet_lamp.git
set CLONE_DIR=pet_lamp

echo.
echo === Checking git ===
where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git.exe not found.
    echo         Install Git for Windows: https://git-scm.com/download/win
    echo.
    pause
    exit /b 1
)
git --version

echo.
echo === Checking bundle ===
if not exist "%BUNDLE%" (
    echo [ERROR] %BUNDLE% not found in the current directory.
    echo         Put %BUNDLE% next to this script and run it again.
    echo         Current directory: %CD%
    echo.
    pause
    exit /b 1
)

if exist "%CLONE_DIR%" (
    echo.
    echo [NOTE] Directory "%CLONE_DIR%" already exists.
    choice /c YN /m "Delete it and clone fresh"
    if errorlevel 2 goto :push_existing
    rmdir /s /q "%CLONE_DIR%"
    if exist "%CLONE_DIR%" (
        echo [ERROR] Could not delete "%CLONE_DIR%". Close any program using it.
        echo.
        pause
        exit /b 1
    )
)

echo.
echo === Cloning from bundle ===
git clone "%BUNDLE%" "%CLONE_DIR%"
if errorlevel 1 (
    echo [ERROR] Clone failed.
    echo.
    pause
    exit /b 1
)

cd "%CLONE_DIR%"
echo.
echo === Setting origin ===
git remote set-url origin "%REPO_URL%" 2>nul
if errorlevel 1 git remote add origin "%REPO_URL%"
git remote -v
goto :do_push

:push_existing
cd "%CLONE_DIR%"
echo.
echo === Setting origin ===
git remote set-url origin "%REPO_URL%" 2>nul
if errorlevel 1 git remote add origin "%REPO_URL%"
git remote -v

:do_push
echo.
echo === Safety check: no sensitive files tracked ===
echo     Expected output: nothing.
echo     bslamp2-stock-4MB.bin is the stock flash backup (device identity,
echo     and the only way to restore the lamp). secrets.yaml holds your
echo     Wi-Fi / API / OTA / web passwords. Neither must ever be pushed.
echo.
git ls-files > "%TEMP%\_pet_lamp_files.txt"
findstr /i /r "\.bin$ secrets\.yaml$" "%TEMP%\_pet_lamp_files.txt"
if not errorlevel 1 (
    echo.
    echo [ABORT] The files listed above must NOT be committed.
    echo         Fix with:  git rm --cached ^<file^>
    echo         then run this script again.
    echo.
    del "%TEMP%\_pet_lamp_files.txt" >nul 2>nul
    pause
    exit /b 1
)
del "%TEMP%\_pet_lamp_files.txt" >nul 2>nul
echo     OK - no sensitive files tracked.

echo.
echo === Pushing to GitHub ===
echo     You will be asked for credentials:
echo       Username = your GitHub username
echo       Password = a Personal Access Token (NOT your account password)
echo.
git push -u origin main
if errorlevel 1 (
    echo.
    echo [FAILED] Push did not succeed. Common causes:
    echo   1. Remote repo does not exist or is not yours.
    echo      Create an empty repo first: https://github.com/shark11634/pet_lamp
    echo   2. Bad credentials - use a Personal Access Token (classic, scope repo).
    echo   3. Remote already has content with unrelated history. To overwrite:
    echo        git push -u origin main --force
    echo.
    pause
    exit /b 1
)

echo.
echo === Done ===
echo Remote: %REPO_URL%
echo Local clone: %CD%
echo.
echo Verify the code (needs a C compiler, no hardware):
echo     cd workspace
echo     powershell -ExecutionPolicy Bypass -File test\run_all_host_tests.ps1
echo Expected: ALL HOST TESTS PASSED (6 suites)
echo.
pause
endlocal