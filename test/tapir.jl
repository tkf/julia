using Base.Tapir

function f()
    let token = @syncregion()
        @spawn token begin
            1 + 1
        end
        @sync_end token
    end
end

function taskloop(N)
    let token = @syncregion()
        for i in 1:N
            @spawn token begin
                1 + 1
            end
        end
        @sync_end token
    end
end

function taskloop2(N)
    @sync for i in 1:N
        @spawn begin
            1 + 1
        end
    end
end

function taskloop3(N)
    @par for i in 1:N
        1+1
    end
end

function vecadd(out, A, B)
    @assert length(out) == length(A) == length(B)
    @inbounds begin
        @par for i in 1:length(out)
            out[i] = A[i] + B[i]
        end
    end
    return out
end

function fib(N)
    if N <= 1
        return N
    end
    token = @syncregion()
    x1 = Ref{Int64}()
    @spawn token begin
        x1[]  = fib(N-1)
    end
    x2 = fib(N-2)
    @sync_end token
    return x1[] + x2
end

###
# Interesting corner cases and broken IR
###

##
# Parallel regions with errors are tricky
# #1  detach within %sr, #2, #3
# #2  ...
#     unreachable()
#     reattach within %sr, #3
# #3  sync within %sr
#
# Normally a unreachable get's turned into a ReturnNode(),
# but that breaks the CFG. So we need to detect that we are
# in a parallel region.
#
# Question:
#   - Can we elimante a parallel region that throws?
#     Probably if the sync is dead as well. We could always
#     use the serial projection and serially execute the region.

function vecadd_err(out, A, B)
    @assert length(out) == length(A) == length(B)
    @inbounds begin
        @par for i in 1:length(out)
            out[i] = A[i] + B[i]
            error()
        end
    end
    return out
end

# This function is broken due to the PhiNode
@noinline function fib2(N)
    if N <= 1
        return N
    end
    token = @syncregion()
    x1 = 0
    @spawn token begin
        x1  = fib2(N-1)
    end
    x2 = fib2(N-2)
    @sync_end token
    return x1 + x2
end

_dummy_mapfold(f, op, xs) = isempty(xs) ? op(f(xs[1]), f(xs[2])) : f(xs[1])

function mapfold(f, op, xs)
    if length(xs) == 1
        return @inbounds f(xs[1])
    elseif length(xs) == 2
        return @inbounds op(f(xs[1]), f(xs[2]))
    else
        left = @inbounds @view xs[begin:(end-begin+1)÷2]
        right = @inbounds @view xs[(end-begin+1)÷2+1:end]
        ytype = Core.Compiler.return_type(  # TODO: don't
            _dummy_mapfold,
            Tuple{typeof(f),typeof(op),typeof(xs)},
        )
        ref = Ref{ytype}()
        token = @syncregion()
        @spawn token begin
            ref[] = mapfold(f, op, right)
        end
        y = mapfold(f, op, left)
        @sync_end token
        return op(y, ref[])
    end
end

function append!!(a, b)
    ys::Vector = a isa Vector ? a : collect(a)
    if eltype(b) <: eltype(ys)
        zs = append!(ys, b)
    else
        zs = similar(ys, promote_type(eltype(ys), eltype(b)), (length(ys) + lengh(b)))
        copyto!(zs, 1, ys, 1, length(ys))
        zs[length(ys)+1:end] .= b
    end
    return zs
end

tmap(f, xs) = mapfold(tuple ∘ f, append!!, xs)
