//===- MLIREmitter.cpp - MLIR EDSC Emitter Class Implementation -*- C++ -*-===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "mlir-c/Core.h"
#include "mlir/AffineOps/AffineOps.h"
#include "mlir/Analysis/AffineAnalysis.h"
#include "mlir/EDSC/MLIREmitter.h"
#include "mlir/EDSC/Types.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Instruction.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Value.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/SuperVectorOps/SuperVectorOps.h"
#include "mlir/Support/Functional.h"
#include "mlir/Support/STLExtras.h"

using llvm::dbgs;
using llvm::errs;

#define DEBUG_TYPE "edsc"

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::detail;

// Factors out the boilerplate that is needed to build and answer the
// following simple question:
//   Given a set of Value* `values`, how do I get the resulting op(`values`)
//
// This is a very loaded question and generally cannot be answered properly.
// For instance, an LLVM operation has many attributes that may not fit within
// this simplistic framing (e.g. overflow behavior etc).
//
// Still, MLIR is a higher-level IR and the Halide experience shows it is
// possible to build useful EDSCs with the right amount of sugar.
//
// To build EDSCs we need to be able to conveniently support simple operations
// such as `add` on the type system. This captures the possible behaviors. In
// the future, this should be automatically constructed from an abstraction
// that is common to the IR verifier, but for now we need to get off the ground
// manually.
//
// This is expected to be a "dialect-specific" functionality: certain dialects
// will not have a simple definition. Two such cases that come to mind are:
//   1. what does it mean to have an operator* on an opaque tensor dialect
//      (dot, vector, hadamard, kronecker ?)-product;
//   2. LLVM add with attributes like overflow.
// This is all left for future consideration; in the meantime let's separate
// concerns and implement useful infrastructure without solving all problems at
// once.

/// Returns the element type if the type is VectorType or MemRefType; returns
/// getType if the type is scalar.
static Type getElementType(const Value &v) {
  if (auto vec = v.getType().dyn_cast<mlir::VectorType>()) {
    return vec.getElementType();
  }
  if (auto mem = v.getType().dyn_cast<mlir::MemRefType>()) {
    return mem.getElementType();
  }
  return v.getType();
}

static bool isIndexElement(const Value &v) {
  return getElementType(v).isIndex();
}
static bool isIntElement(const Value &v) {
  return getElementType(v).isa<IntegerType>();
}
static bool isFloatElement(const Value &v) {
  return getElementType(v).isa<FloatType>();
}

static Value *add(FuncBuilder *builder, Location location, Value *a, Value *b) {
  if (isIndexElement(*a)) {
    auto *context = builder->getContext();
    auto d0 = getAffineDimExpr(0, context);
    auto d1 = getAffineDimExpr(1, context);
    auto map = AffineMap::get(2, 0, {d0 + d1}, {});
    return makeComposedAffineApply(builder, location, map, {a, b});
  } else if (isIntElement(*a)) {
    return builder->create<AddIOp>(location, a, b)->getResult();
  }
  assert(isFloatElement(*a) && "Expected float element");
  return builder->create<AddFOp>(location, a, b)->getResult();
}

static Value *sub(FuncBuilder *builder, Location location, Value *a, Value *b) {
  if (isIndexElement(*a)) {
    auto *context = builder->getContext();
    auto d0 = getAffineDimExpr(0, context);
    auto d1 = getAffineDimExpr(1, context);
    auto map = AffineMap::get(2, 0, {d0 - d1}, {});
    return makeComposedAffineApply(builder, location, map, {a, b});
  } else if (isIntElement(*a)) {
    return builder->create<SubIOp>(location, a, b)->getResult();
  }
  assert(isFloatElement(*a) && "Expected float element");
  return builder->create<SubFOp>(location, a, b)->getResult();
}

static Value *mul(FuncBuilder *builder, Location location, Value *a, Value *b) {
  if (!isFloatElement(*a)) {
    return builder->create<MulIOp>(location, a, b)->getResult();
  }
  assert(isFloatElement(*a) && "Expected float element");
  return builder->create<MulFOp>(location, a, b)->getResult();
}

