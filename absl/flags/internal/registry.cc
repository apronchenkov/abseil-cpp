//
// Copyright 2019 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/flags/internal/registry.h"

#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/flags/config.h"
#include "absl/flags/usage_config.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

// --------------------------------------------------------------------
// FlagRegistry implementation
//    A FlagRegistry holds all flag objects indexed
//    by their names so that if you know a flag's name you can access or
//    set it.

namespace absl {
namespace flags_internal {

// --------------------------------------------------------------------
// FlagRegistry
//    A FlagRegistry singleton object holds all flag objects indexed
//    by their names so that if you know a flag's name (as a C
//    string), you can access or set it.  If the function is named
//    FooLocked(), you must own the registry lock before calling
//    the function; otherwise, you should *not* hold the lock, and
//    the function will acquire it itself if needed.
// --------------------------------------------------------------------

class FlagRegistry {
 public:
  FlagRegistry() = default;
  ~FlagRegistry() {
    for (auto& p : flags_) {
      p.second->Destroy();
    }
  }

  // Store a flag in this registry.  Takes ownership of *flag.
  void RegisterFlag(CommandLineFlag* flag);

  void Lock() ABSL_EXCLUSIVE_LOCK_FUNCTION(lock_) { lock_.Lock(); }
  void Unlock() ABSL_UNLOCK_FUNCTION(lock_) { lock_.Unlock(); }

  // Returns the flag object for the specified name, or nullptr if not found.
  // Will emit a warning if a 'retired' flag is specified.
  CommandLineFlag* FindFlagLocked(absl::string_view name);

  // Returns the retired flag object for the specified name, or nullptr if not
  // found or not retired.  Does not emit a warning.
  CommandLineFlag* FindRetiredFlagLocked(absl::string_view name);

  static FlagRegistry* GlobalRegistry();  // returns a singleton registry

 private:
  friend class FlagSaverImpl;  // reads all the flags in order to copy them
  friend void ForEachFlagUnlocked(
      std::function<void(CommandLineFlag*)> visitor);

  // The map from name to flag, for FindFlagLocked().
  using FlagMap = std::map<absl::string_view, CommandLineFlag*>;
  using FlagIterator = FlagMap::iterator;
  using FlagConstIterator = FlagMap::const_iterator;
  FlagMap flags_;

  absl::Mutex lock_;

  // Disallow
  FlagRegistry(const FlagRegistry&);
  FlagRegistry& operator=(const FlagRegistry&);
};

FlagRegistry* FlagRegistry::GlobalRegistry() {
  static FlagRegistry* global_registry = new FlagRegistry;
  return global_registry;
}

namespace {

class FlagRegistryLock {
 public:
  explicit FlagRegistryLock(FlagRegistry* fr) : fr_(fr) { fr_->Lock(); }
  ~FlagRegistryLock() { fr_->Unlock(); }

 private:
  FlagRegistry* const fr_;
};

}  // namespace

void FlagRegistry::RegisterFlag(CommandLineFlag* flag) {
  FlagRegistryLock registry_lock(this);
  std::pair<FlagIterator, bool> ins =
      flags_.insert(FlagMap::value_type(flag->Name(), flag));
  if (ins.second == false) {  // means the name was already in the map
    CommandLineFlag* old_flag = ins.first->second;
    if (flag->IsRetired() != old_flag->IsRetired()) {
      // All registrations must agree on the 'retired' flag.
      flags_internal::ReportUsageError(
          absl::StrCat(
              "Retired flag '", flag->Name(),
              "' was defined normally in file '",
              (flag->IsRetired() ? old_flag->Filename() : flag->Filename()),
              "'."),
          true);
    } else if (flag->op_ != old_flag->op_) {
      flags_internal::ReportUsageError(
          absl::StrCat("Flag '", flag->Name(),
                       "' was defined more than once but with "
                       "differing types. Defined in files '",
                       old_flag->Filename(), "' and '", flag->Filename(),
                       "' with types '", old_flag->Typename(), "' and '",
                       flag->Typename(), "', respectively."),
          true);
    } else if (old_flag->IsRetired()) {
      // Retired definitions are idempotent. Just keep the old one.
      flag->Destroy();
      return;
    } else if (old_flag->Filename() != flag->Filename()) {
      flags_internal::ReportUsageError(
          absl::StrCat("Flag '", flag->Name(),
                       "' was defined more than once (in files '",
                       old_flag->Filename(), "' and '", flag->Filename(),
                       "')."),
          true);
    } else {
      flags_internal::ReportUsageError(
          absl::StrCat(
              "Something wrong with flag '", flag->Name(), "' in file '",
              flag->Filename(), "'. One possibility: file '", flag->Filename(),
              "' is being linked both statically and dynamically into this "
              "executable. e.g. some files listed as srcs to a test and also "
              "listed as srcs of some shared lib deps of the same test."),
          true);
    }
    // All cases above are fatal, except for the retired flags.
    std::exit(1);
  }
}

CommandLineFlag* FlagRegistry::FindFlagLocked(absl::string_view name) {
  FlagConstIterator i = flags_.find(name);
  if (i == flags_.end()) {
    return nullptr;
  }

  if (i->second->IsRetired()) {
    flags_internal::ReportUsageError(
        absl::StrCat("Accessing retired flag '", name, "'"), false);
  }

  return i->second;
}

CommandLineFlag* FlagRegistry::FindRetiredFlagLocked(absl::string_view name) {
  FlagConstIterator i = flags_.find(name);
  if (i == flags_.end() || !i->second->IsRetired()) {
    return nullptr;
  }

  return i->second;
}

// --------------------------------------------------------------------
// FlagSaver
// FlagSaverImpl
//    This class stores the states of all flags at construct time,
//    and restores all flags to that state at destruct time.
//    Its major implementation challenge is that it never modifies
//    pointers in the 'main' registry, so global FLAG_* vars always
//    point to the right place.
// --------------------------------------------------------------------

class FlagSaverImpl {
 public:
  FlagSaverImpl() = default;
  FlagSaverImpl(const FlagSaverImpl&) = delete;
  void operator=(const FlagSaverImpl&) = delete;

