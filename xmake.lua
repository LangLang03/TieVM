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

target("tievm")
    set_kind("binary")
    add_includedirs("include")
    add_files("src/cli/tievm_main.cpp")
    add_deps("tievm_core")

target("tiebc")
    set_kind("binary")
    add_includedirs("include")
    add_files("src/cli/tiebc_main.cpp")
    add_deps("tievm_core")

target("tievm_tests")
    set_kind("binary")
    add_includedirs("include", "tests")
    add_files("tests/test_main.cpp", "tests/unit/*.cpp", "tests/smoke/*.cpp")
    add_packages("gtest")
    add_deps("tievm_core")
