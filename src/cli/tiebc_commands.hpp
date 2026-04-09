#pragma once

#include <cstdint>
#include <filesystem>

namespace tie::vm::cli {

int RunTiebc(int argc, char** argv);
int Usage();
int Check(const std::filesystem::path& path);
int DisasmTbc(const std::filesystem::path& path);
int DisasmTlb(const std::filesystem::path& path);
int EmitHello(const std::filesystem::path& path);
int EmitOpset(const std::filesystem::path& path);
int EmitClosureUpvalue(const std::filesystem::path& path);
int EmitFib(const std::filesystem::path& path, int64_t n);
int EmitErrorHandling(const std::filesystem::path& path);
int EmitOopOk(const std::filesystem::path& path);
int EmitOopError(const std::filesystem::path& path);
int PrintTlbStruct();
int PrintTbcStruct();
int PrintTlbsStruct();
int BuildStdlibTlbsCmd(const std::filesystem::path& path);
int CheckTlbs(const std::filesystem::path& path);
int PackTlbs(const std::filesystem::path& input_dir, const std::filesystem::path& output_file);
int UnpackTlbs(
    const std::filesystem::path& input_file, const std::filesystem::path& output_dir);

}  // namespace tie::vm::cli
