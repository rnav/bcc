/*
 * Copyright (c) 2016 GitHub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "syms.h"
#include "vendor/optional.hpp"

namespace USDT {

using std::experimental::optional;
using std::experimental::nullopt;
class ArgumentParser;

class Argument {
private:
  optional<int> arg_size_;
  optional<int> constant_;
  optional<int> deref_offset_;
  optional<std::string> deref_ident_;
  optional<std::string> register_name_;

  bool get_global_address(uint64_t *address, const std::string &binpath,
                          const optional<int> &pid) const;

public:
  Argument();
  ~Argument();

  bool assign_to_local(std::ostream &stream, const std::string &local_name,
                       const std::string &binpath,
                       const optional<int> &pid = nullopt) const;

  int arg_size() const { return arg_size_.value_or(sizeof(void *)); }
  std::string ctype() const;

  const optional<std::string> &deref_ident() const { return deref_ident_; }
  const optional<std::string> &register_name() const { return register_name_; }
  const optional<int> constant() const { return constant_; }
  const optional<int> deref_offset() const { return deref_offset_; }

  friend class ArgumentParser;
};

class ArgumentParser {
  const char *arg_;
  ssize_t cur_pos_;

protected:
  virtual bool normalize_register(std::string *reg, int *reg_size) = 0;

  ssize_t parse_number(ssize_t pos, optional<int> *number);
  ssize_t parse_identifier(ssize_t pos, optional<std::string> *ident);
  ssize_t parse_register(ssize_t pos, Argument *dest);
  ssize_t parse_expr(ssize_t pos, Argument *dest);
  ssize_t parse_1(ssize_t pos, Argument *dest);

  void print_error(ssize_t pos);

public:
  bool parse(Argument *dest);
  bool done() { return cur_pos_ < 0 || arg_[cur_pos_] == '\0'; }

  ArgumentParser(const char *arg) : arg_(arg), cur_pos_(0) {}
};

class ArgumentParser_x64 : public ArgumentParser {
  enum Register {
    REG_A,
    REG_B,
    REG_C,
    REG_D,
    REG_SI,
    REG_DI,
    REG_BP,
    REG_SP,
    REG_8,
    REG_9,
    REG_10,
    REG_11,
    REG_12,
    REG_13,
    REG_14,
    REG_15,
    REG_RIP,
  };

  struct RegInfo {
    Register reg;
    int size;
  };

  static const std::unordered_map<std::string, RegInfo> registers_;
  bool normalize_register(std::string *reg, int *reg_size);
  void reg_to_name(std::string *norm, Register reg);

public:
  ArgumentParser_x64(const char *arg) : ArgumentParser(arg) {}
};

class Probe {
  std::string bin_path_;
  std::string provider_;
  std::string name_;
  uint64_t semaphore_;

  struct Location {
    uint64_t address_;
    std::vector<Argument *> arguments_;
    Location(uint64_t addr, const char *arg_fmt);
  };

  std::vector<Location> locations_;
  std::unordered_map<int, uint64_t> semaphores_;
  std::unordered_map<int, ProcStat> enabled_semaphores_;
  optional<bool> in_shared_object_;

  bool add_to_semaphore(int pid, int16_t val);
  bool lookup_semaphore_addr(uint64_t *address, int pid);
  void add_location(uint64_t addr, const char *fmt);

public:
  Probe(const char *bin_path, const char *provider, const char *name,
        uint64_t semaphore);

  size_t num_locations() const { return locations_.size(); }
  size_t num_arguments() const { return locations_.front().arguments_.size(); }

  bool usdt_thunks(std::ostream &stream, const std::string &prefix);
  bool usdt_cases(std::ostream &stream, const optional<int> &pid = nullopt);

  bool need_enable() const { return semaphore_ != 0x0; }
  bool enable(int pid);
  bool disable(int pid);

  bool in_shared_object();
  const std::string &name() { return name_; }
  const std::string &bin_path() { return bin_path_; }
  const std::string &provider() { return provider_; }

  friend class Context;
};

class Context {
  std::vector<Probe *> probes_;
  bool loaded_;

  static void _each_probe(const char *binpath, const struct bcc_elf_usdt *probe,
                          void *p);
  static int _each_module(const char *modpath, uint64_t, uint64_t, void *p);

  void add_probe(const char *binpath, const struct bcc_elf_usdt *probe);
  std::string resolve_bin_path(const std::string &bin_path);

public:
  Context(const std::string &bin_path);
  Context(int pid);

  bool loaded() const { return loaded_; }
  size_t num_probes() const { return probes_.size(); }
  Probe *find_probe(const std::string &probe_name);
};
}
