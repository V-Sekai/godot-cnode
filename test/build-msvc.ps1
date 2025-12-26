# MSVC build script for test_cnode
# Requires: Visual Studio with C++ build tools

param(
    [string]$ErlangHome = $env:ERLANG_HOME,
    [string]$VSVersion = "2022"
)

Write-Host "=== Building test_cnode for Windows with MSVC ===" -ForegroundColor Cyan
Write-Host ""

# Find Visual Studio installation
$vsPaths = @(
    "C:\Program Files\Microsoft Visual Studio\$VSVersion",
    "C:\Program Files (x86)\Microsoft Visual Studio\$VSVersion",
    "C:\Program Files\Microsoft Visual Studio\2022",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022",
    "C:\Program Files\Microsoft Visual Studio\2019",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019"
)

$vsPath = $null
foreach ($path in $vsPaths) {
    if (Test-Path $path) {
        $vsPath = $path
        Write-Host "Found Visual Studio at: $vsPath" -ForegroundColor Green
        break
    }
}

if (-not $vsPath) {
    Write-Host "ERROR: Could not find Visual Studio installation." -ForegroundColor Red
    Write-Host "Please install Visual Studio with C++ build tools." -ForegroundColor Yellow
    exit 1
}

# Find vcvarsall.bat
$vcvarsall = Get-ChildItem $vsPath -Recurse -Filter "vcvarsall.bat" -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not $vcvarsall) {
    Write-Host "ERROR: Could not find vcvarsall.bat" -ForegroundColor Red
    exit 1
}

Write-Host "Using vcvarsall: $($vcvarsall.FullName)" -ForegroundColor Gray
Write-Host ""

# Find Erlang installation if not set
if (-not $ErlangHome) {
    Write-Host "ERLANG_HOME not set, searching for Erlang installation..." -ForegroundColor Yellow
    
    # Check common locations
    $possiblePaths = @(
        "C:\Program Files\erl*",
        "C:\Program Files (x86)\erl*",
        "$env:ProgramFiles\erl*",
        "$env:ProgramFiles(x86)\erl*",
        "C:\Users\ernes\scoop\apps\erlang\*\lib"
    )
    
    foreach ($path in $possiblePaths) {
        $erlDirs = Get-ChildItem $path -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($erlDirs) {
            # If we found lib directory, go up one level
            if ($path -like "*\lib") {
                $ErlangHome = $erlDirs[0].Parent.FullName
            } else {
                $ErlangHome = $erlDirs[0].FullName
            }
            Write-Host "Found Erlang at: $ErlangHome" -ForegroundColor Green
            break
        }
    }
    
    if (-not $ErlangHome) {
        Write-Host "ERROR: Could not find Erlang installation." -ForegroundColor Red
        Write-Host "Please set ERLANG_HOME environment variable." -ForegroundColor Yellow
        exit 1
    }
}

# Find erl_interface
$erlInterfacePath = Get-ChildItem "$ErlangHome\lib" -Directory -Filter "erl_interface*" -ErrorAction SilentlyContinue | 
    Sort-Object Name -Descending | Select-Object -First 1

if (-not $erlInterfacePath) {
    Write-Host "ERROR: Could not find erl_interface in $ErlangHome\lib" -ForegroundColor Red
    exit 1
}

$erlInterfaceBase = $erlInterfacePath.FullName
$includePath = Join-Path $erlInterfaceBase "include"
$libPath = Join-Path $erlInterfaceBase "lib"

Write-Host "Using erl_interface from: $erlInterfaceBase" -ForegroundColor Green
Write-Host "  Include: $includePath" -ForegroundColor Gray
Write-Host "  Library: $libPath" -ForegroundColor Gray
Write-Host ""

# Source and output
$sourceFile = "test_cnode.c"
$outputFile = "test_cnode.exe"

if (-not (Test-Path $sourceFile)) {
    Write-Host "ERROR: Source file not found: $sourceFile" -ForegroundColor Red
    exit 1
}

# Create a batch file to compile with MSVC
$batchFile = "build-msvc-temp.bat"
$batchContent = @"
@echo off
call "$($vcvarsall.FullName)" x64
if errorlevel 1 (
    echo Failed to initialize MSVC environment
    exit /b 1
)

echo Compiling with MSVC...
cl /EHsc /W3 /O2 /I"$includePath" /D_WIN32_WINNT=0x0601 /Fe:$outputFile $sourceFile /link /LIBPATH:"$libPath" ei.lib ws2_32.lib

if errorlevel 1 (
    echo Build failed!
    exit /b 1
) else (
    echo.
    echo Build successful!
    echo Output: $outputFile
    echo.
    echo To run:
    echo   .\$outputFile test_cnode@127.0.0.1 godotcookie
)
"@

$batchContent | Out-File -FilePath $batchFile -Encoding ASCII

Write-Host "Compiling with MSVC..." -ForegroundColor Cyan
Write-Host ""

# Run the batch file
& cmd /c $batchFile

$buildSuccess = $LASTEXITCODE -eq 0

# Clean up batch file
Remove-Item $batchFile -ErrorAction SilentlyContinue

if ($buildSuccess) {
    Write-Host ""
    Write-Host "Build successful!" -ForegroundColor Green
    Write-Host "Output: $outputFile" -ForegroundColor Green
    Write-Host ""
    Write-Host "To run:" -ForegroundColor Cyan
    Write-Host "  .\$outputFile test_cnode@127.0.0.1 godotcookie" -ForegroundColor White
    Write-Host ""
    Write-Host "Note: Make sure epmd is running:" -ForegroundColor Yellow
    Write-Host "  epmd -daemon" -ForegroundColor White
} else {
    Write-Host ""
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

