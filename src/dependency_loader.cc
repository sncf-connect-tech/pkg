#include "pkg/dependency_loader.h"

#include <iostream>
#include <set>
#include <sstream>

#include "boost/filesystem.hpp"

#include "fmt/format.h"

#include "utl/erase.h"
#include "utl/get_or_create.h"
#include "utl/parser/util.h"
#include "utl/to_vec.h"

#include "pkg/git.h"
#include "pkg/read_deps.h"

namespace fs = boost::filesystem;

namespace pkg {

dependency_loader::dependency_loader(fs::path deps_root)
    : deps_root_{std::move(deps_root)} {}

dependency_loader::~dependency_loader() = default;

void dependency_loader::retrieve(
    fs::path const& p, dependency_loader::iteration_fn_t const& iterate) {
  auto& d = dep_mem_.emplace_back(std::make_unique<dep>(dep::root(p)));
  retrieve(deps_[ROOT] = d.get(), iterate);
}

std::vector<dep*> dependency_loader::sorted() {
  auto written = std::set<dep*>{};
  auto sorted = std::vector<dep*>{};
  auto all = get_all();
  while (!all.empty()) {
    // Find the first dep without unwritten successors.
    // There has to be at least one in a cycle-free dependency graph.
    auto const next = std::find_if(begin(all), end(all), [&](auto&& d) {
      return std::all_of(begin(d->succs_), end(d->succs_), [&](auto&& s) {
        return written.find(s) != end(written);
      });
    });
    verify(next != end(all), "cycle detected");

    auto d = (*next);
    written.emplace(d);
    if (d->name() != ROOT) {
      sorted.emplace_back(d);
    }
    all.erase(next);
  }
  return sorted;
}

std::vector<dep*> dependency_loader::get_all() const {
  return utl::to_vec(dep_mem_, [](auto&& d) { return d.get(); });
}

void dependency_loader::retrieve(
    dep* pred, dependency_loader::iteration_fn_t const& iterate) {
  for (auto const& d : read_deps(deps_root_, pred)) {
    auto succ = utl::get_or_create(deps_, d.url_, [&]() {
      auto next = dep_mem_.emplace_back(std::make_unique<dep>(d)).get();
      iterate(next);
      retrieve(next, iterate);
      return next;
    });

    if (succ->commit_ != d.commit_) {
      fmt::print("non-matching ref: {}\n", d.url_);
      fmt::print("{} from: {}\n", d.commit_, pred->url_);
      for (auto const* succ_pred : succ->preds_) {
        fmt::print("{} from: {}\n", succ->commit_, succ_pred->url_);
      }
      throw std::runtime_error{"non-matching ref"};
    }

    succ->preds_.insert(pred);
    pred->succs_.insert(succ);
  }
}

}  // namespace pkg