static void printDefininingStatement(llvm::raw_ostream &os, const Value &v) {
  const auto *inst = v.getDefiningInst();
  if (inst) {
    inst->print(os);
    return;
  }
  if (auto forInst = getForInductionVarOwner(&v)) {
    forInst->getInstruction()->print(os);
  } else if (auto *bbArg = dyn_cast<BlockArgument>(&v)) {
    os << "block_argument";
  } else {
    os << "unknown_ssa_value";
  }
}

mlir::edsc::MLIREmitter::MLIREmitter(FuncBuilder *builder, Location location)
    : builder(builder), location(location), zeroIndex(builder->getIndexType()),
      oneIndex(builder->getIndexType()) {
  // Build the ubiquitous zero and one at the top of the function.
  bindConstant<ConstantIndexOp>(Bindable(zeroIndex), 0);
  bindConstant<ConstantIndexOp>(Bindable(oneIndex), 1);
}

MLIREmitter &mlir::edsc::MLIREmitter::bind(Bindable e, Value *v) {
  LLVM_DEBUG(printDefininingStatement(llvm::dbgs() << "\nBinding " << e << " @"
                                                   << e.getStoragePtr() << ": ",
                                      *v));
  auto it = ssaBindings.insert(std::make_pair(e, v));
  if (!it.second) {
    printDefininingStatement(llvm::errs() << "\nRebinding " << e << " @"
                                          << e.getStoragePtr() << " ",
                             *v);
    llvm_unreachable("Double binding!");
  }
  return *this;
}

Value *mlir::edsc::MLIREmitter::emitExpr(Expr e) {
  // It is still necessary in case we try to emit a bindable directly
  // FIXME: make sure isa<Bindable> works and use it below to delegate emission
  // to Expr::build and remove this, now duplicate, check.
  auto it = ssaBindings.find(e);
  if (it != ssaBindings.end()) {
    return it->second;
  }

  Value *res = nullptr;
  bool expectedEmpty = false;
  if (e.isa<UnaryExpr>() || e.isa<BinaryExpr>() || e.isa<TernaryExpr>() ||
      e.isa<VariadicExpr>()) {
    auto results = e.build(*builder, ssaBindings);
    assert(results.size() <= 1 && "2+-result exprs are not supported");
    expectedEmpty = results.empty();
    if (!results.empty())
      res = results.front();
  }

  if (auto expr = e.dyn_cast<StmtBlockLikeExpr>()) {
    if (expr.getKind() == ExprKind::For) {
      auto exprs = emitExprs(expr.getExprs());
      if (llvm::any_of(exprs, [](Value *v) { return !v; })) {
        return nullptr;
      }
      assert(exprs.size() == 3 && "Expected 3 exprs");
      auto *lb = exprs[0];
      auto *ub = exprs[1];

      // There may be no defining instruction if the value is a function
      // argument.  We accept such values.
      auto *lbDef = lb->getDefiningInst();
      (void)lbDef;
      assert((!lbDef || lbDef->isa<ConstantIndexOp>() ||
              lbDef->isa<AffineApplyOp>() || lbDef->isa<AffineForOp>()) &&
             "lower bound expression does not have affine provenance");
      auto *ubDef = ub->getDefiningInst();
      (void)ubDef;
      assert((!ubDef || ubDef->isa<ConstantIndexOp>() ||
              ubDef->isa<AffineApplyOp>() || ubDef->isa<AffineForOp>()) &&
             "upper bound expression does not have affine provenance");

      // Step must be a static constant.
      auto step =
          exprs[2]->getDefiningInst()->cast<ConstantIndexOp>()->getValue();

      // Special case with more concise emitted code for static bounds.
      OpPointer<AffineForOp> forOp;
      if (lbDef && ubDef)
        if (auto lbConst = lbDef->dyn_cast<ConstantIndexOp>())
          if (auto ubConst = ubDef->dyn_cast<ConstantIndexOp>())
            forOp = builder->create<AffineForOp>(location, lbConst->getValue(),
                                                 ubConst->getValue(), step);

      // General case.
      if (!forOp) {
        auto map = builder->getDimIdentityMap();
        forOp =
            builder->create<AffineForOp>(location, llvm::makeArrayRef(lb), map,
                                         llvm::makeArrayRef(ub), map, step);
      }
      forOp->createBody();
      res = forOp->getInductionVar();
    }
  }

  if (!res && !expectedEmpty) {
    // If we hit here it must mean that the Bindables have not all been bound
    // properly. Because EDSCs are currently dynamically typed, it becomes a
    // runtime error.
    e.print(llvm::errs() << "\nError @" << e.getStoragePtr() << ": ");
    auto it = ssaBindings.find(e);
    if (it != ssaBindings.end()) {
      it->second->print(llvm::errs() << "\nError on value: ");
    } else {
      llvm::errs() << "\nUnbound";
    }
    return nullptr;
  }

  auto resIter = ssaBindings.insert(std::make_pair(e, res));
  (void)resIter;
  assert(resIter.second && "insertion failed");
  return res;
}

