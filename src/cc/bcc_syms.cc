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

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bcc_elf.h"
#include "bcc_proc.h"
#include "bcc_syms.h"

#include "syms.h"

ino_t ProcStat::getinode_() {
  struct stat s;
  return (!stat(procfs_.c_str(), &s)) ? s.st_ino : -1;
}

ProcStat::ProcStat(int pid) : inode_(-1) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "/proc/%d/exe", pid);
  procfs_ = buffer;
}

void KSyms::_add_symbol(const char *symname, uint64_t addr, void *p) {
  KSyms *ks = static_cast<KSyms *>(p);
  ks->syms_.emplace_back(symname, addr);
}

void KSyms::refresh() {
  if (syms_.empty()) {
    bcc_procutils_each_ksym(_add_symbol, this);
    std::sort(syms_.begin(), syms_.end());
  }
}

bool KSyms::resolve_addr(uint64_t addr, struct bcc_symbol *sym) {
  refresh();

  if (syms_.empty()) {
    sym->name = nullptr;
    sym->module = nullptr;
    sym->offset = 0x0;
    return false;
  }

  auto it = std::upper_bound(syms_.begin(), syms_.end(), Symbol("", addr)) - 1;
  sym->name = (*it).name.c_str();
  sym->module = "[kernel]";
  sym->offset = addr - (*it).addr;
  return true;
}

bool KSyms::resolve_name(const char *_unused, const char *name,
                         uint64_t *addr) {
  refresh();

  if (syms_.size() != symnames_.size()) {
    symnames_.clear();
    for (Symbol &sym : syms_) {
      symnames_[sym.name] = sym.addr;
    }
  }

  auto it = symnames_.find(name);
  if (it == symnames_.end())
    return false;

  *addr = it->second;
  return true;
}

ProcSyms::ProcSyms(int pid) : pid_(pid), procstat_(pid) { refresh(); }

void ProcSyms::refresh() {
  modules_.clear();
  bcc_procutils_each_module(pid_, _add_module, this);
  procstat_.reset();
}

int ProcSyms::_add_module(const char *modname, uint64_t start, uint64_t end,
                          void *payload) {
  ProcSyms *ps = static_cast<ProcSyms *>(payload);
  ps->modules_.emplace_back(modname, start, end);
  return 0;
}

bool ProcSyms::resolve_addr(uint64_t addr, struct bcc_symbol *sym) {
  if (procstat_.is_stale())
    refresh();

  sym->module = nullptr;
  sym->name = nullptr;
  sym->offset = 0x0;

  for (Module &mod : modules_) {
    if (addr >= mod.start_ && addr <= mod.end_)
      return mod.find_addr(addr, sym);
  }
  return false;
}

bool ProcSyms::resolve_name(const char *module, const char *name,
                            uint64_t *addr) {
  if (procstat_.is_stale())
    refresh();

  for (Module &mod : modules_) {
    if (mod.name_ == module)
      return mod.find_name(name, addr);
  }
  return false;
}

int ProcSyms::Module::_add_symbol(const char *symname, uint64_t start,
                                  uint64_t end, int flags, void *p) {
  Module *m = static_cast<Module *>(p);
  m->syms_.emplace_back(symname, start, end, flags);
  return 0;
}

bool ProcSyms::Module::is_so() const {
  return strstr(name_.c_str(), ".so") != nullptr;
}

void ProcSyms::Module::load_sym_table() {
  if (syms_.size())
    return;

  bcc_elf_foreach_sym(name_.c_str(), _add_symbol, this);
}

bool ProcSyms::Module::find_name(const char *symname, uint64_t *addr) {
  load_sym_table();

  for (Symbol &s : syms_) {
    if (s.name == symname) {
      *addr = is_so() ? start_ + s.start : s.start;
      return true;
    }
  }
  return false;
}

bool ProcSyms::Module::find_addr(uint64_t addr, struct bcc_symbol *sym) {
  uint64_t offset = is_so() ? (addr - start_) : addr;

  load_sym_table();

  sym->module = name_.c_str();
  sym->offset = offset;

  for (Symbol &s : syms_) {
    if (offset >= s.start && offset <= (s.start + s.size)) {
      sym->name = s.name.c_str();
      sym->offset = (offset - s.start);
      return true;
    }
  }
  return false;
}

extern "C" {

void *bcc_symcache_new(int pid) {
  if (pid < 0)
    return static_cast<void *>(new KSyms());
  return static_cast<void *>(new ProcSyms(pid));
}

int bcc_symcache_resolve(void *resolver, uint64_t addr,
                         struct bcc_symbol *sym) {
  SymbolCache *cache = static_cast<SymbolCache *>(resolver);
  return cache->resolve_addr(addr, sym) ? 0 : -1;
}

int bcc_symcache_resolve_name(void *resolver, const char *name,
                              uint64_t *addr) {
  SymbolCache *cache = static_cast<SymbolCache *>(resolver);
  return cache->resolve_name(nullptr, name, addr) ? 0 : -1;
}

void bcc_symcache_refresh(void *resolver) {
  SymbolCache *cache = static_cast<SymbolCache *>(resolver);
  cache->refresh();
}

static int _find_sym(const char *symname, uint64_t addr, uint64_t end,
                     int flags, void *payload) {
  struct bcc_symbol *sym = (struct bcc_symbol *)payload;
  // TODO: check for actual function symbol in flags
  if (!strcmp(sym->name, symname)) {
    sym->offset = addr;
    return -1;
  }
  return 0;
}

int bcc_find_symbol_addr(struct bcc_symbol *sym) {
  return bcc_elf_foreach_sym(sym->module, _find_sym, sym);
}

int bcc_resolve_symname(const char *module, const char *symname,
                        const uint64_t addr, struct bcc_symbol *sym) {
  uint64_t load_addr;

  sym->module = NULL;
  sym->name = NULL;
  sym->offset = 0x0;

  if (module == NULL)
    return -1;

  if (strchr(module, '/')) {
    sym->module = module;
  } else {
    sym->module = bcc_procutils_which_so(module);
  }

  if (sym->module == NULL)
    return -1;

  if (bcc_elf_loadaddr(sym->module, &load_addr) < 0) {
    sym->module = NULL;
    return -1;
  }

  sym->name = symname;
  sym->offset = addr;

  if (sym->name && sym->offset == 0x0) {
    if (bcc_find_symbol_addr(sym) < 0)
      return -1;
  }

  if (sym->offset == 0x0)
    return -1;

  sym->offset = (sym->offset - load_addr);
  return 0;
}
}
