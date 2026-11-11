/*  tableau.cpp - la page unique du BATMino (200x200, 1 bit).
 *
 *  Version "gros caracteres" (11/11) : plus rien en corps 1. La plus petite
 *  taille de texte de la page est le corps 2 (glyphes de 6x10 px) : en dessous,
 *  c'est illisible sur une dalle de 200 px de cote.
 *
 *     0    heure (corps 7), date, visage du compagnon, etat du wifi
 *    38    filet
 *    42    ciel : temperature du moment (corps 4), etat, demain, interieur
 *    90    filet
 *    94    batterie : pourcentage (corps 4), jauge, consommation
 *   116    filet
 *   120    les quatre jauges du compagnon, en deux colonnes
 *   156    filet
 *   161    agenda du jour
 *   178    dernier mot (deux lignes de corps 2)
 *
 *  Regle : une valeur indisponible affiche N.D. ou --, jamais zero ni un nom
 *  suppose. Aucune dependance ESP-IDF : le meme code tourne sur la carte et
 *  dans l'apercu du PC (tools/apercu_tableau.cpp).
 */
#include "tableau.h"
#include "art.h"
#include <stdio.h>
#include <string.h>

int tab_text_w(int s, const char *str) { return a_text_w(s, str); }

/* ---------------------------------------------------------------- */
/* petits outils                                                     */
/* ---------------------------------------------------------------- */

static void txt(int x, int y, int s, const char *str) { a_text(x, y, s, str); }

static void txt_r(int xr, int y, int s, const char *str)
{
    a_text(xr - a_text_w(s, str), y, s, str);
}

/* un nombre a une decimale (13.6), sans passer par %f de newlib */
static const char *un_chiffre(char *b, size_t n, float v)
{
    bool neg = v < 0;
    float a = neg ? -v : v;
    int i = (int)a;
    int d = (int)((a - (float)i) * 10.0f + 0.5f);
    if (d > 9) d = 9;
    snprintf(b, n, "%s%d.%d", neg ? "-" : "", i, d);
    return b;
}

/* le signe pourcent : deux points et une barre. Le glyphe 3x5 ressemble a un Z
   des le corps 2, on le dessine donc a la main. */
static int pct_signe(int x, int y, int s)
{
    int sz = 4 * s, dot = (s >= 4) ? 3 : (s >= 3) ? 2 : 1;
    for (int dy = 0; dy < dot; dy++)
        for (int dx = 0; dx < dot; dx++) {
            a_px(x + dx, y + dy, 1);                        /* le point d'en haut */
            a_px(x + sz - dot + dx, y + sz - dot + dy, 1);   /* celui d'en bas */
        }
    /* la barre s'arrete a 2 px des coins : collee aux points, elle se lit
       comme une simple barre oblique */
    for (int k = 2; k <= sz - 2; k++)
        a_px(x + sz - k, y + k, 1);
    return sz + 1;
}

/* "36%" : le nombre, puis le signe (renvoie la largeur) */
static int nombre_pct(int x, int y, int s, int v)
{
    char b[12];
    snprintf(b, sizeof(b), "%d", v);
    int w = a_text_w(s, b);
    a_text(x, y, s, b);
    return w + 2 + pct_signe(x + w + 2, y, s);
}

/* copie bornee, coupee au dernier espace pour ne pas finir sur un demi-mot */
static void coupe(char *dst, size_t cap, const char *src, int max_chars)
{
    if (!src || !src[0]) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if ((int)n <= max_chars) {
        snprintf(dst, cap, "%s", src);
    } else {
        int k = max_chars;
        while (k > 8 && src[k] != ' ') k--;
        if (k <= 8) k = max_chars;
        snprintf(dst, cap, "%.*s", k, src);
    }
}

/* une jauge encadree : contour + remplissage (x, y = coin haut-gauche) */
static void barre(int x, int y, int w, int h, int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    a_hline(x, x + w - 1, y, 1);
    a_hline(x, x + w - 1, y + h - 1, 1);
    a_vline(x, y, y + h - 1, 1);
    a_vline(x + w - 1, y, y + h - 1, 1);
    int f = (w - 2) * pct / 100;
    for (int dy = 1; dy < h - 1; dy++)
        for (int dx = 0; dx < f; dx++)
            a_px(x + 1 + dx, y + dy, 1);
}

/* un texte en deux lignes de "cols" signes au plus (le reste est perdu) */
static void deux_lignes(char *l1, char *l2, const char *src, int cols)
{
    l1[0] = l2[0] = 0;
    if (!src || !src[0]) return;
    const char *p = src;
    for (int i = 0; i < 2 && *p; i++) {
        char *dst = (i == 0) ? l1 : l2;
        while (*p == ' ') p++;
        if (!*p) break;
        int n = (int)strlen(p);
        if (n <= cols) { snprintf(dst, 48, "%s", p); return; }
        int k = cols;
        while (k > 8 && p[k] != ' ') k--;
        if (k <= 8) k = cols;
        snprintf(dst, 48, "%.*s", k, p);
        p += k;
    }
}

/* petites barres de signal (10x7 px) : l'icone wifi de art.h est trop haute
   pour l'en-tete */
static void mini_wifi(int x, int y, bool on, int rssi)
{
    int niv = 0;
    if (on) niv = (rssi >= -60) ? 3 : (rssi >= -75) ? 2 : 1;
    static const int H[3] = { 2, 4, 6 };
    for (int i = 0; i < 3; i++)
        if (i < niv)
            for (int dy = 0; dy < H[i]; dy++)
                for (int dx = 0; dx < 2; dx++)
                    a_px(x + i * 3 + dx, y + 5 - dy, 1);
    if (!on) a_hline(x - 1, x + 9, y + 2, 1);   /* barre = hors ligne */
}

