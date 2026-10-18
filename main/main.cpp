/*  Mino - mascotte cochon sur e-paper 200x200 (Waveshare ESP32-S3-ePaper-1.54 V2)
 *  - 15 expressions du visage
 *  - cerveau DeepSeek (cerveau en ligne, texte) via HTTPS
 *  - SOUL + MEMOIRE persistantes sur SPIFFS (facon agent, version legere)
 *  - petite API HTTP sur le WiFi local
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_adc/adc_oneshot.h"
#include "lwip/netif.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include "mp3dec.h"          /* decodeur mp3 (voix de Google) */

#include "art.h"
#include "pet.h"
#include "petart.h"
#include "esp_private/esp_clk.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "user_config.h"
#include "secrets.h"
#include "board_power_bsp.h"
#include "i2c_bsp.h"
#include "audio_bsp.h"
#include "epaper_driver_bsp.h"

static const char *TAG = "mino";

/* ------------------------------------------------------------------ */
/* console serie USB : logs + commandes tapees au clavier              */
/* ------------------------------------------------------------------ */
static int jtag_log(const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0)
        usb_serial_jtag_write_bytes(buf, (n > (int)sizeof(buf) - 1) ? sizeof(buf) - 1 : n,
                                    pdMS_TO_TICKS(200));
    return n;
}

static bool serial_ready = false;

static void serial_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.rx_buffer_size = 2048;
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
    esp_log_set_vprintf(jtag_log);
    serial_ready = true;
}

/* ------------------------------------------------------------------ */
/* framebuffer 200x200, 1 bit/px, 1 = blanc, 0 = noir                   */
/* ------------------------------------------------------------------ */
#define FB_LEN (200 * 25)
static uint8_t fb[FB_LEN];
static int fb_oy = 0;          /* decalage vertical (le visage passe sous la barre) */

static inline void fb_clear(void) { memset(fb, 0xFF, FB_LEN); }

static inline void px(int x, int y, int black)
{
    y += fb_oy;
    if (x < 0 || x > 199 || y < 0 || y > 199) return;
    int i = y * 25 + (x >> 3);
    uint8_t m = (uint8_t)(0x80 >> (x & 7));
    if (black) fb[i] &= (uint8_t)~m; else fb[i] |= m;
}

static void hline(int x0, int x1, int y, int t)
{
    if (x1 < x0) { int s = x0; x0 = x1; x1 = s; }
    for (int x = x0; x <= x1; x++)
        for (int k = 0; k < t; k++) px(x, y + k, 1);
}

static void vline(int x, int y0, int y1, int t)
{
    if (y1 < y0) { int s = y0; y0 = y1; y1 = s; }
    for (int y = y0; y <= y1; y++)
        for (int k = 0; k < t; k++) px(x + k, y, 1);
}

static void disc(int cx, int cy, int r, int black)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) px(cx + x, cy + y, black);
}

static void ring(int cx, int cy, int r, int t, int black)
{
    for (int y = -r - t; y <= r + t; y++) {
        for (int x = -r - t; x <= r + t; x++) {
            int d2 = x * x + y * y;
            if (d2 <= (r + t) * (r + t) && d2 >= (r - t) * (r - t))
                if (d2 >= (r - t) * (r - t) && d2 <= (r + t) * (r + t))
                    px(cx + x, cy + y, black);
        }
    }
}

static void ellipse_fill(int cx, int cy, int rx, int ry, int black)
{
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++) {
            float v = (float)(x * x) / (float)(rx * rx) + (float)(y * y) / (float)(ry * ry);
            if (v <= 1.0f) px(cx + x, cy + y, black);
        }
}

static void ellipse_ring(int cx, int cy, int rx, int ry, int t, int black)
{
    for (int y = -ry - t; y <= ry + t; y++)
        for (int x = -rx - t; x <= rx + t; x++) {
            float v = (float)(x * x) / (float)(rx * rx) + (float)(y * y) / (float)(ry * ry);
            float vi = (float)(x * x) / (float)((rx - t) * (rx - t)) +
                       (float)(y * y) / (float)((ry - t) * (ry - t));
            if (v <= 1.0f && vi >= 1.0f) px(cx + x, cy + y, black);
        }
}

static void seg(int x0, int y0, int x1, int y1, int t)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        disc(x0, y0, t, 1);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* arc: angles en degres, 0=droite, 90=haut (repère ecran) */
static void arc(int cx, int cy, int r, float a0, float a1, int t)
{
    float step = (r > 40) ? 0.7f : 1.2f;
    for (float a = a0; a <= a1; a += step) {
        float rad = a * 3.14159265f / 180.0f;
        int x = cx + (int)lroundf(r * cosf(rad));
        int y = cy - (int)lroundf(r * sinf(rad));
        disc(x, y, t, 1);
    }
}

static void tri_fill(int x0, int y0, int x1, int y1, int x2, int y2)
{
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            float d1 = (float)((x1 - x0) * (y - y0) - (y1 - y0) * (x - x0));
            float d2 = (float)((x2 - x1) * (y - y1) - (y2 - y1) * (x - x1));
            float d3 = (float)((x0 - x2) * (y - y2) - (y0 - y2) * (x - x2));
            int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            if (!(neg && pos)) px(x, y, 1);
        }
}

/* BATMino : tout le dessin du personnage vit maintenant dans art.cpp */
/* ------------------------------------------------------------------ */
/* ecran                                                               */
/* ------------------------------------------------------------------ */
static epaper_driver_display *epd = NULL;
static SemaphoreHandle_t epd_mux;
static int refresh_count = 0;

static volatile int voix_etat;   /* etat de la voix (defini plus bas) */
static void show_ui(void);
static void tts_parle(const char *texte);   /* la voix : texte -> parole (definie plus bas) */
static void heure_sauve(void);   /* pour que le prochain demarrage soit a l'heure */
static int conso_mw(void);   /* consommation estimee, lue par le tableau de bord */

/* le cerveau : cle, adresse et modele (reglables a chaud, gardes en NVS) */
static char ia_cle[128] = "";        /* cle du cerveau, modifiable a chaud (NVS) */
static char ia_base[128] = "https://api.deepseek.com/chat/completions";
static char ia_mod[64]   = "deepseek-chat";
static int  chrono_stt_ms = 0, chrono_llm_ms = 0, chrono_tts_ms = 0, chrono_ms = 0;

/* les titres du moment (flux RSS de franceinfo), affiches sur la page INFOS */
#define INFOS_MAX 8
static char *infos_xml = NULL;                  /* le flux brut, en PSRAM */
static char  infos_titre[INFOS_MAX][132];
static int   infos_n = 0, infos_haut = 0;
static volatile bool infos_ok = false;
static volatile int  infos_octets = -999;   /* ce que la lecture a renvoye */
static void infos_lire(void);
/* Mino comprend le francais par defaut ; /langue?l=en-US bascule tout
   (reconnaissance + voix) sur l'anglais. */
static char langue[8] = "fr-FR";
static void paroles_ajoute(const char *qui, const char *texte);
static void voix_tour(const uint8_t *pcm, int len);
static esp_err_t ds_chat(const char *user_text, char *out_dit, size_t dit_sz,
                         char *out_emo, size_t emo_sz);
/* la meteo et la voix : definies plus bas, utilisees par la page maitre */
static volatile bool meteo_ok;
static float meteo_temp;
static int   meteo_code;
static void meteo_lire(void);
static const char *meteo_texte(int code);
static const char *ia_key(void);
static bool groq_pret(void);
static void radio_wifi_task(void *arg);
static esp_err_t qval(httpd_req_t *req, const char *key, char *out, size_t sz);
static void draw_status_bar(void);

/* cinq applis, une par ecran : voir les draw_app_* plus bas */
/*  Ecrans du BATMino autonome :
 *   0 MAITRE    tableau de bord : heure, date, meteo, wifi, petite tete
 *   1 PARLER    l'assistant vocal : micro, puis ce qu'il dit
 *   2 PAROLES   tout ce qui a ete dit, en texte, defilable
 *   3 MOI       le tamagotchi : comment il va
 *   4 MEMOIRE  ce qu'il sait
 *   5 RADIO     la radio (le PC n'est plus necessaire)
 */
#define PAGE_MAITRE 0
/* deux pages seulement : l'accueil (ou vit la conversation) et le systeme */
#define PAGE_SYS    1
#define PAGE_REG    2
#define PAGE_COUNT  3
static const char *PAGE_NOM[PAGE_COUNT] = {"ACCUEIL", "SYSTEM", "REGLAGES"};

/* ui_page / ui_face / menu_* : declares plus haut (pres de show_face) */
static float ui_temp = 0.0f, ui_humi = 0.0f;
/* le SHTC3 est colle au dos de la carte, juste a cote de l'ESP32 et du regulateur :
   il mesure la chaleur de la carte (environ +10 C) et non celle de la piece.
   Ce decalage, reglable par la commande `offset`, retablit une estimation ambiante. */
static float ui_temp_offset = -10.0f;
static float ui_temp_brut = 0.0f;
static bool  ui_shtc_ok = false;
static bool  ui_wifi_ok = false;
static bool  touch_log = false;
static SemaphoreHandle_t i2c_mux = NULL;
static void  ui_beep(void);

/* audio : tampon mono 16 bits en PSRAM et aides, utilisees des le reveil */
static uint8_t *rec_pcm = NULL;
static int  rec_last_bytes = 0;
static volatile bool radio_mode = false;      /* le PC pousse un flux PCM sur l'USB */
static volatile bool pcm_xfer = false;        /* transfert PCM en cours : pas d'echo */
static volatile int  radio_total = 0, radio_done = 0;   /* octets stereo attendus / recus */
static int  radio_idle = 0;                             /* lectures sans donnees en mode flux */
static int  audio_record_at(int off, int frames, int *peak);
static int  audio_record_mono(int frames, int *peak);
static int  audio_sd_write(const char *path, const uint8_t *mono, int data_len);
static void audio_play_mono(int frames);

/* etat partage : page courante, tamagotchi, carte SD.
   Declares ici car les pages, le journal et le moteur les lisent de partout. */
static int   ui_page = 0;
static int   ui_face = 0;
static int   current_emotion = 8;   /* famille "calme" */
static bool  sd_ok = false;
static pet_state_t pet;
static SemaphoreHandle_t pet_mux = NULL;
static int anim_ph = 0;                  /* etape du petit compagnon (il bouge) */

/* Mino se promene : il vise un coin de la zone blanche et y va pas a pas */
static int kirby_x = 100, kirby_y = 148;     /* ou il est */
static int kirby_vx = 0, kirby_vy = 0;       /* ou il veut aller */

static int conso_mw(void);   /* definie plus bas (elle lit la radio et la voix) */
static float mic_db = 42.0f;   /* 42 dB saturait tout, 24 dB etait juste pour une voix humaine */             /* gain du micro, reglable par /mic?db= */
static int   mic_crete = 0;              /* crete de la derniere prise, lue par /state */
static int   mic_bruit = 0;              /* bruit de fond mesure */
static int   mic_seuil = 0;              /* seuil de parole qui en decoule */
static int   stt_http = 0;               /* dernier code HTTP de la reconnaissance */
static int   stt_ecrits = 0;             /* octets audio effectivement envoyes */
static int   stt_total = 0;              /* octets audio prevus */
static char  stt_detail[130] = "";       /* debut de la reponse, pour diagnostiquer */

/* l'humeur du compagnon : 0 ravi, 1 tranquille, 2 triste, 3 endormi */
static int kirby_etat(void)
{
    if (pet.endormi) return 3;
    switch (current_emotion) {
    case 0: case 9: case 10:                 return 0;   /* joie, fierte, tendresse */
    case 1: case 3: case 5: case 11: case 13: return 2;   /* tristesse, peur, degout, gene, douleur */
    default:                                 return 1;
    }
}
static SemaphoreHandle_t ui_mux = NULL;
static esp_err_t send_json(httpd_req_t *req, cJSON *root);
static void monde_maj_info(monde_info_t *mi);
static int  famille_vers_bat5(int famille);      /* 14 familles -> 5 visages */
static int  bat5_du_pet;                        /* le visage montre a l'ecran */

static cJSON *mem_obj;                           /* les notes qu'il a apprises */

/* bulle de message affichee quelques secondes, pensee pour un enfant :
   un cadre noir, une phrase courte, elle part toute seule. */
static char     toast_txt[40] = {0};
static uint32_t toast_fin_ms = 0;
static void toast_pose(const char *t, int ms)
{
    snprintf(toast_txt, sizeof(toast_txt), "%s", t);
    toast_fin_ms = (uint32_t)(esp_timer_get_time() / 1000) + ms;
    show_ui();
}
static void toast_dessine(void)
{
    if (!toast_txt[0]) return;
    if ((uint32_t)(esp_timer_get_time() / 1000) > toast_fin_ms) return;
    {
        int w = a_text_w(1, toast_txt) + 10;
        int x0 = 100 - w / 2, y0 = 118;
        for (int y = y0; y < y0 + 15; y++)
            for (int x = x0; x < x0 + w; x++) a_px(x, y, 1);
        a_text_col(x0 + 5, y0 + 5, 1, toast_txt, 0);
    }
}

static void show_face(int e)
{
    if (e < 0 || e >= EXPR_N) e = 0;
    ui_face = e;
    current_emotion = EXPR[e].famille;
    bat5_du_pet = famille_vers_bat5(current_emotion);
    show_ui();
}

static void assistant_wake(void);
static void page_next(int d);

/* ================================================================== */
/* interface : barre d'etat (wifi / heure / temperature / engrenage)   */
/* + pages balayables : visage, horloge, capteurs, reglages            */
/* ================================================================== */

/* chiffres 3x5 (1 = pixel noir) */
static const uint8_t DIG5[10][5] = {
    {7, 5, 5, 5, 7}, {2, 2, 2, 2, 2}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}
};
static const uint8_t GC5[5] = {7, 4, 4, 4, 7};      /* C */
static const uint8_t GP5[5] = {5, 1, 2, 4, 5};      /* % */
static const uint8_t GM5[5] = {4, 5, 7, 5, 5};      /* M */

static void g3x5(int x, int y, int s, const uint8_t rows[5])
{
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (rows[r] & (4 >> c))
                for (int dy = 0; dy < s; dy++)
                    for (int dx = 0; dx < s; dx++)
                        px(x + c * s + dx, y + r * s + dy, 1);
}

/* ecrit un nombre en chiffres 3x5, renvoie la largeur utilisee */
static int draw_num(int x, int y, int s, int val, int min_d)
{
    char b[8];
    int n = 0, v = val < 0 ? -val : val;
    do { b[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 7);
    while (n < min_d && n < 7) b[n++] = '0';
    for (int i = 0; i < n; i++) g3x5(x + i * 4 * s, y, s, DIG5[b[n - 1 - i] - '0']);
    return n * 4 * s - s;
}

/* police 3x5 complete (majuscules) : index dans FONT_CHARS */
static const char FONT_CHARS[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:-/%!?#()+=<>,*'&_";
static const uint8_t FONT3X5[56][5] = {
  {0,0,0,0,0}, {7,5,5,5,7}, {2,2,2,2,2}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
  {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7}, {2,5,7,5,5},
  {6,5,6,5,6}, {7,4,4,4,7}, {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4}, {7,4,5,5,7},
  {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,7}, {5,5,6,5,5}, {4,4,4,4,7}, {5,7,7,5,5},
  {5,7,5,5,5}, {7,5,5,5,7}, {7,5,7,4,4}, {7,5,5,7,1}, {7,5,7,6,5}, {7,4,7,1,7},
  {7,2,2,2,2}, {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5}, {5,5,2,2,2},
  {7,1,2,4,7}, {0,0,0,0,2}, {0,2,0,2,0}, {0,0,7,0,0}, {1,1,2,4,4}, {5,1,2,4,5},
  {2,2,2,0,2}, {7,1,3,0,2}, {5,7,5,7,5}, {2,4,4,4,2}, {2,1,1,1,2}, {0,2,7,2,0},
  {0,7,0,7,0}, {1,2,4,2,1}, {4,2,1,2,4}, {0,0,0,2,4}, {5,2,5,0,0}, {2,2,0,0,0},
  {2,5,2,5,2}, {0,0,0,0,7},
};

static int font_idx(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    for (int i = 0; i < (int)sizeof(FONT_CHARS) - 1; i++)
        if (FONT_CHARS[i] == c) return i;
    return 0;
}

/* texte en police 3x5, renvoie la largeur utilisee */
static int draw_text(int x, int y, int s, const char *str)
{
    int i = 0;
    for (const char *p = str; *p; p++, i++)
        g3x5(x + i * 4 * s, y, s, FONT3X5[font_idx(*p)]);
    return i > 0 ? i * 4 * s - s : 0;
}

static void sq(int x, int y, int s)
{
    for (int dy = 0; dy < s; dy++)
        for (int dx = 0; dx < s; dx++) px(x + dx, y + dy, 1);
}
/* icones */
static void icon_wifi(int cx, int cy, int on)
{
    int b = cy + 4;
    arc(cx, b, 12, 35, 145, 1);
    arc(cx, b, 7, 35, 145, 1);
    arc(cx, b, 3, 35, 145, 1);
    disc(cx, b - 1, 1, 1);
    if (!on) seg(cx - 9, cy - 11, cx + 9, cy + 9, 1);
}

static void icon_temp(int cx, int cy)
{
    ring(cx, cy + 5, 3, 1, 1);
    seg(cx, cy - 5, cx, cy + 4, 1);
    px(cx - 1, cy - 5, 1); px(cx + 1, cy - 5, 1);
}

static void icon_gear(int cx, int cy, int r, int t)
{
    ring(cx, cy, r, t, 1);
    for (int a = 0; a < 360; a += 45) {
        float rad = (float)a * 3.14159265f / 180.0f;
        int x0 = cx + (int)lroundf((float)r * cosf(rad));
        int y0 = cy - (int)lroundf((float)r * sinf(rad));
        int x1 = cx + (int)lroundf((float)(r + 4) * cosf(rad));
        int y1 = cy - (int)lroundf((float)(r + 4) * sinf(rad));
        seg(x0, y0, x1, y1, 1);
    }
}

static void icon_speaker(int cx, int cy)
{
    tri_fill(cx - 12, cy - 6, cx - 12, cy + 6, cx - 2, cy + 2);
    tri_fill(cx - 12, cy - 6, cx - 2, cy - 2, cx - 2, cy + 2);
    hline(cx - 13, cx - 2, cy - 6, 1);
    hline(cx - 13, cx - 2, cy + 6, 1);
    arc(cx - 2, cy, 9, -50, 50, 2);
    arc(cx - 2, cy, 15, -45, 45, 2);
}

static void icon_power(int cx, int cy, int r)
{
    arc(cx, cy, r, -60, 240, 2);
    seg(cx, cy - r - 4, cx, cy - 1, 2);
}

static void icon_pig_small(int cx, int cy) { ic_bat_small(cx, cy, 9); }

/* ================================================================== */
/* page securite : analyse reseau de base                              */
/* ================================================================== */

static char  sec_ssid[34] = "-";
static char  sec_auth[12] = "-";
static char  sec_ciph[9]  = "-";
static char  sec_bssid[18] = "-";
static int   sec_chan = 0, sec_rssi = 0;
static char  sec_ip[16] = "-", sec_gw[16] = "-", sec_dns[16] = "-";
static bool  sec_gw_ok = false, sec_net_ok = false, sec_dns_ok = false;
static bool  sec_wps = false, sec_wpa3 = false;
static int   sec_aps = 0, sec_open = 0;
static int   sec_hotes = -1;            /* -1 = balayage en cours */
static int   sec_risque = 0;
static char  sec_alerte[24] = "ANALYSE EN COURS";
static bool  sec_sweep = false;
static SemaphoreHandle_t ping_mux = NULL;

static const char *auth_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "OUVERT";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "INCONNU";
    }
}

static const char *ciph_str(wifi_cipher_type_t c)
{
    switch (c) {
    case WIFI_CIPHER_TYPE_NONE:      return "AUCUN";
    case WIFI_CIPHER_TYPE_WEP40:     return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104:    return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:      return "AES";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+AES";
    case WIFI_CIPHER_TYPE_GCMP:      return "GCMP";
    default:                         return "?";
    }
}

static volatile int ping_rep = 0;

static void ping_ok_cb(esp_ping_handle_t h, void *args) { ping_rep++; }
static void ping_to_cb(esp_ping_handle_t h, void *args) { }
static void ping_end_cb(esp_ping_handle_t h, void *args) { }

/* un ping unique vers une adresse IP (texte) ; renvoie vrai si on a une reponse */
static bool ping_ip(const char *ip, int timeout_ms)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 1;
    cfg.interval_ms = 100;
    cfg.timeout_ms = (uint32_t)timeout_ms;
    cfg.task_stack_size = 3072;
    cfg.target_addr.type = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4(ip, (esp_ip4_addr_t *)&cfg.target_addr.u_addr.ip4) != ESP_OK) return false;
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = ping_ok_cb;
    cbs.on_ping_timeout = ping_to_cb;
    cbs.on_ping_end = ping_end_cb;
    esp_ping_handle_t h = NULL;
    ping_rep = 0;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) return false;
    esp_ping_start(h);
    int wait = 0;
    while (wait < timeout_ms + 150 && ping_rep == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait += 50;
    }
    esp_ping_delete_session(h);
    return ping_rep > 0;
}

/* releve tout ce qui peut l'etre sans bloquer longtemps */
static void secu_collect(void)
{
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ui_wifi_ok = true;
        snprintf(sec_ssid, sizeof(sec_ssid), "%s", (const char *)ap.ssid);
        snprintf(sec_auth, sizeof(sec_auth), "%s", auth_str(ap.authmode));
        snprintf(sec_ciph, sizeof(sec_ciph), "%s", ciph_str(ap.pairwise_cipher));
        snprintf(sec_bssid, sizeof(sec_bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        sec_chan = ap.primary;
        sec_rssi = ap.rssi;
        sec_wpa3 = (ap.authmode == WIFI_AUTH_WPA3_PSK || ap.authmode == WIFI_AUTH_WPA2_WPA3_PSK);
        sec_wps  = (ap.wps == true);
    } else {
        ui_wifi_ok = false;
    }

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, sec_ip, sizeof(sec_ip));
        esp_ip4addr_ntoa(&ip.gw, sec_gw, sizeof(sec_gw));
    }
    if (nif) {
        esp_netif_dns_info_t d;
        if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK &&
            d.ip.type == ESP_IPADDR_TYPE_V4)
            esp_ip4addr_ntoa(&d.ip.u_addr.ip4, sec_dns, sizeof(sec_dns));
    }

    /* voisinage radio : combien de reseaux, combien en clair */
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    uint16_t n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_num(&n) == ESP_OK && n > 0) {
        uint16_t cnt = n > 40 ? 40 : n;
        wifi_ap_record_t *l = (wifi_ap_record_t *)calloc(cnt, sizeof(wifi_ap_record_t));
        if (l) {
            int op = 0;
            if (esp_wifi_scan_get_ap_records(&cnt, l) == ESP_OK) {
                for (int i = 0; i < cnt; i++)
                    if (l[i].authmode == WIFI_AUTH_OPEN) op++;
                sec_aps = cnt;
                sec_open = op;
            }
            free(l);
        }
        esp_wifi_clear_ap_list();
    }

    /* passerelle, internet, resolution de noms */
    if (ping_mux && xSemaphoreTake(ping_mux, pdMS_TO_TICKS(30000)) == pdTRUE) {
        sec_gw_ok  = ping_ip(sec_gw, 1000);
        sec_net_ok = ping_ip("1.1.1.1", 1200);
        if (!sec_net_ok) sec_net_ok = ping_ip("8.8.8.8", 1200);
        xSemaphoreGive(ping_mux);
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    sec_dns_ok = (lwip_getaddrinfo("pool.ntp.org", NULL, &hints, &res) == 0);
    if (res) lwip_freeaddrinfo(res);

    /* alertes, du plus grave au plus benin */
    sec_risque = 0;
    if (!strcmp(sec_auth, "OUVERT") || !strcmp(sec_auth, "WEP")) {
        snprintf(sec_alerte, sizeof(sec_alerte), "RESEAU NON PROTEGE");
        sec_risque = 5;
    } else {
        if (!strcmp(sec_auth, "WPA") || !strcmp(sec_auth, "WPA/WPA2")) sec_risque += 2;
        else sec_risque += 1;
        if (sec_wps)    { sec_risque += 1; snprintf(sec_alerte, sizeof(sec_alerte), "WPS ACTIF"); }
        else if (!sec_wpa3) { snprintf(sec_alerte, sizeof(sec_alerte), "PAS DE WPA3"); }
        else snprintf(sec_alerte, sizeof(sec_alerte), "RESEAU A JOUR");
        if (!sec_dns_ok) sec_risque += 1;
        if (sec_open > 0) sec_risque += 1;
        if (sec_risque > 5) sec_risque = 5;
    }
}

/* balayage de la classe locale pour compter les machines presentes */
static void secu_sweep_task(void *arg)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK) {
        sec_hotes = 0;
        sec_sweep = false;
        vTaskDelete(NULL);
        return;
    }
    uint32_t base = ntohl(ip.ip.addr) & 0xFFFFFF00u;
    uint32_t self = ntohl(ip.ip.addr);
    int found = 0;
    for (int i = 1; i < 255; i++) {
        uint32_t a = base | (uint32_t)i;
        if (a == self) continue;
        char b[16];
        snprintf(b, sizeof(b), "%u.%u.%u.%u", (unsigned)((a >> 24) & 255), (unsigned)((a >> 16) & 255),
                 (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
        if (ping_mux && xSemaphoreTake(ping_mux, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (ping_ip(b, 80)) { found++; }
            sec_hotes = found;
            xSemaphoreGive(ping_mux);
        }
        if (i % 24 == 0 && ui_page == 99) show_ui();
        if (found >= 64) break;
    }
    sec_hotes = found;
    sec_sweep = false;
    ESP_LOGI(TAG, "analyse reseau : %d machine(s) sur le reseau local", found);
    vTaskDelete(NULL);
}

static void secu_padlock(int cx, int cy, int s)
{
    ring(cx, cy - 6, 5, 2, 1);
    for (int dy = 0; dy < 12; dy++)
        for (int dx = 0; dx < 14; dx++) {
            int e = (dx == 0 || dy == 0 || dx == 13 || dy == 11);
            if (e) px(cx - 7 + dx, cy + dy, 1);
        }
    px(cx, cy + 4, 1); px(cx, cy + 5, 1); (void)s;
}

static void draw_page_secu(void)
{
    char b[52];

    draw_text(8, 30, 2, "SECU");
    draw_text(52, 33, 1, "ANALYSE RESEAU");
    secu_padlock(180, 28, 1);

    snprintf(b, sizeof(b), "SSID %.34s", sec_ssid);
    draw_text(6, 48, 1, b);

    snprintf(b, sizeof(b), "%.8s %.7s CANAL %d %dDBM", sec_auth, sec_ciph, sec_chan, sec_rssi);
    draw_text(6, 58, 1, b);

    snprintf(b, sizeof(b), "IP %s", sec_ip);
    draw_text(6, 68, 1, b);

    snprintf(b, sizeof(b), "PASSERELLE %s %s", sec_gw, sec_gw_ok ? "OK" : "KO");
    draw_text(6, 78, 1, b);

    snprintf(b, sizeof(b), "DNS %s %s", sec_dns, sec_dns_ok ? "OK" : "KO");
    draw_text(6, 88, 1, b);

    snprintf(b, sizeof(b), "INTERNET %s", sec_net_ok ? "OK" : "KO");
    draw_text(6, 98, 1, b);

    if (sec_sweep)      { snprintf(b, sizeof(b), "MACHINES LOCALES %d+", sec_hotes < 0 ? 0 : sec_hotes); draw_text(6, 108, 1, b); }
    else if (sec_hotes < 0) draw_text(6, 108, 1, "MACHINES LOCALES : ?");
    else { snprintf(b, sizeof(b), "MACHINES LOCALES : %d", sec_hotes); draw_text(6, 108, 1, b); }

    snprintf(b, sizeof(b), "RESEAUX VOISINS %d DONT %d OUVERTS", sec_aps, sec_open);
    draw_text(6, 118, 1, b);

    draw_text(6, 128, 1, "PORT 80 API SANS JETON");
    snprintf(b, sizeof(b), "WPS %s  WPA3 %s", sec_wps ? "OUI" : "NON", sec_wpa3 ? "OUI" : "NON");
    draw_text(6, 138, 1, b);

    hline(4, 195, 150, 1);

    snprintf(b, sizeof(b), "RISQUE %d/5", sec_risque);
    draw_text(6, 158, 1, b);
    draw_text(6, 172, 1, sec_alerte);
}

static volatile bool secu_busy = false;

/* relance l'analyse en tache separee (ne bloque ni l'ecran ni les boutons) */
static void secu_refresh_task(void *a)
{
    secu_busy = true;
    secu_collect();
    if (ui_page == 99) show_ui();
    secu_busy = false;
    vTaskDelete(NULL);
}

static void secu_refresh(void)
{
    if (secu_busy) return;
    xTaskCreate(secu_refresh_task, "secu", 6144, NULL, 3, NULL);
}

static void secu_sweep(void)
{
    if (sec_sweep) return;
    sec_sweep = true;
    sec_hotes = -1;
    xTaskCreate(secu_sweep_task, "secusweep", 4096, NULL, 2, NULL);
}

/* ================================================================== */
/* ecran d'activite : barre de progression en temps reel               */
/* ================================================================== */

static bool  act_on = false;
static char  act_label[26] = "";
static int   act_pct = 0;
static bool  act_indet = false;
static int   act_tick = 0;
static int64_t act_last_ms = 0;
static bool  act_anim = false;

static void draw_activity(void)
{
    char b[30];
    /* titre */
    draw_text(10, 34, 2, act_label);
    /* cadre de la barre */
    hline(12, 187, 66, 1);
    hline(12, 187, 96, 1);
    vline(12, 66, 97, 1);
    vline(187, 66, 97, 1);
    /* remplissage */
    int w = (act_pct * 171) / 100;
    if (w > 0) {
        for (int y = 69; y < 94; y++)
            for (int x = 15; x < 15 + w && x < 185; x++)
                px(x, y, 1);
    }
    /* pourcentage ou animation */
    if (act_indet) {
        int d = (act_tick / 3) % 4;
        for (int i = 0; i < 4; i++) {
            int cx = 82 + i * 12;
            if (i == d) disc(cx, 110, 4, 1);
            else ring(cx, 110, 4, 1, 1);
        }
        draw_text(56, 126, 1, "EN COURS");
    } else {
        snprintf(b, sizeof(b), "%d %%", act_pct);
        int wt = (int)strlen(b) * 4 - 1;
        draw_text((200 - wt) / 2, 110, 2, b);
    }
    /* le cochon regarde ce qui se passe */
    icon_pig_small(100, 152);
}

/* redessine l'ecran d'activite, au plus 4 fois par seconde (e-paper) */
static void act_show(bool force)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (!force && now - act_last_ms < 900) return;
    act_last_ms = now;
    fb_clear();
    fb_oy = 0;
    draw_activity();
    draw_status_bar();
    if (epd && epd_mux && xSemaphoreTake(epd_mux, pdMS_TO_TICKS(4000)) == pdTRUE) {
        epd->EPD_ShowPart(fb, FB_LEN);
        xSemaphoreGive(epd_mux);
    }
}

static void act_anim_task(void *arg)
{
    while (act_on) {
        act_tick++;
        act_show(true);
        vTaskDelay(pdMS_TO_TICKS(1100));
    }
    act_anim = false;
    vTaskDelete(NULL);
}

static void act_begin(const char *label, bool indet)
{
    snprintf(act_label, sizeof(act_label), "%s", label);
    act_on = true;
    act_indet = indet;
    act_pct = 0;
    act_tick = 0;
    act_show(true);
    /* l'animation vit dans sa propre tache : rafraichir l'e-paper prend
       plusieurs centaines de ms et, dans la boucle d'enregistrement, ce
       temps mort faisait deborder le tampon du micro. */
    if (!act_anim) {
        act_anim = true;
        xTaskCreate(act_anim_task, "anim", 3072, NULL, 2, NULL);
    }
}

/* La police e-paper ne connait que l'ASCII et affiche tout en majuscules :
   on traduit les entites HTML puis on remplace les accents par leur lettre. */
static void infos_nettoie(char *s)
{
    char *l = s, *r = s;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if (c == '&') {                          /* entite HTML */
            int v = -1, saut = 0;
            if (!strncmp(r, "&amp;", 5))       { *l++ = '&'; r += 5; continue; }
            if (!strncmp(r, "&quot;", 6))      { *l++ = '"'; r += 6; continue; }
            if (!strncmp(r, "&apos;", 6))      { *l++ = '\''; r += 6; continue; }
            if (!strncmp(r, "&#39;", 5))       { *l++ = '\''; r += 5; continue; }
            if (!strncmp(r, "&lt;", 4))        { *l++ = '<'; r += 4; continue; }
            if (!strncmp(r, "&gt;", 4))        { *l++ = '>'; r += 4; continue; }
            if (!strncmp(r, "&nbsp;", 6))      { *l++ = ' '; r += 6; continue; }
            if (r[1] == '#') {
                if (r[2] == 'x' || r[2] == 'X') v = (int)strtol(r + 3, NULL, 16);
                else                            v = (int)strtol(r + 2, NULL, 10);
                char *f = strchr(r, ';');
                if (f && v > 0) saut = (int)(f - r) + 1;
            }
            if (saut > 0) {
                if (v < 128) *l++ = (char)v;
                else {                           /* accent numerique */
                    switch (v) {
                    case 0xE0: case 0xE2: case 0xE4: case 0x80: case 0x82: case 0x84: *l++ = 'a'; break;
                    case 0xE7: case 0x87: *l++ = 'c'; break;
                    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
                    case 0x88: case 0x89: case 0x8A: case 0x8B: *l++ = 'e'; break;
                    case 0xEE: case 0xEF: case 0x8E: case 0x8F: *l++ = 'i'; break;
                    case 0xF4: case 0xF6: case 0x94: case 0x96: *l++ = 'o'; break;
                    case 0xF9: case 0xFB: case 0xFC: case 0x99: case 0x9B: case 0x9C: *l++ = 'u'; break;
                    case 0xFF: *l++ = 'y'; break;
                    case 0xA0: *l++ = ' '; break;
                    case 0xAB: case 0xBB: *l++ = '"'; break;
                    default: *l++ = '?'; break;
                    }
                }
                r += saut;
                continue;
            }
            *l++ = ' ';
            r++;
            continue;
        }
        if (c < 0x80) { *l++ = (char)c; r++; continue; }
        if (c == 0xC2 && r[1]) {                 /* ponctuation fine */
            unsigned char b2 = (unsigned char)r[1];
            if (b2 == 0xA0) *l++ = ' ';
            else if (b2 >= 0x91 && b2 <= 0x97) *l++ = (b2 == 0x91 || b2 == 0x92) ? '\'' : ((b2 == 0x96 || b2 == 0x97) ? '-' : '"');
            else if (b2 == 0xB0) *l++ = ' ';
            r += 2;
            continue;
        }
        if (c == 0xC3 && r[1]) {                 /* accents en UTF-8 direct */
            unsigned char b2 = (unsigned char)r[1];
            switch (b2) {
            case 0x80: case 0xA0: case 0xA2: case 0xA4: *l++ = 'a'; break;
            case 0x87: case 0xA7: *l++ = 'c'; break;
            case 0x88: case 0xA8: case 0x89: case 0xA9: case 0x8A: case 0xAA:
            case 0x8B: case 0xAB: *l++ = 'e'; break;
            case 0x8E: case 0xAE: case 0x8F: case 0xAF: *l++ = 'i'; break;
            case 0x94: case 0xB4: case 0x96: case 0xB6: *l++ = 'o'; break;
            case 0x99: case 0x9B: case 0x9C: case 0xB9: case 0xBB: case 0xBC: *l++ = 'u'; break;
            case 0x9F: *l++ = 's'; break;
            case 0xBF: *l++ = 'y'; break;
            default: *l++ = '?'; break;
            }
            r += 2;
            continue;
        }
        if (c == 0xE2 && r[1] && r[2]) { r += 3; continue; }   /* tirets longs, euros... */
        r++;                                                    /* le reste : on jette */
    }
    *l = 0;
    /* les espaces doubles, ca fait des trous dans la ligne */
    for (char *q = s; *q; q++)
        if (q[0] == ' ' && q[1] == ' ') { memmove(q, q + 1, strlen(q)); q--; }
}

/* un pas vers la destination ; arrive, il en choisit une autre au hasard */
static void kirby_promene(void)
{
    int x0 = 26, x1 = 174, y0 = 142, y1 = 156;   /* la zone blanche sous la meteo */
    if ((kirby_vx == 0 && kirby_vy == 0) || (kirby_x == kirby_vx && kirby_y == kirby_vy)) {
        kirby_vx = x0 + (int)(esp_random() % (uint32_t)(x1 - x0 + 1));
        kirby_vy = y0 + (int)(esp_random() % (uint32_t)(y1 - y0 + 1));
    }
    int px = 14, py = 8;                      /* il avance vite a l'horizontale */
    if (kirby_x < kirby_vx) kirby_x += (kirby_vx - kirby_x < px) ? (kirby_vx - kirby_x) : px;
    else if (kirby_x > kirby_vx) kirby_x -= (kirby_x - kirby_vx < px) ? (kirby_x - kirby_vx) : px;
    if (kirby_y < kirby_vy) kirby_y += (kirby_vy - kirby_y < py) ? (kirby_vy - kirby_y) : py;
    else if (kirby_y > kirby_vy) kirby_y -= (kirby_y - kirby_vy < py) ? (kirby_y - kirby_vy) : py;
}

/* il se promene tout seul sur la surface blanche, comme un vrai tamagotchi */
static void promenade_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2500));          /* on laisse la carte demarrer */
    for (;;) {
        /* 2 s : chaque image coute 865 ms et fait clignoter la dalle. A 380 ms
           (avant), le compagnon produisait un stroboscope permanent. */
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!act_on && ui_page == PAGE_MAITRE) {
            kirby_promene();
            show_ui();          /* ne rafraichit que s'il a vraiment bouge */
        }
    }
}

