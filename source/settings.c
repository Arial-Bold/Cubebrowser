/* settings.c - defaults, validation and INI persistence (settings.ini in the data folder). */
#include "settings.h"
#include "storage.h"
#include "theme.h"
#include "gfx.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

Settings g_set;

const char *const category_names[CAT_COUNT] = {
    "Network", "Controls", "Display", "Music", "Browsing", "Security", "Data"
};

static const char *const video_mode_names[] = { "Auto", "480i", "480p", NULL };
static const char *const aspect_names[] = { "4:3", "16:9", NULL };
static const char *const width_names[] = { "Standard (640)", "Full (704)", NULL };
static const char *const theme_choice_names[] = { "Indigo", "Jet Black", "Platinum", "Spice", NULL };
static const char *const osk_layout_names[] = { "QWERTY", "ABC", NULL };
static const char *const img_size_names[] = { "Small", "Medium", "Large", NULL };
static const char *const exit_names[] = { "Load Swiss .dol", "Restart console", NULL };
const char *const search_engine_names[] = { "DuckDuckGo Lite", "FrogFind", "Wiby", "Custom", NULL };
static const char *const search_engine_prefixes[] = {
    "https://lite.duckduckgo.com/lite/?q=", "http://frogfind.com/?q=", "https://wiby.me/?q=", ""
};
const char *const user_agent_names[] = { "CubeBrowser", "Lynx (text mode)", "Classic phone", NULL };
const char *const user_agent_strings[] = {
    "Mozilla/5.0 (Nintendo GameCube; PowerPC) CubeBrowser/" CB_VERSION,
    "Lynx/2.9.0 libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/3.0",
    "Mozilla/5.0 (Linux; U; Android 2.3.6; en-us) AppleWebKit/533.1 (KHTML, like Gecko) Version/4.0 Mobile Safari/533.1",
};

#define O(f) offsetof(Settings, f)

