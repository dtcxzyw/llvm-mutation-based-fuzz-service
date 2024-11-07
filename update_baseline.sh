#!/bin/bash
set -euo pipefail
shopt -s inherit_errexit

llvm_commit=$(git -C llvm-project rev-parse HEAD)
git add .
git commit -m "llvm: Update baseline to $llvm_commit"
git push
