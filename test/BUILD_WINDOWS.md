# Building test_cnode for Windows

## Current Status

The test_cnode program has been modified to support Windows compilation, but there's a linker compatibility issue:

**Problem**: The Erlang `erl_interface` library (version 5.6.2) was compiled with Microsoft Visual C++ (MSVC) and requires MSVC runtime libraries (`libLIBCMT.a`, `libOLDNAMES.a`). When using llvm-mingw (clang-based MinGW), these libraries are not available, causing linker errors.

## Solutions

### Option 1: Use MSVC Compiler (Recommended)

Install Microsoft Visual Studio Build Tools and use the MSVC compiler:

1. **Install Visual Studio Build Tools**:
   - Download from: https://visualstudio.microsoft.com/downloads/
   - Select "Build Tools for Visual Studio"
   - Install "Desktop development with C++" workload

2. **Compile with MSVC**:
   ```powershell
   cd test
   $env:ERLANG_HOME = "C:\Users\ernes\scoop\apps\erlang\28.3"
   $erlInterface = "$env:ERLANG_HOME\lib\erl_interface-5.6.2"
   
   cl /EHsc /I"$erlInterface\include" /Fe:test_cnode.exe test_cnode.c /link /LIBPATH:"$erlInterface\lib" ei.lib ws2_32.lib
   ```

### Option 2: Use MSYS2/MinGW-w64

MSYS2 provides a MinGW-w64 environment that may have better compatibility:

1. **Install MSYS2**: https://www.msys2.org/
2. **Install Erlang and MinGW-w64**:
   ```bash
   pacman -S mingw-w64-x86_64-gcc erlang
   ```
3. **Compile**:
   ```bash
   cd test
   gcc -o test_cnode.exe test_cnode.c -I/usr/lib/erlang/lib/erl_interface-*/include -L/usr/lib/erlang/lib/erl_interface-*/lib -lei -lws2_32
   ```

### Option 3: Create Stub Libraries

Create empty stub libraries for the missing MSVC runtime libraries:

```powershell
# Create empty stub libraries (requires ar.exe from MinGW)
cd test
ar rcs libLIBCMT.a
ar rcs libOLDNAMES.a

# Then compile with these stubs in the library path
```

### Option 4: Use WSL (Windows Subsystem for Linux)

If you have WSL installed, compile and test on Linux:

```bash
wsl
cd /mnt/c/Users/ernes/OneDrive/Desktop/godot-cnode/test
make
./test_cnode test_cnode@127.0.0.1 godotcookie
```

## Code Modifications Made

The following Windows compatibility changes have been applied to `test_cnode.c`:

1. **Conditional includes**: POSIX headers only on Unix, Winsock2 on Windows
2. **Winsock initialization**: `WSAStartup()` in `main()`, `WSACleanup()` on exit
3. **Socket functions**: `closesocket()` instead of `close()`, `ioctlsocket()` instead of `fcntl()`
4. **Non-blocking I/O**: Windows-specific implementation using `ioctlsocket()` with `FIONBIO`
5. **Error handling**: `WSAGetLastError()` instead of `errno` for socket errors

## Build Script

A PowerShell build script (`build-mingw.ps1`) has been created that:
- Automatically finds Erlang installation
- Locates erl_interface library
- Compiles with appropriate flags
- Links against required libraries

**Usage**:
```powershell
cd test
$env:ERLANG_HOME = "C:\path\to\erlang"
.\build-mingw.ps1
```

## Next Steps

1. Choose one of the solutions above
2. Complete the compilation
3. Test the executable:
   ```powershell
   # Start epmd (Erlang Port Mapper Daemon)
   epmd -daemon
   
   # Run the test CNode
   .\test_cnode.exe test_cnode@127.0.0.1 godotcookie
   
   # In another terminal, run the Elixir test
   elixir test_cnode_elixir.exs
   ```

## Notes

- The code compiles successfully (only a warning about socket type comparison)
- The linker error is due to MSVC/MinGW runtime incompatibility
- All Windows-specific code modifications are complete and tested for syntax

