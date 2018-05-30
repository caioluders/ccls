#include "query.h"

#include "indexer.h"
#include "serializer.h"
#include "serializers/json.h"

#include <doctest/doctest.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Used by |HANDLE_MERGEABLE| so only |range| is needed.
MAKE_HASHABLE(Use, t.range, t.file_id);

namespace {

void AssignFileId(int file_id, SymbolRef& ref) {
  if (ref.kind == SymbolKind::File)
    ref.usr = file_id;
}

void AssignFileId(int file_id, Use& use) {
  if (use.kind == SymbolKind::File)
    use.usr = file_id;
  use.file_id = file_id;
}

template <typename T>
void AssignFileId(int file_id, T&) {}

template <typename T>
void AssignFileId(int file_id, Maybe<T>& x) {
  if (x)
    AssignFileId(file_id, *x);
}

template <typename T>
void AssignFileId(int file_id, std::vector<T>& xs) {
  for (T& x : xs)
    AssignFileId(file_id, x);
}

void AddRange(int file_id, std::vector<Use>& into, const std::vector<Use>& from) {
  into.reserve(into.size() + from.size());
  for (Use use : from) {
    use.file_id = file_id;
    into.push_back(use);
  }
}

void AddRange(int _, std::vector<Usr>& into, const std::vector<Usr>& from) {
  into.insert(into.end(), from.begin(), from.end());
}

template <typename T>
void RemoveRange(std::vector<T>& from, const std::vector<T>& to_remove) {
  if (to_remove.size()) {
    std::unordered_set<T> to_remove_set(to_remove.begin(), to_remove.end());
    from.erase(
      std::remove_if(from.begin(), from.end(),
        [&](const T& t) { return to_remove_set.count(t) > 0; }),
      from.end());
  }
}

QueryFile::DefUpdate BuildFileDefUpdate(const IndexFile& indexed) {
  QueryFile::Def def;
  def.path = std::move(indexed.path);
  def.args = std::move(indexed.args);
  def.includes = std::move(indexed.includes);
  def.inactive_regions = std::move(indexed.skipped_by_preprocessor);
  def.dependencies.reserve(indexed.dependencies.size());
  for (auto& dep : indexed.dependencies)
    def.dependencies.push_back(dep.first());
  def.language = indexed.language;

  auto add_all_symbols = [&](Use use, Usr usr, SymbolKind kind) {
    def.all_symbols.push_back(SymbolRef(use.range, usr, kind, use.role));
  };
  auto add_outline = [&](Use use, Usr usr, SymbolKind kind) {
    def.outline.push_back(SymbolRef(use.range, usr, kind, use.role));
  };

  for (auto& it : indexed.usr2type) {
    const IndexType& type = it.second;
    if (type.def.spell)
      add_all_symbols(*type.def.spell, type.usr, SymbolKind::Type);
    if (type.def.extent)
      add_outline(*type.def.extent, type.usr, SymbolKind::Type);
    for (Use decl : type.declarations) {
      add_all_symbols(decl, type.usr, SymbolKind::Type);
      // Constructor positions have references to the class,
      // which we do not want to show in textDocument/documentSymbol
      if (!(decl.role & Role::Reference))
        add_outline(decl, type.usr, SymbolKind::Type);
    }
    for (Use use : type.uses)
      add_all_symbols(use, type.usr, SymbolKind::Type);
  }
  for (auto& it: indexed.usr2func) {
    const IndexFunc& func = it.second;
    if (func.def.spell)
      add_all_symbols(*func.def.spell, func.usr, SymbolKind::Func);
    if (func.def.extent)
      add_outline(*func.def.extent, func.usr, SymbolKind::Func);
    for (Use use : func.declarations) {
      add_all_symbols(use, func.usr, SymbolKind::Func);
      add_outline(use, func.usr, SymbolKind::Func);
    }
    for (Use use : func.uses) {
      // Make ranges of implicit function calls larger (spanning one more column
      // to the left/right). This is hacky but useful. e.g.
      // textDocument/definition on the space/semicolon in `A a;` or `return
      // 42;` will take you to the constructor.
      if (use.role & Role::Implicit) {
        if (use.range.start.column > 0)
          use.range.start.column--;
        use.range.end.column++;
      }
      add_all_symbols(use, func.usr, SymbolKind::Func);
    }
  }
  for (auto& it : indexed.usr2var) {
    const IndexVar& var = it.second;
    if (var.def.spell)
      add_all_symbols(*var.def.spell, var.usr, SymbolKind::Var);
    if (var.def.extent)
      add_outline(*var.def.extent, var.usr, SymbolKind::Var);
    for (Use decl : var.declarations) {
      add_all_symbols(decl, var.usr, SymbolKind::Var);
      add_outline(decl, var.usr, SymbolKind::Var);
    }
    for (Use use : var.uses)
      add_all_symbols(use, var.usr, SymbolKind::Var);
  }

  std::sort(def.outline.begin(), def.outline.end(),
            [](const SymbolRef& a, const SymbolRef& b) {
              return a.range.start < b.range.start;
            });
  std::sort(def.all_symbols.begin(), def.all_symbols.end(),
            [](const SymbolRef& a, const SymbolRef& b) {
              return a.range.start < b.range.start;
            });

  return QueryFile::DefUpdate(def, indexed.file_contents);
}

// Returns true if an element with the same file is found.
template <typename Q>
bool TryReplaceDef(llvm::SmallVectorImpl<Q>& def_list, Q&& def) {
  for (auto& def1 : def_list)
    if (def1.spell->file_id == def.spell->file_id) {
      def1 = std::move(def);
      return true;
    }
  return false;
}

}  // namespace