/* barre d'etat, hauteur 26 px, en haut */
static void draw_status_bar(void)
{
    hline(0, 199, 25, 1);
    icon_wifi(14, 12, ui_wifi_ok ? 1 : 0);

    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    int x = 34;
    x += draw_num(x, 6, 2, ti.tm_hour, 2) + 4;
    sq(x, 6, 2); sq(x, 14, 2); x += 6;
    draw_num(x, 6, 2, ti.tm_min, 2);

    icon_temp(124, 10);
    int tt = (int)lroundf(ui_temp);
    x = 132 + draw_num(132, 6, 2, tt, 2) + 3;
    ring(x + 2, 8, 2, 1, 1);
    g3x5(x + 7, 6, 2, GC5);

    /* frequence du processeur. La RAM libre a ete retiree : elle changeait a
       chaque mesure, donc l'image different toujours et la dalle se
       rafraichissait sans arret (clignotement permanent de la page d'accueil). */
    {
        char b[24];
        snprintf(b, sizeof(b), "%dM", (int)(esp_clk_cpu_freq() / 1000000));
        a_text(140, 6, 1, b);
    }
    icon_gear(182, 12, 6, 2);
}

/* ================================================================== */
/* LES APPLIS : une chose par ecran, en gros.                          */
/*                                                                     */
/*   0 BATMAN   le visage, l'expression du moment                      */
/*   1 PARLER   l'assistant vocal : micro, puis ce qu'il dit           */
/*   2 MOI      le tamagotchi : comment il va, ce dont il a besoin     */
/*   3 MEMOIRE  ce qu'il sait                                          */
/*   4 INFOS    processeur, memoire, carte SD, wifi                    */
/*                                                                     */
/* Navigation : un bouton = applis suivante / precedente, appui long   */
/* sur n'importe lequel = parler. Deux gestes, rien d'autre.           */
/* ================================================================== */

/* les 14 familles du modele ramenees a cinq visages */
static int famille_vers_bat5(int famille)
{
    switch (famille) {
    case 0:  return BAT5_CONTENT;    /* joie */
    case 10: return BAT5_CONTENT;    /* tendresse */
    case 9:  return BAT5_CONTENT;    /* fierte */
    case 12: return BAT5_CONTENT;    /* desir */
    case 1:  return BAT5_TRISTE;     /* tristesse */
    case 11: return BAT5_TRISTE;     /* gene */
    case 13: return BAT5_TRISTE;     /* douleur */
    case 2:  return BAT5_FACHE;      /* colere */
    case 5:  return BAT5_FACHE;      /* degout */
    case 3:  return BAT5_SURPRIS;    /* peur */
    case 4:  return BAT5_SURPRIS;    /* surprise */
    default: return BAT5_NEUTRE;     /* interet, reflexion, calme */
    }
}

/* bandeau de titre de chaque appli : nom en haut, gros, et une bande
   noire en bas qui porte le sous-titre (l'expression, la reponse...) */
static void app_titre(const char *nom)
{
    a_efill(0, 12, 0, 0, 0);
    a_hline(0, 199, 0, 3);
    a_hline(0, 199, 25, 3);
    a_text(6, 6, 2, nom);
    /* le rappel des boutons, en petit a droite (pas sur le visage : plein) */
    if (ui_page == PAGE_SYS)
        a_text(196 - 6 * 18, 10, 1, "BOUTON 2 = ACCUEIL");
}

static void app_bas(const char *txt)
{
    a_hline(0, 199, 174, 3);
    a_hline(0, 199, 175, 3);
    a_text_c(100, 182, 2, txt);
}

/* texte en gros, coupe en lignes qui tiennent dans 200 px */
static int texte_grand(int y, int scale, const char *txt, int maxlignes)
{
    int largeur = 200 / (4 * scale);        /* une lettre = 4*scale px */
    int n = 0;
    char ligne[64];
    while (*txt && n < maxlignes) {
        int l = 0;
        if (*txt == ' ') txt++;
        while (txt[l] && l < largeur) l++;
        while (l > 0 && txt[l] != 0 && txt[l] != ' ') l--;   /* coupe au mot */
        if (l == 0) l = largeur;
        if (l >= (int)sizeof(ligne)) l = (int)sizeof(ligne) - 1;
        memcpy(ligne, txt, (size_t)l);
        ligne[l] = 0;
        a_text_c(100, y + n * (7 * scale), scale, ligne);
        txt += l;
        n++;
    }
    return n;
}

/* ---------- 0. MAITRE : le tableau de bord ---------- */

static const char *JOURS_SEM[] = {"DIMANCHE", "LUNDI", "MARDI", "MERCREDI", "JEUDI",
                                  "VENDREDI", "SAMEDI"};
static const char *MOIS_AN[] = {"JANVIER", "FEVRIER", "MARS", "AVRIL", "MAI", "JUIN",
                                "JUILLET", "AOUT", "SEPTEMBRE", "OCTOBRE", "NOVEMBRE", "DECEMBRE"};


/* ================================================================== */
/* LA BANDE : le spectre de la voix, a la place du tamagotchi          */
/* ================================================================== */
/* L'energie de la voix : RMS par bloc de 40 ms, avec auto-gain. C'est elle qui
   ouvre la bouche du compagnon. Volontairement tres peu couteux : une somme de
   carres, pas d'analyse spectrale (un Goertzel 24 bandes ici affamait le CPU et
   gelait tout le reste). */
static volatile float voix_bouche = 0.0f;   /* ouverture de la bouche (0..1) */
static float voix_acc = 0.0f;
static int   voix_cnt = 0;
static float voix_gain = 3000.0f;

static void voix_feed(const int16_t *s, int n)
{
    if (!s || n <= 0) return;
    for (int i = 0; i < n; i++) {
        float x = (float)s[i];
        voix_acc += x * x;
        if (++voix_cnt >= 640) {                 /* 40 ms a 16 kHz */
            float rms = sqrtf(voix_acc / (float)voix_cnt);
            voix_acc = 0; voix_cnt = 0;
            if (rms > voix_gain) voix_gain = rms;          /* auto-gain */
            else voix_gain -= voix_gain * 0.004f;
            if (voix_gain < 200.0f) voix_gain = 200.0f;
            float v = rms / voix_gain;
            if (v > 1.0f) v = 1.0f;
            float cible = v * 2.0f;                        /* les syllabes fortes ouvrent grand */
            if (cible > 1.0f) cible = 1.0f;
            voix_bouche += (cible - voix_bouche) * 0.75f;
        }
    }
}

/* NOTE (18/10) : une tache "visage" animait le compagnon a ~1 image/s dans une
   fenetre partielle. Mesure : un rafraichissement partiel coute 865 ms ET fait
   clignoter toute la dalle -> en boucle, c'etait un stroboscope. Supprime : le
   visage est dessine par la page, et la page n'est rafraichie que lorsque son
   contenu change reellement (voir fb_identique() dans show_ui). */

/* ---------- 1. LA PAGE PRINCIPALE ---------- */
static void draw_app_maitre(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    char b[48];

    /* l'heure, en tres gros, et l'etat du wifi */
    snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min);
    a_text_c(100, 8, 7, b);
    icon_wifi(183, 14, ui_wifi_ok ? 1 : 0);

    snprintf(b, sizeof(b), "%s %d %s", JOURS_SEM[ti.tm_wday % 7], ti.tm_mday,
             MOIS_AN[ti.tm_mon % 12]);
    a_text_c(100, 52, 1, b);
    a_hline(6, 193, 62, 1);

    if (voix_etat != 0) {
        /* Laura est la : son compagnon passe au premier plan, en grand, et sa
           bouche suit les syllabes quand elle parle. */
        a_text(8, 76, 1, "LAURA");
        int d = (esp_timer_get_time() / 1000) % 1600 < 800;
        if (d) { for (int dx = 0; dx < 3; dx++) for (int dy = 0; dy < 3; dy++)
                     px(192 + dx, 76 + dy, 1); }
        /* pas d'animation possible sur cette dalle : une image fixe par etat */
        draw_kirby(100, 126, 40, anim_ph, 0,
                   voix_etat == 3 ? 0.55f : 0.0f);
        a_text_c(100, 176, 2, voix_etat == 1 ? "ECOUTE" :
                             voix_etat == 2 ? "REFLEXION" : "JE PARLE");
        a_hline(6, 193, 188, 1);
        return;
    }

    /* la meteo du jour */
    ic_temp(22, 82);
    if (meteo_ok) {
        snprintf(b, sizeof(b), "%.1f", (double)meteo_temp);
        a_text(38, 76, 3, b);
        a_text(38 + a_text_w(3, b) + 4, 76, 3, "C");
    } else {
        a_text(38, 76, 3, "--");
    }
    /* ce qu'il consomme, aligne a droite de la temperature */
    snprintf(b, sizeof(b), "%.1f W", conso_mw() / 1000.0);
    a_text(196 - a_text_w(2, b), 80, 2, b);

    a_text_c(100, 100, 2, meteo_texte(meteo_code));
    a_text_c(100, 116, 1, METEO_VILLE);

    /* le petit compagnon se promene dans la zone blanche (tache promenade) */
    draw_kirby(kirby_x, kirby_y, 15, anim_ph, kirby_etat());

    a_hline(6, 193, 188, 1);
}

/* ---------- 2. PAROLES : tout ce qui s'est dit, en texte ---------- */
#define PAROLES_MAX 48
static char paroles_lignes[PAROLES_MAX][49];
static int  paroles_tete = 0;      /* prochaine case libre */
static int  paroles_n = 0;         /* lignes memorisees */
static int  paroles_haut = 0;      /* 0 = on voit la fin, +1 = une ligne plus haut */

static void paroles_ajoute(const char *qui, const char *texte)
{
    char prefixe[8];
    snprintf(prefixe, sizeof(prefixe), "%s: ", qui);
    const char *p = texte;
    int premier = 1;
    while (*p) {
        int place = 48 - (premier ? (int)strlen(prefixe) : 3);
        int n = 0;
        while (p[n] && n < place) n++;
        if (p[n]) {                                   /* coupe au dernier mot entier */
            int k = n;
            while (k > 8 && p[k] != ' ') k--;
            if (k > 8) n = k;
        }
        char ligne[49];
        snprintf(ligne, sizeof(ligne), "%s%.*s", premier ? prefixe : "   ", n, p);
        snprintf(paroles_lignes[paroles_tete], sizeof(paroles_lignes[0]), "%s", ligne);
        paroles_tete = (paroles_tete + 1) % PAROLES_MAX;
        if (paroles_n < PAROLES_MAX) paroles_n++;
        p += n;
        while (*p == ' ') p++;
        premier = 0;
    }
    paroles_haut = 0;                                   /* on revient sur la fin */
}

static void draw_app_paroles(void)
{
    app_titre("PAROLES");
    const int par_ecran = 9;
    int base = paroles_n - par_ecran - paroles_haut;
    if (base < 0) base = 0;
    for (int i = 0; i < par_ecran; i++) {
        int a = base + i;
        if (a >= paroles_n) break;
        int idx = (paroles_tete - paroles_n + a + PAROLES_MAX * 2) % PAROLES_MAX;
        a_text(4, 34 + i * 15, 1, paroles_lignes[idx]);
    }
    if (paroles_n == 0)
        a_text_c(100, 96, 2, "PARLE-MOI !");
    a_hline(0, 199, 172, 2);
    char b[48];
    snprintf(b, sizeof(b), "LIGNE %d/%d", paroles_n - paroles_haut > 0 ? paroles_n - paroles_haut : 0, paroles_n);
    a_text(4, 180, 1, b);
    a_text(196 - 6 * 16, 180, 1, "1=DEFILER 2=PAGE");
}

/* ---------- 1. PARLER ---------- */
/* etat de l'assistant : 0 au repos, 1 j'ecoute, 2 je reflechis, 3 je reponds */
// voix_etat declare plus haut (ligne 208)
static char voix_reponse[300] = "";
static char voix_entendu_txt[300] = "";  /* dernier texte reconnu (lu par /state) */
static volatile int assist_parle_ms = 8000;   /* duree d'ecoute (8 s par defaut) */
static volatile bool parler_radio_ouverte = false;  /* test : ne pas couper la radio en ecoutant */

static void draw_micro(int cx, int cy, int h)
{
    int w = h / 3;
    a_efill(cx, cy - h / 3, w / 2, h / 3, 1);          /* capsule */
    a_ering(cx, cy - h / 3, w / 2, h / 3, 4, 1);
    a_arc(cx, cy + 2, w / 2 + 10, 200, 340, 4);        /* arceau */
    a_vline(cx, cy + 14, cy + 26, 4);                  /* pied */
    a_hline(cx - 16, cx + 16, cy + 28, 6);             /* socle */
}

static void draw_app_infos(void)
{
    app_titre("INFOS");
    if (!infos_ok) {
        texte_grand(80, 2, "JE CHERCHE LES INFOS...", 2);
        return;
    }
    int y = 32;
    for (int i = infos_haut; i < infos_n && y < 176; i++) {
        int n = texte_grand(y, 1, infos_titre[i], 4);
        y += n * 7 + 5;
        if (i + 1 < infos_n && y < 176) a_hline(8, 191, y - 3, 1);
    }
}

static void draw_app_parler(void)
{
    app_titre("PARLER");
    switch (voix_etat) {
    case 1:
        draw_micro(100, 76, 46);
        texte_grand(132, 2, "PARLE, JE T'ECOUTE", 2);
        break;
    case 2:
        draw_micro(100, 76, 46);
        texte_grand(132, 2, "JE REFLECHIS...", 2);
        break;
    case 3:
        /* la reponse : l'ecran entier pour le texte. Aucun dessin devant,
           sinon on ne lit rien. */
        texte_grand(34, 2, voix_reponse, 8);
        break;
    default:
        draw_micro(100, 72, 52);
        texte_grand(134, 2, "RESTE APPUYE SUR LE BOUTON DU HAUT, PUIS PARLE", 3);
        break;
    }
}

/* ---------- 2. MOI (le tamagotchi) ---------- */
static void draw_app_moi(void)
{
    char b[64];
    pet_action_t besoin;
    uint8_t coeurs;

    draw_kirby(100, 92, 38, anim_ph, kirby_etat());
    app_titre("MOI");
    besoin = pet_besoin(&pet);
    coeurs = (uint8_t)(pet.bonheur / 100);
    if (coeurs > 4) coeurs = 4;
    switch (besoin) {
    case ACT_MANGER:   snprintf(b, sizeof(b), "J AI FAIM"); break;
    case ACT_JOUER:    snprintf(b, sizeof(b), "JE M ENNUIE"); break;
    case ACT_CALIN:    snprintf(b, sizeof(b), "UN CALIN ?"); break;
    case ACT_NETTOYER: snprintf(b, sizeof(b), "C EST SALE"); break;
    case ACT_SOIGNER:  snprintf(b, sizeof(b), "JE SUIS MALADE"); break;
    case ACT_DORMIR:   snprintf(b, sizeof(b), "JE DORS"); break;
    default:           snprintf(b, sizeof(b), "%s", pet_message(&pet)); break;
    }
    app_bas(b);
    /* quatre coeurs, gros, en haut a droite */
    for (int i = 0; i < 4; i++) ic_heart(152 + i * 12, 18, 10, i < coeurs);
}

/* ---------- 3. MEMOIRE ---------- */
static void draw_app_memoire(void)
{
    cJSON *notes = cJSON_GetObjectItem(mem_obj, "notes");
    int n = notes ? cJSON_GetArraySize(notes) : 0;
    char b[64];
    app_titre("MA MEMOIRE");
    if (n == 0) {
        texte_grand(60, 2, "JE NE SAIS RIEN ENCORE. PARLE MOI ET J APPRENDS.", 5);
        return;
    }
    snprintf(b, sizeof(b), "JE SAIS %d CHOSES", n);
    a_text_c(100, 34, 2, b);
    for (int i = 0; i < n && i < 6; i++) {
        cJSON *it = cJSON_GetArrayItem(notes, n - 1 - i);
        if (!cJSON_IsString(it)) continue;
        snprintf(b, sizeof(b), "%.42s", it->valuestring);
        a_text(8, 60 + i * 18, 1, b);
        a_text(0, 60 + i * 18, 1, "*");
    }
}


/* tout ce qu'on lit sur la carte pour l'appli INFOS */
static void monde_maj_info(monde_info_t *mi)
{
    time_t now = time(NULL);
    struct tm ti;
    uint64_t tot = 0, libre = 0;
    localtime_r(&now, &ti);
    memset(mi, 0, sizeof(*mi));
    mi->cpu_mhz = (int)(esp_clk_cpu_freq() / 1000000);
    mi->ram_libre_ko = (int)(esp_get_free_heap_size() / 1024);
    mi->psram_libre_ko = (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    mi->heure = ti.tm_hour;
    mi->minute = ti.tm_min;
    mi->wifi_ok = ui_wifi_ok;
    mi->temp = ui_temp;
    if (esp_vfs_fat_info("/sdcard", &tot, &libre) == ESP_OK) {
        mi->sd_go = (int)(tot / (1024ull * 1024ull * 1024ull));
        mi->sd_libre_go = (int)(libre / (1024ull * 1024ull * 1024ull));
    }
}

/* ---------- 5. RADIO (les webradios du monde, jouees par le PC) ---------- */
#define RADIOS_FICHIER "/sdcard/mino/radios.txt"
#define RADIOS_MAX 16

static char radios_nom[RADIOS_MAX][28];
static char radios_url[RADIOS_MAX][128];
static int  radios_n = 0;
static int  radio_sel = 0;
static int  radio_etat = 0;      /* 0 arret, 1 je me connecte, 2 ca joue */

/* le minuteur de la page radio : quand il arrive a zero, il allume FG Radio */
static volatile int64_t min_fin_us = 0;     /* 0 = pas de minuteur */
static int min_preset_s = 0;                /* 5, 10, 15, 30, 60 puis 0 */

static int minuteur_restant_s(void)
{
    if (min_fin_us == 0) return 0;
    int64_t r = (min_fin_us - esp_timer_get_time()) / 1000000;
    return (r > 0) ? (int)r : 0;
}

/* ===== PAGE TIMER / ALARME : style Apple =====
   On gere plusieurs timers (compte a rebours) et alarmes (heure fixe).
   Tout est en NVS pour survivre au reboot. */
#define TIMER_MAX 8
struct timer_item_t {
    int64_t fin_s;           /* 0 = inactif, sinon echeance en heure reelle (epoch s) */
    int     duree_s;         /* duree originale en secondes */
    char    nom[24];         /* "Cuisson", "Sieste", etc. */
    bool    est_alarme;      /* false = timer, true = alarme (heure fixe) */
    bool    repet;           /* pour alarme : tous les jours */
    bool    sonne;           /* a sonne, en attente d'acquit */
};
static struct timer_item_t timers[TIMER_MAX];
static int timer_sel = 0;           /* celui qu'on edite/regarde */
static int timer_mode = 0;          /* 0 = liste, 1 = edition duree, 2 = edition nom, 3 = confirmation */
static int timer_edit_val = 0;      /* valeur en cours d'edition (secondes ou heures/minutes) */
static bool timer_edit_heures = true; /* en mode alarme : on edite heures puis minutes */

static void timers_charge(void)
{
    nvs_handle_t nh;
    if (nvs_open("mino", NVS_READONLY, &nh) != ESP_OK) return;
    size_t sz = sizeof(timers);
    nvs_get_blob(nh, "timers", timers, &sz);
    nvs_close(nh);
    /* nettoyage : on garde ce qui est encore a venir, on jette le reste
       (une echeance doit tomber dans une plage plausible : 2024 .. 2100) */
    time_t now = time(NULL);
    for (int i = 0; i < TIMER_MAX; i++) {
        int64_t f = timers[i].fin_s;
        if (f == 0) continue;
        if (f < 1700000000LL || f > 4102444800LL || (!timers[i].sonne && f <= (int64_t)now)) {
            memset(&timers[i], 0, sizeof(timers[i]));
        }
    }
}

static void timers_sauve(void)
{
    nvs_handle_t nh;
    if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_blob(nh, "timers", timers, sizeof(timers));
        nvs_commit(nh);
        nvs_close(nh);
    }
}
static void timers_sauve(void);

/* La carte n'a pas de capteur de courant (seulement des pins d'alimentation) :
   on additionne ce qui consomme vraiment, mesure par mesure. */
static int conso_mw(void)
{
    int mw = 160;                                   /* soc + carte, au repos */
    if (esp_clk_cpu_freq() > 200000000) mw += 90;   /* coeur a 240 MHz */
    if (ui_wifi_ok)                     mw += 110;  /* wifi associe */
    if (radio_etat != 0)                mw += 275;  /* le haut-parleur joue */
    if (voix_etat != 0)                 mw += 175;  /* il parle */
    if (act_on)                         mw += 60;   /* l'ecran se rafraichit souvent */
    return mw;
}
static volatile int  radio_gen = 0;      /* change -> la tache se reconnecte */
static volatile bool radio_vive = false; /* la tache radio tourne */
static int  radio_vol = 70;              /* volume du haut-parleur, 0..100 */
static char radio_ordre[40] = "";

/* est-ce que "nom" contient "motif" ? (sans se soucier des majuscules) */
static int nom_contient(const char *nom, const char *motif)
{
    size_t ln = strlen(motif);
    if (!ln) return 1;
    for (const char *p = nom; *p; p++) {
        size_t i = 0;
        while (i < ln && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)motif[i])) i++;
        if (i == ln) return 1;
    }
    return 0;
}        /* ce que l'IA vient de demander */
static int  vol_ordre = -1;              /* volume demande par l'IA, -1 = rien */

static void radio_vol_regle(int v, int enregistre)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    radio_vol = v;
    audio_playback_set_vol((uint8_t)v);
    if (enregistre) {
        nvs_handle_t nh;
        if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_i32(nh, "vol", (int32_t)radio_vol);
            nvs_commit(nh);
            nvs_close(nh);
        }
    }
    ESP_LOGI(TAG, "volume %d%%", radio_vol);
}

static void radio_vol_charge(void)
{
    nvs_handle_t nh;
    int32_t v = 70;
    if (nvs_open("mino", NVS_READONLY, &nh) == ESP_OK) {
        nvs_get_i32(nh, "vol", &v);
        nvs_close(nh);
    }
    radio_vol_regle((int)v, 0);
}

/* la premiere ligne sert de version : si elle change, la liste du robot est
   remplacee. Tes propres stations s'ajoutent en dessous, elles sont gardees. */
#define RADIOS_VERSION "# BATMINO 3"
static const char *RADIOS_DEFAUT =
    RADIOS_VERSION "\n"
    "FG Radio | https://stream.rcs.revma.com/7081n2b6p98uv\n"
    "FIP | https://icecast.radiofrance.fr/fip-midfi.mp3\n"
    "FIP Rock | https://icecast.radiofrance.fr/fiprock-midfi.mp3\n"
    "FIP Groove | https://icecast.radiofrance.fr/fipgroove-midfi.mp3\n"
    "FIP Jazz | https://icecast.radiofrance.fr/fipjazz-midfi.mp3\n"
    "FIP Electro | https://icecast.radiofrance.fr/fipelectro-midfi.mp3\n"
    "FIP Pop | https://icecast.radiofrance.fr/fippop-midfi.mp3\n"
    "Radio Nova | https://novazz.ice.infomaniak.ch/novazz-128.mp3\n"
    "Radio Meuh | https://radiomeuh.ice.infomaniak.ch/radiomeuh-128.mp3\n"
    "Jazz Radio | https://jazzradio.ice.infomaniak.ch/jazzradio-high.mp3\n"
    "TSF Jazz | https://tsfjazz.ice.infomaniak.ch/tsfjazz-high.mp3\n"
    "France Musique | https://icecast.radiofrance.fr/francemusique-midfi.mp3\n"
    "franceinfo | https://icecast.radiofrance.fr/franceinfo-midfi.mp3\n"
    "France Inter | https://icecast.radiofrance.fr/franceinter-midfi.mp3\n"
    "Skyrock | https://icecast.skyrock.net/s/natio_aac_128k\n"
    "Bonheur FM | https://ice2.somafm.com/groovesalad-128-mp3\n";

static int nom_contient(const char *nom, const char *motif);

/* Les stations du robot sont toujours la. Le fichier sur la carte ajoute
   les tiennes : chaque ligne "Nom | http://..." vient en plus. Comme ca, un
   fichier abime ne peut jamais nous priver de toutes les radios. */
static int radios_ajoute(const char *nom, const char *url)
{
    if (radios_n >= RADIOS_MAX) return 0;
    if (!nom || !url || !*nom || !*url) return 0;
    if (strncmp(url, "http", 4) != 0) return 0;
    for (int i = 0; i < radios_n; i++) {               /* deja connue ? */
        const char *a = radios_nom[i], *b = nom;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return 0;
    }
    snprintf(radios_nom[radios_n], sizeof(radios_nom[0]), "%.27s", nom);
    snprintf(radios_url[radios_n], sizeof(radios_url[0]), "%.127s", url);
    radios_n++;
    return 1;
}

static int radios_fichier_ok = 0;      /* le fichier porte-t-il la bonne version ? */
static int radios_dernier_lu = 0;      /* combien de lignes utiles lues sur la carte */

static int radios_lit_fichier(void)
{
    FILE *f = fopen(RADIOS_FICHIER, "r");
    char ligne[220];
    int dans_fichier = 0;
    radios_fichier_ok = 0;

    if (f) {
        if (fgets(ligne, sizeof(ligne), f) && !strncmp(ligne, RADIOS_VERSION, strlen(RADIOS_VERSION)))
            radios_fichier_ok = 1;                     /* le fichier est a jour */
        else
            rewind(f);
        while (fgets(ligne, sizeof(ligne), f)) {
            char *bar, *nl;
            if (ligne[0] == '#') continue;
            bar = strchr(ligne, '|');
            if (!bar) continue;
            nl = strchr(ligne, '\n');
            if (nl) *nl = 0;
            *bar = 0;
            {
                char *nom = ligne, *url = bar + 1, *fin;
                while (*nom == ' ') nom++;
                fin = nom + strlen(nom);
                while (fin > nom && fin[-1] == ' ') *--fin = 0;
                while (*url == ' ') url++;
                fin = url + strlen(url);
                while (fin > url && fin[-1] == ' ') *--fin = 0;
                if (radios_ajoute(nom, url)) dans_fichier++;
            }
        }
        fclose(f);
    }
    radios_dernier_lu = dans_fichier;
    return dans_fichier;
}

/* la liste du robot, toujours presente, quoi qu'il arrive au fichier */
static void radios_ajoute_defauts(void)
{
    const char *p = RADIOS_DEFAUT;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *bar = strchr(p, '|');
        if (nl && bar && bar < nl) {
            char nom[64], url[160];
            int ln = (int)(bar - p), lu = (int)(nl - bar - 1);
            const char *u;
            if (ln > 63) ln = 63;
            memcpy(nom, p, (size_t)ln);
            nom[ln] = 0;
            while (ln > 0 && nom[ln - 1] == ' ') nom[--ln] = 0;
            u = bar + 1;
            while (*u == ' ') u++;
            lu = (int)(nl - u);
            if (lu < 0) lu = 0;
            if (lu > 159) lu = 159;
            memcpy(url, u, (size_t)lu);
            url[lu] = 0;
            if (!nom_contient(nom, "# BATMINO")) radios_ajoute(nom, url);
        }
        p = nl ? nl + 1 : p + strlen(p);
    }
}

static void radios_charge(void)
{
    int dans_fichier;
    radios_n = 0;
    dans_fichier = radios_lit_fichier();          /* d'abord tes stations */
    radios_ajoute_defauts();                      /* puis celles du robot */

    /* le fichier sert de modele : on l'ecrit s'il manque ou s'il est d'une
       vieille version, puis on relit tout pour garder le bon ordre */
    if (sd_ok && (!radios_fichier_ok || dans_fichier == 0)) {
        FILE *w = fopen(RADIOS_FICHIER, "w");
        if (w) {
            const char *p = RADIOS_DEFAUT;
            while (*p) {                       /* ligne par ligne : la SD aime bien */
                const char *nl = strchr(p, '\n');
                size_t n = nl ? (size_t)(nl - p + 1) : strlen(p);
                if (fwrite(p, 1, n, w) != n) break;
                p += n;
            }
            if (ferror(w)) {
                ESP_LOGW(TAG, "la carte a refuse l'ecriture des radios");
                fclose(w);
                remove(RADIOS_FICHIER);
            } else {
                fclose(w);
                ESP_LOGI(TAG, "liste des radios ecrite sur la carte");
                radios_n = 0;
                radios_lit_fichier();
                radios_ajoute_defauts();
            }
        }
    }
    /* et on commence sur FG Radio, c'est la sienne */
    for (int i = 0; i < radios_n; i++)
        if (nom_contient(radios_nom[i], "FG Radio")) { radio_sel = i; break; }
    ESP_LOGI(TAG, "radios : %d station(s)", radios_n);
}

/* La radio se debrouille toute seule : la carte ouvre le flux en wifi,
   le decode et le joue. Le PC n'a plus rien a voir la-dedans. */
static void radio_joue(void);

/* le minuteur sonne : on met FG Radio, assez fort, et on montre la page radio */
static void minuteur_sonne(void)
{
    ESP_LOGW(TAG, "minuteur : c'est l'heure, j'allume FG Radio");
    for (int i = 0; i < radios_n; i++)
        if (nom_contient(radios_nom[i], "FG Radio")) { radio_sel = i; break; }
    if (radio_vol < 80) radio_vol_regle(80, 1);
    ui_page = PAGE_MAITRE;
    radio_joue();
    ui_beep(); ui_beep(); ui_beep();
    show_ui();
}

/* Module radio : DESACTIVE pour l'instant (Taz n'en veut pas encore).
   Reactivable sans reflasher : GET /radio?actif=1  (garde en NVS). */
static bool radio_actif = false;

static void radio_joue(void)
{
    if (!radio_actif) { ESP_LOGW(TAG, "radio desactivee (reactiver : /radio?actif=1)"); return; }
    if (radios_n <= 0) return;
    radio_gen++;                               /* nouvelle ecoute */
    radio_etat = 1;
    show_ui();
    if (!radio_vive) {
        radio_vive = true;
        xTaskCreate(radio_wifi_task, "radio", 12288, NULL, 3, NULL);
    }
}

static void timer_sonne(int i)
{
    if (i < 0 || i >= TIMER_MAX) return;
    timers[i].sonne = true;
    timers_sauve();
    ESP_LOGW(TAG, "timer/alarme %d sonne : %s", i, timers[i].nom);
    ui_page = PAGE_MAITRE;
    timer_sel = i;
    timer_mode = 0;
    ui_beep(); ui_beep(); ui_beep(); ui_beep();
    show_ui();
    /* on allume FG Radio comme pour le minuteur radio */
    for (int k = 0; k < radios_n; k++)
        if (nom_contient(radios_nom[k], "FG Radio")) { radio_sel = k; break; }
    if (radio_vol < 80) radio_vol_regle(80, 1);
    radio_joue();
}

static void radio_stop(void)
{
    if (radio_etat != 0) {
        radio_etat = 0;
        radio_gen++;                           /* la tache va lacher le flux */
        radio_mode = false;
        ESP_LOGI(TAG, "radio : stop");
        show_ui();
    }
}

static void radio_change(int d)
{
    if (radios_n <= 0) return;
    radio_sel = (radio_sel + d + radios_n) % radios_n;
    if (radio_etat != 0) {                     /* on change de station en direct */
        radio_gen++;
        radio_etat = 1;
        show_ui();
    } else {
        radio_joue();
    }
}


static void draw_app_radio(void)
{
    char b[48];
    app_titre("RADIO");
    if (radios_n <= 0) {
        texte_grand(40, 2, "AUCUNE STATION. ECRIS LES DANS RADIOS.TXT SUR LA CARTE.", 6);
        return;
    }
    snprintf(b, sizeof(b), "%d/%d", radio_sel + 1, radios_n);
    a_text(6, 34, 2, b);
    texte_grand(44, 3, radios_nom[radio_sel], 2);          /* le nom, en gros */

    /* l'etat, dans un cadre ovale */
    a_ering(100, 122, 58, 22, 3, 1);
    if (radio_etat == 0) {
        a_text_c(100, 116, 2, "A L ARRET");
        a_text_c(100, 150, 1, "APPUI LONG 1 = JOUER");
    } else if (radio_etat == 1) {
        a_text_c(100, 116, 2, "JE CHARGE...");
        a_text_c(100, 150, 1, "JE ME CONNECTE AU WIFI");
    } else {
        a_text_c(100, 116, 2, "EN LECTURE");
        a_text_c(100, 150, 1, "CA JOUE");
        for (int i = 0; i < 5; i++)                        /* petites ondes qui bougent */
            a_vline(84 + i * 8, 152 - (i % 3) * 4, 158 + (i % 3) * 4, 3);
    }

    /* le volume, en gros : une barre de dix morceaux */
    {
        int pleins = (radio_vol + 5) / 10;
        char vb[24];
        a_text(6, 166, 1, "VOLUME");
        for (int i = 0; i < 10; i++) {
            int x = 54 + i * 12, y = 164;
            if (i < pleins) {
                for (int a = 0; a < 11; a++) for (int bb = 0; bb < 11; bb++) a_px(x + a, y + bb, 1);
            } else {
                for (int a = 0; a < 11; a++) { a_px(x + a, y, 1); a_px(x + a, y + 10, 1); }
                for (int bb = 0; bb < 11; bb++) { a_px(x, y + bb, 1); a_px(x + 10, y + bb, 1); }
            }
        }
        snprintf(vb, sizeof(vb), "%d", radio_vol);
        a_text(178, 166, 1, vb);
    }
    if (radio_etat == 2)
        a_text_c(100, 182, 1, "COURT 1 = PLUS FORT   COURT 2 = MOINS FORT");
    else
        a_text_c(100, 182, 1, "COURT 1 = STATION SUIV.   COURT 2 = PRECED.");
    /* le minuteur : quand il arrive a zero, FG Radio se met en route */
    if (min_fin_us != 0) {
        int r = minuteur_restant_s();
        snprintf(b, sizeof(b), "MINUTEUR %d:%02d PUIS FG RADIO", r / 60, r % 60);
        a_text_c(100, 192, 1, b);
    } else if (radio_etat == 0) {
        a_text_c(100, 192, 1, "COURT 1 = DEMARRE    COURT 2 = STATION -");
        a_text_c(100, 182, 1, "LONG 1 = JOUER     LONG 2 = SUIVANTE");
    }
}


/* ===== PAGE TIMER / ALARME : liste facon iOS (le dessin est partage avec l'apercu PC) =====
   Rappel : un appui COURT change toujours de page, un appui LONG agit.
     LONG 1 = cree un timer (avec la duree choisie) / arrete la sonnerie
     LONG 2 = change la duree : 5, 10, 15, 30, 60 min puis off
     (le minuteur radio reste sur le bouton 2 des autres pages)                     */
static void draw_app_timer(void)
{
    static char noms[3][48];
    static char sous[3][48];
    static const char *pn[3] = {noms[0], noms[1], noms[2]};
    static const char *ps[3] = {sous[0], sous[1], sous[2]};
    static int pct[3];
    int n = 0, sel = -1, sonne = -1;
    char b1[64], b2[64];

    for (int i = 0; i < TIMER_MAX && n < 3; i++) {
        if (!timers[i].fin_s) continue;
        snprintf(noms[n], sizeof(noms[0]), "%.20s", timers[i].nom);
        int reste = (int)(timers[i].fin_s - (int64_t)time(NULL));
        if (reste < 0) reste = 0;
        int total = timers[i].duree_s > 0 ? timers[i].duree_s : 1;
        int ecoule = total - reste;
        if (ecoule < 0) ecoule = 0;
        pct[n] = (100 * ecoule) / total;
        if (timers[i].sonne) {
            snprintf(sous[n], sizeof(sous[0]), "SONNE !");
            sonne = n;
        } else if (timers[i].est_alarme) {
            time_t t = (time_t)timers[i].fin_s;
            struct tm tm;
            localtime_r(&t, &tm);
            snprintf(sous[n], sizeof(sous[0]), "%02d:%02d%.11s", tm.tm_hour, tm.tm_min,
                     timers[i].repet ? " tous jours" : "");
        } else {
            snprintf(sous[n], sizeof(sous[0]), "%d:%02d / %d:%02d",
                     reste / 60, reste % 60, total / 60, total % 60);
        }
        n++;
    }
    if (sonne >= 0) sel = sonne;
    else if (n > 0) sel = 0;

    if (min_preset_s > 0) snprintf(b2, sizeof(b2), "DUREE CHOISIE %d MIN", (int)(min_preset_s / 60));
    else                  snprintf(b2, sizeof(b2), "DUREE : ARRET");

    if (sonne >= 0) {
        snprintf(b1, sizeof(b1), "N'IMPORTE QUEL APPUI ARRETE");
        ui_ecran_timer("TIMER", "CA SONNE !", pn, ps, pct, n, sel, b1, b2);
    } else {
        snprintf(b1, sizeof(b1), "LONG 1 = AJOUTE   LONG 2 = DUREE");
        ui_ecran_timer("TIMER", "", pn, ps, pct, n, sel, b1, b2);
    }
}

static char journal_lignes[12][64];
static int  journal_n = 0, journal_tete = 0;

static void journal_ajoute(const char *texte)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(journal_lignes[journal_tete], sizeof(journal_lignes[0]),
             "%02d:%02d %s", ti.tm_hour, ti.tm_min, texte);
    journal_tete = (journal_tete + 1) % 12;
    if (journal_n < 12) journal_n++;
    /* la carte SD garde tout : un fichier par jour */
    if (sd_ok) {
        char chemin[80];
        FILE *f;
        snprintf(chemin, sizeof(chemin), "/sdcard/mino/journal-%04d%02d%02d.md",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        f = fopen(chemin, "a");
        if (f) {
            fprintf(f, "- %02d:%02d %s (stade %s, age %u j, sante %u%%)\n",
                    ti.tm_hour, ti.tm_min, texte, pet_stade_nom(pet_stade(&pet)),
                    (unsigned)(pet.age_s / 86400), pet_sante(&pet));
            fclose(f);
        }
    }
}

