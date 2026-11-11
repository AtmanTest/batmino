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
    t->rssi = -52;
    t->bat_pct = 36; t->conso_mw = 270;
    t->capteur = true; t->temp_int = 24.6f; t->humi = 41.0f;
    t->meteo = true; t->meteo_c = 13.6f;
    snprintf(t->meteo_txt, sizeof(t->meteo_txt), "NUAGEUX");
    t->dem_min = 9; t->dem_max = 14; t->dem_pluie = 20;
    t->faim = 100; t->bonheur = 82; t->energie = 64; t->sante = 38;
    snprintf(t->rdv_quand, sizeof(t->rdv_quand), "DEMAIN 09:00");
    snprintf(t->rdv_qui, sizeof(t->rdv_qui), "DENTISTE");
    snprintf(t->dernier, sizeof(t->dernier),
             "Tu as dit que tu voulais ranger l'atelier ce week-end.");
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
    VERIF(encre_bande(0, 36) > 300, "en-tete : heure, date et visage dessines");
    VERIF(encre_bande(40, 88) > 150, "bloc ciel : meteo, demain, interieur");
    VERIF(encre_bande(92, 114) > 100, "bloc batterie");
    VERIF(encre_bande(118, 154) > 150, "les quatre jauges du compagnon");
    VERIF(encre_bande(158, 172) > 30, "ligne agenda");
    VERIF(encre_bande(176, 199) > 60, "le dernier mot");

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
        plein(&v); v.faim = 10; v.bonheur = 100;                   rend(&v);
        VERIF(memcmp(ref, fb, sizeof(ref)) != 0, "changer les jauges change l'image");
        plein(&v); v.dernier[0] = 0;                               rend(&v);
        VERIF(memcmp(ref, fb, sizeof(ref)) != 0, "perdre le dernier mot change l'image");
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
        v.faim = v.bonheur = v.energie = v.sante = -1;
        rend(&v);
        VERIF(encre() > 800, "la page reste dessinee sans aucune donnee");
        /* les jauges inconnues ne dessinent pas de barre : le cadre de la
           jauge FAIM (x 48..90, y 120..134) doit rester vide */
        int dedans = 0;
        for (int y = 121; y <= 133; y++)
            for (int x = 55; x <= 89; x++)
                if (!((fb[y * 25 + (x >> 3)] >> (7 - (x & 7))) & 1)) dedans++;
        VERIF(dedans == 0, "jauge n.d. : aucune barre dessinee");
    }

    /* ---- 6. debordement de texte : rien ne sort du cadre ---- */
    {
        tableau_t v;
        plein(&v);
        memset(v.dernier, 'B', sizeof(v.dernier) - 1);   /* 95 signes */
        memset(v.rdv_qui, 'D', sizeof(v.rdv_qui) - 1);
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