SmallVector<Value *, 8>
mlir::edsc::MLIREmitter::emitExprs(ArrayRef<Expr> exprs) {
  SmallVector<Value *, 8> res;
  res.reserve(exprs.size());
  for (auto e : exprs) {
    res.push_back(this->emitExpr(e));
    LLVM_DEBUG(
        printDefininingStatement(llvm::dbgs() << "\nEmitted: ", *res.back()));
  }
  return res;
}

void mlir::edsc::MLIREmitter::emitStmt(const Stmt &stmt) {
  auto *block = builder->getBlock();
  auto ip = builder->getInsertionPoint();
  auto *val = emitExpr(stmt.getRHS());
  if (!val) {
    assert((stmt.getRHS().getName() == DeallocOp::getOperationName() ||
            stmt.getRHS().getName() == StoreOp::getOperationName() ||
            stmt.getRHS().getName() == ReturnOp::getOperationName()) &&
           "dealloc, store or return expected as the only 0-result ops");
    return;
  }
  // Force create a bindable from stmt.lhs and bind it.
  bind(Bindable(stmt.getLHS()), val);
  if (stmt.getRHS().getKind() == ExprKind::For) {
    // Step into the loop.
    builder->setInsertionPointToStart(getForInductionVarOwner(val)->getBody());
  }
  emitStmts(stmt.getEnclosedStmts());
  builder->setInsertionPoint(block, ip);
}

void mlir::edsc::MLIREmitter::emitStmts(ArrayRef<Stmt> stmts) {
  for (auto &stmt : stmts) {
    emitStmt(stmt);
  }
}

mlir::edsc::MLIREmitter &
mlir::edsc::MLIREmitter::emitBlock(const StmtBlock &block) {
  // If we have already emitted this block, do nothing.
  if (blockBindings.count(block) != 0)
    return *this;

  // Otherwise, save the current insertion point.
  auto previousBlock = builder->getInsertionBlock();
  auto previousInstr = builder->getInsertionPoint();

  // Create a new IR block and emit the enclosed statements in that block.  Bind
  // the block argument expressions to the arguments of the emitted IR block.
  auto irBlock = builder->createBlock();
  blockBindings.insert({block, irBlock});
  for (const auto &kvp :
       llvm::zip(block.getArguments(), block.getArgumentTypes())) {
    Bindable expr = std::get<0>(kvp);
    assert(expr.getKind() == ExprKind::Unbound &&
           "cannot use bound expressions as block arguments");
    Type type = std::get<1>(kvp);
    bind(expr, irBlock->addArgument(type));
  }
  emitStmts(block.getBody());

  // And finally restore the original insertion point.
  builder->setInsertionPoint(previousBlock, previousInstr);
  return *this;
}

static bool isDynamicSize(int size) { return size < 0; }

