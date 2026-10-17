---
name: batmino-esp32-robot
description: "Build, flash and bench the BATMino ESP32-S3 robot."
---

# BATMino — robot tamagotchi ESP32-S3 e-paper

Carte : Waveshare ESP32-S3-ePaper-1.54 (ESP32-S3-PICO-1-N8R8, 8 Mo flash, 8 Mo PSRAM octal 80 MHz, e-paper 200x200, micro + ES8311). Firmware autonome (voix Wi-Fi : STT Groq / LLM Groq-DeepSeek / TTS Google, tamagotchi local, API HTTP).

## Emplacements

    /home/taz/mino-fw        firmware (git, README-BATMINO.md, main/main.cpp ~200 Ko)
    /home/taz/mino-bench     projet bench mem/flash/SD (main/bench.c)
    /home/taz/mino-app       appli PC annexe (soul-batmino.md...)
    /home/taz/mino-fw/build/mino.bin    app image a reflasher a 0x10000 (sauvegarde de secours)

Partitions (identiques dans les deux projets) : nvs 0x9000, phy 0xf000, factory app 0x10000 3 Mo, spiffs 0x310000 1 Mo.

## Build / flash

    export IDF_TOOLS_PATH=/opt/espressif-tools && . /opt/esp-idf/export.sh
    idf.py build
    idf.py -p /dev/ttyACM0 -b 460800 flash

L'utilisateur `taz` est dans le groupe `uucp` : pas besoin de sudo pour ttyACM0.

## Lire le port serie (le piege)

`cat /dev/ttyACM0` ne renvoie RIEN si la ligne n'est pas configuree. Il faut `stty -F /dev/ttyACM0 115200 raw -echo` avant, plus un reset pour capturer le boot (esptool `--no-stub --after hard_reset chip_id` en arriere-plan, puis stty+cat). Script pret : `scripts/capture-serial.sh <secondes>`. USB-Serial/JTAG = CDC, le baud est ignore.

Apres un flash `idf.py`, la sortie de boot est deja passee : sans reset on ne capture que la boucle finale.

## Pieges deja payes

- **Firmware et bench compiles en Debug (`-Og`) par defaut** : `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`. Pour mesurer du materiel, passer en `CONFIG_COMPILER_OPTIMIZATION_PERF=y` ; c'est aussi le premier levier de perf reel sur mino-fw (pas encore applique la).
- `fsync(fileno(f))` existe (VFS ESP-IDF, via `unistd.h`) ; `f_sync()` attend un `FIL*` (FATFS brut), pas un `FILE*`.
- `build/` herite d'un build interrompu = `objdump: returned non-zero exit status 1` sur une lib : `rm -rf build` puis rebuild.
- Bug bench corrige : `bench_latence(a, 64*1024, ...)` sur des buffers de 32 Ko ecrasait le tas -> `Guru Meditation StoreProhibited` dans `free()`. Toujours passer la taille REELLE du buffer.
- Le pointer-chase de latence est elimine par `-O2` (0.1 ns mesure) : lire via un pointeur `volatile` ou une somme dependante non optimisable.

## Mesures de reference (17/10/2026, CPU 160 MHz, PSRAM octal 80 MHz, -O2)

    SRAM interne   memcpy 243 Mo/s | memset 486 Mo/s
    PSRAM 8 Mo     memcpy  21 Mo/s | memset 470 Mo/s (douteux, > debit du bus)
    flash SPI QIO 80 MHz hors cache : lecture 25 Mo/s
    SD (SDMMC 1 bit, FATFS) : ecriture 0,35 Mo/s | lecture 1,2 Mo/s quel que
       soit le bloc (4-64 Ko) ; 4 Ko + fsync = 8,1 ms ; lecture 4 Ko dispersee 0,23 ms
    SD secteurs BRUTS (sans FATFS), 1 bit : 20 MHz -> 2,34 Mo/s ecriture et lecture ;
       40 MHz -> 4,7 Mo/s (doublement exact, la carte tient 40 MHz) ; 4 Ko brut = 2,3 ms

Diagnostic SD : le debit brut colle au maximum theorique du bus (20 MHz x 1 bit
= 2,5 Mo/s), donc la carte et le cablage ne sont pas le probleme. L'ecart
0,35 vs 2,34 Mo/s vient de FATFS/VFS (facteur ~7 en ecriture, ~2 en lecture).
Deux leviers mesures : `host.max_freq_khz = SDMMC_FREQ_HIGHSPEED` (40 MHz,
+100 % brut) et ecriture par gros blocs / secteurs bruts plutot que fwrite
petit. La carte Waveshare ne cable que CLK/CMD/D0 : le 4 bits est impossible.

