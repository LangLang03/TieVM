#include <gtest/gtest.h>

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

}  // namespace tie::vm