/// This function emits the proper Value* at the place of insertion of b,
/// where each value is the proper ConstantOp or DimOp. Returns a vector with
/// these Value*. Note this function does not concern itself with hoisting of
/// constants and will produce redundant IR. Subsequent MLIR simplification
/// passes like LICM and CSE are expected to clean this up.
///
/// More specifically, a MemRefType has a shape vector in which:
///   - constant ranks are embedded explicitly with their value;
///   - symbolic ranks are represented implicitly by -1 and need to be recovered
///     with a DimOp operation.
///
/// Example:
/// When called on:
///
/// ```mlir
///    memref<?x3x4x?x5xf32>
/// ```
///
/// This emits MLIR similar to:
///
/// ```mlir
///    %d0 = dim %0, 0 : memref<?x3x4x?x5xf32>
///    %c3 = constant 3 : index
///    %c4 = constant 4 : index
///    %d3 = dim %0, 3 : memref<?x3x4x?x5xf32>
///    %c5 = constant 5 : index
/// ```
///
/// and returns the vector with {%d0, %c3, %c4, %d3, %c5}.
static SmallVector<Value *, 8> getMemRefSizes(FuncBuilder *b, Location loc,
                                              Value *memRef) {
  assert(memRef->getType().isa<MemRefType>() && "Expected a MemRef value");
  MemRefType memRefType = memRef->getType().cast<MemRefType>();
  SmallVector<Value *, 8> res;
  res.reserve(memRefType.getShape().size());
  const auto &shape = memRefType.getShape();
  for (unsigned idx = 0, n = shape.size(); idx < n; ++idx) {
    if (isDynamicSize(shape[idx])) {
      res.push_back(b->create<DimOp>(loc, memRef, idx));
    } else {
      res.push_back(b->create<ConstantIndexOp>(loc, shape[idx]));
    }
  }
  return res;
}

SmallVector<edsc::Expr, 8>
mlir::edsc::MLIREmitter::makeBoundFunctionArguments(mlir::Function *function) {
  SmallVector<edsc::Expr, 8> res;
  for (unsigned pos = 0, npos = function->getNumArguments(); pos < npos;
       ++pos) {
    auto *arg = function->getArgument(pos);
    Expr b(arg->getType());
    bind(Bindable(b), arg);
    res.push_back(Expr(b));
  }
  return res;
}

SmallVector<edsc::Expr, 8>
mlir::edsc::MLIREmitter::makeBoundMemRefShape(Value *memRef) {
  assert(memRef->getType().isa<MemRefType>() && "Expected a MemRef value");
  MemRefType memRefType = memRef->getType().cast<MemRefType>();
  auto memRefSizes =
      edsc::makeNewExprs(memRefType.getShape().size(), builder->getIndexType());
  auto memrefSizeValues = getMemRefSizes(getBuilder(), getLocation(), memRef);
  assert(memrefSizeValues.size() == memRefSizes.size());
  bindZipRange(llvm::zip(memRefSizes, memrefSizeValues));
  SmallVector<edsc::Expr, 8> res(memRefSizes.begin(), memRefSizes.end());
  return res;
}

mlir::edsc::MLIREmitter::BoundMemRefView
mlir::edsc::MLIREmitter::makeBoundMemRefView(Value *memRef) {
  auto memRefType = memRef->getType().cast<mlir::MemRefType>();
  auto rank = memRefType.getRank();

  SmallVector<edsc::Expr, 8> lbs;
  lbs.reserve(rank);
  Expr zero(builder->getIndexType());
  bindConstant<mlir::ConstantIndexOp>(Bindable(zero), 0);
  for (unsigned i = 0; i < rank; ++i) {
    lbs.push_back(zero);
  }

  auto ubs = makeBoundMemRefShape(memRef);

  SmallVector<edsc::Expr, 8> steps;
  lbs.reserve(rank);
  Expr one(builder->getIndexType());
  bindConstant<mlir::ConstantIndexOp>(Bindable(one), 1);
  for (unsigned i = 0; i < rank; ++i) {
    steps.push_back(one);
  }

  return BoundMemRefView{lbs, ubs, steps};
}

