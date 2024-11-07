#!/bin/bash

cd build
python3 ../fuzz.py ../alive2-build/alive-tv ../llvm-build/bin/ ../llvm-project/ . ../patch.diff >> ../issue.md
