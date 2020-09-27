#!/bin/bash
set -ex
make -j4
./julia --project=/tmp/Repro -e 'using Repro; display(@doc Repro.Extras)'
