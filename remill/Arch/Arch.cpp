/*
 * Copyright (c) 2017 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <memory>
#include <unordered_map>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "remill/Arch/Arch.h"
#include "remill/Arch/Name.h"

#include "remill/BC/ABI.h"
#include "remill/BC/Compat/Attributes.h"
#include "remill/BC/Compat/DebugInfo.h"
#include "remill/BC/Compat/GlobalValue.h"
#include "remill/BC/Util.h"
#include "remill/BC/Version.h"

#include "remill/OS/OS.h"

DEFINE_string(arch, REMILL_ARCH,
              "Architecture of the code being translated. "
              "Valid architectures: x86, amd64 (with or without "
              "`_avx` or `_avx512` appended), aarch64, "
              "mips32, mips64");

DECLARE_string(os);

namespace remill {
namespace {

static unsigned AddressSize(ArchName arch_name) {
  switch (arch_name) {
    case kArchInvalid:
      LOG(FATAL) << "Cannot get address size for invalid arch.";
      return 0;
    case kArchX86:
    case kArchX86_AVX:
    case kArchX86_AVX512:
    case kArchMips32:
      return 32;
    case kArchAMD64:
    case kArchAMD64_AVX:
    case kArchAMD64_AVX512:
    case kArchMips64:
    case kArchAArch64LittleEndian:
      return 64;
  }
  return 0;
}

static uint64_t ArchKey(llvm::LLVMContext &context_, OSName os_name_,
                        ArchName arch_name_) {
  auto context = reinterpret_cast<uintptr_t>(&context_);
  auto os_name = static_cast<uint64_t>(os_name_);
  auto arch_name = static_cast<uint64_t>(arch_name_);
  return (os_name << 48u) ^ (arch_name << 56u) ^ static_cast<uint64_t>(context);
}

// Used for static storage duration caches of `Arch` specializations. The
// `std::unique_ptr` makes sure that the `Arch` objects are freed on `exit`
// from the program.
using ArchPtr = std::unique_ptr<const Arch>;
using ArchCache = std::unordered_map<uint64_t, ArchPtr>;

}  // namespace

Arch::Arch(llvm::LLVMContext &context_, OSName os_name_, ArchName arch_name_)
    : os_name(os_name_),
      arch_name(arch_name_),
      address_size(AddressSize(arch_name_)),
      context(&context_) {}

Arch::~Arch(void) {}

bool Arch::LazyDecodeInstruction(
    uint64_t address, const std::string &instr_bytes,
    Instruction &inst) const {
  return DecodeInstruction(address, instr_bytes, inst);
}

llvm::Triple Arch::BasicTriple(void) const {
  llvm::Triple triple;
  switch (os_name) {
    case kOSInvalid:
      LOG(FATAL) << "Cannot get triple OS.";
      break;

    case kOSLinux:
    case kOSVxWorks:
      triple.setOS(llvm::Triple::Linux);
      triple.setEnvironment(llvm::Triple::GNU);
      triple.setVendor(llvm::Triple::PC);
      triple.setObjectFormat(llvm::Triple::ELF);
      break;

    case kOSmacOS:
      triple.setOS(llvm::Triple::MacOSX);
      triple.setEnvironment(llvm::Triple::UnknownEnvironment);
      triple.setVendor(llvm::Triple::Apple);
      triple.setObjectFormat(llvm::Triple::MachO);
      break;

    case kOSWindows:
      triple.setOS(llvm::Triple::Win32);
      triple.setEnvironment(llvm::Triple::MSVC);
      triple.setVendor(llvm::Triple::UnknownVendor);
      triple.setObjectFormat(llvm::Triple::COFF);
      break;
  }
  return triple;
}

const Arch *Arch::Get(
    llvm::LLVMContext &context_, OSName os_name_, ArchName arch_name_) {

  static ArchCache gArchCache;
  auto &arch = gArchCache[ArchKey(context_, os_name_, arch_name_)];
  if (arch) {
    return arch.get();
  }

  switch (arch_name_) {
    case kArchInvalid:
      LOG(FATAL) << "Unrecognized architecture.";
      return nullptr;

    case kArchAArch64LittleEndian: {
      DLOG(INFO) << "Using architecture: AArch64, feature set: Little Endian";
      arch.reset(GetAArch64(context_, os_name_, arch_name_));
      break;
    }

    case kArchX86: {
      DLOG(INFO) << "Using architecture: X86";
      arch.reset(GetX86(context_, os_name_, arch_name_));
      break;
    }

    case kArchMips32: {
      DLOG(INFO) << "Using architecture: 32-bit MIPS";
      arch.reset(GetMips(context_, os_name_, arch_name_));
      break;
    }

    case kArchMips64: {
      DLOG(INFO) << "Using architecture: 64-bit MIPS";
      arch.reset(GetMips(context_, os_name_, arch_name_));
      break;
    }

    case kArchX86_AVX: {
      DLOG(INFO) << "Using architecture: X86, feature set: AVX";
      arch.reset(GetX86(context_, os_name_, arch_name_));
      break;
    }

    case kArchX86_AVX512: {
      DLOG(INFO) << "Using architecture: X86, feature set: AVX512";
      arch.reset(GetX86(context_, os_name_, arch_name_));
      break;
    }

    case kArchAMD64: {
      DLOG(INFO) << "Using architecture: AMD64";
      arch.reset(GetX86(context_, os_name_, arch_name_));
      break;
    }

    case kArchAMD64_AVX: {
      DLOG(INFO) << "Using architecture: AMD64, feature set: AVX";
      arch.reset(GetX86(context_, os_name_, arch_name_));
      break;
    }

    case kArchAMD64_AVX512: {
      DLOG(INFO) << "Using architecture: AMD64, feature set: AVX512";
      arch.reset(GetX86(context_, os_name_, arch_name_));
      break;
    }
  }

  return arch.get();
}

// Return information about the register at offset `offset` in the `State`
// structure.
const Register *Arch::RegisterAtStateOffset(uint64_t offset) const {
  if (offset >= reg_by_offset.size()) {
    return nullptr;
  } else {
    return reg_by_offset[offset];  // May be `nullptr`.
  }
}

// Return information about a register, given its name.
const Register *Arch::RegisterByName(const std::string &name) const {
  auto reg_it = reg_by_name.find(name);
  if (reg_it == reg_by_name.end()) {
    return nullptr;
  } else {
    return reg_it->second;
  }
}

const Arch *Arch::GetMips(llvm::LLVMContext &, OSName, ArchName) {
  return nullptr;
}

const Arch *GetHostArch(llvm::LLVMContext &context) {
  static std::unordered_map<llvm::LLVMContext *, const Arch *> gHostArches;
  auto &arch = gHostArches[&context];
  if (!arch) {
    arch = Arch::Get(
        context, GetOSName(REMILL_OS), GetArchName(REMILL_ARCH));
  }
  return arch;
}

const Arch *GetTargetArch(llvm::LLVMContext &context) {
  static std::unordered_map<llvm::LLVMContext *, const Arch *> gTargetArches;
  auto &arch = gTargetArches[&context];
  if (!arch) {
    arch = Arch::Get(context, GetOSName(FLAGS_os), GetArchName(FLAGS_arch));
  }
  return arch;
}

bool Arch::IsX86(void) const {
  switch (arch_name) {
    case remill::kArchX86:
    case remill::kArchX86_AVX:
    case remill::kArchX86_AVX512:
      return true;
    default:
      return false;
  }
}

bool Arch::IsAMD64(void) const {
  switch (arch_name) {
    case remill::kArchAMD64:
    case remill::kArchAMD64_AVX:
    case remill::kArchAMD64_AVX512:
      return true;
    default:
      return false;
  }
}

bool Arch::IsAArch64(void) const {
  return remill::kArchAArch64LittleEndian == arch_name;
}

bool Arch::IsWindows(void) const {
  return remill::kOSWindows == os_name;
}

bool Arch::IsLinux(void) const {
  return remill::kOSLinux == os_name;
}

bool Arch::IsMacOS(void) const {
  return remill::kOSmacOS == os_name;
}

namespace {

// These variables must always be defined within `__remill_basic_block`.
static bool BlockHasSpecialVars(llvm::Function *basic_block) {
  return FindVarInFunction(basic_block, "STATE", true) &&
         FindVarInFunction(basic_block, "MEMORY", true) &&
         FindVarInFunction(basic_block, "PC", true) &&
         FindVarInFunction(basic_block, "BRANCH_TAKEN", true);
}

// Iteratively accumulate the offset, following through GEPs and bitcasts.
static bool GetRegisterOffset(llvm::Module *module, llvm::Type *state_ptr_type,
                              llvm::Value *reg, uint64_t *offset) {

  auto val_name = reg->getName().str();
  llvm::DataLayout dl(module);
  *offset = 0;
  do {
    if (auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(reg)) {
      llvm::APInt gep_offset(dl.getPointerSizeInBits(0), 0);
      if (!gep->accumulateConstantOffset(dl, gep_offset)) {
        DLOG(INFO)
            << "Named variable " << val_name << " is not a register in the "
            << "state structure";
      }
      *offset += gep_offset.getZExtValue();
      reg = gep->getPointerOperand();

    } else if (auto c = llvm::dyn_cast<llvm::CastInst>(reg)) {
      reg = c->getOperand(0);

    // Base case.
    } else if (reg->getType() == state_ptr_type) {
      break;

    } else {
      DLOG(INFO)
          << "Named variable " << val_name << " is not a register in the "
          << "state structure";
      return false;
    }
  } while (reg);

  DLOG(INFO)
      << "Offset of register " << val_name << " in state structure is "
      << *offset;

  return true;
}

// Clang isn't guaranteed to play nice and name the LLVM values within the
// `__remill_basic_block` intrinsic with the same names as we find in the
// C++ definition of that function. However, we compile that function with
// debug information, and so we will try to recover the variables names for
// later lookup.
static void FixupBasicBlockVariables(llvm::Function *basic_block) {
  std::vector<llvm::StoreInst *> stores;
  std::vector<llvm::Instruction *> remove_insts;

  std::unordered_map<llvm::AllocaInst *, llvm::Value *> stored_val;

  for (auto &block : *basic_block) {
    for (auto &inst : block) {
      if (auto debug_inst = llvm::dyn_cast<llvm::DbgInfoIntrinsic>(&inst)) {
        if (auto decl_inst = llvm::dyn_cast<llvm::DbgDeclareInst>(debug_inst)) {
          auto addr = decl_inst->getAddress();
#if LLVM_VERSION_NUMBER >= LLVM_VERSION(3, 7)
          addr->setName(decl_inst->getVariable()->getName());
#else
          llvm::DIVariable var(decl_inst->getVariable());
          addr->setName(var.getName());
#endif
        }
        remove_insts.push_back(debug_inst);

      // Get stores.
      } else if (auto store_inst = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        auto dst_alloca = llvm::dyn_cast<llvm::AllocaInst>(
            store_inst->getPointerOperand());
        if (dst_alloca) {
          stored_val[dst_alloca] = store_inst->getValueOperand();
        }
        stores.push_back(store_inst);

      // Forward stores to loads.
      } else if (auto load_inst = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        auto src_alloca = llvm::dyn_cast<llvm::AllocaInst>(
            load_inst->getPointerOperand());
        if (src_alloca) {
          if (auto val = stored_val[src_alloca]) {
            load_inst->replaceAllUsesWith(val);
            remove_insts.push_back(load_inst);
          }
        }
      } else if (llvm::isa<llvm::BranchInst>(&inst) ||
                 llvm::isa<llvm::CallInst>(&inst) ||
                 llvm::isa<llvm::InvokeInst>(&inst)) {
        LOG(FATAL)
            << "Unsupported instruction in __remill_basic_block: "
            << LLVMThingToString(&inst);
      }
    }
  }

  // At this point, the instructions should have this form:
  //
  //  %BH = alloca i8*, align 8
  //  ...
  //  %24 = getelementptr inbounds ...
  //  store i8* %24, i8** %BH, align 8
  //
  // Our goal is to eliminate the double indirection and get:
  //
  //  %BH = getelementptr inbounds ...

  for (auto inst : stores) {
    auto val = llvm::dyn_cast<llvm::Instruction>(inst->getValueOperand());
    auto ptr = llvm::dyn_cast<llvm::AllocaInst>(inst->getPointerOperand());

    if (val && ptr && val->getType()->isPointerTy() && ptr->hasName()) {
      auto name = ptr->getName().str();
      ptr->setName("");
      val->setName(name);
      remove_insts.push_back(ptr);
      remove_insts.push_back(inst);
    }
  }

  // Remove links between instructions.
  for (auto inst : remove_insts) {
    if (!inst->getType()->isVoidTy()) {
      inst->replaceAllUsesWith(llvm::UndefValue::get(inst->getType()));
    }
  }

  // Remove unneeded instructions.
  for (auto inst : remove_insts) {
    for (auto &operand : inst->operands()) {
      operand = nullptr;
    }
    inst->eraseFromParent();
  }

  CHECK(BlockHasSpecialVars(basic_block))
      << "Unable to locate required variables in `__remill_basic_block`.";
}

// Add attributes to llvm::Argument in a way portable across LLVMs
static void AddNoAliasToArgument(llvm::Argument *arg) {
  IF_LLVM_LT_39(
    arg->addAttr(
      llvm::AttributeSet::get(
        arg->getContext(),
        arg->getArgNo() + 1,
        llvm::Attribute::NoAlias)
    ); 
  );

  IF_LLVM_GTE_39(
    arg->addAttr(llvm::Attribute::NoAlias);
  );
}

// ensures that mandatory remill functions have the correct
// type signature and variable names
static void PrepareModuleRemillFunctions(llvm::Module *mod) {
  auto basic_block = BasicBlockFunction(mod);

  InitFunctionAttributes(basic_block);
  FixupBasicBlockVariables(basic_block);

  basic_block->setLinkage(llvm::GlobalValue::ExternalLinkage);
  basic_block->setVisibility(llvm::GlobalValue::DefaultVisibility);
  basic_block->removeFnAttr(llvm::Attribute::AlwaysInline);
  basic_block->removeFnAttr(llvm::Attribute::InlineHint);
  basic_block->addFnAttr(llvm::Attribute::OptimizeNone);
  basic_block->addFnAttr(llvm::Attribute::NoInline);
  basic_block->setVisibility(llvm::GlobalValue::DefaultVisibility);

  auto memory = remill::NthArgument(basic_block, kMemoryPointerArgNum);
  auto state = remill::NthArgument(basic_block, kStatePointerArgNum);

  memory->setName("");
  state->setName("");
  remill::NthArgument(basic_block, kPCArgNum)->setName("");

  AddNoAliasToArgument(state);
  AddNoAliasToArgument(memory);
}

// Compare two registers for sorting.
static bool RegisterComparator(const Register &lhs, const Register &rhs) {
  // Bigger to appear later in the array. E.g. RAX before EAX.
  if (lhs.size > rhs.size) {
    return true;

  } else if (lhs.size < rhs.size) {
    return false;

  // Registers earlier in the state struct appear earlier in the
  // sort.
  } else if (lhs.offset < rhs.offset) {
    return true;

  } else if (lhs.offset > rhs.offset) {
    return false;

  } else {
    return lhs.order < rhs.order;
  }
}

}  // namespace

Register::Register(const std::string &name_, uint64_t offset_, uint64_t size_,
                   uint64_t order_, llvm::Type *type_)
    : name(name_),
      offset(offset_),
      size(size_),
      order(order_),
      type(type_),
      parent(nullptr) {}

// Returns the enclosing register of size AT LEAST `size`, or `nullptr`.
const Register *Register::EnclosingRegisterOfSize(uint64_t size_) const {
  auto enclosing = this;
  for (; enclosing && enclosing->size < size_; enclosing = enclosing->parent) {
    /* Empty. */;
  }
  return enclosing;
}

