/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file implements logic for lowering XLA dialect to Standard dialect.

#include "llvm/ADT/StringSwitch.h"
#include "mlir/Dialect/StandardOps/Ops.h"  // TF:local_config_mlir
#include "mlir/IR/Block.h"  // TF:local_config_mlir
#include "mlir/IR/BlockAndValueMapping.h"  // TF:local_config_mlir
#include "mlir/IR/Builders.h"  // TF:local_config_mlir
#include "mlir/IR/Function.h"  // TF:local_config_mlir
#include "mlir/IR/PatternMatch.h"  // TF:local_config_mlir
#include "mlir/IR/StandardTypes.h"  // TF:local_config_mlir
#include "mlir/IR/TypeUtilities.h"  // TF:local_config_mlir
#include "mlir/Pass/Pass.h"  // TF:local_config_mlir
#include "mlir/Pass/PassRegistry.h"  // TF:local_config_mlir
#include "mlir/Support/LogicalResult.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.h"
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"

using mlir::PassRegistration;

namespace mlir {
namespace xla_hlo {
namespace {
struct LegalizeControlFlow : public mlir::FunctionPass<LegalizeControlFlow> {
  // Perform the lowering to MLIR control flow.
  void runOnFunction() override;
};

// Replaces terminators for the newly created blocks from a targe region.
// These terminators are replaced with branch operations to a target block.
LogicalResult ReplaceTerminators(Region* region, Block* target_block,
                                 Location loc,
                                 const BlockAndValueMapping& mapper,
                                 OpBuilder* builder) {
  for (auto& old_block : region->getBlocks()) {
    Block* block = mapper.lookup(&old_block);
    auto return_op = dyn_cast<xla_hlo::ReturnOp>(block->getTerminator());
    if (!return_op) return failure();
    builder->setInsertionPointToEnd(block);

    SmallVector<Value*, 4> args(return_op.getOperands());
    builder->create<mlir::BranchOp>(loc, target_block, args);
    return_op.getOperation()->erase();
  }

  return success();
}

LogicalResult LowerConditionalOp(mlir::xla_hlo::ConditionalOp conditional_op) {
  Operation* op_inst = conditional_op.getOperation();
  mlir::OpBuilder builder(conditional_op);
  auto orig_block = op_inst->getBlock();
  auto* tail_block = orig_block->splitBlock(op_inst);
  auto loc = conditional_op.getLoc();

  // Duplicate the true and false regions in the block between the sections
  // before and after the while loop.
  BlockAndValueMapping mapper;
  conditional_op.true_branch().cloneInto(orig_block->getParent(),
                                         Region::iterator(tail_block), mapper);
  conditional_op.false_branch().cloneInto(orig_block->getParent(),
                                          Region::iterator(tail_block), mapper);

  Block* true_block = mapper.lookup(&conditional_op.true_branch().front());
  Block* false_block = mapper.lookup(&conditional_op.false_branch().front());

  // Perform the conditional branch into the true/false cases.
  builder.setInsertionPointToEnd(orig_block);

  // Extract the predicate for checking branching, then branch to the true and
  // false blocks appropriately.
  auto cond_value =
      builder.create<mlir::ExtractElementOp>(loc, conditional_op.pred());
  builder.create<mlir::CondBranchOp>(loc, cond_value, true_block,
                                     conditional_op.true_arg(), false_block,
                                     conditional_op.false_arg());

  // Replace the true case's return operations with a branche to the tail of
  // the condition.
  if (failed(ReplaceTerminators(&conditional_op.true_branch(), tail_block, loc,
                                mapper, &builder)))
    return failure();
  if (failed(ReplaceTerminators(&conditional_op.false_branch(), tail_block, loc,
                                mapper, &builder)))
    return failure();

  tail_block->addArguments(conditional_op.getResult()->getType());
  conditional_op.getResult()->replaceAllUsesWith(tail_block->getArgument(0));

  op_inst->erase();
  return success();
}

LogicalResult LowerWhileOp(mlir::xla_hlo::WhileOp while_op) {
  // Converts an xla while loop into control flow. This mostly generates the
  // right MLIR boilerplate for calling the body / condition functions, then
  // branching on their results appropriately. The operation should look similar
  // to below:
  //
  //   <prior operations>
  //   %0 = "xla_hlo.while"(%arg0) {body: @loop, cond: @cond}
  //   <post operations>
  auto* op_inst = while_op.getOperation();
  mlir::OpBuilder builder(while_op);
  auto loc = while_op.getLoc();

  // Break the block into four sections:
  // orig_block - operations before the while and the branch into looping check.
  // tail_block - operations after the while loop completes.
  // cond_block - check the looping condition, then conditionally branch into
  //              the loop or, if condition is false, jump to the tail branch.
  // body_block - call the loop body, then jump back to the condition block.
  auto* orig_block = op_inst->getBlock();
  auto* tail_block = orig_block->splitBlock(op_inst);

  BlockAndValueMapping mapper;
  while_op.cond().cloneInto(orig_block->getParent(),
                            Region::iterator(tail_block), mapper);
  while_op.body().cloneInto(orig_block->getParent(),
                            Region::iterator(tail_block), mapper);

  // Lookup the entry blocks for both condition and body.
  auto* cond_block = mapper.lookup(&while_op.cond().front());
  auto* body_block = mapper.lookup(&while_op.body().front());

  // Setup the end of the original block:
  //     <prior operations>
  //     br ^cond(%arg0) // Jumps to the condition statement.
  builder.setInsertionPointToEnd(orig_block);
  builder.create<mlir::BranchOp>(loc, cond_block, while_op.getOperand());

  // Updates the condition blocks by replacing the return op with an
  // extract_element and conditional branch. This changes the block below:
  //   ^cond(%0):
  //     %1 = <some operations> -> tensor<i1> // Helper condition function.
  //    "xla_hlo".return(%1)
  //
  //  Into:
  //   ^cond(%0):
  //     %1 = <some operations> -> tensor<i1> // Helper condition function
  //     %2 = extract_element %1[] : tensor<i1> // Extract the condition value.
  //     cond_br %2, ^body(%0), ^tail(%0) // Branch.
  builder.setInsertionPointToStart(cond_block);

  // Replace the xla_hlo::ReturnOp with a call back to the condition block.
  // This is required as the xla_hlo::ReturnOp is used to mark the end of a
  // block for regions nested inside of a operations (MLIR ReturnOp cannot be
  // nested within an non-function region).
  for (auto& block : while_op.cond()) {
    auto new_block = mapper.lookup(&block);

    auto return_op = dyn_cast<xla_hlo::ReturnOp>(new_block->getTerminator());
    if (!return_op) return failure();
    builder.setInsertionPointToEnd(new_block);

    auto return_value = return_op.getOperand(0);
    auto cond_value = builder.create<mlir::ExtractElementOp>(loc, return_value);

    // Get the body block arguments.
    llvm::SmallVector<Value*, 4> body_block_arguments(cond_block->args_begin(),
                                                      cond_block->args_end());

    builder.create<mlir::CondBranchOp>(loc, cond_value, body_block,
                                       body_block_arguments, tail_block,
                                       body_block_arguments);

    return_op.getOperation()->erase();
  }

  // Updates the body blocks by replace the return op with an branch to the
  // conditional block. This changes the block below:
  //   ^body(%0):
  //     %1 = call @body(%0) : (...) -> tensor<i1> // Helper body function.
  //    "xla_hlo".return(%1)
  //
  //  Into:
  //   ^body(%0):
  //     %1 = call @body(%0) : (...) -> tensor<i1> // Helper body function.
  //     br ^cond(%0) // Branch.
  for (auto& block : while_op.body()) {
    auto new_block = mapper.lookup(&block);
    builder.setInsertionPointToEnd(new_block);
    auto return_op =
        dyn_cast<mlir::xla_hlo::ReturnOp>(new_block->getTerminator());
    if (!return_op) return failure();

    llvm::SmallVector<Value*, 4> body_results(return_op.operand_begin(),
                                              return_op.operand_end());
    builder.create<mlir::BranchOp>(loc, cond_block, body_results);
    return_op.getOperation()->erase();
  }

  // Erase the original while loop.
  tail_block->addArgument(while_op.getType());
  while_op.getResult()->replaceAllUsesWith(tail_block->getArgument(0));
  op_inst->erase();

  return success();
}

void LegalizeControlFlow::runOnFunction() {
  auto func = getFunction();
  llvm::SmallVector<ConditionalOp, 4> conditional_ops;
  func.walk([&](ConditionalOp op) { conditional_ops.push_back(op); });

  for (auto& op : conditional_ops) {
    if (failed(LowerConditionalOp(op))) return signalPassFailure();
  }

  llvm::SmallVector<WhileOp, 4> while_ops;
  func.walk([&](WhileOp op) { while_ops.push_back(op); });

  for (auto& op : while_ops) {
    if (failed(LowerWhileOp(op))) return signalPassFailure();
  }
}
}  // namespace
}  // namespace xla_hlo
}  // namespace mlir

std::unique_ptr<mlir::OpPassBase<mlir::FuncOp>>
mlir::xla_hlo::createLegalizeControlFlowPass() {
  return std::make_unique<LegalizeControlFlow>();
}

static PassRegistration<mlir::xla_hlo::LegalizeControlFlow> legalize_cf_pass(
    "xla-legalize-control-flow",
    "Legalize from XLA control flow to MLIR control flow");
