#!/bin/bash
# Tests unitaires (sur le PC, sans la carte).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests
gcc -std=c11 -O2 -Wall -Wextra -o build/tests/test_historique tests/unit/test_historique.c
./build/tests/test_historique
