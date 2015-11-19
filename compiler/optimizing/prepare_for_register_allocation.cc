/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "prepare_for_register_allocation.h"

namespace art {

void PrepareForRegisterAllocation::Run() {
  // Order does not matter.
  for (HReversePostOrderIterator it(*GetGraph()); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    // No need to visit the phis.
    for (HInstructionIterator inst_it(block->GetInstructions()); !inst_it.Done();
         inst_it.Advance()) {
      inst_it.Current()->Accept(this);
    }
  }
}

void PrepareForRegisterAllocation::VisitNullCheck(HNullCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocation::VisitDivZeroCheck(HDivZeroCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocation::VisitBoundsCheck(HBoundsCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocation::VisitBoundType(HBoundType* bound_type) {
  bound_type->ReplaceWith(bound_type->InputAt(0));
  bound_type->GetBlock()->RemoveInstruction(bound_type);
}

void PrepareForRegisterAllocation::VisitClinitCheck(HClinitCheck* check) {
  // Try to find a static invoke from which this check originated.
  HInvokeStaticOrDirect* invoke = nullptr;
  for (HUseIterator<HInstruction*> it(check->GetUses()); !it.Done(); it.Advance()) {
    HInstruction* user = it.Current()->GetUser();
    if (user->IsInvokeStaticOrDirect() && CanMoveClinitCheck(check, user)) {
      invoke = user->AsInvokeStaticOrDirect();
      DCHECK(invoke->IsStaticWithExplicitClinitCheck());
      invoke->RemoveExplicitClinitCheck(HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit);
      break;
    }
  }
  // If we found a static invoke for merging, remove the check from all other static invokes.
  if (invoke != nullptr) {
    for (HUseIterator<HInstruction*> it(check->GetUses()); !it.Done(); ) {
      HInstruction* user = it.Current()->GetUser();
      DCHECK(invoke->StrictlyDominates(user));  // All other uses must be dominated.
      it.Advance();  // Advance before we remove the node, reference to the next node is preserved.
      if (user->IsInvokeStaticOrDirect()) {
        user->AsInvokeStaticOrDirect()->RemoveExplicitClinitCheck(
            HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
      }
    }
  }

  HLoadClass* load_class = check->GetLoadClass();
  bool can_merge_with_load_class = CanMoveClinitCheck(load_class, check);

  check->ReplaceWith(load_class);

  if (invoke != nullptr) {
    // Remove the check from the graph. It has been merged into the invoke.
    check->GetBlock()->RemoveInstruction(check);
    // Check if we can merge the load class as well.
    if (can_merge_with_load_class && !load_class->HasUses()) {
      load_class->GetBlock()->RemoveInstruction(load_class);
    }
  } else if (can_merge_with_load_class) {
    // Pass the initialization duty to the `HLoadClass` instruction,
    // and remove the instruction from the graph.
    load_class->SetMustGenerateClinitCheck(true);
    check->GetBlock()->RemoveInstruction(check);
  }
}

void PrepareForRegisterAllocation::VisitCondition(HCondition* condition) {
  bool needs_materialization = false;
  if (!condition->GetUses().HasOnlyOneUse() || !condition->GetEnvUses().IsEmpty()) {
    needs_materialization = true;
  } else {
    HInstruction* user = condition->GetUses().GetFirst()->GetUser();
    if (!user->IsIf() && !user->IsDeoptimize()) {
      needs_materialization = true;
    } else {
      // TODO: if there is no intervening instructions with side-effect between this condition
      // and the If instruction, we should move the condition just before the If.
      if (condition->GetNext() != user) {
        needs_materialization = true;
      }
    }
  }
  if (!needs_materialization) {
    condition->ClearNeedsMaterialization();
  }
}

void PrepareForRegisterAllocation::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  if (invoke->IsStaticWithExplicitClinitCheck()) {
    size_t last_input_index = invoke->InputCount() - 1;
    HLoadClass* last_input = invoke->InputAt(last_input_index)->AsLoadClass();
    DCHECK(last_input != nullptr)
        << "Last input is not HLoadClass. It is " << last_input->DebugName();

    // Detach the explicit class initialization check from the invoke.
    // Keeping track of the initializing instruction is no longer required
    // at this stage (i.e., after inlining has been performed).
    invoke->RemoveExplicitClinitCheck(HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);

    // Merging with load class should have happened in VisitClinitCheck().
    DCHECK(!CanMoveClinitCheck(last_input, invoke));
  }
}

bool PrepareForRegisterAllocation::CanMoveClinitCheck(HInstruction* input, HInstruction* user) {
  // Determine if input and user come from the same dex instruction, so that we can move
  // the clinit check responsibility from one to the other, i.e. from HClinitCheck (user)
  // to HLoadClass (input), or from HClinitCheck (input) to HInvokeStaticOrDirect (user).

  // Start with a quick dex pc check.
  if (user->GetDexPc() != input->GetDexPc()) {
    return false;
  }

  // Now do a thorough environment check that this is really coming from the same instruction in
  // the same inlined graph. Unfortunately, we have to go through the whole environment chain.
  HEnvironment* user_environment = user->GetEnvironment();
  HEnvironment* input_environment = input->GetEnvironment();
  while (user_environment != nullptr || input_environment != nullptr) {
    if (user_environment == nullptr || input_environment == nullptr) {
      // Different environment chain length. This happens when a method is called
      // once directly and once indirectly through another inlined method.
      return false;
    }
    if (user_environment->GetDexPc() != input_environment->GetDexPc() ||
        user_environment->GetMethodIdx() != input_environment->GetMethodIdx() ||
        !IsSameDexFile(user_environment->GetDexFile(), input_environment->GetDexFile())) {
      return false;
    }
    user_environment = user_environment->GetParent();
    input_environment = input_environment->GetParent();
  }

  // Check for code motion taking the input to a different block.
  if (user->GetBlock() != input->GetBlock()) {
    return false;
  }

  // In debug mode, check that we have not inserted a throwing instruction
  // or an instruction with side effects between input and user.
  if (kIsDebugBuild) {
    for (HInstruction* between = input->GetNext(); between != user; between = between->GetNext()) {
      CHECK(between != nullptr);  // User must be after input in the same block.
      CHECK(!between->CanThrow());
      CHECK(!between->HasSideEffects());
    }
  }
  return true;
}

}  // namespace art
