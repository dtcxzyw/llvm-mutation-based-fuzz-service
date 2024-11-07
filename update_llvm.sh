#!/bin/bash
set -euo pipefail
shopt -s inherit_errexit

last_commit=$(git -C llvm-project rev-parse HEAD)
echo "LLVM_LAST_REVISION=$last_commit" >> $GITHUB_ENV
git submodule update --remote --merge llvm-project
current_commit=$(git -C llvm-project rev-parse HEAD)
echo "LLVM_REVISION=$current_commit" >> $GITHUB_ENV
echo -e "from $last_commit to $current_commit\n" > CHANGELOGS
git -C llvm-project log $last_commit..HEAD --pretty=oneline >> CHANGELOGS
cat CHANGELOGS
