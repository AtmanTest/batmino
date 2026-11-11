/* Tests du routeur des boutons, sans carte et sans ESP-IDF.

   Le bloc teste n'est pas une reecriture : tests/unit/extrait_routeur.py tire
   le VRAI code de main/main.cpp (entre les marqueurs @@ROUTEUR_DEBUT@@ et
   @@ROUTEUR_FIN@@) et l'inclut ici. Tout ce qui vient du firmware est remplace
   par un stub qui compte les appels : on verifie donc l'ordre des priorites et
   la table PAGE_ACT, pas une maquette.

   Tourne sur le PC :  bash tests/run_unit.sh  */
#include <cstdio>
#include <cstdint>
#include <cstdbool>
#include <cstring>

static int ok = 0, ko = 0;

#define VERIF(cond, nom) do {                       \
    if (cond) { ok++; printf("  ok   %s\n", nom); } \
    else      { ko++; printf("  KO   %s\n", nom); } \
} while (0)

/* ---------- ce que le firmware fournit d'habitude ---------- */
#define PAGE_MAITRE  0
#define PAGE_METEO   1
#define PAGE_SYS     2
#define PAGE_HIST    3
#define PAGE_YT      4
#define PAGE_CONSEIL 5
#define PAGE_MATRIX  6
#define PAGE_LIVRE   7
#define PAGE_LANC    8
#define PAGE_REG     9
#define PAGE_COUNT   1     /* le firmware n'a plus qu'un ecran : le tableau */

static const char *PAGE_NOM[PAGE_REG + 1] = {"ACCUEIL", "METEO", "SYSTEM",
                                             "HISTORIQUE", "YOUTUBE", "CONSEIL",
                                             "MATRIX", "LIVRE", "LANCER", "REGLAGES"};

#define REG_WIFI     0
#define REG_CLAVIER  1

#define TAG "test"
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

/* horloge simulee : vTaskDelay avance le temps, comme sur la carte */
static int64_t faux_us = 1000000000LL;      /* 1000 s */
static int64_t esp_timer_get_time(void) { return faux_us; }
static void    vTaskDelay(int t) { faux_us += (int64_t)t * 1000; }
static void    avance_ms(int64_t ms) { faux_us += ms * 1000; }

/* etat du monde */
static int  ui_page;
static int64_t geste_ms;
static bool alarme_on, wake_busy, radio_etat, voix_etat, act_on;
static bool rep_ouvert, livre_pret, conseil_occupe;
static int  hist_vue, conseil_demande, reg_sel, reg_vue;
static char reg_pass[64];

/* compteurs d'appels */
static int n_show, n_tache, n_assist, n_beep;
static int n_rep_court, n_rep_long, n_rep_ferme;
static int n_livre_tourne, n_livre_suivant, n_livre_reprise;
static int dernier_sens;
static int n_reg_bouton, n_reg_long, n_touches;

static void show_ui(void)        { n_show++; }
static void tache_arrete(void)   { n_tache++; }
static void assistant_wake(void) { n_assist++; }
static void ui_beep(void)        { n_beep++; }
static void rep_ferme(void)      { n_rep_ferme++; rep_ouvert = false; }

static bool rep_bouton_court(int n)
{
    (void)n;
    if (!rep_ouvert) return false;
    n_rep_court++;
    return true;
}

static bool rep_bouton_long(int n)
{
    (void)n;
    if (!rep_ouvert) return false;
    n_rep_long++;
    rep_ouvert = false;
    return true;
}

static bool livre_reprise(void) { n_livre_reprise++; livre_pret = true; return true; }
static void livre_suivant(int d) { (void)d; n_livre_suivant++; }
static bool livre_tourne(int d)  { n_livre_tourne++; dernier_sens = d; return true; }

static int  n_mtx_lance;
static void mtx_lance(void)        { n_mtx_lance++; }

/* ce que act_maitre_court declenche : la relecture des donnees */
static bool meteo_force, yt_force;
static int  n_capteurs;
static void ui_read_sensors(void)  { n_capteurs++; }

static void reg_bouton(int n)      { (void)n; n_reg_bouton++; }
static void reg_bouton_long(int n) { (void)n; n_reg_long++; }
static int  reg_touches_n(void)    { return n_touches; }

/* la grille du launcher : definie dans main.cpp, hors du bloc extrait */
#define LANC_N 8
static int lanc_sel;
static const int LANC_PAGE[LANC_N] = {PAGE_MAITRE, PAGE_METEO, PAGE_SYS, PAGE_HIST,
                                      PAGE_YT, PAGE_CONSEIL, PAGE_MATRIX, PAGE_LIVRE};

/* ---------- LE VRAI CODE DU FIRMWARE ---------- */
#include "routeur_extrait.inc"

