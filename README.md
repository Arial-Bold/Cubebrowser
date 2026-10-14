# CubeBrowser

A small HTTPS web browser for the Nintendo GameCube, built as a `.dol` you
launch from Swiss. It needs a Broadband Adapter (BBA) for networking and an
SD2SP2 (or SD Gecko) for settings, bookmarks and certificates.

It shows text, links, headings, lists, simple tables and basic forms, so search
boxes work. It does not show pictures, styles or JavaScript. Lightweight and
text-only sites work best; the start page links to several.

## 1. Build the .dol

### Option A: Let GitHub build it (nothing to install)

1. Make a free account at https://github.com and sign in.
2. Click **+** (top right) > **New repository**. Give it a name, choose
   **Public** (free build minutes are unlimited for public repos), and click
   **Create repository**.
3. On the new repository page, click **uploading an existing file**. Unzip
   this project on your computer, open the `gcbrowser` folder, select
   everything inside it, and drag it onto the page. Click **Commit changes**.
4. Check that the `.github` folder uploaded (see "If .github is missing"
   below).
5. Open the **Actions** tab. A build named "Build CubeBrowser" starts
   automatically and takes a few minutes. A green check means it worked.
6. Click the finished build, scroll to **Artifacts**, and download
   **cubebrowser**. It's a zip containing `cubebrowser.dol` and the
   `gcbrowser` folder for your SD card.

If the build shows a red X, click it, then click the failed step to see the
error text.

**If .github is missing:** some computers hide folders whose names start with a
dot, so they don't get dragged in. In that case, on the repository page click
**Add file > Create new file**, type `.github/workflows/build.yml` as the
name, paste in the contents of that file from this project (open it with
Notepad or TextEdit), and click **Commit changes**.

### Option B: Docker

Install Docker Desktop. Then, in this folder:

```sh
./get-mbedtls.sh
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkitppc make
```

On Windows PowerShell, use `${PWD}` instead of `$PWD`.

### Option C: Install devkitPro

1. Install devkitPro (https://devkitpro.org/wiki/Getting_Started). On Windows,
   use the graphical installer and tick **GameCube development**. On Linux or
   Mac, run `sudo dkp-pacman -S gamecube-dev`.
2. In this folder, run `./get-mbedtls.sh` then `make`.

### Getting mbedTLS manually

If `get-mbedtls.sh` doesn't work on your system (for example, plain Windows
without bash), download
`https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2`,
extract it, and rename the folder so this file exists:
`third_party/mbedtls/library/ssl_tls.c`.
Use the release download, not a git clone; the release includes generated
files the build needs. Any later 3.6.x release also works.

The result is `cubebrowser.dol` in this folder.

## 2. Set up the SD card

Copy the `sd/gcbrowser` folder to the root of your SD card, so you have:

```
SD card
├── cubebrowser.dol          (anywhere you like)
└── gcbrowser/
    └── cacert.pem           (trusted root certificates)
```

The browser creates `settings.ini`, `bookmarks.txt` and `seed.bin` in the
same folder. You can edit `settings.ini` on a PC; bad values reset to
defaults.

To refresh the certificate list later, download
`https://curl.se/ca/cacert.pem` and replace `gcbrowser/cacert.pem`.

## 3. Run it

Plug in the Broadband Adapter and the SD2SP2, boot Swiss, browse to
`cubebrowser.dol` and launch it. The first start takes a few seconds while it
gets an address from your router, sets the clock and loads certificates (the
status bar shows progress).

## Controls

| Button | Action |
|---|---|
| Control Stick | Move the pointer (hold Z for slow, precise movement) |
| A | Click |
| B | Back |
| X | Type an address or search |
| Y | Bookmarks |
| START | Settings |
| C-Stick | Scroll |
| L / R | Page up / page down |
| D-Pad | Jump between links and buttons |

On the on-screen keyboard: B deletes, Y adds a space, X is shift, L and R move
the text cursor, and START accepts.

Every button except the D-Pad can be remapped in Settings > Controller.
Hold **L + R + START** while the browser starts to reset all settings.

## Settings

Settings has eight sections: Network (DHCP or static IP, DNS, time server),
Time, Controller, Keyboard, Display (aspect ratio 4:3 or 16:9, video mode
480i/480p, screen margin, text size, theme, sounds), Browsing (home page,
search engine, how the browser identifies itself, page size limit), Security
(certificate checking) and Data (clear history and bookmarks, reset).

16:9 mode is for TVs set to stretch the picture to widescreen. 480p needs
component cables.

## Testing in Dolphin

In Dolphin's GameCube settings, set **SP1** to **Broadband Adapter (HLE)**,
then open the `.dol` with File > Open.

Dolphin's support for GameCube SD adapters is limited, so the browser may not
find an SD card there. It still runs with default settings, but nothing is
saved and HTTPS sites will fail the certificate check because `cacert.pem`
can't be loaded. For Dolphin testing, try plain HTTP sites such as
`http://68k.news` or `http://frogfind.com`, or temporarily turn off
Settings > Security > Verify certificates. Test HTTPS on the real console.

## Security notes

- Certificates and hostnames are fully checked against `cacert.pem` using
  mbedTLS (TLS 1.2 and 1.3).
- The GameCube has no hardware random number generator. Randomness comes from
  a seed file saved between runs, CPU timing jitter, and controller and network
  timing, mixed through SHA-256. That's reasonable for casual browsing, but
  don't use this browser for banking or other high-value accounts.
- Turning off "Verify certificates" removes HTTPS protection; it's meant only
  for troubleshooting.

## If the build fails

This code was checked for errors on a PC against stand-in headers, but it
hasn't been compiled with devkitPPC yet. These are the spots most likely to
need a small change depending on your libogc version:

| Error mentions | What it means |
|---|---|
| `__io_gcsd2` | Your libogc is too old to know the SD2SP2. Update devkitPro (`dkp-pacman -Syu`). |
| `net_select`, `fd_set` or `FD_SET` | Timeout code in `netio.c` (`netio_wait`) needs adjusting for your libogc's socket API. |
| `asndlib.h` or `-lasnd` | The sound library isn't installed. `sound.c` can be switched to AESND. |
| `if_config` | The network setup call's arguments differ in your libogc version (`netio.c`). |
| `#error` from mbedTLS `check_config.h` | A setting in `source/gc_mbedtls_config.h` conflicts with your mbedTLS version. |

Copy the error text and it can be fixed quickly.

## Project layout

```
source/        browser source code (C)
  main.c         start-up and main loop, cube-turn transitions
  gfx.c          video modes, 4:3/16:9, drawing, text
  ui.c theme.c   widgets, pointer, themes, background
  osk.c          on-screen keyboard
  input.c        controller, pointer physics, rumble
  sound.c        synthesized sound effects
  settings*.c    settings data and Settings screen
  netio.c        Broadband Adapter, DNS, clock sync
  conn.c http.c  TLS (mbedTLS) and HTTP
  fetch.c        background download thread
  page.c         HTML renderer and forms
  browser.c      browser screen, history, bookmarks
  font_data.h    baked fonts (regenerate with tools/make_font.py)
tools/         font baking script
sd/            files for the SD card
```

The bundled fonts are DejaVu Sans (free license). To use a rounder font, run
`python3 tools/make_font.py YourFont-Bold.ttf YourFont-Regular.ttf`.
