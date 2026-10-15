# CubeBrowser

A web browser for the Nintendo GameCube built with Claude Opus 5.5. It loads real websites over a wired
network connection, including secure (HTTPS) sites, with pictures, page
styles, tables and forms, all controlled with a standard GameCube controller.
It runs as a `.dol` program launched from Swiss.

---

## What you need

| Item | Notes |
|---|---|
| A GameCube that runs Swiss | Swiss loads the browser like any other homebrew program. |
| A network adapter | The official Nintendo Broadband Adapter, or a WIZnet-based adapter such as the ETH2GC. Plug it into your router with an Ethernet cable. |
| An SD card adapter | SD2SP2 (recommended) or SD Gecko. Holds the browser, its certificates, settings and bookmarks. |
| A GameCube controller | In port 1. |

Component cables are optional; they unlock 480p video.

---

## Installing

### 1. Get the files

Download the latest build from your GitHub repository: open the **Actions**
tab, click the newest run with a green check, scroll to **Artifacts**, and
download **cubebrowser**. Unzip it. Inside you'll find:

- `cubebrowser.dol`, the browser itself
- a `gcbrowser` folder with `cacert.pem`, the list of trusted certificates
  used to check secure websites

(To build it yourself instead, see [Building from source](#building-from-source)
at the end.)

### 2. Copy them to your SD card

Put the `gcbrowser` folder in the **root** of the SD card, and the `.dol`
anywhere you like:

```
SD card
├── cubebrowser.dol
└── gcbrowser/
    └── cacert.pem
```

The browser adds its own files to `gcbrowser` as you use it:

| File | Contents |
|---|---|
| `settings.ini` | Your settings (plain text; editable on a PC) |
| `bookmarks.txt` | Your bookmarks |
| `seed.bin` | Saved randomness for secure connections |

### 3. Launch it

Insert the SD card and network adapter, boot Swiss, browse to
`cubebrowser.dol`, and launch it.

On the first start, the status bar at the bottom shows the browser getting an
address from your router, setting its clock, and loading certificates. This
takes a few seconds. When the dot in the bottom-right corner turns green,
you're online and the start page is ready.

---

## Using the browser

### Controls

| Button | Action |
|---|---|
| Control Stick | Move the pointer |
| Z (hold) | Slow, precise pointer movement |
| A | Click |
| B | Back |
| X | Type an address or search |
| Y | Bookmarks |
| START | Settings |
| C-Stick | Scroll the page |
| L / R | Page up / page down |
| D-Pad | Jump the pointer between links and buttons |

Every button except the D-Pad can be changed in Settings > Controller.

### Going to a website or searching

Press **X** (or click the address bar). Type a web address such as
`text.npr.org`, or type any words to search. Then press **START** or click
**OK**. Addresses without `https://` are opened securely automatically.

### The on-screen keyboard

| Button | Action |
|---|---|
| A | Press the key under the pointer |
| D-Pad | Jump from key to key |
| B | Delete |
| Y | Space |
| X | Shift (click the on-screen Shift key twice for caps lock) |
| L / R | Move the text cursor |
| START | Accept |

When typing an address, a row of shortcut keys (`https://`, `www.`, `.com`,
`.org`, `.net`, `/`) saves typing.

### The toolbar

From left to right: **Back**, **Forward**, **Reload** (becomes **Stop** while
loading), **Home**, the **address bar**, **Bookmarks** (star) and
**Settings** (gear). A green lock in the address bar means the site's
security certificate was checked successfully.

### Bookmarks

Press **Y** to open your bookmarks. Click **Bookmark this page** to add the
current page, click a bookmark to open it, or click the **x** beside one twice
to delete it.

### Forms

Click a text box to type into it. Search boxes submit automatically when you
press OK. Checkboxes and radio buttons toggle when clicked, and submit buttons
send the form.

### Status bar

The bottom bar shows loading progress, the address of the link under the
pointer, the network status dot (green: connected, yellow: connecting, red:
no network) and the time.

---

## Features

- **Secure browsing (HTTPS)** with full certificate checking, using TLS 1.2
  and 1.3.
- **Pictures** (JPEG, PNG, GIF and BMP), loaded after the text so pages
  appear quickly.
- **Page styles:** the colours, fonts, spacing, borders and backgrounds each
  site chooses, including hiding elements sites mark as hidden (menus, cookie
  banners). The browser presents itself as a phone-sized screen, so many sites
  send their simpler mobile layouts.
- **Tables** with real side-by-side columns, so classic table-based sites look
  the way they were designed.
- **Floating pictures and boxes** with text wrapping around them.
- **Fonts:** regular, bold, italic and monospace.
- **Lists** with bullets, numbers and letters.
- **Forms:** text boxes, password fields, checkboxes, radio buttons and submit
  buttons, sent by GET or POST.
- **Framed pages** (a 1990s technique) show a link to each part of the page.
- **History** with Back and Forward, and **bookmarks** saved to the SD card.
- **Four themes** named after the console colours: Indigo, Jet Black,
  Platinum and Spice.
- **4:3 and 16:9** display, 480i or 480p, and adjustable screen margins for
  TVs that cut off the edges.
- **Fully remappable controls**, pointer speed and acceleration, D-Pad
  snapping, and rumble feedback.
- **Synthesized sound effects** and smooth, animated menus.
- **Built-in start page** with links to sites that work well.

---

## Settings

Press **START** to open Settings. Changes apply immediately and are saved when
you leave the Settings screen. Hold **L + R + START** while the browser starts
to reset everything to defaults.

| Section | What you can change |
|---|---|
| **Network** | Automatic (DHCP) or manual IP address, subnet mask, gateway, DNS server, time server. Network changes apply the next time you launch the browser. |
| **Time** | Time zone and daylight saving, for the clock in the status bar. |
| **Controller** | Stick dead zone, pointer speed, acceleration, scroll speed, pointer snap, rumble, and the button for each action. To remap, click an action and press the button you want. |
| **Keyboard** | QWERTY or ABC key layout, and key repeat speed. |
| **Display** | Aspect ratio (4:3 or 16:9), video mode (Auto, 480i, 480p), screen margin, page text size, theme, animations, sound effects and volume. |
| **Browsing** | Home page, search engine (DuckDuckGo Lite, FrogFind, Wiby or your own), how the browser identifies itself to sites, page styles on/off, pictures on/off, image helper, picture labels, and the largest page size to load. |
| **Security** | Certificate checking and the certificate file location. |
| **Data** | Clear history, delete all bookmarks, reset all settings, save settings now. |

### Notes on a few settings

- **16:9** is for TVs set to stretch the picture to widescreen. Pages are laid
  out wider to use the extra space.
- **Page styles: Off** shows every page in a plain, readable format. Useful if
  a site's design doesn't suit the TV.
- **Identify as: Lynx (text mode)** makes some sites send very simple pages,
  which load faster.
- **Image helper** (on by default) routes pictures through the free
  images.weserv.nl service, which shrinks them for your screen and converts
  formats the GameCube can't read (such as WebP). It makes pictures much faster
  and work on more sites, but that service sees the address of each picture
  you view. Turn it off to fetch pictures directly.
- **Verify certificates** should stay on. Turning it off removes the
  protection HTTPS provides; it's only meant for troubleshooting.

---

## Sites that work well

The GameCube has a 485 MHz processor and 24 MB of memory, so lightweight sites
work best. Good places to start (all linked from the start page):

- **DuckDuckGo Lite** (search): `lite.duckduckgo.com/lite`
- **Wikipedia, mobile version**: `en.m.wikipedia.org`
- **NPR Text** (news): `text.npr.org`
- **CNN Lite** (news): `lite.cnn.com`
- **68k.news** (headlines): `68k.news`
- **FrogFind** (search that simplifies pages): `frogfind.com`
- **Wiby** (search for classic-style sites): `wiby.me`

Older and hobbyist sites from the 1990s and 2000s usually display very well.

---

## Troubleshooting

| Problem | What to try |
|---|---|
| **"No network (error -1)"** | Check the Ethernet cable and that the adapter is seated firmly. If you use manual network settings, check them under Settings > Network. Retry by opening any page. |
| **A secure site says its certificate couldn't be verified** | Usually the clock. Make sure the time server under Settings > Network is reachable (the default `pool.ntp.org` works for most people). If it persists, download a fresh certificate list from `https://curl.se/ca/cacert.pem` and replace `gcbrowser/cacert.pem` on the SD card. |
| **A page is blank or nearly empty** | The site probably builds its content with JavaScript, which isn't supported. Try the site's mobile or "lite" version, or open it through FrogFind. |
| **A page looks cluttered or oddly arranged** | Modern multi-column layouts are shown as a single column. Try Settings > Browsing > Page styles: Off for a clean, plain view. |
| **Pictures don't appear** | Check Settings > Browsing > Load images. With the image helper off, some formats (like WebP) can't be shown. Very large pages show only their first several pictures to save memory. |
| **A page is cut off at the end** | It was larger than the page size limit. Raise Settings > Browsing > Max page size. |
| **The screen edges are cut off on my TV** | Increase Settings > Display > Screen margin. |
| **The picture looks squashed or stretched** | Match Settings > Display > Aspect ratio to your TV's setting. |
| **Settings or bookmarks aren't saved** | The SD card wasn't found. Make sure it's in the SD2SP2 (or SD Gecko) and formatted FAT32. |
| **The pointer drifts on its own** | Raise Settings > Controller > Stick dead zone. |
| **Something's wrong and I can't get back** | Hold L + R + START while launching to reset all settings. |

---

## Limitations

- **No JavaScript.** Sites that depend on it may be blank or only partly work.
- **No modern multi-column layouts** (CSS flexbox and grid). These sections
  are stacked into a single column.
- **No video, audio or animation.** Animated GIFs show their first frame.
- **Drop-down menus** show their selected choice but can't be changed yet.
- **Frames** open one part at a time rather than side by side.
- **Downloads** aren't supported yet.
- **Speed:** heavy pages can take several seconds to lay out, and each new
  secure site needs a second or two to set up its connection.

## Security and privacy

- Secure sites are checked against the trusted certificate list on your SD
  card, using mbedTLS.
- The GameCube has no hardware random number generator. The browser gathers
  randomness from a seed saved between sessions, processor timing, and
  controller and network activity. That's fine for everyday browsing, but don't
  use it for banking or other high-value accounts.
- With the image helper on, images.weserv.nl sees the addresses of pictures
  you view.
- The browser keeps no cookies, so you won't stay logged in to sites.

---

## Building from source

The easiest way is GitHub Actions, which builds on GitHub's servers with
nothing to install:

1. Create a free GitHub account and a new **public** repository.
2. Upload everything inside this project's folder, including the hidden
   `.github` folder. If your computer hides it, create
   `.github/workflows/cubebrowser-libogc2.yml` on GitHub with **Add file >
   Create new file** and paste in the file's contents.
3. Open the **Actions** tab. Each upload starts a build; when it shows a green
   check, download the **cubebrowser** artifact.

If a build fails, click the run, then the failed step, to see the error.

**With Docker** (any operating system):

```sh
./get-mbedtls.sh
docker run --rm -v "$PWD:/src" -w /src ghcr.io/extremscorner/libogc2:latest make
```

The build uses [libogc2](https://libogc2.org/) (for official and WIZnet
network adapters), [mbedTLS 3.6](https://github.com/Mbed-TLS/mbedtls) and
[stb_image](https://github.com/nothings/stb). `get-mbedtls.sh` downloads the
last two into `third_party/`.

### Project layout

| Path | Contents |
|---|---|
| `source/main.c` | Start-up, main loop, screen transitions |
| `source/browser.c` | Browser screen, history, bookmarks, download scheduling |
| `source/page.c` | Layout engine (blocks, text, floats, tables), drawing, forms |
| `source/dom.c`, `source/css.c` | HTML parsing and stylesheets |
| `source/http.c`, `source/conn.c` | HTTP and secure connections |
| `source/netio.c`, `source/fetch.c` | Network setup, DNS, clock, background downloads |
| `source/image.c` | Picture decoding to GameCube textures |
| `source/gfx.c`, `source/ui.c`, `source/osk.c` | Graphics, menus, on-screen keyboard |
| `source/settings*.c`, `source/input.c`, `source/sound.c` | Settings, controller, sound |
| `tools/make_font.py` | Regenerates the built-in fonts |
| `sd/gcbrowser/` | Files for the SD card |

The bundled fonts are DejaVu Sans (free license). For a rounder look, run
`python3 tools/make_font.py` with your own `.ttf` files (bold, regular,
italic, monospace).
