//===---- ExecutionUtils.cpp - Utilities for executing functions in Orc ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/JITLink/x86_64.h"
#include "llvm/ExecutionEngine/Orc/Layer.h"
#include "llvm/ExecutionEngine/Orc/ObjectFileInterface.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Target/TargetMachine.h"
#include <string>

namespace llvm {
namespace orc {

CtorDtorIterator::CtorDtorIterator(const GlobalVariable *GV, bool End)
  : InitList(
      GV ? dyn_cast_or_null<ConstantArray>(GV->getInitializer()) : nullptr),
    I((InitList && End) ? InitList->getNumOperands() : 0) {
}

bool CtorDtorIterator::operator==(const CtorDtorIterator &Other) const {
  assert(InitList == Other.InitList && "Incomparable iterators.");
  return I == Other.I;
}

bool CtorDtorIterator::operator!=(const CtorDtorIterator &Other) const {
  return !(*this == Other);
}

CtorDtorIterator& CtorDtorIterator::operator++() {
  ++I;
  return *this;
}

CtorDtorIterator CtorDtorIterator::operator++(int) {
  CtorDtorIterator Temp = *this;
  ++I;
  return Temp;
}

CtorDtorIterator::Element CtorDtorIterator::operator*() const {
  ConstantStruct *CS = dyn_cast<ConstantStruct>(InitList->getOperand(I));
  assert(CS && "Unrecognized type in llvm.global_ctors/llvm.global_dtors");

  Constant *FuncC = CS->getOperand(1);
  Function *Func = nullptr;

  // Extract function pointer, pulling off any casts.
  while (FuncC) {
    if (Function *F = dyn_cast_or_null<Function>(FuncC)) {
      Func = F;
      break;
    } else if (ConstantExpr *CE = dyn_cast_or_null<ConstantExpr>(FuncC)) {
      if (CE->isCast())
        FuncC = CE->getOperand(0);
      else
        break;
    } else {
      // This isn't anything we recognize. Bail out with Func left set to null.
      break;
    }
  }

  auto *Priority = cast<ConstantInt>(CS->getOperand(0));
  Value *Data = CS->getNumOperands() == 3 ? CS->getOperand(2) : nullptr;
  if (Data && !isa<GlobalValue>(Data))
    Data = nullptr;
  return Element(Priority->getZExtValue(), Func, Data);
}

iterator_range<CtorDtorIterator> getConstructors(const Module &M) {
  const GlobalVariable *CtorsList = M.getNamedGlobal("llvm.global_ctors");
  return make_range(CtorDtorIterator(CtorsList, false),
                    CtorDtorIterator(CtorsList, true));
}

iterator_range<CtorDtorIterator> getDestructors(const Module &M) {
  const GlobalVariable *DtorsList = M.getNamedGlobal("llvm.global_dtors");
  return make_range(CtorDtorIterator(DtorsList, false),
                    CtorDtorIterator(DtorsList, true));
}

bool StaticInitGVIterator::isStaticInitGlobal(GlobalValue &GV) {
  if (GV.isDeclaration())
    return false;

  if (GV.hasName() && (GV.getName() == "llvm.global_ctors" ||
                       GV.getName() == "llvm.global_dtors"))
    return true;

  if (ObjFmt == Triple::MachO) {
    // FIXME: These section checks are too strict: We should match first and
    // second word split by comma.
    if (GV.hasSection() &&
        (GV.getSection().startswith("__DATA,__objc_classlist") ||
         GV.getSection().startswith("__DATA,__objc_selrefs")))
      return true;
  }

  return false;
}

void CtorDtorRunner::add(iterator_range<CtorDtorIterator> CtorDtors) {
  if (CtorDtors.empty())
    return;

  MangleAndInterner Mangle(
      JD.getExecutionSession(),
      (*CtorDtors.begin()).Func->getParent()->getDataLayout());

  for (auto CtorDtor : CtorDtors) {
    assert(CtorDtor.Func && CtorDtor.Func->hasName() &&
           "Ctor/Dtor function must be named to be runnable under the JIT");

    // FIXME: Maybe use a symbol promoter here instead.
    if (CtorDtor.Func->hasLocalLinkage()) {
      CtorDtor.Func->setLinkage(GlobalValue::ExternalLinkage);
      CtorDtor.Func->setVisibility(GlobalValue::HiddenVisibility);
    }

    if (CtorDtor.Data && cast<GlobalValue>(CtorDtor.Data)->isDeclaration()) {
      dbgs() << "  Skipping because why now?\n";
      continue;
    }

    CtorDtorsByPriority[CtorDtor.Priority].push_back(
        Mangle(CtorDtor.Func->getName()));
  }
}

Error CtorDtorRunner::run() {
  using CtorDtorTy = void (*)();

  SymbolLookupSet LookupSet;
  for (auto &KV : CtorDtorsByPriority)
    for (auto &Name : KV.second)
      LookupSet.add(Name);
  assert(!LookupSet.containsDuplicates() &&
         "Ctor/Dtor list contains duplicates");

  auto &ES = JD.getExecutionSession();
  if (auto CtorDtorMap = ES.lookup(
          makeJITDylibSearchOrder(&JD, JITDylibLookupFlags::MatchAllSymbols),
          std::move(LookupSet))) {
    for (auto &KV : CtorDtorsByPriority) {
      for (auto &Name : KV.second) {
        assert(CtorDtorMap->count(Name) && "No entry for Name");
        auto CtorDtor = reinterpret_cast<CtorDtorTy>(
            static_cast<uintptr_t>((*CtorDtorMap)[Name].getAddress()));
        CtorDtor();
      }
    }
    CtorDtorsByPriority.clear();
    return Error::success();
  } else
    return CtorDtorMap.takeError();
}

void LocalCXXRuntimeOverridesBase::runDestructors() {
  auto& CXXDestructorDataPairs = DSOHandleOverride;
  for (auto &P : CXXDestructorDataPairs)
    P.first(P.second);
  CXXDestructorDataPairs.clear();
}

int LocalCXXRuntimeOverridesBase::CXAAtExitOverride(DestructorPtr Destructor,
                                                    void *Arg,
                                                    void *DSOHandle) {
  auto& CXXDestructorDataPairs =
    *reinterpret_cast<CXXDestructorDataPairList*>(DSOHandle);
  CXXDestructorDataPairs.push_back(std::make_pair(Destructor, Arg));
  return 0;
}

Error LocalCXXRuntimeOverrides::enable(JITDylib &JD,
                                        MangleAndInterner &Mangle) {
  SymbolMap RuntimeInterposes;
  RuntimeInterposes[Mangle("__dso_handle")] =
    JITEvaluatedSymbol(toTargetAddress(&DSOHandleOverride),
                       JITSymbolFlags::Exported);
  RuntimeInterposes[Mangle("__cxa_atexit")] =
    JITEvaluatedSymbol(toTargetAddress(&CXAAtExitOverride),
                       JITSymbolFlags::Exported);

  return JD.define(absoluteSymbols(std::move(RuntimeInterposes)));
}

void ItaniumCXAAtExitSupport::registerAtExit(void (*F)(void *), void *Ctx,
                                             void *DSOHandle) {
  std::lock_guard<std::mutex> Lock(AtExitsMutex);
  AtExitRecords[DSOHandle].push_back({F, Ctx});
}

void ItaniumCXAAtExitSupport::runAtExits(void *DSOHandle) {
  std::vector<AtExitRecord> AtExitsToRun;

  {
    std::lock_guard<std::mutex> Lock(AtExitsMutex);
    auto I = AtExitRecords.find(DSOHandle);
    if (I != AtExitRecords.end()) {
      AtExitsToRun = std::move(I->second);
      AtExitRecords.erase(I);
    }
  }

  while (!AtExitsToRun.empty()) {
    AtExitsToRun.back().F(AtExitsToRun.back().Ctx);
    AtExitsToRun.pop_back();
  }
}

DynamicLibrarySearchGenerator::DynamicLibrarySearchGenerator(
    sys::DynamicLibrary Dylib, char GlobalPrefix, SymbolPredicate Allow)
    : Dylib(std::move(Dylib)), Allow(std::move(Allow)),
      GlobalPrefix(GlobalPrefix) {}

Expected<std::unique_ptr<DynamicLibrarySearchGenerator>>
DynamicLibrarySearchGenerator::Load(const char *FileName, char GlobalPrefix,
                                    SymbolPredicate Allow) {
  std::string ErrMsg;
  auto Lib = sys::DynamicLibrary::getPermanentLibrary(FileName, &ErrMsg);
  if (!Lib.isValid())
    return make_error<StringError>(std::move(ErrMsg), inconvertibleErrorCode());
  return std::make_unique<DynamicLibrarySearchGenerator>(
      std::move(Lib), GlobalPrefix, std::move(Allow));
}

Error DynamicLibrarySearchGenerator::tryToGenerate(
    LookupState &LS, LookupKind K, JITDylib &JD,
    JITDylibLookupFlags JDLookupFlags, const SymbolLookupSet &Symbols) {
  orc::SymbolMap NewSymbols;

  bool HasGlobalPrefix = (GlobalPrefix != '\0');

  for (auto &KV : Symbols) {
    auto &Name = KV.first;

    if ((*Name).empty())
      continue;

    if (Allow && !Allow(Name))
      continue;

    if (HasGlobalPrefix && (*Name).front() != GlobalPrefix)
      continue;

    std::string Tmp((*Name).data() + HasGlobalPrefix,
                    (*Name).size() - HasGlobalPrefix);
    if (void *Addr = Dylib.getAddressOfSymbol(Tmp.c_str())) {
      NewSymbols[Name] = JITEvaluatedSymbol(
          static_cast<JITTargetAddress>(reinterpret_cast<uintptr_t>(Addr)),
          JITSymbolFlags::Exported);
    }
  }

  if (NewSymbols.empty())
    return Error::success();

  return JD.define(absoluteSymbols(std::move(NewSymbols)));
}

Expected<std::unique_ptr<StaticLibraryDefinitionGenerator>>
StaticLibraryDefinitionGenerator::Load(
    ObjectLayer &L, const char *FileName,
    GetObjectFileInterface GetObjFileInterface) {
  auto ArchiveBuffer = MemoryBuffer::getFile(FileName);

  if (!ArchiveBuffer)
    return createFileError(FileName, ArchiveBuffer.getError());

  return Create(L, std::move(*ArchiveBuffer), std::move(GetObjFileInterface));
}

Expected<std::unique_ptr<StaticLibraryDefinitionGenerator>>
StaticLibraryDefinitionGenerator::Load(
    ObjectLayer &L, const char *FileName, const Triple &TT,
    GetObjectFileInterface GetObjFileInterface) {

  auto B = object::createBinary(FileName);
  if (!B)
    return createFileError(FileName, B.takeError());

  // If this is a regular archive then create an instance from it.
  if (isa<object::Archive>(B->getBinary()))
    return Create(L, std::move(B->takeBinary().second),
                  std::move(GetObjFileInterface));

  // If this is a universal binary then search for a slice matching the given
  // Triple.
  if (auto *UB = cast<object::MachOUniversalBinary>(B->getBinary())) {
    for (const auto &Obj : UB->objects()) {
      auto ObjTT = Obj.getTriple();
      if (ObjTT.getArch() == TT.getArch() &&
          ObjTT.getSubArch() == TT.getSubArch() &&
          (TT.getVendor() == Triple::UnknownVendor ||
           ObjTT.getVendor() == TT.getVendor())) {
        // We found a match. Create an instance from a buffer covering this
        // slice.
        auto SliceBuffer = MemoryBuffer::getFileSlice(FileName, Obj.getSize(),
                                                      Obj.getOffset());
        if (!SliceBuffer)
          return make_error<StringError>(
              Twine("Could not create buffer for ") + TT.str() + " slice of " +
                  FileName + ": [ " + formatv("{0:x}", Obj.getOffset()) +
                  " .. " + formatv("{0:x}", Obj.getOffset() + Obj.getSize()) +
                  ": " + SliceBuffer.getError().message(),
              SliceBuffer.getError());
        return Create(L, std::move(*SliceBuffer),
                      std::move(GetObjFileInterface));
      }
    }

    return make_error<StringError>(Twine("Universal binary ") + FileName +
                                       " does not contain a slice for " +
                                       TT.str(),
                                   inconvertibleErrorCode());
  }

  return make_error<StringError>(Twine("Unrecognized file type for ") +
                                     FileName,
                                 inconvertibleErrorCode());
}

Expected<std::unique_ptr<StaticLibraryDefinitionGenerator>>
StaticLibraryDefinitionGenerator::Create(
    ObjectLayer &L, std::unique_ptr<MemoryBuffer> ArchiveBuffer,
    GetObjectFileInterface GetObjFileInterface) {
  Error Err = Error::success();

  std::unique_ptr<StaticLibraryDefinitionGenerator> ADG(
      new StaticLibraryDefinitionGenerator(
          L, std::move(ArchiveBuffer), std::move(GetObjFileInterface), Err));

  if (Err)
    return std::move(Err);

  return std::move(ADG);
}

Error StaticLibraryDefinitionGenerator::tryToGenerate(
    LookupState &LS, LookupKind K, JITDylib &JD,
    JITDylibLookupFlags JDLookupFlags, const SymbolLookupSet &Symbols) {
  // Don't materialize symbols from static archives unless this is a static
  // lookup.
  if (K != LookupKind::Static)
    return Error::success();

  // Bail out early if we've already freed the archive.
  if (!Archive)
    return Error::success();

  DenseSet<std::pair<StringRef, StringRef>> ChildBufferInfos;

  for (const auto &KV : Symbols) {
    const auto &Name = KV.first;
    if (!ObjectFilesMap.count(Name))
      continue;
    auto ChildBuffer = ObjectFilesMap[Name];
    ChildBufferInfos.insert(
        {ChildBuffer.getBuffer(), ChildBuffer.getBufferIdentifier()});
  }

  for (auto ChildBufferInfo : ChildBufferInfos) {
    MemoryBufferRef ChildBufferRef(ChildBufferInfo.first,
                                   ChildBufferInfo.second);

    auto I = GetObjFileInterface(L.getExecutionSession(), ChildBufferRef);
    if (!I)
      return I.takeError();

    if (auto Err = L.add(JD, MemoryBuffer::getMemBuffer(ChildBufferRef, false),
                         std::move(*I)))
      return Err;
  }

  return Error::success();
}

Error StaticLibraryDefinitionGenerator::buildObjectFilesMap() {
  DenseMap<uint64_t, MemoryBufferRef> MemoryBuffers;
  DenseSet<uint64_t> Visited;
  DenseSet<uint64_t> Excluded;
  for (auto &S : Archive->symbols()) {
    StringRef SymName = S.getName();
    auto Member = S.getMember();
    if (!Member)
      return Member.takeError();
    auto DataOffset = Member->getDataOffset();
    if (!Visited.count(DataOffset)) {
      Visited.insert(DataOffset);
      auto Child = Member->getAsBinary();
      if (!Child)
        return Child.takeError();
      if ((*Child)->isCOFFImportFile()) {
        ImportedDynamicLibraries.insert((*Child)->getFileName().str());
        Excluded.insert(DataOffset);
        continue;
      }
      MemoryBuffers[DataOffset] = (*Child)->getMemoryBufferRef();
    }
    if (!Excluded.count(DataOffset))
      ObjectFilesMap[L.getExecutionSession().intern(SymName)] =
          MemoryBuffers[DataOffset];
  }

  return Error::success();
}

StaticLibraryDefinitionGenerator::StaticLibraryDefinitionGenerator(
    ObjectLayer &L, std::unique_ptr<MemoryBuffer> ArchiveBuffer,
    GetObjectFileInterface GetObjFileInterface, Error &Err)
    : L(L), GetObjFileInterface(std::move(GetObjFileInterface)),
      ArchiveBuffer(std::move(ArchiveBuffer)),
      Archive(std::make_unique<object::Archive>(*this->ArchiveBuffer, Err)) {
  ErrorAsOutParameter _(&Err);
  if (!this->GetObjFileInterface)
    this->GetObjFileInterface = getObjectFileInterface;
  if (!Err)
    Err = buildObjectFilesMap();
}

std::unique_ptr<DLLImportDefinitionGenerator>
DLLImportDefinitionGenerator::Create(ExecutionSession &ES,
                                     ObjectLinkingLayer &L) {
  return std::unique_ptr<DLLImportDefinitionGenerator>(
      new DLLImportDefinitionGenerator(ES, L));
}

Error DLLImportDefinitionGenerator::tryToGenerate(
    LookupState &LS, LookupKind K, JITDylib &JD,
    JITDylibLookupFlags JDLookupFlags, const SymbolLookupSet &Symbols) {
  JITDylibSearchOrder LinkOrder;
  JD.withLinkOrderDo([&](const JITDylibSearchOrder &LO) {
    LinkOrder.reserve(LO.size());
    for (auto &KV : LO) {
      if (KV.first == &JD)
        continue;
      LinkOrder.push_back(KV);
    }
  });

  // FIXME: if regular symbol name start with __imp_ we have to issue lookup of
  // both __imp_ and stripped name and use the lookup information to resolve the
  // real symbol name.
  SymbolLookupSet LookupSet;
  DenseMap<StringRef, SymbolLookupFlags> ToLookUpSymbols;
  for (auto &KV : Symbols) {
    StringRef Deinterned = *KV.first;
    if (Deinterned.startswith(getImpPrefix()))
      Deinterned = Deinterned.drop_front(StringRef(getImpPrefix()).size());
    // Don't degrade the required state
    if (ToLookUpSymbols.count(Deinterned) &&
        ToLookUpSymbols[Deinterned] == SymbolLookupFlags::RequiredSymbol)
      continue;
    ToLookUpSymbols[Deinterned] = KV.second;
  }

  for (auto &KV : ToLookUpSymbols)
    LookupSet.add(ES.intern(KV.first), KV.second);

  auto Resolved =
      ES.lookup(LinkOrder, LookupSet, LookupKind::DLSym, SymbolState::Resolved);
  if (!Resolved)
    return Resolved.takeError();

  auto G = createStubsGraph(*Resolved);
  if (!G)
    return G.takeError();
  return L.add(JD, std::move(*G));
}

Expected<unsigned>
DLLImportDefinitionGenerator::getTargetPointerSize(const Triple &TT) {
  switch (TT.getArch()) {
  case Triple::x86_64:
    return 8;
  default:
    return make_error<StringError>(
        "architecture unsupported by DLLImportDefinitionGenerator",
        inconvertibleErrorCode());
  }
}

Expected<support::endianness>
DLLImportDefinitionGenerator::getTargetEndianness(const Triple &TT) {
  switch (TT.getArch()) {
  case Triple::x86_64:
    return support::endianness::little;
  default:
    return make_error<StringError>(
        "architecture unsupported by DLLImportDefinitionGenerator",
        inconvertibleErrorCode());
  }
}

Expected<std::unique_ptr<jitlink::LinkGraph>>
DLLImportDefinitionGenerator::createStubsGraph(const SymbolMap &Resolved) {
  Triple TT = ES.getExecutorProcessControl().getTargetTriple();
  auto PointerSize = getTargetEndianness(TT);
  if (!PointerSize)
    return PointerSize.takeError();
  auto Endianness = getTargetEndianness(TT);
  if (!Endianness)
    return Endianness.takeError();

  auto G = std::make_unique<jitlink::LinkGraph>(
      "<DLLIMPORT_STUBS>", TT, *PointerSize, *Endianness,
      jitlink::getGenericEdgeKindName);
  jitlink::Section &Sec =
      G->createSection(getSectionName(), MemProt::Read | MemProt::Exec);

  for (auto &KV : Resolved) {
    jitlink::Symbol &Target = G->addAbsoluteSymbol(
        *KV.first, ExecutorAddr(KV.second.getAddress()), *PointerSize,
        jitlink::Linkage::Strong, jitlink::Scope::Local, false);

    // Create __imp_ symbol
    jitlink::Symbol &Ptr =
        jitlink::x86_64::createAnonymousPointer(*G, Sec, &Target);
    auto NameCopy = G->allocateContent(Twine(getImpPrefix()) + *KV.first);
    StringRef NameCopyRef = StringRef(NameCopy.data(), NameCopy.size());
    Ptr.setName(NameCopyRef);
    Ptr.setLinkage(jitlink::Linkage::Strong);
    Ptr.setScope(jitlink::Scope::Default);

    // Create PLT stub
    // FIXME: check PLT stub of data symbol is not accessed
    jitlink::Block &StubBlock =
        jitlink::x86_64::createPointerJumpStubBlock(*G, Sec, Ptr);
    G->addDefinedSymbol(StubBlock, 0, *KV.first, StubBlock.getSize(),
                        jitlink::Linkage::Strong, jitlink::Scope::Default, true,
                        false);
  }

  return std::move(G);
}

} // End namespace orc.
} // End namespace llvm.