mlir::edsc::MLIREmitter::BoundMemRefView
mlir::edsc::MLIREmitter::makeBoundMemRefView(Expr boundMemRef) {
  auto *v = getValue(mlir::edsc::Expr(boundMemRef));
  assert(v && "Expected a bound Expr");
  return makeBoundMemRefView(v);
}

edsc_expr_t bindConstantBF16(edsc_mlir_emitter_t emitter, double value) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  Expr b(e->getBuilder()->getBF16Type());
  e->bindConstant<mlir::ConstantFloatOp>(Bindable(b), mlir::APFloat(value),
                                         e->getBuilder()->getBF16Type());
  return b;
}

edsc_expr_t bindConstantF16(edsc_mlir_emitter_t emitter, float value) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  Expr b(e->getBuilder()->getBF16Type());
  bool unused;
  mlir::APFloat val(value);
  val.convert(e->getBuilder()->getF16Type().getFloatSemantics(),
              mlir::APFloat::rmNearestTiesToEven, &unused);
  e->bindConstant<mlir::ConstantFloatOp>(Bindable(b), val,
                                         e->getBuilder()->getF16Type());
  return b;
}

edsc_expr_t bindConstantF32(edsc_mlir_emitter_t emitter, float value) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  Expr b(e->getBuilder()->getF32Type());
  e->bindConstant<mlir::ConstantFloatOp>(Bindable(b), mlir::APFloat(value),
                                         e->getBuilder()->getF32Type());
  return b;
}

edsc_expr_t bindConstantF64(edsc_mlir_emitter_t emitter, double value) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  Expr b(e->getBuilder()->getF64Type());
  e->bindConstant<mlir::ConstantFloatOp>(Bindable(b), mlir::APFloat(value),
                                         e->getBuilder()->getF64Type());
  return b;
}

edsc_expr_t bindConstantInt(edsc_mlir_emitter_t emitter, int64_t value,
                            unsigned bitwidth) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  Expr b(e->getBuilder()->getIntegerType(bitwidth));
  e->bindConstant<mlir::ConstantIntOp>(
      b, value, e->getBuilder()->getIntegerType(bitwidth));
  return b;
}

edsc_expr_t bindConstantIndex(edsc_mlir_emitter_t emitter, int64_t value) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  Expr b(e->getBuilder()->getIndexType());
  e->bindConstant<mlir::ConstantIndexOp>(Bindable(b), value);
  return b;
}

unsigned getRankOfFunctionArgument(mlir_func_t function, unsigned pos) {
  auto *f = reinterpret_cast<mlir::Function *>(function);
  assert(pos < f->getNumArguments());
  auto *arg = *(f->getArguments().begin() + pos);
  if (auto memRefType = arg->getType().dyn_cast<mlir::MemRefType>()) {
    return memRefType.getRank();
  }
  return 0;
}

mlir_type_t getTypeOfFunctionArgument(mlir_func_t function, unsigned pos) {
  auto *f = reinterpret_cast<mlir::Function *>(function);
  assert(pos < f->getNumArguments());
  auto *arg = *(f->getArguments().begin() + pos);
  return mlir_type_t{arg->getType().getAsOpaquePointer()};
}

edsc_expr_t bindFunctionArgument(edsc_mlir_emitter_t emitter,
                                 mlir_func_t function, unsigned pos) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  auto *f = reinterpret_cast<mlir::Function *>(function);
  assert(pos < f->getNumArguments());
  auto *arg = *(f->getArguments().begin() + pos);
  Expr b(arg->getType());
  e->bind(Bindable(b), arg);
  return Expr(b);
}

void bindFunctionArguments(edsc_mlir_emitter_t emitter, mlir_func_t function,
                           edsc_expr_list_t *result) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  auto *f = reinterpret_cast<mlir::Function *>(function);
  assert(result->n == f->getNumArguments());
  for (unsigned pos = 0; pos < result->n; ++pos) {
    auto *arg = *(f->getArguments().begin() + pos);
    Expr b(arg->getType());
    e->bind(Bindable(b), arg);
    result->exprs[pos] = Expr(b);
  }
}

