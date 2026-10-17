/* settings.h - every user setting, its defaults, INI persistence and the
 * descriptor table that drives both the INI file and the Settings screen. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "input.h"

typedef struct {
    /* Network */
    int  dhcp;
    char ip[16], netmask[16], gateway[16], dns[16];
    char ntp[64];
    /* Time */
    int  tz_minutes, dst;
    /* Controller */
    int  deadzone, cursor_speed, accel, scroll_speed, rumble, magnet;
    int  bind[ACT_COUNT];
    /* Keyboard */
    int  key_repeat, osk_layout;
    /* Display */
    int  video_mode, widescreen, margin, text_size, theme, animations, sounds, volume, full_width;
    /* Music */
    char music_sf2[256];   /* "" = first .sf2 in the midi folder, "builtin", or a file */
    int  music_volume, music_shuffle, music_repeat, music_glow;
    /* Browsing */
    char home[256];
    int  search_engine;
    char search_custom[256];
    int  user_agent, show_alt, max_page_kb, load_images, image_proxy, page_styles, img_size;
    /* Security */
    int  verify_certs;
    char ca_path[128];
    int  cookies, keep_logins;
    /* Downloads */
    char dl_folder[128];
    int  dl_delete_zip, dl_connections;
    int  update_auto;
    /* Leaving the browser */
    int  exit_action;
    char swiss_dol[256];
} Settings;

extern Settings g_set;

typedef enum { CAT_NETWORK, CAT_CONTROLS, CAT_DISPLAY, CAT_MUSIC, CAT_BROWSING, CAT_SECURITY, CAT_DATA, CAT_COUNT } SetCategory;
extern const char *const category_names[CAT_COUNT];

typedef enum { ST_BOOL, ST_INT, ST_CHOICE, ST_STR, ST_BIND, ST_ACTION } SetType;

enum { SA_CLEAR_HISTORY = 1, SA_CLEAR_BOOKMARKS, SA_RESET_ALL, SA_SAVE_NOW, SA_FIND_DOL, SA_CLEAR_COOKIES, SA_MOVE_DATA, SA_CHECK_UPDATE, SA_OPEN_MUSIC };
enum { OSK_TEXTMODE_TEXT = 0, OSK_TEXTMODE_URL = 1, OSK_TEXTMODE_IP = 2 };

typedef struct {
    int cat;
    const char *key;           /* INI key; NULL for actions */
    const char *label;
    SetType type;
    size_t off;                /* offset into Settings */
    int min, max, step;        /* ints; for ST_STR max = buffer size, min = OSK mode; for ST_ACTION min = id */
    const char *const *choices;
    const char *unit;
    const char *help;
} SetDef;

extern const SetDef g_setdefs[];
extern const int g_setdef_count;

extern const char *const search_engine_names[];
extern const char *const user_agent_names[];
extern const char *const user_agent_strings[];

void settings_defaults(Settings *s);
void settings_validate(Settings *s);
bool settings_load(void);
bool settings_save(void);
int  *setdef_int(const SetDef *d);
char *setdef_str(const SetDef *d);
void settings_format_value(const SetDef *d, char *out, size_t outlen);
const char *settings_search_prefix(void);
float settings_max_picture_height(void);