const SetDef g_setdefs[] = {
    /* Network: updates first, then the connection, then the clock */
    { CAT_NETWORK, NULL, "Check for updates", ST_ACTION, 0, SA_CHECK_UPDATE, 0, 0, NULL, NULL,
      "Looks for a newer CubeBrowser on GitHub and can install it for you." },
    { CAT_NETWORK, "upd.auto", "Check for updates at start", ST_BOOL, O(update_auto), 0, 1, 1, NULL, NULL,
      "A quick check when the network connects. The start page says when there's a new version." },
    { CAT_NETWORK, "net.dhcp", "Automatic IP (DHCP)", ST_BOOL, O(dhcp), 0, 1, 1, NULL, NULL,
      "Get an address from your router. Changes apply next launch." },
    { CAT_NETWORK, "net.ip", "IP address", ST_STR, O(ip), OSK_TEXTMODE_IP, 16, 0, NULL, NULL,
      "Used only when DHCP is off." },
    { CAT_NETWORK, "net.netmask", "Subnet mask", ST_STR, O(netmask), OSK_TEXTMODE_IP, 16, 0, NULL, NULL,
      "Used only when DHCP is off." },
    { CAT_NETWORK, "net.gateway", "Gateway", ST_STR, O(gateway), OSK_TEXTMODE_IP, 16, 0, NULL, NULL,
      "Your router's address. Used only when DHCP is off." },
    { CAT_NETWORK, "net.dns", "DNS server", ST_STR, O(dns), OSK_TEXTMODE_IP, 16, 0, NULL, NULL,
      "Leave blank to use the router's DNS. Example: 1.1.1.1" },
    { CAT_NETWORK, "net.ntp", "Time server", ST_STR, O(ntp), OSK_TEXTMODE_URL, 64, 0, NULL, NULL,
      "Sets the clock at startup so secure sites can be checked." },
    /* Time (shown on the Network page) */
    { CAT_NETWORK, "time.tz", "Time zone", ST_INT, O(tz_minutes), -720, 840, 30, NULL, NULL,
      "Offset from UTC. Used for the clock display." },
    { CAT_NETWORK, "time.dst", "Daylight saving", ST_BOOL, O(dst), 0, 1, 1, NULL, NULL,
      "Adds one hour to the displayed time." },
    /* Controls: controller */
    { CAT_CONTROLS, "pad.deadzone", "Stick dead zone", ST_INT, O(deadzone), 0, 40, 2, NULL, NULL,
      "Raise this if the pointer drifts on its own." },
    { CAT_CONTROLS, "pad.speed", "Pointer speed", ST_INT, O(cursor_speed), 1, 10, 1, NULL, NULL,
      "How fast the pointer moves at full tilt." },
    { CAT_CONTROLS, "pad.accel", "Acceleration", ST_INT, O(accel), 0, 10, 1, NULL, NULL,
      "Higher values give finer control on small stick movements." },
    { CAT_CONTROLS, "pad.scroll", "Scroll speed", ST_INT, O(scroll_speed), 1, 10, 1, NULL, NULL,
      "C-Stick and L/R page scrolling speed." },
    { CAT_CONTROLS, "pad.magnet", "Pointer snap", ST_BOOL, O(magnet), 0, 1, 1, NULL, NULL,
      "Gently pulls the pointer onto nearby buttons." },
    { CAT_CONTROLS, "pad.rumble", "Rumble", ST_BOOL, O(rumble), 0, 1, 1, NULL, NULL,
      "Light rumble feedback when pointing at and pressing buttons." },
    { CAT_CONTROLS, "bind.click", "Click", ST_BIND, O(bind[ACT_CLICK]), 0, BTN_COUNT - 1, 1, NULL, NULL,
      "Select a binding, then press the button you want." },
    { CAT_CONTROLS, "bind.back", "Back", ST_BIND, O(bind[ACT_BACK]), 0, BTN_COUNT - 1, 1, NULL, NULL, NULL },
    { CAT_CONTROLS, "bind.address", "Address bar", ST_BIND, O(bind[ACT_ADDRESS]), 0, BTN_COUNT - 1, 1, NULL, NULL, NULL },
    { CAT_CONTROLS, "bind.bookmarks", "Bookmarks", ST_BIND, O(bind[ACT_BOOKMARKS]), 0, BTN_COUNT - 1, 1, NULL, NULL, NULL },
    { CAT_CONTROLS, "bind.menu", "Settings menu", ST_BIND, O(bind[ACT_MENU]), 0, BTN_COUNT - 1, 1, NULL, NULL, NULL },
    { CAT_CONTROLS, "bind.slow", "Slow pointer", ST_BIND, O(bind[ACT_SLOW]), 0, BTN_COUNT - 1, 1, NULL, NULL, NULL },
    { CAT_CONTROLS, "bind.pageup", "Page up", ST_BIND, O(bind[ACT_PAGE_UP]), 0, BTN_COUNT - 1, 1, NULL, NULL, NULL },
    { CAT_CONTROLS, "bind.pagedown", "Page down", ST_BIND, O(bind[ACT_PAGE_DOWN]), 0, BTN_COUNT - 1, 1, NULL, NULL, NULL },
    /* Controls: on-screen keyboard */
    { CAT_CONTROLS, "osk.layout", "Key layout", ST_CHOICE, O(osk_layout), 0, 1, 1, osk_layout_names, NULL,
      "Arrangement of letters on the on-screen keyboard." },
    { CAT_CONTROLS, "osk.repeat", "Key repeat speed", ST_INT, O(key_repeat), 1, 10, 1, NULL, NULL,
      "How fast held Backspace and arrow shortcuts repeat." },
    /* Display */
    { CAT_DISPLAY, "disp.aspect", "Aspect ratio", ST_CHOICE, O(widescreen), 0, 1, 1, aspect_names, NULL,
      "Choose 16:9 if your TV stretches the picture to widescreen." },
    { CAT_DISPLAY, "disp.video", "Video mode", ST_CHOICE, O(video_mode), 0, 2, 1, video_mode_names, NULL,
      "480p needs component cables." },
    { CAT_DISPLAY, "disp.width", "Picture width", ST_CHOICE, O(full_width), 0, 1, 1, width_names, NULL,
      "Full uses the whole width of the TV picture. Standard leaves thin borders at the sides." },
    { CAT_DISPLAY, "disp.margin", "Screen margin", ST_INT, O(margin), 0, 10, 1, NULL, "%",
      "Raise this if the edges are cut off on your TV." },
    { CAT_DISPLAY, "disp.textsize", "Page text size", ST_INT, O(text_size), 70, 160, 10, NULL, "%",
      "Size of text on web pages." },
    { CAT_DISPLAY, "disp.theme", "Theme", ST_CHOICE, O(theme), 0, THEME_COUNT - 1, 1, theme_choice_names, NULL,
      "Colour scheme for menus." },
    { CAT_DISPLAY, "disp.anim", "Animations", ST_BOOL, O(animations), 0, 1, 1, NULL, NULL,
      "Menu motion and screen transitions." },
    { CAT_DISPLAY, "disp.sounds", "Sound effects", ST_BOOL, O(sounds), 0, 1, 1, NULL, NULL, NULL },
    { CAT_DISPLAY, "disp.volume", "Volume", ST_INT, O(volume), 0, 10, 1, NULL, NULL, NULL },
    /* Music player */
    { CAT_MUSIC, NULL, "Open the music player", ST_ACTION, 0, SA_OPEN_MUSIC, 0, 0, NULL, NULL,
      "Plays .mid files from the midi folder on your SD card while you browse." },
    { CAT_MUSIC, "music.sf2", "Instruments", ST_STR, O(music_sf2), OSK_TEXTMODE_URL, 256, 0, NULL, NULL,
      "Choose a SoundFont (.sf2) from the midi folder, or the built-in instruments." },
    { CAT_MUSIC, "music.volume", "Music volume", ST_INT, O(music_volume), 0, 10, 1, NULL, NULL, NULL },
    { CAT_MUSIC, "music.shuffle", "Shuffle", ST_BOOL, O(music_shuffle), 0, 1, 1, NULL, NULL,
      "Plays the songs in a random order." },
    { CAT_MUSIC, "music.repeat", "Repeat", ST_BOOL, O(music_repeat), 0, 1, 1, NULL, NULL,
      "Starts the list again after the last song." },
    /* Browsing */
    { CAT_BROWSING, "web.home", "Home page", ST_STR, O(home), OSK_TEXTMODE_URL, 256, 0, NULL, NULL,
      "Use about:home for the built-in start page." },
    { CAT_BROWSING, "web.search", "Search engine", ST_CHOICE, O(search_engine), 0, 3, 1, search_engine_names, NULL,
      "Lightweight engines work best on GameCube." },
    { CAT_BROWSING, "web.searchurl", "Custom search URL", ST_STR, O(search_custom), OSK_TEXTMODE_URL, 256, 0, NULL, NULL,
      "Your words are added to the end, e.g. https://site/search?q=" },
    { CAT_BROWSING, "web.ua", "Identify as", ST_CHOICE, O(user_agent), 0, 2, 1, user_agent_names, NULL,
      "Some sites send simpler pages to text browsers." },
    { CAT_BROWSING, "web.css", "Page styles", ST_BOOL, O(page_styles), 0, 1, 1, NULL, NULL,
      "Use each site's own colours, fonts and layout. Off shows plain pages." },
    { CAT_BROWSING, "web.images", "Load images", ST_BOOL, O(load_images), 0, 1, 1, NULL, NULL,
      "Show pictures on pages. Turn off for faster loading." },
    { CAT_BROWSING, "web.imgsize", "Picture size", ST_CHOICE, O(img_size), 0, 2, 1, img_size_names, NULL,
      "Limits how tall pictures can be. Smaller means less scrolling and faster loading." },
    { CAT_BROWSING, "web.imgproxy", "Image helper", ST_BOOL, O(image_proxy), 0, 1, 1, NULL, NULL,
      "Shrinks pictures on images.weserv.nl. Faster, but that site sees image addresses." },
    { CAT_BROWSING, "web.alt", "Show image labels", ST_BOOL, O(show_alt), 0, 1, 1, NULL, NULL,
      "Show descriptions for pictures that can't be shown." },
    { CAT_BROWSING, "web.maxkb", "Max page size", ST_INT, O(max_page_kb), 256, 4096, 256, NULL, "KB",
      "Larger pages are cut off to protect memory." },
    /* Security */
    { CAT_SECURITY, "sec.verify", "Verify certificates", ST_BOOL, O(verify_certs), 0, 1, 1, NULL, NULL,
      "Keep this on. Turning it off removes HTTPS protection." },
    { CAT_SECURITY, "sec.cookies", "Cookies", ST_BOOL, O(cookies), 0, 1, 1, NULL, NULL,
      "Lets sites remember you, which logins need. Off: no site can recognise you." },
    { CAT_SECURITY, "sec.keep", "Keep logins after restart", ST_BOOL, O(keep_logins), 0, 1, 1, NULL, NULL,
      "Saves cookies to the SD card. Anyone with the card could use your logins." },
    { CAT_SECURITY, "sec.ca", "Certificate file", ST_STR, O(ca_path), OSK_TEXTMODE_URL, 128, 0, NULL, NULL,
      "Trusted root certificates (PEM). Applies next launch." },
    /* Data */
    { CAT_DATA, "exit.action", "Exit button", ST_CHOICE, O(exit_action), 0, 1, 1, exit_names, NULL,
      "Restart console suits setups where Swiss starts automatically at power-on." },
    { CAT_DATA, "exit.dol", "Swiss .dol file", ST_STR, O(swiss_dol), OSK_TEXTMODE_URL, 256, 0, NULL, NULL,
      "The program the Exit button starts. Use Find .dol files to pick it." },
    { CAT_DATA, NULL, "Find .dol files on SD card", ST_ACTION, 0, SA_FIND_DOL, 0, 0, NULL, NULL,
      "Lists the programs on your card so you can choose Swiss." },
    { CAT_DATA, "dl.folder", "Download folder", ST_STR, O(dl_folder), OSK_TEXTMODE_URL, 128, 0, NULL, NULL,
      "Where downloaded files are saved. Swiss can open them from here." },
    { CAT_DATA, "dl.delzip", "Delete zip after unzipping", ST_BOOL, O(dl_delete_zip), 0, 1, 1, NULL, NULL,
      "Saves space by removing the .zip once its files are extracted." },
    { CAT_DATA, NULL, "Move data folder", ST_ACTION, 0, SA_MOVE_DATA, 0, 0, NULL, NULL,
      "Where settings, bookmarks and cookies are kept. Press twice to confirm." },
    { CAT_DATA, NULL, "Clear cookies and logins", ST_ACTION, 0, SA_CLEAR_COOKIES, 0, 0, NULL, NULL,
      "Signs you out of every site. Press twice to confirm." },
    { CAT_DATA, NULL, "Clear history", ST_ACTION, 0, SA_CLEAR_HISTORY, 0, 0, NULL, NULL,
      "Forget Back/Forward pages." },
    { CAT_DATA, NULL, "Delete all bookmarks", ST_ACTION, 0, SA_CLEAR_BOOKMARKS, 0, 0, NULL, NULL,
      "Press twice to confirm." },
    { CAT_DATA, NULL, "Reset all settings", ST_ACTION, 0, SA_RESET_ALL, 0, 0, NULL, NULL,
      "Press twice to confirm. Hold L+R+START at boot to do this too." },
    { CAT_DATA, NULL, "Save settings now", ST_ACTION, 0, SA_SAVE_NOW, 0, 0, NULL, NULL,
      "Settings also save when you leave this screen." },
};
const int g_setdef_count = sizeof(g_setdefs) / sizeof(g_setdefs[0]);

