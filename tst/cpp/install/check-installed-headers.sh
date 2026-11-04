#!/bin/bash
# Installs the build into a temporary prefix, then compiles every installed header on its own with
# only that prefix on the include path: a header including one that isn't installed fails.
# usage: check-installed-headers.sh BUILD_DIR CXX
set -u
BUILD_DIR=$1
CXX=$2
PREFIX=$(mktemp -d)
trap 'rm -rf "$PREFIX"' EXIT
if ! cmake --install "$BUILD_DIR" --prefix "$PREFIX" > "$PREFIX/install.log" 2>&1; then
    cat "$PREFIX/install.log"
    echo "install failed"
    exit 1
fi
failed=0
checked=0
while IFS= read -r header; do
    relative=${header#"$PREFIX/include/"}
    checked=$((checked + 1))
    if ! echo "#include \"$relative\"" | "$CXX" -std=c++14 -fsyntax-only -I"$PREFIX/include" -x c++ - 2> "$PREFIX/error.log"; then
        echo "FAILED: $relative"
        head -5 "$PREFIX/error.log"
        failed=1
    fi
done < <(find "$PREFIX/include" -name '*.h' | sort)
if [ "$checked" -eq 0 ]; then
    echo "no installed header found"
    exit 1
fi
echo "$checked installed headers checked"
exit $failed
