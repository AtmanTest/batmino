# BATMino — le robot e-paper (Waveshare ESP32-S3-ePaper-1.54)

Carte : ESP32-S3, 8 Mo PSRAM, écran e-paper 200x200 monochrome, micro + ES8311,
carte micro SD 128 Go. MAC `70:04:1d:d7:aa:28`, port série `/dev/ttyACM0`
(USB-Serial/JTAG). Ce dossier est la copie de travail : la copie d'origine
`/home/taz/robot-ia` appartient à root.

## Ce qu'il est devenu

Il s'appelait Mino, un cochon. C'est maintenant **BATMino** : une petite
chauve-souris à cagoule et cape, qui vit une vie de tamagotchi dans une ville
en noir et blanc, et qui parle.

## Les fichiers

    main/main.cpp        firmware : WiFi/API, audio, écran, boutons, menu, tâches
    main/art.h/.cpp      le personnage 1 bit (73 expressions, 14 familles) + primitives
    main/pet.h/.cpp      le moteur du tamagotchi : jauges, évolution, maladie, mort
    main/petart.h/.cpp   le monde (ville, bat-signal, sol), le sprite qui évolue, le bandeau
    tools/preview.cpp    aperçu PNG sur le PC : le même code de dessin que la carte
    tools/pettest.cpp    vérifie le moteur en accéléré (jours simulés en secondes)
    tools/talk.py        dialogue série avec la carte
    tools/ecran.py       photo de l'écran réel -> PNG (attend que la carte soit prête)
    tools/ascii_fb.py    le framebuffer de la carte en ASCII, pour trancher un doute

Le rendu est **partagé** : `art.cpp`/`petart.cpp` se compilent côté carte et côté
PC. Ce qu'on voit dans `./preview` est exactement ce que l'e-paper affiche.

## Interface : deux boutons, quatre gestes

    APPUI COURT bouton 1 : lance la tache de l'ecran courant
                           (accueil/parler -> chat vocal, radio -> jouer/arreter,
                            minuteur -> creer/valider)
    APPUI COURT bouton 2 : abandonne la tache en cours (l'ecoute est coupee
                           proprement) et passe a la page suivante ; sur la
                           derniere page, retour a l'accueil
    APPUI LONG           : reserve, pas encore d'action
    DOUBLE APPUI         : detecte (< 400 ms) et reserve pour la suite, sans
                           rejouer l'action de l'appui simple

Testable a distance : `/bouton?n=0|1&m=court|long|double`.

Pages : MONDE (0, le tamagotchi en train de vivre), VISAGE (1, la tête en grand
avec les 73 expressions), HORLOGE, CAPTEURS, RÉGLAGES, SÉCU, JOURNAL, AIDE.
Le menu s'affiche par-dessus le monde : NOURRIR, JOUER, CALIN, NETTOYER,
SOIGNER, DORMIR, PARLER, PAGES, AIDE — et il s'ouvre directement sur ce dont
le personnage a besoin.

## Le tamagotchi (moteur local, déterministe)

Le LLM ne touche jamais une jauge : la vie du personnage est calculée sur la
carte, seconde par seconde (`pet_tick`), et elle survit aux coupures.

    œuf 20 s -> bébé 20 min -> enfant 3 h -> ado 3 h -> adulte 4-10 j
    -> vieillard ~2 j -> départ, puis génération suivante

* Quatre jauges en points (100 = 1 cœur, 400 = 4 cœurs) : faim, bonheur,
  énergie, propreté. Plus l'obéissance (0-100).
* `care_mistakes` monte quand un cœur reste vide plus de 15 min. C'est lui qui
  décide de la forme adulte : héros, normal, grognon ou maladif.
* Le caca arrive après les repas et salit ; la maladie vient de la faim, du
  caca ou de la saleté prolongés ; non soignée, elle emporte le personnage.
* La nuit (22 h - 7 h), il dort ; il se balade tout seul (un pas toutes les
  2-4 s), et la page MONDE ne se redessine **que s'il a vraiment bougé** :
  l'e-paper met plusieurs secondes à se rafraîchir.