// ----------------------
// INDEX THREAD FUNCTIONS
// ----------------------

// static
IndexUpdate IndexUpdate::CreateDelta(IndexFile* previous,
                                     IndexFile* current) {
  IndexUpdate r;
  static IndexFile empty(current->path, "<empty>");
  if (!previous)
    previous = &empty;

  r.files_def_update = BuildFileDefUpdate(std::move(*current));

  for (auto& it : previous->usr2func) {
    auto& func = it.second;
    if (func.def.spell)
      r.funcs_removed.push_back(func.usr);
    r.funcs_declarations[func.usr].first = std::move(func.declarations);
    r.funcs_uses[func.usr].first = std::move(func.uses);
    r.funcs_derived[func.usr].first = std::move(func.derived);
  }
  for (auto& it : current->usr2func) {
    auto& func = it.second;
    if (func.def.spell && func.def.detailed_name.size())
      r.funcs_def_update.emplace_back(it.first, func.def);
    r.funcs_declarations[func.usr].second = std::move(func.declarations);
    r.funcs_uses[func.usr].second = std::move(func.uses);
    r.funcs_derived[func.usr].second = std::move(func.derived);
  }

  for (auto& it : previous->usr2type) {
    auto& type = it.second;
    if (type.def.spell)
      r.types_removed.push_back(type.usr);
    r.types_declarations[type.usr].first = std::move(type.declarations);
    r.types_uses[type.usr].first = std::move(type.uses);
    r.types_derived[type.usr].first = std::move(type.derived);
    r.types_instances[type.usr].first = std::move(type.instances);
  };
  for (auto& it : current->usr2type) {
    auto& type = it.second;
    if (type.def.spell && type.def.detailed_name.size())
      r.types_def_update.emplace_back(it.first, type.def);
    r.types_declarations[type.usr].second = std::move(type.declarations);
    r.types_uses[type.usr].second = std::move(type.uses);
    r.types_derived[type.usr].second = std::move(type.derived);
    r.types_instances[type.usr].second = std::move(type.instances);
  };

  for (auto& it : previous->usr2var) {
    auto& var = it.second;
    if (var.def.spell)
      r.vars_removed.push_back(var.usr);
    r.vars_declarations[var.usr].first = std::move(var.declarations);
    r.vars_uses[var.usr].first = std::move(var.uses);
  }
  for (auto& it : current->usr2var) {
    auto& var = it.second;
    if (var.def.spell && var.def.detailed_name.size())
      r.vars_def_update.emplace_back(it.first, var.def);
    r.vars_declarations[var.usr].second = std::move(var.declarations);
    r.vars_uses[var.usr].second = std::move(var.uses);
  }

  return r;
}

void DB::RemoveUsrs(SymbolKind kind,
                    int file_id,
                    const std::vector<Usr>& to_remove) {
  switch (kind) {
    case SymbolKind::Func: {
      for (Usr usr : to_remove) {
        // FIXME
        if (!func_usr.count(usr)) continue;
        QueryFunc& func = Func(usr);
        auto it = llvm::find_if(func.def, [=](const QueryFunc::Def& def) {
          return def.spell->file_id == file_id;
        });
        if (it != func.def.end())
          func.def.erase(it);
      }
      break;
    }
    case SymbolKind::Type: {
      for (Usr usr : to_remove) {
        // FIXME
        if (!type_usr.count(usr)) continue;
        QueryType& type = Type(usr);
        auto it = llvm::find_if(type.def, [=](const QueryType::Def& def) {
          return def.spell->file_id == file_id;
        });
        if (it != type.def.end())
          type.def.erase(it);
      }
      break;
    }
    case SymbolKind::Var: {
      for (Usr usr : to_remove) {
        // FIXME
        if (!var_usr.count(usr)) continue;
        QueryVar& var = Var(usr);
        auto it = llvm::find_if(var.def, [=](const QueryVar::Def& def) {
          return def.spell->file_id == file_id;
        });
        if (it != var.def.end())
          var.def.erase(it);
      }
      break;
    }
    default:
      break;
  }
}

