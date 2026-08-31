#include "StaticSymbolAudit.hpp"

#include <cstdio>
#include <string>

using namespace ipasim::probe::static_symbol_audit_detail;

namespace {

bool expect(bool Condition, const char *Message) {
  if (Condition)
    return true;
  std::fprintf(stderr, "[static-symbol-audit-smoke] FAIL: %s\n", Message);
  return false;
}

Image makeImage(const char *PathName) {
  Image Result;
  Result.Path = PathName;
  Result.DisplayName = PathName;
  return Result;
}

} // namespace

int main() {
  bool Ok = true;

  Context Ctx;
  Ctx.RuntimeRoot = ".";
  Ctx.MainExecutable = "main";
  Ctx.BridgePath = "bridge-does-not-participate-in-this-smoke";

  Image Main = makeImage("main");
  Main.Dependencies.push_back(Dependency{LcLoadDylib, "provider-a"});
  Main.Dependencies.push_back(Dependency{LcLoadDylib, "provider-b"});

  Image ProviderA = makeImage("provider-a");
  ProviderA.Exports.emplace("_foo", ExportEntry{});
  ProviderA.Exports.emplace("_bar", ExportEntry{});

  Image ProviderB = makeImage("provider-b");

  const std::string MainKey = pathKey("main");
  const std::string AKey = pathKey("provider-a");
  const std::string BKey = pathKey("provider-b");
  Ctx.Images.emplace(MainKey, Main);
  Ctx.Images.emplace(AKey, ProviderA);
  Ctx.Images.emplace(BKey, ProviderB);

  std::string Reason;
  Ok &= expect(providerHasSymbol(Ctx, AKey, "_foo", Reason),
               "expected provider A should satisfy its own _foo export");

  Reason.clear();
  Ok &= expect(!providerHasSymbol(Ctx, BKey, "_foo", Reason),
               "provider B must not inherit _foo merely because provider A exports it");

  // The importing image explicitly binds _foo to ordinal 2 (provider B).
  // A global required-minus-provided union would incorrectly mark this as
  // satisfied by provider A; the two-level audit must keep it missing.
  Ctx.Images[MainKey].Imports.push_back(Import{2, false, "_foo"});
  std::size_t RequiredCount = 0;
  std::size_t WeakMissing = 0;
  auto Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  Ok &= expect(RequiredCount == 1,
               "one required ordinal-bound import should be counted");
  Ok &= expect(Missing.size() == 1 && Missing[0].Symbol == "_foo" &&
                   Missing[0].ExpectedLibrary == "provider-b",
               "wrong-provider export must not satisfy two-level import");

  // Put the symbol in the expected provider and verify the binding clears.
  Ctx.Images[BKey].Exports.emplace("_foo", ExportEntry{});
  Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  Ok &= expect(Missing.empty(),
               "expected-provider export should satisfy ordinal-bound import");

  // Named export-trie reexports must follow their declared ordinal rather than
  // using the global closure namespace.
  ExportEntry Reexport;
  Reexport.Type = ExportEntry::Kind::Reexport;
  Reexport.LibOrdinal = 1;
  Reexport.ImportName = "_bar";
  Ctx.Images[BKey].Dependencies.push_back(
      Dependency{LcLoadDylib, "provider-a"});
  Ctx.Images[BKey].Exports.emplace("_alias", Reexport);
  Reason.clear();
  Ok &= expect(providerHasSymbol(Ctx, BKey, "_alias", Reason),
               "named reexport should resolve through its declared dependency");

  // Classic LC_REEXPORT_DYLIB exposes the dependency namespace when the symbol
  // is not directly present in the provider image.
  Ctx.Images[BKey].Dependencies.push_back(
      Dependency{LcReexportDylib, "provider-a"});
  Reason.clear();
  Ok &= expect(providerHasSymbol(Ctx, BKey, "_bar", Reason),
               "classic reexport dependency should expose _bar");

  // Weak missing imports are visible diagnostically but do not inflate the
  // required-import count.
  Ctx.Images[MainKey].Imports.clear();
  Ctx.Images[MainKey].Imports.push_back(Import{2, true, "_weak_missing"});
  Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  Ok &= expect(RequiredCount == 0 && WeakMissing == 1 && Missing.size() == 1 &&
                   Missing[0].Weak,
               "weak missing binding should be classified separately");

  // Flat lookup is the one namespace where a symbol from any loaded provider
  // may satisfy the import.
  Ctx.Images[MainKey].Imports.clear();
  Ctx.Images[MainKey].Imports.push_back(
      Import{BindSpecialDylibFlatLookup, false, "_bar"});
  Missing = findMissingBindings(Ctx, RequiredCount, WeakMissing);
  Ok &= expect(Missing.empty(),
               "flat lookup should allow closure-wide provider search");

  if (!Ok)
    return 1;

  std::printf("[static-symbol-audit-smoke] two-level namespace, reexport, weak, and flat-lookup semantics passed\n");
  return 0;
}
