/*  tableau.cpp - la page unique du BATMino (200x200, 1 bit).
 *
 *  Plan de la page :
 *
 *     0   en-tete   heure (corps 6), visage du compagnon, date, batterie
 *    33   filet
 *    36   etat      borne wifi + rssi, uptime
 *    45   filet
 *    49   rangee 1  METEO (ciel du moment)      | DEMAIN (plage, pluie)
 *    78   filet
 *    81   rangee 2  INTERIEUR (capteur, stade)  | BATTERIE (jauge, watts)
 *   110   filet
 *   113   rangee 3  SYSTEME (cpu, ram)          | AGENDA (prochain rdv)
 *   142   filet
 *   145   rangee 4  DERNIER MOT (2 lignes)      | CONSEIL (2 lignes)
 *   168   filet
 *   171   pied      youtube, livre, jauges du compagnon, gestes
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

/* etiquette de section, corps 1 */
static void lab(int x, int y, const char *str) { a_text(x, y, 1, str); }

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

/* le signe pourcent : deux points et une barre. Le glyphe 3x5 du jeu de
   caracteres ressemble a un Z a partir du corps 2, on le dessine donc. */
static int pct_signe(int x, int y, int s)
{
    /* deux points fins et une barre de 1 px : a ce corps, des points epais
       transforment le signe en X */
    int sz = 4 * s, dot = (s >= 3) ? 2 : 1;
    for (int dy = 0; dy < dot; dy++)
        for (int dx = 0; dx < dot; dx++) {
            a_px(x + dx, y + dy, 1);                       /* point haut-gauche */
            a_px(x + sz - dot + dx, y + sz - dot + dy, 1); /* point bas-droite */
        }
    for (int k = 0; k <= sz; k++)
        a_px(x + sz - k, y + k, 1);                        /* la barre oblique */
    return sz + 1;
}

/* "36%" : le nombre, puis le signe */
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

/* un texte en deux lignes de "cols" signes, coupees sur un espace */
static void deux_lignes(char *l1, char *l2, const char *src, int cols)
{
    l1[0] = l2[0] = 0;
    if (!src || !src[0]) return;
    int n = (int)strlen(src);
    if (n <= cols) { snprintf(l1, 40, "%s", src); return; }
    int k = cols;
    while (k > 6 && src[k] != ' ') k--;
    if (k <= 6) k = cols;
    snprintf(l1, 40, "%.*s", k, src);
    const char *p = src + k;
    while (*p == ' ') p++;
    snprintf(l2, 40, "%.*s", cols, p);
}

/* petites barres de signal (8x9 px) : l'icone wifi de art.h est trop haute
   pour la bande d'etat */
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

/* une lettre seule, corps 1 (l'etiquette d'une jauge) */
static void g_lettre(int x, int y, char c)
{
    char s[2] = { c, 0 };
    a_text(x, y, 1, s);
}

/* ---------------------------------------------------------------- */
/* la page                                                          */
/* ---------------------------------------------------------------- */

