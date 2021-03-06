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

#include "tensorflow/compiler/mlir/tensorflow/analysis/side_effect_analysis.h"

#include <cstdint>
#include <initializer_list>

#include "absl/strings/str_cat.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Block.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/Module.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_device.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_executor.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"
#include "tensorflow/compiler/tf2xla/resource_operation_table.h"
#include "tensorflow/core/framework/resource_mgr.h"

namespace mlir {
namespace TF {
namespace {

constexpr auto kUnknownResourceId =
    ResourceAliasAnalysis::Info::kUnknownResourceId;

//===----------------------------------------------------------------------===//
// SideEffectAnalysisInfo helper functions.
//===----------------------------------------------------------------------===//

// Returns a set that contains only kUnknownResourceId.
llvm::SmallDenseSet<int64_t, 8> UnknownResourceSet() {
  llvm::SmallDenseSet<int64_t, 8> unknown_set;
  unknown_set.insert(kUnknownResourceId);
  return unknown_set;
}

// Returns all resources that could be accessed by op, or UnknownResourceSet()
// if we cannot find all of them.
llvm::SmallDenseSet<int64_t, 8> FindAccessedResources(
    Operation* op, const ResourceAliasAnalysis::Info& alias_analysis) {
  llvm::SmallDenseSet<int64_t, 8> resources;

  for (auto operand : filter_resources(op->getOperands())) {
    if (alias_analysis.IsUnknownResource(operand)) return UnknownResourceSet();
    const auto& ids = alias_analysis.GetResourceUniqueIds(operand);
    resources.insert(ids.begin(), ids.end());
  }
  for (auto result : filter_resources(op->getResults())) {
    if (alias_analysis.IsUnknownResource(result)) return UnknownResourceSet();
    const auto& ids = alias_analysis.GetResourceUniqueIds(result);
    resources.insert(ids.begin(), ids.end());
  }
  return resources;
}

// Returns an XlaResourceOpInfo (or nullptr if it does not exist) that specifies
// the resource access type of the op. It tells whether the op is read only,
// etc.
//
// TODO(yuanzx): Define this information in a different place. Currently we use
// tensorflow/compiler/tf2xla/resource_operation_table.h.
const tensorflow::XlaResourceOpInfo* GetResourceInfoForOp(Operation* op) {
  if (op->getName().getDialect() !=
      TF::TensorFlowDialect::getDialectNamespace()) {
    return nullptr;
  }
  return tensorflow::GetResourceOpInfoForOp(
      op->getName().getStringRef().split('.').second.str());
}

// Returns whether `op` accesses resources and it is known to be read-only.
bool OpIsReadOnly(Operation* op) {
  auto resource_op_info = GetResourceInfoForOp(op);
  return resource_op_info &&
         resource_op_info->kind() == tensorflow::XlaResourceOpKind::kRead;
}

// Returns if `op` is a resource declaration.
bool OpIsDeclaration(Operation* op,
                     const ResourceAliasAnalysis::Info& alias_analysis) {
  // TODO(yuanzx): Add other types of resources.
  return llvm::isa<TF::VarHandleOp>(op) ||
         (llvm::isa<TF::IdentityNOp, TF::IdentityOp>(op) &&
          !FindAccessedResources(op, alias_analysis).empty());
}

// Returns if `op` is know to not have any side effect.
bool OpIsKnownToHaveNoSideEffect(Operation* op) {
  // Note: Identity op is really side-effect free, but it is not marked as such
  // in the TF dialect (see comments in definition of Identity op in tf_ops.td)
  // However, for adding control dependencies, its safe to assume
  // that the Identity op is side-effect free.
  if (isa<IdentityOp>(op)) return true;

  // For op's in the Tensorflow dialect, query the dialect.
  if (op->getName().getDialect() ==
      TF::TensorFlowDialect::getDialectNamespace())
    return !TensorFlowDialect::CanHaveSideEffects(op);

  // Otherwise, conservatively assume that there can be side effects.
  return false;
}

}  // namespace

namespace detail {
//===----------------------------------------------------------------------===//
// SideEffectAnalysisInfo
//===----------------------------------------------------------------------===//

void SideEffectAnalysisInfo::TrackAccess(int64_t resource_id, Operation* op,
                                         bool read_only) {
  if (resource_id == kUnknownResourceId) {
    if (read_only) {
      // New unknown read is not tracked by any known resource access.
      for (auto& entry : per_resource_access_info_) {
        entry.getSecond().tracked_last_unknown_read = false;
      }
    } else {
      // Unknown write can clear all other tracked information, since it acts
      // like a barrier.
      per_resource_access_info_.clear();
    }
  }
  auto& info = per_resource_access_info_[resource_id];
  if (read_only) {
    info.reads_since_last_write.push_back(op);
    // Resource read must have carried control dependencies of unknown write. It
    // can only avoid adding control edges (from uknown accesses) for a later
    // write, but not for a later read, because this read can be reordered with
    // a later read.
    info.tracked_last_unknown_write_for_write = true;
  } else {
    // Resource write must have carried control dependencies of unknown access.
    info.tracked_last_unknown_write_for_read = true;
    info.tracked_last_unknown_write_for_write = true;
    info.tracked_last_unknown_read = true;
    info.last_write = op;
    info.reads_since_last_write.clear();
  }
}

void SideEffectAnalysisInfo::AddPredecessorsForAccess(int64_t resource_id,
                                                      Operation* op,
                                                      bool read_only) {
  auto it = per_resource_access_info_.find(resource_id);
  if (it == per_resource_access_info_.end()) return;
  const auto& access_info = it->getSecond();
  auto& control_predecessors = control_predecessors_[op];
  bool read_tracked = false;
  if (!read_only) {
    control_predecessors.insert(access_info.reads_since_last_write.begin(),
                                access_info.reads_since_last_write.end());
    read_tracked = !access_info.reads_since_last_write.empty();
  }
  if (access_info.last_write && !read_tracked) {
    control_predecessors.insert(access_info.last_write);
  }
}

void SideEffectAnalysisInfo::AnalyzeFunction(
    FuncOp func_op, const TF::ResourceAliasAnalysis::Info& alias_analysis) {
  // AnalyzeRegion() recursively analyzes the function body, and only populates
  // control_predecessors_.
  AnalyzeRegion(&func_op.getBody(), alias_analysis);
  // Populate sorted_control_predecessors_ and sorted_control_successors_ based
  // on control_predecessors.
  for (auto& entry : control_predecessors_) {
    auto op = entry.getFirst();
    auto& sorted_predecessors = sorted_control_predecessors_[op];
    for (auto predecessor : entry.getSecond()) {
      sorted_predecessors.push_back(predecessor);
      sorted_control_successors_[predecessor].push_back(op);
    }
  }
  control_predecessors_.clear();
  for (auto& entry : sorted_control_predecessors_) {
    llvm::sort(entry.getSecond(), [](Operation* a, Operation* b) {
      return a->isBeforeInBlock(b);
    });
  }
  for (auto& entry : sorted_control_successors_) {
    llvm::sort(entry.getSecond(), [](Operation* a, Operation* b) {
      return a->isBeforeInBlock(b);
    });
  }
}

void SideEffectAnalysisInfo::AnalyzeRegion(
    Region* region, const TF::ResourceAliasAnalysis::Info& alias_analysis) {
  // This function populates control_predecessors_ by walking through the
  // region, and tracking resource accesses in per_resource_access_info_.

  // Returns whether an access to `resource` can skip control edges from
  // previous accesses to unknown resources, due to that earlier accesses to
  // `resource` already indirectly tracked previous accesses to unknown
  // resources. `read_only` specifies the type of access of the current op being
  // considered.
  auto unknown_access_indirectly_tracked_by_resource = [&](int64_t resource,
                                                           bool read_only) {
    auto it = per_resource_access_info_.find(resource);
    if (it == per_resource_access_info_.end()) return false;
    auto unknown_it = per_resource_access_info_.find(kUnknownResourceId);
    const bool no_unknown_read =
        unknown_it == per_resource_access_info_.end() ||
        unknown_it->getSecond().reads_since_last_write.empty();
    return read_only
               ? it->second.tracked_last_unknown_write_for_read
               : it->second.tracked_last_unknown_write_for_write &&
                     (it->second.tracked_last_unknown_read || no_unknown_read);
  };

  // We explicitly iterates through the regions and blocks, in order to handle
  // different nested regions separately.
  for (auto& block : *region) {
    for (auto& op : block) {
      for (Region& child : op.getRegions()) {
        SideEffectAnalysisInfo child_analysis(&child, alias_analysis);
        // Moves the control_predecessors_ fields in child region to current
        // region
        for (auto& entry : child_analysis.control_predecessors_)
          control_predecessors_[entry.first] = std::move(entry.second);
      }

      // We do not need explicit control edges for declaration ops.
      if (OpIsDeclaration(&op, alias_analysis)) continue;

      auto resource_op_info = GetResourceInfoForOp(&op);
      if (!resource_op_info && OpIsKnownToHaveNoSideEffect(&op)) continue;

      llvm::SmallDenseSet<int64_t, 8> resources =
          resource_op_info ? FindAccessedResources(&op, alias_analysis)
                           : UnknownResourceSet();
      assert(!resources.empty());
      const bool is_unknown = resources.count(kUnknownResourceId) > 0;
      const bool read_only = OpIsReadOnly(&op);
      bool indirectly_tracked_unknown_access = false;
      // First add edges from known resources.
      if (is_unknown) {
        for (auto& entry : per_resource_access_info_) {
          if (entry.getFirst() == kUnknownResourceId) continue;
          AddPredecessorsForAccess(entry.getFirst(), &op, read_only);
          indirectly_tracked_unknown_access |=
              unknown_access_indirectly_tracked_by_resource(entry.getFirst(),
                                                            read_only);
        }
      } else {
        for (int64_t resource : resources) {
          AddPredecessorsForAccess(resource, &op, read_only);
          indirectly_tracked_unknown_access |=
              unknown_access_indirectly_tracked_by_resource(resource,
                                                            read_only);
          // Update access info for known resources.
          TrackAccess(resource, &op, read_only);
        }
      }
      // If not indirectly tracked, add edges from the unknown resource.
      if (!indirectly_tracked_unknown_access) {
        AddPredecessorsForAccess(kUnknownResourceId, &op, read_only);
      }
      if (is_unknown) {
        // Update access info for unknown resource.
        TrackAccess(kUnknownResourceId, &op, read_only);
      }
    }
  }
}

llvm::SmallVector<Operation*, 4>
SideEffectAnalysisInfo::DirectControlPredecessors(
    Operation* op, llvm::function_ref<bool(Operation*)> filter) const {
  llvm::SmallVector<Operation*, 4> result;
  auto it = sorted_control_predecessors_.find(op);
  if (it == sorted_control_predecessors_.end()) return result;
  result.reserve(it->getSecond().size());
  for (auto predecessor : it->getSecond()) {
    if (!filter || filter(predecessor)) result.push_back(predecessor);
  }
  return result;
}

llvm::SmallVector<Operation*, 4>
SideEffectAnalysisInfo::DirectControlSuccessors(
    Operation* op, llvm::function_ref<bool(Operation*)> filter) const {
  llvm::SmallVector<Operation*, 4> result;
  auto it = sorted_control_successors_.find(op);
  if (it == sorted_control_successors_.end()) return result;
  result.reserve(it->getSecond().size());
  for (auto successor : it->getSecond()) {
    if (!filter || filter(successor)) result.push_back(successor);
  }
  return result;
}
}  // namespace detail

SideEffectAnalysis::SideEffectAnalysis(Operation* op) {
  auto module = dyn_cast<ModuleOp>(op);
  assert(module);

  // Analyze entire module for alias analysis info.
  ResourceAliasAnalysis alias_analysis(module);

  // Analyze all functions.
  for (auto func : module.getOps<FuncOp>())
    this->info_map_.try_emplace(func, func,
                                alias_analysis.GetAnalysisForFunc(func));
}

}  // namespace TF
}  // namespace mlir