const Register *Register::EnclosingRegister(void) const {
  auto enclosing = this;
  while (enclosing->parent) {
    enclosing = enclosing->parent;
  }
  return enclosing;
}

namespace {

// Create an array of index values to pass to a GetElementPtr instruction
// that will let us locate a particular register.
static std::pair<size_t, llvm::Type *>
BuildIndexes(const llvm::DataLayout &dl, llvm::Type *type,
             llvm::Type * const goal_type, size_t offset,
             const size_t goal_offset,
             llvm::SmallVector<llvm::Value *, 8> &indexes_out) {
  if (offset == goal_offset) {
    if (type == goal_type) {
      return {offset, goal_type};
    }
  }

  CHECK_LE(offset, goal_offset);
  CHECK_LE(goal_offset, (offset + dl.getTypeAllocSize(type)));

  size_t index = 0;
  const auto index_type = llvm::Type::getInt32Ty(type->getContext());

  if (const auto struct_type = llvm::dyn_cast<llvm::StructType>(type)) {
    for (const auto elem_type : struct_type->elements()) {
      const auto elem_size = dl.getTypeAllocSize(elem_type);
      if ((offset + elem_size) <= goal_offset) {
        offset += elem_size;
        index += 1;

      } else {
        CHECK_LE(offset, goal_offset);
        CHECK_LE(goal_offset, (offset + elem_size));

        indexes_out.push_back(llvm::ConstantInt::get(index_type, index, false));
        const auto nearest = indexes_out.size();
        const auto ret = BuildIndexes(dl, elem_type, goal_type, offset, goal_offset, indexes_out);
        if (ret.second) {
          return ret;
        }

        indexes_out.resize(nearest);
        return {offset, elem_type};
      }
    }

  } else if (auto seq_type = llvm::dyn_cast<llvm::SequentialType>(type)) {
    const auto elem_type = seq_type->getElementType();
    const auto elem_size = dl.getTypeAllocSize(elem_type);
    const auto num_elems = seq_type->getNumElements();

    while ((offset + elem_size) <= goal_offset && index < num_elems) {
      offset += elem_size;
      index += 1;
    }

    CHECK_LE(offset, goal_offset);
    CHECK_LE(goal_offset, (offset + elem_size));

    indexes_out.push_back(llvm::ConstantInt::get(index_type, index, false));
    const auto nearest = indexes_out.size();
    const auto ret = BuildIndexes(dl, elem_type, goal_type, offset, goal_offset, indexes_out);
    if (ret.second) {
      return ret;
    }

    indexes_out.resize(nearest);
    return {offset, elem_type};

  } else if (type->isIntegerTy() || type->isFloatingPointTy()) {

    // We may need to bitcast.
    if (offset == goal_offset) {
      return {offset, type};
    }
  }

  return {offset, nullptr};
}

}  // namespace

