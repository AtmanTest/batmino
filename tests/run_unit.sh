#!/bin/bash
# Tests unitaires (sur le PC, sans la carte).
cd "$(dirname "$0")/.."
mkdir -p build/tests
rc=0
gcc -std=c11 -O2 -Wall -Wextra -o build/tests/test_historique tests/unit/test_historique.c || rc=1
./build/tests/test_historique || rc=1

# les gestes des deux boutons : le coeur de la navigation, sans materiel
gcc -std=c11 -O2 -Wall -Wextra -o build/tests/test_geste tests/unit/test_geste.c || rc=1
./build/tests/test_geste || rc=1

# le routeur des boutons : le vrai bloc de main.cpp, compile contre des stubs
python3 tests/unit/extrait_routeur.py build/tests/routeur_extrait.inc || rc=1
g++ -std=c++11 -O2 -Wall -Wextra -Ibuild/tests -o build/tests/test_routeur \
    tests/unit/test_routeur.cpp || rc=1
./build/tests/test_routeur || rc=1

g++ -std=c++11 -O2 -Wall -Wextra -o build/tests/test_livre \
    tests/unit/test_livre.cpp main/livre.cpp main/font5.cpp main/art.cpp -lm || rc=1
./build/tests/test_livre || rc=1

# la page unique : le vrai dessin du firmware, rendu sur le PC
g++ -std=c++11 -O2 -Wall -Wextra -Imain -o build/tests/test_tableau \
    tests/unit/test_tableau.cpp main/tableau.cpp main/art.cpp main/pet.cpp \
    main/petart.cpp main/font5.cpp -lm || rc=1
./build/tests/test_tableau || rc=1

if [ $rc -eq 0 ]; then echo "unitaires : tout bon"; else echo "unitaires : ECHEC"; fi
exit $rc
