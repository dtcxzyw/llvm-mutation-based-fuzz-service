#!/bin/bash
set -euo pipefail
shopt -s inherit_errexit

echo "LLVM_REVISION=$(git -C llvm-project rev-parse HEAD)" >> $GITHUB_ENV

COMMIT_URL=$(echo $GITHUB_PATCH_ID | tr -d '\r\n')
PATCH_URL=$COMMIT_URL.diff

echo "Downloading patch $PATCH_URL..."
wget $PATCH_URL -O patch.diff
git -C llvm-project apply ../patch.diff
PATCH_SHA256=$(sha256sum patch.diff | sed 's/\|/ /'|awk '{print $1}')
echo "COMMIT_URL=$COMMIT_URL" >> $GITHUB_ENV
echo "PATCH_SHA256=$PATCH_SHA256" >> $GITHUB_ENV
