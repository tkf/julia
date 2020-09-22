#!/bin/bash
JULIA="$PWD/usr/bin/julia"
set -ex
make clean
make -j2
cd ~/.julia/dev/PyCall
"$JULIA" --startup-file=no -e 'using Pkg; pkg"add PackageCompiler#cb994c72e2087c57ffa4727ef93589e1b98d8a32"'
"$JULIA" --startup-file=no -e 'using Pkg; pkg"dev PyCall'
"$JULIA" --startup-file=no aot/compile.jl
aot/julia.sh --startup-file=no -e 'using InteractiveUtils; versioninfo()'
