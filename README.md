# CubeBrowser

**A real web browser for the Nintendo GameCube.**

CubeBrowser loads websites over a wired network connection, including secure
(HTTPS) sites, with pictures, page styles, tables and forms. You can download
files straight to your SD card, and control everything with a GameCube
controller. It runs as a `.dol` program launched from Swiss,
with menus designed to feel like they belong on the console.

---

## What you need

| Item | Notes |
|---|---|
| A GameCube that runs **Swiss** | CubeBrowser launches like any other homebrew program. |
| A **network adapter** | The official Nintendo Broadband Adapter, or a WIZnet-based adapter such as the ETH2GC, connected to your router with an Ethernet cable. |
| An **SD card adapter** | SD2SP2 (recommended) or SD Gecko, with a FAT32 card. |
| A **controller** | Any port. Bluetooth controllers through BlueRetro work too. |

Component cables are optional and unlock 480p video.

---

## Installing

1. Download the latest release from the **Releases** section of this page.
   You'll get `cubebrowser.dol` and a `gcbrowser` folder.
2. Copy both to your SD card, keeping the `gcbrowser` folder **next to**
   `cubebrowser.dol`. That can be the root of the card or any folder you keep
   apps in:

   ```
   SD card                        SD card
   ├── cubebrowser.dol      or    └── apps/
   └── gcbrowser/                     └── cubebrowser/
       └── cacert.pem                     ├── cubebrowser.dol
                                          └── gcbrowser/
                                              └── cacert.pem
   ```

   The `gcbrowser` folder holds your settings, bookmarks and saved logins.
   CubeBrowser uses the one next to the `.dol` if there is one, otherwise
   `gcbrowser` at the root of the card. Settings from an older version kept at
   the root are moved over automatically. You can also move it later in
   Settings > Data > Move data folder.

3. Insert the SD card, plug in the network adapter, boot Swiss and launch
   `cubebrowser.dol`.

On first start, the status bar shows the browser connecting to your router,
setting its clock and loading security certificates. When the dot in the
bottom-right corner turns green, you're online.

To upgrade later, use **Settings > Network > Check for updates** (or the
"Update available" bubble on the start page), which downloads the newest
release from GitHub and installs it. You can also just replace
`cubebrowser.dol` yourself. Your settings, bookmarks and downloads stay put.

---

## Controls

### Controller

| Button | Action |
|---|---|
| Control Stick | Move the pointer |
| Z (hold) | Slow, precise pointer movement |
| A | Click |
| Z + A | Save a link to the SD card instead of opening it |
| B | Back |
| X | Type an address or search |
| Y | Bookmarks |
| START | Settings |
| C-Stick | Scroll |
| L / R | Page up / page down |
| D-Pad | Jump between links and buttons |

Every button except the D-Pad can be remapped in Settings > Controls.

### On-screen keyboard

Point and press A on a key, or use the D-Pad to hop from key to key.

| Button | Action |
|---|---|
| B | Delete |
| Y | Space |
| X | Shift (click the Shift key twice for caps lock) |
| L / R | Move the text cursor |
| START | Accept |

When typing an address, shortcut keys (`https://`, `www.`, `.com`, `.org`,
`.net`, `/`) save typing.

---

## Browsing

- **Go anywhere:** press X, type an address like `text.npr.org`, or type any
  words to search. Addresses open securely (HTTPS) automatically.
- **Toolbar:** Back, Forward, Home, Exit (door), the address bar (with a stop
  button inside it while a page loads), Music (note), Bookmarks (star) and
  Settings (gear). A green lock means the site's security certificate checked
  out.
- **Exit to Swiss:** press the Exit button twice to leave the browser and
  start Swiss. Choose which `.dol` it starts under Settings > Data (**Find .dol
  files on SD card** lists them), or set the Exit button to **Restart console**
  if Swiss starts automatically when your GameCube powers on. If the chosen
  file has moved, the browser looks for a Swiss `.dol` on the card by itself.
- **Back and Forward** return instantly to your last few pages, scrolled to
  where you left them, and Back after a search takes you to your results.
- **Bookmarks:** press Y, then **Bookmark this page**. Click a bookmark to open
  it, or click the **x** beside one twice to delete it.
- **Forms:** text boxes, password fields, checkboxes, radio buttons and submit
  buttons all work. Search boxes submit as soon as you press OK.
- **Logins:** cookies let sites remember you, so traditional login forms work
  (forums, wikis, hobby sites, router pages). Logins are kept after restarting
  unless you turn that off. Sites whose sign-in relies on JavaScript (Google,
  Microsoft, Facebook, Reddit and most other big services) can't be logged
  into. Only use accounts that don't matter much (see Security and privacy).
- **Status bar:** loading progress, the address of the link under the pointer,
  the network status dot (green connected, yellow connecting, red offline) and
  the time.

---

## Music player

CubeBrowser can play MIDI music in the background while you browse.

