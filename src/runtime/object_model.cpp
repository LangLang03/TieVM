#include "tie/vm/runtime/object_model.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>

#include "tie/vm/gc/gc_controller.hpp"

namespace tie::vm {

namespace {

using ClassMap = std::unordered_map<std::string, ClassDescriptor>;

StatusOr<std::vector<std::string>> MergeC3Impl(std::vector<std::vector<std::string>> seqs) {
    std::vector<std::string> result;
    while (true) {
        seqs.erase(
            std::remove_if(
                seqs.begin(), seqs.end(),
                [](const auto& s) { return s.empty(); }),
            seqs.end());
        if (seqs.empty()) {
            return result;
        }

        std::optional<std::string> candidate;
        for (const auto& seq : seqs) {
            const auto& head = seq.front();
            bool in_tail = false;
            for (const auto& other : seqs) {
                if (&other == &seq || other.size() < 2) {
                    continue;
                }
                if (std::find(other.begin() + 1, other.end(), head) != other.end()) {
                    in_tail = true;
                    break;
                }
            }
            if (!in_tail) {
                candidate = head;
                break;
            }
        }
        if (!candidate.has_value()) {
            return Status::InvalidState("C3 linearization failed due to inconsistent hierarchy");
        }

        result.push_back(candidate.value());
        for (auto& seq : seqs) {
            if (!seq.empty() && seq.front() == candidate.value()) {
                seq.erase(seq.begin());
            }
        }
    }
}

StatusOr<std::vector<std::string>> LinearizeClass(
    const ClassMap& classes, const std::string& class_name,
    std::unordered_map<std::string, std::vector<std::string>>* memo,
    std::unordered_set<std::string>* visiting) {
    if (auto it = memo->find(class_name); it != memo->end()) {
        return it->second;
    }
    if (!classes.contains(class_name)) {
        return Status::NotFound("class not found: " + class_name);
    }
    if (visiting->contains(class_name)) {
        return Status::InvalidState("class inheritance cycle detected at: " + class_name);
    }
    visiting->insert(class_name);

    const auto& klass = classes.at(class_name);
    std::vector<std::vector<std::string>> seqs;
    for (const auto& base : klass.base_classes) {
        auto base_lin_or = LinearizeClass(classes, base, memo, visiting);
        if (!base_lin_or.ok()) {
            return base_lin_or.status();
        }
        seqs.push_back(base_lin_or.value());
    }
    seqs.push_back(klass.base_classes);

    auto merged_or = MergeC3Impl(std::move(seqs));
    if (!merged_or.ok()) {
        return merged_or.status();
    }
    std::vector<std::string> out;
    out.push_back(class_name);
    for (const auto& item : merged_or.value()) {
        out.push_back(item);
    }
    (*memo)[class_name] = out;
    visiting->erase(class_name);
    return out;
}

}  // namespace

Status ObjectModel::RegisterClass(ClassDescriptor descriptor) {
    std::lock_guard<std::mutex> lock(mu_);
    if (classes_.contains(descriptor.name)) {
        return Status::AlreadyExists("class already registered: " + descriptor.name);
    }
    for (const auto& base : descriptor.base_classes) {
        if (!classes_.contains(base)) {
            return Status::NotFound("base class not found: " + base);
        }
    }
    classes_.insert({descriptor.name, std::move(descriptor)});
    mro_cache_.clear();
    return Status::Ok();
}

StatusOr<ObjectId> ObjectModel::NewObject(std::string_view class_name) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string name(class_name);
    if (!classes_.contains(name)) {
        return Status::NotFound("class not found: " + name);
    }
    ObjectId id = next_object_id_++;
    if (gc_ != nullptr) {
        auto id_or = gc_->Allocate();
        if (!id_or.ok()) {
            return id_or.status();
        }
        id = id_or.value();
    }
    objects_[id] = ObjectInstance{id, name, {}};
    return id;
}