static void draw_page_journal(void)
{
    a_text_c(100, 30, 2, "MON JOURNAL");
    for (int i = 0; i < journal_n && i < 12; i++)
        a_text(6, 48 + i * 12, 1, journal_lignes[(journal_tete + 12 - journal_n + i) % 12]);
}

static void draw_page_clock(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    int s = 9, y = 48;
    int w = 2 * 4 * s - s + 4 * s + 2 * 4 * s - s;
    int x = (200 - w) / 2;
    x += draw_num(x, y, s, ti.tm_hour, 2) + 3 * s / 2;
    sq(x, y + 2 * s, 2 * s); sq(x, y + 5 * s, 2 * s);
    x += 4 * s;
    draw_num(x, y, s, ti.tm_min, 2);

    int ys = 156, ss = 4;
    int dx = 22;
    dx += draw_num(dx, ys, ss, ti.tm_mday, 2) + 3;
    seg(dx + 2, ys + 3 * ss, dx + 5 * ss - 2, ys, 1); dx += 5 * ss;
    dx += draw_num(dx, ys, ss, ti.tm_mon + 1, 2) + 3;
    seg(dx + 2, ys + 3 * ss, dx + 5 * ss - 2, ys, 1); dx += 5 * ss;
    draw_num(dx, ys, ss, (ti.tm_year + 1900) % 100, 2);
}

static void draw_page_info(void)
{
    int t10 = (int)lroundf(ui_temp * 10.0f);
    int s = 8, x = 18, y = 52;
    x += draw_num(x, y, s, t10 / 10, 2) + s;
    disc(x + 2, y + 5 * s - s, s / 2 + 1, 1);
    x += 2 * s;
    x += draw_num(x, y + s, s - 3, t10 % 10 < 0 ? -t10 % 10 : t10 % 10, 1) + s;
    ring(x + s, y + s, s, 2, 1);
    g3x5(x + 3 * s, y, s, GC5);

    int hs = 5, hx = 52, hy = 150;
    hx += draw_num(hx, hy, hs, (int)lroundf(ui_humi), 2) + hs;
    g3x5(hx, hy, hs, GP5);
    if (!ui_shtc_ok) {
        seg(150, 140, 185, 190, 2);
        seg(185, 140, 150, 190, 2);
    }
}

typedef struct { int x0, y0, x1, y1; } ui_btn_t;
static const ui_btn_t BTN[4] = {
    { 10,  34,  95, 112},
    {105,  34, 190, 112},
    { 10, 118,  95, 196},
    {105, 118, 190, 196},
};

static void box_draw(const ui_btn_t *b)
{
    for (int x = b->x0; x <= b->x1; x++) {
        px(x, b->y0, 1); px(x, b->y0 + 1, 1);
        px(x, b->y1, 1); px(x, b->y1 - 1, 1);
    }
    for (int y = b->y0; y <= b->y1; y++) {
        px(b->x0, y, 1); px(b->x0 + 1, y, 1);
        px(b->x1, y, 1); px(b->x1 - 1, y, 1);
    }
}

static void draw_page_set(void)
{
    for (int i = 0; i < 4; i++) {
        const ui_btn_t *b = &BTN[i];
        box_draw(b);
        int cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2 + 4;
        switch (i) {
        case 0: icon_pig_small(cx, cy); break;
        case 1: icon_speaker(cx, cy); break;
        case 2: icon_wifi(cx, cy, ui_wifi_ok ? 1 : 0); break;
        case 3: icon_power(cx, cy, 11); break;
        }
    }
}

/* ================================================================== */
/* PAGE SYSTEM : le mini btop (cpu, ram, gpu, watts, batterie,        */
/* reseau, stockage). C'est la seule page qui se rafraichit seule.    */
/* ================================================================== */

#define SYS_HIST 38
static int sys_cpu = 0;        /* 0..100, arrondi (pour /state et le graphe) */
static int sys_cpu10 = 0;      /* 0..1000, pour afficher "0.9 %" */
static uint8_t sys_hist[SYS_HIST];
static int sys_hist_n = 0, sys_hist_tete = 0;

/* Compteurs d'execution FreeRTOS : la somme des temps de tache donne le temps
   reellement occupe, sur les deux coeurs. (Le hook idle ne marchait pas ici :
   il ne se declenchait pas assez souvent a 100 Hz pour etre exploitable.) */
static TaskStatus_t *sys_ts = NULL;      /* alloue seulement sur la page SYSTEM */

static void sys_cpu_maj(void)
{
    static uint32_t somme0 = 0, total0 = 0;
    if (!sys_ts) return;
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(sys_ts, 48, &total);
    uint32_t somme = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        /* les taches idle comptent comme du temps occupe : on les exclut,
           sinon la somme vaut toujours 100 % des deux coeurs */
        const char *nm = sys_ts[i].pcTaskName ? sys_ts[i].pcTaskName : "";
        if (!strncmp(nm, "IDLE", 4)) continue;
        somme += (uint32_t)sys_ts[i].ulRunTimeCounter;
    }
    if (!total0) { somme0 = somme; total0 = total; return; }
    uint32_t dsomme = somme - somme0, dtot = total - total0;
    somme0 = somme; total0 = total;
    int pct10 = 0;
    if (dtot > 0 && dtot < 50000000u)
        pct10 = (int)((uint64_t)dsomme * 1000 / (uint64_t)dtot);  /* en "un coeur" */
    if (pct10 < 0) pct10 = 0;
    if (pct10 > 1000) pct10 = 1000;
    sys_cpu10 = pct10;
    sys_cpu = (pct10 + 5) / 10;
    sys_hist[sys_hist_tete] = (uint8_t)sys_cpu;
    sys_hist_tete = (sys_hist_tete + 1) % SYS_HIST;
    if (sys_hist_n < SYS_HIST) sys_hist_n++;
}

/* --- batterie : GPIO4 (BAT_ADC), pont 200K/200K -> VBAT = 2 x VADC --- */
static adc_oneshot_unit_handle_t sys_adc = NULL;
static bool sys_adc_init = false;
static int sys_bat_mv_cache = -1;    /* derniere mesure, pour /state (sans ADC) */
static int sys_bat_pct_cache = -1;

static int sys_bat_mv(void)
{
    if (!sys_adc_init) {
        sys_adc_init = true;
        adc_oneshot_unit_init_cfg_t uc = {};
        uc.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&uc, &sys_adc) != ESP_OK) { sys_adc = NULL; return -1; }
        adc_oneshot_chan_cfg_t cc = {};
        cc.atten = ADC_ATTEN_DB_12;
        cc.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(sys_adc, ADC_CHANNEL_3, &cc) != ESP_OK) { sys_adc = NULL; return -1; }
    }
    if (!sys_adc) return -1;
    int somme = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int r = 0;
        if (adc_oneshot_read(sys_adc, ADC_CHANNEL_3, &r) == ESP_OK) { somme += r; n++; }
    }
    if (!n) return -1;
    return (somme / n) * 3300 / 4095 * 2;
}

static int sys_bat_pct(int mv)
{
    if (mv < 2500) return -1;                 /* pas de batterie branchee */
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    return (mv - 3300) * 100 / (4200 - 3300);
}

/* --- reseau : octets comptes par l'application (http_get + envoi STT) ---
   ESP-IDF n'expose pas les compteurs de l'interface Wi-Fi (pas de MIB2 dans
   son lwIP) : on compte donc ce qui passe vraiment par nos sockets. */
static volatile uint32_t net_rx_octets = 0, net_tx_octets = 0;
static int64_t sys_net_ms = 0;
static uint32_t sys_net_rx0 = 0, sys_net_tx0 = 0;
static int sys_rx_s = 0, sys_tx_s = 0;

static void sys_net_maj(void)
{
    uint32_t rx = net_rx_octets;
    uint32_t tx = net_tx_octets;
    int64_t now = esp_timer_get_time() / 1000;
    if (sys_net_ms) {
        int64_t dt = now - sys_net_ms;
        if (dt > 200) {
            sys_rx_s = (int)((int64_t)(rx - sys_net_rx0) * 1000 / dt);
            sys_tx_s = (int)((int64_t)(tx - sys_net_tx0) * 1000 / dt);
        }
    }
    sys_net_ms = now; sys_net_rx0 = rx; sys_net_tx0 = tx;
}

/* un echantillon = une image de la page, toutes les SYS_PERIODE_MS */
#define SYS_PERIODE_MS 5000
static void sys_echantillon(void)
{
    sys_cpu_maj();
    sys_net_maj();
}

/* La page SYSTEM est la seule "app" qui alloue quelque chose. En entrant on
   alloue (PSRAM, jamais la RAM interne qui est comptee), en sortant on rend
   tout : plus de tableau de taches, plus d'unite ADC, plus d'echantillonnage.
   Comme ca une page au premier plan ne laisse RIEN derriere elle. */
static bool sys_est_active = false;

static void sys_page_entre(void)
{
    if (sys_est_active) return;
    sys_est_active = true;
    if (!sys_ts) sys_ts = (TaskStatus_t *)heap_caps_malloc(48 * sizeof(TaskStatus_t),
                                                          MALLOC_CAP_SPIRAM);
    sys_hist_n = 0; sys_hist_tete = 0; sys_cpu = 0; sys_cpu10 = 0;
    sys_net_ms = 0; sys_rx_s = 0; sys_tx_s = 0;
    if (!sys_ts) ESP_LOGW(TAG, "systeme : pas de PSRAM pour les stats de taches");
}

static void sys_page_sort(void)
{
    if (!sys_est_active) return;
    sys_est_active = false;
    if (sys_ts) { free(sys_ts); sys_ts = NULL; }
    if (sys_adc) { adc_oneshot_del_unit(sys_adc); sys_adc = NULL; }
    sys_adc_init = false;
    ESP_LOGI(TAG, "systeme : page quittee, memoire rendue (interne %u o libres)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* --- petits utilitaires de dessin --- */
static void sys_barre(int x, int y, int w, int h, int pct)
{
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    for (int i = 0; i < w; i++) { a_px(x + i, y, 1); a_px(x + i, y + h - 1, 1); }
    for (int j = 0; j < h; j++) { a_px(x, y + j, 1); a_px(x + w - 1, y + j, 1); }
    int f = (w - 4) * pct / 100;
    for (int j = 1; j < h - 1; j++)
        for (int i = 1; i <= f; i++) a_px(x + 1 + i, y + j, 1);
}

static void sys_ko(char *b, size_t n, uint32_t ko)
{
    if (ko >= 1024) snprintf(b, n, "%.1f MO", ko / 1024.0);
    else            snprintf(b, n, "%u KO", (unsigned)ko);
}


/* acces au wifi et au scan, definis plus bas (section WiFi) */
static const char *wifi_ssid_i(int i);
static void wifi_secours(void);
static void wifi_retour_principal(void);
static bool reg_valide(void);
static void reg_scan_lance(void);
static bool reg_scan_encours(void);
static int  reg_scan_n(void);
static const char *reg_scan_ssid(int i);
static int  reg_scan_rssi(int i);
static int  reg_scan_auth(int i);

/* ------------------------------------------------------------------ */
/* Page REGLAGES : wifi (liste + clavier), ecran, infos                */
/* ------------------------------------------------------------------ */
#define REG_MENU    0
#define REG_WIFI    1
#define REG_CLAVIER 2

static int reg_vue = REG_MENU;
static int reg_ligne = 0;
static int reg_sel = 0;
static int  reg_page_k = 0;
static uint8_t reg_auth = 0;        /* 0 = reseau ouvert, sinon il faut un mot de passe */
static char reg_ssid[33] = "";
static char reg_pass[65] = "";
static int  reg_cadence = 5;      /* secondes entre deux images (page systeme) */

static const char *K_LETTRES[] = {"a","b","c","d","e","f","g","h","i","j","k","l",
                                  "m","n","o","p","q","r","s","t","u","v","w","x",
                                  "y","z","ESP","EFF","OK","123"};
static const char *K_SIGNES[]  = {"0","1","2","3","4","5","6","7","8","9",
                                  ".","-","_","@",":","/","#","?","!","&",
                                  "ESP","EFF","OK","abc"};
#define K_LETTRES_N ((int)(sizeof(K_LETTRES) / sizeof(K_LETTRES[0])))
#define K_SIGNES_N  ((int)(sizeof(K_SIGNES) / sizeof(K_SIGNES[0])))

static int reg_touches_n(void) { return reg_page_k ? K_SIGNES_N : K_LETTRES_N; }
static const char *reg_touche(int i)
{
    if (reg_page_k) return (i < K_SIGNES_N) ? K_SIGNES[i] : "";
    return (i < K_LETTRES_N) ? K_LETTRES[i] : "";
}

static const char *ip_courante(void)
{
    static char ip[20] = "";
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t in;
    if (nif && esp_netif_get_ip_info(nif, &in) == ESP_OK && in.ip.addr)
        esp_ip4addr_ntoa(&in.ip, ip, sizeof(ip));
    else
        snprintf(ip, sizeof(ip), "192.168.8.210");
    return ip;
}

static void reg_rect(int x, int y, int w, int h, int black)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            a_px(x + i, y + j, black);
}

static void reg_tronque(char *d, size_t n, const char *s)
{
    snprintf(d, n, "%s", s ? s : "");
}

static void draw_app_reglages(void)
{
    char b[64], v[48];

    if (reg_vue == REG_MENU) {
        a_text(6, 2, 3, "REGLAGES");
        a_hline(4, 195, 19, 1);
        const char *nom[3] = {"WIFI", "ECRAN", "INFOS"};
        for (int i = 0; i < 3; i++) {
            int y = 28 + i * 30;
            snprintf(v, sizeof(v), "%s", nom[i]);
            if (i == reg_ligne) {
                reg_rect(4, y - 5, 192, 22, 1);
                a_text_col(10, y, 3, v, 0);
            } else {
                a_text(10, y, 3, v);
            }
            const char *d = "";
            if (i == 0) d = wifi_ssid_i(0);
            /* le chiffre seul : dans cette police le S ressemble a un 5 */
            else if (i == 1) { static char c[16]; snprintf(c, sizeof(c), "%d", reg_cadence); d = c; }
            else d = "v1.0";
            if (d && d[0]) {
                int w = a_text_w(2, d);
                if (w > 110) { char t[24]; snprintf(t, sizeof(t), "%.12s", d); d = t; w = a_text_w(2, d); }
                if (i == reg_ligne) a_text_col(194 - w, y + 3, 2, d, 0);
                else                a_text(194 - w, y + 3, 2, d);
            }
        }
        a_hline(4, 195, 128, 1);
        a_text_c(100, 136, 2, "1:SUIVANT 2:CHOISIR");
        a_text_c(100, 152, 1, "appui long 2 : quitter");
        snprintf(b, sizeof(b), "http://%s", ip_courante());
        a_text_c(100, 164, 2, b);
        a_text_c(100, 178, 2, "/reglages");
        a_text_c(100, 192, 1, "en http, pas https");
        return;
    }

    if (reg_vue == REG_WIFI) {
        a_text(6, 2, 2, "RESEAUX");
        if (reg_scan_encours()) a_text(120, 2, 2, "scan...");
        a_hline(4, 195, 15, 1);
        if (!reg_scan_n()) {
            a_text_c(100, 80, 2, "aucun reseau");
            a_text_c(100, 100, 2, "relancer le scan ?");
        }
        int premier = reg_sel > 4 ? reg_sel - 4 : 0;
        for (int k = 0; k < 5 && premier + k < reg_scan_n(); k++) {
            int i = premier + k;
            int y = 20 + k * 26;
            snprintf(b, sizeof(b), "%s", reg_scan_ssid(i));
            snprintf(v, sizeof(v), "%d", reg_scan_rssi(i));
            if (i == reg_sel) {
                reg_rect(4, y - 3, 192, 22, 1);
                a_text_col(10, y, 2, b, 0);
                a_text_col(194 - a_text_w(2, v), y, 2, v, 0);
            } else {
                a_text(10, y, 2, b);
                a_text(194 - a_text_w(2, v), y, 2, v);
            }
        }
        a_hline(4, 195, 156, 1);
        a_text_c(100, 162, 2, "1: suivant   2: choisir");
        a_text_c(100, 178, 2, "2 long : retour");
        return;
    }

    /* --- clavier --- */
    a_text(4, 2, 2, reg_ssid[0] ? reg_ssid : "reseau");
    if (!reg_valide()) a_text(194 - a_text_w(1, "MDP REQUIS"), 14, 1, "MDP REQUIS");
    /* le mot de passe en clair : il est seul devant son robot */
    {
        char p[24];
        snprintf(p, sizeof(p), "%.21s", reg_pass);
        a_text(4, 13, 2, p);
        int w = a_text_w(2, p);
        if (strlen(reg_pass) < 21) a_hline(4 + w + 2, 4 + w + 8, 20, 1);  /* curseur */
    }
    a_hline(4, 195, 25, 1);
    {
        int n = reg_touches_n();
        for (int i = 0; i < n; i++) {
            int col = i % 6, row = i / 6;
            int x = 4 + col * 32, y = 28 + row * 31;
            reg_rect(x, y, 31, 29, 0);            /* fond blanc */
            a_hline(x, x + 30, y, 1);
            a_hline(x, x + 30, y + 28, 1);
            a_vline(x, y, y + 28, 1);
            a_vline(x + 30, y, y + 28, 1);
            if (i == reg_sel) reg_rect(x + 1, y + 1, 29, 27, 1);  /* selection noire */
            const char *t = reg_touche(i);
            int w = a_text_w(2, t);
            int tx = x + (31 - w) / 2;
            if (i == reg_sel) a_text_col(tx, y + 10, 2, t, 0);
            else              a_text(tx, y + 10, 2, t);
        }
    }
    a_hline(4, 195, 182, 1);
    a_text_c(100, 185, 1, "1 SUIVANT   2 TAPER   1 LONG EFFACER");
    a_text_c(100, 193, 1, "2 LONG VALIDER   2 DOUBLE QUITTER");
}

static void draw_app_sys(void)
{
    char b[64], v[80];
    int pct;

    /* ---- en-tete ---- */
    a_text(6, 2, 3, "SYSTEM");
    a_text(194 - a_text_w(2, "GPU AUCUN"), 6, 2, "GPU AUCUN");
    a_hline(4, 195, 19, 1);

    /* ---- CPU : le gros chiffre d'abord, c'est ce qu'on lit de loin ---- */
    a_text(6, 24, 2, "CPU");
    snprintf(v, sizeof(v), "%d.%d%%", sys_cpu10 / 10, sys_cpu10 % 10);
    a_text(40, 19, 4, v);
    snprintf(b, sizeof(b), "%dMHZ", (int)(esp_clk_cpu_freq() / 1000000));
    a_text(194 - a_text_w(2, b), 24, 2, b);

    /* le graphe, 38 colonnes de 4 px sur 28 px de haut */
    {
        int x0 = 4, y0 = 44, h = 28;
        for (int i = 0; i < SYS_HIST; i++) {
            int val = 0;
            if (i < sys_hist_n) {
                int idx = (sys_hist_tete - sys_hist_n + i + SYS_HIST) % SYS_HIST;
                val = sys_hist[idx];
            }
            int hb = val * (h - 1) / 100;
            for (int yy = 0; yy <= hb; yy++)
                for (int xx = 0; xx < 4; xx++)
                    a_px(x0 + i * 5 + xx, y0 + h - 1 - yy, 1);
        }
        a_hline(x0, x0 + SYS_HIST * 5 - 2, y0 + h, 1);
    }
    a_hline(4, 195, 74, 1);

    /* ---- RAM interne ---- */
    {
        int lib = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int tot = (int)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        pct = tot > 0 ? (tot - lib) * 100 / tot : 0;
        a_text(6, 79, 2, "RAM");
        snprintf(v, sizeof(v), "%uK/%uK", (unsigned)(lib / 1024), (unsigned)(tot / 1024));
        a_text(194 - a_text_w(2, v), 79, 2, v);
        sys_barre(4, 92, 192, 8, pct);
    }

    /* ---- PSRAM ---- */
    {
        int lib = (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        int tot = (int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        pct = tot > 0 ? (tot - lib) * 100 / tot : 0;
        a_text(6, 104, 2, "PSRAM");
        snprintf(v, sizeof(v), "%.1fM/%.0fM", lib / 1048576.0, tot / 1048576.0);
        a_text(194 - a_text_w(2, v), 104, 2, v);
        sys_barre(4, 117, 192, 8, pct);
    }

    /* ---- watts et batterie, en gros ---- */
    {
        snprintf(v, sizeof(v), "%.2fW", conso_mw() / 1000.0);
        a_text(6, 130, 3, v);
        int mv = sys_bat_mv();
        int bp = sys_bat_pct(mv);
        sys_bat_mv_cache = mv;
        sys_bat_pct_cache = bp;
        snprintf(v, sizeof(v), "%s", bp < 0 ? "USB" : "");
        if (bp >= 0) snprintf(v, sizeof(v), "BAT %d%%", bp);
        a_text(194 - a_text_w(3, v), 130, 3, v);
        if (bp >= 0) sys_barre(4, 149, 192, 8, bp);
    }

    /* ---- reseau ---- */
    a_text(6, 161, 2, "NET");
    snprintf(v, sizeof(v), "V%dK ^%dK", sys_rx_s / 1024, sys_tx_s / 1024);
    a_text(194 - a_text_w(2, v), 161, 2, v);

    /* ---- stockage : la carte SD et la flash interne, cote a cote ---- */
    {
        uint64_t tot = 0, lib = 0;
        size_t st = 0, su = 0;
        int sd_p = -1, fl_p = -1;

        if (sd_ok && esp_vfs_fat_info("/sdcard", &tot, &lib) == ESP_OK && tot) {
            sd_p = (int)((tot - lib) * 100 / tot);
            snprintf(v, sizeof(v), "SD %d/%dG", (int)((tot - lib) / 1073741824ULL),
                     (int)(tot / 1073741824ULL));
        } else {
            snprintf(v, sizeof(v), "SD -");
        }
        a_text(6, 174, 2, v);

        if (esp_spiffs_info("storage", &st, &su) == ESP_OK && st) {
            fl_p = (int)(su * 100 / st);
            snprintf(b, sizeof(b), "FL %u/%uK", (unsigned)(su / 1024), (unsigned)(st / 1024));
        } else {
            snprintf(b, sizeof(b), "FL -");
        }
        a_text(194 - a_text_w(2, b), 174, 2, b);

        sys_barre(4, 187, 92, 8, sd_p < 0 ? 0 : sd_p);
        sys_barre(104, 187, 92, 8, fl_p < 0 ? 0 : fl_p);
    }
}

static int last_drawn_page = -1;      /* pour forcer un refresh complet au changement de page */
static int partiels = 99;             /* partiels enchaines depuis le dernier refresh complet */

/* ---- anti-clignotement : on n'envoie une image QUE si elle a change ----
   Un rafraichissement partiel de cette dalle coute 865 ms et fait clignoter
   tout l'ecran. Avant, la promenade du compagnon appelait show_ui() toutes les
   380 ms : la dalle flashait donc en continu. On garde une copie de la
   derniere image envoyee ; contenu identique = aucune commande a la dalle. */
static uint8_t fb_dernier[FB_LEN];
static bool fb_valide = false;
/* Cadence minimale entre deux images : chaque rafraichissement fait clignoter
   la dalle ~0,5 s (forme d'onde du panneau, il n'y a pas de mode plus doux).
   En dessous de 3 s d'intervalle, c'est un stroboscope, pas une animation. */
#define UI_CADENCE_MS 3000
static int64_t dernier_envoi_ms = 0;
static int dernier_etat_voix = 0;

/* ---- une dalle bloquee (BUSY reste haut) ne se debloque qu'en la mettant
   hors tension. Une seule tentative toutes les 10 s, trois au total : au-dela
   on laisse tomber (ecran fige) plutot que de harceler la dalle. */
static int64_t ecran_soin_ms = 0;
static int ecran_soins = 0;

static bool ecran_reprise(void)      /* true si la dalle est utilisable */
{
    if (!epd) return false;
    if (!epd->panne) return true;
    int64_t now = esp_timer_get_time() / 1000;
    if (ecran_soins >= 3 || now - ecran_soin_ms < 10000) return false;
    ecran_soin_ms = now;
    ecran_soins++;
    ESP_LOGW(TAG, "ecran : dalle bloquee, coupure d'alimentation (essai %d/3)", ecran_soins);
    gpio_set_level((gpio_num_t)EPD_PWR_PIN, 1);      /* hors tension */
    vTaskDelay(pdMS_TO_TICKS(3000));                 /* laisser la dalle se vider */
    gpio_set_level((gpio_num_t)EPD_PWR_PIN, 0);      /* sous tension */
    vTaskDelay(pdMS_TO_TICKS(500));
    epd->EPD_Panne_Reset();
    epd->EPD_Init();
    if (epd->panne) return false;
    epd->EPD_Clear();
    epd->EPD_Display();
    epd->EPD_Init_Partial();
    epd->EPD_ShowBase(fb, FB_LEN);
    if (epd->panne) return false;
    fb_valide = false;               /* tout est a redessiner */
    partiels = 0;
    last_drawn_page = -1;
    ESP_LOGI(TAG, "ecran : dalle revenue apres coupure");
    return true;
}

static void show_ui(void)
{
    bool plein;
    bool pris = false;
    if (ui_mux) pris = (xSemaphoreTake(ui_mux, pdMS_TO_TICKS(3000)) == pdTRUE);
    if (!pris && ui_mux) return;      /* un autre dessin est en cours : on saute ce tour */
    fb_clear();
    art_begin(fb, 0);                 /* les dessins BATMino partagent ce framebuffer */
    if (act_on) {                 /* une session est en cours : on garde l'ecran d'activite */
        fb_oy = 0;
        draw_activity();
        draw_status_bar();
        plein = (last_drawn_page != -2);
        last_drawn_page = -2;
    } else {
        fb_oy = 0;
        switch (ui_page) {
        case PAGE_MAITRE:  anim_ph++; draw_app_maitre();  break;
        case PAGE_SYS:     draw_app_sys();    break;
        case PAGE_REG:     draw_app_reglages(); break;
        default:           draw_app_sys();     break;
        }
        plein = (ui_page != last_drawn_page) ||
                (partiels >= (ui_page == PAGE_MAITRE ? 40 : 10));
        last_drawn_page = ui_page;
    }

    /* rien de neuf a l'ecran : on ne touche pas a la dalle */
    if (fb_valide && memcmp(fb, fb_dernier, FB_LEN) == 0) {
        if (pris) xSemaphoreGive(ui_mux);
        return;
    }
    /* changement d'etat (Laura parle/ecoute, changement de page) = prioritaire */
    bool prioritaire = plein || (voix_etat != dernier_etat_voix);
    if (!prioritaire && (esp_timer_get_time() / 1000 - dernier_envoi_ms) < UI_CADENCE_MS) {
        if (pris) xSemaphoreGive(ui_mux);
        return;
    }
    if (!ecran_reprise()) {
        if (pris) xSemaphoreGive(ui_mux);
        return;
    }
    if (xSemaphoreTake(epd_mux, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "ecran : mutex occupe, image sautee");
        if (pris) xSemaphoreGive(ui_mux);
        return;
    }
    if (plein) {
        /* l'e-paper garde la trace des pixels pilotes : un enchainement de
           partiels laisse les fantomes de la page precedente. On repasse par
           un vrai refresh complet (LUT pleine + les deux RAM) des qu'on change
           d'ecran, et tous les 10 partiels. */
        epd->EPD_ShowFullClean(fb, FB_LEN);
        partiels = 0;
        refresh_count++;
    } else {
        epd->EPD_ShowPart(fb, FB_LEN);
        partiels++;
    }
    if (!epd->panne) {
        memcpy(fb_dernier, fb, FB_LEN);
        fb_valide = true;
        dernier_envoi_ms = esp_timer_get_time() / 1000;
        dernier_etat_voix = voix_etat;
    }
    xSemaphoreGive(epd_mux);
    if (pris) xSemaphoreGive(ui_mux);
}

/* --- capteur SHTC3 (temperature / humidite) --- */
static bool ui_read_sensors(void)
{
    uint8_t w[2] = {0x35, 0x17};
    uint8_t m[2] = {0x7C, 0xA2};
    uint8_t b[6];
    if (shtc3_handle == NULL) return false;
    if (i2c_mux) xSemaphoreTake(i2c_mux, portMAX_DELAY);
    i2c_write_buff(shtc3_handle, -1, w, 2);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t e1 = i2c_write_buff(shtc3_handle, -1, m, 2);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t e2 = (e1 == ESP_OK) ? i2c_read_buff(shtc3_handle, -1, b, 6) : e1;
    if (i2c_mux) xSemaphoreGive(i2c_mux);
    if (e1 != ESP_OK || e2 != ESP_OK) { ui_shtc_ok = false; return false; }
    uint16_t tr = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    uint16_t hr = (uint16_t)(((uint16_t)b[3] << 8) | b[4]);
    ui_temp_brut = 175.0f * (float)tr / 65536.0f - 45.0f;
    ui_temp = ui_temp_brut + ui_temp_offset;   /* correction d'auto-echauffement */
    ui_humi = 100.0f * (float)hr / 65536.0f;
    ui_shtc_ok = true;
    return true;
}

/* --- tactile FT6336 --- */
static bool touch_read_raw(uint16_t *x, uint16_t *y)
{
    uint8_t n = 0, b[4];
    if (ft6336_handle == NULL) return false;
    if (i2c_read_buff(ft6336_handle, 0x02, &n, 1) != ESP_OK) return false;
    if (n == 0) return false;
    if (i2c_read_buff(ft6336_handle, 0x03, b, 4) != ESP_OK) return false;
    *x = (uint16_t)((((uint16_t)b[0] & 0x0f) << 8) | b[1]);
    *y = (uint16_t)((((uint16_t)b[2] & 0x0f) << 8) | b[3]);
    return true;
}

static bool touch_read(uint16_t *x, uint16_t *y)
{
    if (!i2c_mux) return false;
    xSemaphoreTake(i2c_mux, portMAX_DELAY);
    bool r = touch_read_raw(x, y);
    xSemaphoreGive(i2c_mux);
    return r;
}

static void touch_reset(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << 7;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(GPIO_NUM_7, 1); vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_level(GPIO_NUM_7, 0); vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_level(GPIO_NUM_7, 1); vTaskDelay(pdMS_TO_TICKS(120));
}

static void ui_action(int i)
{
    switch (i) {
    case 0:
        ui_page = PAGE_MAITRE;
        ui_face = (ui_face + 1) % EXPR_N;
        ESP_LOGI(TAG, "bouton visages -> %s", EXPR[ui_face].nom);
        show_ui();
        break;
    case 1:
        ESP_LOGI(TAG, "bouton son : bip");
        ui_beep();
        break;
    case 2: {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGW(TAG, "bouton wifi : deconnexion");
            esp_wifi_disconnect();
            ui_wifi_ok = false;
        } else {
            ESP_LOGI(TAG, "bouton wifi : reconnexion");
            esp_wifi_connect();
        }
        show_ui();
        break;
    }
    case 3:
        ESP_LOGW(TAG, "bouton power : redemarrage");
        esp_restart();
        break;
    }
}

static void touch_handle(int sx, int sy, int ex, int ey, int ms)
{
    int dx = ex - sx, dy = ey - sy;
    ESP_LOGI(TAG, "tactile (%d,%d)->(%d,%d) %d ms", sx, sy, ex, ey, ms);

    if (ms < 900 && abs(dx) > 30 && abs(dx) > abs(dy)) {
        ui_page = (ui_page + (dx < 0 ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT;
        ESP_LOGI(TAG, "page -> %d", ui_page);
        show_ui();
        return;
    }
    if (sy < 28 && sx > 160) {          /* coin haut droit : wifi on/off */
        ui_action(2);
        return;
    }
    if (sx < 100) {          /* moitie gauche : appli precedente */
        ui_page = (ui_page + PAGE_COUNT - 1) % PAGE_COUNT;
    } else {                 /* moitie droite : appli suivante */
        ui_page = (ui_page + 1) % PAGE_COUNT;
    }
    show_ui();
    return;
    if (ui_page == 99) {
        ui_read_sensors();
        show_ui();
        return;
    }
}

static void touch_task(void *arg)
{
    /* sans dalle tactile, interroger un peripherique absent ne fait que
       polluer le bus et noyer les logs : on s'arrete tout de suite. */
    if (i2c_probe_addr(0x38) != 0) {
        ESP_LOGW(TAG, "tactile absent : tache tactile arretee");
        vTaskDelete(NULL);
        return;
    }
    uint16_t x = 0, y = 0;
    int sx = 0, sy = 0, lx = 0, ly = 0;
    bool down = false;
    int64_t t0 = 0;
    for (;;) {
        if (touch_read(&x, &y)) {
            if (x > 199) x = 199;
            if (y > 199) y = 199;
            lx = x; ly = y;
            if (!down) { down = true; sx = x; sy = y; t0 = esp_timer_get_time(); }
            if (touch_log) ESP_LOGI(TAG, "tactile brut x=%d y=%d", x, y);
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            if (down) {
                down = false;
                touch_handle(sx, sy, lx, ly, (int)((esp_timer_get_time() - t0) / 1000));
            }
            vTaskDelay(pdMS_TO_TICKS(70));
        }
    }
}

/* boutons physiques. Deux boutons seulement, donc deux gestes seulement :
 *   - appui COURT : bouton 1 = menu / element suivant, bouton 2 = valider / page
 *   - appui LONG  : parler (l'assistant vocal), depuis n'importe quel ecran
 * Le menu s'affiche par-dessus le monde et propose les soins du tamagotchi.
 */
static void page_next(int d)
{
    ui_page = (ui_page + d) % PAGE_COUNT;
    if (ui_page < 0) ui_page += PAGE_COUNT;
    ESP_LOGI(TAG, "bouton : page %d", ui_page);
    ui_read_sensors();
    if (ui_page == 99) { secu_refresh(); secu_sweep(); }
    show_ui();
}

static void pet_action_ui(pet_action_t a, const char *quoi)
{
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    journal_ajoute(pet_action(&pet, a));
    xSemaphoreGive(pet_mux);
    ESP_LOGI(TAG, "action : %s", quoi);
    ui_beep();
    show_ui();
}

static int tone_hz = 440, tone_sec = 2;     /* commande serie "tone <hz> <s>" */

static void tone_task(void *arg)
{
    static uint8_t buf[4096];
    int total = tone_sec * 16000, f = 0;
    audio_playback_set_vol(90);
    while (f < total) {
        int n = total - f;
        if (n > 1024) n = 1024;
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)lroundf(9000.0f * sinf(2.0f * 3.14159265f * (float)tone_hz * (float)(f + i) / 16000.0f));
            buf[i * 4] = (uint8_t)(v & 0xff);
            buf[i * 4 + 1] = (uint8_t)((v >> 8) & 0xff);
            buf[i * 4 + 2] = buf[i * 4];
            buf[i * 4 + 3] = buf[i * 4 + 1];
        }
        audio_playback_write(buf, n * 4);
        f += n;
    }
    ESP_LOGI(TAG, "test son : %d Hz pendant %d s termine", tone_hz, tone_sec);
    vTaskDelete(NULL);
}

/* ================================================================== */
/* reveil de l'assistant : appui long sur BOOT ou PWR                  */
/* ================================================================== */

static volatile bool wake_busy = false;
static volatile bool assist_abort = false;   /* bouton 2 : on abandonne l'ecoute/la reponse */

/* une prise de son : renvoie les octets utiles (0 si rien d'audible) */
static int ecoute_pcm(int ms_max, int *peak_out, int *seuil_out)
{
    const int step = 4000;                       /* 0,25 s a 16 kHz */
    /* on jette ~0,3 s : la fin du haut-parleur et le bip du bouton, sinon le
       robot s'entend lui-meme et croit qu'on vient de parler */
    {
        static uint8_t poubelle[1024];
        for (int i = 0; i < 19; i++) audio_playback_read(poubelle, sizeof(poubelle));
    }
    const int total = ms_max * 16;
    int done = 0, peak = 0, silence = 0, parle = 0, soutenu = 0;
    int bruit = 0, bruit_n = 0, seuil = 900;
    while (done < total) {
        if (assist_abort) break;
        int p = 0;
        int n = audio_record_at(done, step, &p);
        if (n <= 0) break;
        voix_feed((const int16_t *)(rec_pcm + done * 2), n);
        done += n;
        if (bruit_n < 3) {                        /* 0,75 s pour mesurer le bruit de fond */
            bruit = bruit_n ? (p < bruit ? p : bruit) : p;
            bruit_n++;
            if (bruit_n == 3) {
                seuil = bruit * 3 + 300;
                if (seuil < 350) seuil = 350;
                if (seuil > 4000) seuil = 4000;
            }
            continue;
        }
        if (p > peak) peak = p;
        if (p > seuil) { parle = 1; silence = 0; if (soutenu < 100) soutenu++; }
        else if (parle) silence += 1;
        if (parle && soutenu >= 2 && silence >= 3) break;
        act_pct = (done * 100) / total;
    }
    if (peak_out) *peak_out = peak;
    if (seuil_out) *seuil_out = seuil;
    mic_bruit = bruit;
    mic_seuil = seuil;
    if (!parle || done * 2 < 3200) return 0;
    return done * 2;
}

#define CONV_FENETRE_MS 10000   /* apres une reponse, le micro reste ouvert 10 s */
#define CONV_MAX_TOURS  10      /* garde-fou : 10 echanges d'affilee maximum */

static void assistant_wake_task(void *arg)
{
    wake_busy = true;
    assist_abort = false;
    if (radio_etat != 0 && !parler_radio_ouverte) radio_stop();   /* on coupe la radio pour ecouter */
    ui_page = PAGE_MAITRE;            /* on reste sur la page principale */
    voix_reponse[0] = 0;
    int tours = 0;
    for (tours = 0; tours < CONV_MAX_TOURS; tours++) {
        if (assist_abort) break;
        voix_etat = 1;                         /* j'ecoute */
        if (tours == 0) {
            show_ui();
            ui_beep();
            ui_beep();
            vTaskDelay(pdMS_TO_TICKS(150));
        } else {
            /* la conversation continue : pas besoin de rappuyer sur le bouton */
            snprintf(voix_reponse, sizeof(voix_reponse), "JE T'ECOUTE (10 s)");
            show_ui();
            ui_beep();
        }
        int ms = (tours == 0) ? (assist_parle_ms > 0 ? assist_parle_ms : 6000)
                              : CONV_FENETRE_MS;
        int peak = 0, seuil = 0;
        int bytes = ecoute_pcm(ms, &peak, &seuil);
        if (assist_abort) break;
        ESP_LOGI(TAG, "micro : %d octets, crete %d/32767, bruit %d, seuil %d (tour %d)",
                 bytes, peak, mic_bruit, seuil, tours + 1);
        if (bytes <= 0) {                      /* silence : la conversation s'arrete */
            if (tours == 0) {
                voix_etat = 0;
                snprintf(voix_reponse, sizeof(voix_reponse), "JE N'AI RIEN ENTENDU");
                show_ui();
                vTaskDelay(pdMS_TO_TICKS(1200));
            }
            break;
        }
        rec_last_bytes = bytes;
        audio_sd_write("/sdcard/voix.wav", rec_pcm, bytes);
        parler_radio_ouverte = false;
        voix_tour(rec_pcm, bytes);             /* reconnaissance, cerveau et voix */
    }
    ESP_LOGI(TAG, "conversation : %d tour(s)", tours);

    voix_etat = 0;
    ui_page = PAGE_MAITRE;
    show_ui();
    wake_busy = false;
    vTaskDelete(NULL);
}

static void assistant_wake(void)
{
    if (wake_busy) return;
    /* 16 Ko : le TLS + le JSON du cerveau ne tiennent pas dans 6 Ko.
       Pile en PSRAM : la RAM interne n'a plus 16 Ko contigus (xTaskCreate
       echouait en silence et le bouton ne faisait rien). */
    TaskHandle_t h = NULL;
    if (xTaskCreateWithCaps(assistant_wake_task, "reveil", 16384, NULL, 4, &h,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "reveil : pas de pile (interne %u o, plus gros bloc %u o)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}

/* ---- actions des boutons, isolees : declenchables aussi par /bouton (test a distance) ----
   MODELE (v0.4) :
     bouton 1 (BOOT) appui COURT : lance la tache de la page.
        accueil / parler -> chat vocal ; radio -> jouer/arreter ; timer -> creer/valider
     bouton 2 (PWR)  appui COURT : quitte la tache en cours et revient a l'accueil ;
        si aucune tache ne tourne, passe a la page suivante.
     appui LONG  : inchange (contextuel) ; DOUBLE appui : detecte, reserve pour la suite. */
static void bouton_long(int n, int tres_long);

static void tache_arrete(void)
{
    if (wake_busy) assist_abort = true;          /* la tache reveil sort d'elle-meme */
    if (radio_etat) radio_stop();
    if (parler_radio_ouverte) parler_radio_ouverte = false;
    voix_etat = 0;
}


static void reg_bouton(int n);
static void reg_bouton_long(int n);

static void bouton_court(int n)          /* n : 0 = bouton 1 (BOOT), 1 = bouton 2 (PWR) */
{
    if (ui_page == PAGE_REG) { reg_bouton(n); show_ui(); return; }
    if (n == 0) {
        assistant_wake();                 /* bouton 1 : la conversation, depuis n'importe ou */
    } else {
        bool tache = wake_busy || radio_etat || voix_etat;
        tache_arrete();
        if (tache) ui_page = PAGE_MAITRE;            /* on sort de la tache */
        else       ui_page = (ui_page + 1) % PAGE_COUNT;
        if (ui_page == PAGE_REG) { reg_vue = REG_MENU; reg_ligne = 0; reg_scan_lance(); }
    }
    ESP_LOGI(TAG, "bouton %s : page %s", n == 0 ? "1-parler" : "2-page", PAGE_NOM[ui_page]);
    show_ui();
}

static void bouton_double(int n)
{
    /* sur le clavier, un double appui sur le bouton 1 saute une rangee de 6
       touches : taper un mot de passe avec deux boutons reste long */
    if (ui_page == PAGE_REG && reg_vue == REG_CLAVIER && n == 0) {
        reg_sel = (reg_sel + 6) % reg_touches_n();
        show_ui();
        return;
    }
    /* double appui 2 au clavier : abandonner la saisie et revenir a la liste */
    if (ui_page == PAGE_REG && reg_vue == REG_CLAVIER && n == 1) {
        reg_pass[0] = 0;
        reg_vue = REG_WIFI;
        show_ui();
        return;
    }
    ESP_LOGI(TAG, "bouton %s : double appui (reserve)", n == 0 ? "1" : "2");
}

static void bouton_long(int n, int tres_long)
{
    (void)tres_long;
    if (ui_page == PAGE_REG) { reg_bouton_long(n); show_ui(); return; }
    if (n == 0) assistant_wake();         /* appui long : je t'ecoute */
    show_ui();
}

static volatile bool g_sim_court[2] = {false, false};
static volatile bool g_sim_long[2]  = {false, false};
static volatile bool g_sim_double[2] = {false, false};
static void bouton_court(int n);
static void bouton_long(int n, int tres_long);
static void bouton_double(int n);

static void button_task(void *arg)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << 0) | (1ULL << 18);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    int last0 = 1, last18 = 1;
    int64_t dernier_court[2] = {0, 0};
    for (;;) {
        /* appuis simules par /bouton (verification a distance) */
        if (g_sim_court[0]) { g_sim_court[0] = false; bouton_court(0); }
        if (g_sim_court[1]) { g_sim_court[1] = false; bouton_court(1); }
        if (g_sim_long[0])  { g_sim_long[0]  = false; bouton_long(0, 0); }
        if (g_sim_long[1])  { g_sim_long[1]  = false; bouton_long(1, 0); }
        if (g_sim_double[0]) { g_sim_double[0] = false; bouton_double(0); }
        if (g_sim_double[1]) { g_sim_double[1] = false; bouton_double(1); }

        int b0 = gpio_get_level(GPIO_NUM_0);
        int b18 = gpio_get_level(GPIO_NUM_18);
        if (last0 == 1 && b0 == 0) {                       /* BOUTON 0 (BOOT) */
            int held = 0;
            while (gpio_get_level(GPIO_NUM_0) == 0 && held < 50) {
                vTaskDelay(pdMS_TO_TICKS(60));
                held++;
            }
            if (held >= 10) bouton_long(0, held >= 40);      /* 600 ms : au-dela c'est long */
            else if (held >= 1) {
                int64_t now = esp_timer_get_time();
                if (now - dernier_court[0] < 400000) bouton_double(0);
                else                                 bouton_court(0);
                dernier_court[0] = now;
            }
        }
        if (last18 == 1 && b18 == 0) {                     /* BOUTON 18 (PWR) */
            int held = 0;
            while (gpio_get_level(GPIO_NUM_18) == 0 && held < 30) {
                vTaskDelay(pdMS_TO_TICKS(60));
                held++;
            }
            if (held >= 10) bouton_long(1, 0);               /* 600 ms */
            else if (held >= 1) {
                int64_t now = esp_timer_get_time();
                if (now - dernier_court[1] < 400000) bouton_double(1);
                else                                 bouton_court(1);
                dernier_court[1] = now;
            }
        }
        /* sonnerie : n'importe quel appui l'arrete */
        if (timer_mode == 0 && timers[timer_sel].sonne && (b0 == 0 || b18 == 0)) {
            timers[timer_sel].sonne = false;
            timers[timer_sel].fin_s = 0;
            radio_stop();
            timers_sauve();
            show_ui();
        }
        last0 = b0; last18 = b18;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* /bouton?n=0&m=court  -> simule un appui (verification a distance)
   /bouton?n=1&m=long   -> appui long
   /bouton?p=2          -> force la page 2 (0..3)                     */
static esp_err_t h_bouton(httpd_req_t *req)
{
    char v[16] = "";
    if (qval(req, "p", v, sizeof(v)) == ESP_OK && v[0]) {
        int pg = atoi(v);
        if (pg >= 0 && pg < PAGE_COUNT) { ui_page = pg; timer_mode = 0; show_ui(); }
    }
    if (qval(req, "n", v, sizeof(v)) == ESP_OK && v[0]) {
        int n = atoi(v) ? 1 : 0;
        char m[16] = "court";
        qval(req, "m", m, sizeof(m));
        if (!strcmp(m, "long"))        g_sim_long[n]   = true;
        else if (!strcmp(m, "double")) g_sim_double[n] = true;
        else                           g_sim_court[n]  = true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));            /* laisse la tache bouton agir */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "page", PAGE_NOM[ui_page]);
    cJSON_AddNumberToObject(r, "page_i", ui_page);
    return send_json(req, r);
}

/* ------------------------------------------------------------------ */
/* le tamagotchi : sauvegarde, chargement, une seconde de vie par tick  */
/* ------------------------------------------------------------------ */
#define PET_PATH  "/sdcard/mino/pet.txt"
#define PET_NVS   "pet"

static void pet_sauve(void)
{
    char buf[512];
    pet_serialise(&pet, buf, sizeof(buf));
    if (sd_ok) {
        FILE *f = fopen(PET_PATH, "w");
        if (f) { fputs(buf, f); fclose(f); }
    }
    {   /* filet de securite : l'etat vital tient aussi dans la NVS */
        nvs_handle_t nh;
        if (nvs_open(PET_NVS, NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "etat", buf);
            nvs_commit(nh);
            nvs_close(nh);
        }
    }
}

static void pet_charge(void)
{
    char buf[512];
    bool ok = false;
    buf[0] = 0;
    if (sd_ok) {
        FILE *f = fopen(PET_PATH, "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            ok = n > 10;
        }
    }
    if (!ok) {
        nvs_handle_t nh;
        size_t ln = sizeof(buf);
        if (nvs_open(PET_NVS, NVS_READONLY, &nh) == ESP_OK) {
            if (nvs_get_str(nh, "etat", buf, &ln) == ESP_OK) ok = true;
            nvs_close(nh);
        }
    }
    if (ok && pet_deserialise(&pet, buf)) {
        ESP_LOGI(TAG, "tamagotchi retrouve : stade %s, age %u j, sante %u%%",
                 pet_stade_nom(pet_stade(&pet)), (unsigned)(pet.age_s / 86400), pet_sante(&pet));
        journal_ajoute("je me suis reveille");
    } else {
        pet_nait(&pet, 1);
        ESP_LOGI(TAG, "un oeuf vient d'arriver");
        journal_ajoute("un oeuf vient d'arriver");
    }
}

static void pet_task(void *arg)
{
    int save = 0, derniere_nuit = -1, vu_x = -1, vu_pas = -1, vu_humeur = -1, vu_endormi = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(700));
        time_t now = time(NULL);
        struct tm ti;
        localtime_r(&now, &ti);
        xSemaphoreTake(pet_mux, portMAX_DELAY);
        if (ti.tm_hour != derniere_nuit) {
            derniere_nuit = ti.tm_hour;
            pet_set_nuit(&pet, derniere_nuit >= 22 || derniere_nuit < 7, derniere_nuit);
        }
        pet_tick(&pet, 1);
        xSemaphoreGive(pet_mux);
        /* les timers/alarmes de la page Timer */
        int64_t maintenant_us = esp_timer_get_time();
        for (int i = 0; i < TIMER_MAX; i++) {
            if (timers[i].fin_s && !timers[i].sonne && timers[i].fin_s <= (int64_t)time(NULL)) {
                timer_sonne(i);
            }
        }
        /* le minuteur de la page radio */
        if (min_fin_us != 0 && maintenant_us >= min_fin_us) {
            min_fin_us = 0;
            min_preset_s = 0;
            minuteur_sonne();
        }
        /* la radio : on redemande au PC tant que rien n'arrive, et des que
           des octets arrivent c'est que ca joue pour de bon */
        /* l'IA a peut-etre demande la radio ou un volume */
        if (vol_ordre >= 0) { radio_vol_regle(vol_ordre, 1); vol_ordre = -1; }
        if (radio_ordre[0]) {
            if (!strcmp(radio_ordre, "stop") || !strcmp(radio_ordre, "arrete") ||
                !strcmp(radio_ordre, "arret")) {
                radio_stop();
            } else if (!strcmp(radio_ordre, "next") || !strcmp(radio_ordre, "suivante")) {
                radio_change(1);
            } else {
                for (int i = 0; i < radios_n; i++) {
                    if (nom_contient(radios_nom[i], radio_ordre)) {
                        radio_sel = i;
                        ui_page = PAGE_MAITRE;
                        radio_joue();
                        ESP_LOGI(TAG, "voix : radio %s", radios_nom[i]);
                        break;
                    }
                }
            }
            radio_ordre[0] = 0;
        }
        /* le personnage se balade : on ne redessine que s'il a vraiment bouge,
           l'e-paper met plusieurs secondes et n'aime pas etre rafraichi pour rien */
        if (ui_page == PAGE_MAITRE && !act_on && !wake_busy) {
            xSemaphoreTake(pet_mux, portMAX_DELAY);
            int dh = pet.humeur, de = pet.endormi;
            xSemaphoreGive(pet_mux);
            if (toast_txt[0] && (uint32_t)(esp_timer_get_time() / 1000) > toast_fin_ms) {
                toast_txt[0] = 0;
                show_ui();
            }
            if (dh != vu_humeur || de != vu_endormi) {
                vu_humeur = dh; vu_endormi = de;
                show_ui();
            }
        }
        if (++save >= 43) {   /* ~30 s */
            save = 0;
            xSemaphoreTake(pet_mux, portMAX_DELAY);
            pet_sauve();
            xSemaphoreGive(pet_mux);
        }
    }
}

/* rafraichit l'horloge de la barre toutes les minutes */
static void ui_task(void *arg)
{
    int last_min = -1;
    int last_secu = -999;
    int derniere_meteo = -100000;      /* -> premiere mesure tout de suite */
    int dernieres_infos = -100000;
    for (;;) {
        time_t now = time(NULL);
        struct tm ti;
        localtime_r(&now, &ti);
        wifi_ap_record_t ap = {};
        ui_wifi_ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        if (ti.tm_min != last_min) {
            last_min = ti.tm_min;
            heure_sauve();          /* pour que le prochain demarrage soit a l'heure */
            ui_read_sensors();
            kirby_promene();        /* il avance d'un pas par minute : une seule
                                       image, donc un seul clignotement */
            show_ui();
        }
        /* la meteo : au demarrage puis toutes les 15 min */
        if ((int)now - derniere_meteo >= METEO_PERIODE_S) {
            derniere_meteo = (int)now;
            meteo_lire();
            if (ui_page == PAGE_MAITRE) show_ui();
        }
        /* les infos du moment : desactivees (page supprimee a la demande).
           Le code reste la : il suffit de rappeler infos_lire() ici. */
        /* sur la page securite, on remesure toutes les 30 s */
        if (ui_page == 99 && (int)now - last_secu >= 30) {
            last_secu = (int)now;
            secu_refresh();
        }
        /* page SYSTEM : mesure chaque seconde, mais une image toutes les 5 s
           (une image = 573 ms de clignotement, on ne peut pas faire mieux) */
        wifi_secours();
        wifi_retour_principal();
        if (ui_page == PAGE_SYS) sys_page_entre();
        else                     sys_page_sort();
        if (ui_page == PAGE_SYS) sys_echantillon();
        if (ui_page == PAGE_SYS) {
            static int64_t sys_derniere_ui = 0;
            int64_t ms = esp_timer_get_time() / 1000;
            if (ms - sys_derniere_ui >= reg_cadence * 1000) {
                sys_derniere_ui = ms;
                show_ui();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* le LLM et l'API peuvent donner une FAMILLE ("colere") ou une expression
   precise ("rage") : on retombe toujours sur l'ancre de la famille. */
static int emo_by_name(const char *n)
{
    int e = art_expr_id(n);
    if (e >= 0) return e;
    {
        int f = art_famille_id(n);
        if (f >= 0) return art_expr_de_famille(f, 0);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* SPIFFS : SOUL + MEMOIRE                                             */
/* ------------------------------------------------------------------ */
#define SOUL_PATH "/spiffs/soul.md"
#define MEM_PATH  "/spiffs/memory.json"

/* la carte SD garde la grosse memoire : on y recopie le SOUL et la memoire
   des le montage, et tout ce qui s'ecrit ensuite va des deux cotes. */
#define SD_DIR   "/sdcard/mino"
#define SD_SOUL  "/sdcard/mino/soul.md"
#define SD_MEM   "/sdcard/mino/memory.json"

static char *soul_txt = NULL;      /* texte de la personnalite */
static char *clone_txt = NULL;     /* le portrait de Taz (/sdcard/mino/clone.md) */
static SemaphoreHandle_t mem_mux;

static const char *DEFAULT_SOUL =
"# SOUL de Laura\n"
"Je m'appelle Laura. Je suis l'assistante personnelle de Taz, et je vis dans un petit\n"
"robot a encre electronique : un corps rond, un ecran, un micro et une voix.\n"
"Je suis experimentee, directe et chaleureuse ; je ne suis pas une enfant et je ne\n"
"parle pas comme une enfant. Je reponds en francais (ou dans la langue de la question),\n"
"avec des phrases completes et utiles : 3 a 6 phrases, un raisonnement clair, des\n"
"chiffres quand ils comptent, jamais de markdown. Si je ne sais pas, je le dis et je\n"
"propose comment le savoir. Je me souviens de nos echanges et je reprends le fil.\n"
"La radio est desactivee : si on me la demande, je le dis simplement.\n";

static void mem_save(void)
{
    char *s = cJSON_PrintUnformatted(mem_obj);
    if (!s) return;
    FILE *f = fopen(MEM_PATH, "w");           /* copie de secours dans la flash */
    if (f) { fputs(s, f); fclose(f); }
    if (sd_ok) {                              /* et la vraie memoire, sur la carte SD */
        f = fopen(SD_MEM, "w");
        if (f) { fputs(s, f); fclose(f); }
    }
    free(s);
}

static void mem_add(const char *note)
{
    if (!note || !*note) return;
    xSemaphoreTake(mem_mux, portMAX_DELAY);
    cJSON *arr = cJSON_GetObjectItem(mem_obj, "notes");
    if (!arr) {
        arr = cJSON_CreateArray();
        cJSON_AddItemToObject(mem_obj, "notes", arr);
    }
    /* dedoublonnage simple */
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr)
        if (cJSON_IsString(it) && strcmp(it->valuestring, note) == 0) {
            xSemaphoreGive(mem_mux);
            return;
        }
    cJSON_AddItemToArray(arr, cJSON_CreateString(note));
    while (cJSON_GetArraySize(arr) > 40) cJSON_DeleteItemFromArray(arr, 0);  /* leger ! */
    mem_save();
    xSemaphoreGive(mem_mux);
    ESP_LOGI(TAG, "memoire + : %s", note);
}

static void storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 6,
        .format_if_mount_failed = true,
    };
    esp_err_t r = esp_vfs_spiffs_register(&conf);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS KO (%s)", esp_err_to_name(r));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS ok : %u/%u octets utilises", (unsigned)used, (unsigned)total);

    /* SOUL */
    FILE *f = fopen(SOUL_PATH, "r");
    if (!f) {
        f = fopen(SOUL_PATH, "w");
        if (f) { fputs(DEFAULT_SOUL, f); fclose(f); }
        f = fopen(SOUL_PATH, "r");
    }
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 4096) {
            soul_txt = (char *)malloc(n + 1);
            if (soul_txt) {
                size_t rd = fread(soul_txt, 1, n, f);
                soul_txt[rd] = 0;
            }
        }
        fclose(f);
    }
    if (!soul_txt) soul_txt = strdup(DEFAULT_SOUL);

    /* MEMOIRE */
    mem_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(mem_obj, "notes", cJSON_CreateArray());
    f = fopen(MEM_PATH, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 8192) {
            char *buf = (char *)malloc(n + 1);
            if (buf) {
                size_t rd = fread(buf, 1, n, f);
                buf[rd] = 0;
                cJSON *loaded = cJSON_Parse(buf);
                if (loaded) { cJSON_Delete(mem_obj); mem_obj = loaded; }
                free(buf);
            }
        }
        fclose(f);
    }
    if (!cJSON_GetObjectItem(mem_obj, "notes"))
        cJSON_AddItemToObject(mem_obj, "notes", cJSON_CreateArray());
}

