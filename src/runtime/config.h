/*
 * Copyright 2022 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <re2/re2.h>

#include <functional>
#include <map>
#include <set>
#include <string>

#include "json/json5.h"

struct WakeConfigOverrides {
  wcl::optional<std::string> log_header;
  wcl::optional<int64_t> log_header_source_width;

  // if label_filter == {} then we don't override
  // if label_filter == {{}} then we do override but the filter accepts everything
  // if label_filter == {{filter}} then only jobs matching the filter are accepted
  // these semantics fall out of the normal way overrides work, this comment
  // is just to make this unusual type's meaning clear.
  wcl::optional<wcl::optional<std::string>> label_filter;

  // Determines the maximum size of the cache
  wcl::optional<uint64_t> max_cache_size;

  // Determines the size of the cache that collection
  // tries to get us back to.
  wcl::optional<uint64_t> low_cache_size;

  // Determines if log headers should be aligned
  wcl::optional<bool> log_header_align;

  // Determines if job cache should terminate on error or return a cache miss
  wcl::optional<bool> cache_miss_on_failure;

  // Lets you specify an alternative user config
  wcl::optional<std::string> user_config;
};

template <class T>
using Override = wcl::optional<T> WakeConfigOverrides::*;

/********************************************************************
 * Policies
 ********************************************************************/

struct VersionPolicy {
  using type = std::string;
  using input_type = type;
  static constexpr const char* key = "version";
  static constexpr bool allowed_in_wakeroot = true;
  static constexpr bool allowed_in_userconfig = false;
  type version = "";
  static constexpr type VersionPolicy::*value = &VersionPolicy::version;
  static constexpr Override<input_type> override_value = nullptr;
  static constexpr const char* env_var = nullptr;

  VersionPolicy() = default;
  static void set(VersionPolicy& p, const JAST& json);
  static void set_input(VersionPolicy& p, const input_type& v) { p.*value = v; }
  static void emit(const VersionPolicy& p, std::ostream& os) { os << p.*value; }
  static void set_env_var(VersionPolicy& p, const char* env_var){};
};

struct UserConfigPolicy {
  using type = std::string;
  using input_type = type;
  static constexpr const char* key = "user_config";
  static constexpr bool allowed_in_wakeroot = true;
  static constexpr bool allowed_in_userconfig = false;
  type user_config;
  static constexpr type UserConfigPolicy::*value = &UserConfigPolicy::user_config;
  static constexpr Override<input_type> override_value = &WakeConfigOverrides::user_config;
  static constexpr const char* env_var = "WAKE_USER_CONFIG";

  UserConfigPolicy();
  static void set(UserConfigPolicy& p, const JAST& json);
  static void set_input(UserConfigPolicy& p, const input_type& v) { p.*value = v; }
  static void emit(const UserConfigPolicy& p, std::ostream& os) { os << p.*value; }
  static void set_env_var(UserConfigPolicy& p, const char* env_var) { p.*value = env_var; };
};

struct LogHeaderPolicy {
  using type = std::string;
  using input_type = type;
  static constexpr const char* key = "log_header";
  static constexpr bool allowed_in_wakeroot = true;
  static constexpr bool allowed_in_userconfig = true;
  type log_header = "[$stream] $source: ";
  static constexpr type LogHeaderPolicy::*value = &LogHeaderPolicy::log_header;
  static constexpr Override<input_type> override_value = &WakeConfigOverrides::log_header;
  static constexpr const char* env_var = nullptr;

  LogHeaderPolicy() = default;
  static void set(LogHeaderPolicy& p, const JAST& json);
  static void set_input(LogHeaderPolicy& p, const input_type& v) { p.*value = v; }
  static void emit(const LogHeaderPolicy& p, std::ostream& os) { os << p.*value; }
  static void set_env_var(LogHeaderPolicy& p, const char* env_var){};
};

struct LogHeaderSourceWidthPolicy {
  using type = int64_t;
  using input_type = type;
  static constexpr const char* key = "log_header_source_width";
  static constexpr bool allowed_in_wakeroot = true;
  static constexpr bool allowed_in_userconfig = true;
  type log_header_source_width = 25;
  static constexpr type LogHeaderSourceWidthPolicy::*value =
      &LogHeaderSourceWidthPolicy::log_header_source_width;
  static constexpr Override<input_type> override_value =
      &WakeConfigOverrides::log_header_source_width;
  static constexpr const char* env_var = nullptr;

  LogHeaderSourceWidthPolicy() {}
  static void set(LogHeaderSourceWidthPolicy& p, const JAST& json);
  static void set_input(LogHeaderSourceWidthPolicy& p, const input_type& v) { p.*value = v; }
  static void emit(const LogHeaderSourceWidthPolicy& p, std::ostream& os) { os << p.*value; }
  static void set_env_var(LogHeaderSourceWidthPolicy& p, const char* env_var){};
};

