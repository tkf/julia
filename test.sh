#!/bin/bash
set -ex
mkdir -pv ~/.julia/config
echo oops > ~/.julia/config/startup.jl
rm -vf usr/lib/julia/sys*
make -j4
