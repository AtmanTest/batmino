# 01 — Les boutons

## Ce que ça doit faire

Deux boutons, deux rôles, aucun geste qui en cache un autre.

| Geste | Bouton 1 (BOOT) | Bouton 2 (PWR) |
|---|---|---|
| appui simple | page suivante | page précédente |
| double appui | lance l'assistant vocal (et **lui seul**) | — |
| appui long | retour à l'accueil, tâche en cours arrêtée | retour à l'accueil |

Pendant une réponse en plein écran : 1 = suite, 2 = avant, un appui de plus à la
fin referme. Pendant une écoute ou une lecture : n'importe quel appui arrête.

Sous la page RAPPELS, les gestes de l'appli reprennent la main :
LONG 1 = ajoute un rappel de la durée choisie, LONG 2 = change la durée.

## Comment c'est fait

- Détection dans `button_task` : anti-rebond 40 ms, appui long à 900 ms.
- L'appui simple du bouton 1 est **retenu 320 ms** (fenêtre du double appui) :
  c'est le prix à payer pour que le double appui lance l'assistant sans changer
  de page au passage. Le bouton 2 part tout de suite.
- Anti-rafale : deux changements de page à moins de 1,2 s sont refusés
  (`page : appui trop rapide, ignore`), parce qu'une dalle e-paper met ~1 s à se
  rafraîchir et qu'un enchaînement trop serré la met en panne.

## Tests

Rejoués par `tools/test_global.py` (section 2), et à la main par la console USB :

    btn 1            -> page suivante
    btn 2            -> page précédente
    btn 1 long       -> ACCUEIL
    btn 1 dbl        -> lance l'assistant (voix_etat=1)

| Test | Attendu |
|---|---|
| `btn 1` cinq fois depuis ACCUEIL | METEO, RAPPELS, SYSTEM, HISTORIQUE, ACCUEIL |
| `btn 2` depuis ACCUEIL | HISTORIQUE |
| `btn 1 long` depuis n'importe où | ACCUEIL |
| deux `btn 1` à moins de 1,2 s | le second répond `(ignore)` |
| appui simple du bouton 1 | ne lance **jamais** l'assistant |