unsigned getBoundMemRefRank(edsc_mlir_emitter_t emitter,
                            edsc_expr_t boundMemRef) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  auto *v = e->getValue(mlir::edsc::Expr(boundMemRef));
  assert(v && "Expected a bound Expr");
  auto memRefType = v->getType().cast<mlir::MemRefType>();
  return memRefType.getRank();
}

void bindMemRefShape(edsc_mlir_emitter_t emitter, edsc_expr_t boundMemRef,
                     edsc_expr_list_t *result) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  auto *v = e->getValue(mlir::edsc::Expr(boundMemRef));
  assert(v && "Expected a bound Expr");
  auto memRefType = v->getType().cast<mlir::MemRefType>();
  auto rank = memRefType.getRank();
  assert(result->n == rank && "Unexpected memref shape binding results count");
  auto bindables = e->makeBoundMemRefShape(v);
  for (unsigned i = 0; i < rank; ++i) {
    result->exprs[i] = bindables[i];
  }
}

void bindMemRefView(edsc_mlir_emitter_t emitter, edsc_expr_t boundMemRef,
                    edsc_expr_list_t *resultLbs, edsc_expr_list_t *resultUbs,
                    edsc_expr_list_t *resultSteps) {
  auto *e = reinterpret_cast<mlir::edsc::MLIREmitter *>(emitter);
  auto *v = e->getValue(mlir::edsc::Expr(boundMemRef));
  auto memRefType = v->getType().cast<mlir::MemRefType>();
  auto rank = memRefType.getRank();
  assert(resultLbs->n == rank && "Unexpected memref binding results count");
  assert(resultUbs->n == rank && "Unexpected memref binding results count");
  assert(resultSteps->n == rank && "Unexpected memref binding results count");
  auto bindables = e->makeBoundMemRefShape(v);
  Expr zero(e->getBuilder()->getIndexType());
  e->bindConstant<mlir::ConstantIndexOp>(zero, 0);
  Expr one(e->getBuilder()->getIndexType());
  e->bindConstant<mlir::ConstantIndexOp>(one, 1);
  for (unsigned i = 0; i < rank; ++i) {
    resultLbs->exprs[i] = zero;
    resultUbs->exprs[i] = bindables[i];
    resultSteps->exprs[i] = one;
  }
}

#define DEFINE_EDSL_BINARY_OP(FUN_NAME, OP_SYMBOL)                             \
  edsc_expr_t FUN_NAME(edsc_expr_t e1, edsc_expr_t e2) {                       \
    using edsc::op::operator OP_SYMBOL;                                        \
    return Expr(e1) OP_SYMBOL Expr(e2);                                        \
  }

DEFINE_EDSL_BINARY_OP(Add, +);
DEFINE_EDSL_BINARY_OP(Sub, -);
DEFINE_EDSL_BINARY_OP(Mul, *);
// DEFINE_EDSL_BINARY_OP(Div, /);
DEFINE_EDSL_BINARY_OP(LT, <);
DEFINE_EDSL_BINARY_OP(LE, <=);
DEFINE_EDSL_BINARY_OP(GT, >);
DEFINE_EDSL_BINARY_OP(GE, >=);
DEFINE_EDSL_BINARY_OP(EQ, ==);
DEFINE_EDSL_BINARY_OP(NE, !=);
DEFINE_EDSL_BINARY_OP(And, &&);
DEFINE_EDSL_BINARY_OP(Or, ||);

#undef DEFINE_EDSL_BINARY_OP

#define DEFINE_EDSL_UNARY_OP(FUN_NAME, OP_SYMBOL)                              \
  edsc_expr_t FUN_NAME(edsc_expr_t e) {                                        \
    using edsc::op::operator OP_SYMBOL;                                        \
    return (OP_SYMBOL(Expr(e)));                                               \
  }

DEFINE_EDSL_UNARY_OP(Negate, !);

#undef DEFINE_EDSL_UNARY_OP