// Generate a GEP that will let us load/store to this register, given
// a `State *`.
llvm::Instruction *Register::AddressOf(llvm::Value *state_ptr,
                                       llvm::BasicBlock *add_to_end) const {
  CHECK_EQ(&(type->getContext()), &(state_ptr->getContext()));
  const auto state_ptr_type = llvm::dyn_cast<llvm::PointerType>(state_ptr->getType());
  CHECK_NOTNULL(state_ptr_type);
  const auto addr_space = state_ptr_type->getAddressSpace();

  const auto state_type = llvm::dyn_cast<llvm::StructType>(state_ptr_type->getPointerElementType());
  CHECK_NOTNULL(state_type);

  const auto module = add_to_end->getParent()->getParent();
  auto &context = module->getContext();
  llvm::DataLayout dl(module);

  const auto index_type = llvm::IntegerType::getInt32Ty(context);
  llvm::SmallVector<llvm::Value *, 8> indexes;
  indexes.push_back(llvm::ConstantInt::get(index_type, 0, false));

  llvm::Type *found_type = nullptr;
  size_t found_offset = 0;
  std::tie(found_offset, found_type) = BuildIndexes(
      dl, state_type, type, 0, offset, indexes);

  CHECK(found_type != nullptr)
      << "Unable to create index list for register '" << name << "'";

  llvm::IRBuilder<> ir(add_to_end);
  const auto gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
      ir.CreateInBoundsGEP(state_type, state_ptr, indexes));
  CHECK_NOTNULL(gep);

  llvm::APInt accumulated_offset(64, 0, false);
  CHECK(gep->accumulateConstantOffset(dl, accumulated_offset));
  CHECK_EQ(found_offset, accumulated_offset.getZExtValue());

  const auto goal_ptr_type = llvm::PointerType::get(type, addr_space);

  // Best case: we've found a value field in the structure that
  // is located at the correct byte offset.
  if (found_offset == offset) {
    if (found_type == type) {
      return gep;

    } else {
      return llvm::dyn_cast<llvm::Instruction>(ir.CreateBitCast(
          gep, goal_ptr_type));
    }
  }

  const auto diff = offset - found_offset;

  // Next best case: the difference between what we want and what we have
  // is a multiple of the size of the register, so we can cast to the
  // `goal_ptr_type` and index.
  if (((diff / size) * size) == diff) {
    llvm::Value *elem_indexes[] = {
        llvm::ConstantInt::get(index_type, diff / size, false)
    };

    const auto arr = ir.CreateBitCast(gep, goal_ptr_type);
    return llvm::dyn_cast<llvm::Instruction>(
         ir.CreateGEP(type, arr, elem_indexes));
  }

  // Worst case is that we have to fall down to byte-granularity
  // pointer arithmetic.
  const auto byte_type = llvm::IntegerType::getInt8Ty(context);
  const auto arr = ir.CreateBitCast(
      gep, llvm::PointerType::get(byte_type, addr_space));

  llvm::Value *elem_indexes[] = {
      llvm::ConstantInt::get(index_type, diff, false)
  };

  const auto byte_addr = ir.CreateGEP(byte_type, arr, elem_indexes);
  return llvm::dyn_cast<llvm::Instruction>(
      ir.CreateBitCast(byte_addr, goal_ptr_type));
}