int *setdef_int(const SetDef *d) { return (int *)((char *)&g_set + d->off); }
char *setdef_str(const SetDef *d) { return (char *)&g_set + d->off; }

void settings_defaults(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->dhcp = 1;
    strcpy(s->ip, "192.168.1.50");
    strcpy(s->netmask, "255.255.255.0");
    strcpy(s->gateway, "192.168.1.1");
    s->dns[0] = 0;
    strcpy(s->ntp, "pool.ntp.org");
    s->tz_minutes = -300;
    s->dst = 1;
    s->deadzone = 12;
    s->cursor_speed = 6;
    s->accel = 5;
    s->scroll_speed = 5;
    s->rumble = 1;
    s->magnet = 1;
    s->bind[ACT_CLICK] = BTN_A;
    s->bind[ACT_BACK] = BTN_B;
    s->bind[ACT_ADDRESS] = BTN_X;
    s->bind[ACT_BOOKMARKS] = BTN_Y;
    s->bind[ACT_MENU] = BTN_START;
    s->bind[ACT_SLOW] = BTN_Z;
    s->bind[ACT_PAGE_UP] = BTN_L;
    s->bind[ACT_PAGE_DOWN] = BTN_R;
    s->key_repeat = 5;
    s->osk_layout = 0;
    s->video_mode = VMODE_AUTO;
    s->widescreen = 0;
    s->full_width = 1;
    s->margin = 4;
    s->text_size = 100;
    s->theme = THEME_INDIGO;
    s->animations = 1;
    s->sounds = 1;
    s->volume = 7;
    s->music_sf2[0] = 0;
    s->music_volume = 7;
    s->music_shuffle = 0;
    s->music_repeat = 1;
    strcpy(s->home, "about:home");
    s->search_engine = 0;
    strcpy(s->search_custom, "https://html.duckduckgo.com/html/?q=");
    s->user_agent = 0;
    s->show_alt = 1;
    s->load_images = 1;
    s->page_styles = 1;
    s->img_size = 1;
    s->image_proxy = 1;
    s->max_page_kb = 1024;
    s->verify_certs = 1;
    s->cookies = 1;
    s->keep_logins = 1;
    storage_path(s->ca_path, sizeof(s->ca_path), "cacert.pem");
    strcpy(s->dl_folder, "sd:/downloads");
    s->dl_connections = 4;
    s->update_auto = 1;
    s->exit_action = 0;
    strcpy(s->swiss_dol, "sd:/ipl.dol");
}

