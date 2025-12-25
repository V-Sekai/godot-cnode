#!/usr/bin/env python
import os
import sys
import subprocess
import glob

# Build godot-cpp first
env = SConscript("thirdparty/godot-cpp/SConstruct")

# CNode-specific defines
cnode_defines = ["HAVE_CONFIG_H"]
# POSIX defines only for Unix systems
if env["platform"] != "windows":
    cnode_defines.extend([
        "_DARWIN_C_SOURCE",
        "_POSIX_C_SOURCE=200112L",
    ])
env.Append(CPPDEFINES=cnode_defines)

env.Prepend(CPPPATH=["thirdparty", "src"])
env.Append(CPPPATH=["src/"])

# Find system erl_interface installation
def find_erl_interface():
    """Find erl_interface include and library paths from system installation."""
    include_paths = []
    lib_paths = []
    libs = []

    # Try pkg-config first (not available on Windows by default)
    if sys.platform != 'win32':
        try:
            result = subprocess.run(['pkg-config', '--cflags', 'erl_interface'],
                                  capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                flags = result.stdout.strip().split()
                for flag in flags:
                    if flag.startswith('-I'):
                        include_paths.append(flag[2:])

            result = subprocess.run(['pkg-config', '--libs', 'erl_interface'],
                                  capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                flags = result.stdout.strip().split()
                for flag in flags:
                    if flag.startswith('-L'):
                        lib_paths.append(flag[2:])
                    elif flag.startswith('-l'):
                        libs.append(flag[2:])
        except:
            pass

    # If pkg-config didn't work, try common installation paths
    if not include_paths:
        # Common Erlang installation paths
        search_paths = []

        # Check ERL_TOP environment variable
        erl_top = os.environ.get('ERL_TOP')
        if erl_top:
            search_paths.append(os.path.join(erl_top, 'lib', 'erl_interface'))

        # Standard system paths
        if sys.platform == 'darwin':  # macOS
            # Homebrew paths
            search_paths.extend([
                '/opt/homebrew/lib/erlang/lib/erl_interface-*',
                '/usr/local/lib/erlang/lib/erl_interface-*',
                '/opt/local/lib/erlang/lib/erl_interface-*',
            ])
        elif sys.platform.startswith('linux'):  # Linux
            search_paths.extend([
                '/usr/lib/erlang/lib/erl_interface-*',
                '/usr/local/lib/erlang/lib/erl_interface-*',
            ])
        elif sys.platform == 'win32':  # Windows
            # Common Windows Erlang paths
            program_files = os.environ.get('ProgramFiles', 'C:\\Program Files')
            program_files_x86 = os.environ.get('ProgramFiles(x86)', os.path.join('C:\\', 'Program Files (x86)'))

            # Try exact paths first (without glob)
            exact_paths = [
                os.path.join(program_files, 'Erlang OTP', 'lib'),
                os.path.join(program_files_x86, 'Erlang OTP', 'lib'),
            ]
            for base_path in exact_paths:
                if os.path.exists(base_path):
                    # Look for erl_interface-* directories
                    try:
                        dirs = [d for d in os.listdir(base_path) if d.startswith('erl_interface-') and os.path.isdir(os.path.join(base_path, d))]
                        if dirs:
                            dirs.sort(reverse=True)  # Get latest version
                            erl_interface_base = os.path.join(base_path, dirs[0])
                            include_dir = os.path.join(erl_interface_base, 'include')
                            lib_dir = os.path.join(erl_interface_base, 'lib')
                            if os.path.exists(include_dir):
                                include_paths.append(include_dir)
                            if os.path.exists(lib_dir):
                                lib_paths.append(lib_dir)
                            break
                    except:
                        pass

            # Fallback to glob patterns
            if not include_paths:
                search_paths.extend([
                    os.path.join(program_files, 'Erlang OTP', 'lib', 'erl_interface-*'),
                    os.path.join(program_files_x86, 'Erlang OTP', 'lib', 'erl_interface-*'),
                    os.path.join(program_files, 'erl-*', 'lib', 'erl_interface-*'),
                ])

        # Find the most recent version
        for pattern in search_paths:
            matches = glob.glob(pattern)
            if matches:
                # Sort by version (assuming version numbers in path)
                matches.sort(reverse=True)
                erl_interface_base = matches[0]
                include_dir = os.path.join(erl_interface_base, 'include')
                lib_dir = os.path.join(erl_interface_base, 'lib')

                if os.path.exists(include_dir):
                    include_paths.append(include_dir)
                if os.path.exists(lib_dir):
                    lib_paths.append(lib_dir)
                break

    # Default library names if not found via pkg-config
    if not libs:
        # On Windows, library names differ by compiler
        if sys.platform == 'win32':
            # MSVC uses .lib extension, MinGW uses lib prefix
            # SCons will handle the extension automatically
            libs = ['ei']
        else:
            libs = ['ei']

    return include_paths, lib_paths, libs

# Get erl_interface paths
erl_include_paths, erl_lib_paths, erl_libs = find_erl_interface()

if not erl_include_paths:
    print("Warning: Could not find erl_interface headers. Trying default paths...")
    # Fallback: try to use erlang-config or erl (Unix only)
    if sys.platform != 'win32':
        try:
            result = subprocess.run(['erl', '-noshell', '-eval',
                                   'io:format("~s~n", [code:lib_dir(erl_interface)]), halt().'],
                                  capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                erl_interface_dir = result.stdout.strip()
                include_dir = os.path.join(erl_interface_dir, 'include')
                lib_dir = os.path.join(erl_interface_dir, 'lib')
                if os.path.exists(include_dir):
                    erl_include_paths.append(include_dir)
                if os.path.exists(lib_dir):
                    erl_lib_paths.append(lib_dir)
        except:
            pass

if erl_include_paths:
    print("Found erl_interface includes:", erl_include_paths)
    env.Prepend(CPPPATH=erl_include_paths)
else:
    print("Error: Could not find erl_interface headers. Please install erlang-dev (Linux) or erlang (macOS/Windows)")
    sys.exit(1)

# CNode source files
cnode_sources = Glob("src/*.cpp")

# Combine all sources
sources = list(cnode_sources)

# Link against system erl_interface libraries
if erl_lib_paths:
    env.Prepend(LIBPATH=erl_lib_paths)
env.Append(LIBS=erl_libs)

# Build as shared library (GDExtension) - output to bin/samples
if env["platform"] == "macos":
    library = env.SharedLibrary(
        "bin/samples/addons/godot_cnode/bin/libgodot_cnode{}.framework/libgodot_cnode{}".format(
            env["suffix"], env["suffix"]
        ),
        source=sources,
    )
elif env["platform"] == "ios":
    library = env.SharedLibrary(
        "bin/samples/addons/godot_cnode/bin/libgodot_cnode{}.framework/libgodot_cnode{}".format(
            env["suffix"], env["suffix"]
        ),
        source=sources,
    )
else:
    library = env.SharedLibrary(
        "bin/samples/addons/godot_cnode/bin/libgodot_cnode{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
        source=sources,
    )

# Build unit test executable (optional, only if test source exists)
if os.path.exists("test/test_cnode_unit.cpp"):
    test_env = env.Clone()
    test_env.Append(CPPDEFINES=["UNIT_TEST"])

    # Unit test doesn't need Godot API, just erl_interface
    test_sources = ["test/test_cnode_unit.cpp"]

    # Build test executable
    if test_env["platform"] == "macos" or test_env["platform"] == "linux":
        test_program = test_env.Program(
            "bin/test_cnode_unit",
            test_sources,
        )
    elif test_env["platform"] == "windows":
        test_program = test_env.Program(
            "bin/test_cnode_unit.exe",
            test_sources,
        )
    else:
        test_program = None

Default(library)
