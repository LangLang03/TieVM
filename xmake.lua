set_project("TieVM")
set_version("0.1.0")
set_languages("cxx20")

set_allowedmodes("debug", "release", "sanitize")
set_defaultmode("debug")
add_rules("mode.debug", "mode.release")

add_requires("gtest 1.14.0")

if is_mode("sanitize") then
    set_symbols("debug")
    set_optimize("none")
    add_cxflags("-fsanitize=address,undefined", {force = true})
    add_ldflags("-fsanitize=address,undefined", {force = true})
end

target("tievm_core")
    set_kind("static")
    add_includedirs("include", {public = true})
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
    add_files(
        "src/cli/tiebc_main.cpp",
        "src/cli/tiebc_disasm.cpp",
        "src/cli/tiebc_emit.cpp",
        "src/cli/tiebc_struct.cpp",
        "src/cli/tiebc_dispatch.cpp"
    )
    add_deps("tievm_core", "tievm_std_native")

target("tievm_tests")
    set_kind("binary")
    add_includedirs("include", "tests")
    add_files("tests/test_main.cpp", "tests/unit/*.cpp", "tests/smoke/*.cpp")
    add_packages("gtest")
    add_deps("tievm_core", "tievm_std_native")
