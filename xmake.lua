set_project("TieVM")
set_version("0.1.0")
set_languages("cxx20")

set_allowedmodes("debug", "release", "sanitize")
set_defaultmode("debug")
add_rules("mode.debug", "mode.release")

add_requires("gtest 1.14.0")
add_requires("zlib")

option("cli_help")
    set_default(true)
    set_showmenu(true)
    set_description("Compile CLI help/usage text")
option_end()

option("minimal_strings")
    set_default(false)
    set_showmenu(true)
    set_description("Strip user-facing strings from tievm runtime CLIs")
option_end()

if is_mode("sanitize") then
    set_symbols("debug")
    set_optimize("none")
    if is_plat("windows") then
        add_defines("TIEVM_SANITIZE_STUB=1")
    else
        add_cxflags("-fsanitize=address,undefined", {force = true})
        add_ldflags("-fsanitize=address,undefined", {force = true})
    end
end

target("tievm_core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_packages("zlib", {public = true})
    add_headerfiles("include/(tie/**.hpp)")
    add_files(
        "src/bytecode/*.cpp",
        "src/runtime/*.cpp",
        "src/gc/*.cpp",
        "src/ffi/*.cpp",
        "src/tlb/*.cpp",
        "src/utf8/*.cpp",
        "src/aot/*.cpp",
        "src/stdlib/*.cpp"
    )
    set_warnings("allextra")
    if is_mode("release") then
        set_optimize("faster")
    end

target("tievm_std_native")
    set_kind("shared")
    add_includedirs("include")
    add_files("src/stdlib/native/tievm_std_native.cpp")
    set_basename("tievm_std_native")

target("tievm")
    set_kind("binary")
    add_includedirs("include")
    if has_config("cli_help") then
        add_defines("TIEVM_ENABLE_HELP=1")
    end
    if has_config("minimal_strings") then
        add_defines("TIEVM_MINIMAL_STRINGS=1")
    end
    add_files(
        "src/cli/tievm_main.cpp",
        "src/cli/tievm_dispatch.cpp",
        "src/cli/tievm_run_tbc.cpp",
        "src/cli/tievm_run_tlb.cpp"
    )
    add_deps("tievm_core", "tievm_std_native")

target("tiebc")
    set_kind("binary")
    add_includedirs("include")
    if has_config("cli_help") then
        add_defines("TIEVM_ENABLE_HELP=1")
    end
    add_files(
        "src/cli/tiebc_main.cpp",
        "src/cli/tiebc_disasm.cpp",
        "src/cli/tiebc_emit.cpp",
        "src/cli/tiebc_aot.cpp",
        "src/cli/tiebc_struct.cpp",
        "src/cli/tiebc_dispatch.cpp"
    )
    add_deps("tievm_core", "tievm_std_native")

target("tievm_tests")
    set_kind("binary")
    add_includedirs("include", "tests", "src/cli")
    add_files(
        "tests/test_main.cpp",
        "tests/unit/*.cpp",
        "tests/smoke/*.cpp",
        "src/cli/tiebc_emit.cpp"
    )
    add_packages("gtest")
    add_deps("tievm_core", "tievm_std_native")

target("tievm_perf")
    set_kind("binary")
    add_includedirs("include")
    add_files("benchmarks/tievm_perf.cpp")
    add_deps("tievm_core")
    set_targetdir("artifacts/perf")

target("tievm_embed")
    set_kind("binary")
    add_includedirs("include")
    if has_config("cli_help") then
        add_defines("TIEVM_ENABLE_HELP=1")
    end
    if has_config("minimal_strings") then
        add_defines("TIEVM_MINIMAL_STRINGS=1")
    end
    add_files("examples/embed_vm.cpp")
    add_deps("tievm_core")
    set_rundir("$(projectdir)")

target("tievm_vmp_auth")
    set_kind("binary")
    add_includedirs("include")
    add_files("examples/vmp_auth.cpp")
    add_deps("tievm_core")
    set_rundir("$(projectdir)")
    if is_mode("release") then
        set_symbols("none")
        if not is_plat("windows") then
            set_strip("all")
        else
            add_ldflags("/DEBUG:NONE", {force = true})
        end
    end