Consequences : PSRAM bonne seulement en gros buffers sequentiels (memcpy 21 Mo/s) ; aucun streaming sur la SD (0,35 Mo/s tue l'enregistrement audio) ; gros rodata depuis la flash a 25 Mo/s (canon.pcm 1,9 Mo = ~76 ms).

## API de transfert de fichiers (sauvegarde sur la micro SD)

Depuis v0.3, sans lecteur de carte on passe par le Wi-Fi :

    curl -X POST --data-binary @fichier.tar.gz \
         "http://192.168.8.208/fichier?p=/backup/fichier.tar.gz"   # ecrit sur la SD
    curl "http://192.168.8.208/fichier?p=/backup/fichier.tar.gz" -o copie  # relit

Mesure : 7,3 Mo en 15 s (~0,47 Mo/s, coherent avec les 0,35 Mo/s FATFS).
Toujours verifier par `cmp` apres relecture : l'ecriture SD n'est pas fiable a 100 %.

Piege FATFS : avec `CONFIG_FATFS_LFN_NONE=y` (defaut du projet), fopen echoue
en "ouverture" des qu'un nom a une extension de plus de 3 caracteres ou deux
points (`x.tar.gz`). Passe en `CONFIG_FATFS_LFN_HEAP=y` + `CONFIG_FATFS_CODEPAGE_850=y`.
Le listing `/sd` reste illisible (octets bruts dans les noms) : bug de
h_sd_list, pas un probleme de nommage.

## Depot git

    remote : https://github.com/AtmanTest/batmino.git (branche main, tags v0.2/v0.3)

Le token fine-grained est enregistre dans `~/.git-credentials` (helper `store`,
chmod 600) ET colle en premiere ligne de `LIRE-D-ABORD.md` sur la cle MINO-ESP32.
Ne jamais repondre "il n'y a pas de token" sans avoir teste : `curl -H "Authorization: Bearer <t>" https://api.github.com/user`
renvoie 401 pour un token revoque, ce qui distingue "absent" de "mort".

## Boutons (v0.4)

    bouton 1 (BOOT, GPIO0) appui court : lance la tache de la page
        accueil/parler -> chat vocal ; radio -> jouer/arreter ; timer -> creer/valider
    bouton 2 (PWR, GPIO18) appui court : quitte la tache et revient a l'accueil,
        sinon page suivante ; en edition timer : +1 min
    double appui (< 400 ms) : detecte, reserve (bouton_double) ; appui long : inchange

L'abandon passe par `assist_abort` (volatile) verifie dans la boucle d'ecoute de
assistant_wake_task : c'est le seul moyen propre de sortir d'une tache de 6 s.
Test a distance : `/bouton?n=0|1&m=court|long|double`, `/bouton?p=0..3`.

## Debit SD : ou est le vrai plafond

- 4, 16, 64, 256 Ko de bloc via FATFS : 0,37 Mo/s ecriture / 1,6 Mo/s lecture,
  identique => la taille de bloc ne change rien.
- secteurs bruts (sdmmc_write_sectors) : 4,65 / 4,70 Mo/s a 40 MHz.
- => le plafond est FATFS + chemin d'ecriture, pas la carte ni le bus.
- Pas de swap possible : ESP32-S3 sans MMU, aucune pagination. Pour utiliser
  vraiment la carte comme memoire (4,6 Mo/s), il faut une zone BRUTE hors FAT32
  (repartition : FAT32 + region reservee, acces par sdmmc_*_sectors).
- Endpoints ajoutes : `/swap` (etat + `op=bench&bloc=K&mo=N`), `/sdformat?f=1`
  (reformate vraiment), `/sd?p=/dossier` (listing JSON nom/taille/lisible).

## Laura, et la radio desactivee (v0.8)

L'assistante s'appelle **Laura**. Son SOUL vit sur la carte
(`/sdcard/mino/soul.md`, ~1,6 Ko) et `DEFAULT_SOUL` sert de repli : les deux
parlent d'une adulte experte, plus d'un enfant de six ans. Le prompt systeme
n'envoie que `%.1500s` du SOUL — un SOUL plus long est tronque en silence (c'est
ce qui a fait disparaitre la consigne radio).

Module radio : `radio_actif` (NVS, cle `radio`, defaut desactive). Garde dans
`radio_joue()`, interrupteur `/radio?actif=1|0`, et le prompt perd la liste des
stations + le champ `radio` du JSON quand c'est coupe. Sans cette consigne
explicite, le modele inventait des actions (« je lance la radio… »).

## Conversation continue, clone et historique (v0.7)

`assistant_wake_task()` boucle : un appui lance le 1er tour, puis le micro se
reouvre seul `CONV_FENETRE_MS` (10 s) apres chaque reponse ; silence = fin,
`CONV_MAX_TOURS` (10) borne la boucle. Le bouton 2 (`assist_abort`) coupe tout.

Historique : `main/historique.h` (header seul, compile cote carte ET cote PC).
`hist_t` garde `HIST_MAX` 12 tours de `HIST_TXT_SZ` 320 octets, envoyes au modele
en vrais messages `user`/`assistant`. `/state` expose `historique_tours`.