static bool valid_ipv4(const char *s) {
    int parts = 0, val = -1;
    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            val = (val < 0 ? 0 : val) * 10 + (*s - '0');
            if (val > 255) return false;
        } else if (*s == '.') {
            if (val < 0) return false;
            parts++;
            val = -1;
        } else {
            return false;
        }
    }
    return parts == 3 && val >= 0;
}

void settings_validate(Settings *s) {
    Settings def;
    settings_defaults(&def);
    for (int i = 0; i < g_setdef_count; i++) {
        const SetDef *d = &g_setdefs[i];
        if (d->type == ST_ACTION) continue;
        char *base = (char *)s, *dbase = (char *)&def;
        if (d->type == ST_STR) {
            char *str = base + d->off;
            str[d->max - 1] = 0;
            if (d->min == OSK_TEXTMODE_IP && str[0] && !valid_ipv4(str))
                strcpy(str, dbase + d->off);
        } else {
            int *v = (int *)(base + d->off);
            if (*v < d->min || *v > d->max) *v = *(int *)(dbase + d->off);
            if (d->type == ST_INT && d->step > 1 && (*v - d->min) % d->step)
                *v = d->min + ((*v - d->min) / d->step) * d->step;
        }
    }
    if (!s->home[0]) strcpy(s->home, def.home);
    if (!s->ca_path[0]) strcpy(s->ca_path, def.ca_path);
    if (strncmp(s->dl_folder, "sd:/", 4) != 0) strcpy(s->dl_folder, def.dl_folder);
    if (!s->swiss_dol[0]) strcpy(s->swiss_dol, def.swiss_dol);
    /* Click and Back must stay on distinct buttons or the UI becomes unusable */
    if (s->bind[ACT_CLICK] == s->bind[ACT_BACK]) {
        s->bind[ACT_CLICK] = BTN_A;
        s->bind[ACT_BACK] = BTN_B;
    }
}