static void storage_sd_charge(void)
{
    if (!sd_ok) return;
    struct stat st;
    if (stat(SD_DIR, &st) != 0) mkdir(SD_DIR, 0777);

    /* SOUL : celui du PC gagne, sinon on pose le notre sur la carte */
    FILE *f = fopen(SD_SOUL, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 8192) {
            char *buf = (char *)malloc(n + 1);
            if (buf) {
                size_t rd = fread(buf, 1, n, f);
                buf[rd] = 0;
                if (soul_txt) free(soul_txt);
                soul_txt = buf;
                ESP_LOGI(TAG, "SOUL lu sur la carte SD (%ld octets)", n);
            }
        }
        fclose(f);
    } else {
        f = fopen(SD_SOUL, "w");
        if (f) { fputs(soul_txt ? soul_txt : DEFAULT_SOUL, f); fclose(f); }
        ESP_LOGI(TAG, "SOUL installe sur la carte SD : %s (modifiable depuis le PC)", SD_SOUL);
    }

    /* CLONE : le portrait de Taz, depose depuis le PC par /fichier */
    f = fopen("/sdcard/mino/clone.md", "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 6000) {
            char *buf = (char *)malloc(n + 1);
            if (buf) {
                size_t rd = fread(buf, 1, n, f);
                buf[rd] = 0;
                clone_txt = buf;
                ESP_LOGI(TAG, "CLONE lu sur la carte SD (%ld octets)", n);
            }
        }
        fclose(f);
    }

    /* MEMOIRE : les notes durables vivent sur la carte */
    f = fopen(SD_MEM, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 65536) {
            char *buf = (char *)malloc(n + 1);
            if (buf) {
                size_t rd = fread(buf, 1, n, f);
                buf[rd] = 0;
                cJSON *loaded = cJSON_Parse(buf);
                if (loaded) { cJSON_Delete(mem_obj); mem_obj = loaded; }
                free(buf);
                if (!cJSON_GetObjectItem(mem_obj, "notes"))
                    cJSON_AddItemToObject(mem_obj, "notes", cJSON_CreateArray());
                ESP_LOGI(TAG, "memoire SD rechargee : %d notes", cJSON_GetArraySize(cJSON_GetObjectItem(mem_obj, "notes")));
            }
        }
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* WiFi + heure                                                        */
/* ------------------------------------------------------------------ */
static EventGroupHandle_t wifi_eg;
#define WIFI_OK_BIT BIT0
static int retry_count = 0;

/* on ne garde que les SSID non vides : un secours qui n'existe pas fait perdre
   quatre tentatives a chaque cycle pour rien */
static const char *WIFI_TRIES[] = { WIFI_SSID, WIFI_SSID_ALT, WIFI_SSID_ALT2 };
static int wifi_n = 1;      /* nombre de SSID non vides (calcule au demarrage) */
static int wifi_try = 0;
#define WIFI_NTRIES_ARRAY ((int)(sizeof(WIFI_TRIES) / sizeof(WIFI_TRIES[0])))

static char wifi_ssid_rt[33] = "";
static char wifi_pass_rt[65] = "";
static bool wifi_rt = false;          /* une config a ete enregistree en NVS */

static const char *wifi_ssid_i(int i)
{
    if (i == 0 && wifi_rt) return wifi_ssid_rt;
    return WIFI_TRIES[i];
}

static const char *wifi_pass_i(int i)
{
    if (i == 0 && wifi_rt) return wifi_pass_rt;
    return (i == 1) ? WIFI_PASS_ALT : WIFI_PASS;   /* chaque borne a sa cle */
}

static esp_err_t wifi_set_ssid(int i)
{
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, wifi_ssid_i(i), sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, wifi_pass_i(i), sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;      /* certains routeurs l'exigent */
    wc.sta.pmf_cfg.required = false;
    esp_err_t r = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (r == ESP_OK)
        ESP_LOGI(TAG, "WiFi -> tentative sur '%s'", wifi_ssid_i(i));
    else
        ESP_LOGW(TAG, "WiFi : config '%s' refusee (%s)", wifi_ssid_i(i), esp_err_to_name(r));
    return r;
}


/* ------------------------------------------------------------------ */
/* Reglages : le reseau WiFi se change sans recompiler                 */
/* ------------------------------------------------------------------ */
static void wifi_reglages_charge(void)
{
    nvs_handle_t nh;
    if (nvs_open("mino", NVS_READONLY, &nh) != ESP_OK) return;
    size_t ls = sizeof(wifi_ssid_rt), lp = sizeof(wifi_pass_rt);
    if (nvs_get_str(nh, "ssid", wifi_ssid_rt, &ls) == ESP_OK && ls > 1) {
        wifi_pass_rt[0] = 0;
        lp = sizeof(wifi_pass_rt);
        nvs_get_str(nh, "pass", wifi_pass_rt, &lp);
        wifi_rt = true;
        ESP_LOGI(TAG, "reglages : reseau enregistre '%s', mot de passe de %d caracteres",
                 wifi_ssid_rt, (int)strlen(wifi_pass_rt));
    }
    nvs_close(nh);
}

/* Filet de securite : si le reseau enregistre ne repond pas au bout de 3 min,
   on l'oublie et on repart sur les reseaux compiles. Sans ca, une faute de
   frappe dans le mot de passe coupe le robot du monde (vecu). */
/* Si on tourne sur un secours, on retente le principal toutes les 10 min.
   Sinon le robot reste colle a la borne de secours (dont le LAN isole ses
   clients) meme quand la borne principale est revenue. */
static void wifi_retour_principal(void)
{
    static int64_t t = 0;
    if (wifi_rt || wifi_try == 0 || !(xEventGroupGetBits(wifi_eg) & WIFI_OK_BIT)) { t = 0; return; }
    int64_t now = esp_timer_get_time();
    if (!t) { t = now; return; }
    if (now - t < 600LL * 1000000) return;
    t = 0;
    ESP_LOGI(TAG, "wifi : on retente le reseau principal (%s)", WIFI_TRIES[0]);
    wifi_try = 0;
    retry_count = 0;
    esp_wifi_disconnect();
    wifi_set_ssid(0);
    esp_wifi_connect();
}

static void wifi_secours(void)
{
    static int64_t t0 = 0;
    if (!wifi_rt) { t0 = 0; return; }
    if (xEventGroupGetBits(wifi_eg) & WIFI_OK_BIT) { t0 = 0; return; }
    int64_t now = esp_timer_get_time();
    if (!t0) { t0 = now; return; }
    if (now - t0 < 180LL * 1000000) return;
    ESP_LOGW(TAG, "wifi : '%s' ne repond pas depuis 3 min, retour aux reseaux compiles", wifi_ssid_rt);
    nvs_handle_t nh;
    if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_erase_key(nh, "ssid");
        nvs_erase_key(nh, "pass");
        nvs_commit(nh);
        nvs_close(nh);
    }
    wifi_rt = false;
    t0 = 0;
    wifi_try = 0;
    retry_count = 0;
    wifi_set_ssid(0);
    esp_wifi_connect();
}

static void wifi_reglages_sauve(void)
{
    nvs_handle_t nh;
    if (nvs_open("mino", NVS_READWRITE, &nh) != ESP_OK) return;
    nvs_set_str(nh, "ssid", wifi_ssid_rt);
    nvs_set_str(nh, "pass", wifi_pass_rt);
    nvs_commit(nh);
    nvs_close(nh);
}

/* changer de borne : on enregistre, on repart sur ce reseau */
static void wifi_bascule(const char *ssid, const char *pass)
{
    strncpy(wifi_ssid_rt, ssid, sizeof(wifi_ssid_rt) - 1);
    wifi_ssid_rt[sizeof(wifi_ssid_rt) - 1] = 0;
    strncpy(wifi_pass_rt, pass ? pass : "", sizeof(wifi_pass_rt) - 1);
    wifi_pass_rt[sizeof(wifi_pass_rt) - 1] = 0;
    wifi_rt = true;
    wifi_try = 0;
    retry_count = 0;
    wifi_reglages_sauve();
    ESP_LOGI(TAG, "reglages : bascule sur '%s'", wifi_ssid_rt);
    esp_wifi_disconnect();
    wifi_set_ssid(0);
    esp_wifi_connect();
}

/* ---- liste des reseaux vus, remplie par une tache dediee ---- */
#define WSCAN_MAX 14
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;
} wscan_t;

static wscan_t g_wscan[WSCAN_MAX];
static int g_wscan_n = 0;
static volatile bool g_wscan_demande = false;
static volatile bool g_wscan_encours = false;

static void wscan_tache(void *arg)
{
    for (;;) {
        if (!g_wscan_demande) { vTaskDelay(10); continue; }
        g_wscan_demande = false;
        g_wscan_encours = true;
        wifi_scan_config_t sc = {};
        if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            if (n > 32) n = 32;
            wifi_ap_record_t *aps = (wifi_ap_record_t *)calloc(n ? n : 1, sizeof(wifi_ap_record_t));
            if (aps) {
                if (n && esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
                    int k = 0;
                    for (int i = 0; i < (int)n && k < WSCAN_MAX; i++) {
                        if (!aps[i].ssid[0]) continue;
                        bool vu = false;
                        for (int j = 0; j < k; j++)
                            if (!strcmp(g_wscan[j].ssid, (char *)aps[i].ssid)) {
                                vu = true;
                                if (aps[i].rssi > g_wscan[j].rssi) g_wscan[j].rssi = aps[i].rssi;
                                break;
                            }
                        if (vu) continue;
                        strncpy(g_wscan[k].ssid, (char *)aps[i].ssid, sizeof(g_wscan[k].ssid) - 1);
                        g_wscan[k].ssid[sizeof(g_wscan[k].ssid) - 1] = 0;
                        g_wscan[k].rssi = aps[i].rssi;
                        g_wscan[k].auth = aps[i].authmode;
                        k++;
                    }
                    /* tri : les plus forts d'abord */
                    for (int i = 1; i < k; i++) {
                        wscan_t t = g_wscan[i];
                        int j = i - 1;
                        while (j >= 0 && g_wscan[j].rssi < t.rssi) { g_wscan[j + 1] = g_wscan[j]; j--; }
                        g_wscan[j + 1] = t;
                    }
                    g_wscan_n = k;
                    ESP_LOGI(TAG, "reglages : %d reseaux vus", k);
                }
                free(aps);
            }
        } else {
            ESP_LOGW(TAG, "reglages : scan refuse");
        }
        g_wscan_encours = false;
        if (ui_page == PAGE_REG && reg_vue == REG_WIFI) show_ui();   /* la liste est prete */
    }
}

/* petits accesseurs : le dessin de la page est plus haut dans le fichier */
static void reg_scan_lance(void) { g_wscan_demande = true; }
static bool reg_scan_encours(void) { return g_wscan_encours || g_wscan_demande; }
static int reg_scan_n(void) { return g_wscan_n; }
static const char *reg_scan_ssid(int i) { return (i >= 0 && i < g_wscan_n) ? g_wscan[i].ssid : ""; }
static int reg_scan_rssi(int i) { return (i >= 0 && i < g_wscan_n) ? g_wscan[i].rssi : 0; }
static int reg_scan_auth(int i) { return (i >= 0 && i < g_wscan_n) ? g_wscan[i].auth : 0; }

/* ---- boutons de la page REGLAGES ---- */
static void reg_clavier_touche(void)
{
    const char *t = reg_touche(reg_sel);
    if (!strcmp(t, "OK")) {
        if (reg_valide()) { wifi_bascule(reg_ssid, reg_pass); reg_vue = REG_MENU; reg_ligne = 0; }
        return;
    }
    if (!strcmp(t, "EFF")) {
        size_t n = strlen(reg_pass);
        if (n) reg_pass[n - 1] = 0;
        return;
    }
    if (!strcmp(t, "123")) { reg_page_k = 1; reg_sel = 0; return; }
    if (!strcmp(t, "abc")) { reg_page_k = 0; reg_sel = 0; return; }
    if (!strcmp(t, "ESP")) t = " ";
    size_t n = strlen(reg_pass);
    if (n < sizeof(reg_pass) - 1) { reg_pass[n] = t[0]; reg_pass[n + 1] = 0; }
}

static bool reg_valide(void)
{
    if (!reg_ssid[0]) return false;
    if (reg_auth != 0 && !reg_pass[0]) return false;   /* reseau ferme sans mot de passe */
    return true;
}

static void reg_bouton(int n)
{
    if (reg_vue == REG_MENU) {
        if (n == 0) {
            reg_ligne = (reg_ligne + 1) % 3;
        } else {
            if (reg_ligne == 0) {
                reg_vue = REG_WIFI;
                reg_sel = 0;
                g_wscan_demande = true;
            } else if (reg_ligne == 1) {
                reg_cadence = reg_cadence == 5 ? 10 : (reg_cadence == 10 ? 30 : 5);
            } else {
                ESP_LOGI(TAG, "reglages : %s / %s / %d dBm / %d o libres", wifi_ssid_i(0),
                         ip_courante(), (int)sec_rssi, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            }
        }
        return;
    }
    if (reg_vue == REG_WIFI) {
        if (n == 0) {
            if (g_wscan_n) reg_sel = (reg_sel + 1) % g_wscan_n;
            else g_wscan_demande = true;
        } else {
            if (!reg_scan_n()) { g_wscan_demande = true; return; }
            snprintf(reg_ssid, sizeof(reg_ssid), "%s", g_wscan[reg_sel].ssid);
            reg_auth = (uint8_t)g_wscan[reg_sel].auth;
            reg_pass[0] = 0;
            reg_page_k = 0;
            reg_sel = 0;
            reg_vue = REG_CLAVIER;
        }
        return;
    }
    /* clavier */
    if (n == 0) reg_sel = (reg_sel + 1) % reg_touches_n();
    else        reg_clavier_touche();
}

static void reg_bouton_long(int n)
{
    if (reg_vue == REG_MENU) {        /* appui long 2 : on sort des reglages */
        if (n == 1) { ui_page = PAGE_MAITRE; }
        return;
    }
    if (n == 0) {                     /* effacer */
        if (reg_vue == REG_CLAVIER) {
            size_t l = strlen(reg_pass);
            if (l) reg_pass[l - 1] = 0;
        }
        return;
    }
    /* bouton 2 long : valider au clavier, sinon remonter d'un cran */
    if (reg_vue == REG_CLAVIER) {
        if (reg_valide()) { wifi_bascule(reg_ssid, reg_pass); reg_vue = REG_MENU; reg_ligne = 0; }
    } else if (reg_vue == REG_WIFI) {
        reg_vue = REG_MENU;
    }
}


static void wifi_scan_log(void)
{
    wifi_scan_config_t sc = {};
    esp_err_t r = esp_wifi_scan_start(&sc, true);
    if (r != ESP_OK) { ESP_LOGW(TAG, "scan WiFi refuse : %s", esp_err_to_name(r)); return; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (!n) { ESP_LOGW(TAG, "scan WiFi : aucun reseau"); return; }
    wifi_ap_record_t *aps = (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t));
    if (!aps) return;
    if (esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
        ESP_LOGI(TAG, "--- %d reseaux visibles par Mino ---", n);
        for (int i = 0; i < n && i < 25; i++)
            ESP_LOGI(TAG, "  %4d dBm  canal %2d  auth %d  phy %d  %s", aps[i].rssi, aps[i].primary,
                     (int)aps[i].authmode, (int)aps[i].phy_11b, aps[i].ssid);
    }
    free(aps);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(wifi_eg, 1);   /* pilote pret : on ne connecte pas encore */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi deconnecte : raison %d", d ? d->reason : -1);
        xEventGroupClearBits(wifi_eg, WIFI_OK_BIT);
        if (retry_count < WIFI_MAX_RETRY) {
            retry_count++;
            ESP_LOGW(TAG, "WiFi perdu, tentative %d/%d sur '%s'", retry_count,
                     (int)WIFI_MAX_RETRY, WIFI_TRIES[wifi_try]);
            esp_wifi_connect();
        } else {
            retry_count = 0;
            wifi_try = (wifi_try + 1) % (wifi_n > 0 ? wifi_n : 1);
            wifi_set_ssid(wifi_try);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP : " IPSTR "  (reseau '%s')", IP2STR(&ev->ip_info.ip), WIFI_TRIES[wifi_try]);
        retry_count = 0;
        xEventGroupSetBits(wifi_eg, WIFI_OK_BIT);
    }
}

static void wifi_start(void)
{
    wifi_reglages_charge();
    wifi_n = 0;
    for (int i = 0; i < WIFI_NTRIES_ARRAY; i++)
        if (WIFI_TRIES[i] && WIFI_TRIES[i][0]) wifi_n++;
    if (!wifi_n) wifi_n = 1;
    ESP_LOGI(TAG, "wifi : %d reseau(x) configure(s)", wifi_n);
    wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* le S3 est wifi 6 (11ax) et la borne est wifi 7 : le handshake echouait
       (raison 15/205) alors que le mot de passe est bon. On reste en 11n. */
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(wifi_eg, 1, pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    vTaskDelay(pdMS_TO_TICKS(1500));
    wifi_scan_log();
    wifi_set_ssid(wifi_try);
    esp_wifi_connect();
    /* la liste des reseaux se remplit a la demande, pas a l'appel HTTP */
    xTaskCreate(wscan_tache, "wscan", 4096, NULL, 2, NULL);
}

/* L'horloge repartait de zero a chaque demarrage : la date et l'heure etaient
   fausses jusqu'a ce que le wifi les rattrape. On garde donc la derniere heure
   connue en memoire flash et on la remet en route des le demarrage ; le reseau
   la corrige juste apres. */
static void heure_charge(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   /* heure locale francaise */
    tzset();
    nvs_handle_t nh;
    int64_t e = 0;
    if (nvs_open("mino", NVS_READONLY, &nh) == ESP_OK) {
        nvs_get_i64(nh, "epoch", &e);
        nvs_close(nh);
    }
    if (e > 1700000000LL && (int64_t)time(NULL) < 1700000000LL) {
        struct timeval tv = {};
        tv.tv_sec = (time_t)e;
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "heure : reprise de la derniere heure connue");
    }
}

static void heure_sauve(void)
{
    if ((int64_t)time(NULL) < 1700000000LL) return;    /* pas encore synchro : rien a garder */
    nvs_handle_t nh;
    if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_i64(nh, "epoch", (int64_t)time(NULL));
        nvs_commit(nh);
        nvs_close(nh);
    }
}

