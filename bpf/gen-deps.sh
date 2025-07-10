#!/bin/bash

USE_BTF=${1:-1}

echo "# Copy this to Makefile (USE_BTF=$USE_BTF):"
echo ""
echo "TUTUICMPTUNNEL_BASE_DEPS := \\"

# Base dependencies (always needed)
clang -target bpf -I../common -I. -M tutuicmptunnel.bpf.c 2>/dev/null | \
    sed 's/.*://' | \
    tr ' ' '\n' | \
    grep -E '\.(h|hpp)$' | \
    grep -v vmlinux.h | \
    sort -u | \
    sed 's/^/                            /' | \
    sed '$!s/$/ \\/'

echo ""
echo ""
echo "# BTF-specific dependencies"
if [ "$USE_BTF" = "1" ]; then
    echo "ifeq (\$(USE_BTF),1)"
    echo "TUTUICMPTUNNEL_BTF_DEPS := vmlinux.h"
    echo "else"
    echo "TUTUICMPTUNNEL_BTF_DEPS := "
    echo "endif"
else
    echo "# vmlinux.h not needed when USE_BTF=0"
fi

