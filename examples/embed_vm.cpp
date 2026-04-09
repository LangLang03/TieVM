#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tie/vm/api.hpp"

namespace {

using tie::vm::AccessModifier;
using tie::vm::BytecodeAccessModifier;
using tie::vm::BytecodeClassDecl;
using tie::vm::ClassDescriptor;
using tie::vm::ErrorCode;
using tie::vm::MethodDescriptor;
using tie::vm::Module;
using tie::vm::ObjectId;
using tie::vm::Status;
using tie::vm::Value;
using tie::vm::VmInstance;

void PrintUsage() {
#if defined(TIEVM_ENABLE_HELP) && !defined(TIEVM_MINIMAL_STRINGS)
    std::cerr << "Usage:\n";
    std::cerr << "  tievm_embed [--validate]\n";
    std::cerr << "  tievm_embed [--validate] <file.tbc>\n";
    std::cerr << "  tievm_embed [--validate] <file.tlb|file.tlbs> [module_name]\n";
#endif
}

AccessModifier ToRuntimeAccess(BytecodeAccessModifier access) {
    switch (access) {
        case BytecodeAccessModifier::kPublic:
            return AccessModifier::kPublic;
        case BytecodeAccessModifier::kProtected:
            return AccessModifier::kProtected;
        case BytecodeAccessModifier::kPrivate:
            return AccessModifier::kPrivate;
    }
    return AccessModifier::kPublic;
}

Status RegisterModuleClasses(VmInstance& vm, std::shared_ptr<const Module> module_ref) {
    std::vector<const BytecodeClassDecl*> pending;
    pending.reserve(module_ref->classes().size());
    for (const auto& class_decl : module_ref->classes()) {
        pending.push_back(&class_decl);
    }

    while (!pending.empty()) {
        bool progressed = false;
        for (size_t i = 0; i < pending.size();) {
            const auto* class_decl = pending[i];
            ClassDescriptor descriptor;
            descriptor.name = class_decl->name;
            descriptor.base_classes = class_decl->base_classes;
            descriptor.methods.reserve(class_decl->methods.size());
            for (const auto& method_decl : class_decl->methods) {
                MethodDescriptor method;
                method.name = method_decl.name;
                method.access = ToRuntimeAccess(method_decl.access);
                method.is_virtual = method_decl.is_virtual;
                const uint32_t function_index = method_decl.function_index;
                method.body = [&vm, module_ref, function_index](
                                  ObjectId object_id,
                                  const std::vector<Value>& args) -> tie::vm::StatusOr<Value> {
                    std::vector<Value> call_args;
                    call_args.reserve(args.size() + 1);
                    call_args.push_back(Value::Object(object_id));
                    call_args.insert(call_args.end(), args.begin(), args.end());
                    auto thread = vm.CreateThread();
                    return thread.Execute(*module_ref, function_index, call_args);
                };
                descriptor.methods.insert({method.name, std::move(method)});
            }

            auto status = vm.reflection().RegisterClass(std::move(descriptor));
            if (status.ok() || status.code() == ErrorCode::kAlreadyExists) {
                pending.erase(pending.begin() + static_cast<ptrdiff_t>(i));
                progressed = true;
                continue;
            }
            if (status.code() == ErrorCode::kNotFound) {
                ++i;
                continue;
            }
            return status;
        }

        if (!progressed) {
            return Status::InvalidState(
                "failed registering class metadata due to unresolved base classes");
        }
    }
    return Status::Ok();
}

std::string RenderValue(VmInstance& vm, const Value& value) {
    if (value.type() == Value::Type::kString) {
        auto text_or = vm.ResolveString(value);
        if (text_or.ok()) {
            return text_or.value();
        }
    }
    return value.ToString();
}

void PrintError(const Status& status) {
#if !defined(TIEVM_MINIMAL_STRINGS)
    std::cerr << "runtime failed: ";
    if (status.vm_error().has_value()) {
        std::cerr << status.vm_error()->Format() << "\n";
    } else {
        std::cerr << status.message() << "\n";
    }
#else
    (void)status;
#endif
}

tie::vm::StatusOr<Value> ExecuteBytecodeModule(VmInstance& vm, Module module) {
    auto module_ref = std::make_shared<Module>(std::move(module));
    auto class_status = RegisterModuleClasses(vm, module_ref);
    if (!class_status.ok()) {
        return class_status;
    }
    return vm.ExecuteModule(*module_ref);
}

Module BuildEmbeddedDemoModule() {
    Module module("embed.demo");
    const auto c_40 = module.AddConstant(tie::vm::Constant::Int64(40));
    const auto c_2 = module.AddConstant(tie::vm::Constant::Int64(2));
    const auto c_demo_class = module.AddConstant(tie::vm::Constant::Utf8("EmbedDemo"));
    const auto c_plus1_method = module.AddConstant(tie::vm::Constant::Utf8("plus1"));

    auto& add2 = module.AddFunction("add2", 4, 2);
    auto& add2_bb = add2.AddBlock("entry");
    tie::vm::InstructionBuilder(add2_bb).Add(0, 0, 1).Ret(0);

    auto& plus1 = module.AddFunction("plus1_impl", 4, 2);
    auto& plus1_bb = plus1.AddBlock("entry");
    tie::vm::InstructionBuilder(plus1_bb).AddImm(0, 1, 1).Ret(0);

    BytecodeClassDecl embed_demo;
    embed_demo.name = "EmbedDemo";
    embed_demo.methods.push_back(tie::vm::BytecodeMethodDecl{
        "plus1",
        1,
        tie::vm::BytecodeAccessModifier::kPublic,
        true,
    });
    module.AddClass(std::move(embed_demo));

    auto& entry = module.AddFunction("entry", 12, 0);
    auto& entry_bb = entry.AddBlock("entry");
    tie::vm::InstructionBuilder(entry_bb)
        .LoadK(1, c_40)
        .LoadK(2, c_2)
        .Call(0, 0, 2)
        .NewObject(3, c_demo_class)
        .Mov(4, 0)
        .Invoke(3, c_plus1_method, 1)
        .Add(0, 0, 3)
        .Ret(0);
    module.set_entry_function(2);
    return module;
}

int RunEmbeddedDemo(VmInstance& vm) {
    auto result_or = ExecuteBytecodeModule(vm, BuildEmbeddedDemoModule());
    if (!result_or.ok()) {
        PrintError(result_or.status());
        return 2;
    }
    std::cout << RenderValue(vm, result_or.value()) << "\n";
    return 0;
}

int RunTbcFile(VmInstance& vm, const std::filesystem::path& input) {
    const auto stdlib_candidate = input.parent_path() / "stdlib.tlbs";
    if (std::filesystem::exists(stdlib_candidate)) {
        auto status = vm.loader().LoadTlbsFile(stdlib_candidate);
        if (!status.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
            std::cerr << "warning: auto-load stdlib.tlbs failed: " << status.message() << "\n";
#endif
        }
    }

    auto module_or = tie::vm::Serializer::DeserializeFromFile(input);
    if (!module_or.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
#endif
        return 2;
    }
    auto result_or = ExecuteBytecodeModule(vm, std::move(module_or.value()));
    if (!result_or.ok()) {
        PrintError(result_or.status());
        return 3;
    }
    std::cout << RenderValue(vm, result_or.value()) << "\n";
    return 0;
}

int RunTlbFile(
    VmInstance& vm, const std::filesystem::path& input,
    const std::optional<std::string>& module_name_override) {
    auto status = Status::Ok();
    if (input.extension() == ".tlbs") {
        status = vm.loader().LoadTlbsFile(input);
    } else {
        status = vm.loader().LoadTlbFile(input);
    }
    if (!status.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "bundle load failed: " << status.message() << "\n";
#endif
        return 4;
    }

    std::string module_name;
    if (module_name_override.has_value()) {
        module_name = *module_name_override;
    } else {
        auto names = vm.loader().ActiveModuleNames();
        if (names.empty()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
            std::cerr << "bundle has no module\n";
#endif
            return 5;
        }
        module_name = names.front();
    }

    auto module_or = vm.loader().GetModule(module_name);
    if (!module_or.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "module not found: " << module_name << "\n";
#endif
        return 6;
    }
    auto module_ref = std::make_shared<Module>(std::move(module_or.value()));
    auto class_status = RegisterModuleClasses(vm, module_ref);
    if (!class_status.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "register class metadata failed: " << class_status.message() << "\n";
#endif
        return 7;
    }

    auto result_or = vm.ExecuteLoadedModule(module_name);
    if (!result_or.ok()) {
        PrintError(result_or.status());
        return 8;
    }
    std::cout << RenderValue(vm, result_or.value()) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool enable_validate = false;
    std::optional<std::filesystem::path> input;
    std::optional<std::string> module_name_override;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--validate") {
            enable_validate = true;
            continue;
        }
        if (!input.has_value()) {
            input = std::filesystem::path(arg);
            continue;
        }
        if (!module_name_override.has_value()) {
            module_name_override = arg;
            continue;
        }
        PrintUsage();
        return 1;
    }

    VmInstance vm;
    vm.SetRuntimeValidationEnabled(enable_validate);

    if (!input.has_value()) {
        return RunEmbeddedDemo(vm);
    }

    const auto ext = input->extension().string();
    if (ext == ".tbc") {
        return RunTbcFile(vm, *input);
    }
    if (ext == ".tlb" || ext == ".tlbs") {
        return RunTlbFile(vm, *input, module_name_override);
    }

#if !defined(TIEVM_MINIMAL_STRINGS)
    std::cerr << "unsupported file extension: " << ext << "\n";
#endif
    PrintUsage();
    return 1;
}