/* ---------------------------------------------------------------- */
/* la page                                                          */
/* ---------------------------------------------------------------- */

void tableau_dessine(const tableau_t *t)
{
    static const char *const JOURS[7] = { "DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM" };
    static const char *const MOIS[12] = { "JAN", "FEV", "MAR", "AVR", "MAI", "JUN",
                                          "JUL", "AOU", "SEP", "OCT", "NOV", "DEC" };
    char b[128], c[64], d[64], e[64];

    /* ============ en-tete : l'heure, en tres gros ==================== */
    snprintf(b, sizeof(b), "%02d:%02d", t->heure % 24, t->minute % 60);
    txt(2, 0, 6, b);                                  /* "18:54" : 114 x 30 px */

    /* la date sur deux lignes, collee a droite : "MER 11" / "NOV". Sur une
       seule ligne elle venait toucher le dernier chiffre de l'heure. */
    snprintf(b, sizeof(b), "%s %d",
             JOURS[(t->sem < 0 || t->sem > 6) ? 0 : t->sem], t->jour);
    txt_r(197, 0, 2, b);
    txt_r(197, 13, 2, MOIS[(t->mois < 0 || t->mois > 11) ? 0 : t->mois]);
    mini_wifi(188, 27, t->wifi, t->rssi);             /* le wifi, sans un mot */
    a_hline(2, 197, 38, 1);

    /* ============ le ciel =========================================== */
    {
        int y = 42;
        if (t->meteo) {
            snprintf(b, sizeof(b), "%sC", un_chiffre(c, sizeof(c), t->meteo_c));
            txt(3, y, 4, b);                           /* 13.6C en corps 4 */
            coupe(d, sizeof(d), t->meteo_txt, 12);     /* 94 px : 103..197, la
                                                          temperature s'arrete a 79 */
            txt_r(197, y + 4, 2, d);                   /* l'etat du ciel a droite */
        } else {
            txt(3, y, 4, "--");
            txt_r(197, y + 4, 2, "N.D.");
        }
        /* demain a gauche, la pluie a droite */
        if (t->dem_min > -900) {
            snprintf(b, sizeof(b), "DEM %d-%dC", t->dem_min, t->dem_max);
            txt(3, y + 22, 2, b);
            if (t->dem_pluie >= 0) {
                char n[16];
                snprintf(n, sizeof(n), "%d", t->dem_pluie);
                int wl = a_text_w(2, "PLUIE"), wv = a_text_w(2, n);
                int x = 197 - (wl + 6 + wv + 2 + 9);   /* 9 = largeur du signe */
                txt(x, y + 22, 2, "PLUIE");
                nombre_pct(x + wl + 6, y + 22, 2, t->dem_pluie);
            }
        } else {
            txt(3, y + 22, 2, "DEM --");
        }
        /* le capteur interieur, en clair */
        if (t->capteur)
            snprintf(b, sizeof(b), "INT %sC %d%%",
                     un_chiffre(c, sizeof(c), t->temp_int), (int)(t->humi + 0.5f));
        else
            snprintf(b, sizeof(b), "INT N.D.");
        coupe(e, sizeof(e), b, 21);
        txt(3, y + 36, 2, e);
    }
    a_hline(2, 197, 90, 1);

    /* ============ la batterie ======================================= */
    {
        int y = 94;
        if (t->bat_pct >= 0) {
            int w = nombre_pct(3, y, 4, t->bat_pct);
            barre(3 + w + 8, y + 2, 196 - (3 + w + 8), 16, t->bat_pct);
        } else {
            txt(3, y, 4, "USB");                       /* pas de batterie : pas de jauge */
        }
    }
    a_hline(2, 197, 116, 1);

    /* ============ le compagnon : ses quatre jauges =================== */
    {
        struct { const char *nom; int v; } j[4] = {
            { "FAIM", t->faim },     { "BONH", t->bonheur },
            { "ENER", t->energie },  { "SANTE", t->sante }
        };
        for (int i = 0; i < 4; i++) {
            int x = (i % 2) ? 104 : 3;
            int y = (i < 2) ? 120 : 137;
            txt(x, y, 2, j[i].nom);                    /* 4 ou 5 signes : 31 ou 39 px */
            if (j[i].v < 0) txt(x + 45, y, 2, "-");    /* n.d. : jamais une barre vide */
            else            barre(x + 45, y, 43, 14, j[i].v);
        }
    }
    a_hline(2, 197, 156, 1);

    /* ============ l'agenda ========================================== */
    if (t->alarme) {
        txt(3, 160, 3, "ALARME");
        txt_r(197, 160, 3, "1=STOP");
    } else if (t->rdv_quand[0]) {
        coupe(d, sizeof(d), t->rdv_quand, 15);
        txt(3, 161, 2, d);
        coupe(d, sizeof(d), t->rdv_qui, 16);
        txt_r(197, 161, 2, d);
    } else if (t->rappels > 0) {
        snprintf(b, sizeof(b), "%d RAPPEL%s", t->rappels, t->rappels > 1 ? "S" : "");
        txt(3, 161, 2, b);
    } else {
        txt(3, 161, 2, "AGENDA LIBRE");
    }

    /* ============ le dernier mot : deux lignes de corps 2 =========== */
    /* 24 signes par ligne en corps 2 (24 x 8 - 2 = 190 px) : a 30 le texte
       touchait le bord droit de la dalle */
    {
        char l1[48], l2[48];
        deux_lignes(l1, l2, t->dernier, 24);
        txt(3, 176, 2, l1[0] ? l1 : "-");
        if (l2[0]) txt(3, 187, 2, l2);
    }
}
