# BATMino — robot tamagotchi e-paper (ESP32-S3)

Firmware autonome pour la carte **Waveshare ESP32-S3-ePaper-1.54** (ESP32-S3,
8 Mo flash, 8 Mo PSRAM, e-paper 200x200, micro + ES8311) : un personnage 1 bit
qui vit sa vie (tamagotchi déterministe calculé sur la carte), parle, écoute et
répond en Wi-Fi — micro → STT (Groq whisper) → LLM (Groq / DeepSeek) → TTS
(Google Traduction), le tout sur la carte, sans PC.

Deux boutons : appui court = menu / page suivante, appui long 2 s = parler.

## Démarrer

```bash
cp main/secrets.h.example main/secrets.h   # puis remplir Wi-Fi + cles
export IDF_TOOLS_PATH=/opt/espressif-tools && . /opt/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 -b 460800 flash
```

ESP-IDF 5.5.1, cible `esp32s3`, partition `factory` 3 Mo + SPIFFS 1 Mo
(`partitions.csv`). `main/secrets.h` n'est **jamais** versionné : il contient le
mot de passe Wi-Fi et les clés d'API.

## Documents

* `README-BATMINO.md` — le personnage, le moteur tamagotchi, l'écran, l'API.
* `main/user_config.h` — broches, URLs des services, ville de la météo.
* `tools/` — outillage PC : `preview.cpp` (même code de dessin, rendu PNG),
  `pettest.cpp` (rejoue des jours en secondes), `talk.py`, `ecran.py`,
  `ascii_fb.py`.

```bash
cd tools
g++ -O2 -o preview preview.cpp ../main/art.cpp ../main/petart.cpp ../main/pet.cpp -lm -lz
./preview monde
```

## API HTTP de la carte

```
/state            etat complet (JSON)
/face?e=<famille> 14 familles d'expression
/say?emotion=<famille>&text=...
/pet?a=manger|jouer|calin|nettoyer|soigner|dormir|reveiller
/soul /note /rec /play /radio /infos /mic /tts /timer /minuteur /modele
/langue /cle /bouton /parle /sd /sdformat /spiffs /sdcard
```