/* ---------- outillage de test ---------- */
static void remise(void)
{
    avance_ms(5000);                 /* les gardes de cadence sont loin derriere */
    ui_page = PAGE_MAITRE;
    alarme_on = wake_busy = radio_etat = voix_etat = act_on = false;
    rep_ouvert = livre_pret = conseil_occupe = false;
    hist_vue = 7; conseil_demande = 0; reg_sel = 0; reg_vue = REG_WIFI;
    memset(reg_pass, 'x', sizeof(reg_pass));
    n_show = n_tache = n_assist = n_beep = 0;
    n_rep_court = n_rep_long = n_rep_ferme = 0;
    n_livre_tourne = n_livre_suivant = n_livre_reprise = 0;
    n_reg_bouton = n_reg_long = 0; n_touches = 12;
    n_mtx_lance = 0; lanc_sel = 0;
    meteo_force = yt_force = false; n_capteurs = 0;
    dernier_sens = 0;
}

int main(void)
{
    printf("tests du routeur : une table par page, quatre gestes\n");

    /* ---- le tableau est la seule page : le court ne tourne plus ---- */
    remise();
    bouton_court(0);
    VERIF(ui_page == PAGE_MAITRE && n_show == 1, "ACCUEIL + court 1 : on reste sur le tableau");
    VERIF(meteo_force, "ACCUEIL + court 1 : relecture meteo demandee");
    VERIF(n_capteurs == 1 && n_beep == 1, "ACCUEIL + court 1 : capteurs relus et bip");
    bouton_court(1);
    VERIF(ui_page == PAGE_MAITRE, "court 2 sur le tableau : aucune page a tourner");

    remise();
    bouton_double(0);
    VERIF(n_assist == 1 && ui_page == PAGE_MAITRE, "ACCUEIL + double 1 : assistant, on reste");

    /* deux appuis rapproches : chacun relit, aucun perdu */
    remise();
    bouton_court(0);
    bouton_court(0);
    VERIF(n_show == 2 && n_capteurs == 2, "deux appuis rapproches : deux relectures, aucun perdu");

    /* ---- LIVRE : la table prend la main ---- */
    remise();
    ui_page = PAGE_LIVRE; livre_pret = true;
    bouton_court(0);
    VERIF(n_livre_tourne == 1 && dernier_sens == 1, "LIVRE + court 1 : page du livre en avant");
    VERIF(ui_page == PAGE_LIVRE, "LIVRE + court : la page du cycle ne change pas");
    bouton_court(1);
    VERIF(dernier_sens == -1, "LIVRE + court 2 : page du livre en arriere");

    remise();
    ui_page = PAGE_LIVRE; livre_pret = true;
    bouton_double(0);
    VERIF(n_assist == 1 && n_livre_suivant == 0, "LIVRE + double 1 : assistant, pas le livre suivant");
    remise();
    ui_page = PAGE_LIVRE; livre_pret = true;
    bouton_double(1);
    VERIF(n_livre_suivant == 1, "LIVRE + double 2 : livre suivant");

    remise();
    ui_page = PAGE_LIVRE; livre_pret = true;
    bouton_repeat(0);
    VERIF(n_livre_tourne == 1, "LIVRE + maintien : defilement");

    remise();
    ui_page = PAGE_LIVRE; livre_pret = true;
    bouton_long(0, 0);
    VERIF(ui_page == PAGE_MAITRE && n_tache == 1, "LIVRE + long : retour accueil (la table n'a pas de long)");

    /* ---- CONSEIL : le long agit, il ne ramene pas a l'accueil ---- */
    remise();
    ui_page = PAGE_CONSEIL;
    bouton_long(0, 0);
    VERIF(conseil_demande == 1 && n_beep == 1, "CONSEIL + long 1 : nouveau jugement demande");
    VERIF(ui_page == PAGE_CONSEIL, "CONSEIL + long : on reste sur la page");
    remise();
    ui_page = PAGE_CONSEIL;
    bouton_long(1, 0);
    VERIF(conseil_demande == 0 && ui_page == PAGE_CONSEIL, "CONSEIL + long 2 : rien, et on reste");

    /* ---- MATRIX : le long 1 relance, le court reste la navigation ---- */
    remise();
    ui_page = PAGE_MATRIX;
    bouton_court(0);
    VERIF(n_mtx_lance == 0 && ui_page == PAGE_MAITRE, "MATRIX + court 1 : retour au tableau, pas de relance");
    remise();
    ui_page = PAGE_MATRIX;
    bouton_long(0, 0);
    VERIF(n_mtx_lance == 1 && n_beep == 1, "MATRIX + long 1 : la sequence est relancee");
    VERIF(ui_page == PAGE_MATRIX, "MATRIX + long : on reste sur la page");
    remise();
    ui_page = PAGE_MATRIX;
    bouton_long(1, 0);
    VERIF(n_mtx_lance == 0 && ui_page == PAGE_MAITRE, "MATRIX + long 2 : retour accueil");

    /* ---- HISTORIQUE ---- */
    remise();
    ui_page = PAGE_HIST;
    bouton_long(1, 0);
    VERIF(hist_vue == 0 && ui_page == PAGE_HIST, "HISTORIQUE + long : retour au plus recent");

    /* ---- REGLAGES : le clavier ---- */
    remise();
    ui_page = PAGE_REG; reg_vue = REG_CLAVIER; reg_sel = 0; n_touches = 12;
    bouton_double(0);
    VERIF(reg_sel == 6, "REGLAGES clavier + double 1 : saute six touches");
    bouton_double(1);
    VERIF(reg_vue == REG_WIFI && reg_pass[0] == 0, "REGLAGES clavier + double 2 : abandonne la saisie");

    remise();
    ui_page = PAGE_REG; reg_vue = REG_WIFI;
    bouton_double(1);
    VERIF(reg_vue == REG_WIFI && n_assist == 0, "REGLAGES hors clavier + double 2 : rien de casse");
    ui_page = PAGE_REG;                 /* le double 2 a ouvert le livre : on revient */
    bouton_court(0);
    VERIF(n_reg_bouton == 1 && ui_page == PAGE_REG, "REGLAGES + court : la page traite, pas le cycle");

    /* ---- les priorites ---- */
    remise();
    wake_busy = true; ui_page = PAGE_METEO;
    bouton_court(0);
    VERIF(n_tache == 1 && ui_page == PAGE_MAITRE, "tache en cours + court : elle s'arrete, retour accueil");

    remise();
    rep_ouvert = true; ui_page = PAGE_METEO;
    bouton_court(0);
    VERIF(n_rep_court == 1 && ui_page == PAGE_METEO, "REPONSE ouverte : le court lui appartient");
    bouton_long(0, 0);
    VERIF(n_rep_long == 1 && n_tache == 0, "REPONSE ouverte : le long la ferme");

    remise();
    alarme_on = true;
    bouton_court(0);
    VERIF(!alarme_on && ui_page == PAGE_MAITRE, "sonnerie : le bouton 1 la fait taire");
    remise();
    alarme_on = true;
    bouton_court(1);
    VERIF(alarme_on, "sonnerie : le bouton 2 ne la coupe pas (comportement d'origine)");

    remise();
    ui_page = PAGE_YT;
    bouton_double(1);
    VERIF(n_assist == 0 && ui_page == PAGE_MAITRE, "double 2 : plus de livre, retour au tableau");
    VERIF(n_livre_reprise == 0, "double 2 : le livre n'est plus ouvert");

    /* ---- LAUNCHER : la grille ouvre les applis ---- */
    remise();
    ui_page = PAGE_LANC;
    bouton_court(0);
    VERIF(ui_page == PAGE_LANC && n_show == 1, "LANCER + court 1 : la tuile change, on reste");
    bouton_court(1);
    VERIF(ui_page == PAGE_METEO, "LANCER + court 2 : ouvre l'appli selectionnee");
    remise();
    ui_page = PAGE_LANC;
    bouton_long(0, 0);
    VERIF(ui_page == PAGE_MAITRE && n_tache == 1, "LANCER + long : retour accueil");
    remise();
    ui_page = PAGE_SYS;
    bouton_double(1);
    VERIF(ui_page == PAGE_MAITRE, "double 2 depuis SYSTEM : retour au tableau");

    /* ---- ACCUEIL : le livre a quitte l'ecran, le long ramene au tableau ---- */
    remise();
    bouton_long(1, 0);
    VERIF(ui_page == PAGE_MAITRE && n_livre_reprise == 0, "ACCUEIL + long 2 : plus de livre");
    VERIF(n_tache == 1, "ACCUEIL + long 2 : le long garde son role d'arret");
    remise();
    bouton_long(0, 0);
    VERIF(ui_page == PAGE_MAITRE, "ACCUEIL + long 1 : on reste sur le tableau");

    /* ---- la table couvre tous les ecrans ---- */
    VERIF(page_act() != NULL, "une page du cycle a sa ligne");
    ui_page = 99;                                  /* l'ecran securite, hors cycle */
    VERIF(page_act() == NULL, "un ecran hors cycle n'a pas de ligne (pas de debordement)");
    ui_page = PAGE_REG;
    VERIF(page_act() != NULL, "REGLAGES a sa ligne, hors cycle mais dans la table");
    VERIF(!strcmp(PAGE_NOM[PAGE_LIVRE], "LIVRE") &&
          !strcmp(PAGE_NOM[PAGE_MATRIX], "MATRIX") && PAGE_COUNT == 1,
          "PAGE_NOM suit l'ordre des pages (les logs s'en servent)");

    printf("resultat : %d ok, %d ko\n", ok, ko);
    return ko ? 1 : 0;
}
