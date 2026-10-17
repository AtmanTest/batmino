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

## Interface : deux boutons, deux gestes

    APPUI COURT : bouton 1 = menu / élément suivant ; bouton 2 = valider / page
    APPUI LONG  : PARLER (assistant vocal), depuis n'importe quel écran

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

## La carte SD (128 Go)

    /sdcard/mino/pet.txt        sauvegarde de l'état (reprise après coupure)
    /sdcard/mino/journal-*.md   journal du jour, une ligne par événement
    /sdcard/mino/soul.md        la personnalité, modifiable depuis le PC
    /sdcard/mino/memory.json    notes durables apprises
    /sdcard/voix.wav            dernière prise du micro

La flash interne (SPIFFS, 1,5 Mo) ne sert plus que de secours : tout ce qui
compte vit sur la carte.

## L'assistant vocal

Le robot ne fait qu'une chose : appuyer 2 secondes, enregistrer, déposer
`/sdcard/voix.wav` et prévenir sur le câble. C'est le PC qui réfléchit.

    assistant 1. appui 2 s sur un bouton -> "PARLE MAINTENANT" (le micro s'ouvre,
                  l'enregistrement s'arrête seul après 1 s de silence, max 6 s)
               2. "JE REFLECHIS" -> whisper transcrit, DeepSeek répond,
                  edge-tts renvoie la voix dans le haut-parleur
               3. si aucun PC n'est branché : "BRANCHE MON CABLE USB !"

Côté PC, le cerveau est un service utilisateur qui se relance tout seul (c'est
l'ancien point de rupture : le câble qui bouge tuait `assistant.py`) :

    systemctl --user status mino-cerveau
    tail -f /home/taz/mino-app/cerveau.log
    bash /home/taz/mino-app/cerveau.sh      (à la main)

**Le port série est exclusif** : pour dumper l'écran avec `talk.py`, il faut
`systemctl --user stop mino-cerveau` d'abord, sinon le cerveau mange les
octets du dump.

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

* La WiFi ne s'accroche pas au réseau `MON_RESEAU_SECOURS` (le cerveau passe par
  l'USB, donc la voix marche quand même) ; à revoir : SSID/mot de passe.
* Le capteur de température lit la chaleur de la carte : le décalage
  (`offset`) est à refaire pour ce boîtier.
* Pistes non faites : reconnaissance de mots pour réveiller sans bouton,
  plusieurs personnages sur la carte SD, jeux à deux boutons.