void tableau_dessine(const tableau_t *t)
{
    static const char *const JOURS[7] = { "DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM" };
    static const char *const MOIS[12] = { "JAN", "FEV", "MAR", "AVR", "MAI", "JUN",
                                          "JUL", "AOU", "SEP", "OCT", "NOV", "DEC" };
    char b[128], c[64], d[80];

    /* ============ en-tete : heure, visage, date, batterie ============ */
    snprintf(b, sizeof(b), "%02d:%02d", t->heure % 24, t->minute % 60);
    txt(3, 1, 6, b);                                  /* l'heure, en tres gros */

    draw_kirby(138, 17, 11, 0, t->visage);            /* le compagnon, toujours la */

    snprintf(b, sizeof(b), "%s %d %s",
             JOURS[(t->sem < 0 || t->sem > 6) ? 0 : t->sem],
             t->jour, MOIS[(t->mois < 0 || t->mois > 11) ? 0 : t->mois]);
    txt_r(197, 4, 1, b);                              /* la date */

    if (t->bat_pct >= 0) {
        snprintf(b, sizeof(b), "BAT %d", t->bat_pct);
        int x = 197 - (a_text_w(1, b) + 2 + 5);
        txt(x, 13, 1, b);
        pct_signe(x + a_text_w(1, b) + 2, 13, 1);
    } else {
        txt_r(197, 13, 1, "USB");
    }
    a_hline(2, 197, 33, 1);

    /* ============ etat : borne wifi et uptime ======================== */
    if (t->wifi && t->ssid[0]) {
        coupe(d, sizeof(d), t->ssid, 14);
        int w = a_text_w(1, d);
        mini_wifi(3, 38, true, t->rssi);
        lab(15, 38, d);
        if (t->rssi) {
            snprintf(b, sizeof(b), "%dDB", t->rssi);
            lab(15 + w + 4, 38, b);
        }
    } else {
        mini_wifi(3, 38, false, 0);
        lab(15, 38, "HORS LIGNE");
    }
    snprintf(b, sizeof(b), "UP %dH%02d", t->uptime_min / 60, t->uptime_min % 60);
    txt_r(197, 38, 1, b);
    a_hline(2, 197, 45, 1);

    /* ============ rangee 1 : le ciel ================================= */
    {
        int y = 49;
        lab(3, y, "METEO");
        if (t->meteo) {
            snprintf(b, sizeof(b), "%s", un_chiffre(c, sizeof(c), t->meteo_c));
            txt(3, y + 7, 3, b);
            int w = a_text_w(3, b);
            g_lettre(3 + w + 2, y + 14, 'C');
            coupe(d, sizeof(d), t->meteo_txt, 11);   /* "PEU NUAGEUX" tient en 11 signes */
            lab(3 + w + 10, y + 16, d);
        } else {
            txt(3, y + 7, 3, "--");
            lab(38, y + 16, "N.D.");
        }
        lab(104, y, "DEMAIN");
        if (t->dem_min > -900) {
            snprintf(b, sizeof(b), "%d-%dC", t->dem_min, t->dem_max);
            txt(104, y + 8, 2, b);
            if (t->dem_pluie >= 0) {
                char n[16];
                snprintf(n, sizeof(n), "%d", t->dem_pluie);
                int wl = a_text_w(1, "PLUIE "), wv = a_text_w(1, n);
                int x = 197 - (wl + wv + 3 + 6);
                lab(x, y + 21, "PLUIE");
                nombre_pct(x + wl, y + 21, 1, t->dem_pluie);
            }
        } else {
            txt(104, y + 8, 2, "--");
            lab(104, y + 21, "N.D.");
        }
    }
    a_hline(2, 197, 78, 1);

    /* ============ rangee 2 : interieur et batterie =================== */
    {
        int y = 81;
        lab(3, y, "INTERIEUR");
        if (t->capteur) {
            snprintf(b, sizeof(b), "%s", un_chiffre(c, sizeof(c), t->temp_int));
            txt(3, y + 8, 2, b);
            g_lettre(3 + a_text_w(2, b) + 2, y + 13, 'C');
            nombre_pct(62, y + 8, 2, (int)(t->humi + 0.5f));
        } else {
            txt(3, y + 8, 2, "N.D.");
        }
        /* qui il est : le stade, la forme et son age */
        {
            char e[40] = "";
            if (t->stade) snprintf(e, sizeof(e), "%s", t->stade);
            if (t->forme && strcmp(t->forme, "-")) {
                size_t l = strlen(e);
                snprintf(e + l, sizeof(e) - l, " %s", t->forme);
            }
            if (t->age_j >= 0) {
                size_t l = strlen(e);
                snprintf(e + l, sizeof(e) - l, " %dJ", t->age_j);
            }
            coupe(d, sizeof(d), e, 23);
            lab(3, y + 22, d);
        }

        lab(104, y, "BATTERIE");
        if (t->bat_pct >= 0) {
            int w = nombre_pct(104, y + 8, 2, t->bat_pct);
            barre(104 + w + 6, y + 11, 196 - (104 + w + 6), 8, t->bat_pct);
        } else {
            txt(104, y + 8, 2, "USB");
        }
        if (t->conso_mw >= 0) {
            snprintf(b, sizeof(b), "%dMW", t->conso_mw);
            txt_r(197, y + 22, 1, b);
        }
    }
    a_hline(2, 197, 110, 1);

    /* ============ rangee 3 : banc et agenda ========================== */
    {
        int y = 113;
        lab(3, y, "SYSTEME");
        lab(3, y + 8, "CPU");
        nombre_pct(34, y + 6, 2, t->cpu_pct < 0 ? 0 : t->cpu_pct);
        snprintf(b, sizeof(b), "RAM %uK", t->ram_libre_k);
        lab(3, y + 22, b);

        lab(104, y, "AGENDA");
        if (t->rdv_quand[0]) {
            coupe(d, sizeof(d), t->rdv_quand, 15);
            lab(104, y + 8, d);
            coupe(d, sizeof(d), t->rdv_qui, 15);
            txt(104, y + 15, 2, d);
        } else if (t->rappels > 0) {
            snprintf(b, sizeof(b), "%d RAPPEL%s", t->rappels, t->rappels > 1 ? "S" : "");
            txt(104, y + 9, 2, b);
        } else {
            txt(104, y + 9, 2, "LIBRE");
        }
    }
    a_hline(2, 197, 142, 1);

    /* ============ rangee 4 : le dernier mot et le conseil ============ */
    {
        int y = 145;
        char l1[40], l2[40];
        lab(3, y, "DERNIER MOT");
        deux_lignes(l1, l2, t->dernier, 23);
        lab(3, y + 7, l1[0] ? l1 : "-");
        if (l2[0]) lab(3, y + 14, l2);

        lab(104, y, "CONSEIL");
        deux_lignes(l1, l2, t->conseil, 23);
        lab(104, y + 7, l1[0] ? l1 : "-");
        if (l2[0]) lab(104, y + 14, l2);
    }
    a_hline(2, 197, 166, 1);

    /* ============ pied : youtube, livre, jauges ====================== */
    {
        lab(3, 169, "CH.");
        if (t->yt_chaine[0]) {
            coupe(d, sizeof(d), t->yt_chaine, 14);
            lab(3 + a_text_w(1, "CH.") + 4, 169, d);
        }
        if (t->yt_vues) {
            if (t->yt_vues >= 1000000UL)
                snprintf(b, sizeof(b), "%lu,%02luM VUES", t->yt_vues / 1000000UL,
                         (t->yt_vues / 10000UL) % 100UL);
            else if (t->yt_vues >= 1000UL)
                snprintf(b, sizeof(b), "%luK VUES", t->yt_vues / 1000UL);
            else
                snprintf(b, sizeof(b), "%lu VUES", t->yt_vues);
            txt_r(197, 169, 1, b);
        }
        if (t->yt_video[0]) {
            coupe(d, sizeof(d), t->yt_video, 49);
            lab(3, 176, d);
        }

        if (t->livre_n > 0) {
            snprintf(b, sizeof(b), "LIVRE %s %d/%d", t->livre, t->livre_p + 1, t->livre_n);
            coupe(d, sizeof(d), b, 30);
            lab(3, 183, d);
            barre(150, 184, 47, 6, t->livre_n > 1 ? t->livre_p * 100 / (t->livre_n - 1) : 0);
        } else {
            lab(3, 183, "PAS DE LIVRE");
        }

        /* les jauges du compagnon : F faim, B bonheur, E energie, S sante */
        {
            struct { char l; int v; } j[4] = {
                { 'F', t->faim }, { 'B', t->bonheur }, { 'E', t->energie }, { 'S', t->sante }
            };
            int x = 3;
            for (int i = 0; i < 4; i++) {
                g_lettre(x, 191, j[i].l);
                if (j[i].v < 0) lab(x + 6, 191, "-");   /* n.d. : jamais un zero */
                else            barre(x + 6, 191, 18, 6, j[i].v);
                x += 27;
            }
            if (t->alarme) txt_r(197, 192, 1, "ALARME 1=STOP");
            else           txt_r(197, 192, 1, "LONG 2 = LIVRE");
        }
    }
}
