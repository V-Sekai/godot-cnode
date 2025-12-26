# Windows build script for test_cnode
# Requires: Erlang/OTP with erl_interface, MinGW gcc

param(
    [string]$ErlangHome = $env:ERLANG_HOME,
    [string]$Compiler = "gcc"
)

Write-Host "=== Building test_cnode for Windows ===" -ForegroundColor Cyan
Write-Host ""

# Find Erlang installation if not set
if (-not $ErlangHome) {
    Write-Host "ERLANG_HOME not set, searching for Erlang installation..." -ForegroundColor Yellow
    
    # Check common locations
    $possiblePaths = @(
        "C:\Program Files\erl*",
        "C:\Program Files (x86)\erl*",
        "$env:ProgramFiles\erl*",
        "$env:ProgramFiles(x86)\erl*"
    )
    
    foreach ($path in $possiblePaths) {
        $erlDirs = Get-ChildItem $path -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($erlDirs) {
            $ErlangHome = $erlDirs[0].FullName
            Write-Host "Found Erlang at: $ErlangHome" -ForegroundColor Green
            break
        }
    }
    
    if (-not $ErlangHome) {
        Write-Host "ERROR: Could not find Erlang installation." -ForegroundColor Red
        Write-Host "Please install Erlang/OTP from https://www.erlang.org/downloads" -ForegroundColor Yellow
        Write-Host "Or set ERLANG_HOME environment variable." -ForegroundColor Yellow
        exit 1
    }
}

# Find erl_interface
$erlInterfacePath = Get-ChildItem "$ErlangHome\lib" -Directory -Filter "erl_interface*" -ErrorAction SilentlyContinue | 
    Sort-Object Name -Descending | Select-Object -First 1

if (-not $erlInterfacePath) {
    Write-Host "ERROR: Could not find erl_interface in $ErlangHome\lib" -ForegroundColor Red
    Write-Host "erl_interface is required for building CNodes." -ForegroundColor Yellow
    Write-Host "Please install the full Erlang/OTP package (not just runtime)." -ForegroundColor Yellow
    exit 1
}

$erlInterfaceBase = $erlInterfacePath.FullName
$includePath = Join-Path $erlInterfaceBase "include"
$libPath = Join-Path $erlInterfaceBase "lib"

Write-Host "Using erl_interface from: $erlInterfaceBase" -ForegroundColor Green
Write-Host "  Include: $includePath" -ForegroundColor Gray
Write-Host "  Library: $libPath" -ForegroundColor Gray
Write-Host ""

# Check if include directory exists
if (-not (Test-Path $includePath)) {
    Write-Host "ERROR: Include directory not found: $includePath" -ForegroundColor Red
    exit 1
}

# Check if lib directory exists
if (-not (Test-Path $libPath)) {
    Write-Host "ERROR: Library directory not found: $libPath" -ForegroundColor Red
    exit 1
}

# Find gcc
$gccPath = Get-Command $Compiler -ErrorAction SilentlyContinue
if (-not $gccPath) {
    Write-Host "ERROR: Could not find $Compiler" -ForegroundColor Red
    Write-Host "Please install MinGW or add gcc to PATH" -ForegroundColor Yellow
    exit 1
}

Write-Host "Using compiler: $($gccPath.Source)" -ForegroundColor Green
Write-Host ""

# Compiler flags
$cFlags = @(
    "-Wall",
    "-Wextra",
    "-std=c11",
    "-g",
    "-O2",
    "-I`"$includePath`"",
    "-D_POSIX_C_SOURCE=200112L"
)

# On Windows with MinGW, we might need additional defines
if ($IsWindows -or $env:OS -eq "Windows_NT") {
    $cFlags += "-D_WIN32_WINNT=0x0601"  # Windows 7+
}

# Library flags
$libFlags = @(
    "-L`"$libPath`"",
    "-lei",
    "-lws2_32",  # Winsock2 library for Windows
    "-static-libgcc"  # Static link libgcc to avoid runtime dependencies
)

# Source and output
$sourceFile = "test_cnode.c"
$outputFile = "test_cnode.exe"

if (-not (Test-Path $sourceFile)) {
    Write-Host "ERROR: Source file not found: $sourceFile" -ForegroundColor Red
    exit 1
}

Write-Host "Compiling $sourceFile..." -ForegroundColor Cyan
$compileCmd = "$($gccPath.Source) $($cFlags -join ' ') -o $outputFile $sourceFile $($libFlags -join ' ')"
Write-Host "Command: $compileCmd" -ForegroundColor Gray
Write-Host ""

# Compile
& $gccPath.Source $cFlags -o $outputFile $sourceFile $libFlags

if ($LASTEXITCODE -eq 0) {
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

