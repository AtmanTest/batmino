# US-006 — Deux pages : l'accueil-conversation et le système

**Demande (Taz)** : « on va mettre le parler directement dans le 0, inutile de
faire une page différente, on peut juste changer l'apparence de la petite boule
en mode écoute ou plus simple mettre le texte listening puis reflexion. On
supprime radio, on supprime timer. Page 2 je veux un mini btop like, uniquement
pour le cpu, ram, gpu (si existe), watt, quantité batterie, vitesse network,
état disque dur et ssd. »

## Ce qui existe maintenant

    PAGE_MAITRE (0)  ACCUEIL : heure, date, météo, watts, le compagnon.
                     Pendant une conversation, l'accueil RESTE affiché : le
                     compagnon passe en grand et le texte d'état s'affiche
                     (ECOUTE / REFLEXION / JE PARLE).
    PAGE_SYS    (1)  SYSTEM : le mini btop.

Plus de pages PARLER, RADIO ni TIMER. Le moteur radio et le moteur de timers
restent dans le code (ils ne sont simplement plus accessibles par l'écran) :
rien n'a été détruit, /radio et /timer répondent toujours.

## La page SYSTEM

| ligne | contenu | source |
|-------|---------|--------|
| en-tête | SYSTEM + uptime | esp_timer |
| CPU | % + fréquence + graphe 38 colonnes (historique) | temps d'exécution par tâche (FreeRTOS) |
| RAM | libres / barre | heap_caps, caps interne |
| PSRAM | libres / barre | heap_caps, caps SPIRAM |
| GPU | AUCUN (l'ESP32-S3 n'en a pas) | — |
| W | consommation estimée | conso_mw() |
| BATTERIE | % + tension, ou USB si aucune batterie | ADC1 canal 3 = GPIO4, pont 200K/200K, VBAT = 2 x VADC |
| RESEAU | octets/s montants et descendants | compteurs de l'application (http_get + envoi STT) |
| SD | occupé / total + barre | esp_vfs_fat_info("/sdcard") |
| FLASH | SPIFFS occupé / total + barre | esp_spiffs_info("storage") |
| pied | adresse IP | netif_default |

## Cadence

Une image de cette page toutes les 5 s (573 ms de clignotement à chaque image,
c'est le plancher du matériel). Les mesures, elles, sont prises chaque seconde.

## Pièges payés

- **Compter le CPU avec un hook idle ne marche pas** à 100 Hz de tick : le hook
  n'est pas appelé assez souvent et le pourcentage sort faux (93 % sur un CPU
  au repos). Il faut `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` +
  `CONFIG_FREERTOS_USE_TRACE_FACILITY` et `uxTaskGetSystemState()`.
- **La somme des temps de tâche inclut les tâches IDLE** : sans les exclure, le
  total vaut toujours 100 % des deux cœurs. Exclure les noms qui commencent par
  "IDLE".
- Le pourcentage réel au repos est ~0,7 % : arrondi à l'entier il affiche 0,
  donc on garde un dixième (`cpu_pct10`).
- **ESP-IDF n'expose pas les compteurs de l'interface Wi-Fi** (pas de MIB2 dans
  son lwIP, `mib2_counters` n'existe pas). On compte donc les octets applicatifs
  (http_get, envoi STT) — c'est ce qui circule vraiment pour la voix.
