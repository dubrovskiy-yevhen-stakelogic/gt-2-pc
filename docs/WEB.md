# Browser edition

The browser edition compiles the shared C++ game to WebAssembly with WebGL2.
Arcade, Simulation, vehicle physics, AI, menus and saves use the same game source
as the native platforms. Rebuild each target to distribute shared code fixes.

## Play

Open the hosted page, select your own supported full 2352-byte-sector BIN image
and press **Start**. The disc stays on your device; it is not uploaded. BIOS,
disc images and game saves are not bundled. A 2048-byte ISO cannot be used.

Use a current desktop browser with WebGL2 and sufficient memory for the complete
disc image. The GPU must support textures at least 8192 pixels wide and 512 array
texture layers. BIN files larger than 1 GiB are rejected. Mobile compatibility
depends on available memory and graphics limits.

## Controls

- Arrows / D-pad: menu navigation; Enter selects and Space goes back.
- Up: accelerate. Down: brake, then reverse after stopping in automatic transmission.
- Left / Right: steer. Space: handbrake. C: camera. Esc: pause.
- Shift+Q: settings. Desktop render scale changes in 5% steps from 50% to 200%.
- A browser-supported gamepad can be used through the browser Gamepad API.

Brake-to-reverse can be disabled in settings. Manual transmission retains its
explicit reverse binding. The browser runs in flat-screen mode; VR and
physical-wheel force feedback require native applications.

## Saves

Saves and settings are stored in IndexedDB for the current site address.
**Export saves** downloads a backup; **Import saves** restores it before starting
the game. Export before clearing browser data, switching browser profiles or
moving to another site address. Private browsing may discard stored data.

Only one disc is loaded per page session. Reload the page to change it or recover
from graphics-context loss. Save progress before reloading or closing the page.

## Deploy the included site

Upload the contents of the release's `web/` directory to a static HTTPS host.
Keep `index.html`, `launcher.js`, `gt2.js` and `gt2.wasm` together. Serve `.wasm`
as `application/wasm`. No application server or game assets are required.
Opening `index.html` through `file://` does not work.

For local use from the extracted release folder:

```sh
python web/serve-web.py --directory web --port 8080
```

Open `http://127.0.0.1:8080/`. Deploy all four application files together when
updating the site, and invalidate cached copies from the previous build.

## Build from source

Install and activate Emscripten SDK 6.0.10. The build uses SDL 2.32.10.
From the source folder on Windows:

```powershell
./scripts/build-web.ps1 -Emsdk C:/path/to/emsdk -Build build_web
./scripts/package-web.ps1 -Build build_web -Output dist/GT2-Web-0.8.0
```

The build generates a deployable site under `build_web/site`. The packaging
script includes the site, required license notices and the local HTTP server.
