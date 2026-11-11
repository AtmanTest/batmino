/* Tests unitaires de la page unique (main/tableau.cpp), sur le PC.
   Le dessin est le vrai code du firmware : on verifie des invariants de
   rendu, pas des pixels a la main.

   bash tests/run_unit.sh  */
#include <stdio.h>
#include <string.h>

#include "art.h"
#include "tableau.h"

static int ok = 0, ko = 0;

#define VERIF(cond, nom) do {                       \
    if (cond) { ok++; printf("  ok   %s\n", nom); } \
    else      { ko++; printf("  KO   %s\n", nom); } \
} while (0)

static uint8_t fb[ART_FB_LEN];

/* nombre de pixels d'encre (bit a 0) */
static int encre(void)
{
    int n = 0;
    for (int i = 0; i < ART_FB_LEN; i++)
        for (int b = 0; b < 8; b++)
            if (!((fb[i] >> (7 - b)) & 1)) n++;
    return n;
}

/* encre dans une bande horizontale [y0..y1] */
static int encre_bande(int y0, int y1)
{
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = 0; x < ART_W; x++) {
            int bit = (fb[y * 25 + (x >> 3)] >> (7 - (x & 7))) & 1;
            if (!bit) n++;
        }
    return n;
}

static void plein(tableau_t *t)
{
    memset(t, 0, sizeof(*t));
    t->heure = 18; t->minute = 54; t->sem = 3; t->jour = 11; t->mois = 10;
    t->wifi = true;
    snprintf(t->ssid, sizeof(t->ssid), "MON_RESEAU_SECOURS");
    t->rssi = -52;
    t->bat_pct = 36; t->conso_mw = 270; t->uptime_min = 133;
    t->capteur = true; t->temp_int = 24.6f; t->humi = 41.0f;
    t->meteo = true; t->meteo_c = 13.6f;
    snprintf(t->meteo_txt, sizeof(t->meteo_txt), "NUAGEUX");
    t->dem_min = 9; t->dem_max = 14; t->dem_pluie = 20;
    t->stade = "ADULTE"; t->forme = "HEROS"; t->emotion = "calme"; t->age_j = 5;
    t->faim = 100; t->bonheur = 82; t->energie = 64; t->sante = 38;
    t->cpu_pct = 1; t->ram_libre_k = 228;
    snprintf(t->rdv_quand, sizeof(t->rdv_quand), "DEMAIN 09:00");
    snprintf(t->rdv_qui, sizeof(t->rdv_qui), "DENTISTE");
    snprintf(t->dernier, sizeof(t->dernier),
             "Tu as dit que tu voulais ranger l'atelier ce week-end.");
    snprintf(t->conseil, sizeof(t->conseil),
             "Batterie a 36, branche le chargeur avant ce soir.");
    snprintf(t->yt_chaine, sizeof(t->yt_chaine), "@youtube");
    snprintf(t->yt_video, sizeof(t->yt_video),
             "COMMENT CREER UN AQUARIUM PLANTE DE A A Z AVEC SERPADESIGN");
    t->yt_vues = 1370938UL;
    snprintf(t->livre, sizeof(t->livre), "FLEURS");
    t->livre_p = 80; t->livre_n = 456;
    t->visage = 1;
}

static void rend(const tableau_t *t)
{
    memset(fb, 0xFF, sizeof(fb));
    art_begin(fb, 0);
    art_clear();
    tableau_dessine(t);
}

