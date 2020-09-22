#!/bin/bash
JULIA="$PWD/usr/bin/julia"
set -ex
make clean
make -j2
cd ~/.julia/dev/PyCall
"$JULIA" --startup-file=no aot/compile.jl
aot/julia.sh --startup-file=no -e 'using InteractiveUtils; versioninfo()'
