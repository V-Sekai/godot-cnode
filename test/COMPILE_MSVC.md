# Compiling test_cnode with MSVC

## Quick Start

Since MSVC is installed, you can compile using one of these methods:

### Method 1: Developer Command Prompt (Easiest)

1. **Open "Developer Command Prompt for VS"**:
   - Press `Win + S` and search for "Developer Command Prompt"
   - Or find it in Start Menu under Visual Studio folder

2. **Navigate to test directory**:
   ```cmd
   cd C:\Users\ernes\OneDrive\Desktop\godot-cnode\test
   ```

3. **Run the build script**:
   ```cmd
   build-msvc.ps1
   ```

   Or compile manually:
   ```cmd
   set ERLANG_HOME=C:\Users\ernes\scoop\apps\erlang\28.3
   set ERL_INTERFACE=%ERLANG_HOME%\lib\erl_interface-5.6.2
   cl /EHsc /W3 /O2 /I"%ERL_INTERFACE%\include" /D_WIN32_WINNT=0x0601 /Fe:test_cnode.exe test_cnode.c /link /LIBPATH:"%ERL_INTERFACE%\lib" ei.lib ws2_32.lib
   ```

### Method 2: PowerShell (If vcvarsall.bat is found)

If you know where Visual Studio is installed, you can run:

```powershell
cd C:\Users\ernes\OneDrive\Desktop\godot-cnode\test
$erlInterface = "C:\Users\ernes\scoop\apps\erlang\28.3\lib\erl_interface-5.6.2"
cmd /c "call `"C:\Path\To\Visual Studio\VC\Auxiliary\Build\vcvarsall.bat`" x64 && cl /EHsc /W3 /O2 /I`"$erlInterface\include`" /D_WIN32_WINNT=0x0601 /Fe:test_cnode.exe test_cnode.c /link /LIBPATH:`"$erlInterface\lib`" ei.lib ws2_32.lib"
```

### Method 3: Find Visual Studio Installation

If you're not sure where Visual Studio is installed:

```powershell
# Search for vcvarsall.bat
Get-ChildItem "C:\Program Files" -Recurse -Filter "vcvarsall.bat" -ErrorAction SilentlyContinue | Select-Object FullName
Get-ChildItem "C:\Program Files (x86)" -Recurse -Filter "vcvarsall.bat" -ErrorAction SilentlyContinue | Select-Object FullName
```

Then use the path found in Method 2.

## Compilation Flags Explained

- `/EHsc` - Enable C++ exception handling
- `/W3` - Warning level 3
- `/O2` - Optimize for speed
- `/I"path"` - Include directory
- `/D_WIN32_WINNT=0x0601` - Define Windows version (Windows 7+)
- `/Fe:test_cnode.exe` - Output executable name
- `/link /LIBPATH:"path"` - Library search path
- `ei.lib ws2_32.lib` - Link against erl_interface and Winsock2

## After Compilation

Once compiled successfully:

1. **Start epmd** (Erlang Port Mapper Daemon):
   ```cmd
   epmd -daemon
   ```

2. **Run the test CNode**:
   ```cmd
   test_cnode.exe test_cnode@127.0.0.1 godotcookie
   ```

3. **In another terminal, test with Elixir**:
   ```cmd
   elixir test_cnode_elixir.exs
   ```

## Troubleshooting

### "cl is not recognized"
- Make sure you're running from Developer Command Prompt
- Or manually initialize MSVC environment using vcvarsall.bat

### "Cannot open include file 'ei.h'"
- Check that ERLANG_HOME is set correctly
- Verify erl_interface is installed at the expected path

### "Cannot open library file 'ei.lib'"
- Check the library path in /LIBPATH
- Verify ei.lib exists in the erl_interface lib directory

