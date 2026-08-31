#pragma once

#include "StaticSymbolAudit.hpp"

#include <cstdio>
#include <string>

namespace ipasim::probe {

inline bool runStaticSymbolAuditSelfTest() {
  using namespace static_symbol_audit_detail;

  Context Ctx;
  Ctx.RuntimeRoot = ".";
  Ctx.MainExecutable = "main";
  Ctx.BridgePath = "bridge-does-not-participate-in-this-self-test";

  Image Main;
  Main.Path = "main";
  Main.DisplayName = "main";
  Main.Dependencies.push_back(Dependency{LcLoadDylib, "provider-a"});
  Main.Dependencies.push_back(Dependency{LcLoadDylib, "provider-b"});

  Image ProviderA;
  ProviderA.Path = "provider-a";
  ProviderA.DisplayName = "provider-a";
  ProviderA.Exports.emplace("_foo", ExportEntry{});
  ProviderA.Exports.emplace("_bar", ExportEntry{});

  Image ProviderB;
  ProviderB.Path = "provider-b";
  ProviderB.DisplayName = "provider-b";

  const std::string MainKey = pathKey("main");
  const std::string AKey = pathKey("provider-a");
  const std::string BKey = pathKey("provider-b");
  Ctx.Images.emplace(MainKey, Main);
  Ctx.Images.emplace(AKey, ProviderA);
  Ctx.Images.emplace(BKey, ProviderB);

  auto Fail = [](const char *Message) {
    std::fprintf(stderr, "[static-symbol-audit-self-test] FAIL: %s\n", Message);
    return false;
  };

  std::string Reason;
  if (!providerHasSymbol(Ctx, AKey, "_foo", Reason))
    return Fail("expected provider must satisfy its own export");
  Reason.clear();
  if (providerHasSymbol(Ctx, BKey, "_foo", Reason))
    return Fail("wrong provider must not inherit another image's export");

  // Positive ordinal 2 names provider B. Provider A exporting the same symbol
  // must not satisfy this two-level import.
  Ctx.Images[MainKey].Imports.push_back(Import{2, false, "_foo"});
  std::size_t RequiredCount = 0;
  std::size_t WeakMissing = 0;
  auto Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  if (RequiredCount != 1 || Missing.size() != 1 ||
      Missing[0].ExpectedLibrary != "provider-b" ||
      Missing[0].Symbol != "_foo")
    return Fail("positive ordinal was incorrectly flattened across providers");

  Ctx.Images[BKey].Exports.emplace("_foo", ExportEntry{});
  Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  if (!Missing.empty())
    return Fail("expected provider export did not satisfy its ordinal binding");

  // A named export-trie reexport must recurse through the reexport's declared
  // ordinal and import name.
  Ctx.Images[BKey].Dependencies.push_back(
      Dependency{LcLoadDylib, "provider-a"});
  ExportEntry NamedReexport;
  NamedReexport.Type = ExportEntry::Kind::Reexport;
  NamedReexport.LibOrdinal = 1;
  NamedReexport.ImportName = "_bar";
  Ctx.Images[BKey].Exports.emplace("_alias", NamedReexport);
  Reason.clear();
  if (!providerHasSymbol(Ctx, BKey, "_alias", Reason))
    return Fail("named reexport did not follow its declared dependency");

  // LC_REEXPORT_DYLIB exposes the dependency namespace for symbols not directly
  // exported by the provider.
  Ctx.Images[BKey].Dependencies.push_back(
      Dependency{LcReexportDylib, "provider-a"});
  Reason.clear();
  if (!providerHasSymbol(Ctx, BKey, "_bar", Reason))
    return Fail("classic reexport dependency did not expose its symbol");

  Ctx.Images[MainKey].Imports.clear();
  Ctx.Images[MainKey].Imports.push_back(Import{2, true, "_weak_missing"});
  Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  if (RequiredCount != 0 || WeakMissing != 1 || Missing.size() != 1 ||
      !Missing[0].Weak)
    return Fail("weak missing binding classification is wrong");

  // Flat lookup is intentionally the one place where closure-wide symbol
  // availability can satisfy an import.
  Ctx.Images[MainKey].Imports.clear();
  Ctx.Images[MainKey].Imports.push_back(
      Import{BindSpecialDylibFlatLookup, false, "_bar"});
  Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  if (!Missing.empty())
    return Fail("flat lookup did not search the closure-wide namespace");

  return true;
}

} // namespace ipasim::probe