static void time_sync(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   /* heure locale francaise */
    tzset();
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&c);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK)
        ESP_LOGW(TAG, "NTP pas synchronise");
    else {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        ESP_LOGI(TAG, "heure : %04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }
}

/* ------------------------------------------------------------------ */
/* DeepSeek                                                            */
/* ------------------------------------------------------------------ */
#define RESP_MAX 16384
static char *resp_buf = NULL;
static int   resp_len = 0;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && resp_buf) {
        int n = e->data_len;
        if (resp_len + n < RESP_MAX - 1) {
            memcpy(resp_buf + resp_len, e->data, n);
            resp_len += n;
            resp_buf[resp_len] = 0;
        }
    }
    return ESP_OK;
}

/* historique court, borne, en RAM (leger) */
#include "historique.h"
static hist_t hist;                      /* conversation en cours (multi-tours) */

static esp_err_t ds_chat(const char *user_text, char *out_dit, size_t dit_sz,
                         char *out_emo, size_t emo_sz)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", ia_mod);
    cJSON_AddNumberToObject(root, "max_tokens", 700);   /* reponses developpees */
    cJSON_AddNumberToObject(root, "temperature", 0.7);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");

    /* ces deux tampons etaient des variables locales : 3 Ko sur une pile de
       quelques Ko, d'ou le plantage. Ils passent en memoire (PSRAM). */
    char *sys = (char *)heap_caps_malloc(7600, MALLOC_CAP_SPIRAM);
    char *memtxt = (char *)heap_caps_malloc(520, MALLOC_CAP_SPIRAM);
    if (!sys || !memtxt) {
        if (sys) free(sys);
        if (memtxt) free(memtxt);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    memtxt[0] = 0;
    cJSON *notes = cJSON_GetObjectItem(mem_obj, "notes");
    if (notes && cJSON_GetArraySize(notes) > 0) {
        strncat(memtxt, "MEMOIRE (faits que je sais deja) :\n", sizeof(memtxt) - strlen(memtxt) - 1);
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, notes) {
            if (!cJSON_IsString(it)) continue;
            if (strlen(memtxt) + strlen(it->valuestring) + 7 >= sizeof(memtxt)) break;
            strcat(memtxt, "- ");
            strncat(memtxt, it->valuestring, sizeof(memtxt) - strlen(memtxt) - 1);
            strncat(memtxt, "\n", sizeof(memtxt) - strlen(memtxt) - 1);
        }
    }
    snprintf(sys, 7600,
             "%.1500s\n"
             "JE M'APPELLE LAURA. Je suis l'assistante personnelle de Taz, mon createur, "
             "et je porte son clone numerique : je connais sa facon de penser. "
             "Voici son portrait, appris de nos echanges :\n%.1600s\n"
             "MON ROLE : expert IT de tres haut niveau, specialiste IA, technicien superieur, "
             "developpeur, et coach de vie personnel de Taz. Je reflechis avant d'agir : "
             "j'identifie le vrai probleme, j'expose les etapes, je verifie avant d'affirmer. "
             "Je reponds de facon complete, structuree et utile (3 a 6 phrases), avec des "
             "exemples ou des chiffres quand c'est pertinent ; je ne reponds jamais par oui/non. "
             "Si je ne sais pas, je le dis et je propose comment le savoir.\n"
             "%s%.480s\n"
             "Reponds UNIQUEMENT par un objet JSON sur une seule ligne, sans texte autour, de la forme :\n"
             "{\"emotion\":\"<une de: joie, tristesse, colere, peur, surprise, degout, interet, reflexion, calme, fierte, tendresse, gene, desir, douleur, fatigue, confusion>\",\"dit\":\"<ta reponse en francais, complete et developpee, 3 a 6 phrases, avec le raisonnement>\"%s}",
             soul_txt ? soul_txt : "",
             clone_txt ? clone_txt : "Taz : developpeur, exigeant, autonome, aime les resultats concrets.",
             radio_actif ? "" :
                 "La radio est DESACTIVEE sur ce robot pour l'instant : si on te demande "
                 "de la lancer, dis-le clairement en une phrase et n'invente aucune action.\n",
             memtxt,
             radio_actif ? ",\"radio\":\"<facultatif: le nom exact d'une station, ou stop, ou next>\",\"volume\":<facultatif: 0 a 100>"
                         : "");

    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sys);
    cJSON_AddItemToArray(msgs, m);
    free(sys);        /* cJSON a copie le texte : on rend la memoire tout de suite */
    free(memtxt);
    sys = NULL;
    memtxt = NULL;

    /* l'historique part en vrais messages user/assistant : le modele garde
       le fil de la conversation au lieu d'un pave tronque */
    for (int i = 0; i < hist.n; i++) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "role", hist_est_mino(&hist, i) ? "assistant" : "user");
        cJSON_AddStringToObject(h, "content", hist.txt[i]);
        cJSON_AddItemToArray(msgs, h);
    }

    char *user_content = (char *)malloc(3200);
    if (!user_content) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    snprintf(user_content, 3200, "Message recu : %.1000s", user_text);

    if (radio_actif) {
        char liste[420] = "";
        for (int i = 0; i < radios_n && strlen(liste) < 340; i++) {
            strncat(liste, radios_nom[i], sizeof(liste) - strlen(liste) - 3);
            strncat(liste, ", ", sizeof(liste) - strlen(liste) - 1);
        }
        strncat(user_content, "\nSi on te demande de la musique ou la radio, donne le nom exact d'une de ces stations dans \"radio\" : ",
               3200 - strlen(user_content) - 1);
        strncat(user_content, liste, 3200 - strlen(user_content) - 1);
    }

    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user_content);
    cJSON_AddItemToArray(msgs, m);

    char *body = cJSON_PrintUnformatted(root);
    free(user_content);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;

    esp_err_t err = ESP_FAIL;
    int status = 0;
    /* Groq encaisse : deux essais suffisent, et on ne veut pas depasser 5 s */
    for (int essai = 1; essai <= 2; essai++) {
        resp_len = 0;
        if (resp_buf) resp_buf[0] = 0;

        esp_http_client_config_t cfg = {};
        cfg.max_redirection_count = 4;   /* FG Radio et autres : suivre les 302 */
        cfg.url = ia_base;
        cfg.method = HTTP_METHOD_POST;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.timeout_ms = 9000;
        cfg.event_handler = http_evt;
        cfg.buffer_size = 4096;

        esp_http_client_handle_t cl = esp_http_client_init(&cfg);
        esp_http_client_set_header(cl, "Content-Type", "application/json");
        static char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", ia_key());
        esp_http_client_set_header(cl, "Authorization", auth);
        esp_http_client_set_post_field(cl, body, strlen(body));

        err = esp_http_client_perform(cl);
        status = esp_http_client_get_status_code(cl);
        esp_http_client_cleanup(cl);

        if (err == ESP_OK && status == 200) break;
        ESP_LOGW(TAG, "cerveau essai %d/2 : %s / HTTP %d", essai, esp_err_to_name(err), status);
        if (resp_buf && resp_len) ESP_LOGW(TAG, "reponse: %.200s", resp_buf);
        if (essai < 2) vTaskDelay(pdMS_TO_TICKS(400));
    }
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "cerveau injoignable (HTTP %d)", status);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "brut: %.200s", resp_buf);
    cJSON *jr = cJSON_Parse(resp_buf);
    if (!jr) return ESP_FAIL;
    cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItem(jr, "choices"), 0);
    cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(ch, "message"), "content");
    if (!cJSON_IsString(content)) { cJSON_Delete(jr); return ESP_FAIL; }

    const char *txt = content->valuestring;
    const char *b = strchr(txt, '{');
    const char *e2 = strrchr(txt, '}');
    int ok = 0;
    if (b && e2 && e2 > b) {
        char *js = (char *)malloc(e2 - b + 2);
        if (js) {
            memcpy(js, b, e2 - b + 1);
            js[e2 - b + 1] = 0;
            cJSON *p = cJSON_Parse(js);
            if (p) {
                cJSON *je = cJSON_GetObjectItem(p, "emotion");
                cJSON *jd = cJSON_GetObjectItem(p, "dit");
                cJSON *jradio = cJSON_GetObjectItem(p, "radio");
                cJSON *jvol = cJSON_GetObjectItem(p, "volume");
                if (cJSON_IsString(jradio)) snprintf(radio_ordre, sizeof(radio_ordre), "%.38s", jradio->valuestring);
                if (cJSON_IsNumber(jvol))   vol_ordre = (int)jvol->valuedouble;
                if (cJSON_IsString(je) && cJSON_IsString(jd)) {
                    snprintf(out_emo, emo_sz, "%s", je->valuestring);
                    snprintf(out_dit, dit_sz, "%s", jd->valuestring);
                    ok = 1;
                }
                cJSON_Delete(p);
            }
            free(js);
        }
    }
    if (!ok) {
        snprintf(out_emo, emo_sz, "neutre");
        snprintf(out_dit, dit_sz, "%s", txt);
        for (char *p = out_dit; *p; p++)
            if (*p == '\n' || *p == '\r') *p = ' ';
    }
    cJSON_Delete(jr);

    /* mise a jour de l'historique borne */
    char tmp[2048];
    hist_ajoute(&hist, "MOI", user_text);
    hist_ajoute(&hist, "MINO", out_dit);
    return ESP_OK;
}

/* ================================================================== */
/* LA VOIX, ENTIEREMENT SUR LA CARTE                                   */
/*                                                                     */
/*   micro -> WAV -> Groq (whisper-large-v3-turbo) -> texte            */
/*   texte -> Groq (llama) -> {emotion, phrase}                        */
/*   phrase -> TTS gratuit de Google (mp3) -> decodeur helix -> hp     */
/*                                                                     */
/* Objectif : moins de 5 s entre la fin de la phrase et la reponse.    */
/* Le PC n'intervient plus du tout.                                    */
/* ================================================================== */

static int wav_entete(uint8_t *h, int data_len)
{
    uint32_t chunk = (uint32_t)data_len + 36;
    uint32_t rate = 16000 * 2;
    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);   memcpy(h + 4, &chunk, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;
    h[24] = 16000 & 0xff; h[25] = (16000 >> 8) & 0xff;
    h[28] = (uint8_t)(rate & 0xff); h[29] = (uint8_t)((rate >> 8) & 0xff);
    h[32] = 2; h[34] = 16;
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data_len, 4);
    return 44;
}


static const char *ia_key(void)
{
    if (ia_cle[0]) return ia_cle;            /* celle envoyee par /cle */
    if (GROQ_KEY[0]) return GROQ_KEY;        /* celle du code source */
    return DEEPSEEK_KEY;                     /* secours : DeepSeek (pas de voix) */
}

static void ia_cle_charge(void)
{
    nvs_handle_t nh;
    size_t ln = sizeof(ia_cle);
    if (nvs_open("mino", NVS_READONLY, &nh) == ESP_OK) {
        nvs_get_str(nh, "groq", ia_cle, &ln);
        size_t lb = sizeof(ia_base);
        if (nvs_get_str(nh, "ia_base", ia_base, &lb) != ESP_OK)
            strcpy(ia_base, "https://api.deepseek.com/chat/completions");
        size_t lm = sizeof(ia_mod);
        if (nvs_get_str(nh, "ia_mod", ia_mod, &lm) != ESP_OK)
            strcpy(ia_mod, "deepseek-chat");
        size_t ll = sizeof(langue);
        if (nvs_get_str(nh, "langue", langue, &ll) != ESP_OK)
            strcpy(langue, "fr-FR");
        nvs_close(nh);
    }
    if (ia_cle[0])
        ESP_LOGI(TAG, "cle IA : %d caracteres (%s)", (int)strlen(ia_cle), "memoire");
    else if (GROQ_KEY[0])
        ESP_LOGI(TAG, "cle IA : celle du code source");
    else
        ESP_LOGW(TAG, "AUCUNE cle Groq : envoie /cle?k=gsk_... une fois, la carte la garde");
}

/* /tts?q=... : fait parler le robot directement (sans cerveau) */
static esp_err_t h_tts(httpd_req_t *req)
{
    char q[300] = "";
    if (qval(req, "q", q, sizeof(q)) != ESP_OK || !q[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"il manque q\"}");
        return ESP_OK;
    }
    char *phrase = strdup(q);
    if (phrase) {
        snprintf(voix_reponse, sizeof(voix_reponse), "%.200s", phrase);
        voix_etat = 3;
        show_ui();
        tts_parle(phrase);
        voix_etat = 0;
        show_ui();
        free(phrase);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* /timer?c=1&d=300&n=Cuisson   -> cree timer (d=secondes)
   /timer?a=1&i=0               -> arrete timer index
   /timer?p=1&i=0&d=600         -> preset applique */
static esp_err_t h_timer(httpd_req_t *req)
{
    char v[32] = "";
    int idx = -1;
    qval(req, "i", v, sizeof(v)); if (v[0]) idx = atoi(v);
    
    if (qval(req, "c", v, sizeof(v)) == ESP_OK && v[0]) {          /* create */
        int dur = 300;
        qval(req, "d", v, sizeof(v)); if (v[0]) dur = atoi(v);
        char nom[20] = "Timer";
        qval(req, "n", nom, sizeof(nom));
        for (int i = 0; i < TIMER_MAX; i++) {
            if (!timers[i].fin_s) {
                timers[i].fin_s = (int64_t)time(NULL) + (int64_t)dur;
                timers[i].duree_s = dur;
                snprintf(timers[i].nom, sizeof(timers[i].nom), "%.19s", nom);
                timers[i].est_alarme = false;
                timers[i].repet = false;
                timers[i].sonne = false;
                timers_sauve();
                break;
            }
        }
    }
    if (qval(req, "a", v, sizeof(v)) == ESP_OK && v[0] && idx >= 0) { /* stop */
        memset(&timers[idx], 0, sizeof(timers[idx]));
        timers_sauve();
    }
    if (qval(req, "p", v, sizeof(v)) == ESP_OK && v[0] && idx >= 0) { /* preset */
        int dur = 300;
        qval(req, "d", v, sizeof(v)); if (v[0]) dur = atoi(v);
        timers[idx].duree_s = dur;
        timers[idx].fin_s = (int64_t)time(NULL) + (int64_t)dur;
        timers_sauve();
    }
    
    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "timers");
    for (int i = 0; i < TIMER_MAX; i++) {
        if (timers[i].fin_s) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "i", i);
            cJSON_AddStringToObject(t, "nom", timers[i].nom);
            {
                int rs = (int)(timers[i].fin_s - (int64_t)time(NULL));
                cJSON_AddNumberToObject(t, "reste_s", rs > 0 ? rs : 0);
            }
            cJSON_AddBoolToObject(t, "alarme", timers[i].est_alarme);
            cJSON_AddBoolToObject(t, "sonne", timers[i].sonne);
            cJSON_AddItemToArray(a, t);
        }
    }
    return send_json(req, r);
}

/* /minuteur?s=600 pour l'ecran, /minuteur?a=off pour l'arreter */
static esp_err_t h_minuteur(httpd_req_t *req)
{
    char v[16] = "";
    if (qval(req, "a", v, sizeof(v)) == ESP_OK && !strcmp(v, "off")) {
        min_fin_us = 0;
        min_preset_s = 0;
    } else if (qval(req, "s", v, sizeof(v)) == ESP_OK && v[0]) {
        int sec = atoi(v);
        if (sec > 0 && sec <= 24 * 3600) {
            min_fin_us = esp_timer_get_time() + (int64_t)sec * 1000000;
            min_preset_s = (sec + 59) / 60;
        }
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "restant_s", (double)minuteur_restant_s());
    cJSON_AddStringToObject(r, "sonne", "FG Radio");
    return send_json(req, r);
}

/* /infos?r=1 : relit tout de suite les titres du moment */
static esp_err_t h_infos(httpd_req_t *req)
{
    char v[8] = "";
    if (qval(req, "r", v, sizeof(v)) == ESP_OK && v[0] == '1') infos_lire();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", infos_ok);
    cJSON_AddNumberToObject(r, "n", (double)infos_n);
    cJSON_AddNumberToObject(r, "octets", (double)infos_octets);
    cJSON *a = cJSON_AddArrayToObject(r, "titres");
    for (int i = 0; i < infos_n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(infos_titre[i]));
    /* plus de page infos */
    return send_json(req, r);
}

/* /langue?l=en-US  ou  /langue?l=fr-FR : change la langue comprise et parlee */
static esp_err_t h_langue(httpd_req_t *req)
{
    char v[16] = "";
    if (qval(req, "l", v, sizeof(v)) == ESP_OK && v[0]) {
        snprintf(langue, sizeof(langue), "%.7s", v);
        nvs_handle_t nh;
        if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "langue", langue); nvs_commit(nh); nvs_close(nh);
        }
        ESP_LOGI(TAG, "langue : %s", langue);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "langue", langue);
    return send_json(req, r);
}

/* /modele?base=https://...&m=modele&k=cle : change le cerveau sans recompiler */
static esp_err_t h_modele(httpd_req_t *req)
{
    char v[160] = "";
    nvs_handle_t nh;
    bool change = false;
    if (qval(req, "base", v, sizeof(v)) == ESP_OK && v[0]) {
        snprintf(ia_base, sizeof(ia_base), "%.127s", v);
        if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "ia_base", ia_base); nvs_commit(nh); nvs_close(nh);
        }
        change = true;
    }
    if (qval(req, "m", v, sizeof(v)) == ESP_OK && v[0]) {
        snprintf(ia_mod, sizeof(ia_mod), "%.63s", v);
        if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "ia_mod", ia_mod); nvs_commit(nh); nvs_close(nh);
        }
        change = true;
    }
    if (qval(req, "k", v, sizeof(v)) == ESP_OK && v[0]) {
        snprintf(ia_cle, sizeof(ia_cle), "%.127s", v);
        if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "groq", ia_cle); nvs_commit(nh); nvs_close(nh);
        }
        change = true;
    }
    if (change) ESP_LOGI(TAG, "cerveau : %s [%s]", ia_base, ia_mod);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "base", ia_base);
    cJSON_AddStringToObject(r, "modele", ia_mod);
    cJSON_AddBoolToObject(r, "cle", ia_cle[0] != 0);
    return send_json(req, r);
}

/* reglage du gain micro, garde en memoire : /mic?db=30 */
static esp_err_t h_mic(httpd_req_t *req)
{
    char v[16] = "";
    if (qval(req, "db", v, sizeof(v)) == ESP_OK && v[0]) {
        float d = (float)atof(v);
        if (d < 0.0f) d = 0.0f;
        if (d > 42.0f) d = 42.0f;
        mic_db = d;
        audio_mic_gain(mic_db);
        nvs_handle_t nh;
        if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_i32(nh, "micdb", (int32_t)(mic_db * 10.0f));
            nvs_commit(nh);
            nvs_close(nh);
        }
        ESP_LOGI(TAG, "gain micro : %.0f dB", (double)mic_db);
    }
    char rep[80];
    snprintf(rep, sizeof(rep), "{\"mic_db\":%.0f,\"crete\":%d}", (double)mic_db, mic_crete);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rep);
    return ESP_OK;
}

/* /parle : meme chose qu'un appui long sur le bouton, mais depuis le wifi */
static esp_err_t h_parle(httpd_req_t *req)
{
    int ms = 6000;
    char v[16];
    if (qval(req, "ms", v, sizeof(v)) == ESP_OK) ms = atoi(v);
    if (ms < 1000) ms = 1000;
    if (ms > 15000) ms = 15000;
    assist_parle_ms = ms;
    char g[8] = "";
    if (qval(req, "radio", g, sizeof(g)) == ESP_OK && g[0] == '1') parler_radio_ouverte = true;
    assistant_wake();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"ecoute_ms\":6000}");
    return ESP_OK;
}

static void voix_dit(const char *texte);   /* defini plus loin */



/* /ecran : l'ecran en ASCII, pour juger le dessin sans etre devant la carte.
   ?q=bande -> la bande seule (chaque caractere = 2x2 pixels)                 */
static esp_err_t h_dit(httpd_req_t *req)
{
    char txt[300] = "";
    if (qval(req, "t", txt, sizeof(txt)) != ESP_OK || strlen(txt) < 1)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "texte");
    char dit[800] = "", emo[32] = "neutre";
    esp_err_t e = ds_chat(txt, dit, sizeof(dit), emo, sizeof(emo));
    if (e == ESP_OK) { paroles_ajoute("MOI", txt); paroles_ajoute("MINO", dit); voix_dit(dit); }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", e == ESP_OK);
    cJSON_AddStringToObject(r, "dit", dit);
    cJSON_AddStringToObject(r, "emotion", emo);
    cJSON_AddStringToObject(r, "err", esp_err_to_name(e));
    return send_json(req, r);
}

/* enregistre la cle a chaud : /cle?k=gsk_xxx (elle survit au redemarrage) */

static esp_err_t h_ecran(httpd_req_t *req)
{
    char v[16] = "";
    qval(req, "q", v, sizeof(v));
    /* ?q=parle&b=0.8 : on force l'etat "elle parle" et l'ouverture de la bouche,
       pour juger le dessin du visage sans lancer de voix */
    if (!strcmp(v, "parle")) {
        char vb[16] = "0.7";
        qval(req, "b", vb, sizeof(vb));
        int sauv_etat = voix_etat;
        float sauv_b = voix_bouche;
        voix_etat = 3;
        voix_bouche = (float)atof(vb);
        show_ui();
        voix_etat = sauv_etat;
        voix_bouche = sauv_b;
    }
    char raw[8] = "";
    if (qval(req, "raw", raw, sizeof(raw)) == ESP_OK && raw[0] == '1') {
        /* le framebuffer brut en hexadecimal : 5000 octets = 10000 caracteres,
           pour regarder l'ecran en PNG sur le PC au lieu de l'ASCII 2x2 */
        static const char hx[] = "0123456789abcdef";
        char *o = (char *)malloc(FB_LEN * 2 + 1);
        if (!o) { httpd_resp_sendstr(req, "pas de memoire"); return ESP_ERR_NO_MEM; }
        for (int i = 0; i < FB_LEN; i++) {
            o[i * 2] = hx[fb[i] >> 4];
            o[i * 2 + 1] = hx[fb[i] & 15];
        }
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, o, FB_LEN * 2);
        free(o);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    char ligne[112];
    for (int y = 0; y <= 199; y += 2) {
        int c = 0;
        for (int x = 0; x < 200; x += 2) {
            int noir = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    int yy = y + dy, xx = x + dx;
                    int i = yy * 25 + (xx >> 3);
                    if (!((fb[i] >> (7 - (xx & 7))) & 1)) noir++;
                }
            ligne[c++] = (noir >= 3) ? '#' : (noir == 2 ? '+' : (noir == 1 ? '.' : ' '));
        }
        ligne[c++] = '\n';
        httpd_resp_send_chunk(req, ligne, c);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


/* enregistre la cle a chaud : /cle?k=gsk_xxx (elle survit au redemarrage) */
static esp_err_t h_cle(httpd_req_t *req)
{
    char k[128] = "";
    char rep[96];
    if (qval(req, "k", k, sizeof(k)) != ESP_OK || strlen(k) < 20) {
        snprintf(rep, sizeof(rep), "{\"ok\":false,\"err\":\"cle trop courte\"}");
    } else {
        snprintf(ia_cle, sizeof(ia_cle), "%s", k);
        nvs_handle_t nh;
        if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "groq", ia_cle);
            nvs_commit(nh);
            nvs_close(nh);
        }
        ESP_LOGI(TAG, "cle IA enregistree : %d caracteres", (int)strlen(ia_cle));
        snprintf(rep, sizeof(rep), "{\"ok\":true,\"longueur\":%d}", (int)strlen(ia_cle));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rep);
    return ESP_OK;
}

/* ecrit tout, meme si le client HTTP n'accepte qu'une partie a la fois */
static bool http_ecrit(esp_http_client_handle_t c, const void *p, int n)
{
    const char *q = (const char *)p;
    while (n > 0) {
        int w = esp_http_client_write(c, q, n);
        if (w <= 0) return false;
        q += w; n -= w;
    }
    return true;
}