- Put `.mid` files (also `.midi`, `.kar` and `.rmi`) in a folder called `midi`
  at the root of your SD card. The folder is created for you the first time
  you open the player.
- Press the **note button** in the toolbar to open the player: play/pause,
  previous, next, stop, shuffle, repeat, volume and the list of songs. Close it
  and the music keeps playing; the note button pulses while it does.
- The music is played the way GameCube games play theirs: each note is a
  recorded instrument sound on one of the console's hardware sound voices.
- **Instruments:** for real instrument sounds, add a General MIDI SoundFont
  (`.sf2`) to the `midi` folder. Any size works, even 100 MB and more: like a
  game loading a level's sounds, each song loads only the instrument samples
  it uses (up to about 10 MB of them), so a song may take a few seconds to
  start the first time. Without a SoundFont, simple built-in instruments are
  used. Pick a SoundFont, or switch back to the
  built-in instruments, under Settings > Music > Instruments.

## How pages look

CubeBrowser has its own layout engine, so pages look like real web pages
rather than plain text:

- **Page styles:** each site's colours, fonts, sizes, spacing, borders and
  backgrounds. Menus, cookie banners and other elements that sites hide are
  kept hidden.
- **Mobile layouts:** the browser presents itself as a phone-sized screen, so
  many sites send their simpler single-column versions, which suit a TV well.
- **Tables** with real side-by-side columns, so classic table-based sites look
  the way they were built.
- **Pictures** (JPEG, PNG, GIF, BMP), loaded after the text so pages appear
  quickly. Text wraps around pictures placed to the side, and duplicate copies
  of the same photo (which news sites often include for different screen
  sizes) are shown only once.
- **Fonts:** regular, bold, italic and monospace.
- **Lists** with bullets, numbers and letters.
- **Framed pages** (a 1990s technique) show a link to each part of the page.
- **Always readable:** if a page's colours would make text hard to read,
  CubeBrowser switches to a readable colour.

Prefer a cleaner look on a particular site? Turn off **Settings > Browsing >
Page styles** for a plain, readable view.

---

## Downloads

Click a link to a file (such as `.dol`, `.zip`, `.iso`, `.mp3` or `.pdf`) and
CubeBrowser asks **"Save to SD card?"**, showing the file's name and size. Hold
**Z** and press **A** to save any link this way.

- Files are saved to `sd:/downloads` (changeable in Settings > Data), where
  Swiss can launch them after you leave the browser.
- The download screen shows progress, speed and time remaining. Cancel takes
  two presses, so it can't be hit by accident.
- **Interrupted downloads continue where they stopped:** choose Retry, or open
  the same link again later.
- **Zip files** can be unzipped automatically (**Download & unzip**) or later
  from the "Download complete" screen. Files go into a folder named after the
  zip, and the zip can be deleted afterwards if you like.
- Existing files are never overwritten, and free space is checked before
  starting.

**Faster downloads:** large files are downloaded over several connections at
once (up to 4, fewer automatically if the server or network struggles) on
servers that allow it, which most file hosts do. Each connection on its own is
limited by the GameCube's network stack, so this typically makes big
downloads several times faster. Plain `http://` downloads and servers on your
own network are the fastest of all.

Please only download software you have the
right to use, such as homebrew or backups of your own games.

---

## Settings

Press **START**. Changes apply immediately and save when you leave the screen.
Hold **L + R + START** while CubeBrowser starts to reset everything to
defaults.

| Section | What you can change |
|---|---|
| **Network** | Check for updates (now, and automatically at start), automatic (DHCP) or manual IP address, subnet mask, gateway, DNS server, time server, time zone and daylight saving. |
| **Controls** | Stick dead zone, pointer speed and acceleration, scroll speed, pointer snapping, rumble, the button for each action, on-screen key layout (QWERTY or ABC) and key repeat speed. |
| **Display** | Aspect ratio (4:3 or 16:9), video mode (Auto, 480i, 480p), picture width (Full 704 or Standard 640), screen margin, page text size, theme, animations, sound effects and volume. |
| **Music** | Open the music player, the instruments (a SoundFont from the `midi` folder, or built-in), music volume, shuffle and repeat. |
| **Browsing** | Home page, search engine, how the browser identifies itself, page styles, pictures on/off, picture size, image helper, picture labels, maximum page size. |
| **Security** | Cookies on/off, keep logins after restart, certificate checking and the certificate file. |
| **Data** | Exit button action and Swiss `.dol` file, clear cookies and logins, download folder, move data folder, delete zips after unzipping, clear history, delete bookmarks, reset all settings. |

A few worth knowing about:

- **Themes:** Indigo, Jet Black, Platinum and Spice, named after the console
  colours.
- **16:9** is for TVs set to stretch the picture to widescreen.
- **Picture size** (Small, Medium, Large) limits how tall photos can be, so
  news pages don't fill the screen with one photo at a time.
- **Image helper** (on by default) routes pictures through the free
  images.weserv.nl service, which shrinks them for your screen and converts
  formats the GameCube can't read. It makes pictures much faster and work on
  more sites, but that service sees the addresses of pictures you view.
