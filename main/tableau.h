/*  tableau.h - LA page unique : tout ce que le robot sait, sur un seul ecran.
 *
 *  Le firmware n'a plus de cycle de pages : une seule image de 200x200 porte
 *  l'horloge, le ciel, le compagnon, la batterie, le banc systeme, l'agenda,
 *  le dernier mot, le conseil, YouTube et le livre en cours.
 *
 *  Pur C++ : ne depend que de art.h (aucun include ESP-IDF), donc le meme code
 *  se dessine sur la carte et dans l'apercu PNG du PC (tools/apercu_tableau.cpp).
 */
#ifndef TABLEAU_H_INCLUDED
#define TABLEAU_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#define TAB_SSID_MAX   32
#define TAB_TXT_MAX    96

typedef struct {
    /* --- horloge --- */
    int  heure, minute;           /* 18, 54 */
    int  jour, mois;              /* 11, 11 */
    int  sem;                     /* 0 = dimanche ... 6 = samedi */

    /* --- reseau --- */
    bool wifi;
    char ssid[TAB_SSID_MAX];
    int  rssi;                    /* dBm, 0 = inconnu */

    /* --- energie --- */
    int  bat_pct;                 /* -1 = pas de batterie (USB) */
    int  conso_mw;                /* -1 = inconnu */
    int  uptime_min;

    /* --- capteur interne (SHTC3) --- */
    bool  capteur;                /* false : on ecrit N.D. */
    float temp_int, humi;

    /* --- meteo (Open-Meteo, par le reseau) --- */
    bool  meteo;
    float meteo_c;
    char  meteo_txt[24];
    int   dem_min, dem_max, dem_pluie;   /* -999 = inconnu */

    /* --- compagnon --- */
    const char *stade, *forme, *emotion;
    int  faim, bonheur, energie, sante;  /* 0..100, -1 = inconnu */
    int  age_j;

    /* --- banc systeme --- */
    int      cpu_pct;
    unsigned ram_libre_k;
    unsigned ram_tot_k;

    /* --- agenda --- */
    char rdv_quand[24];           /* "DANS 45 MIN", "DEMAIN 09:00", "" = rien */
    char rdv_qui[28];
    int  rappels;                 /* minuteurs en cours */

    /* --- ce qu'il a dit en dernier / son conseil --- */
    char dernier[TAB_TXT_MAX];
    char conseil[TAB_TXT_MAX];

    /* --- youtube --- */
    char yt_chaine[TAB_SSID_MAX];
    char yt_video[TAB_TXT_MAX];
    unsigned long yt_vues;

    /* --- livre en cours --- */
    char livre[28];
    int  livre_p, livre_n;

    /* --- etat --- */
    bool alarme;
    int  visage;                  /* 0 joie, 1 neutre, 2 peine, 3 endormi */
} tableau_t;

/* dessine la page entiere dans le framebuffer courant (art_begin deja fait) */
void tableau_dessine(const tableau_t *t);

/* largeur en pixels d'un texte de la police 3x5 (utilitaire d'alignement) */
int tab_text_w(int s, const char *str);

#endif /* TABLEAU_H_INCLUDED */
