module Tapir

export @syncregion, @spawn, @sync_end, @par

using Base: _wait

#####
##### Julia-Tapir Runtime
#####

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

#####
##### Julia-Tapir Frontend
#####

macro syncregion()
    Expr(:syncregion)
end

macro spawn(token, expr)
    Expr(:spawn, esc(token), esc(expr))
end

macro sync_end(token)
    Expr(:sync, esc(token))
end

macro loopinfo(args...)
    Expr(:loopinfo, args...)
end

const tokenname = gensym(:token)
macro sync(block)
    var = esc(tokenname)
    quote
        let $var = @syncregion()
            $(esc(block))
            @sync_end($var)
        end
    end
end

macro spawn(expr)
    var = esc(tokenname)
    quote
        @spawn $var $(esc(expr))
    end
end

macro par(expr)
    par_impl(:dac, expr)
end

macro par(strategy::Symbol, expr)
    par_impl(strategy, expr)
end

function par_impl(strategy::Symbol, expr)
    @assert expr.head === :for
    stcode = get((dac = ST_DAC, seq = ST_SEQ), strategy, nothing)
    if stcode === nothing
        error("Invalid strategy: ", strategy)
    end
    token = gensym(:token)
    body = expr.args[2]
    lhs = expr.args[1].args[1]
    range = expr.args[1].args[2]
    quote
        let $token = @syncregion()
            for $(esc(lhs)) = $(esc(range))
                @spawn $token $(esc(body))
                $(Expr(:loopinfo, (Symbol("tapir.loop.spawn.strategy"), Int(stcode))))
            end
            @sync_end $token
        end
    end
end

"""
    Base.Tapir.SpawningStrategy

[INTERNAL] Tapir spawning strategies.

This type enumerates valid arguments to `tapir.loop.spawn.strategy` loopinfo.
For the C++ coutner part, see `TapirLoopHints::SpawningStrategy`.

See:
* https://github.com/OpenCilk/opencilk-project/blob/opencilk/beta3/llvm/include/llvm/Transforms/Utils/TapirUtils.h#L216-L222
* https://github.com/OpenCilk/opencilk-project/blob/opencilk/beta3/llvm/include/llvm/Transforms/Utils/TapirUtils.h#L256-L265
"""
@enum SpawningStrategy begin
    ST_SEQ  # Spawn iterations sequentially
    ST_DAC  # Use divide-and-conquer
end

end

# Runtime functions called via `../src/task-tapir.c`
const _Tapir_taskgroup = Tapir.taskgroup
const _Tapir_spawn! = Tapir.spawn!
const _Tapir_sync! = Tapir.sync!