// Converts an LLVM module object to have the right triple / data layout
// information for the target architecture.
//
void Arch::PrepareModuleDataLayout(llvm::Module *mod) const {
  mod->setDataLayout(DataLayout().getStringRepresentation());
  mod->setTargetTriple(Triple().str());

  // Go and remove compile-time attributes added into the semantics. These
  // can screw up later compilation. We purposefully compile semantics with
  // things like auto-vectorization disabled so that it keeps the bitcode
  // to a simpler subset of the available LLVM instuction set. If/when we
  // compile this bitcode back into machine code, we may want to use those
  // features, and clang will complain if we try to do so if these metadata
  // remain present.
  auto &context = mod->getContext();

  llvm::AttributeSet target_attribs;
  target_attribs = target_attribs.addAttribute(
      context,
      IF_LLVM_LT_50_(llvm::AttributeSet::FunctionIndex)
      "target-features");
  target_attribs = target_attribs.addAttribute(
      context,
      IF_LLVM_LT_50_(llvm::AttributeSet::FunctionIndex)
      "target-cpu");

  for (llvm::Function &func : *mod) {
    auto attribs = func.getAttributes();
    attribs = attribs.removeAttributes(
        context,
        llvm::AttributeLoc::FunctionIndex,
        target_attribs);
    func.setAttributes(attribs);
  }
}