- **Identify as: Lynx (text mode)** makes some sites send very simple pages
  that load faster.

---

## Sites that work well

Lightweight sites work best. All of these are linked from the start page:

| Site | Address |
|---|---|
| DuckDuckGo Lite (search) | `lite.duckduckgo.com/lite` |
| Wikipedia, mobile version | `en.m.wikipedia.org` |
| NPR Text (news) | `text.npr.org` |
| CNN Lite (news) | `lite.cnn.com` |
| 68k.news (headlines) | `68k.news` |
| FrogFind (search that simplifies pages) | `frogfind.com` |
| Wiby (search for classic-style sites) | `wiby.me` |

Older and hobbyist sites from the 1990s and 2000s usually display very well.

---

## Troubleshooting

| Problem | What to try |
|---|---|
| **"No network (error -1)"** | Check the Ethernet cable and that the adapter is seated firmly. If you use manual settings, check Settings > Network. Opening any page retries the connection. |
| **"Certificate couldn't be verified"** | Usually the clock: make sure the time server in Settings > Network is reachable. If it persists, replace `gcbrowser/cacert.pem` with a fresh copy from `https://curl.se/ca/cacert.pem`. |
| **A page is blank or nearly empty** | The site builds its content with JavaScript, which isn't supported. Try its mobile or "lite" version, or open it through FrogFind. |
| **A page looks cluttered** | Turn off Settings > Browsing > Page styles. |
| **Pictures don't appear** | Check Settings > Browsing > Load images, and leave Image helper on for best results. |
| **A page is cut off at the end** | Raise Settings > Browsing > Max page size. |
| **The screen edges are cut off** | Raise Settings > Display > Screen margin, or set Picture width to Standard (640). |
| **The picture looks stretched or squashed** | Match Settings > Display > Aspect ratio to your TV. |
| **A file link shows an error instead of downloading** | Some sites only allow downloads for signed-in accounts. If the site's login works without JavaScript, log in first; otherwise it isn't possible. |
| **A login doesn't stick** | Make sure Settings > Security > Cookies is on. Sites that sign in with JavaScript can't be logged into. |
| **A download stopped partway** | Choose Retry, or open the same link again; it continues where it stopped. |
| **"Not enough space on the SD card"** | Free up space. FAT32 cards can't hold single files over 4 GB. |
| **Exit doesn't find Swiss** | Use Settings > Data > Find .dol files on SD card and pick your Swiss `.dol`. |
| **Settings or bookmarks aren't saved** | The SD card wasn't found. Check it's in the SD2SP2 and formatted FAT32. |
| **The pointer drifts on its own** | Raise Settings > Controls > Stick dead zone. |
| **Something's set wrong and you can't get back** | Hold L + R + START while launching to reset all settings. |

### If it freezes or stutters

CubeBrowser protects itself against graphics lockups and keeps notes when
something goes wrong:

(These are in the `gcbrowser` data folder, wherever it is on your card.)

- `gcbrowser/freeze.log` is written if the screen stops updating for 10
  seconds.
- `gcbrowser/gpu-reset.log` is written if the graphics chip had to be reset (a
  brief pause instead of a freeze).

If you report a problem, please include these files and the page address.

---

## Limitations

- **No JavaScript.** Sites that depend on it may be blank or only partly work.
- **No modern multi-column layouts** (CSS flexbox and grid). Those sections are
  shown as a single column.
- **Logins that need JavaScript** (most big sites) don't work; classic login
  forms do.
- **No video, and no sound on web pages.** Animated GIFs show their first frame.
  (MIDI songs from your SD card play in the music player.)
- **Unzipping** supports standard `.zip` files; 7z, RAR and RVZ aren't
  supported.
- **Speed:** very heavy pages can take a few seconds to lay out, and each new
  secure site needs a second or two to connect.

## Security and privacy

- Secure sites are checked against the trusted certificate list on your SD
  card (TLS 1.2 and 1.3, via mbedTLS).
- The GameCube has no hardware random number generator, so the browser gathers
  randomness from a saved seed, processor timing and your input. That's fine
  for everyday browsing, but **don't log in to important accounts** (email,
  banking, anything sharing a password with other accounts).
- With **Keep logins after restart** on, cookies are saved in
  `gcbrowser/cookies.txt`. Anyone with your SD card could use those logins.
  Turn the setting off, or use Settings > Data > Clear cookies and logins, if
  that matters to you.
- Pictures sent through the image helper never carry your cookies, but
  images.weserv.nl does see the addresses of pictures you view.
- History is kept only in memory while the browser is running; bookmarks,
  settings and (optionally) cookies are stored on your SD card. Nothing is
  sent anywhere else.

---

## Credits

Built with [libogc2](https://libogc2.org/), [mbedTLS](https://github.com/Mbed-TLS/mbedtls),
[stb_image](https://github.com/nothings/stb) and the DejaVu fonts.