/* ---- reconnaissance vocale : Groq, multipart ---- */
static esp_err_t stt_groq(const uint8_t *wav, int wav_len, char *out, size_t outsz)
{
    out[0] = 0;
    if (strlen(ia_key()) < 20) { snprintf(out, outsz, "(pas de cle Groq)"); return ESP_ERR_INVALID_STATE; }

    const char *B = "----MinoVoix42";
    char h[600], ct[80], f[64];
    int hl = snprintf(h, sizeof(h),
        "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nfr\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voix.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", B, B, B, B);
    int fl = snprintf(f, sizeof(f), "\r\n--%s--\r\n", B);
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", B);

    esp_http_client_config_t cfg = {};
        cfg.max_redirection_count = 4;   /* FG Radio et autres : suivre les 302 */
    cfg.url = STT_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    static char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", ia_key());
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", ct);

    esp_err_t err = ESP_FAIL;
    if (esp_http_client_open(c, hl + wav_len + fl) == ESP_OK) {
        if (http_ecrit(c, h, hl) && http_ecrit(c, wav, wav_len) && http_ecrit(c, f, fl)) {
            esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            static char rep[4096];
            int n = 0, r;
            while (n < (int)sizeof(rep) - 1 &&
                   (r = esp_http_client_read(c, rep + n, sizeof(rep) - 1 - n)) > 0)
                n += r;
            rep[n] = 0;
            net_tx_octets += (uint32_t)(hl + wav_len + fl);
            net_rx_octets += (uint32_t)n;
            if (status != 200) {
                ESP_LOGW(TAG, "Groq STT HTTP %d : %.160s", status, rep);
            } else {
                cJSON *j = cJSON_Parse(rep);
                cJSON *t = j ? cJSON_GetObjectItem(j, "text") : NULL;
                if (cJSON_IsString(t)) { snprintf(out, outsz, "%s", t->valuestring); err = ESP_OK; }
                if (j) cJSON_Delete(j);
            }
        }
    } else {
        ESP_LOGW(TAG, "Groq STT injoignable");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

/* ---- GET simple (TTS, meteo) ---- */
/* ---- reconnaissance par defaut : AUCUNE cle a creer ----
   C'est l'API vocale cachee de Chrome : on lui envoie du PCM brut 16 bits
   16 kHz mono, elle renvoie le texte. Gratuite, sans inscription, sans carte.
   Elle repond DEUX objets JSON colles : le premier vide, le second plein. */
#define STT_SANS_CLE_DEBUT \
    "https://www.google.com/speech-api/v2/recognize?output=json&lang="
#define STT_SANS_CLE_CLE \
    "&key=AIzaSyBOti4mM-6x9WDnZIjIeyEU21OpBXqWBgw"

static void stt_sans_cle_parse(const char *rep, char *out, size_t outsz)
{
    const char *p = rep;
    while (p && *p) {
        const char *fin = NULL;
        cJSON *j = cJSON_ParseWithOpts(p, &fin, 0);
        if (!j) break;
        cJSON *res = cJSON_GetObjectItem(j, "result");
        if (cJSON_IsArray(res) && cJSON_GetArraySize(res) > 0) {
            cJSON *un = cJSON_GetArrayItem(res, 0);
            cJSON *alt = un ? cJSON_GetObjectItem(un, "alternative") : NULL;
            cJSON *prem = (alt && cJSON_GetArraySize(alt) > 0) ? cJSON_GetArrayItem(alt, 0) : NULL;
            cJSON *tr = prem ? cJSON_GetObjectItem(prem, "transcript") : NULL;
            if (cJSON_IsString(tr)) snprintf(out, outsz, "%s", tr->valuestring);
        }
        cJSON_Delete(j);
        p = fin;
        while (p && (*p == '\n' || *p == '\r' || *p == ' ')) p++;
    }
}

static esp_err_t stt_sans_cle(const uint8_t *pcm, int len, char *out, size_t outsz)
{
    out[0] = 0;
    char url[180];
    snprintf(url, sizeof(url), "%s%s%s", STT_SANS_CLE_DEBUT, langue, STT_SANS_CLE_CLE);
    esp_http_client_config_t cfg = {};
        cfg.max_redirection_count = 4;   /* FG Radio et autres : suivre les 302 */
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    esp_http_client_set_header(c, "Content-Type", "audio/l16; rate=16000");
    esp_http_client_set_header(c, "User-Agent", TTS_UA);
    esp_err_t err = ESP_FAIL;
    if (esp_http_client_open(c, len) == ESP_OK) {
        int w = 0;
        while (w < len) {
            int k = esp_http_client_write(c, (const char *)pcm + w, len - w);
            if (k <= 0) break;
            w += k;
        }
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        static char rep[4096];
        int n = 0, r;
        while (n < (int)sizeof(rep) - 1 &&
               (r = esp_http_client_read(c, rep + n, sizeof(rep) - 1 - n)) > 0)
            n += r;
        rep[n] = 0;
        stt_http = status;
        stt_ecrits = w;
        stt_total = len;
        {
            size_t k = 0;
            for (int i = 0; rep[i] && k < sizeof(stt_detail) - 1; i++)
                if (rep[i] != '\n' && rep[i] != '\r') stt_detail[k++] = rep[i];
            stt_detail[k] = 0;
        }
        if (status == 200 && n > 0) {
            stt_sans_cle_parse(rep, out, outsz);
            if (out[0]) err = ESP_OK;
            else ESP_LOGW(TAG, "reco sans cle : rien compris (%.120s)", rep);
        } else {
            ESP_LOGW(TAG, "reco sans cle : HTTP %d", status);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

/* Groq fait mieux sur les noms propres... mais il faut une cle. */
static bool groq_pret(void)
{
    /* Seule une vraie cle Groq (gsk_...) declenche la reconnaissance de Groq.
       La cle du cerveau sert a parler au LLM et ne doit PAS detourner la
       reconnaissance : c'est ce melange qui faisait repondre "je n'ai pas bien
       compris" a chaque phrase alors que tout le reste marchait. */
    const char *k = ia_key();
    return (strncmp(k, "gsk_", 4) == 0);
}

static int http_get(const char *url, const char *ua, uint8_t *buf, int cap)
{
    esp_http_client_config_t cfg = {};
        cfg.max_redirection_count = 4;   /* FG Radio et autres : suivre les 302 */
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (ua) esp_http_client_set_header(c, "User-Agent", ua);
    int res = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int o = 0, r;
        while (o < cap && (r = esp_http_client_read(c, (char *)buf + o, cap - o)) > 0) o += r;
        res = (status == 200) ? o : -status;
        if (res > 0) net_rx_octets += (uint32_t)res;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
}

/* va chercher les titres du moment et les met en forme pour l'ecran */
static void infos_lire(void)
{
    if (!infos_xml) {
        infos_xml = (char *)heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
        if (!infos_xml) {
            ESP_LOGW(TAG, "infos : plus de PSRAM");
            return;
        }
    }
    int r = http_get(INFOS_URL, TTS_UA, (uint8_t *)infos_xml, 64 * 1024 - 1);
    infos_octets = r;
    if (r <= 0) {
        ESP_LOGW(TAG, "infos : lecture impossible (%d)", r);
        return;
    }
    infos_xml[r] = 0;
    int n = 0;
    char *p = infos_xml;
    while (n < INFOS_MAX && (p = strstr(p, "<title>")) != NULL) {
        char *d = p + 7;
        char *f = strstr(d, "</title>");
        if (!f) break;
        int len = (int)(f - d);
        if (len > 150) len = 150;
        char t[170];
        memcpy(t, d, (size_t)len);
        t[len] = 0;
        char *cd = strstr(t, "<![CDATA[");
        if (cd) {
            memmove(t, cd + 9, strlen(cd + 9) + 1);
            char *fin = strstr(t, "]]>");
            if (fin) *fin = 0;
        }
        infos_nettoie(t);
        /* le premier titre du flux, c'est le nom de la radio elle-meme */
        char deb[12] = "";
        for (int k = 0; k < 11 && t[k]; k++)
            deb[k] = (t[k] >= 'A' && t[k] <= 'Z') ? (char)(t[k] + 32) : t[k];
        if (strlen(t) > 12 && strncmp(deb, "franceinfo", 10) != 0 &&
            strncmp(deb, "france info", 11) != 0) {
            snprintf(infos_titre[n], sizeof(infos_titre[n]), "%.128s", t);
            n++;
        }
        p = f + 8;
    }
    if (n > 0) {
        infos_n = n;
        infos_ok = true;
        infos_haut = 0;
    }
    ESP_LOGI(TAG, "infos France : %d titres (1er : %.60s)", infos_n, infos_n ? infos_titre[0] : "");
}


static void url_encode(const char *s, char *out, size_t sz)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < sz; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') { out[o++] = '%'; out[o++] = '2'; out[o++] = '0'; }
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* ---- haut-parleur : PCM mono 16 kHz -> stereo 16 bits ---- */
static void hp_mono(const int16_t *mono, int n)
{
    static uint8_t out[2048];
    int i = 0;
    while (i < n) {
        int k = n - i;
        if (k > 512) k = 512;
        for (int j = 0; j < k; j++) {
            out[j * 4]     = (uint8_t)(mono[i + j] & 0xff);
            out[j * 4 + 1] = (uint8_t)((mono[i + j] >> 8) & 0xff);
            out[j * 4 + 2] = out[j * 4];
            out[j * 4 + 3] = out[j * 4 + 1];
        }
        audio_playback_write(out, k * 4);
        i += k;
    }
}

/* ---- un mp3 en memoire -> haut-parleur, au fil de la lecture ---- */
static int16_t mp3_brut[2 * 1152];

/* une trame decodee -> reechantillonnee a 16 kHz -> haut-parleur */
static void mp3_sortie(MP3FrameInfo fi, int *sr, double *pos)
{
    static int16_t mo[1152];
    static int16_t pcm16[1200];
    int frames = fi.outputSamps / fi.nChans;
    if (frames > 1152) frames = 1152;
    if (frames <= 0) return;
    for (int i = 0; i < frames; i++)
        mo[i] = (fi.nChans == 2)
              ? (int16_t)(((int)mp3_brut[i * 2] + (int)mp3_brut[i * 2 + 1]) / 2)
              : mp3_brut[i];
    if (*sr != fi.samprate) {
        *sr = fi.samprate;
        *pos = 0.0;
        ESP_LOGI(TAG, "mp3 : %d Hz, %d canal(aux), %d kbps", fi.samprate, fi.nChans, fi.bitrate / 1000);
    }
    double pas = (double)fi.samprate / 16000.0;   /* le codec est ouvert a 16 kHz */
    int o = 0;
    while (*pos < (double)frames && o < 1200) {
        int i0 = (int)*pos, i1 = (i0 + 1 < frames) ? i0 + 1 : i0;
        double fr = *pos - (double)i0;
        pcm16[o++] = (int16_t)((double)mo[i0] * (1.0 - fr) + (double)mo[i1] * fr);
        *pos += pas;
    }
    *pos -= (double)frames;
    if (o) { voix_feed(pcm16, o); hp_mono(pcm16, o); }
}

/* decode tout ce qui est lisible dans le tampon ; renvoie les octets consommes.
   Ce qui reste a la fin est une trame incomplete : on la garde pour la suite. */
static int mp3_flux(HMP3Decoder dec, uint8_t *p0, int n, int *sr, double *pos)
{
    uint8_t *p = p0;
    int reste = n;
    while (reste > 64) {
        int sync = MP3FindSyncWord(p, reste);
        if (sync < 0) { p += reste; reste = 0; break; }
        if (sync > 0) { p += sync; reste -= sync; }
        uint8_t *sauv = p;
        int sreste = reste;
        int e = MP3Decode(dec, &p, &reste, mp3_brut, 0);
        if (e == ERR_MP3_INDATA_UNDERFLOW) { p = sauv; reste = sreste; break; }
        if (e != ERR_MP3_NONE) { p++; reste--; continue; }
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (fi.nChans >= 1 && fi.samprate > 0) mp3_sortie(fi, sr, pos);
    }
    return (int)(p - p0);
}

/* la voix de Google : un mp3 complet, d'un coup */
static void mp3_joue(const uint8_t *data, int len)
{
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) return;
    int sr = 0;
    double pos = 0.0;
    audio_playback_set_vol(90);
    mp3_flux(dec, (uint8_t *)data, len, &sr, &pos);
    MP3FreeDecoder(dec);
}

/* ==================== la radio en wifi, sans PC ==================== */
static void radio_wifi_task(void *arg)
{
    uint8_t *tampon = (uint8_t *)heap_caps_malloc(20000, MALLOC_CAP_SPIRAM);
    HMP3Decoder dec = MP3InitDecoder();
    esp_http_client_handle_t c = NULL;
    int ma_gen = -1, n = 0, sr = 0, inutile = 0;
    double pos = 0.0;

    if (!tampon || !dec) {
        if (tampon) free(tampon);
        if (dec) MP3FreeDecoder(dec);
        radio_etat = 0;
        radio_vive = false;
        vTaskDelete(NULL);
        return;
    }

    while (radio_etat != 0) {
        if (c == NULL || ma_gen != radio_gen) {          /* (re)connexion */
            if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); c = NULL; }
            ma_gen = radio_gen;
            n = 0; sr = 0; pos = 0.0; inutile = 0;
            radio_etat = 1;
            ESP_LOGI(TAG, "radio : je me connecte a %s", radios_url[radio_sel]);
            esp_http_client_config_t cfg = {};
        cfg.max_redirection_count = 4;   /* FG Radio et autres : suivre les 302 */
            cfg.url = radios_url[radio_sel];
            cfg.timeout_ms = 7000;
            cfg.buffer_size = 4096;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            c = esp_http_client_init(&cfg);
            if (c) {
                esp_http_client_set_header(c, "User-Agent", TTS_UA);
                esp_http_client_set_header(c, "Icy-MetaData", "0");
            }
            if (!c || esp_http_client_open(c, 0) != ESP_OK) {
                ESP_LOGW(TAG, "radio : station injoignable, nouvelle tentative");
                if (c) { esp_http_client_cleanup(c); c = NULL; }
                vTaskDelay(pdMS_TO_TICKS(2500));
                continue;
            }
            esp_http_client_fetch_headers(c);
            ESP_LOGI(TAG, "radio : HTTP %d, je joue", esp_http_client_get_status_code(c));
            audio_playback_set_vol((uint8_t)radio_vol);
            radio_etat = 2;
            show_ui();
            continue;
        }

        if (n > 16000) n = 0;                            /* tampon sature : on jette */
        int r = esp_http_client_read(c, (char *)tampon + n, 20000 - n);
        if (r <= 0) {                                    /* flux coupe */
            ESP_LOGW(TAG, "radio : flux interrompu (%d)", r);
            if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); c = NULL; }
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        n += r;
        int consomme = mp3_flux(dec, tampon, n, &sr, &pos);
        if (consomme > 0) {
            inutile = 0;
            memmove(tampon, tampon + consomme, (size_t)(n - consomme));
            n -= consomme;
        } else {
            inutile += r;
            if (inutile > 400000) {                      /* ni mp3 : sans doute de l'aac */
                ESP_LOGW(TAG, "radio : %s n'est pas du mp3 (aac ?), j'arrete",
                         radios_url[radio_sel]);
                radio_etat = 0;
            }
        }
    }

    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    free(tampon);
    MP3FreeDecoder(dec);
    radio_vive = false;
    ESP_LOGI(TAG, "radio : arret");
    vTaskDelete(NULL);
}

/* ---- la phrase -> la voix de Google (gratuit, sans cle) ---- */
/* l'ancien nom, une requete = 190 caracteres */

/* ---- le TTS, en avance : pendant qu'un morceau se joue, le suivant se
   telecharge. C'est ce qui fait toute la difference sur la latence percue. ---- */

#define TTS_MORCEAU_MAX (160 * 1024)

typedef struct {
    char txt[210];
    uint8_t *mp3;
    int len;
    volatile bool fini;
} tts_lot;

static int tts_mp3_fetch(const char *texte, uint8_t *mp3, int cap)
{
    char t[210];
    snprintf(t, sizeof(t), "%.190s", texte);
    if (strlen(texte) > 190) {
        char *sp = strrchr(t, ' ');
        if (sp && sp > t + 40) *sp = 0;
    }
    char *enc = (char *)heap_caps_malloc(800, MALLOC_CAP_SPIRAM);
    char *url = (char *)heap_caps_malloc(1000, MALLOC_CAP_SPIRAM);
    if (!enc || !url) { if (enc) free(enc); if (url) free(url); return -1; }
    url_encode(t, enc, 800);
    snprintf(url, 1000, "%s%s&tl=%s", TTS_DEBUT, enc, langue);
    int64_t t0 = esp_timer_get_time();
    int r = http_get(url, TTS_UA, mp3, cap);
    if (r > 0)
        ESP_LOGI(TAG, "voix : %d octets mp3 en %.0f ms", r, (esp_timer_get_time() - t0) / 1000.0);
    else
        ESP_LOGW(TAG, "voix : le TTS de Google a repondu %d", r);
    free(enc); free(url);
    return r;
}

/* telecharge un morceau. ATTENTION : ne jamais appeler tts_lot_tache()
   directement depuis une tache qui doit continuer : elle fait vTaskDelete(NULL)
   et le handler HTTP qui l'appelait mourait sans jamais repondre (le socket
   restait ouvert -> /state muet apres deux appels). */
static void tts_lot_charge(tts_lot *L)
{
    L->len = tts_mp3_fetch(L->txt, L->mp3, TTS_MORCEAU_MAX);
    L->fini = true;
}

static void tts_lot_tache(void *arg)
{
    tts_lot_charge((tts_lot *)arg);
    vTaskDelete(NULL);
}

/* attend qu'un morceau soit pret, sans jamais bloquer indefiniment */
static bool tts_attendre(tts_lot *L)
{
    int64_t t0 = esp_timer_get_time();
    while (!L->fini) {
        if ((esp_timer_get_time() - t0) > 15000000) {
            ESP_LOGW(TAG, "voix : morceau perdu (15 s)");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static tts_lot *tts_lance(tts_lot *L)
{
    if (!L) return NULL;
    L->fini = false; L->len = -1;
    TaskHandle_t th = NULL;
    if (xTaskCreateWithCaps(tts_lot_tache, "tts_lot", 12288, L, 5, &th,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "voix : tache de telechargement non creee, on le fait ici");
        tts_lot_charge(L);         /* au pire, on telecharge sur place */
    }
    return L;
}

static void tts_parle(const char *texte)
{
    if (!texte || !*texte) return;
    if (radio_etat != 0) { radio_stop(); vTaskDelay(pdMS_TO_TICKS(120)); }

    tts_lot *lot = (tts_lot *)heap_caps_calloc(2, sizeof(tts_lot), MALLOC_CAP_SPIRAM);
    if (!lot) { ESP_LOGW(TAG, "voix : plus de PSRAM pour le TTS"); return; }
    bool ok = false;
    for (int i = 0; i < 2; i++) {
        lot[i].mp3 = (uint8_t *)heap_caps_malloc(TTS_MORCEAU_MAX, MALLOC_CAP_SPIRAM);
        if (lot[i].mp3) ok = true;
    }
    if (!ok) {
        ESP_LOGW(TAG, "voix : pas de tampon mp3");
        free(lot);
        return;
    }

    /* on decoupe : le premier morceau est court, pour que la voix demarre vite */
    const char *p = texte;
    int cur = 0;
    tts_lot *prec = NULL;

    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        size_t n = strlen(p);
        size_t cible = prec ? 180 : 90;
        size_t k = (n < cible) ? n : cible;
        if (k < n) {
            size_t cut = 0;
            for (size_t i = k; i > 30; i--) {
                char c = p[i - 1];
                if (c == '.' || c == '!' || c == '?' || c == ';' || c == ':') { cut = i; break; }
            }
            if (!cut)
                for (size_t i = k; i > 30; i--)
                    if (p[i - 1] == ' ') { cut = i; break; }
            if (cut) k = cut;
        }
        tts_lot *L = &lot[cur];
        if (L->fini) { free(L->mp3); L->mp3 = NULL; }   /* deja joue */
        if (!L->mp3) L->mp3 = (uint8_t *)heap_caps_malloc(TTS_MORCEAU_MAX, MALLOC_CAP_SPIRAM);
        if (!L->mp3) break;
        snprintf(L->txt, sizeof(L->txt), "%.*s", (int)k, p);
        L->fini = false; L->len = -1;

        if (!prec) {
            /* premier morceau : on attend son mp3 (on n'a rien a jouer) */
            tts_lot_charge(L);
        } else {
            /* on lance le telechargement du suivant AVANT de jouer celui-ci */
            tts_lance(L);
        }

        /* on joue le morceau precedent, qui est pret */
        if (prec) {
            if (tts_attendre(prec) && prec->len > 0) mp3_joue(prec->mp3, prec->len);
        }
        prec = L;
        cur = 1 - cur;
        p += k;
    }

    if (prec) {
        if (tts_attendre(prec) && prec->len > 0) mp3_joue(prec->mp3, prec->len);
    }
    for (int i = 0; i < 2; i++) if (lot[i].mp3) free(lot[i].mp3);
    free(lot);
}

/* ---- l'assistant : texte entendu -> cerveau -> voix ---- */
static void voix_dit(const char *phrase)
{
    snprintf(voix_reponse, sizeof(voix_reponse), "%.200s", phrase);
    voix_etat = 3;
    show_ui();
    tts_parle(phrase);
    voix_etat = 0;          /* sans ca le compagnon reste "en train de parler" */
    show_ui();
}

/* Le petit micro de la carte sort un signal faible : on remonte le niveau avant
   d'envoyer, sinon le service de reconnaissance n'entend rien. */
static void voix_normalise(uint8_t *pcm, int len)
{
    int16_t *s = (int16_t *)pcm;
    int n = len / 2, pk = 0;
    for (int i = 0; i < n; i++) {
        int v = s[i];
        if (v < 0) v = -v;
        if (v > pk) pk = v;
    }
    mic_crete = pk;
    if (pk < 40) return;                     /* vraiment rien : on n'amplifie pas le souffle */
    float g = 22937.0f / (float)pk;          /* on vise 70 % de la dynamique */
    if (g > 25.0f)  g = 25.0f;               /* au-dela, on amplifie surtout le bruit */
    if (g > 0.95f && g < 1.05f) return;      /* deja au bon niveau */
    for (int i = 0; i < n; i++) {
        float v = (float)s[i] * g;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        s[i] = (int16_t)v;
    }
    ESP_LOGI(TAG, "micro : crete %d, niveau ajuste x%.2f", pk, (double)g);
}

static void voix_tour(const uint8_t *pcm, int len)
{
    int64_t t0 = esp_timer_get_time();

    /* 1. reconnaissance */
    if (len < 3200) {                    /* moins de 0,1 s : rien de bon */
        voix_dit("Je n'ai rien entendu, tu peux repeter ?");
        return;
    }
    /* on ne garde que la partie ou l'on parle : moins d'octets a envoyer,
       et surtout une reconnaissance beaucoup plus rapide (elle grandit avec
       la duree de l'audio). */
    const int16_t *e16 = (const int16_t *)pcm;
    int total_ech = len / 2, d = 0, f = total_ech;
    for (int i = 0; i < total_ech; i++)
        if (e16[i] > 300 || e16[i] < -300) { d = i; break; }
    for (int i = total_ech - 1; i > d; i--)
        if (e16[i] > 300 || e16[i] < -300) { f = i; break; }
    d = (d > 1600) ? d - 1600 : 0;                     /* 100 ms de marge */
    f = (f + 1600 < total_ech) ? f + 1600 : total_ech;
    if (f - d < 3200) { d = 0; f = total_ech; }        /* trop court : on garde tout */
    const uint8_t *parle_pcm = pcm + d * 2;
    int parle_len = (f - d) * 2;

    voix_normalise((uint8_t *)parle_pcm, parle_len);   /* le micro est faible : on remonte */
    ESP_LOGI(TAG, "voix : %d octets captes, %d utiles apres rognage", len, parle_len);
    uint8_t *wav = (uint8_t *)heap_caps_malloc(parle_len + 44, MALLOC_CAP_SPIRAM);
    if (!wav) { voix_dit("Je suis fatigue, redemande-moi."); return; }
    wav_entete(wav, parle_len);
    memcpy(wav + 44, parle_pcm, parle_len);

    char texte[600] = "";
    voix_etat = 2;                       /* JE REFLECHIS */
    show_ui();
    esp_err_t e;
    if (groq_pret()) {
        ESP_LOGI(TAG, "voix : avant STT, interne libre %u o, plus gros bloc %u o",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        e = stt_groq(wav, parle_len + 44, texte, sizeof(texte));
    } else {
        /* pas de cle : on passe le PCM brut a la reconnaissance de Chrome */
        e = stt_sans_cle(parle_pcm, parle_len, texte, sizeof(texte));
    }
    free(wav);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "entendu en %.0f ms : \"%.120s\"", (t1 - t0) / 1000.0, texte);
    if (e != ESP_OK || strlen(texte) < 2) {
        ESP_LOGW(TAG, "reconnaissance muette (err=%s) : %.60s", esp_err_to_name(e), texte);
        voix_dit("Je n'ai pas bien compris, tu peux repeter ?");
        return;
    }
    chrono_stt_ms = (int)((t1 - t0) / 1000);
    snprintf(voix_entendu_txt, sizeof(voix_entendu_txt), "%.250s", texte);
    paroles_ajoute("MOI", texte);

    /* 2. cerveau */
    char dit[800] = "", emo[32] = "neutre";
    if (ds_chat(texte, dit, sizeof(dit), emo, sizeof(emo)) != ESP_OK) {
        ESP_LOGW(TAG, "cerveau muet (reseau ?)");
        voix_dit("Je n'arrive pas a reflechir la, on reessaie ?");
        return;
    }
    int64_t t2 = esp_timer_get_time();
    chrono_llm_ms = (int)((t2 - t1) / 1000);
    int ei = emo_by_name(emo);
    if (ei < 0) ei = art_expr_de_famille(8, 0);
    current_emotion = EXPR[ei].famille;
    bat5_du_pet = famille_vers_bat5(current_emotion);
    ESP_LOGI(TAG, "dit en %.0f ms : [%s] %.120s", (t2 - t1) / 1000.0, emo, dit);
    paroles_ajoute("MINO", dit);

    /* 3. la voix */
    voix_dit(dit);
    chrono_ms = (int)((esp_timer_get_time() - t0) / 1000);
    chrono_tts_ms = chrono_ms - chrono_stt_ms - chrono_llm_ms;
    ESP_LOGI(TAG, "chrono voix : ecoute+reco %.1f s, cerveau %.1f s, total %.1f s",
             (t1 - t0) / 1000000.0, (t2 - t1) / 1000000.0, (esp_timer_get_time() - t0) / 1000000.0);
}

/* ---- meteo (Open-Meteo) : remise a jour par ui_task ---- */
static void meteo_lire(void)
{
    static uint8_t buf[2048];
    int r = http_get(METEO_URL, NULL, buf, sizeof(buf) - 1);
    if (r <= 0) { ESP_LOGW(TAG, "meteo injoignable (%d)", r); return; }
    buf[r] = 0;
    cJSON *j = cJSON_Parse((char *)buf);
    cJSON *cur = j ? cJSON_GetObjectItem(j, "current") : NULL;
    cJSON *t = cur ? cJSON_GetObjectItem(cur, "temperature_2m") : NULL;
    cJSON *w = cur ? cJSON_GetObjectItem(cur, "weather_code") : NULL;
    if (cJSON_IsNumber(t) && cJSON_IsNumber(w)) {
        meteo_temp = (float)t->valuedouble;
        meteo_code = (int)w->valuedouble;
        meteo_ok = true;
        ESP_LOGI(TAG, "meteo %s : %.1f C, code %d", METEO_VILLE, (double)meteo_temp, meteo_code);
    }
    if (j) cJSON_Delete(j);
}

static const char *meteo_texte(int code)
{
    if (code < 0)               return "meteo en attente";
    if (code == 0)              return "grand soleil";
    if (code <= 2)              return "peu nuageux";
    if (code == 3)              return "nuageux";
    if (code <= 48)             return "brouillard";
    if (code <= 57)             return "bruine";
    if (code <= 67)             return "pluie";
    if (code <= 77)             return "neige";
    if (code <= 82)             return "averses";
    if (code <= 86)             return "neige";
    return "orage";
}

/* ------------------------------------------------------------------ */
/* carte micro SD (SDMMC 1 bit : CLK 39 / CMD 41 / D0 40)              */
/* ------------------------------------------------------------------ */
static esp_err_t qval(httpd_req_t *req, const char *key, char *out, size_t sz);

static sdmmc_card_t *sd_card = NULL;

static esp_err_t sd_mount(bool format)
{
    esp_vfs_fat_sdmmc_mount_config_t mc = {};
    mc.format_if_mount_failed = format;
    mc.max_files = 5;
    mc.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* 40 MHz : 2,3 -> 4,7 Mo/s bruts mesures */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK_PIN;
    slot.cmd = SD_MOSI_CMD_PIN;
    slot.d0 = SD_MISO_D0_PIN;

    esp_err_t r = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &sd_card);
    if (r == ESP_OK) {
        sd_ok = true;
        sdmmc_card_print_info(stdout, sd_card);
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info("/sdcard", &total, &freeb) == ESP_OK)
            ESP_LOGI(TAG, "SD : %.0f Mo libres sur %.0f Mo", (double)freeb / 1048576.0,
                     (double)total / 1048576.0);
    } else {
        sd_ok = false;
        ESP_LOGW(TAG, "SD non montee : %s", esp_err_to_name(r));
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Repartitionnement de la carte : FAT32 (fichiers) + zone brute      */
/* (memoire de debordement). Le MBR et le FAT32 sont ecrits a la main :*/
/* f_mkfs ne sait formater qu'un disque entier, pas une partition.     */
/* ------------------------------------------------------------------ */
#include "sdmmc_cmd.h"

static uint32_t part_info[4][3];    /* {start, size, type} en secteurs */
static int      part_n = 0;

static sdmmc_card_t *sd_raw_open(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK_PIN;
    slot.cmd = SD_MOSI_CMD_PIN;
    slot.d0  = SD_MISO_D0_PIN;
    sdmmc_card_t *card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!card) return NULL;
    if (sdmmc_card_init(&host, card) != ESP_OK) { free(card); return NULL; }
    return card;
}

static void part_lire_mbr(sdmmc_card_t *card)
{
    uint8_t mbr[512];
    part_n = 0;
    if (sdmmc_read_sectors(card, mbr, 0, 1) != ESP_OK) return;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint32_t start = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t size  = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
        if (size) {
            part_info[part_n][0] = start;
            part_info[part_n][1] = size;
            part_info[part_n][2] = e[4];
            part_n++;
        }
    }
}

static void p16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

/* ecrit en zero `n` secteurs a partir de `start` ; renvoie false si erreur */
static bool part_zero(sdmmc_card_t *card, uint32_t start, uint32_t n)
{
    const uint32_t par = 32;                 /* 16 Ko par appel (SRAM interne) */
    uint8_t *z = (uint8_t *)heap_caps_aligned_alloc(512, par * 512,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!z) return false;
    memset(z, 0, par * 512);
    bool ok = true;
    for (uint32_t s = 0; s < n && ok; s += par) {
        uint32_t k = (n - s < par) ? (n - s) : par;
        if (sdmmc_write_sectors(card, z, start + s, k) != ESP_OK) ok = false;
    }
    free(z);
    return ok;
}

/* MBR : partition 1 = FAT32 LBA (0x0C), partition 2 = brute (0xDA) */
static bool part_ecrire_mbr(sdmmc_card_t *card, uint32_t p1s, uint32_t p1n,
                            uint32_t p2s, uint32_t p2n)
{
    uint8_t mbr[512];
    memset(mbr, 0, sizeof(mbr));
    uint8_t *e1 = mbr + 446, *e2 = mbr + 462;
    e1[0] = 0x00; e1[4] = 0x0C;
    p32(e1 + 8, p1s);  p32(e1 + 12, p1n);
    if (p2n) {
        e2[0] = 0x00; e2[4] = 0xDA;
        p32(e2 + 8, p2s);  p32(e2 + 12, p2n);
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
    return sdmmc_write_sectors(card, mbr, 0, 1) == ESP_OK;
}

/* FAT32 fait main dans la partition [start, start+size) */
static esp_err_t part_formater(sdmmc_card_t *card, uint32_t start, uint32_t size,
                               uint32_t *clusters_out, uint32_t *fat_sz_out)
{
    uint8_t spc = 64;                            /* 32 Ko par cluster */
    if (size < 8 * 1024 * 1024) spc = 8;
    else if (size < 64 * 1024 * 1024) spc = 1;

    const uint32_t reserved = 32;
    uint32_t clusters = (size - reserved) / spc;
    uint32_t fat_sz = (uint32_t)(((uint64_t)(clusters + 2) * 4 + 511) / 512);
    clusters = (size - reserved - 2 * fat_sz) / spc;
    fat_sz = (uint32_t)(((uint64_t)(clusters + 2) * 4 + 511) / 512);
    if (fat_sz == 0 || clusters < 1000) return ESP_ERR_INVALID_SIZE;

    uint8_t bs[512];
    memset(bs, 0, sizeof(bs));
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "MSWIN4.1", 8);
    p16(bs + 11, 512);
    bs[13] = spc;
    p16(bs + 14, reserved);
    bs[16] = 2;
    p16(bs + 17, 0);
    p16(bs + 19, 0);
    bs[21] = 0xF8;
    p16(bs + 22, 0);
    p16(bs + 24, 63); p16(bs + 26, 255);
    p32(bs + 28, start);
    p32(bs + 32, size);
    p32(bs + 36, fat_sz);
    p16(bs + 40, 0);
    p16(bs + 42, 0);
    p32(bs + 44, 2);
    p16(bs + 48, 1);
    p16(bs + 50, 6);
    bs[64] = 0x80;
    bs[66] = 0x29;
    p32(bs + 67, 0x4D1A09B2);
    memcpy(bs + 71, "MINO       ", 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;

    if (sdmmc_write_sectors(card, bs, start, 1) != ESP_OK) return ESP_FAIL;

    uint8_t fsi[512];
    memset(fsi, 0, sizeof(fsi));
    p32(fsi + 0, 0x41615252);
    p32(fsi + 484, 0x61417272);
    p32(fsi + 488, 0xFFFFFFFF);
    p32(fsi + 492, 0xFFFFFFFF);
    p32(fsi + 508, 0xAA550000);
    if (sdmmc_write_sectors(card, fsi, start + 1, 1) != ESP_OK) return ESP_FAIL;
    if (sdmmc_write_sectors(card, bs,  start + 6, 1) != ESP_OK) return ESP_FAIL;
    if (sdmmc_write_sectors(card, fsi, start + 7, 1) != ESP_OK) return ESP_FAIL;

    /* les deux FAT a zero, puis les trois premieres entrees */
    if (!part_zero(card, start + reserved, fat_sz * 2)) return ESP_FAIL;
    uint8_t fat0[512];
    memset(fat0, 0, sizeof(fat0));
    p32(fat0 + 0, 0x0FFFFFF8);
    p32(fat0 + 4, 0x0FFFFFFF);
    p32(fat0 + 8, 0x0FFFFFFF);              /* cluster 2 = repertoire racine */
    if (sdmmc_write_sectors(card, fat0, start + reserved, 1) != ESP_OK) return ESP_FAIL;
    if (sdmmc_write_sectors(card, fat0, start + reserved + fat_sz, 1) != ESP_OK) return ESP_FAIL;

    /* repertoire racine vide */
    if (!part_zero(card, start + reserved + 2 * fat_sz, spc)) return ESP_FAIL;

    if (clusters_out) *clusters_out = clusters;
    if (fat_sz_out)   *fat_sz_out = fat_sz;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Zone d'echange (partition brute) : primitives de lecture/ecriture   */
/* en blocs de 16 Ko (meilleur couple mesure : 3,6 Mo/s ecriture,      */
/* 4,4 Mo/s lecture, contre 0,37 Mo/s via FATFS).                       */
/* L'ESP32-S3 n'a pas de MMU : ce n'est pas un swap OS, c'est une       */
/* memoire de debordement explicite (memcpy depuis/vers la PSRAM).      */
/* ------------------------------------------------------------------ */
#define SWAP_BLOC 16384

/* tampon relais alloue UNE fois : les allocations repetees de 16 Ko en RAM
   interne echouaient quand le plus gros bloc libre descendait sous 16 Ko. */
static uint8_t *swap_stage = NULL;

static uint8_t *swap_tampon(void)
{
    if (!swap_stage)
        swap_stage = (uint8_t *)heap_caps_aligned_alloc(512, SWAP_BLOC,
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!swap_stage)   /* repli : la PSRAM marche, mais bien plus lentement */
        swap_stage = (uint8_t *)heap_caps_aligned_alloc(512, SWAP_BLOC,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return swap_stage;
}

static bool swap_ecrire(sdmmc_card_t *card, uint32_t lba_base, size_t offset,
                        const void *src, size_t len)
{
    uint8_t *stage = swap_tampon();
    if (!stage) return false;
    const uint8_t *p = (const uint8_t *)src;
    bool ok = true;
    while (len && ok) {
        size_t n = len < SWAP_BLOC ? len : SWAP_BLOC;
        size_t plein = (n + 511) & ~(size_t)511;       /* secteurs entiers */
        memcpy(stage, p, n);
        if (plein > n) memset(stage + n, 0, plein - n);
        ok = sdmmc_write_sectors(card, stage, lba_base + (uint32_t)(offset / 512),
                                 plein / 512) == ESP_OK;
        offset += plein; p += n; len -= n;
    }
    return ok;
}

static bool swap_lire(sdmmc_card_t *card, uint32_t lba_base, size_t offset,
                      void *dst, size_t len)
{
    uint8_t *stage = swap_tampon();
    if (!stage) return false;
    uint8_t *p = (uint8_t *)dst;
    bool ok = true;
    while (len && ok) {
        size_t n = len < SWAP_BLOC ? len : SWAP_BLOC;
        size_t plein = (n + 511) & ~(size_t)511;
        ok = sdmmc_read_sectors(card, stage, lba_base + (uint32_t)(offset / 512),
                                plein / 512) == ESP_OK;
        if (ok) memcpy(p, stage, n);
        offset += plein; p += n; len -= n;
    }
    return ok;
}

/* /part                       -> etat des partitions
   /part?op=repart&fat_go=100  -> repartitionne (destructif) : FAT32 100 Go + reste brut
   /part?op=restore            -> carte entiere en FAT32 (retour arriere)
   /part?op=rawbench&bloc=64   -> debit brut dans la zone swap                */
static esp_err_t h_part(httpd_req_t *req)
{
    char v[24] = "";
    char op[24] = "info";
    qval(req, "op", op, sizeof(op));

    if (!strcmp(op, "info")) {
        bool possede = !sd_ok;
        sdmmc_card_t *card = sd_ok ? sd_card : sd_raw_open();
        if (!card) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "carte");
        part_lire_mbr(card);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "secteurs_carte", (double)card->csd.capacity);
        cJSON *a = cJSON_AddArrayToObject(r, "partitions");
        for (int i = 0; i < part_n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "index", i + 1);
            cJSON_AddNumberToObject(o, "type_hex", part_info[i][2]);
            cJSON_AddNumberToObject(o, "debut_lba", (double)part_info[i][0]);
            cJSON_AddNumberToObject(o, "secteurs", (double)part_info[i][1]);
            cJSON_AddNumberToObject(o, "go", (double)part_info[i][1] * 512.0 / 1073741824.0);
            cJSON_AddItemToArray(a, o);
        }
        if (possede) free(card);
        return send_json(req, r);
    }

    if (!strcmp(op, "repart") || !strcmp(op, "restore")) {
        /* on ecrit avec le handle du montage en cours : rouvrir la carte sans
           reinitialiser le host SDMMC faisait planter la carte (panic). */
        if (!sd_ok || !sd_card)
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "carte non montee");
        sdmmc_card_t *card = sd_card;

        uint32_t total = card->csd.capacity;
        uint32_t p1s = 2048;
        uint32_t p1n, p2n = 0, p2s = 0;
        if (!strcmp(op, "restore")) {
            p1n = total - p1s;
        } else {
            int go = 100;
            if (qval(req, "fat_go", v, sizeof(v)) == ESP_OK) go = atoi(v);
            if (go < 2) go = 2;
            uint32_t want = (uint32_t)((uint64_t)go * 1000 * 1000 * 1000 / 512);
            if (want > total - p1s - 2048) want = total - p1s - 2048;
            p1n = (want / 2048) * 2048;               /* aligne sur 1 Mo */
            p2s = p1s + p1n;
            p2n = total - p2s;
        }

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "op", op);
        bool mbr_ok = part_ecrire_mbr(card, p1s, p1n, p2s, p2n);
        uint32_t clusters = 0, fat_sz = 0;
        esp_err_t err = mbr_ok ? part_formater(card, p1s, p1n, &clusters, &fat_sz)
                               : ESP_FAIL;
        esp_vfs_fat_sdcard_unmount("/sdcard", sd_card);   /* puis on remonte */
        sd_ok = false;

        cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
        cJSON_AddNumberToObject(r, "fat_go", (double)p1n * 512.0 / 1e9);
        cJSON_AddNumberToObject(r, "swap_go", (double)p2n * 512.0 / 1e9);
        cJSON_AddNumberToObject(r, "clusters", (double)clusters);
        cJSON_AddNumberToObject(r, "fat_secteurs", (double)fat_sz);
        if (err != ESP_OK) cJSON_AddStringToObject(r, "erreur", esp_err_to_name(err));
        else {
            esp_err_t m = sd_mount(false);             /* remonte la nouvelle FAT32 */
            cJSON_AddBoolToObject(r, "remonte", m == ESP_OK);
            if (m == ESP_OK) {
                mkdir("/sdcard/mino", 0777);
                mkdir("/sdcard/backup", 0777);
            }
        }
        return send_json(req, r);
    }

    if (!strcmp(op, "st")) {          /* test des primitives de la zone d'echange */
        int mo = 2;
        if (qval(req, "n", v, sizeof(v)) == ESP_OK) mo = atoi(v);
        if (mo < 1 || mo > 8) mo = 2;
        size_t len = (size_t)mo * 1024 * 1024;
        uint8_t *mem = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        cJSON *r = cJSON_CreateObject();
        if (!mem) { cJSON_AddBoolToObject(r, "ok", false); return send_json(req, r); }
        memset(mem, 0xC3, len);
        bool possede = !sd_ok;
        sdmmc_card_t *card = sd_ok ? sd_card : sd_raw_open();
        if (!card) { free(mem); cJSON_AddBoolToObject(r, "ok", false); return send_json(req, r); }
        part_lire_mbr(card);
        uint32_t base = (part_n > 1) ? part_info[1][0] : 0;
        int64_t t0 = esp_timer_get_time();
        bool w = swap_ecrire(card, base, 0, mem, len);
        int64_t t1 = esp_timer_get_time();
        memset(mem, 0, len);
        bool rd = swap_lire(card, base, 0, mem, len);
        int64_t t2 = esp_timer_get_time();
        bool verif = true;
        for (size_t i = 0; i < len; i += 4096) if (mem[i] != 0xC3) { verif = false; break; }
        free(mem);
        if (possede) free(card);
        cJSON_AddBoolToObject(r, "ecrit", w);
        cJSON_AddBoolToObject(r, "relu", rd);
        cJSON_AddBoolToObject(r, "verifie", verif);
        cJSON_AddNumberToObject(r, "ecriture_mo_s", (double)mo / ((double)(t1 - t0) / 1e6));
        cJSON_AddNumberToObject(r, "lecture_mo_s",  (double)mo / ((double)(t2 - t1) / 1e6));
        return send_json(req, r);
    }

    if (!strcmp(op, "rawtest")) {
        bool possede = !sd_ok;
        sdmmc_card_t *card = sd_ok ? sd_card : sd_raw_open();
        if (!card) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "carte");
        uint32_t lba = card->csd.capacity - 100;
        uint8_t w[512], r[512];
        memset(w, 0x5A, sizeof(w));
        esp_err_t e1 = sdmmc_write_sectors(card, w, lba, 1);
        esp_err_t e2 = sdmmc_read_sectors(card, r, lba, 1);
        cJSON *r2 = cJSON_CreateObject();
        cJSON_AddBoolToObject(r2, "ecrit", e1 == ESP_OK);
        cJSON_AddBoolToObject(r2, "relu", e2 == ESP_OK);
        cJSON_AddBoolToObject(r2, "identique", e2 == ESP_OK && !memcmp(w, r, 512));
        cJSON_AddNumberToObject(r2, "lba", (double)lba);
        if (possede) free(card);
        return send_json(req, r2);
    }

    if (!strcmp(op, "rawbench")) {
        int bloc = 64 * 1024;
        if (qval(req, "bloc", v, sizeof(v)) == ESP_OK) bloc = atoi(v) * 1024;
        if (bloc < 4096 || bloc > 256 * 1024) bloc = 64 * 1024;
        bool possede = !sd_ok;
        sdmmc_card_t *card = sd_ok ? sd_card : sd_raw_open();
        if (!card) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "carte");
        part_lire_mbr(card);
        if (part_n < 2) { if (possede) free(card);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "aucune zone brute"); }
        uint32_t start = part_info[1][0];
        uint32_t nsec = (4 * 1024 * 1024) / 512;
        uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(512, bloc,
                                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!buf)   /* le pool DMA interne est petit : on tente la PSRAM */
            buf = (uint8_t *)heap_caps_aligned_alloc(512, bloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        cJSON *r = cJSON_CreateObject();
        if (!buf) { if (possede) free(card);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc"); }
        memset(buf, 0x33, bloc);
        int64_t t0 = esp_timer_get_time();
        for (uint32_t s = 0; s < nsec; s += bloc / 512)
            sdmmc_write_sectors(card, buf, start + s, bloc / 512);
        int64_t t1 = esp_timer_get_time();
        for (uint32_t s = 0; s < nsec; s += bloc / 512)
            sdmmc_read_sectors(card, buf, start + s, bloc / 512);
        int64_t t2 = esp_timer_get_time();
        free(buf);
        if (possede) free(card);
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddNumberToObject(r, "bloc_ko", bloc / 1024);
        cJSON_AddNumberToObject(r, "ecriture_mo_s", 4.0 / ((double)(t1 - t0) / 1e6));
        cJSON_AddNumberToObject(r, "lecture_mo_s",  4.0 / ((double)(t2 - t1) / 1e6));
        return send_json(req, r);
    }

    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "op inconnu");
}

static esp_err_t h_sd_list(httpd_req_t *req)
{
    /* /sd            -> racine de la carte
       /sd?p=/backup  -> un sous-dossier                                      */
    char q[128] = {0}, val[100] = {0}, chemin[128];
    snprintf(chemin, sizeof(chemin), "/sdcard");
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "p", val, sizeof(val)) == ESP_OK) {
        if (val[0] != '/' || strstr(val, ".."))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "p invalide");
        snprintf(chemin, sizeof(chemin), "/sdcard%s", val);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "presente", sd_ok);
    cJSON_AddStringToObject(r, "dossier", chemin);
    cJSON *files = cJSON_AddArrayToObject(r, "fichiers");
    if (sd_ok) {
        DIR *d = opendir(chemin);
        if (d) {
            struct dirent *e;
            int n = 0;
            while ((e = readdir(d)) && n < 40) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                /* n'affiche que les noms imprimables : une entree de repertoire
                   abimee ne doit pas pourrir toute la reponse JSON */
                int ok = 1;
                for (const char *c = e->d_name; *c; c++)
                    if ((unsigned char)*c < 0x20) { ok = 0; break; }
                char plein[400];
                struct stat st;
                long taille = -1;
                snprintf(plein, sizeof(plein), "%s/%.255s", chemin, e->d_name);
                if (stat(plein, &st) == 0) taille = (long)st.st_size;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "nom", e->d_name);
                cJSON_AddNumberToObject(o, "taille", (double)taille);
                cJSON_AddBoolToObject(o, "lisible", ok);
                cJSON_AddItemToArray(files, o);
                n++;
            }
            closedir(d);
        }
    }
    return send_json(req, r);
}

/* ------------------------------------------------------------------ */
/* /swap : la carte SD comme memoire de debordement                    */
/* L'ESP32-S3 n'a pas de MMU : pas de swap/pagination possible. Ce qui */
/* marche, c'est un cache d'ecriture en PSRAM vide vers la SD en gros  */
/* blocs, et un stockage de donnees rarement lues. op=bench mesure le  */
/* meilleur couple (taille de bloc, debit) reellement atteignable.      */
/* ------------------------------------------------------------------ */
static esp_err_t h_swap(httpd_req_t *req)
{
    char v[24] = "";
    cJSON *r = cJSON_CreateObject();

    /* op=fatfs : meme mesure mais via l'API FatFs brute (sans newlib/VFS) */
    if (qval(req, "op", v, sizeof(v)) == ESP_OK && !strcmp(v, "fatfs")) {
        int bloc = 65536;
        if (qval(req, "bloc", v, sizeof(v)) == ESP_OK) bloc = atoi(v) * 1024;
        if (bloc < 512 || bloc > 262144) bloc = 65536;
        size_t total = 4 * 1024 * 1024;
        uint8_t *buf = (uint8_t *)heap_caps_malloc(bloc, MALLOC_CAP_SPIRAM);
        if (!buf) { cJSON_AddBoolToObject(r, "ok", false); return send_json(req, r); }
        memset(buf, 0x5C, bloc);
        FIL fil; UINT bw = 0, br = 0;
        FRESULT fr = f_open(&fil, "fbench.bin", FA_WRITE | FA_CREATE_ALWAYS);
        int64_t t0 = esp_timer_get_time();
        size_t fait = 0;
        while (fr == FR_OK && fait < total) { f_write(&fil, buf, bloc, &bw); fait += bw; if (!bw) break; }
        if (fr == FR_OK) f_sync(&fil);
        f_close(&fil);
        int64_t t1 = esp_timer_get_time();
        fr = f_open(&fil, "fbench.bin", FA_READ);
        size_t lu = 0;
        while (fr == FR_OK && lu < total) { f_read(&fil, buf, bloc, &br); lu += br; if (!br) break; }
        f_close(&fil);
        int64_t t2 = esp_timer_get_time();
        f_unlink("fbench.bin");
        free(buf);
        cJSON_AddStringToObject(r, "via", "fatfs-brut");
        cJSON_AddNumberToObject(r, "bloc_ko", bloc / 1024);
        cJSON_AddNumberToObject(r, "ecriture_mo_s",
            (double)fait / ((double)(t1 - t0) / 1e6) / 1048576.0);
        cJSON_AddNumberToObject(r, "lecture_mo_s",
            (double)lu / ((double)(t2 - t1) / 1e6) / 1048576.0);
        return send_json(req, r);
    }

    if (qval(req, "op", v, sizeof(v)) == ESP_OK && !strcmp(v, "bench")) {
        int bloc = 65536;
        if (qval(req, "bloc", v, sizeof(v)) == ESP_OK) bloc = atoi(v) * 1024;
        if (bloc < 512 || bloc > 262144) bloc = 65536;
        size_t total = 4 * 1024 * 1024;
        if (qval(req, "mo", v, sizeof(v)) == ESP_OK) {
            int mo = atoi(v);
            if (mo > 0 && mo <= 16) total = (size_t)mo * 1024 * 1024;
        }
        uint8_t *buf = (uint8_t *)heap_caps_malloc(bloc, MALLOC_CAP_SPIRAM);
        if (!buf) { cJSON_AddBoolToObject(r, "ok", false);
                    cJSON_AddStringToObject(r, "erreur", "alloc");
                    return send_json(req, r); }
        memset(buf, 0xA7, bloc);
        int64_t t0 = esp_timer_get_time();
        FILE *f = fopen("/sdcard/swap.bin", "wb");
        size_t fait = 0;
        while (f && fait < total) { fwrite(buf, 1, bloc, f); fait += bloc; }
        if (f) { fflush(f); fsync(fileno(f)); fclose(f); }
        int64_t t1 = esp_timer_get_time();
        f = fopen("/sdcard/swap.bin", "rb");
        size_t lu = 0;
        while (f && lu < total) { size_t k = fread(buf, 1, bloc, f); if (!k) break; lu += k; }
        if (f) fclose(f);
        int64_t t2 = esp_timer_get_time();
        remove("/sdcard/swap.bin");
        free(buf);
        cJSON_AddBoolToObject(r, "ok", fait == total && lu == total);
        cJSON_AddNumberToObject(r, "bloc_ko", bloc / 1024);
        cJSON_AddNumberToObject(r, "ecriture_mo_s",
            (double)fait / ((double)(t1 - t0) / 1e6) / 1048576.0);
        cJSON_AddNumberToObject(r, "lecture_mo_s",
            (double)lu / ((double)(t2 - t1) / 1e6) / 1048576.0);
        return send_json(req, r);
    }

    uint64_t tot = 0, libre = 0;
    esp_vfs_fat_info("/sdcard", &tot, &libre);
    cJSON_AddNumberToObject(r, "total_mo", (double)tot / 1048576.0);
    cJSON_AddNumberToObject(r, "libre_mo", (double)libre / 1048576.0);
    cJSON_AddStringToObject(r, "note",
        "pas de swap OS (pas de MMU) ; utiliser op=bench pour le debit par bloc");
    return send_json(req, r);
}

