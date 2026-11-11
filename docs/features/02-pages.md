# 02 — UNE seule page

## Ce que ça doit faire

Depuis le 11/11, le robot n'a plus de cycle d'applis : **un seul écran, le
tableau**, porte tout ce qu'il sait. Rien à faire défiler pour retrouver une
information : elle est là, ou elle n'existe pas.

    y 0    heure (corps 6), visage du compagnon, date, batterie
    y 33   filet
    y 36   état : borne Wi-Fi + RSSI + uptime
    y 45   filet
    y 49   MÉTÉO (température et ciel)        | DEMAIN (plage, pluie)
    y 78   filet
    y 81   INTÉRIEUR (capteur, stade, forme)  | BATTERIE (jauge, watts)
    y 110  filet
    y 113  SYSTÈME (cpu, ram)                 | AGENDA (prochain rdv ou minuteurs)
    y 142  filet
    y 145  DERNIER MOT (3 lignes)             | CONSEIL (3 lignes)
    y 172  filet
    y 176  pied : les quatre jauges du compagnon, en deux colonnes
           (FAIM, BONH | ENER, SANTE) avec leur valeur en clair

Règle de lecture, héritée du reste du firmware : **une valeur indisponible
affiche `N.D.` ou `--`, jamais zéro ni un nom supposé.** Une jauge inconnue
affiche un tiret, pas une barre vide (qui se lirait « à zéro »).

## Ce qui a disparu, et où c'est allé

| Avant | Maintenant |
|---|---|
| page METEO | rangée 1 du tableau |
| page SYSTEM | rangées 2 (batterie) et 3 (cpu, ram) |
| page ACCUEIL (heure, date, rdv) | en-tête et rangée 3 |
| page HISTORIQUE | « DERNIER MOT » (rangée 4) |
| page CONSEIL | « CONSEIL » (rangée 4) |
| page YOUTUBE | **retirée** le 11/11 (plus affichée, plus relue : son TLS toutes les 30 min n'a plus de consommateur) |
| page LIVRE | **retirée de l'écran** le 11/11 ; le lecteur reste dans le firmware et se pilote par le web (`/livre?op=liste`) |
| launcher (grille de 8 tuiles) | supprimé : plus rien à lancer |
| personnalité, voix, radio, rappels, réglages | inchangés (écrans plein écran et web) |

Les fonctions `draw_app_*` des anciennes pages sont toujours dans `main.cpp`
(le routeur et la table `PAGE_ACT` les connaissent), mais le cycle ne contient
plus qu'une page : `PAGE_COUNT` vaut 1. La page RÉGLAGES (`PAGE_REG`, hors
cycle) reste joignable par le web pour le clavier Wi-Fi.

## Gestes de la page

| Geste | Effet |
|---|---|
| bouton 1 court | relit les données (météo, capteurs) et redessine |
| bouton 2 court | rien de plus : il n'y a pas d'autre page |
| double bouton 1 | l'assistant vocal (inchangé, depuis n'importe quel écran) |
| double bouton 2 | sans effet (plus de livre à ouvrir) |
| appui long (1 ou 2) | retour au tableau, arrêt de ce qui tourne (voix, radio) |

Depuis le 11/11, aucun geste n'ouvre plus le livre : la page unique ne le
montre plus et son lecteur se pilote par le web.

## Comment c'est fait

- `main/tableau.h` définit `tableau_t` (des données, aucun pointeur vers le
  firmware) et `tableau_dessine()`.
- `main/tableau.cpp` porte **tout le dessin** et n'inclut que `art.h` : aucune
  dépendance ESP-IDF. Le même code est donc rendu sur le PC par
  `tools/apercu_tableau.cpp`, ce qui permet de juger une mise en page sans
  flasher la carte :

      g++ -O2 -o build/apercu_tableau tools/apercu_tableau.cpp main/tableau.cpp \
          main/art.cpp main/petart.cpp main/pet.cpp main/font5.cpp -Imain -lm
      build/apercu_tableau plein /tmp/plein     # jeu de données complet
      build/apercu_tableau vide  /tmp/vide      # tout ce qui peut manquer manque
      python3 tools/fb2png.py /tmp/plein.fb /tmp/plein.png 3

- `tableau_maitre()` dans `main.cpp` remplit la structure avec les valeurs
  vivantes (horloge, `wifi_actif`, SHTC3, météo, compagnon sous `pet_mux`,
  `sys_cpu10`, agenda `ms_*`, dernier tour de MINO lu dans `hist`,
  `conseil_txt`) puis appelle `tableau_dessine()`.
- Le signe `%` est dessiné (deux points + barre) et non pris dans la police
  3x5 : son glyphe ressemble à un `Z` dès le corps 2.
- L'icône Wi-Fi de `art.h` fait 24 px de haut et ne rentre pas dans la bande
  d'état : la page dessine trois barres de signal (une par niveau de RSSI).
- Le banc système (`sys_page_entre` / `sys_echantillon`) tourne aussi sur le
  tableau, sinon le CPU resterait à zéro ; seule la page SYSTEM redessine à sa
  propre cadence.

## Tests

| Test | Attendu |
|---|---|
| `bash tests/run_unit.sh` | `test_tableau` : 19 vérifications vertes |
| encre | la page porte du contenu (19 % de pixels noirs avec un jeu complet) et reste sous 45 % |
| zones | les sept bandes (en-tête, état, 4 rangées, pied) contiennent de l'encre |
| déterminisme | deux rendus de la même donnée sont identiques au pixel |
| sensibilité | changer l'heure, la batterie, la météo, l'agenda, les jauges ou les deux textes change l'image |
| données absentes | la page reste dessinée, `N.D.` / `--`, aucune jauge pleine |
| texte très long | 95 signes dans chaque champ : la dernière colonne de pixels reste blanche |

Sur la carte (après flash) : `/state` doit annoncer le nouvel `app`, `page` =
`ACCUEIL` en permanence, et `/ecran?raw=1` doit rendre exactement l'aperçu du
PC (passé à `tools/fb2png.py`, qui accepte l'hexadécimal). `btn 1` ne doit plus
changer de page.
