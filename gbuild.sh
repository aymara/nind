#!/bin/bash

#Fail if anything goes wrong
# set -o errexit
set -o pipefail
set -o nounset
# set -o xtrace

echoerr() { printf "\e[31;1m%s\e[0m\n" "$*" >&2; }


usage()
{
cat << EOF 1>&2; exit 1;
Synopsis: $0 [OPTIONS]

Options default values are in parentheses.

  -a asan           <(OFF)|ON> compile with adress sanitizer
  -j n              <INTEGER> set the compilation to a number of parallel processes.
                    Default 0 => the value is derived from CPUs available.
  -m mode           <(Debug)|Release|RelWithDebInfo> compile mode
  -n arch           <(generic)|native> target architecture mode
  -p package        <(OFF)|ON> package building selection
  -t tests          <(OFF)|ON> run unit tests after install
  -v version    <(val)|rev> version number is set either to the value set by
                config files or to the short git sha1
  -G Generator  <(Ninja)|Unix|MSYS|NMake|VS> which cmake generator to use.
EOF
exit 1
}

[ -z "$NIND_DIST" ] && echo "Need to set NIND_DIST" && exit 1;

arch="generic"
j="0"
WITH_DEBUG_MESSAGES="OFF"
mode="Debug"
version="val"
resources="build"
CMAKE_GENERATOR="Ninja"
WITH_ASAN="OFF"
WITH_ARCH="OFF"
WITH_PACK="OFF"
RUN_UNIT_TESTS="OFF"

while getopts ":a:j:m:n:p:t:v:G:" o; do
    case "${o}" in
        a)
            WITH_ASAN=${OPTARG}
            [[ "$WITH_ASAN" == "ON" || "$WITH_ASAN" == "OFF" ]] || usage
            ;;
        j)
            j=${OPTARG}
            [[ -n "${j##*[!0-9]*}" ]] || usage
            ;;
        m)
            mode=${OPTARG}
            [[ "$mode" == "Debug" || "$mode" == "Release"  || "$mode" == "RelWithDebInfo" ]] || usage
            ;;
        n)
            arch=${OPTARG}
            [[ "x$arch" == "xnative" || "x$arch" == "xgeneric" ]] || usage
            ;;
        p)
            WITH_PACK=${OPTARG}
            [[ "$WITH_PACK" == "ON" || "$WITH_PACK" == "OFF" ]] || usage
            ;;
        t)
            RUN_UNIT_TESTS=${OPTARG}
            [[ "x$RUN_UNIT_TESTS" == "xON" || "x$RUN_UNIT_TESTS" == "xOFF" ]] || usage
            ;;
        v)
            version=$OPTARG
            [[ "$version" == "val" ||  "$version" == "rev" ]] || usage
            ;;
        G)
            CMAKE_GENERATOR=${OPTARG}
            echo "CMAKE_GENERATOR=$CMAKE_GENERATOR"
            [[     "$CMAKE_GENERATOR" == "Unix"  || 
                   "$CMAKE_GENERATOR" == "Ninja" ||
                   "$CMAKE_GENERATOR" == "MSYS"  ||
                   "$CMAKE_GENERATOR" == "NMake" ||
                   "$CMAKE_GENERATOR" == "VS"
            ]] || usage
          ;;
        *)
            usage
            ;;
    esac
done
shift $((OPTIND-1))

if type git && git rev-parse --git-dir; then
    current_branch=`git rev-parse --abbrev-ref HEAD`
    current_revision=`git rev-parse --short HEAD`
    current_timestamp=`git show -s --format=%cI HEAD | sed -e 's/[^0-9]//g'`
else
    # use default values
    current_branch="default"
    current_revision="default"
    current_timestamp=1
fi
current_project=`basename $PWD`
current_project_name="`head -n1 CMakeLists.txt`"
build_prefix=build/$current_branch
source_dir=$PWD

if [[ $version = "rev" ]]; then
  release="$current_timestamp-$current_revision"
else
  release="0"
fi

if [[ "$j" == "0" ]]; then
  if [[ $CMAKE_GENERATOR == "VS" ]]; then
    j=`WMIC CPU Get NumberOfCores | head -n 2 | tail -n 1 | sed -n "s/\s//gp"`
  elif [[ $CMAKE_GENERATOR == "Unix" || $CMAKE_GENERATOR == "Ninja" ]]; then
    j=`grep -c ^processor /proc/cpuinfo`
  fi
fi
if [[ "$j" == "1" ]]; then
  echoerr "Linear build"