static const SetDef *find_def(const char *key) {
    for (int i = 0; i < g_setdef_count; i++)
        if (g_setdefs[i].key && strcmp(g_setdefs[i].key, key) == 0) return &g_setdefs[i];
    return NULL;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

bool settings_load(void) {
    settings_defaults(&g_set);
    size_t len;
    char ini[400], tmp[400];
    storage_path(ini, sizeof(ini), "settings.ini");
    storage_path(tmp, sizeof(tmp), "settings.tmp");
    char *data = storage_read(ini, &len);
    if (!data) data = storage_read(tmp, &len);  /* interrupted save */
    if (!data) return false;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';' || *l == '[') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(l), *val = trim(eq + 1);
        const SetDef *d = find_def(key);
        if (!d) continue;
        if (d->type == ST_STR) {
            snprintf(setdef_str(d), d->max, "%s", val);
        } else if (d->type == ST_BIND) {
            for (int b = 0; b < BTN_COUNT; b++)
                if (strcasecmp(val, button_names[b]) == 0) *setdef_int(d) = b;
        } else {
            *setdef_int(d) = atoi(val);
        }
    }
    free(data);
    /* the old fixed default follows the data folder if it has moved */
    if (!strcmp(g_set.ca_path, STORAGE_ROOT_DIR "/cacert.pem")) storage_path(g_set.ca_path, sizeof(g_set.ca_path), "cacert.pem");
    settings_validate(&g_set);
    return true;
}