void DB::ApplyIndexUpdate(IndexUpdate* u) {
#define HANDLE_MERGEABLE(update_var_name, def_var_name, C, to_index) \
  for (auto& it : u->update_var_name)                                \
    if (it.first != ~0ULL && it.first != ~0ULL - 1) {                \
      auto R = to_index.try_emplace(it.first, to_index.size());      \
      if (R.second)                                                  \
        C.emplace_back();                                            \
      if (to_index.size() != C.size()) {                             \
        volatile static int z = 0;                                   \
        while (!z)                                                   \
          asm("pause");                                              \
      }                                                              \
      auto& entity = C[R.first->second];                             \
      AssignFileId(u->file_id, it.second.first);                     \
      RemoveRange(entity.def_var_name, it.second.first);             \
      AssignFileId(u->file_id, it.second.second);                    \
      AddRange(u->file_id, entity.def_var_name, it.second.second);   \
    }

  if (u->files_removed)
    files[name2file_id[LowerPathIfInsensitive(*u->files_removed)]].def =
        std::nullopt;
  u->file_id = u->files_def_update ? Update(std::move(*u->files_def_update)) : -1;

  RemoveUsrs(SymbolKind::Func, u->file_id, u->funcs_removed);
  Update(u->file_id, std::move(u->funcs_def_update));
  HANDLE_MERGEABLE(funcs_declarations, declarations, funcs, func_usr);
  HANDLE_MERGEABLE(funcs_derived, derived, funcs, func_usr);
  HANDLE_MERGEABLE(funcs_uses, uses, funcs, func_usr);

  RemoveUsrs(SymbolKind::Type, u->file_id, u->types_removed);
  Update(u->file_id, std::move(u->types_def_update));
  HANDLE_MERGEABLE(types_declarations, declarations, types, type_usr);
  HANDLE_MERGEABLE(types_derived, derived, types, type_usr);
  HANDLE_MERGEABLE(types_instances, instances, types, type_usr);
  HANDLE_MERGEABLE(types_uses, uses, types, type_usr);

  RemoveUsrs(SymbolKind::Var, u->file_id, u->vars_removed);
  Update(u->file_id, std::move(u->vars_def_update));
  HANDLE_MERGEABLE(vars_declarations, declarations, vars, var_usr);
  HANDLE_MERGEABLE(vars_uses, uses, vars, var_usr);

#undef HANDLE_MERGEABLE
}

int DB::Update(QueryFile::DefUpdate&& u) {
  int id = files.size();
  auto it = name2file_id.try_emplace(LowerPathIfInsensitive(u.value.path), id);
  if (it.second)
    files.emplace_back().id = id;
  QueryFile& existing = files[it.first->second];
  existing.def = u.value;
  return existing.id;
}

void DB::Update(int file_id, std::vector<std::pair<Usr, QueryFunc::Def>>&& us) {
  for (auto& u : us) {
    auto& def = u.second;
    assert(!def.detailed_name.empty());
    AssignFileId(file_id, def.spell);
    AssignFileId(file_id, def.extent);
    AssignFileId(file_id, def.callees);
    auto R = func_usr.try_emplace(u.first, func_usr.size());
    if (R.second)
      funcs.emplace_back();
    QueryFunc& existing = funcs[R.first->second];
    existing.usr = u.first;
    if (!TryReplaceDef(existing.def, std::move(def)))
      existing.def.push_back(std::move(def));
  }
}

void DB::Update(int file_id, std::vector<std::pair<Usr, QueryType::Def>>&& us) {
  for (auto& u : us) {
    auto& def = u.second;
    assert(!def.detailed_name.empty());
    AssignFileId(file_id, def.spell);
    AssignFileId(file_id, def.extent);
    auto R = type_usr.try_emplace(u.first, type_usr.size());
    if (R.second)
      types.emplace_back();
    QueryType& existing = types[R.first->second];
    existing.usr = u.first;
    if (!TryReplaceDef(existing.def, std::move(def)))
      existing.def.push_back(std::move(def));

  }
}

void DB::Update(int file_id, std::vector<std::pair<Usr, QueryVar::Def>>&& us) {
  for (auto& u : us) {
    auto& def = u.second;
    assert(!def.detailed_name.empty());
    AssignFileId(file_id, def.spell);
    AssignFileId(file_id, def.extent);
    auto R = var_usr.try_emplace(u.first, var_usr.size());
    if (R.second)
      vars.emplace_back();
    QueryVar& existing = vars[R.first->second];
    existing.usr = u.first;
    if (!TryReplaceDef(existing.def, std::move(def)))
      existing.def.push_back(std::move(def));
  }
}

std::string_view DB::GetSymbolName(SymbolIdx sym, bool qualified) {
  Usr usr = sym.usr;
  switch (sym.kind) {
    default:
      break;
    case SymbolKind::File:
      if (files[usr].def)
        return files[usr].def->path;
      break;
    case SymbolKind::Func:
      if (const auto* def = Func(usr).AnyDef())
        return def->Name(qualified);
      break;
    case SymbolKind::Type:
      if (const auto* def = Type(usr).AnyDef())
        return def->Name(qualified);
      break;
    case SymbolKind::Var:
      if (const auto* def = Var(usr).AnyDef())
        return def->Name(qualified);
      break;
  }
  return "";
}