  // Saves the flag states from the flag registry into this object.
  // It's an error to call this more than once.
  void SaveFromRegistry() {
    assert(backup_registry_.empty());  // call only once!
    flags_internal::ForEachFlag([&](flags_internal::CommandLineFlag* flag) {
      if (auto flag_state = flag->SaveState()) {
        backup_registry_.emplace_back(std::move(flag_state));
      }
    });
  }

  // Restores the saved flag states into the flag registry.
  void RestoreToRegistry() {
    for (const auto& flag_state : backup_registry_) {
      flag_state->Restore();
    }
  }

 private:
  std::vector<std::unique_ptr<flags_internal::FlagStateInterface>>
      backup_registry_;
};

FlagSaver::FlagSaver() : impl_(new FlagSaverImpl) { impl_->SaveFromRegistry(); }

void FlagSaver::Ignore() {
  delete impl_;
  impl_ = nullptr;
}

FlagSaver::~FlagSaver() {
  if (!impl_) return;

  impl_->RestoreToRegistry();
  delete impl_;
}

// --------------------------------------------------------------------

CommandLineFlag* FindCommandLineFlag(absl::string_view name) {
  if (name.empty()) return nullptr;
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagRegistryLock frl(registry);

  return registry->FindFlagLocked(name);
}

CommandLineFlag* FindRetiredFlag(absl::string_view name) {
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagRegistryLock frl(registry);

  return registry->FindRetiredFlagLocked(name);
}

// --------------------------------------------------------------------

void ForEachFlagUnlocked(std::function<void(CommandLineFlag*)> visitor) {
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  for (FlagRegistry::FlagConstIterator i = registry->flags_.begin();
       i != registry->flags_.end(); ++i) {
    visitor(i->second);
  }
}

void ForEachFlag(std::function<void(CommandLineFlag*)> visitor) {
  FlagRegistry* const registry = FlagRegistry::GlobalRegistry();
  FlagRegistryLock frl(registry);
  ForEachFlagUnlocked(visitor);
}

// --------------------------------------------------------------------

bool RegisterCommandLineFlag(CommandLineFlag* flag) {
  FlagRegistry::GlobalRegistry()->RegisterFlag(flag);
  return true;
}

// --------------------------------------------------------------------

namespace {

class RetiredFlagObj final : public flags_internal::CommandLineFlag {
 public:
  constexpr RetiredFlagObj(const char* name, FlagOpFn ops,
                           FlagMarshallingOpFn marshalling_ops)
      : flags_internal::CommandLineFlag(
            name, flags_internal::HelpText::FromStaticCString(nullptr),
            /*filename=*/"RETIRED", ops, marshalling_ops,
            /*initial_value_gen=*/nullptr,
            /*def=*/nullptr,
            /*cur=*/nullptr) {}

 private:
  bool IsRetired() const override { return true; }

  void Destroy() const override {
    // Values are heap allocated for Retired Flags.
    if (cur_) Delete(op_, cur_);
    if (def_) Delete(op_, def_);

    if (locks_) delete locks_;

    delete this;
  }

  std::unique_ptr<flags_internal::FlagStateInterface> SaveState() override {
    return nullptr;
  }
};

}  // namespace

bool Retire(const char* name, FlagOpFn ops,
            FlagMarshallingOpFn marshalling_ops) {
  auto* flag = new flags_internal::RetiredFlagObj(name, ops, marshalling_ops);
  FlagRegistry::GlobalRegistry()->RegisterFlag(flag);
  return true;
}

// --------------------------------------------------------------------

bool IsRetiredFlag(absl::string_view name, bool* type_is_bool) {
  assert(!name.empty());
  CommandLineFlag* flag = flags_internal::FindRetiredFlag(name);
  if (flag == nullptr) {
    return false;
  }
  assert(type_is_bool);
  *type_is_bool = flag->IsOfType<bool>();
  return true;
}

}  // namespace flags_internal
}  // namespace absl