void Arch::PrepareModule(llvm::Module *mod) const {
  CHECK_EQ(&(mod->getContext()), context);
  PrepareModuleRemillFunctions(mod);
  PrepareModuleDataLayout(mod);
  if (registers.empty()) {
    CollectRegisters(mod);
  }
}

// Get all of the register information from the prepared module.
void Arch::CollectRegisters(llvm::Module *module) const {
  llvm::DataLayout dl(module);
  auto basic_block = BasicBlockFunction(module);
  auto state_ptr_type = StatePointerType(module);
  std::vector<llvm::Instruction *> named_insts;
  uint64_t order = 0;

  // Collect all registers.
  for (auto &block : *basic_block) {
    for (auto &inst : block) {
      uint64_t offset = 0;
      if (!inst.hasName()) {
        continue;
      }
      auto ptr_type = llvm::dyn_cast<llvm::PointerType>(inst.getType());
      if (!ptr_type || ptr_type->getElementType()->isPointerTy()) {
        continue;
      }
      auto reg_type = ptr_type->getElementType();
      if (!GetRegisterOffset(module, state_ptr_type, &inst, &offset)) {
        continue;
      }
      auto name = inst.getName().str();
      registers.emplace_back(
          name, offset, dl.getTypeAllocSize(reg_type), order++, reg_type);
    }
  }

  // Sort them in such a way that we can recover the parentage of registers.
  std::sort(registers.begin(), registers.end(), RegisterComparator);

  auto num_bytes = dl.getTypeAllocSize(state_ptr_type->getElementType());
  reg_by_offset.resize(num_bytes);

  // Figure out parentage of registers, and fill in the various maps.
  for (auto &reg : registers) {
    reg_by_name[reg.name] = &reg;

    for (uint64_t i = 0; i < reg.size; ++i) {
      auto &reg_at_offset = reg_by_offset[reg.offset + i];
      if (!reg.parent) {
        reg.parent = reg_at_offset;
      } else if (!reg_at_offset) {
        LOG(FATAL)
            << "Register " << reg.name << " is not fully enclosed by parent "
            << reg.parent->name;
      } else if (reg.parent != reg_at_offset) {
        LOG(FATAL)
            << "Can't set parent of register " << reg.name
            << " to " << reg_at_offset->name << " because it already has "
            << reg.parent->name << " as its parent";
      }
      reg_at_offset = &reg;
    }
  }
}

}  // namespace remill