int main(void)
{
    printf("tests unitaires : page unique (tableau)\n");
    art_tables_init();

    tableau_t t;
    plein(&t);

    /* ---- 1. la page est dense mais pas noire ---- */
    rend(&t);
    int e = encre();
    int pct = e * 100 / (ART_W * ART_H);
    VERIF(e > 1500, "la page porte du contenu");
    VERIF(pct < 45, "la page n'est pas saturee (moins de 45 % d'encre)");
    printf("       (encre : %d px, %d %%)\n", e, pct);

    /* ---- 2. chaque zone de la page est servie ---- */
    VERIF(encre_bande(0, 32) > 300, "en-tete : heure et visage dessines");
    VERIF(encre_bande(34, 45) > 30, "filet d'etat : reseau et uptime");
    VERIF(encre_bande(48, 77) > 100, "rangee 1 : meteo et demain");
    VERIF(encre_bande(80, 109) > 100, "rangee 2 : interieur et batterie");
    VERIF(encre_bande(112, 141) > 100, "rangee 3 : systeme et agenda");
    VERIF(encre_bande(144, 165) > 60, "rangee 4 : dernier mot et conseil");
    VERIF(encre_bande(168, 199) > 100, "pied : youtube, livre, jauges");

    /* ---- 3. le rendu est deterministe ---- */
    static uint8_t ref[ART_FB_LEN];
    memcpy(ref, fb, sizeof(ref));
    rend(&t);
    VERIF(memcmp(ref, fb, sizeof(ref)) == 0, "meme donnee = meme image");

    /* ---- 4. chaque donnee change vraiment l'image ---- */
    {
        tableau_t v;
        /* une mutation par donnee : chaque bloc de la page est branche */
        plein(&v); v.heure = 7; v.minute = 3;                      rend(&v);
        VERIF(memcmp(ref, fb, sizeof(ref)) != 0, "changer l'heure change l'image");
        plein(&v); v.bat_pct = 91;                                 rend(&v);
        VERIF(memcmp(ref, fb, sizeof(ref)) != 0, "changer la batterie change l'image");
        plein(&v); v.meteo = false; v.dem_min = -999;              rend(&v);
        VERIF(memcmp(ref, fb, sizeof(ref)) != 0, "perdre la meteo change l'image");
        plein(&v); v.rdv_quand[0] = 0; v.rdv_qui[0] = 0; v.rappels = 2; rend(&v);
        VERIF(memcmp(ref, fb, sizeof(ref)) != 0, "changer l'agenda change l'image");
        plein(&v); v.livre_n = 0;                                  rend(&v);
        VERIF(memcmp(ref, fb, sizeof(ref)) != 0, "perdre le livre change l'image");
    }

    /* ---- 5. donnees absentes : N.D. / --, jamais un zero ni un nom suppose ---- */
    {
        tableau_t v;
        memset(&v, 0, sizeof(v));
        v.heure = 7; v.minute = 3; v.sem = 3; v.jour = 11; v.mois = 10;
        v.wifi = false;
        v.bat_pct = -1; v.conso_mw = -1;
        v.capteur = false;
        v.meteo = false; v.dem_min = -999;
        v.stade = "N.D."; v.forme = ""; v.emotion = "N.D.";
        v.faim = v.bonheur = v.energie = v.sante = -1;
        v.age_j = -1;
        rend(&v);
        VERIF(encre() > 800, "la page reste dessinee sans aucune donnee");
        VERIF(encre_bande(34, 45) > 20, "hors ligne : le filet d'etat le dit");
        /* les jauges inconnues ne dessinent pas de barre pleine */
        int bas = encre_bande(188, 199);
        VERIF(bas < 400, "jauges n.d. : pas de barres pleines");
    }

    /* ---- 6. debordement de texte : rien ne sort du cadre ---- */
    {
        tableau_t v;
        plein(&v);
        memset(v.yt_video, 'A', sizeof(v.yt_video) - 1);   /* 95 signes */
        memset(v.dernier, 'B', sizeof(v.dernier) - 1);
        memset(v.conseil, 'C', sizeof(v.conseil) - 1);
        memset(v.rdv_qui, 'D', sizeof(v.rdv_qui) - 1);
        memset(v.ssid, 'E', sizeof(v.ssid) - 1);
        rend(&v);
        /* la derniere colonne de pixels reste blanche : rien n'a deborde */
        int bord = 0;
        for (int y = 0; y < ART_H; y++)
            if (!((fb[y * 25 + 24] >> 0) & 1)) bord++;
        VERIF(bord == 0, "un texte tres long ne deborde pas a droite");
    }

    printf("%d ok, %d ko\n", ok, ko);
    return ko ? 1 : 0;
}
