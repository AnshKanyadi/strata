#pragma once

#include <string>
#include <unordered_map>
#include <utility>

#include "strata/storage/columnar_table.hpp"

namespace strata {

// A name -> table registry used by the binder to resolve table and column names.
// Holds non-owning pointers; the tables must outlive the catalog.
class Catalog {
 public:
  void Add(std::string name, const ColumnarTable& table) {
    tables_[std::move(name)] = &table;
  }
  const ColumnarTable* Find(const std::string& name) const {
    const auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second;
  }

 private:
  std::unordered_map<std::string, const ColumnarTable*> tables_;
};

}  // namespace strata
