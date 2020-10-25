module Tapir

using Base: _wait

# const TaskGroup = Vector{Task}
const TaskGroup = Channel{Task}

struct OutlinedFunction
    f::Ptr{Cvoid}
    arg::Vector{UInt8}
end

function (outlined::OutlinedFunction)()
    # TODO: propagate world age of the callee?
    ccall(outlined.f, Cvoid, (Ptr{UInt8},), outlined.arg)
    return nothing
end

# taskgroup() = Task[]
taskgroup() = Channel{Task}(4)

function spawn!(tasks::TaskGroup, f::Ptr{Cvoid}, parg::Ptr{UInt8}, arg_size::Int64)
    # ASK: Do we need to copy `arg` here? It looks like that's what
    # `qthread_fork_copyargs` does. But is it possible to use `parg` (a pointer
    # to `alloca`'ed struct) as-is because we know that the task would be
    # synchronized before the parent function exist? But maybe this is
    # problematic when spawning tasks in a loop?
    arg = copy(unsafe_wrap(Vector{UInt8}, parg, arg_size))
    t = Task(OutlinedFunction(f, arg))
    t.sticky = false
    schedule(t)
    push!(tasks, t)
    return nothing
end

function sync!(tasks::TaskGroup)
    # We can use `while isempty(tasks)` without data race because once we hit
    # `isempty(tasks)`, there is no task adding a new task to this task group.
    c_ex = nothing
    while !isempty(tasks)
        r = popfirst!(tasks)
        _wait(r)
        if istaskfailed(r)
            if c_ex === nothing
                c_ex = CompositeException()
            end
            push!(c_ex, TaskFailedException(r))
        end
    end
    close(tasks)
    if c_ex !== nothing
        throw(c_ex)
    end
end

@assert precompile(taskgroup, ())
@assert precompile(spawn!, (TaskGroup, Ptr{Cvoid}, Ptr{UInt8}, Int64))
@assert precompile(sync!, (TaskGroup,))

end

const _Tapir_taskgroup = Tapir.taskgroup
const _Tapir_spawn! = Tapir.spawn!
const _Tapir_sync! = Tapir.sync!
