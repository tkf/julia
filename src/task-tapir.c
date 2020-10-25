// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  task-tapir.c
  Task-Tapir interface
*/

#include "julia.h"
#include "julia_internal.h"

JL_DLLEXPORT jl_value_t *jl_tapir_taskgroup(void)
{
    jl_value_t *ans;
    jl_value_t **argv;
    JL_GC_PUSHARGS(argv, 1);
    argv[0] = jl_get_function(jl_base_module, "_Tapir_taskgroup");
    ans = jl_apply(argv, 1);
    JL_GC_POP();
    return ans;
}

JL_DLLEXPORT void jl_tapir_spawn(jl_value_t *taskgroup, void *f, void *arg, size_t arg_size)
{
    jl_value_t **argv;
    JL_GC_PUSHARGS(argv, 5);
    argv[0] = jl_get_function(jl_base_module, "_Tapir_spawn!");
    argv[1] = taskgroup;
    argv[2] = jl_box_voidpointer(f);
    argv[3] = jl_box_uint8pointer(arg);
    argv[4] = jl_box_int64(arg_size); // TODO:jl_box_csize_t?
    jl_apply(argv, 5);
    JL_GC_POP();
    // Not using `jl_call` to propagate exception.
}

JL_DLLEXPORT void jl_tapir_sync(jl_value_t *taskgroup)
{
    jl_value_t **argv;
    JL_GC_PUSHARGS(argv, 2);
    argv[0] = jl_get_function(jl_base_module, "_Tapir_sync!");
    argv[1] = taskgroup;
    jl_apply(argv, 2);
    JL_GC_POP();
}