bool settings_save(void) {
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap);
    if (!buf) return false;
    n += snprintf(buf + n, cap - n, "# CubeBrowser settings. Edit on a PC if you like; bad values reset to defaults.\n");
    for (int c = 0; c < CAT_COUNT; c++) {
        n += snprintf(buf + n, cap - n, "\n[%s]\n", category_names[c]);
        for (int i = 0; i < g_setdef_count && n < cap - 400; i++) {
            const SetDef *d = &g_setdefs[i];
            if (d->cat != c || !d->key) continue;
            if (d->type == ST_STR)
                n += snprintf(buf + n, cap - n, "%s=%s\n", d->key, setdef_str(d));
            else if (d->type == ST_BIND)
                n += snprintf(buf + n, cap - n, "%s=%s\n", d->key, button_names[*setdef_int(d)]);
            else
                n += snprintf(buf + n, cap - n, "%s=%d\n", d->key, *setdef_int(d));
        }
    }
    char ini[400], tmp[400];
    storage_path(ini, sizeof(ini), "settings.ini");
    storage_path(tmp, sizeof(tmp), "settings.tmp");
    bool ok = storage_write_atomic(ini, tmp, buf, n);
    free(buf);
    return ok;
}

void settings_format_value(const SetDef *d, char *out, size_t outlen) {
    switch (d->type) {
    case ST_BOOL: snprintf(out, outlen, "%s", *setdef_int(d) ? "On" : "Off"); break;
    case ST_CHOICE: snprintf(out, outlen, "%s", d->choices[*setdef_int(d)]); break;
    case ST_BIND: snprintf(out, outlen, "%s", button_names[*setdef_int(d)]); break;
    case ST_STR: snprintf(out, outlen, "%s", setdef_str(d)[0] ? setdef_str(d) : "(blank)"); break;
    case ST_ACTION: out[0] = 0; break;
    case ST_INT:
        if (d->off == offsetof(Settings, tz_minutes)) {
            int m = *setdef_int(d), a = m < 0 ? -m : m;
            snprintf(out, outlen, "UTC%c%02d:%02d", m < 0 ? '-' : '+', a / 60, a % 60);
        } else {
            snprintf(out, outlen, "%d%s%s", *setdef_int(d), d->unit ? " " : "", d->unit ? d->unit : "");
        }
        break;
    }
}

const char *settings_search_prefix(void) {
    if (g_set.search_engine == 3) return g_set.search_custom;
    return search_engine_prefixes[g_set.search_engine];
}

float settings_max_picture_height(void) {
    static const float h[3] = { 140, 240, 1024 };
    return h[g_set.img_size >= 0 && g_set.img_size <= 2 ? g_set.img_size : 1];
}