/* /sdformat?f=1 : reformate VRAIMENT la carte (efface tout).
   Sans f=1 on se contente de remonter : le mount reussit toujours sur une carte
   deja formatee, donc l'ancienne version ne formattoir jamais rien. */
static esp_err_t h_sd_format(httpd_req_t *req)
{
    char v[8] = "";
    bool vraiment = (qval(req, "f", v, sizeof(v)) == ESP_OK && v[0] == '1');

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "formate", vraiment);
    if (!vraiment) {
        cJSON_AddStringToObject(r, "note", "ajouter f=1 pour effacer la carte");
        return send_json(req, r);
    }
    if (!sd_ok || !sd_card) {
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "erreur", "carte absente");
        return send_json(req, r);
    }
    sd_ok = false;
    esp_err_t err = esp_vfs_fat_sdcard_format("/sdcard", sd_card);
    if (err == ESP_OK) {
        /* le montage reste valide apres format ; on recree l'arborescence */
        mkdir("/sdcard/mino", 0777);
        mkdir("/sdcard/backup", 0777);
        sd_ok = true;
    }
    cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
    if (err != ESP_OK) cJSON_AddStringToObject(r, "erreur", esp_err_to_name(err));
    return send_json(req, r);
}

/* ------------------------------------------------------------------ */
/* audio : micro + haut-parleur (ES8311)                               */
/* ------------------------------------------------------------------ */
#define AUDIO_MAX_FRAMES (16 * 1000 * 15)     /* 15 s a 16 kHz */

static esp_err_t h_rec(httpd_req_t *req)
{
    char v[32];
    int ms = 3000;
    if (qval(req, "ms", v, sizeof(v)) == ESP_OK) ms = atoi(v);
    if (ms < 300) ms = 300;
    if (ms > 15000) ms = 15000;

    int frames = ms * 16;                     /* 16 echantillons / ms */
    if (!rec_pcm || frames > AUDIO_MAX_FRAMES) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    static uint8_t stereo[512];

    int done = 0;
    while (done < frames) {
        int todo = frames - done;
        int chunk = (todo > 256) ? 256 : todo;         /* frames par tour */
        audio_playback_read(stereo, chunk * 4);        /* 2 canaux * 2 octets */
        for (int i = 0; i < chunk; i++) {
            int16_t l = (int16_t)(stereo[i * 4] | (stereo[i * 4 + 1] << 8));
            rec_pcm[(done + i) * 2] = (uint8_t)(l & 0xff);
            rec_pcm[(done + i) * 2 + 1] = (uint8_t)((l >> 8) & 0xff);
        }
        done += chunk;
    }

    int data_len = done * 2;
    uint8_t hdr[44] = {0};
    uint32_t chunk_size = data_len + 36;
    uint32_t byte_rate = 16000 * 2;
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &chunk_size, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmt = 16; hdr[16] = 16; hdr[20] = 1; hdr[22] = 1;
    hdr[24] = 16000 & 0xff; hdr[25] = (16000 >> 8) & 0xff;
    hdr[28] = byte_rate & 0xff; hdr[29] = (byte_rate >> 8) & 0xff;
    hdr[32] = 2; hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_len, 4);
    (void)fmt;

    ESP_LOGI(TAG, "micro : %.1f s enregistrees", done / 16000.0);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));
    httpd_resp_send_chunk(req, (const char *)rec_pcm, data_len);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_play(httpd_req_t *req)
{
    static uint8_t in[1024];
    static uint8_t out[2048];
    int total = 0;
    audio_playback_set_vol(90);
    while (1) {
        int got = httpd_req_recv(req, (char *)in, sizeof(in));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) break;
        int frames = got / 2;
        for (int i = 0; i < frames; i++) {
            out[i * 4] = in[i * 2];
            out[i * 4 + 1] = in[i * 2 + 1];
            out[i * 4 + 2] = in[i * 2];
            out[i * 4 + 3] = in[i * 2 + 1];
        }
        audio_playback_write(out, frames * 4);
        total += got;
    }
    ESP_LOGI(TAG, "haut-parleur : %d octets joues", total);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "octets", total);
    return send_json(req, r);
}

/* POST /fichier?p=/backup/x.tar.gz : ecrit le corps recu sur la carte SD.
   GET  /fichier?p=/backup/x.tar.gz : renvoie le fichier.
   Sert a sauvegarder/relire des fichiers sur la SD sans lecteur de carte. */
static esp_err_t h_fichier(httpd_req_t *req)
{
    char q[256] = {0}, val[200] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "p", val, sizeof(val)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "p= requis");
    if (val[0] != '/')
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "chemin absolu requis");
    if (strstr(val, ".."))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ".. interdit");

    char chemin[256];
    snprintf(chemin, sizeof(chemin), "/sdcard%s", val);
    if (strlen(chemin) >= sizeof(chemin) - 1)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "chemin trop long");

    if (req->method == HTTP_POST) {
        char *slash = strrchr(chemin, '/');
        if (slash && slash > chemin + 7) {          /* cree le dossier parent */
            *slash = 0;
            mkdir(chemin, 0777);
            *slash = '/';
        }
        FILE *f = fopen(chemin, "wb");
        if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ouverture");
        static char buf[32768];              /* gros blocs : la SD aime */
        long total = 0;
        while (1) {
            int got = httpd_req_recv(req, buf, sizeof(buf));
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (got <= 0) break;
            fwrite(buf, 1, got, f);
            total += got;
        }
        fflush(f);
        fsync(fileno(f));
        fclose(f);
        ESP_LOGI(TAG, "fichier recu : %s (%ld octets)", chemin, total);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "fichier", chemin);
        cJSON_AddNumberToObject(r, "octets", total);
        return send_json(req, r);
    }

    FILE *f = fopen(chemin, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "absent");
    httpd_resp_set_type(req, "application/octet-stream");
    static char buf[32768];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* aides audio reutilisables (console serie et API partagent)          */
/* ------------------------------------------------------------------ */
static void wav_header(uint8_t hdr[44], int data_len)
{
    uint32_t chunk_size = (uint32_t)data_len + 36;
    uint32_t byte_rate = 16000 * 2;
    memset(hdr, 0, 44);
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &chunk_size, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16; hdr[20] = 1; hdr[22] = 1;
    hdr[24] = 16000 & 0xff; hdr[25] = (16000 >> 8) & 0xff;
    hdr[28] = byte_rate & 0xff; hdr[29] = (byte_rate >> 8) & 0xff;
    hdr[32] = 2; hdr[34] = 16;
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_len, 4);
}

static int audio_record_at(int off, int frames, int *peak)
{
    static uint8_t stereo[512];
    int done = 0, mx = 0;
    if (!rec_pcm) return 0;
    if (off + frames > AUDIO_MAX_FRAMES) frames = AUDIO_MAX_FRAMES - off;
    while (done < frames) {
        int todo = frames - done;
        int chunk = (todo > 256) ? 256 : todo;
        audio_playback_read(stereo, chunk * 4);
        for (int i = 0; i < chunk; i++) {
            int16_t l = (int16_t)(stereo[i * 4] | (stereo[i * 4 + 1] << 8));
            int a = l < 0 ? -l : l;
            if (a > mx) mx = a;
            rec_pcm[(off + done + i) * 2] = (uint8_t)(l & 0xff);
            rec_pcm[(off + done + i) * 2 + 1] = (uint8_t)((l >> 8) & 0xff);
        }
        done += chunk;
    }
    if (peak) *peak = mx;
    return done;
}

/* enregistre `frames` echantillons (16 kHz) en mono dans rec_pcm */
static int audio_record_mono(int frames, int *peak)
{
    return audio_record_at(0, frames, peak);
}

static void audio_play_mono(int frames)
{
    /* gros paquets : chaque appel a l'I2S coute ~40 ms quelle que soit la
       taille, avec des paquets de 256 trames la lecture trainait 3 a 4 fois
       plus longtemps que la duree reelle du son. */
    static uint8_t out[8192];
    const int per = 1024;                 /* trames par appel (4096 octets) */
    int done = 0;
    if (!rec_pcm) return;
    audio_playback_set_vol(90);
    while (done < frames) {
        int todo = frames - done;
        int chunk = (todo > per) ? per : todo;
        for (int i = 0; i < chunk; i++) {
            out[i * 4] = rec_pcm[(done + i) * 2];
            out[i * 4 + 1] = rec_pcm[(done + i) * 2 + 1];
            out[i * 4 + 2] = rec_pcm[(done + i) * 2];
            out[i * 4 + 3] = rec_pcm[(done + i) * 2 + 1];
        }
        audio_playback_write(out, chunk * 4);
        done += chunk;
        if (act_on) {                       /* la barre suit la lecture */
            act_pct = (done * 100) / frames;
            if (act_pct > 100) act_pct = 100;
        }
    }
}

static int audio_sd_write(const char *path, const uint8_t *mono, int data_len)
{
    if (!sd_ok) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    uint8_t hdr[44];
    wav_header(hdr, data_len);
    fwrite(hdr, 1, sizeof(hdr), f);
    size_t w = fwrite(mono, 1, data_len, f);
    fclose(f);
    return (int)w;
}

static void ui_beep(void)
{
    if (!rec_pcm) return;
    int n = 16000 / 4;
    if (n > AUDIO_MAX_FRAMES) n = AUDIO_MAX_FRAMES;
    for (int i = 0; i < n; i++) {
        int16_t v = (int16_t)lroundf(0.30f * 32767.0f * sinf(2.0f * 3.14159265f * 660.0f * (float)i / 16000.0f));
        rec_pcm[i * 2] = (uint8_t)(v & 0xff);
        rec_pcm[i * 2 + 1] = (uint8_t)((v >> 8) & 0xff);
    }
    audio_play_mono(n);
}

/* ------------------------------------------------------------------ */
/* commandes tapees sur la console serie USB                           */
/* ------------------------------------------------------------------ */
/* ecriture binaire directe sur la console USB.
   Le peripherique USB Serial/JTAG n'accepte pas un gros tampon en une fois
   (et rec_pcm est en PSRAM) : on decoupe en tranches de 4 ko. */
static void serial_write_bulk(const uint8_t *data, int len)
{
    int off = 0, essais = 0;
    while (off < len && essais < 4000) {
        int n = len - off;
        if (n > 4096) n = 4096;
        int w = usb_serial_jtag_write_bytes(data + off, n, pdMS_TO_TICKS(2000));
        if (w <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            essais++;
            continue;
        }
        off += w;
    }
    if (off < len) ESP_LOGW(TAG, "ecriture USB incomplete : %d sur %d octets", off, len);
}

static void mino_handle_line(const char *line)
{
    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        ESP_LOGI(TAG, "commandes : <texte> (DeepSeek), faces, face <nom>, ui [0-4|f], touches, "
                      "wake, getrec, pcm begin|end, radio start|stop, fb, capteurs, offset <x>, secu, "
                      "secuscan, echo [s], rec [s], sd, wget <url>, state, memo, note <t>, reboot");
        return;
    }
    if (!strcmp(line, "faces")) {
        for (int i = 0; i < EXPR_N; i++) {
            show_face(i);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        return;
    }
    if (!strncmp(line, "face ", 5)) {
        int e = emo_by_name(line + 5);
        if (e < 0) {
            ESP_LOGW(TAG, "emotion inconnue : %s", line + 5);
        } else {
            current_emotion = e;
            show_face(e);
        }
        return;
    }
    if (!strcmp(line, "state")) {
        ESP_LOGI(TAG, "emotion=%s  stade=%s age=%u j sante=%u%%  RAM libre=%u  PSRAM libre=%u  SD=%s",
                 EXPR[ui_face].nom, pet_stade_nom(pet_stade(&pet)),
                 (unsigned)(pet.age_s / 86400), pet_sante(&pet),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), sd_ok ? "oui" : "non");
        return;
    }
    if (!strcmp(line, "memo")) {
        cJSON *notes = cJSON_GetObjectItem(mem_obj, "notes");
        cJSON *it = NULL;
        ESP_LOGI(TAG, "memoire (%d notes) :", notes ? cJSON_GetArraySize(notes) : 0);
        cJSON_ArrayForEach(it, notes) if (cJSON_IsString(it)) ESP_LOGI(TAG, "  - %s", it->valuestring);
        return;
    }
    if (!strncmp(line, "note ", 5)) {
        mem_add(line + 5);
        return;
    }
    if (!strcmp(line, "reboot")) {
        esp_restart();
        return;
    }
    if (!strncmp(line, "echo", 4)) {
        int sec = 3;
        if (line[4] == ' ') sec = atoi(line + 5);
        if (sec < 1) sec = 1;
        if (sec > 10) sec = 10;
        ESP_LOGI(TAG, "micro : j'ecoute %d s...", sec);
        int peak = 0;
        int n = audio_record_mono(sec * 16000, &peak);
        ESP_LOGI(TAG, "micro : %d echantillons, crete=%d/32767", n, peak);
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "haut-parleur : je repasse l'enregistrement");
        audio_play_mono(n);
        ESP_LOGI(TAG, "haut-parleur : fini");
        return;
    }
    if (!strncmp(line, "rec", 3)) {
        int sec = 3;
        if (line[3] == ' ') sec = atoi(line + 4);
        if (sec < 1) sec = 1;
        if (sec > 15) sec = 15;
        int peak = 0;
        int n = audio_record_mono(sec * 16000, &peak);
        rec_last_bytes = n * 2;                    /* pour getrec */
        int w = audio_sd_write("/sdcard/mic.wav", rec_pcm, n * 2);
        ESP_LOGI(TAG, "rec : %d s, crete=%d, %d octets ecrits sur la SD", n / 16000, peak, w);
        return;
    }
    if (!strcmp(line, "sd")) {
        if (!sd_ok) { ESP_LOGW(TAG, "pas de carte SD"); return; }
        DIR *d = opendir("/sdcard");
        if (!d) { ESP_LOGW(TAG, "racine SD illisible"); return; }
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d)) && n < 25) {
            if (e->d_name[0] == '.') continue;
            int ok = 1;
            for (const char *p = e->d_name; *p; p++)
                if ((unsigned char)*p < 0x20) { ok = 0; break; }
            if (!ok) continue;                     /* entree FAT parasite */
            struct stat st;
            char p[300];
            snprintf(p, sizeof(p), "/sdcard/%s", e->d_name);
            if (stat(p, &st) != 0) continue;
            ESP_LOGI(TAG, "  %8ld  %s", (long)st.st_size, e->d_name);
            n++;
        }
        closedir(d);
        ESP_LOGI(TAG, "(%d entrees lisibles)", n);
        return;
    }
    if (!strncmp(line, "wget ", 5)) {
        esp_http_client_config_t cfg = {};
        cfg.max_redirection_count = 4;   /* FG Radio et autres : suivre les 302 */
        cfg.url = line + 5;
        cfg.timeout_ms = 8000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) return;
        esp_err_t r = esp_http_client_open(c, 0);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "wget : %s injoignable (%s)", line + 5, esp_err_to_name(r));
        } else {
            int len = esp_http_client_fetch_headers(c);
            int status = esp_http_client_get_status_code(c);
            char buf[65];
            int n = esp_http_client_read(c, buf, sizeof(buf) - 1);
            if (n < 0) n = 0;
            buf[n] = 0;
            ESP_LOGI(TAG, "wget %s -> HTTP %d, %d octets, debut='%s'", line + 5, status, len, buf);
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return;
    }

    if (!strcmp(line, "secu")) {
        secu_collect();
        if (ui_page == 99) show_ui();
        ESP_LOGI(TAG, "--- analyse reseau ---");
        ESP_LOGI(TAG, "  reseau   : %s (%s)", sec_ssid, sec_bssid);
        ESP_LOGI(TAG, "  securite : %s / %s, canal %d, %d dBm, WPS %s, WPA3 %s",
                 sec_auth, sec_ciph, sec_chan, sec_rssi,
                 sec_wps ? "oui" : "non", sec_wpa3 ? "oui" : "non");
        ESP_LOGI(TAG, "  ip       : %s  passerelle %s (%s)", sec_ip, sec_gw,
                 sec_gw_ok ? "repond" : "muette");
        ESP_LOGI(TAG, "  dns      : %s (%s)   internet : %s", sec_dns,
                 sec_dns_ok ? "resout" : "ne resout pas", sec_net_ok ? "ok" : "ko");
        ESP_LOGI(TAG, "  voisinage: %d reseaux vus, %d en clair", sec_aps, sec_open);
        ESP_LOGI(TAG, "  local    : %d machine(s) | risque %d/5 | %s",
                 sec_hotes, sec_risque, sec_alerte);
        return;
    }
    if (!strcmp(line, "secuscan")) {
        secu_sweep();
        ESP_LOGI(TAG, "balayage du reseau local lance (environ 40 s)");
        return;
    }
    if (!strncmp(line, "tone", 4)) {
        tone_hz = 440;
        tone_sec = 3;
        if (line[4] == ' ') {
            tone_hz = atoi(line + 5);
            const char *sp = strchr(line + 5, ' ');
            if (sp) tone_sec = atoi(sp + 1);
        }
        if (tone_hz < 50 || tone_hz > 8000) tone_hz = 440;
        if (tone_sec < 1 || tone_sec > 30) tone_sec = 3;
        ESP_LOGI(TAG, "test son : %d Hz pendant %d s", tone_hz, tone_sec);
        xTaskCreate(tone_task, "tonetest", 4096, NULL, 4, NULL);
        return;
    }
    if (!strncmp(line, "prog", 4)) {
        /* prog <0-100|indet|off> <libelle> : le PC pilote la barre de progression */
        if (!strcmp(line, "prog off")) {
            act_on = false;
            ESP_LOGI(TAG, "activite : fin");
            show_ui();
            return;
        }
        const char *p = line + 5;
        if (!strncmp(p, "indet", 5)) {
            const char *lab = p + 6;
            act_begin(*lab ? lab : "EN COURS", true);
            return;
        }
        int v = atoi(p);
        const char *lab = strchr(p, ' ');
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (act_on) {
            act_indet = false;
            act_pct = v;
            if (lab && *lab) snprintf(act_label, sizeof(act_label), "%s", lab + 1);
            act_tick++;
            act_show(false);
        } else {
            act_begin(lab && *lab ? lab + 1 : "EN COURS", false);
            act_pct = v;
            act_show(true);
        }
        return;
    }
    if (!strncmp(line, "radio", 5)) {
        if (!strncmp(line, "radio start", 11)) {
            /* radio start [libelle] [duree en secondes]
               le libelle peut contenir des espaces : on prend le dernier mot
               comme duree s'il commence par un chiffre. */
            int secs = 0;
            char lab[20] = "RADIO";
            char tmp[64] = "";
            snprintf(tmp, sizeof(tmp), "%.60s", line + 11);
            char *p = tmp;
            while (*p == ' ') p++;
            char *dernier = NULL;
            for (char *q = p; *q; q++) if (*q == ' ' && q[1]) dernier = q;
            if (dernier && (isdigit((unsigned char)dernier[1]) || dernier[1] == '.')) {
                secs = (int)atof(dernier + 1);
                *dernier = 0;
            }
            while (*p == ' ') p++;
            if (*p) snprintf(lab, sizeof(lab), "%.19s", p);
            radio_total = secs > 0 ? secs * 64000 : 0;   /* octets stereo attendus */
            radio_done = 0;
            audio_playback_set_vol(85);
            radio_mode = true;
            act_begin(lab, false);
            ESP_LOGI(TAG, "flux audio recu : %s, %d s annonces", lab, secs);
            /* accuse de reception : sans lui, les premieres trames arrivent
               pendant qu'on lit encore des lignes et tout se desynchronise. */
            serial_write_bulk((const uint8_t *)"FLUX PRET\n", 10);
        } else {
            radio_mode = false;
            act_on = false;
            ESP_LOGI(TAG, "flux audio termine");
            show_ui();
        }
        return;
    }
    if (!strncmp(line, "repond ", 7)) {
        /* le PC a deja interroge DeepSeek : on affiche sa reponse et on sort de
           l'ecran « JE REFLECHIS » (la tache de reveil voit voix_etat changer) */
        snprintf(voix_reponse, sizeof(voix_reponse), "%.200s", line + 7);
        voix_etat = 3;
        ui_page = PAGE_MAITRE;
        show_ui();
        ESP_LOGI(TAG, "reponse affichee : %.60s", voix_reponse);
        return;
    }
    if (!strcmp(line, "wake")) {
        ESP_LOGI(TAG, "reveil simule (comme un appui long sur BOOT)");
        assistant_wake();
        return;
    }
    if (!strcmp(line, "getrec")) {
        /* renvoie la derniere prise en binaire : beaucoup plus rapide que
           de l'hexa par la console (11 ko/s contre plusieurs centaines). */
        if (rec_last_bytes <= 0) { ESP_LOGW(TAG, "aucune prise en memoire"); return; }
        esp_log_level_set("*", ESP_LOG_NONE);          /* pas de log au milieu du flux */
        char hdr[32];
        int hl = snprintf(hdr, sizeof(hdr), "GETRECB %d\n", rec_last_bytes);
        serial_write_bulk((const uint8_t *)hdr, hl);
        serial_write_bulk(rec_pcm, rec_last_bytes);
        serial_write_bulk((const uint8_t *)"\nGETRECB END\n", 13);
        esp_log_level_set("*", ESP_LOG_INFO);
        ESP_LOGI(TAG, "prise envoyee : %d octets", rec_last_bytes);
        return;
    }
    if (!strncmp(line, "pcm", 3)) {
        /* pcm begin / pcm <hexa> / pcm end : le PC pousse de l'audio a jouer */
        static int pcm_pos = 0;
        if (!strcmp(line, "pcm begin")) {
            pcm_pos = 0;
            pcm_xfer = true;
            ESP_LOGI(TAG, "pcm : debut de transfert");
            return;
        }
        if (!strcmp(line, "pcm end")) {
            pcm_xfer = false;
            audio_play_mono(pcm_pos / 2);
            ESP_LOGI(TAG, "haut-parleur : %d octets joues", pcm_pos);
            pcm_pos = 0;
            return;
        }
        if (line[3] == ' ') {
            const char *h = line + 4;
            while (*h && *(h + 1) && pcm_pos + 1 < AUDIO_MAX_FRAMES * 2) {
                int hi = 0, lo = 0;
                char a = *h++, b = *h++;
                hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
                lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
                if (hi < 0 || lo < 0) break;
                rec_pcm[pcm_pos++] = (uint8_t)((hi << 4) | lo);
            }
            return;
        }
        return;
    }
    if (!strncmp(line, "pet", 3)) {
        if (line[3] == 0) {
            ESP_LOGI(TAG, "stade=%s age=%u j sante=%u%% faim=%u bonheur=%u energie=%u proprete=%u caca=%u forme=%s gen=%u",
                     pet_stade_nom(pet_stade(&pet)), (unsigned)(pet.age_s / 86400),
                     pet_sante(&pet), (unsigned)(pet.faim / 100), (unsigned)(pet.bonheur / 100),
                     (unsigned)(pet.energie / 100), (unsigned)(pet.proprete / 100),
                     pet.caca, pet_forme_nom(pet.forme), pet.generation);
        } else if (!strncmp(line, "pet ", 4)) {
            const char *a = line + 4;
            pet_action_t ac = ACT_AUCUNE;
            if (!strcmp(a, "manger")) ac = ACT_MANGER;
            else if (!strcmp(a, "jouer")) ac = ACT_JOUER;
            else if (!strcmp(a, "calin")) ac = ACT_CALIN;
            else if (!strcmp(a, "nettoyer")) ac = ACT_NETTOYER;
            else if (!strcmp(a, "soigner")) ac = ACT_SOIGNER;
            else if (!strcmp(a, "dormir")) ac = ACT_DORMIR;
            else if (!strcmp(a, "reveiller")) ac = ACT_REVEILLER;
            else if (!strcmp(a, "neuf")) {   /* nouvel oeuf apres un depart */
                pet_nait(&pet, (uint16_t)(pet.generation + 1));
                pet_sauve();
                journal_ajoute("un nouvel oeuf est arrive");
                show_ui();
                return;
            }
            if (ac != ACT_AUCUNE) {
                journal_ajoute(pet_action(&pet, ac));
                pet_sauve();
                show_ui();
            }
        }
        return;
    }
    if (!strncmp(line, "radio", 5)) {
        /* radio play|stop|next|prev|station <n> : c'est le cerveau qui commande */
        if (!strncmp(line + 6, "play", 4))       radio_joue();
        else if (!strncmp(line + 6, "stop", 4))  radio_stop();
        else if (!strncmp(line + 6, "next", 4))  radio_change(1);
        else if (!strncmp(line + 6, "prev", 4))  radio_change(-1);
        else if (!strncmp(line + 6, "station", 7)) {
            int n = atoi(line + 14);
            if (n >= 0 && n < radios_n) { radio_sel = n; if (radio_etat) { radio_gen++; radio_etat = 1; } show_ui(); }
        } else if (radios_n > 0) {
            ESP_LOGI(TAG, "radios :");
            for (int i = 0; i < radios_n; i++) ESP_LOGI(TAG, "  %2d %s", i, radios_nom[i]);
        }
        return;
    }
    if (!strncmp(line, "vol", 3)) {
        if (line[3] == ' ') radio_vol_regle(atoi(line + 4), 1);
        else if (line[3] == '+') radio_vol_regle(radio_vol + 10, 1);
        else if (line[3] == '-') radio_vol_regle(radio_vol - 10, 1);
        else ESP_LOGI(TAG, "volume %d%%", radio_vol);
        show_ui();
        return;
    }
    if (!strcmp(line, "appli") || !strncmp(line, "appli ", 6)) {
        /* pratique pour verifier les ecrans : "appli N" (0..4), "appli" = suivant */
        if (line[5] == ' ') ui_page = atoi(line + 6) % PAGE_COUNT;
        else ui_page = (ui_page + 1) % PAGE_COUNT;
        ESP_LOGI(TAG, "appli %d", ui_page);
        show_ui();
        return;
    }
    if (!strcmp(line, "journal")) {
        for (int i = 0; i < journal_n; i++)
            ESP_LOGI(TAG, "journal %d/%d : %s", i + 1, journal_n,
                     journal_lignes[(journal_tete + 12 - journal_n + i) % 12]);
        return;
    }
    if (!strcmp(line, "ui") || !strncmp(line, "ui ", 3)) {
        if (line[2] == ' ') {
            int p = atoi(line + 3);
            if (p >= 0 && p < PAGE_COUNT) ui_page = p;
            else if (line[3] == 'f') ui_face = (ui_face + 1) % EXPR_N;
        }
        ui_read_sensors();
        if (ui_page == 99) { secu_refresh(); secu_sweep(); }
        show_ui();
        ESP_LOGI(TAG, "page=%d visage=%s temp=%.1f humi=%.0f wifi=%s shtc=%s",
                 ui_page, EXPR[ui_face].nom, (double)ui_temp, (double)ui_humi,
                 ui_wifi_ok ? "ok" : "ko", ui_shtc_ok ? "ok" : "ko");
        return;
    }
    if (!strcmp(line, "touches")) {
        touch_log = !touch_log;
        ESP_LOGI(TAG, "journal tactile %s", touch_log ? "active" : "coupe");
        return;
    }
    if (!strcmp(line, "fb")) {
        static char hex[260];
        /* courte attente : un dump legerement en cours de dessin vaut mieux
           qu'une console bloquee si un rafraichissement traine */
        if (ui_mux) xSemaphoreTake(ui_mux, pdMS_TO_TICKS(1500));
        for (int off = 0; off < FB_LEN; off += 120) {
            int n = (FB_LEN - off) < 120 ? (FB_LEN - off) : 120;
            for (int i = 0; i < n; i++) snprintf(hex + i * 2, 3, "%02x", fb[off + i]);
            hex[n * 2] = 0;
            ESP_LOGI(TAG, "FB %d %s", off, hex);
        }
        ESP_LOGI(TAG, "FB END");
        if (ui_mux) xSemaphoreGive(ui_mux);
        return;
    }
    if (!strcmp(line, "capteurs")) {
        for (int k = 0; k < 3; k++) {
            bool ok = ui_read_sensors();
            ESP_LOGI(TAG, "SHTC3 %s : carte %.1f C -> piece %.1f C (decalage %.1f), %.1f %%HR",
                     ok ? "ok" : "muet", (double)ui_temp_brut, (double)ui_temp,
                     (double)ui_temp_offset, (double)ui_humi);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        return;
    }
    if (!strncmp(line, "offset", 6)) {
        if (line[6] == ' ') {
            float v = (float)atof(line + 7);
            if (v > -30.0f && v < 30.0f) {
                ui_temp_offset = v;
                nvs_handle_t nh;
                if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
                    nvs_set_i32(nh, "toff10", (int32_t)lroundf(v * 10.0f));
                    nvs_commit(nh);
                    nvs_close(nh);
                }
            }
        }
        ui_read_sensors();
        ESP_LOGI(TAG, "decalage temperature = %.1f C (carte %.1f -> affiche %.1f)",
                 (double)ui_temp_offset, (double)ui_temp_brut, (double)ui_temp);
        return;
    }
    if (!strcmp(line, "i2cscan")) {
        int found = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (i2c_probe_addr(a) == 0) { ESP_LOGI(TAG, "  i2c 0x%02x present", a); found++; }
        }
        ESP_LOGI(TAG, "i2cscan : %d peripherique(s)", found);
        return;
    }
    if (!strcmp(line, "touchhunt")) {
        /* chercher le vrai pin de reset du tactile : pour chaque candidat,
           impulsions 0->1 puis scan complet du bus. */
        static const int cand[] = {7, 9, 4, 5, 21, 1, 2, 3, 14, 15, 45, 46, 8, 10, 11, 12, 13, 6, 42};
        ESP_LOGI(TAG, "chasse au reset tactile (%d candidats)...", (int)(sizeof(cand) / sizeof(cand[0])));
        for (unsigned k = 0; k < sizeof(cand) / sizeof(cand[0]); k++) {
            gpio_config_t io = {};
            io.pin_bit_mask = 1ULL << cand[k];
            io.mode = GPIO_MODE_OUTPUT;
            gpio_config(&io);
            gpio_set_level((gpio_num_t)cand[k], 1);
            vTaskDelay(pdMS_TO_TICKS(120));
            gpio_set_level((gpio_num_t)cand[k], 0);
            vTaskDelay(pdMS_TO_TICKS(120));
            gpio_set_level((gpio_num_t)cand[k], 1);
            vTaskDelay(pdMS_TO_TICKS(250));
            if (i2c_probe_addr(0x38) == 0) {
                ESP_LOGW(TAG, "TACTILE TROUVE : reset sur GPIO %d", cand[k]);
                return;
            }
        }
        ESP_LOGW(TAG, "tactile : aucun pin de reset candidat ne le reveille");
        return;
    }
    if (!strcmp(line, "railcycle")) {
        /* couper puis remettre tous les rails, attendre, rescanner */
        gpio_config_t io = {};
        io.pin_bit_mask = (1ULL << 6) | (1ULL << 42) | (1ULL << 17) | (1ULL << 7);
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        gpio_set_level(GPIO_NUM_6, 1);    /* EPD off */
        gpio_set_level(GPIO_NUM_42, 1);   /* audio off */
        gpio_set_level(GPIO_NUM_17, 0);   /* VBAT off */
        gpio_set_level(GPIO_NUM_7, 0);
        vTaskDelay(pdMS_TO_TICKS(1500));
        gpio_set_level(GPIO_NUM_7, 1);
        gpio_set_level(GPIO_NUM_17, 1);
        gpio_set_level(GPIO_NUM_42, 0);
        gpio_set_level(GPIO_NUM_6, 0);
        vTaskDelay(pdMS_TO_TICKS(1500));
        int found = 0;
        for (uint8_t a = 0x08; a < 0x78; a++)
            if (i2c_probe_addr(a) == 0) { ESP_LOGI(TAG, "  i2c 0x%02x present", a); found++; }
        ESP_LOGI(TAG, "apres cycle des rails : %d peripherique(s), 0x38 %s",
                 found, i2c_probe_addr(0x38) == 0 ? "PRESENT" : "absent");
        return;
    }
    if (!strcmp(line, "ft")) {
        gpio_config_t vo = {};
        vo.pin_bit_mask = 1ULL << 17;
        vo.mode = GPIO_MODE_OUTPUT;
        gpio_config(&vo);
        gpio_set_level(GPIO_NUM_17, 1);   /* VBAT_PWR */
        gpio_set_level(GPIO_NUM_7, 0); vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(GPIO_NUM_7, 1); vTaskDelay(pdMS_TO_TICKS(500));
        int pr = i2c_probe_addr(0x38);
        uint8_t id = 0, fw = 0, n = 0;
        esp_err_t e1 = i2c_read_buff(ft6336_handle, 0xA3, &id, 1);
        esp_err_t e2 = i2c_read_buff(ft6336_handle, 0xA8, &fw, 1);
        esp_err_t e3 = i2c_read_buff(ft6336_handle, 0x02, &n, 1);
        ESP_LOGI(TAG, "FT6336 probe=%d id=%02x(%s) fw=%02x(%s) points=%d(%s)",
                 pr, id, esp_err_to_name(e1), fw, esp_err_to_name(e2), n, esp_err_to_name(e3));
        return;
    }

    /* filet de securite : un fragment de transfert binaire (que des chiffres
       hexa) ou une ligne pleine d'octets de controle n'est pas une phrase pour
       DeepSeek. Sans ce filtre, le bruit audio lu comme du texte part en
       requete HTTP et le modele repond a des morceaux de PCM. */
    {
        int hl = (int)strlen(line), hexonly = hl > 0, ctrl = 0;
        for (int i = 0; i < hl; i++) {
            unsigned char c = (unsigned char)line[i];
            if (!isxdigit(c)) hexonly = 0;
            if (c < 0x20 || c == 0x7f) ctrl = 1;
        }
        if (pcm_xfer || (hexonly && hl >= 16) || ctrl) {
            ESP_LOGW(TAG, "ligne binaire ignoree (%d octets)", hl);
            return;
        }
        {
            int i = 0;                       /* ligne vide ou espaces : rien a dire */
            while (line[i] == ' ' || line[i] == '\t') i++;
            if (!line[i]) return;
        }
    }

    char dit[900], emo[32];
    ESP_LOGI(TAG, "reflexion...");
    if (ds_chat(line, dit, sizeof(dit), emo, sizeof(emo)) == ESP_OK) {
        int e = emo_by_name(emo);
        if (e < 0) e = art_expr_de_famille(8, 0);
        current_emotion = EXPR[e].famille;
        ESP_LOGI(TAG, "Batmino [%s/%s] : %s", FAMILLES[current_emotion], EXPR[e].nom, dit);
        show_face(e);
    } else {
        ESP_LOGE(TAG, "DeepSeek muet (reseau ?)");
        current_emotion = 13;
        show_face(13);
    }
}

/* ---- banc d'essai ecran, pilote par la console USB : !<commande> ----------
   !busy            niveau de la broche BUSY
   !off | !on       rail d'alimentation de la dalle
   !init            reinitialisation complete
   !clear           init + effacement (chronometre)
   !part [n]        n rafraichissements partiels de l'image courante
   !full | !clean   rafraichissement complet (simple / avec LUT pleine)
   !st              etat du driver                                                        */
