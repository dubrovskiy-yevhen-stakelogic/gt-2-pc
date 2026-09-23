# Cockpit view - 0.5.0

The Driver camera has a three-dimensional cabin fitted inside the selected car's original body. Its hood, roof, pillars and window outlines retain the original geometry. Window openings follow the glass textures; the inner frames receive dark trim while the exterior keeps its paint and reflections. Cockpit view is enabled by default.

The dashboard, steering wheel, seats and lower door panels use shared procedural geometry. The speedometer and tachometer follow the live car. Every vehicle uses the same left-side driving layout; the dashboard is not a reconstruction of each car's real instruments. Basic Vice City interiors were studied as visual references, but no Vice City models, textures or installation are used.

## Controls

1. Select the **Driver** camera in a single-player race. **C** cycles Driver, Chase 1 and Chase 2; the assigned controller camera button works as before.
2. Open **Cockpit / driver view** with **F10**. In VR, open settings with **L3 + R3** or **both grips + Menu**.
3. Choose **Cockpit** or **Original**. Both chase cameras remain available.
4. Adjust **Seat height** from **-20 to +20 cm** and **Seat forward / back** from **-20 to +40 cm**, in 2 cm steps. Positive values raise the eye or move it farther from the wheel. VR orientation follows the headset; the desktop view is wider and aimed slightly downward to include the dashboard.
5. Turn **Steering wheel** off when using a physical wheel if it obstructs your view. This changes the decorative wheel only; steering input and force feedback remain active. In VR virtual-wheel mode, the interactive wheel takes its place.
6. Set **Rear-view mirror** to **OFF** to remove the cockpit mirror and its rear-view pass. **Mirror size** adjusts both dimensions from **25% to 100%**, in 5% steps, around its fitted centre. **HUD elements > Rear-view mirror** also controls its visibility.

Settings save automatically across Arcade and Simulation. **Reset cockpit settings** restores the enabled cabin, visible decorative wheel, enabled full-size mirror and zero seat adjustments. The instrument HUD starts **OFF** for new profiles; the physical dashboard instruments remain visible. Updates preserve saved preferences.

## Seating and visibility

The base seating position is 15 cm farther back than the initial cockpit, with both front seats and console accessories repositioned. The cushions are lowered by up to 15 cm and the backrests/headrests by 13 cm, within the available floor clearance. Seat lowering does not lower the camera. User seat offsets remain relative to this base, and the interactive VR wheel has its own reach calibration.

The instrument pod sits below the bonnet/cowl. The dashboard follows the curved windscreen base, with closed joins to the door trim. Door cards follow the lower edge of the actual side-window openings. Inner body lining, dashboard and door edges use a common neutral dark colour. The steering column reaches the wheel hub, including its saved height, distance and world scale.

Hands and the interactive wheel follow the body during suspension roll, pitch and impacts. Controller tracking and steering input remain in seated coordinates. The cabin, wheel and hands appear when the native countdown camera switches to Driver in its final second; vehicle movement begins at GO as before.

Window cutting preserves separate panes, pillars and opaque structural trim. Connected dark glass reflections are cleared, and exterior alpha decals over separately modelled glass are excluded from the inner windshield so they cannot become false black triangles. Roof paint and detached texture highlights do not create window holes. These operations prepare a cached cockpit mesh; source vehicle assets are unchanged.

## Mirror and rendering

The central mirror is a physical surface near the upper windshield with scene depth and stereo parallax. Its full-size frame is 25 cm wide and 7 cm high, fitted behind the windshield with a support to the roof.

One low-detail rear camera renders to a shared 480 x 128 texture, sampled in both eyes. There is no extra rear camera per eye, CPU readback or duplicate HUD mirror in cockpit mode. Reducing the visible mirror size keeps that pass; switching it off removes the pass. It is available in cockpit races whose original mode suppressed the HUD mirror, so those modes gain a rear-view pass when it is enabled.

The exterior retains its additive environment-map reflections using the existing environment texture, without another scene capture. Interior lining and window openings are excluded from that reflection draw.

Side-mirror views are not implemented. The central mirror remains a fixed rear camera rather than a physically traced reflection.

## Scope and validation

- The cockpit applies to live single-player driving, including look-back. Replays, the external starting flythrough and two-player split-screen retain their existing views.
- Fitting follows the available low-polygon body geometry. It cannot add detail absent from the original model, and unusual bodies or extreme seat positions can still need adjustment.
- Cockpit settings change presentation; vehicle simulation and save formats remain unchanged.
- The project author accepted this cockpit revision on Quest after the Lancer windshield fix. The corpus audit covers 1096 models at the first paint, default seat and four fixed views. It is not an in-headset test of every model or head pose.
- Desktop and OpenXR simulator checks do not establish PCVR comfort, physical-wheel alignment or sustained headset performance. See [validation](VALIDATION.md).

## Developer captures

`tests/check_cockpit.py` prepares isolated settings and a finite capture plan for Viper GTS, Pajero Mini, Mini Cooper, Skyline GT-R and Lotus Elise data from the installed Arcade disc. It covers cockpit on/off, the camera cycle, look-back, steering animation and wheel visibility. With `--with-xr`, it also captures both eyes while turning, leaning and looking down in the local OpenXR simulator. The script requires Python with Pillow and does not download tools or launch the game unless `--run` is given.

```powershell
python tests/check_cockpit.py --game build_update/gt2game.exe --disc runtime/arcade --output work/cockpit-check --with-xr --run
```

The output folder must be new. Settings, logs and screenshots stay there; existing game saves are not used. `report.json` records process/image checks and `gallery.html` groups the captures for visual inspection. Successful image and submission checks still require inspection of the actual cabin, hood and dashboard.

`gt2assetchecks --cockpits runtime/simulation work/cockpit-corpus.csv` checks the fitted body meshes against all suitable cars in a local disc and reports window counts, vertex budgets and forward visibility. These geometric checks complement the captures; they do not establish every car's appearance or comfort.

`tests/run_cockpit_audit.py runtime/simulation work/cockpit-audit` runs the Windows `gt2cockpitaudit` tool against every suitable model, rendering its front, left, right and seat views. A checker background exposes blocked openings without a track. `tests/cockpit_audit_sheets.py work/cockpit-audit` groups these images for visual review. The audit records its executable hash and uses the first paint, a fixed default seat and four viewing directions; it is a geometry review, not a headset or performance benchmark.

`tests/check_cockpit_countdown.py` checks the native camera cut before GO using finite cockpit-on/off desktop and XR runs. The mirror cases in `tests/check_cockpit.py` also compare CSV draw counts at 100%, 25% and OFF, including the saved cockpit mirror preferences.
