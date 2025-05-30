#!/bin/bash
eval "$(garden-exec)"
set -e

garden env-keep-only

SCONS=scons/3.1.2-01c7
BOOST=boost/1.75.0-gcc11.2-py3.10-02c7
INCHI=inchi/1.05-01c7
SQLITE=sqlite/3.32.2-02c7
PYBIND11=pybind11/2.7.1-01c7

version=$(src/version.py)

loadmodules() {
    garden load \
        $SCONS/bin \
        $BOOST/lib \
        $INCHI/lib \
        $SQLITE/lib \
        $PYBIND11/lib \
        gcc/11.2.0-04c7/bin \
        enscons/0.23.0-01c7/lib-python37 \
        auditwheel/5.1.2-01c7/bin \
        patchelf/0.9-01c7/bin \


    garden prepend-path DESRES_MODULE_CXXFLAGS -fpermissive
    garden prepend-path PYTHONPATH $(readlink -f $(dirname $0))    # for sconsutils
    export MSYS_WITH_INCHI=1
}

build() {
    export PYTHONVER=$(python -c 'import sys; print("".join(map(str, sys.version_info[:2])))')
    rm -rf build/wheel/msys*
    BUILD_WHEEL_VERSION=$PYTHONVER DESRES_LOCATION= scons -j `nproc`
    ifile=build/wheel/dist/msys-${version}-cp${PYTHONVER}-cp${PYTHONVER}m-linux_x86_64.whl
    #auditwheel repair --plat manylinux_2_31_x86_64 -w build/wheel/dist $ifile
}

main() {
    loadmodules
    for py in desres-python/3.10.6-04c7; do
        garden load $py/bin
        build
    done
    # FIXME: python3.8 drops the 'm' from the ABI tag, but enscons still generates an ABI tag of 38m.
    for x in build/wheel/dist/*cp310m*.whl; do
        y=${x//cp310m/cp310}
        echo "rename $x -> $y"
        mv $x $y
    done
}

main
