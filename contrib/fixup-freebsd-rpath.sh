#!/bin/sh
# Usage: fixup-freebsd-rpath.sh <patchelf path> <install libdir> <build libdir>

if [ "$(uname -s)" != "FreeBSD" ]; then
    echo "This script is intended only for use on FreeBSD"
    exit 1
fi

if [ $# -ne 3 ]; then
    echo "Incorrect number of arguments"
    exit 1
fi

patchelf="$1"
install_libdir="$2"
build_libdir="$3"

for lib in ${install_libdir}/*.so*; do
    # First get the current RPATH
    rpath="$(${patchelf} --print-rpath ${lib})"

    # If it doesn't contain the build's libdir, we're don't care about it
    if [ -z "$(echo ${rpath} | grep -F ${build_libdir})" ]; then
        continue
    fi

    # Remove build_libdir from the RPATH, retaining the rest
    new_rpath="$(echo ${rpath} | tr : \\n | grep -vF ${build_libdir} | tr \\n :)"
    new_rpath="${new_rpath%?}"

    # Now set the new RPATH
    ${patchelf} --set-rpath "${new_rpath}" ${lib}
done
