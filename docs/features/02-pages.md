# 02 — Une appli par page

## Ce que ça doit faire

Le même principe que les premières versions : **un écran = une appli = un
sujet**. Pas de fourre-tout.

| Page | Appli | Contenu |
|---|---|---|
| 0 ACCUEIL | l'horloge | l'heure, la date, le prochain rendez-vous, le compagnon |
| 1 METEO | la météo | aujourd'hui (température en gros, état) et demain (min/max, pluie) |
| 2 RAPPELS | les minuteurs | les rappels en cours : nom, temps restant, anneau |
| 3 SYSTEM | le banc | cpu, ram, psram, watts, réseau, carte SD et flash |
| 4 HISTORIQUE | le journal | les derniers échanges lus sur la carte |
| 5 YOUTUBE | la chaîne | la dernière vidéo, ses vues, son âge (voir 08-youtube.md) |

La page RÉGLAGES (`PAGE_REG`, hors cycle) existe toujours dans le code pour le
clavier Wi-Fi ; elle n'est plus dans la rotation.

## Comment c'est fait

- `PAGE_MAITRE` 0, `PAGE_METEO` 1, `PAGE_TIMER` 2, `PAGE_SYS` 3, `PAGE_HIST` 4,
  `PAGE_COUNT` 5, `PAGE_REG` 5 (hors cycle).
- `show_ui()` aiguille sur une fonction `draw_app_*` par page, et rien d'autre.
- La météo, qui était sur l'accueil, a déménagé sur sa page ; le nom de la borne
  Wi-Fi a suivi sur SYSTEM.
- Le cycle se parcourt au bouton (1 = suivante, 2 = précédente) ; `ui N` par la
  console fait la même chose pour les tests et les captures.

## Tests

| Test | Attendu |
|---|---|
| `btn 1` six fois | les 6 pages dans l'ordre, puis retour à ACCUEIL |
| une image par page (`fb`) | 6 framebuffers différents |
| contenu | ACCUEIL ne contient **ni météo ni réseau** ; METEO ne contient que le ciel ; SYSTEM porte le nom de la borne ; YOUTUBE la dernière vidéo |

Vérifié par `tools/test_global.py` sections 2 et 3 ; les images sont écrites
dans `/tmp/test_<page>.png`.