Status ObjectModel::SetField(ObjectId object_id, std::string_view field, Value value) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = objects_.find(object_id);
    if (it == objects_.end()) {
        return Status::NotFound("object not found");
    }
    const std::string field_name(field);
    if (gc_ != nullptr) {
        auto prev = it->second.fields.find(field_name);
        if (prev != it->second.fields.end() &&
            prev->second.type() == Value::Type::kObject) {
            (void)gc_->RemoveReference(object_id, prev->second.AsObject());
        }
        if (value.type() == Value::Type::kObject) {
            auto add_status = gc_->AddReference(object_id, value.AsObject());
            if (!add_status.ok()) {
                return add_status;
            }
        }
    }
    it->second.fields[field_name] = value;
    return Status::Ok();
}

StatusOr<Value> ObjectModel::GetField(ObjectId object_id, std::string_view field) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto obj_it = objects_.find(object_id);
    if (obj_it == objects_.end()) {
        return Status::NotFound("object not found");
    }
    auto f_it = obj_it->second.fields.find(std::string(field));
    if (f_it == obj_it->second.fields.end()) {
        return Status::NotFound("field not found");
    }
    return f_it->second;
}

StatusOr<Value> ObjectModel::Invoke(
    ObjectId object_id, std::string_view method, const std::vector<Value>& args,
    bool allow_private) const {
    std::function<StatusOr<Value>(ObjectId, const std::vector<Value>&)> call;
    const std::string method_name(method);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto obj_it = objects_.find(object_id);
        if (obj_it == objects_.end()) {
            return Status::NotFound("object not found");
        }

        auto mro_it = mro_cache_.find(obj_it->second.class_name);
        if (mro_it == mro_cache_.end()) {
            std::unordered_map<std::string, std::vector<std::string>> memo;
            std::unordered_set<std::string> visiting;
            auto mro_or =
                LinearizeClass(classes_, obj_it->second.class_name, &memo, &visiting);
            if (!mro_or.ok()) {
                return mro_or.status();
            }
            mro_it =
                mro_cache_.emplace(obj_it->second.class_name, std::move(mro_or.value())).first;
        }

        for (const auto& klass_name : mro_it->second) {
            const auto& klass = classes_.at(klass_name);
            auto m_it = klass.methods.find(method_name);
            if (m_it == klass.methods.end()) {
                continue;
            }
            if (m_it->second.access == AccessModifier::kPrivate && !allow_private) {
                return Status::InvalidState("method is private");
            }
            call = m_it->second.body;
            break;
        }
    }

    if (!call) {
        return Status::NotFound("method not found");
    }
    return call(object_id, args);
}

StatusOr<std::vector<std::string>> ObjectModel::ComputeMro(std::string_view class_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string class_key(class_name);
    auto it = mro_cache_.find(class_key);
    if (it != mro_cache_.end()) {
        return it->second;
    }
    std::unordered_map<std::string, std::vector<std::string>> memo;
    std::unordered_set<std::string> visiting;
    auto mro_or = LinearizeClass(classes_, class_key, &memo, &visiting);
    if (!mro_or.ok()) {
        return mro_or.status();
    }
    auto inserted = mro_cache_.emplace(class_key, mro_or.value());
    return inserted.first->second;
}

StatusOr<ClassDescriptor> ObjectModel::GetClass(std::string_view class_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = classes_.find(std::string(class_name));
    if (it == classes_.end()) {
        return Status::NotFound("class not found");
    }
    return it->second;
}

std::vector<std::string> ObjectModel::ListClassNames() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> names;
    names.reserve(classes_.size());
    for (const auto& [name, _] : classes_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

StatusOr<std::vector<std::string>> ObjectModel::Linearize(std::string_view class_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string class_key(class_name);
    auto it = mro_cache_.find(class_key);
    if (it != mro_cache_.end()) {
        return it->second;
    }
    std::unordered_map<std::string, std::vector<std::string>> memo;
    std::unordered_set<std::string> visiting;
    auto mro_or = LinearizeClass(classes_, class_key, &memo, &visiting);
    if (!mro_or.ok()) {
        return mro_or.status();
    }
    auto inserted = mro_cache_.emplace(class_key, mro_or.value());
    return inserted.first->second;
}

StatusOr<std::vector<std::string>> ObjectModel::MergeC3(
    std::vector<std::vector<std::string>> seqs) {
    return MergeC3Impl(std::move(seqs));
}

}  // namespace tie::vm
