#include "tievm_commands.hpp"

#include <iostream>
#include <memory>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

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
    for (const auto& class_decl : module_ref->classes()) {
        ClassDescriptor descriptor;
        descriptor.name = class_decl.name;
        descriptor.base_classes = class_decl.base_classes;
        descriptor.methods.reserve(class_decl.methods.size());
        for (const auto& method_decl : class_decl.methods) {
            MethodDescriptor method;
            method.name = method_decl.name;
            method.access = ToRuntimeAccess(method_decl.access);
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

        auto status = vm.reflection().RegisterClass(std::move(descriptor));
        if (!status.ok() && status.code() != ErrorCode::kAlreadyExists) {
            return status;
        }
    }
    return Status::Ok();
}

}  // namespace

int RunTbcFile(VmInstance& vm, const std::filesystem::path& input) {
    const auto stdlib_candidate = input.parent_path() / "stdlib.tlbs";
    if (std::filesystem::exists(stdlib_candidate)) {
        auto load_status = vm.loader().LoadTlbsFile(stdlib_candidate);
        if (!load_status.ok()) {
            std::cerr << "warning: auto-load stdlib.tlbs failed: " << load_status.message()
                      << "\n";
        }
    }

    auto module_or = Serializer::DeserializeFromFile(input);
    if (!module_or.ok()) {
        std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
        return 2;
    }

    auto module_ref = std::make_shared<Module>(std::move(module_or.value()));
    auto register_status = RegisterModuleClasses(vm, module_ref);
    if (!register_status.ok()) {
        std::cerr << "register class metadata failed: " << register_status.message() << "\n";
        return 3;
    }

    auto value_or = vm.ExecuteModule(*module_ref);
    if (!value_or.ok()) {
        std::cerr << "runtime failed: ";
        if (value_or.status().vm_error().has_value()) {
            std::cerr << value_or.status().vm_error()->Format() << "\n";
        } else {
            std::cerr << value_or.status().message() << "\n";
        }
        return 4;
    }

    std::cout << value_or.value().ToString() << "\n";
    return 0;
}

}  // namespace tie::vm::cli