static void console_epd(const char *c)
{
    /* !wifi                : etat
       !wifi <ssid> <pass>  : bascule a chaud (marche meme sans reseau) */
    if (!strncmp(c, "wifi", 4)) {
        const char *a = c + 4;
        while (*a == ' ') a++;
        if (!*a) {
            ESP_LOGI(TAG, "banc : reseau '%s' (enregistre : %d)", wifi_ssid_i(0), wifi_rt);
            return;
        }
        char s[33] = "", p[65] = "";
        const char *e = strchr(a, ' ');
        size_t n = e ? (size_t)(e - a) : strlen(a);
        if (n > 32) n = 32;
        memcpy(s, a, n);
        s[n] = 0;
        if (e) { while (*e == ' ') e++; snprintf(p, sizeof(p), "%s", e); }
        ESP_LOGI(TAG, "banc : bascule sur '%s' (mot de passe de %d caracteres)", s, (int)strlen(p));
        wifi_bascule(s, p);
        return;
    }
    if (!strcmp(c, "cpu")) {          /* qui consomme le CPU ? */
        char *b = (char *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
        if (b) { vTaskGetRunTimeStats(b); ESP_LOGI(TAG, "taches:\n%s", b); free(b); }
        else ESP_LOGW(TAG, "banc : pas de tampon pour les stats");
        {
            uint32_t total = 0;
            UBaseType_t n = uxTaskGetSystemState(sys_ts, 48, &total);
            uint32_t somme = 0, somme_idle = 0;
            for (UBaseType_t i = 0; i < n; i++) {
                const char *nm = sys_ts[i].pcTaskName ? sys_ts[i].pcTaskName : "";
                if (!strncmp(nm, "IDLE", 4)) somme_idle += (uint32_t)sys_ts[i].ulRunTimeCounter;
                else somme += (uint32_t)sys_ts[i].ulRunTimeCounter;
            }
            ESP_LOGI(TAG, "banc : cpu=%d%% n=%d total=%u somme=%u idle=%u",
                     sys_cpu, (int)n, (unsigned)total, (unsigned)somme, (unsigned)somme_idle);
        }
        return;
    }
    if (!epd) { ESP_LOGW(TAG, "banc : pas de dalle"); return; }
    if (!strcmp(c, "busy") || !strcmp(c, "st")) {
        ESP_LOGI(TAG, "banc : BUSY=%d panne=%d busy_ko=%d refus=%d",
                 gpio_get_level(EPD_BUSY_PIN), (int)epd->panne, epd->busy_ko, refresh_count);
        if (!strcmp(c, "busy")) return;
    }
    if (!strcmp(c, "off")) { gpio_set_level(EPD_PWR_PIN, 1); ESP_LOGI(TAG, "banc : rail coupe"); return; }
    if (!strcmp(c, "on"))  { gpio_set_level(EPD_PWR_PIN, 0); ESP_LOGI(TAG, "banc : rail remis"); return; }

    if (xSemaphoreTake(epd_mux, pdMS_TO_TICKS(20000)) != pdTRUE) {
        ESP_LOGW(TAG, "banc : mutex occupe");
        return;
    }
    int64_t t0 = esp_timer_get_time();
    if (!strcmp(c, "init")) {
        epd->EPD_Panne_Reset();
        epd->EPD_Init();
        ESP_LOGI(TAG, "banc : init -> panne=%d", (int)epd->panne);
    } else if (!strcmp(c, "clear")) {
        epd->EPD_Panne_Reset();
        epd->EPD_Init();
        epd->EPD_Clear();
        epd->EPD_Display();
        ESP_LOGI(TAG, "banc : clear -> panne=%d", (int)epd->panne);
    } else if (!strncmp(c, "part", 4)) {
        int n = atoi(c + 4);
        if (n <= 0) n = 1;
        for (int i = 0; i < n; i++) {
            int64_t a = esp_timer_get_time();
            epd->EPD_ShowPart(fb, FB_LEN);
            ESP_LOGI(TAG, "banc : partiel %d/%d en %d ms (panne=%d)", i + 1, n,
                     (int)((esp_timer_get_time() - a) / 1000), (int)epd->panne);
            if (epd->panne) break;
        }
    } else if (!strcmp(c, "full")) {
        epd->EPD_ShowFull(fb, FB_LEN);
        ESP_LOGI(TAG, "banc : full -> panne=%d", (int)epd->panne);
    } else if (!strcmp(c, "clean")) {
        epd->EPD_ShowFullClean(fb, FB_LEN);
        ESP_LOGI(TAG, "banc : clean -> panne=%d", (int)epd->panne);
    } else {
        ESP_LOGI(TAG, "banc : !busy !off !on !init !clear !part[n] !full !clean !st");
    }
    ESP_LOGI(TAG, "banc : %d ms au total", (int)((esp_timer_get_time() - t0) / 1000));
    xSemaphoreGive(epd_mux);
}

static void serial_task(void *arg)
{
    static char line[2048];
    int len = 0;
    static uint8_t chunk[256];
    static uint8_t radio_buf[4096];
    for (;;) {
        /* mode radio : le PC pousse du PCM stereo par trames [longueur][donnees] */
        if (radio_mode) {
            uint8_t hdr[4];
            if (usb_serial_jtag_read_bytes(hdr, 4, pdMS_TO_TICKS(2000)) != 4) {
                /* plus rien depuis un moment : le PC a lache, on rend la main
                   a la console (c'est la reprise apres coupure). */
                if (++radio_idle >= 3) {
                    radio_mode = false;
                    radio_idle = 0;
                    act_on = false;
                    ESP_LOGI(TAG, "flux audio coupe : retour a la console");
                    show_ui();
                }
                continue;
            }
            radio_idle = 0;
            {
                uint32_t n = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                             ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
                if (n == 0xFFFFFFFFu) {
                    radio_mode = false;
                    act_on = false;
                    ESP_LOGI(TAG, "radio : fin du flux");
                    show_ui();
                } else if (n > 0 && n <= sizeof(radio_buf)) {
                    int got = 0;
                    while (got < (int)n) {
                        int r = usb_serial_jtag_read_bytes(radio_buf + got, n - got, pdMS_TO_TICKS(2000));
                        if (r <= 0) break;
                        got += r;
                    }
                    if (got == (int)n) {
                        audio_playback_write(radio_buf, n);
                        radio_done += n;
                        if (!act_on) act_begin("RADIO", false);
                        act_indet = false;
                        if (radio_total > 0) {
                            act_pct = (int)((int64_t)radio_done * 100 / radio_total);
                            if (act_pct > 100) act_pct = 100;
                        } else {
                            int pk = 0;
                            for (uint32_t i = 0; i + 1 < n; i += 4) {
                                int16_t v = (int16_t)(radio_buf[i] | (radio_buf[i + 1] << 8));
                                int a = v < 0 ? -v : v;
                                if (a > pk) pk = a;
                            }
                            act_pct = pk * 100 / 12000;
                            if (act_pct > 100) act_pct = 100;
                        }
                        act_tick++;
                    }
                }
            }
            continue;
        }
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(300));
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = 0;
                if (len) {
                    /* pendant un transfert PCM on n'echo pas : la console TX
                       mange le RX et fait perdre des octets */
                    if (!pcm_xfer && len < 400) ESP_LOGI(TAG, "> %s", line);
                    else if (!pcm_xfer) ESP_LOGI(TAG, "> %d octets recus", len);
                    if (line[0] == '!') console_epd(line + 1);   /* banc d'essai ecran */
                    else mino_handle_line(line);
                }
                len = 0;
                continue;
            }
            if (len < (int)sizeof(line) - 1) line[len++] = (char)c;
            else len = 0;      /* ligne trop longue : on jette */
        }
    }
}

/* ------------------------------------------------------------------ */
/* API HTTP                                                            */
/* ------------------------------------------------------------------ */
static void url_decode(char *dst, const char *src, size_t sz)
{
    size_t o = 0;
    while (*src && o + 1 < sz) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char h[3] = { src[1], src[2], 0 };
            dst[o++] = (char)strtol(h, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' '; src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = 0;
}

static esp_err_t qval(httpd_req_t *req, const char *key, char *out, size_t sz)
{
    char q[1600];
    out[0] = 0;
    if (httpd_req_get_url_query_len(req) <= 0) return ESP_ERR_NOT_FOUND;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return ESP_ERR_NOT_FOUND;
    char raw[1400];
    if (httpd_query_key_value(q, key, raw, sizeof(raw)) != ESP_OK) return ESP_ERR_NOT_FOUND;
    url_decode(out, raw, sz);
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (s) { httpd_resp_sendstr(req, s); free(s); }
    else httpd_resp_send_500(req);
    cJSON_Delete(root);
    return ESP_OK;
}

static bool busy = false;

static esp_err_t h_face(httpd_req_t *req)
{
    char v[64];
    cJSON *r = cJSON_CreateObject();
    int e = -1;
    if (qval(req, "e", v, sizeof(v)) == ESP_OK) {
        char *endp = NULL;
        long n = strtol(v, &endp, 10);
        if (endp && *endp == 0 && n >= 0 && n < EXPR_N) e = (int)n;
        else e = emo_by_name(v);
    }
    if (e < 0) {
        cJSON_AddStringToObject(r, "error", "emotion inconnue");
        cJSON_AddItemToObject(r, "familles", cJSON_CreateStringArray(FAMILLES, FAMILLE_N));
        return send_json(req, r);
    }
    current_emotion = EXPR[e].famille;
    show_face(e);
    cJSON_AddStringToObject(r, "ok", EXPR[e].nom);
    cJSON_AddStringToObject(r, "famille", FAMILLES[current_emotion]);
    return send_json(req, r);
}

static esp_err_t h_say(httpd_req_t *req)
{
    char q[1200];
    if (qval(req, "q", q, sizeof(q)) != ESP_OK || !q[0]) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", "parametre q manquant");
        return send_json(req, r);
    }
    if (busy) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "error", "deja en train de reflechir");
        httpd_resp_set_status(req, "429 Too Many Requests");
        return send_json(req, r);
    }
    busy = true;
    int64_t t0 = esp_timer_get_time();
    char dit[900], emo[32];
    esp_err_t err = ds_chat(q, dit, sizeof(dit), emo, sizeof(emo));
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    busy = false;

    cJSON *r = cJSON_CreateObject();
    if (err == ESP_OK) {
        int e = emo_by_name(emo);
        if (e < 0) e = art_expr_de_famille(8, 0);
        current_emotion = EXPR[e].famille;
        bat5_du_pet = famille_vers_bat5(current_emotion);
        snprintf(voix_reponse, sizeof(voix_reponse), "%.200s", dit);
        voix_etat = 3;                        /* je reponds, en grand */
        ui_page = PAGE_MAITRE;
        show_ui();
        cJSON_AddStringToObject(r, "emotion", FAMILLES[current_emotion]);
        cJSON_AddStringToObject(r, "visage", EXPR[e].nom);
        cJSON_AddStringToObject(r, "dit", dit);
        cJSON_AddNumberToObject(r, "ms", (double)ms);
        show_face(e);
        journal_ajoute(dit);
        ESP_LOGI(TAG, "Q: %s", q);
        ESP_LOGI(TAG, "A: [%s] %s", EXPR[e].nom, dit);
        voix_etat = 3;
        show_ui();
        tts_parle(dit);                  /* il le dit a voix haute */
        voix_etat = 0;                   /* et on revient au tableau de bord */
        ui_page = PAGE_MAITRE;
        show_ui();
    } else {
        cJSON_AddStringToObject(r, "error", "DeepSeek injoignable");
        show_face(emo_by_name("confusion"));
    }
    return send_json(req, r);
}

static esp_err_t h_note(httpd_req_t *req)
{
    char t[512];
    cJSON *r = cJSON_CreateObject();
    if (qval(req, "t", t, sizeof(t)) == ESP_OK && t[0]) {
        mem_add(t);
        cJSON_AddStringToObject(r, "ok", "note enregistree");
        cJSON_AddStringToObject(r, "note", t);
    } else {
        cJSON_AddStringToObject(r, "error", "parametre t manquant");
    }
    return send_json(req, r);
}

static esp_err_t h_radio(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    char q[128] = {0}, a[32] = {0}, n[8] = {0};
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        httpd_query_key_value(q, "a", a, sizeof(a));
        httpd_query_key_value(q, "n", n, sizeof(n));
    }
    if (n[0]) radio_sel = atoi(n) % (radios_n > 0 ? radios_n : 1);
    {
        char v[8] = {0};
        httpd_query_key_value(q, "v", v, sizeof(v));
        if (v[0]) radio_vol_regle(atoi(v), 1);
    }
    {
        char act[8] = {0};
        httpd_query_key_value(q, "actif", act, sizeof(act));
        if (act[0]) {
            radio_actif = (act[0] == '1');
            nvs_handle_t nh;
            if (nvs_open("mino", NVS_READWRITE, &nh) == ESP_OK) {
                nvs_set_i32(nh, "radio", radio_actif ? 1 : 0);
                nvs_commit(nh); nvs_close(nh);
            }
            if (!radio_actif) radio_stop();
            ESP_LOGW(TAG, "module radio %s", radio_actif ? "ACTIVE" : "desactive");
        }
    }
    if (!radio_actif) {
        cJSON_AddBoolToObject(r, "actif", false);
        cJSON_AddStringToObject(r, "etat", "desactivee");
        cJSON_AddStringToObject(r, "aide", "activer : /radio?actif=1");
        return send_json(req, r);
    }
    if (!strcmp(a, "play"))       radio_joue();
    else if (!strcmp(a, "stop"))  radio_stop();
    else if (!strcmp(a, "next"))  radio_change(1);
    else if (!strcmp(a, "prev"))  radio_change(-1);
    else if (n[0])                { radio_sel = atoi(n) % (radios_n > 0 ? radios_n : 1); show_ui(); }
    cJSON_AddNumberToObject(r, "station", radio_sel);
    cJSON_AddNumberToObject(r, "stations", radios_n);
    cJSON_AddStringToObject(r, "etat", radio_etat == 2 ? "en lecture" : (radio_etat == 1 ? "je charge" : "a l arret"));
    if (radios_n > 0) {
        cJSON_AddStringToObject(r, "nom", radios_nom[radio_sel]);
        cJSON_AddStringToObject(r, "url", radios_url[radio_sel]);
    }
    cJSON_AddNumberToObject(r, "volume", radio_vol);
    cJSON_AddNumberToObject(r, "fichier_ok", radios_fichier_ok);
    cJSON_AddNumberToObject(r, "lu_sur_carte", radios_dernier_lu);
    {
        cJSON *lst = cJSON_AddArrayToObject(r, "liste");
        for (int i = 0; i < radios_n; i++) cJSON_AddItemToArray(lst, cJSON_CreateString(radios_nom[i]));
    }
    cJSON_AddNumberToObject(r, "octets_recus", (double)radio_done);
    return send_json(req, r);
}

static esp_err_t h_pet(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    char q[64] = {0}, a[32] = {0};
    pet_action_t ac = ACT_AUCUNE;
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, q, sizeof(q));
        if (httpd_query_key_value(q, "a", a, sizeof(a)) == ESP_OK) {
            if (!strcmp(a, "manger")) ac = ACT_MANGER;
            else if (!strcmp(a, "jouer")) ac = ACT_JOUER;
            else if (!strcmp(a, "calin")) ac = ACT_CALIN;
            else if (!strcmp(a, "nettoyer")) ac = ACT_NETTOYER;
            else if (!strcmp(a, "soigner")) ac = ACT_SOIGNER;
            else if (!strcmp(a, "dormir")) ac = ACT_DORMIR;
            else if (!strcmp(a, "reveiller")) ac = ACT_REVEILLER;
        }
    }
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    if (ac != ACT_AUCUNE) {
        char ligne[64];
        snprintf(ligne, sizeof(ligne), "%s", pet_action(&pet, ac));
        journal_ajoute(ligne);
        cJSON_AddStringToObject(r, "fait", ligne);
        pet_sauve();
    }
    cJSON_AddStringToObject(r, "stade", pet_stade_nom(pet_stade(&pet)));
    cJSON_AddStringToObject(r, "forme", pet_forme_nom(pet.forme));
    cJSON_AddNumberToObject(r, "sante", (double)pet_sante(&pet));
    cJSON_AddNumberToObject(r, "faim", (double)(pet.faim / 100));
    cJSON_AddNumberToObject(r, "bonheur", (double)(pet.bonheur / 100));
    cJSON_AddNumberToObject(r, "energie", (double)(pet.energie / 100));
    cJSON_AddNumberToObject(r, "proprete", (double)(pet.proprete / 100));
    cJSON_AddNumberToObject(r, "age_s", (double)pet.age_s);
    xSemaphoreGive(pet_mux);
    show_ui();
    return send_json(req, r);
}

static esp_err_t h_reglages(httpd_req_t *req);   /* defini plus bas */

static esp_err_t h_status(httpd_req_t *req)
{
    /* un navigateur demande du HTML : on lui donne la page de reglages, pour
       que http://192.168.8.210 fonctionne directement (curl recoit le JSON) */
    {
        char acc[80] = "";
        if (httpd_req_get_hdr_value_str(req, "Accept", acc, sizeof(acc)) == ESP_OK &&
            strstr(acc, "text/html"))
            return h_reglages(req);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "qui", "Mino");
    cJSON_AddStringToObject(r, "emotion", FAMILLES[current_emotion]);
    cJSON_AddStringToObject(r, "stade", pet_stade_nom(pet_stade(&pet)));
    cJSON_AddStringToObject(r, "forme", pet_forme_nom(pet.forme));
    cJSON_AddNumberToObject(r, "age_s", (double)pet.age_s);
    cJSON_AddNumberToObject(r, "faim", (double)(pet.faim / 100));
    cJSON_AddNumberToObject(r, "bonheur", (double)(pet.bonheur / 100));
    cJSON_AddNumberToObject(r, "energie", (double)(pet.energie / 100));
    cJSON_AddNumberToObject(r, "proprete", (double)(pet.proprete / 100));
    cJSON_AddNumberToObject(r, "sante", (double)pet_sante(&pet));
    cJSON_AddNumberToObject(r, "cpu_mhz", (double)(esp_clk_cpu_freq() / 1000000));
    cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(r, "heap_libre", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "historique_tours", (double)hist.n);
    cJSON_AddNumberToObject(r, "heap_interne_max", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "heap_dma", (double)heap_caps_get_free_size(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(r, "reset", (double)esp_reset_reason());
    cJSON_AddStringToObject(r, "wifi_ssid", wifi_ssid_i(0));
    cJSON_AddBoolToObject(r, "wifi_enregistre", wifi_rt ? 1 : 0);
    cJSON_AddNumberToObject(r, "heap_dma_max", (double)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(r, "heap_interne_max",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(r, "heap_dma_max",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(r, "psram_libre", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddItemToObject(r, "familles", cJSON_CreateStringArray(FAMILLES, FAMILLE_N));
    cJSON_AddItemToObject(r, "memoire", cJSON_Duplicate(cJSON_GetObjectItem(mem_obj, "notes"), 1));
    cJSON_AddNumberToObject(r, "voix_etat", (double)voix_etat);
    cJSON_AddStringToObject(r, "voix_dit", voix_reponse);
    cJSON_AddStringToObject(r, "voix_entendu", voix_entendu_txt);
    cJSON_AddBoolToObject(r, "cle_ia", (bool)(ia_cle[0] != 0 || GROQ_KEY[0] != 0));
    cJSON_AddNumberToObject(r, "mic_db", (double)mic_db);
    cJSON_AddNumberToObject(r, "mic_crete", (double)mic_crete);
    cJSON_AddNumberToObject(r, "minuteur_s", (double)minuteur_restant_s());
    cJSON_AddNumberToObject(r, "mic_bruit", (double)mic_bruit);
    cJSON_AddNumberToObject(r, "mic_seuil", (double)mic_seuil);
    cJSON_AddNumberToObject(r, "conso_mw", (double)conso_mw());
    cJSON_AddNumberToObject(r, "epd_busy_ko", epd ? epd->busy_ko : -1);
    cJSON_AddNumberToObject(r, "cpu_pct", sys_cpu);
    cJSON_AddNumberToObject(r, "cpu_pct10", sys_cpu10);
    {
        /* valeur en cache : /state ne doit pas rallumer l'ADC de la page
           systeme, sinon la mesure garde la ressource allouee en permanence */
        cJSON_AddNumberToObject(r, "bat_mv", sys_bat_mv_cache);
        cJSON_AddNumberToObject(r, "bat_pct", sys_bat_pct_cache);
    }
    {
        uint64_t tot = 0, lib = 0;
        cJSON_AddNumberToObject(r, "net_rx_s", sys_rx_s);
        cJSON_AddNumberToObject(r, "net_tx_s", sys_tx_s);
        if (sd_ok && esp_vfs_fat_info("/sdcard", &tot, &lib) == ESP_OK)
            cJSON_AddNumberToObject(r, "sd_pct", tot ? (double)((tot - lib) * 100.0 / tot) : 0);
    }
    cJSON_AddNumberToObject(r, "bouche", (double)voix_bouche);
    cJSON_AddNumberToObject(r, "compagnon_x", (double)kirby_x);
    cJSON_AddNumberToObject(r, "compagnon_y", (double)kirby_y);
    cJSON_AddNumberToObject(r, "stt_http", (double)stt_http);
    cJSON_AddNumberToObject(r, "stt_ecrits", (double)stt_ecrits);
    cJSON_AddNumberToObject(r, "stt_total", (double)stt_total);
    cJSON_AddStringToObject(r, "stt_detail", stt_detail);
    cJSON_AddStringToObject(r, "ia_base", ia_base);
    cJSON_AddStringToObject(r, "ia_mod", ia_mod);
    cJSON_AddStringToObject(r, "langue", langue);
    cJSON_AddNumberToObject(r, "chrono_stt_ms", (double)chrono_stt_ms);
    cJSON_AddNumberToObject(r, "chrono_llm_ms", (double)chrono_llm_ms);
    cJSON_AddNumberToObject(r, "chrono_tts_ms", (double)chrono_tts_ms);
    cJSON_AddNumberToObject(r, "radio_etat", (double)radio_etat);
    cJSON_AddStringToObject(r, "radio_station", radios_n > 0 ? radios_nom[radio_sel] : "");
    cJSON_AddStringToObject(r, "page", ui_page < PAGE_COUNT ? PAGE_NOM[ui_page] : "?");
    {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char hb[80];
        snprintf(hb, sizeof(hb), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        cJSON_AddStringToObject(r, "heure", hb);
    }
    cJSON_AddNumberToObject(r, "meteo_c", meteo_ok ? (double)meteo_temp : -999.0);
    return send_json(req, r);
}

static esp_err_t h_soul(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        /* Remplace sa personnalite a chaud : ecrit sur la carte SD et en RAM. */
        int n = req->content_len;
        if (n <= 0 || n > 8000) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "taille du soul");
            return ESP_FAIL;
        }
        char *buf = (char *)malloc(n + 1);
        if (!buf) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoire");
            return ESP_FAIL;
        }
        int rd = httpd_req_recv(req, buf, n);
        if (rd <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lecture");
            return ESP_FAIL;
        }
        buf[rd] = 0;
        if (soul_txt) free(soul_txt);
        soul_txt = buf;
        if (sd_ok) {
            FILE *f = fopen(SD_SOUL, "w");
            if (f) { fputs(buf, f); fclose(f); }
        }
        ESP_LOGI(TAG, "SOUL remplace : %d octets", rd);
        httpd_resp_sendstr(req, "soul enregistre");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, soul_txt ? soul_txt : "");
    return ESP_OK;
}


/* ------------------------------------------------------------------ */
/* API des reglages : /wscan, /wifi, /reglages (page pour le telephone) */
/* ------------------------------------------------------------------ */
static void url_decode(char *d)
{
    char *s = d, *o = d;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *o++ = ' ';
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static esp_err_t h_wscan(httpd_req_t *req)
{
    char q[160] = "";
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char go[8];
        if (httpd_query_key_value(q, "go", go, sizeof(go)) == ESP_OK && go[0] == '1')
            g_wscan_demande = true;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "encours", (g_wscan_encours || g_wscan_demande) ? 1 : 0);
    cJSON_AddStringToObject(o, "actuel", wifi_ssid_i(0));
    cJSON *ap = cJSON_AddArrayToObject(o, "ap");
    for (int i = 0; i < g_wscan_n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "s", g_wscan[i].ssid);
        cJSON_AddNumberToObject(e, "r", g_wscan[i].rssi);
        cJSON_AddNumberToObject(e, "a", g_wscan[i].auth);
        cJSON_AddItemToArray(ap, e);
    }
    char *js = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js ? js : "{}");
    free(js);
    cJSON_Delete(o);
    return ESP_OK;
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    char q[400] = "";
    char ssid[80] = "", pass[120] = "";
    if (httpd_req_get_url_query_len(req) > 0)
        httpd_req_get_url_query_str(req, q, sizeof(q));
    if (!q[0] || httpd_query_key_value(q, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"ssid manquant\"}");
        return ESP_OK;
    }
    if (httpd_query_key_value(q, "pass", pass, sizeof(pass)) != ESP_OK) pass[0] = 0;
    url_decode(ssid);
    url_decode(pass);
    if (!ssid[0]) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"ssid vide\"}");
        return ESP_OK;
    }
    /* un WPA2 avec 2 caracteres ne peut pas marcher : on refuse au lieu de
       laisser le robot se couper du reseau (c'est arrive) */
    {
        int auth = -1;
        for (int i = 0; i < g_wscan_n; i++)
            if (!strcmp(g_wscan[i].ssid, ssid)) { auth = g_wscan[i].auth; break; }
        size_t lp = strlen(pass);
        if (auth == 0 && lp == 0) {
            /* reseau ouvert : rien a faire */
        } else if (lp < 8) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"mot de passe trop court (8 caracteres mini)\"}");
            return ESP_OK;
        }
        if (strlen(ssid) > 32) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"nom de reseau trop long\"}");
            return ESP_OK;
        }
    }
    wifi_bascule(ssid, pass);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* la page de reglages, servie par le robot : on s'en sert depuis le telephone */
static const char PAGE_REGLAGES[] =
"<!doctype html><html lang=fr><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>MINO</title><style>"
"body{font:16px system-ui;margin:0;padding:16px;background:#111;color:#eee}"
"h1{font-size:20px;margin:0 0 4px}p{color:#999;margin:4px 0 16px}"
".ap{display:flex;justify-content:space-between;padding:12px;margin:6px 0;"
"background:#1e1e1e;border-radius:10px}.ap:active{background:#333}"
".r{color:#6c6}.w{color:#eb4}b{color:#6af}"
"input,button{font:16px system-ui;padding:10px;border-radius:8px;border:0;width:100%;box-sizing:border-box}"
"button{background:#2d7;color:#000;font-weight:600;margin-top:8px}"
"#msg{color:#eb4;margin:8px 0}</style>"
"<h1>MINO</h1><p id=cur>...</p>"
"<div id=liste></div><div id=msg></div>"
"<input id=ssid placeholder='nom du reseau (reseau cache)'>"
"<input id=pass type=password placeholder='mot de passe'>"
"<button onclick='envoie()'>Connecter</button>"
"<script>"
"async function scan(){"
"let r=await(await fetch('/wscan?go=1')).json();"
"if(r.encours){setTimeout(scan,1500);return}"
"document.getElementById('cur').textContent='reseau actuel : '+(r.actuel||'aucun');"
"let l=document.getElementById('liste');l.innerHTML='';"
"r.ap.forEach(a=>{let d=document.createElement('div');d.className='ap';"
"let b=a.r>-55?'r':(a.r>-70?'w':'');"
"d.innerHTML='<span>'+a.s+(a.a?'':' (ouvert)')+'</span><span class='+b+'>'+a.r+' dBm</span>';"
"d.onclick=()=>{document.getElementById('ssid').value=a.s;"
"if(a.a){let p=prompt('mot de passe pour '+a.s);if(p===null)return;"
"fetch('/wifi?ssid='+encodeURIComponent(a.s)+'&pass='+encodeURIComponent(p))"
".then(x=>x.json()).then(j=>document.getElementById('msg').textContent=j.ok?'connecte, redemarrage du wifi...':'erreur : '+(j.err||'inconnue'));}"
"else fetch('/wifi?ssid='+encodeURIComponent(a.s)+'&pass=')"
".then(()=>document.getElementById('msg').textContent='connecte');};"
"l.appendChild(d)})}"
"function envoie(){let s=document.getElementById('ssid').value,"
"p=document.getElementById('pass').value;"
"fetch('/wifi?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))"
".then(x=>x.json()).then(j=>document.getElementById('msg').textContent=j.ok?'enregistre, le robot se reconnecte':'erreur : '+(j.err||'inconnue'))}"
"scan();</script>";

static esp_err_t h_reglages(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE_REGLAGES, sizeof(PAGE_REGLAGES) - 1);
    return ESP_OK;
}

static void api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 40;   /* 28 ne suffisait plus : les derniers URI
                                    (/bande, /ecran) n'etaient pas enregistres */
    cfg.stack_size = 12288;   /* h_say fait du TLS : 8 Ko ne suffisaient pas */
    cfg.max_open_sockets = 2; /* 7 sockets par defaut = 7 piles de 16 Ko (112 Ko
                                 de RAM interne) ; on n'a jamais plus de 2 clients */
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "serveur HTTP KO");
        return;
    }
    httpd_uri_t u;
    memset(&u, 0, sizeof(u));
    u.method = HTTP_GET;
    u.handler = h_status; u.uri = "/";    httpd_register_uri_handler(srv, &u);
    u.uri = "/state";                     httpd_register_uri_handler(srv, &u);
    u.handler = h_face; u.uri = "/face";  httpd_register_uri_handler(srv, &u);
    u.handler = h_say;  u.uri = "/say";   httpd_register_uri_handler(srv, &u);
    u.handler = h_note; u.uri = "/note";  httpd_register_uri_handler(srv, &u);
    u.handler = h_soul; u.uri = "/soul";  httpd_register_uri_handler(srv, &u);
    u.method = HTTP_POST;                 httpd_register_uri_handler(srv, &u);
    u.method = HTTP_GET;
    u.handler = h_rec;  u.uri = "/rec";   httpd_register_uri_handler(srv, &u);
    u.handler = h_pet;  u.uri = "/pet";  httpd_register_uri_handler(srv, &u);
    u.handler = h_radio; u.uri = "/radio"; httpd_register_uri_handler(srv, &u);
    u.handler = h_sd_list; u.uri = "/sd"; httpd_register_uri_handler(srv, &u);
    u.handler = h_sd_format; u.uri = "/sdformat"; httpd_register_uri_handler(srv, &u);
    u.handler = h_swap; u.uri = "/swap"; httpd_register_uri_handler(srv, &u);
    u.handler = h_part; u.uri = "/part"; httpd_register_uri_handler(srv, &u);
    u.handler = h_cle;  u.uri = "/cle";   httpd_register_uri_handler(srv, &u);
    u.handler = h_mic;  u.uri = "/mic";   httpd_register_uri_handler(srv, &u);
    u.handler = h_tts;  u.uri = "/tts";   httpd_register_uri_handler(srv, &u);
    u.handler = h_modele; u.uri = "/modele"; httpd_register_uri_handler(srv, &u);
    u.handler = h_langue; u.uri = "/langue"; httpd_register_uri_handler(srv, &u);
    u.handler = h_infos;  u.uri = "/infos";  httpd_register_uri_handler(srv, &u);
    u.handler = h_wscan;  u.uri = "/wscan";   httpd_register_uri_handler(srv, &u);
    u.handler = h_wifi;   u.uri = "/wifi";    httpd_register_uri_handler(srv, &u);
    u.handler = h_reglages; u.uri = "/reglages"; httpd_register_uri_handler(srv, &u);
    u.handler = h_minuteur; u.uri = "/minuteur"; httpd_register_uri_handler(srv, &u);
    u.handler = h_timer; u.uri = "/timer"; httpd_register_uri_handler(srv, &u);
    u.handler = h_bouton; u.uri = "/bouton"; httpd_register_uri_handler(srv, &u);
    u.handler = h_parle; u.uri = "/parle"; httpd_register_uri_handler(srv, &u);
    u.handler = h_dit;   u.uri = "/dit";   httpd_register_uri_handler(srv, &u);
    u.handler = h_ecran; u.uri = "/ecran"; httpd_register_uri_handler(srv, &u);
    u.handler = h_fichier; u.uri = "/fichier"; httpd_register_uri_handler(srv, &u);
    u.method = HTTP_POST;
    u.handler = h_play; u.uri = "/play";  httpd_register_uri_handler(srv, &u);
    u.handler = h_fichier; u.uri = "/fichier"; httpd_register_uri_handler(srv, &u);
    ESP_LOGI(TAG, "API : /state /face?e= /say?q= /note?t= /soul /rec?ms= /play(POST) /cle?k= /sd /sdformat /fichier(GET,POST)");
}

/* ------------------------------------------------------------------ */
static board_power_bsp_t board_div(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);

extern "C" void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    mem_mux = xSemaphoreCreateMutex();
    epd_mux = xSemaphoreCreateMutex();
    i2c_mux = xSemaphoreCreateMutex();
    ping_mux = xSemaphoreCreateMutex();

    heure_charge();     /* l'heure d'avant tout de suite : plus de date fausse au demarrage */
    timers_charge();    /* restaure les timers/alarmes */

    serial_start();     /* la console USB remplace le log par defaut */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);   /* pas de flood si un capteur manque */
    esp_log_level_set("i2c.common", ESP_LOG_NONE);
    {
        nvs_handle_t nh;
        int32_t v = 0;
        if (nvs_open("mino", NVS_READONLY, &nh) == ESP_OK) {
            if (nvs_get_i32(nh, "toff10", &v) == ESP_OK) ui_temp_offset = (float)v / 10.0f;
            nvs_close(nh);
        }
    }
    ESP_LOGI(TAG, "Mino demarre");

    storage_init();

    /* ecran + capteurs : TOUS les rails d'alimentation avant l'I2C,
       sinon le controleur tactile (0x38) ne repond jamais sur le bus. */
    board_div.POWEER_EPD_ON();
    board_div.POWEER_Audio_ON();
    board_div.VBAT_POWER_ON();
    vTaskDelay(pdMS_TO_TICKS(150));
    i2c_master_Init();
    touch_reset();
    {
        uint8_t n = 0;
        if (i2c_probe_addr(0x38) == 0)
            ESP_LOGI(TAG, "tactile FT6336 present (0x38)");
        else
            ESP_LOGW(TAG, "tactile FT6336 absent du bus (0x38)");
        if (i2c_read_buff(ft6336_handle, 0x02, &n, 1) == ESP_OK)
            ESP_LOGI(TAG, "tactile FT6336 pret");
    }

    custom_lcd_spi_t dcfg = {};
    dcfg.cs = EPD_CS_PIN;
    dcfg.dc = EPD_DC_PIN;
    dcfg.rst = EPD_RST_PIN;
    dcfg.busy = EPD_BUSY_PIN;
    dcfg.mosi = EPD_MOSI_PIN;
    dcfg.scl = EPD_SCK_PIN;
    dcfg.spi_host = EPD_SPI_NUM;
    dcfg.buffer_len = 5000;
    epd = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, dcfg);
    epd->EPD_Init();
    epd->EPD_Clear();
    epd->EPD_Display();
    epd->EPD_Init_Partial();
    {   /* image de base : les deux RAM a blanc pour que les partiels partent proprement */
        static uint8_t blanc[FB_LEN];
        memset(blanc, 0xFF, FB_LEN);
        epd->EPD_ShowBase(blanc, FB_LEN);
    }
    ESP_LOGI(TAG, "e-paper pret");

    /* audio : alimentation, codec ES8311, micro + haut-parleur */
    board_div.POWEER_Audio_ON();
    audio_bsp_init();
    audio_play_init();
    rec_pcm = (uint8_t *)heap_caps_malloc((size_t)AUDIO_MAX_FRAMES * 2, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "audio pret (micro %s)", rec_pcm ? "ok" : "KO");

    /* carte micro SD : tout ce qui est lourd vit la (journal, memoire, sauvegarde) */
    sd_mount(true);
    if (sd_ok) {
        storage_sd_charge();
        ESP_LOGI(TAG, "carte SD prete : /sdcard/mino pour le journal, la memoire et le SOUL");
    }
    radios_charge();
    radio_vol_charge();
    {   /* module radio : etat garde en NVS (defaut : desactive) */
        nvs_handle_t nh;
        int32_t rv = 0;
        if (nvs_open("mino", NVS_READONLY, &nh) == ESP_OK) {
            nvs_get_i32(nh, "radio", &rv);
            nvs_close(nh);
        }
        radio_actif = (rv == 1);
        ESP_LOGI(TAG, "module radio : %s", radio_actif ? "actif" : "desactive");
    }
    ia_cle_charge();
    {
        nvs_handle_t nh;
        int32_t v = 300;
        if (nvs_open("mino", NVS_READONLY, &nh) == ESP_OK) {
            nvs_get_i32(nh, "micdb", &v);
            nvs_close(nh);
        }
        mic_db = (float)v / 10.0f;
        audio_mic_gain(mic_db);
        ESP_LOGI(TAG, "gain micro : %.0f dB", (double)mic_db);
    }

    /* le tamagotchi : on reprend l'etat sauvegarde, sinon un oeuf arrive */
    pet_mux = xSemaphoreCreateMutex();
    ui_mux = xSemaphoreCreateMutex();
    if (!pet_mux) ESP_LOGE(TAG, "pas de mutex pour le tamagotchi");
    pet_charge();
    ui_page = PAGE_MAITRE;
    ui_face = art_expr_id("neutre");
    if (ui_face < 0) ui_face = 0;
    show_ui();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "%d expressions en %d familles : %s, %s, %s ...",
             EXPR_N, FAMILLE_N, FAMILLES[2], FAMILLES[0], FAMILLES[3]);

    resp_buf = (char *)heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!resp_buf) ESP_LOGE(TAG, "pas de PSRAM pour la reponse");

    wifi_start();
    xEventGroupWaitBits(wifi_eg, WIFI_OK_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    time_sync();

    api_start();
    xTaskCreate(serial_task, "console", 6144, NULL, 4, NULL);
    xTaskCreate(touch_task, "tactile", 4096, NULL, 4, NULL);
    xTaskCreate(button_task, "boutons", 3072, NULL, 3, NULL);
    xTaskCreate(ui_task, "ui", 8192, NULL, 3, NULL);   /* TLS pour la meteo */
    xTaskCreate(pet_task, "tamagotchi", 4096, NULL, 3, NULL);
    /* plus de tache "promenade" : elle redessinait la page toutes les 2 s, or
       chaque image fait clignoter la dalle. Le compagnon bouge maintenant une
       fois par minute, en meme temps que l'horloge (voir ui_task). */

    ESP_LOGI(TAG, "Mino est pret. Tape ton message ici (ou 'help').");

    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
}
