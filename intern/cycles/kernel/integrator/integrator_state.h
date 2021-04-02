/*
 * Copyright 2011-2021 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Integrator State
 *
 * This file defines the data structures that define the state of a path. Any state that is
 * preserved and passed between kernel executions is part of this.
 *
 * The size of this state must be kept as small as possible, to reduce cache misses and keep memory
 * usage under control on GPUs that may execute millions of kernels.
 *
 * Memory may be allocated and passed along in different ways depending on the device. There may
 * be a scalar layout, or AoS or SoA layout for batches. The state may be passed along as a pointer
 * to every kernel, or the pointer may exist at program scope or in constant memory. To abstract
 * these differences between devices and experiment with different layouts, macros are used.
 *
 * INTEGRATOR_STATE_ARGS: prepend to argument definitions for every function that accesses
 * path state.
 * INTEGRATOR_STATE_CONST_ARGS: same as INTEGRATOR_STATE_ARGS, when state is read-only
 * INTEGRATOR_STATE_PASS: use to pass along state to other functions access it.
 *
 * INTEGRATOR_STATE(x, y): read nested struct member x.y of IntegratorState
 * INTEGRATOR_STATE_WRITE(x, y): write to nested struct member x.y of IntegratorState
 *
 * INTEGRATOR_STATE_ARRAY(x, index, y): read x[index].y
 * INTEGRATOR_STATE_ARRAY_WRITE(x, index, y): write x[index].y
 *
 * INTEGRATOR_STATE_COPY(to_x, from_x): copy contents of one nested struct to another
 *
 * INTEGRATOR_STATE_IS_NULL: test if any integrator state is available, for shader evaluation
 * INTEGRATOR_STATE_PASS_NULL: use to pass empty state to other functions.
 *
 * NOTE: if we end up with a device that passes no arguments, the leading comma will be a problem.
 * Can solve it with more macros if we encouter it, but rather ugly so postpone for now.
 */

#include "kernel/kernel_types.h"

#include "util/util_types.h"

#pragma once

CCL_NAMESPACE_BEGIN

/* Constants
 *
 * TODO: these could be made dynamic depending on the features used in the scene. */

#define INTEGRATOR_VOLUME_STACK_SIZE 4
#define INTEGRATOR_SHADOW_ISECT_SIZE 4

/* Abstraction
 *
 * Macros to access data structures on different devices. */

#ifdef __KERNEL_CPU__

/* Combined state for path. */
typedef struct IntegratorState {
#  define KERNEL_STRUCT_BEGIN(name) struct {
#  define KERNEL_STRUCT_MEMBER(type, name) type name;
#  define KERNEL_STRUCT_END(name) \
    } \
    name;
#  define KERNEL_STRUCT_END_ARRAY(name, size) \
    } \
    name[size];
#  include "kernel/integrator/integrator_state_template.h"
#  undef KERNEL_STRUCT_BEGIN
#  undef KERNEL_STRUCT_MEMBER
#  undef KERNEL_STRUCT_END
#  undef KERNEL_STRUCT_END_ARRAY
} IntegratorState;

/* Scalar access on CPU. */

#  define INTEGRATOR_STATE_ARGS \
    ccl_attr_maybe_unused const KernelGlobals *ccl_restrict kg, IntegratorState *ccl_restrict state
#  define INTEGRATOR_STATE_CONST_ARGS \
    ccl_attr_maybe_unused const KernelGlobals *ccl_restrict kg, \
        const IntegratorState *ccl_restrict state
#  define INTEGRATOR_STATE_PASS kg, state

#  define INTEGRATOR_STATE_PASS_NULL kg, NULL
#  define INTEGRATOR_STATE_IS_NULL state == NULL

#  define INTEGRATOR_STATE(nested_struct, member) \
    (((const IntegratorState *)state)->nested_struct.member)
#  define INTEGRATOR_STATE_WRITE(nested_struct, member) (state->nested_struct.member)

#  define INTEGRATOR_STATE_ARRAY(nested_struct, array_index, member) \
    (((const IntegratorState *)state)->nested_struct[array_index].member)
#  define INTEGRATOR_STATE_ARRAY_WRITE(nested_struct, array_index, member) \
    ((state)->nested_struct[array_index].member)

#else /* __KERNEL_CPU__ */

/* Array access on GPU with Structure-of-Arrays.
 *
 * PathTraceWorkGPU on the host manages memory allocation and assumes the struct
 * memory list is an array of 64-bit pointers. */

typedef struct IntegratorState {
#  define KERNEL_STRUCT_BEGIN(name) struct {
#  define KERNEL_STRUCT_MEMBER(type, name) type *name;
#  define KERNEL_STRUCT_END(name) \
    } \
    name;
#  define KERNEL_STRUCT_END_ARRAY(name, size) \
    } \
    name[size];
#  include "kernel/integrator/integrator_state_template.h"
#  undef KERNEL_STRUCT_BEGIN
#  undef KERNEL_STRUCT_MEMBER
#  undef KERNEL_STRUCT_END
#  undef KERNEL_STRUCT_END_ARRAY
} IntegratorState;

ccl_device_constant IntegratorState __integrator_state;

#  define INTEGRATOR_STATE_ARGS \
    const KernelGlobals *ccl_restrict kg, ccl_global IntegratorPathQueue *ccl_restrict queue, \
        const int path_index
#  define INTEGRATOR_STATE_CONST_ARGS \
    const KernelGlobals *ccl_restrict kg, ccl_global IntegratorPathQueue *ccl_restrict queue, \
        const int path_index
#  define INTEGRATOR_STATE_PASS kg, queue, path_index

#  define INTEGRATOR_STATE_PASS_NULL kg, NULL, -1
#  define INTEGRATOR_STATE_IS_NULL (path_index == -1)

#  define INTEGRATOR_STATE(nested_struct, member) \
    __integrator_state.nested_struct.member[path_index]
#  define INTEGRATOR_STATE_WRITE(nested_struct, member) INTEGRATOR_STATE(nested_struct, member)

#  define INTEGRATOR_STATE_ARRAY(nested_struct, array_index, member) \
    __integrator_state.nested_struct[array_index].member[path_index]
#  define INTEGRATOR_STATE_ARRAY_WRITE(nested_struct, array_index, member) \
    INTEGRATOR_STATE_ARRAY(nested_struct, array_index, member)

#endif /* __KERNEL__CPU__ */

CCL_NAMESPACE_END