Clone de Taz : `/sdcard/mino/clone.md`, relu au demarrage (`CLONE lu sur la
carte SD`), injecte dans le prompt avec le role (expert IT, IA, technicien
superieur, developpeur, coach de vie, reflechir avant d'agir). Depose par
`POST /fichier?p=/mino/clone.md` — pas besoin de reflasher.

Reponses longues : `max_tokens` 700, temperature 0,7, prompt impose 3-6 phrases.
`tts_parle()` decoupe en morceaux de 180 caracteres (limite Google) coupes sur
la ponctuation et appelle `tts_parle_un()` — sinon la voix s'arretait a 190
caracteres.

Tests (`bash tests/run_unit.sh`, `bash tests/api.sh`) : 12 unitaires + 13
d'integration. Le test `zone d'echange` attrape un piege reel : le tampon relais
16 Ko doit etre alloue UNE fois (`swap_tampon()`), sinon l'allocation echoue des
que le plus gros bloc interne passe sous 16 Ko.

Sauvegarde : `~/mino-backup-usb.sh` + timer systemd utilisateur
`mino-backup.timer` (30 min, OnUnitActiveSec), archive datee dans
`/run/media/taz/MINO-ESP32/backup`, journal `~/mino-backup.log`. Le cron Hermes
n'est pas fiable ici : il exige la passerelle locale demarree.

## RAM interne : le piege qui casse la voix (v0.6)

Symptomes : le bouton ne fait rien, ou "je n'ai pas bien compris", ou TTS -1.
Journal : `mbedtls_ssl_handshake returned -0x3000` / `mbedtls_ssl_setup
returned -0x7F00` = MBEDTLS_ERR_SSL_ALLOC_FAILED (PAS un probleme de certificat,
ne pas desactiver la verification).

Cause : RAM interne saturee (~11 Ko libres, plus gros bloc 4 Ko). Le serveur
HTTP reservait 7 sockets x 16 Ko de pile = 112 Ko, et `xTaskCreate(16384)` du
reveil echouait en SILENCE (retour non teste -> bouton muet).

Regles a garder :
- `cfg.max_open_sockets = 2` et pile httpd 12 Ko (TLS tient dedans).
- piles de taches lourdes en PSRAM : `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`
  + `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`, et TOUJOURS tester le retour.
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` (pas 16384) : les tampons TLS de
  16 Ko peuvent ainsi aller en PSRAM.
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` et `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`.
- Diagnostiquer avec `/state` : `heap_interne`, `heap_interne_max`, `heap_dma_max`
  (sain : >30 Ko libres, plus gros bloc >16 Ko).
- Tester cerveau+voix sans parler : `/dit?t=texte` (ni micro ni STT).

## Carte repartitionnee (v0.5) : FAT32 93 Go + zone brute 23,9 Go

Etat actuel de la carte : MBR 2 partitions, p1 = FAT32 (0x0C, LBA 2048, 93 Go,
montee sur /sdcard), p2 = brute (0xDA, LBA 195313664, 23,9 Go).

    /part?op=info                      etat des partitions (lu sur la carte)
    /part?op=repart&fat_go=100         repartitionne + formate (DESTRUCTIF)
    /part?op=restore                   retour a une carte entiere en FAT32
    /part?op=rawtest | rawbench&bloc=K verif et debit de la zone brute
    /part?op=st&n=4                    ecrit/relit n Mo via swap_ecrire/swap_lire

Le MBR et le FAT32 sont ecrits A LA MAIN : f_mkfs ne formate qu'un disque entier,
jamais une partition (il recreerait un MBR pleine carte). Formule FAT32 utilisee :
reserved 32, 2 FAT, clusters 32 Ko, root cluster 2, FSInfo + backup boot.

Piege paye : rouvrir la carte avec sdmmc_card_init sans rappeler host->init() ni
sdmmc_host_init_slot() => panic au premier acces. Pour ecrire des secteurs bruts,
reutiliser le handle du montage en cours (sd_card) puis demonter/remonter.

Debits mesures : FATFS 0,37 Mo/s ecriture / 1,6 lecture ; zone brute via tampon
relais 16 Ko en SRAM interne : 3,4 Mo/s ecriture / 3,5 lecture (verifie).
Un tampon de 64-256 Ko en PSRAM fait CHUTER le debit a 0,4-0,6 Mo/s.

Ce n'est PAS un swap OS : pas de MMU sur ESP32-S3, aucune pagination. C'est un
stockage explicite (memcpy PSRAM <-> zone brute).

## Restaurer le firmware apres un flash de bench

    cd /home/taz/mino-fw && python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
        write_flash 0x10000 build/mino.bin

Verifier au serie : `mino: SOUL lu sur la carte SD`, `carte SD prete : /sdcard/mino`.
