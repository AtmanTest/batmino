#!/bin/bash
# Tests unitaires (sur le PC, sans la carte).
cd "$(dirname "$0")/.."
mkdir -p build/tests
rc=0
gcc -std=c11 -O2 -Wall -Wextra -o build/tests/test_historique tests/unit/test_historique.c || rc=1
./build/tests/test_historique || rc=1
g++ -std=c++11 -O2 -Wall -Wextra -o build/tests/test_livre \
    tests/unit/test_livre.cpp main/livre.cpp main/font5.cpp main/art.cpp -lm || rc=1
./build/tests/test_livre || rc=1
exit $rc
