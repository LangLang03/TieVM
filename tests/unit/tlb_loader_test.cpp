#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "test_helpers.hpp"

namespace tie::vm {

TEST(TlbLoaderTest, LoadTlbAndListModules) {
    TlbContainer container;
    auto module = test::BuildAddModule(1, 2);
    module.version() = SemanticVersion{0, 1, 0};
    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();

    TlbModuleEntry entry;
    entry.module_name = module.name();
    entry.version = module.version();
    entry.bytecode = std::move(bytes_or.value());
    entry.native_plugins = {"plugin_stub"};
    container.AddModule(std::move(entry));

    const auto path = test::TempPath("module.tlb");
    ASSERT_TRUE(container.SerializeToFile(path).ok());

    ModuleLoader loader;
    auto status = loader.LoadTlbFile(path);
    ASSERT_TRUE(status.ok()) << status.message();
    auto names = loader.ActiveModuleNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names.front(), "test.math");
}

TEST(TlbLoaderTest, HotReloadRejectsVersionConflictForSameNameInBatch) {
    ModuleLoader loader;
    auto session = loader.BeginHotReload();

    LoadedModule a{
        "test.mod",
        SemanticVersion{0, 1, 0},
        test::BuildAddModule(1, 1),
    };
    a.module.version() = a.version;
    LoadedModule b{
        "test.mod",
        SemanticVersion{0, 2, 0},
        test::BuildAddModule(2, 2),
    };
    b.module.version() = b.version;

    session.Stage(std::move(a));
    session.Stage(std::move(b));
    auto status = session.Commit();
    EXPECT_FALSE(status.ok());
}

TEST(TlbLoaderTest, TlbsDirectoryAndZipRoundTrip) {
    auto bundle_or = BuildStdlibBundle();
    ASSERT_TRUE(bundle_or.ok()) << bundle_or.status().message();

    const auto dir = test::TempPath("stdlib_dir.tlbs");
    ASSERT_TRUE(bundle_or.value().SerializeToDirectory(dir).ok());
    const auto dir_loaded_or = TlbsBundle::Deserialize(dir);
    ASSERT_TRUE(dir_loaded_or.ok()) << dir_loaded_or.status().message();
    EXPECT_EQ(
        dir_loaded_or.value().manifest().modules.size(),
        bundle_or.value().manifest().modules.size());

    const auto zip = test::TempPath("stdlib_zip.tlbs");
    ASSERT_TRUE(bundle_or.value().SerializeToZip(zip).ok());
    const auto zip_loaded_or = TlbsBundle::Deserialize(zip);
    ASSERT_TRUE(zip_loaded_or.ok()) << zip_loaded_or.status().message();
    EXPECT_EQ(
        zip_loaded_or.value().manifest().modules.size(),
        bundle_or.value().manifest().modules.size());
}

TEST(TlbLoaderTest, ModuleLoaderCanLoadTlbsBundle) {
    auto bundle_or = BuildStdlibBundle();
    ASSERT_TRUE(bundle_or.ok()) << bundle_or.status().message();
    const auto dir = test::TempPath("loader_stdlib.tlbs");
    ASSERT_TRUE(bundle_or.value().SerializeToDirectory(dir).ok());

    ModuleLoader loader;
    ASSERT_TRUE(loader.LoadTlbsFile(dir).ok());
    const auto names = loader.ActiveModuleNames();
    EXPECT_FALSE(names.empty());
}

TEST(TlbLoaderTest, ModuleLoaderCachesZipBundleAndRepairsMissingManifest) {
    auto bundle_or = BuildStdlibBundle();
    ASSERT_TRUE(bundle_or.ok()) << bundle_or.status().message();

    const auto zip = test::TempPath("loader_stdlib_cache_zip.tlbs");
    ASSERT_TRUE(bundle_or.value().SerializeToZip(zip).ok());

    const auto cache_root = test::TempPath("loader_stdlib_cache");
    ModuleLoader::TlbsLoadOptions options;
    options.cache_dir = cache_root;
    options.enable_cache = true;

    ModuleLoader loader_first;
    ASSERT_TRUE(loader_first.LoadTlbsFile(zip, options).ok());

    std::vector<std::filesystem::path> manifests;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(cache_root)) {
        if (entry.is_regular_file() &&
            entry.path().filename() == "manifest.toml") {
            manifests.push_back(entry.path());
        }
    }
    ASSERT_FALSE(manifests.empty());
    const auto cached_bundle_dir = manifests.front().parent_path();
    ASSERT_TRUE(std::filesystem::remove(cached_bundle_dir / "manifest.toml"));

    ModuleLoader loader_second;
    ASSERT_TRUE(loader_second.LoadTlbsFile(zip, options).ok());
    EXPECT_TRUE(std::filesystem::exists(cached_bundle_dir / "manifest.toml"));
}