else
  echoerr "Parallel build on $j processors"
fi

# export VERBOSE=1
if [[ $mode == "Release" ]]; then
  cmake_mode="Release"
elif [[ $mode == "RelWithDebInfo" ]]; then
  cmake_mode="RelWithDebInfo"
else
  cmake_mode="Debug"
fi

if [[ $arch == "native" ]]; then
  WITH_ARCH="ON"
else
  WITH_ARCH="OFF"
fi

if [[ $CMAKE_GENERATOR == "Unix" ]]; then
  make_cmd="make -j$j"
  make_test="make test"
  make_install="make install"
  make_package="make package"
  generator="Unix Makefiles"
elif [[ $CMAKE_GENERATOR == "Ninja" ]]; then
  make_cmd="ninja -j $j"
  make_test="ctest"
  make_install="ninja install"
  make_package="ninja package"
  generator="Ninja"
elif [[ $CMAKE_GENERATOR == "MSYS" ]]; then
  make_cmd="make -j$j"
  make_test="make test"
  make_install="make install"
  make_package="make package"
  generator="MSYS Makefiles"
elif [[ $CMAKE_GENERATOR == "NMake" ]]; then
  make_cmd="nmake && exit 0"
  make_test="nmake test"
  make_install="nmake install"
  make_package=""
  generator="NMake Makefiles"
elif [[ $CMAKE_GENERATOR == "VS" ]]; then
  make_cmd="""
  pwd &&
  cmake --build . --config $cmake_mode
  """
  make_test=""
  make_install=""
  generator="Visual Studio 14 2015 Win64"
else
  make_cmd="make -j$j"
fi


echo "version='$version' release='$release'"

build_dir=$build_prefix/$mode-$WITH_ASAN/$current_project
mkdir -p $build_dir
pushd $build_dir

if [ $CMAKE_GENERATOR == "Unix" ] && [ "x$cmake_mode" == "xRelease" ] ;
then
  if compgen -G "*/src/*-build/*.rpm" > /dev/null; then
    rm -f */src/*-build/*.rpm
  fi
  if compgen -G "*/src/*-build/*.deb" > /dev/null; then
    rm -f */src/*-build/*.deb
  fi
fi

# export LSAN_OPTIONS=suppressions=${LIMA_SOURCES}/suppr.txt
export ASAN_OPTIONS=halt_on_error=0,fast_unwind_on_malloc=0

echoerr "Launching cmake from $PWD $source_dir WITH_ASAN=$WITH_ASAN"
cmake -G "$generator"  \
  -DCMAKE_BUILD_TYPE:STRING=$cmake_mode \
  -DCMAKE_INSTALL_PREFIX:PATH=$NIND_DIST \
  -DWITH_ARCH=$WITH_ARCH \
  -DWITH_ASAN=$WITH_ASAN \
  $source_dir
result=$?
if [ "$result" != "0" ]; then echoerr "Failed to configure NIND."; popd; exit $result; fi

echoerr "Running make command:"
echo "$make_cmd"
eval $make_cmd
result=$?
if [ "$result" != "0" ]; then echoerr "Failed to build NIND."; popd; exit $result; fi


if [ $RUN_UNIT_TESTS == "ON" ];
then
echoerr "Running make test:"
eval $make_test
result=$?
echoerr "Done make test:"
fi

echoerr "Running make install:"
echo "$make_install"
eval $make_install
result=$?
if [ "$result" != "0" ]; then echoerr "Failed to install NIND."; popd; exit $result; fi

if [ $WITH_PACK == "ON" ];
then
  echoerr "Running make package:"
  echo "$make_package"
  eval $make_package
  result=$?
  if [ "$result" != "0" ]; then echoerr "Failed to package NIND."; popd; exit $result; fi
fi


if [[ ( $CMAKE_GENERATOR == "Ninja" || $CMAKE_GENERATOR == "Unix" ) && "x$cmake_mode" == "xRelease" ]] ;
then
  echoerr "Install package:"
  install -d $NIND_DIST/share/apps/nind/packages
  if compgen -G ./*.rpm > /dev/null; then
    echo "Install RPM package into $NIND_DIST/share/apps/nind/packages"
    install ./*.rpm $NIND_DIST/share/apps/nind/packages
  fi
  if compgen -G ./*.deb > /dev/null; then
    echo "Install DEB package into $NIND_DIST/share/apps/nind/packages"
    install ./*.deb $NIND_DIST/share/apps/nind/packages
  fi
fi

echoerr "Built NIND successfully.";
popd

exit $result