struct LabelFilterPolicy {
  using type = std::unique_ptr<re2::RE2>;
  using input_type = wcl::optional<std::string>;
  static constexpr const char* key = "label_filter";
  static constexpr bool allowed_in_wakeroot = false;
  static constexpr bool allowed_in_userconfig = false;
  type label_filter;
  static constexpr type LabelFilterPolicy::*value = &LabelFilterPolicy::label_filter;
  static constexpr Override<input_type> override_value = &WakeConfigOverrides::label_filter;
  static constexpr const char* env_var = nullptr;

  LabelFilterPolicy() : label_filter(std::make_unique<re2::RE2>(".*")) {}

  static void set(LabelFilterPolicy& p, const JAST& json) {}
  static void set_input(LabelFilterPolicy& p, const input_type& v) {
    if (v) {
      p.*value = std::make_unique<re2::RE2>(*v);
    }
  }
  static void emit(const LabelFilterPolicy& p, std::ostream& os) {
    os << p.label_filter->pattern();
  }
  static void set_env_var(LogHeaderSourceWidthPolicy& p, const char* value){};
};

struct SharedCacheMissOnFailure {
  using type = bool;
  using input_type = type;
  static constexpr const char* key = "cache_miss_on_failure";
  static constexpr bool allowed_in_wakeroot = true;
  static constexpr bool allowed_in_userconfig = true;
  type cache_miss_on_failure = false;
  static constexpr type SharedCacheMissOnFailure::*value =
      &SharedCacheMissOnFailure::cache_miss_on_failure;
  static constexpr Override<input_type> override_value =
      &WakeConfigOverrides::cache_miss_on_failure;
  static constexpr const char* env_var = "WAKE_SHARED_CACHE_MISS_ON_FAILURE";

  static void set(SharedCacheMissOnFailure& p, const JAST& json);
  static void set_input(SharedCacheMissOnFailure& p, const input_type& v) { p.*value = v; }
  static void emit(const SharedCacheMissOnFailure& p, std::ostream& os) {
    if (p.cache_miss_on_failure) {
      os << "true";
    } else {
      os << "false";
    }
  }
  static void set_env_var(SharedCacheMissOnFailure& p, const char* env_var) {
    if (std::string(env_var) == "1") {
      p.*value = true;
    } else {
      p.*value = false;
    }
  }
};

struct LogHeaderAlignPolicy {
  using type = bool;
  using input_type = type;
  static constexpr const char* key = "log_header_align";
  static constexpr bool allowed_in_wakeroot = true;
  static constexpr bool allowed_in_userconfig = true;
  type log_header_align = false;
  static constexpr type LogHeaderAlignPolicy::*value = &LogHeaderAlignPolicy::log_header_align;
  static constexpr Override<input_type> override_value = &WakeConfigOverrides::log_header_align;
  static constexpr const char* env_var = nullptr;

  LogHeaderAlignPolicy() {}
  static void set(LogHeaderAlignPolicy& p, const JAST& json);
  static void set_input(LogHeaderAlignPolicy& p, const input_type& v) { p.*value = v; }
  static void emit(const LogHeaderAlignPolicy& p, std::ostream& os) {
    if (p.log_header_align) {
      os << "true";
    } else {
      os << "false";
    }
  }
  static void set_env_var(LogHeaderAlignPolicy& p, const char* env_var) {}
};

struct BulkLoggingDirPolicy {
  using type = std::string;
  using input_type = type;
  static constexpr const char* key = "bulk_logging_dir";
  static constexpr bool allowed_in_wakeroot = false;
  static constexpr bool allowed_in_userconfig = true;
  type bulk_logging_dir;
  static constexpr type BulkLoggingDirPolicy::*value = &BulkLoggingDirPolicy::bulk_logging_dir;
  static constexpr Override<input_type> override_value = nullptr;
  static constexpr const char* env_var = "WAKE_BULK_LOGGING_DIR";

  BulkLoggingDirPolicy() {}
  static void set(BulkLoggingDirPolicy& p, const JAST& json);
  static void set_input(BulkLoggingDirPolicy& p, const input_type& v) { p.*value = v; }
  static void emit(const BulkLoggingDirPolicy& p, std::ostream& os) { os << p.*value; }
  static void set_env_var(BulkLoggingDirPolicy& p, const char* env_var) {
    p.bulk_logging_dir = env_var;
  }
};

struct InterpreterRuntimeWarningPolicy {
  using type = int64_t;
  using input_type = type;
  static constexpr const char* key = "interpreter_runtime_warning_s";
  static constexpr bool allowed_in_wakeroot = true;
  static constexpr bool allowed_in_userconfig = true;
  type interpreter_runtime_warning_s = 0;  // Default is off
  static constexpr type InterpreterRuntimeWarningPolicy::*value =
      &InterpreterRuntimeWarningPolicy::interpreter_runtime_warning_s;
  static constexpr Override<input_type> override_value = nullptr;
  static constexpr const char* env_var = "WAKE_INTERPRETER_RUNTIME_WARNING_S";

  InterpreterRuntimeWarningPolicy() = default;
  static void set(InterpreterRuntimeWarningPolicy& p, const JAST& json);
  static void set_input(InterpreterRuntimeWarningPolicy& p, const input_type& v) { p.*value = v; }
  static void emit(const InterpreterRuntimeWarningPolicy& p, std::ostream& os) { os << p.*value; }
  static void set_env_var(InterpreterRuntimeWarningPolicy& p, const char* env_var) {
    p.*value = std::stod(env_var);
  }
};

