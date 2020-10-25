module TestTapir
include("tapir.jl")

using Test

macro test_error(expr)
    @gensym err tmp
    quote
        local $err
        $Test.@test try
            $expr
            false
        catch $tmp
            $err = $tmp
            true
        end
        $err
    end |> esc
end

@testset "fib" begin
    @test fib(1) == 1
    @test fib(2) == 1
    @test fib(3) == 2
    @test fib(4) == 3
    @test fib(5) == 5
end

@noinline always() = rand() <= 1
@noinline donothing() = always() || error("unreachable")

@testset "exceptions" begin
    function f()
        token = @syncregion()
        @spawn token begin
            always() && throw(KeyError(1))
        end
        @spawn token begin
            always() && throw(KeyError(2))
        end
        donothing()  # TODO: don't
        # `donothing` here is required for convincing the compiler to _not_
        # inline the tasks.
        @sync_end token
    end
    err = @test_error f()
    @test err isa CompositeException
    @test all(x -> x isa TaskFailedException, err.exceptions)
    exceptions = [e.task.result for e in err.exceptions]
    @test Set(exceptions) == Set([KeyError(1), KeyError(2)])
end

end
