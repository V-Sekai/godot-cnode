#!/usr/bin/env python
import os
import sys

# Build godot-cpp first
env = SConscript("thirdparty/godot-cpp/SConstruct")

# CNode-specific defines
env.Append(
    CPPDEFINES=[
        "HAVE_CONFIG_H",
        "_DARWIN_C_SOURCE",
        "_POSIX_C_SOURCE=200112L",
    ]
)

env.Prepend(CPPPATH=["thirdparty", "src"])
env.Append(CPPPATH=["src/"])

# Erlang interface paths
erl_interface_dir = "thirdparty/erl_interface"
erl_include_dir = erl_interface_dir + "/include"
erl_src_dir = erl_interface_dir + "/src"

erl_include_paths = [
    erl_include_dir,
    erl_src_dir,
    erl_src_dir + "/misc",
    erl_src_dir + "/connect",
    erl_src_dir + "/epmd",
    erl_src_dir + "/encode",
    erl_src_dir + "/decode",
    erl_src_dir + "/global",
    erl_src_dir + "/openssl/include",
]

env.Prepend(CPPPATH=erl_include_paths)

# Build erl_interface static library
env_erl_interface = env.Clone()
# Suppress warnings for erl_interface
env_erl_interface.Append(CCFLAGS=["-w"])

# List erl_interface source files
erl_interface_sources = [
    # Connect
    erl_src_dir + "/connect/ei_connect.c",
    erl_src_dir + "/connect/ei_resolve.c",
    erl_src_dir + "/connect/eirecv.c",
    erl_src_dir + "/connect/send.c",
    erl_src_dir + "/connect/send_exit.c",
    erl_src_dir + "/connect/send_reg.c",
    # Decode
    erl_src_dir + "/decode/decode_atom.c",
    erl_src_dir + "/decode/decode_big.c",
    erl_src_dir + "/decode/decode_bignum.c",
    erl_src_dir + "/decode/decode_binary.c",
    erl_src_dir + "/decode/decode_boolean.c",
    erl_src_dir + "/decode/decode_char.c",
    erl_src_dir + "/decode/decode_double.c",
    erl_src_dir + "/decode/decode_fun.c",
    erl_src_dir + "/decode/decode_intlist.c",
    erl_src_dir + "/decode/decode_iodata.c",
    erl_src_dir + "/decode/decode_list_header.c",
    erl_src_dir + "/decode/decode_long.c",
    erl_src_dir + "/decode/decode_longlong.c",
    erl_src_dir + "/decode/decode_pid.c",
    erl_src_dir + "/decode/decode_port.c",
    erl_src_dir + "/decode/decode_ref.c",
    erl_src_dir + "/decode/decode_skip.c",
    erl_src_dir + "/decode/decode_string.c",
    erl_src_dir + "/decode/decode_trace.c",
    erl_src_dir + "/decode/decode_tuple_header.c",
    erl_src_dir + "/decode/decode_ulong.c",
    erl_src_dir + "/decode/decode_ulonglong.c",
    erl_src_dir + "/decode/decode_version.c",
    # Encode
    erl_src_dir + "/encode/encode_atom.c",
    erl_src_dir + "/encode/encode_big.c",
    erl_src_dir + "/encode/encode_bignum.c",
    erl_src_dir + "/encode/encode_binary.c",
    erl_src_dir + "/encode/encode_boolean.c",
    erl_src_dir + "/encode/encode_char.c",
    erl_src_dir + "/encode/encode_double.c",
    erl_src_dir + "/encode/encode_fun.c",
    erl_src_dir + "/encode/encode_list_header.c",
    erl_src_dir + "/encode/encode_long.c",
    erl_src_dir + "/encode/encode_longlong.c",
    erl_src_dir + "/encode/encode_pid.c",
    erl_src_dir + "/encode/encode_port.c",
    erl_src_dir + "/encode/encode_ref.c",
    erl_src_dir + "/encode/encode_string.c",
    erl_src_dir + "/encode/encode_trace.c",
    erl_src_dir + "/encode/encode_tuple_header.c",
    erl_src_dir + "/encode/encode_ulong.c",
    erl_src_dir + "/encode/encode_ulonglong.c",
    erl_src_dir + "/encode/encode_version.c",
    # EPMD
    erl_src_dir + "/epmd/epmd_port.c",
    erl_src_dir + "/epmd/epmd_publish.c",
    erl_src_dir + "/epmd/epmd_unpublish.c",
    # Global
    erl_src_dir + "/global/global_names.c",
    erl_src_dir + "/global/global_register.c",
    erl_src_dir + "/global/global_unregister.c",
    erl_src_dir + "/global/global_whereis.c",
    # Misc
    erl_src_dir + "/misc/ei_cmp_nc.c",
    erl_src_dir + "/misc/ei_compat.c",
    erl_src_dir + "/misc/ei_decode_term.c",
    erl_src_dir + "/misc/ei_format.c",
    erl_src_dir + "/misc/ei_init.c",
    erl_src_dir + "/misc/ei_locking.c",
    erl_src_dir + "/misc/ei_malloc.c",
    erl_src_dir + "/misc/ei_portio.c",
    erl_src_dir + "/misc/ei_printterm.c",
    erl_src_dir + "/misc/ei_pthreads.c",
    erl_src_dir + "/misc/ei_trace.c",
    erl_src_dir + "/misc/ei_x_encode.c",
    erl_src_dir + "/misc/get_type.c",
    erl_src_dir + "/misc/show_msg.c",
]

erl_interface_obj = []
for src in erl_interface_sources:
    if os.path.exists(src):
        erl_interface_obj.append(env_erl_interface.Object(src))

erl_interface_lib = env_erl_interface.StaticLibrary("libei", erl_interface_obj)

# CNode source files
cnode_sources = Glob("src/*.cpp")

# Combine all sources
sources = list(cnode_sources)

# Link against erl_interface
env.Prepend(LIBS=[erl_interface_lib])
env.Prepend(LIBPATH=["."])

# Build as shared library (GDExtension)
if env["platform"] == "macos":
    library = env.SharedLibrary(
        "bin/addons/godot_cnode/bin/libgodot_cnode{}.framework/libgodot_cnode{}".format(
            env["suffix"], env["suffix"]
        ),
        source=sources,
    )
elif env["platform"] == "ios":
    library = env.SharedLibrary(
        "bin/addons/godot_cnode/bin/libgodot_cnode{}.framework/libgodot_cnode{}".format(
            env["suffix"], env["suffix"]
        ),
        source=sources,
    )
else:
    library = env.SharedLibrary(
        "bin/addons/godot_cnode/bin/libgodot_cnode{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
        source=sources,
    )

Default(library)
