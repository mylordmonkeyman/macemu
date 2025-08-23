@echo off
setlocal enabledelayedexpansion

:: Set the base paths - modify these to match your directory structure
set "BASE_PATH=C:\Users\Michael Saunders\Github\macemu"
set "SHEEPSHAVER_PATH=%BASE_PATH%\SheepShaver"
set "BASILISKII_PATH=%BASE_PATH%\BasiliskII"

:: Counters for statistics
set /a replaced_count=0
set /a checked_count=0
set /a error_count=0

echo ========================================
echo SheepShaver Placeholder Replacement Tool
echo ========================================
echo.
echo Base Path: %BASE_PATH%
echo SheepShaver Path: %SHEEPSHAVER_PATH%
echo BasiliskII Path: %BASILISKII_PATH%
echo.

:: Check if directories exist
if not exist "%SHEEPSHAVER_PATH%" (
    echo ERROR: SheepShaver directory not found!
    echo Path: %SHEEPSHAVER_PATH%
    pause
    exit /b 1
)

if not exist "%BASILISKII_PATH%" (
    echo ERROR: BasiliskII directory not found!
    echo Path: %BASILISKII_PATH%
    pause
    exit /b 1
)

echo Starting scan...
echo.

:: Recursively process all files in SheepShaver directory
for /r "%SHEEPSHAVER_PATH%" %%F in (*) do (
    set /a checked_count+=1
    set "current_file=%%F"
    
    :: Check if file is a single-line placeholder
    call :CheckPlaceholder "%%F"
)

echo.
echo ========================================
echo Process Complete!
echo ========================================
echo Files checked: !checked_count!
echo Files replaced: !replaced_count!
echo Errors: !error_count!
echo.
echo You can now safely delete the BasiliskII folder.
pause
exit /b 0

:CheckPlaceholder
set "file=%~1"
set "line_count=0"
set "first_line="
set "is_placeholder=0"

:: Count lines and get first line
for /f "usebackq delims=" %%L in ("%file%") do (
    set /a line_count+=1
    if !line_count! equ 1 (
        set "first_line=%%L"
    )
)

:: Check if it's a single-line file containing "BasiliskII"
if !line_count! equ 1 (
    echo !first_line! | findstr /i "BasiliskII" >nul
    if !errorlevel! equ 0 (
        set "is_placeholder=1"
    )
)

:: If it's a placeholder, process it
if !is_placeholder! equ 1 (
    echo Found placeholder: %file%
    echo   Content: !first_line!
    
    :: Extract the relative path from the placeholder content
    set "relative_path=!first_line!"
    
    :: Remove any leading/trailing spaces
    for /f "tokens=* delims= " %%A in ("!relative_path!") do set "relative_path=%%A"
    
    :: Convert the relative path to absolute path
    :: The placeholder path is relative to the file's directory
    for %%D in ("%file%") do set "file_dir=%%~dpD"
    
    :: Combine the file's directory with the relative path
    pushd "!file_dir!"
    
    :: Resolve the full path
    set "source_file="
    for %%R in ("!relative_path!") do (
        set "source_file=%%~fR"
    )
    
    popd
    
    :: Check if the source file exists
    if exist "!source_file!" (
        echo   Copying from: !source_file!
        
        :: Backup the placeholder file (optional - comment out if not needed)
        :: copy "%file%" "%file%.placeholder.bak" >nul 2>&1
        
        :: Replace the placeholder with the actual file
        copy /y "!source_file!" "%file%" >nul
        
        if !errorlevel! equ 0 (
            echo   SUCCESS: File replaced
            set /a replaced_count+=1
        ) else (
            echo   ERROR: Failed to copy file
            set /a error_count+=1
        )
    ) else (
        echo   ERROR: Source file not found: !source_file!
        set /a error_count+=1
    )
    echo.
)

goto :eof