TEST(TlbLoaderTest, TlbsManifestEntryModuleRoundTrip) {
    Module module = test::BuildAddModule(4, 5);
    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();

    TlbsBundle bundle;
    bundle.manifest().name = "custom.bundle";
    bundle.manifest().version = SemanticVersion{0, 2, 0};
    bundle.manifest().modules = {"modules/test.math.tbc"};
    bundle.manifest().entry_module = "modules/test.math.tbc";
    bundle.SetModule("modules/test.math.tbc", std::move(bytes_or.value()));

    const auto dir = test::TempPath("entry_module_roundtrip.tlbs");
    ASSERT_TRUE(bundle.SerializeToDirectory(dir).ok());

    auto loaded_or = TlbsBundle::Deserialize(dir);
    ASSERT_TRUE(loaded_or.ok()) << loaded_or.status().message();
    ASSERT_TRUE(loaded_or.value().manifest().entry_module.has_value());
    EXPECT_EQ(*loaded_or.value().manifest().entry_module, "modules/test.math.tbc");
}

TEST(TlbLoaderTest, TlbsDeserializeRejectsTraversalModulePath) {
    const auto dir = test::TempPath("bad_manifest_path.tlbs");
    std::filesystem::create_directories(dir);
    std::ofstream manifest(dir / "manifest.toml", std::ios::trunc);
    ASSERT_TRUE(manifest.is_open());
    manifest << "name = \"bad.bundle\"\n";
    manifest << "version = \"0.1.0\"\n";
    manifest << "modules = [\"../evil.tbc\"]\n";
    manifest.close();

    auto loaded_or = TlbsBundle::Deserialize(dir);
    EXPECT_FALSE(loaded_or.ok());
}

TEST(TlbLoaderTest, TlbsSerializeRejectsTraversalModulePath) {
    Module module = test::BuildAddModule(7, 8);
    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();

    TlbsBundle bundle;
    bundle.manifest().name = "bad.serialize.bundle";
    bundle.manifest().modules = {"../evil.tbc"};
    bundle.SetModule("../evil.tbc", std::move(bytes_or.value()));

    const auto dir = test::TempPath("bad_serialize_path.tlbs");
    auto status = bundle.SerializeToDirectory(dir);
    EXPECT_FALSE(status.ok());
}

TEST(TlbLoaderTest, ModuleLoaderRejectsEscapingFfiLibraryPath) {
    Module module("bad.ffi.path");
    module.AddFfiLibraryPath("../evil.so");
    FunctionSignature signature;
    signature.name = "bad.sym";
    signature.convention = CallingConvention::kSystem;
    signature.return_type = {AbiValueKind::kVoid, OwnershipQualifier::kBorrowed};
    const auto sig_idx = module.AddFfiSignature(std::move(signature));
    module.AddFfiBinding(FfiSymbolBinding{"bad.sym", "bad_native", 0, sig_idx});
    auto& fn = module.AddFunction("entry", 2, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).Ret(0);
    module.set_entry_function(0);

    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();

    TlbsBundle bundle;
    bundle.manifest().name = "bad.ffi.bundle";
    bundle.manifest().modules = {"modules/bad.tbc"};
    bundle.SetModule("modules/bad.tbc", std::move(bytes_or.value()));

    const auto dir = test::TempPath("bad_ffi_loader.tlbs");
    ASSERT_TRUE(bundle.SerializeToDirectory(dir).ok());

    ModuleLoader loader;
    auto status = loader.LoadTlbsFile(dir);
    EXPECT_FALSE(status.ok());
}

}  // namespace tie::vm
