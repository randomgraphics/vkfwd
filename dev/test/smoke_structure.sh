#!/usr/bin/env sh
set -eu

root="${1:-.}"

test -f "$root/README.md"
test -d "$root/src/vkfwd"
test -d "$root/src/contrib"
test -d "$root/dev/test"
test -d "$root/dev/env"
test -d "$root/dev/bin"
test -f "$root/src/vkfwd/layer.cpp"
