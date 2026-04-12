#include "tievm_commands.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <unordered_set>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

StatusOr<AccessModifier> ToRuntimeAccess(BytecodeAccessModifier access) {
    switch (access) {
        case BytecodeAccessModifier::kPublic:
            return StatusOr<AccessModifier>(AccessModifier::kPublic);
        case BytecodeAccessModifier::kProtected:
            return StatusOr<AccessModifier>(AccessModifier::kProtected);
        case BytecodeAccessModifier::kPrivate:
            return StatusOr<AccessModifier>(AccessModifier::kPrivate);
    }
    return Status::InvalidArgument("invalid class access modifier in metadata");
}

StatusOr<ClassDescriptor> BuildClassDescriptor(
    VmInstance& vm, std::shared_ptr<const Module> module_ref,
    const BytecodeClassDecl& class_decl) {
    ClassDescriptor descriptor;
    descriptor.name = class_decl.name;
    descriptor.base_classes = class_decl.base_classes;
    descriptor.methods.reserve(class_decl.methods.size());
    for (const auto& method_decl : class_decl.methods) {
        auto access_or = ToRuntimeAccess(method_decl.access);
        if (!access_or.ok()) {
            return access_or.status();
        }
        MethodDescriptor method;
        method.name = method_decl.name;
        method.access = access_or.value();
        method.is_virtual = method_decl.is_virtual;
        const uint32_t function_index = method_decl.function_index;
        method.body = [&vm, module_ref, function_index](
                          ObjectId object_id, const std::vector<Value>& args) -> StatusOr<Value> {
            std::vector<Value> call_args;
            call_args.reserve(args.size() + 1);
            call_args.push_back(Value::Object(object_id));
            call_args.insert(call_args.end(), args.begin(), args.end());
            auto thread = vm.CreateThread();
            return thread.Execute(*module_ref, function_index, call_args);
        };
        descriptor.methods.insert({method.name, std::move(method)});
    }
    return descriptor;
}

Status RegisterModuleClasses(VmInstance& vm, std::shared_ptr<const Module> module_ref) {
    auto existing_or = vm.reflection().ListClasses();
    if (!existing_or.ok()) {
        return existing_or.status();
    }
    std::unordered_set<std::string> available(
        existing_or.value().begin(), existing_or.value().end());

    std::vector<const BytecodeClassDecl*> pending;
    pending.reserve(module_ref->classes().size());
    for (const auto& class_decl : module_ref->classes()) {
        pending.push_back(&class_decl);
    }

    while (!pending.empty()) {
        bool progressed = false;
        std::vector<const BytecodeClassDecl*> next_round;
        next_round.reserve(pending.size());
        for (const auto* class_decl : pending) {
            bool ready = true;
            for (const auto& base_name : class_decl->base_classes) {
                if (!available.contains(base_name)) {
                    ready = false;
                    break;
                }
            }
            if (!ready) {
                next_round.push_back(class_decl);
                continue;
            }

            auto descriptor_or = BuildClassDescriptor(vm, module_ref, *class_decl);
            if (!descriptor_or.ok()) {
                return descriptor_or.status();
            }
            auto status = vm.reflection().RegisterClass(std::move(descriptor_or.value()));
            if (!status.ok() && status.code() != ErrorCode::kAlreadyExists) {
                return status;
            }
            available.insert(class_decl->name);
            progressed = true;
        }

        if (!progressed) {
            std::ostringstream oss;
            oss << "class metadata has unresolved bases or cycle: ";
            for (size_t i = 0; i < next_round.size(); ++i) {
                if (i > 0) {
                    oss << ", ";
                }
                oss << next_round[i]->name;
            }
            return Status::InvalidState(oss.str());
        }
        pending = std::move(next_round);
    }
    return Status::Ok();
}

}  // namespace

int RunTlbFile(
    VmInstance& vm,
    const std::filesystem::path& input,
    const std::optional<std::string>& module_name_override,
    const RunConfig& config) {
    ModuleLoader::TlbsLoadOptions tlbs_options;
    tlbs_options.deserialize_options.verify = !config.trusted;
    tlbs_options.cache_dir = config.cache_dir;
    ModuleLoader::TlbLoadOptions tlb_options;
    tlb_options.deserialize_options.verify = !config.trusted;

    std::optional<std::string> manifest_entry_module;
    Status status = Status::Ok();
    if (input.extension() == ".tlbs") {
        auto bundle_or = TlbsBundle::Deserialize(input);
        if (!bundle_or.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
            std::cerr << "bundle load failed: " << bundle_or.status().message() << "\n";
#endif
            return 4;
        }
        manifest_entry_module = bundle_or.value().manifest().entry_module;
        status = vm.loader().LoadTlbsFile(input, tlbs_options);
    } else {
        status = vm.loader().LoadTlbFile(input, tlb_options);
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
        if (manifest_entry_module.has_value()) {
            const auto file_name = std::filesystem::path(*manifest_entry_module).filename().string();
            const auto ext = std::filesystem::path(file_name).extension().string();
            if (ext == ".tbc") {
                module_name = std::filesystem::path(file_name).stem().string();
            }
        }
        auto names = vm.loader().ActiveModuleNames();
        if (names.empty()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
            std::cerr << "bundle has no module\n";
#endif
            return 5;
        }
        if (!module_name.empty()) {
            const bool found =
                std::find(names.begin(), names.end(), module_name) != names.end();
            if (!found) {
                module_name.clear();
            }
        }
        if (module_name.empty()) {
            module_name = names.front();
        }
    }

    auto module_or = vm.loader().GetModule(module_name);
    if (!module_or.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "module load failed: " << module_or.status().message() << "\n";
#endif
        return 6;
    }
    auto module_ref = std::make_shared<Module>(std::move(module_or.value()));
    auto register_status = RegisterModuleClasses(vm, module_ref);
    if (!register_status.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "register class metadata failed: " << register_status.message() << "\n";
#endif
        return 6;
    }

    auto value_or = vm.ExecuteLoadedModule(module_name);
    if (!value_or.ok()) {
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "runtime failed: ";
        if (value_or.status().vm_error().has_value()) {
            std::cerr << value_or.status().vm_error()->Format() << "\n";
        } else {
            std::cerr << value_or.status().message() << "\n";
        }
#endif
        return 6;
    }
    std::cout << value_or.value().ToString() << "\n";
    return 0;
}

}  // namespace tie::vm::cli