/********************************************************************
 * Generic WakeConfig implementation
 *********************************************************************/

enum class WakeConfigProvenance { Default, WakeRoot, UserConfig, CommandLine, EnvVar };

static inline const char* to_string(WakeConfigProvenance p) {
  switch (p) {
    case WakeConfigProvenance::Default:
      return "Default";
    case WakeConfigProvenance::WakeRoot:
      return "WakeRoot";
    case WakeConfigProvenance::UserConfig:
      return "UserConfig";
    case WakeConfigProvenance::CommandLine:
      return "Commandline";
    case WakeConfigProvenance::EnvVar:
      return "EnvVar";
  }
  return "Unknown (this is an error, please report this to Wake devs)";
}

template <class... Policies>
struct WakeConfigImpl : public Policies... {
  WakeConfigImpl(const WakeConfigImpl&) = delete;
  WakeConfigImpl(WakeConfigImpl&&) = delete;
  WakeConfigImpl() = default;

  std::map<std::string, WakeConfigProvenance> provenance;

 private:
  struct void_t {};

  static void call_all() {}

  template <class F, class... Args>
  static void call_all(F f, Args... fs) {
    f();
    call_all(fs...);
  }

  template <class P>
  auto emit_each(std::ostream& os) const {
    return [&os, this]() {
      auto iter = provenance.find(P::key);
      auto p = WakeConfigProvenance::Default;
      if (iter != provenance.end()) {
        p = iter->second;
      }
      os << "  " << P::key << " = '";
      P::emit(*this, os);
      os << "' (" << to_string(p) << ")" << std::endl;
    };
  }

  template <class P>
  static auto add_wakeroot_key(std::set<std::string>& out) {
    return [&out]() {
      if (P::allowed_in_wakeroot) {
        out.emplace(P::key);
      }
    };
  }

  template <class P>
  static auto add_userconfig_key(std::set<std::string>& out) {
    return [&out]() {
      if (P::allowed_in_userconfig) {
        out.emplace(P::key);
      }
    };
  }

  template <WakeConfigProvenance p, class P>
  auto set_policy(const JAST& json) {
    return [&json, this]() {
      if (p == WakeConfigProvenance::WakeRoot && !P::allowed_in_wakeroot) {
        return;
      }
      if (p == WakeConfigProvenance::UserConfig && !P::allowed_in_userconfig) {
        return;
      }
      auto opt_value = json.get_opt(P::key);
      if (opt_value) {
        provenance[P::key] = p;
        P::set(*this, **opt_value);
      }
    };
  }

  template <class P>
  auto set_env_var() {
    return [this]() {
      if (P::env_var == nullptr) return;
      const char* env_var = getenv(P::env_var ? P::env_var : "");
      if (env_var == nullptr) return;
      provenance[P::key] = WakeConfigProvenance::EnvVar;
      P::set_env_var(*this, env_var);
    };
  }

  template <class P>
  auto override_policy(const WakeConfigOverrides& overrides) {
    return [&overrides, this]() {
      if (P::override_value && overrides.*P::override_value) {
        provenance[P::key] = WakeConfigProvenance::CommandLine;
        P::set_input(*this, *(overrides.*P::override_value));
      }
    };
  }

 protected:
  static std::set<std::string> wakeroot_allowed_keys() {
    std::set<std::string> out;
    call_all(add_wakeroot_key<Policies>(out)...);
    return out;
  }

  static std::set<std::string> userconfig_allowed_keys() {
    std::set<std::string> out;
    call_all(add_userconfig_key<Policies>(out)...);
    return out;
  }

  template <WakeConfigProvenance p>
  void set_all(const JAST& json) {
    call_all(set_policy<p, Policies>(json)...);
  }

  void set_all_env_var() { call_all(set_env_var<Policies>()...); }

  void override_all(const WakeConfigOverrides& overrides) {
    call_all(override_policy<Policies>(overrides)...);
  }

 public:
  void emit(std::ostream& os) const {
    os << "Wake config:" << std::endl;
    call_all(emit_each<Policies>(os)...);
  }
};

using WakeConfigImplFull =
    WakeConfigImpl<UserConfigPolicy, VersionPolicy, LogHeaderPolicy, LogHeaderSourceWidthPolicy,
                   LabelFilterPolicy, SharedCacheMissOnFailure, LogHeaderAlignPolicy,
                   BulkLoggingDirPolicy, InterpreterRuntimeWarningPolicy>;

struct WakeConfig final : public WakeConfigImplFull {
  static bool init(const std::string& wakeroot_path, const WakeConfigOverrides& overrides);
  static const WakeConfig* const get();

  WakeConfig(const WakeConfig&) = delete;
  WakeConfig(WakeConfig&&) = delete;

 private:
  WakeConfig() = default;
};

std::ostream& operator<<(std::ostream& os, const WakeConfig& config);