Vérifié hors carte : `tools/pettest.cpp` rejoue des jours en secondes —
avec des soins parfaits le personnage survit et devient héros, sans soin il
part en 12 h avec 8 fautes de soin, et la sauvegarde fait un aller-retour.

## La carte SD (128 Go), partitionnee

La carte porte maintenant deux partitions (ecrites par le firmware, `/part`) :

    p1  FAT32   93,1 Go   LBA 2048        montee sur /sdcard
    p2  brute   23,9 Go   LBA 195313664   zone d'echange, hors FAT

Fichiers sur la partition FAT :

    /sdcard/mino/pet.txt        sauvegarde de l'état (reprise après coupure)
    /sdcard/mino/journal-*.md   journal du jour, une ligne par événement
    /sdcard/mino/soul.md        la personnalité, modifiable depuis le PC
    /sdcard/mino/memory.json    notes durables apprises
    /sdcard/voix.wav            dernière prise du micro
    /sdcard/backup/*.tar.gz     sauvegardes completes (source + git bundle)

Debits mesures sur cette carte (bus 1 bit, 40 MHz — seule cette largeur est
cablee sur la Waveshare) :

    FATFS (fichiers)                  0,37 Mo/s ecriture | 1,6 Mo/s lecture
    zone brute, tampon relais 16 Ko   3,4 Mo/s ecriture  | 3,5 Mo/s lecture
    zone brute, tampon 64-256 Ko PSRAM 0,4-0,6 Mo/s (le tampon PSRAM casse le DMA)

La taille de bloc FATFS ne change rien (0,37 Mo/s de 4 a 256 Ko) : le plafond
vient de FATFS, pas de la carte ni du bus (4,65 Mo/s en secteurs bruts).

Cette zone n'est **pas un swap systeme** : l'ESP32-S3 n'a pas de MMU, donc
aucune pagination. C'est un stockage explicite, utilise par `swap_ecrire()` /
`swap_lire()` (blocs de 16 Ko, memcpy PSRAM <-> carte).

    /part?op=info        etat des partitions
    /part?op=repart&fat_go=100   repartitionne (DESTRUCTIF) puis formate la FAT32
    /part?op=restore     retour a une carte entiere en FAT32
    /part?op=st&n=4      ecrit puis relit n Mo dans la zone d'echange (verifie)
    /part?op=rawbench&bloc=K     debit brut de la zone

La flash interne (SPIFFS, 1,5 Mo) ne sert plus que de secours : tout ce qui
compte vit sur la carte.

## L'assistant vocal

Tout se passe sur la carte, sans PC : micro -> WAV en PSRAM -> Groq
(whisper-large-v3-turbo) -> texte -> DeepSeek/Groq (chat) -> reponse -> TTS
(Google Traduction) -> haut-parleur. Le modele et les cles se changent par
`/cle` et `/modele` (stockes en NVS, pas dans le binaire).

    appui court bouton 1 sur l'accueil -> "PARLE MAINTENANT" (le micro s'ouvre,
    l'enregistrement s'arrete seul apres 1 s de silence, max 6 s)
      -> "JE REFLECHIS" -> la reponse s'affiche et se dit
    appui court bouton 2 -> l'ecoute est coupee proprement, page suivante

Si le Wi-Fi manque, la carte le dit a l'ecran au lieu d'attendre.

### Le cerveau (maj 2026-10-19)

* Modele : **deepseek-flash** = DeepSeek V4.1 Flash (l'ancien `deepseek-chat`
  donnait des reponses pauvres). Reglable par `/modele?m=deepseek-flash`.
* Style : le prompt impose UNE phrase, 20 mots maximum, pas de politesses, pas
  de rappel de la question, pas de description de soi. `max_tokens` 500.
* Contexte injecte a chaque appel : date + heure en francais, meteo
  maintenant, meteo de demain (min/max, pluie), et les 8 titres du moment
  (franceinfo, rafraichis toutes les 30 min). Le modele ne repond donc plus
  avec sa memoire d'entrainement quand on lui demande le jour ou l'actu.
* Web : si le modele ne sait pas, il rend `{"cherche":"2 a 6 mots"}` ; la carte
  interroge alors **Wikipedia** (`generator=search` + `extracts`, 2 articles)
  et **Google News RSS** (4 titres, seulement si la question est une actu), puis
  redemande une reponse courte avec ces resultats. Pas de cle d'API.
  DuckDuckGo/Bing/Mojeek sont inutilisables depuis l'ESP (captcha/202).
* Cout : ~2-4 s pour une question simple, ~10-20 s quand il faut chercher.
* Echec de recherche : le modele repond quand meme en une phrase, il ne dit
  plus "je ne trouve rien".

### Wi-Fi : deux bugs corriges

* Un mot de passe de 1 caractere etait enregistre en NVS (saisie ratee dans
  `/reglages`) : le handshake echouait en boucle avec la raison 15. Un mot de
  passe enregistre de moins de 8 caracteres est maintenant ignore.
* `caca` (LAN isole, 192.168.1.x) passait en secours AVANT `MON_RESEAU_SECOURS_2` : le
  robot y devenait injoignable depuis le PC. Il est desormais en dernier, et
  on attend 2,5 s entre deux tentatives (la borne rejette les reconnexions
  trop rapides).

L'ancien cerveau PC (`mino-cerveau`) n'est plus necessaire ; s'il tourne encore
il faut l'arreter, il tient le port serie :

    systemctl --user stop mino-cerveau

## Anti-ghosting

Le pilote ne rechargeait jamais la seconde RAM (0x26) ni la LUT pleine, donc
l'ancienne image restait collée. Ajouté : `EPD_ShowFull`, `EPD_ShowBase`,
`EPD_ShowFullClean` (LUT pleine + 0x24/0x26), image blanche au démarrage,
et dans `show_ui` un rafraîchissement complet à chaque changement de page et
tous les 10 rafraîchissements partiels.

## Compiler, flasher, regarder

    cd /home/taz/mino-fw
    export IDF_TOOLS_PATH=/opt/espressif-tools && source /opt/esp-idf/export.sh
    idf.py build
    idf.py -p /dev/ttyACM0 flash

    cd tools
    g++ -O2 -o preview preview.cpp ../main/art.cpp ../main/petart.cpp ../main/pet.cpp -lm -lz
    ./preview monde            # montage 4x3 : œuf, bébé, enfant, ado, adulte...
    ./preview pet 4 0          # une scène en grand (4 = adulte, 0 = héros)
    ./preview sel colere,peur,surprise,degout,joie,tristesse,neutre,sourire_en_coin,reflexion,curiosite,fierte,douleur sel3.png

    systemctl --user stop mino-cerveau
    /opt/mino-venv/bin/python talk.py --fb ecran_reel.png     # l'écran réel
    /opt/mino-venv/bin/python talk.py state                   # état complet
    systemctl --user start mino-cerveau

Ou en un seul geste (recommandé) :

    systemctl --user stop mino-cerveau
    /opt/mino-venv/bin/python ecran.py ecran.png "ui 0" "menu 4"
    /opt/mino-venv/bin/python ascii_fb.py                     # le framebuffer en ASCII
    systemctl --user start mino-cerveau

Attention : **ouvrir le port série redémarre la carte** (DTR/RTS du
USB-Serial/JTAG). Les outils ci-dessus baissent DTR/RTS puis attendent que la
carte réponde avant d'envoyer une commande — c'est indispensable, sinon la
commande part pendant le démarrage et personne ne répond.

## API HTTP de la carte

    /state                 état complet (stade, jauges, cpu, forme)
    /face?e=<famille|expression>   visage (familles : colere, peur, joie...)
    /pet?a=manger|jouer|calin|nettoyer|soigner|dormir|reveiller
    /say?emotion=<famille>&text=... texte + visage
    /soul, /note, /rec, /play, /sd

## Ce qui reste

* WiFi `MON_RESEAU_SECOURS` : corrige (mot de passe NVS pourri + ordre des
  secours), le robot vit sur 192.168.8.x sans PC.
* Le capteur de température lit la chaleur de la carte : le décalage
  (`offset`) est à refaire pour ce boîtier.
* Pistes non faites : reconnaissance de mots pour réveiller sans bouton,
  plusieurs personnages sur la carte SD, jeux à deux boutons.
