# Racing wheels, pedals and shifters - 0.4.0

Windows PC and PCVR share one wheel setup. Known USB devices configure themselves using an offline catalogue embedded in the game. No other racing game, profile download or account is required. Separate USB pedals and shifters can be combined with a wheel base from another manufacturer. Quest standalone USB wheels/FFB are not implemented.

Wheel driving and weak countersteering were tried with a **Fanatec Gran Turismo DD Pro (8 Nm)** and **Thrustmaster TH8A Shifter**. Other devices remain unverified. The catalogue contains 72 device records, including separate components and partial mappings; this does not mean 72 complete rigs have been tested. See [profile coverage and evidence](WHEEL-PROFILES.md).

## Connect and drive

1. Install the manufacturer's Windows driver, select PC mode and connect the wheel/pedals. Set your preferred rotation range in the driver's control panel. The game maps that range to the car's steering lock; it does not change the base's rotation setting.
2. Start the game. A recognized base and its pedals are assigned automatically. A single recognized USB pedal set takes priority over the base-connected pedals. A recognized USB H-shifter is assigned independently. Multiple eligible devices can be selected by name under **Devices and clutch**.
3. Open **F10 > Controls > Racing wheel / pedals / shifter** (or the normal VR settings shortcut). The wheel's menu button opens settings once mapped. The wheel D-pad, confirm/back buttons and paddles operate menus where a profile supplies those controls.
4. The main page shows live steering, accelerator and brake movement. Change **Gearbox**, **Force feedback** or **Force strength** directly. **Input** cycles between Off, Automatic and Custom with left/right; choosing Automatic rebuilds assignments from the catalogue. Existing manual settings are kept in Custom mode until you choose Automatic.

A two-pedal set works without configuring a clutch. If a third pedal is fitted, enable **Clutch pedal fitted** under **Devices and clutch**. Profiles with interchangeable-rim caveats offer **Wheel rim buttons**: select the documented layout or learn the buttons on your rim. An unrecognized device is not assigned by guessing its manufacturer's layout.

Settings save automatically and are shared by both discs. Disconnecting the wheel or selected pedals clears driving input instead of changing devices or keeping the accelerator held. Disconnecting a separate H-shifter keeps steering and pedals working and temporarily switches to sequential paddle controls. Its saved gear assignments are retained, including after restarting the game; reconnecting the same Windows instance restores H-pattern control. The wheel status displays this temporary fallback. If PC/compatibility mode changes the device identity, select Automatic again or choose the new device by name.

## When a control needs setup

Use **Guided setup / buttons** only for missing mappings or adjustments:

- **Steering / Accelerator / Brake / Clutch**: select the device, follow the centre/release and full-travel prompts, then check the live percentage and confirm to save. Steering records both endpoints. Back cancels without replacing your previous calibration.
- **H-pattern shifter**: select the USB device (or base if attached there), choose the number of forward gears, put the lever in neutral and confirm. The diagram highlights each requested gear. Select it, then return to neutral. Reverse is learned last. Confirm the completed result to save; cancelling keeps the previous bindings. The wizard expects one held button per gate and checks duplicates and neutral transitions.
- **Menu buttons**: choose an action and press its physical button. Confirm, back, directions, settings, tabs and the remaining PS1 buttons are available. Driving assignments are separate. Escape cancels a button capture.

The inspected Fanatec ClubSport USB Shifter reference has invalid fields, so this version uses the illustrated wizard for that device instead of installing an unverified mapping. The GT DD Pro base and base-connected pedals have a separate automatic profile.

**Advanced settings** retains individual driving-button assignments, inversion, dead zones, response curves, damping and force-direction adjustment. Changes there select Custom mode so the automatic profile does not overwrite calibration. Combined pedals can use one axis with a released centre and opposite pressed endpoints. Digital pedal buttons and analog handbrake travel are not currently exposed as analog pedal/handbrake bindings.

When enabled, the wheel owns the player's driving controls; gamepads and VR controllers can still operate menus. Select Off to use the existing gamepad/virtual-wheel path.

## Gearbox and clutch

- The race's **AT/MT** selection controls the transmission. **AT** changes forward gears automatically with either wheel paddles or an H-shifter connected. **MT controls** in wheel settings chooses the manual hardware layout; **Race default** uses paddles for MT. These settings no longer override the race's AT selection.
- With MT and paddles, one press selects one gear; release before shifting again. At rest, downshifting from first selects reverse; upshifting leaves reverse. A separate reverse button is also available. With AT, the same paddles select reverse from first and return from reverse to automatic forward gears; an H-shifter's reverse/forward gates can also select the direction.
- To leave reverse, brake to a stop, release the paddles, then press shift up. The game blocks forward/reverse changes while moving in the opposite direction above 0.5 m/s. **"Stop the car to change gear"** remains visible for four seconds after a blocked request, or until the gear change succeeds.
- With MT and H-pattern controls, the lever selects its gate directly. No active gate means neutral. Multiple simultaneous gates or a gear the car does not have also give neutral. Forward/reverse direction changes retain the same speed interlock.
- The clutch pedal progressively reduces torque through the clutch; fully depressed disconnects the engine. The original launch assistance is retained. The game does not simulate missed-shift damage or require the clutch to change gear.
- Wheel steering is linear and bypasses the gamepad steering curve and speed-dependent steering assist. Pedals bypass the original minimum pedal boost.
- Wheel races and ghosts record their gearbox and clutch state, with 12-bit steering, 10-bit accelerator/brake and 8-bit clutch travel. These wheel replay frames require version 0.4.0 or later; they are not compatible with the original PS1 game or 0.3.0. Original gamepad replay frames are unchanged.
- Replay and ghost buffers still have their original capacity. High-resolution wheel input can fill them sooner; a long recording may stop before the race or lap ends.

## Driving assists

Open **Racing wheel / pedals / shifter -> Driving assists**. The defaults are **Traction control 0/5**, **Countersteering assistance Weak**, and **Ignore gear-change speed Off**. Existing saved choices are retained. These options apply to live wheel input in both discs, single-player and player 1 of split-screen; they save with the wheel preferences.

- **Traction control 0..5** reduces accelerator input when a grounded driven tyre spins beyond its native peak-slip threshold. Higher levels intervene sooner and more strongly. Throttle returns progressively when grip recovers. Level 0 disables it.
- **Countersteering assistance Off / Weak / Strong** adds a limited, gradual steering correction when the moving car's rear slides out. It does not add grip or directly rotate the car. Reverse, neutral, handbrake driving and post-race AI control bypass both helpers.
- **Ignore gear-change speed Off / On** removes the forward/reverse speed interlock when On. It works with AT direction requests, MT paddles and H-pattern gates. The gear changes without first stopping; the car retains its motion and must still slow down before travelling in the new direction. Neutral, clutch operation, available gear counts and one-shift-per-paddle-press remain in effect. Off retains the 0.5 m/s interlock. The chosen policy is recorded with wheel input for deterministic replay.

This is our assistance implementation, not a reproduction of GT7's private algorithms. It makes wheel driving more accessible but cannot guarantee recovery from every spin. The existing GT2 tyre/vehicle physics remain in use. Wheel steering bypasses the original stick processing, which is why it can feel different from a gamepad even with identical vehicle physics. Assisted inputs are recorded; playback does not apply the assistance a second time.

With a wheel in **AT**, rolling backwards without throttle no longer selects reverse. Use an explicit reverse button/H gate, or the left paddle in first, to request reverse; the right paddle requests first again after stopping. Existing recordings retain their recorded transmission policy.

## Force feedback

Recognized wheels enable **Force feedback** when their driver advertises it; your explicit on/off choice is preserved. It can also be changed in the wheel submenu. The initial strength is **25%** and can be changed from 0 to 100%. The backend requires a DirectInput constant-force actuator on the assigned steering axis. Steering/pedals still work on devices without force feedback.

Feedback uses the moment from each front tyre: the native lateral force acts through a mechanical trail and a pneumatic trail computed with a brush contact model. The pneumatic trail shrinks as the patch slides, each unloaded wheel loses its contribution, and reverse travel swaps the pneumatic leading edge. Sideslip can therefore turn the released wheel away from centre. There is no added centre spring: a stationary wheel is not forcibly straightened just because a race starts. Damping opposes measured physical wheel motion and does not invert with **Invert aligning force**.

**Steering model** exposes saved parameters: wheel rotation (match the driver's total angle/SEN), mechanical trail, reference contact half-length and torque reference. Defaults are 900 degrees, 30 mm, 80 mm and 40 Nm. GT2 supplies loads, forces, tyre curves and road-wheel lock; its current port does not supply measured caster geometry or contact-patch dimensions for every car. The trail and patch defaults are generic model parameters, not recovered vehicle specifications. Torque reference is the calculated handwheel torque corresponding to maximum requested FFB, not the motor's advertised torque. Lowering it strengthens feedback and increases clipping; the existing FFB strength remains the output cap.

The model runs from the game's 30 Hz physics, with elapsed-time damping and output slew limiting. It does not reproduce detailed steering-linkage compliance, static tyre twist, caster jacking or power-assistance maps. See [steering model derivation and validation](formats/steering_feedback.md).

Forces stop in menus, on pause, focus loss, device loss and exit. Each driver effect expires after 200 ms unless the game refreshes it. Other programs holding exclusive access to the wheel can prevent FFB; close them and rescan. There is no motor test in the settings menu, so assigning axes cannot start an unexpected force effect.

## First hardware check

For a Fanatec Gran Turismo DD Pro with pedals connected to the base: the base/pedals should be automatic. If a separate USB shifter is not mapped automatically, run the H-pattern wizard. Check that the three main live indicators move correctly. For the first force-direction check, use a comfortable base torque limit and the initial 25% game strength. Check centring direction, release/reapply throttle, braking, neutral/reverse and pause. Repeat in PCVR with the same saved profile.

When reporting a problem, include device names from the menu, PC/compatibility mode, driver version, connection layout, the action that failed and `wheel-settings.txt` from the save root. The Windows console prints detected device IDs and whether the driver advertises FFB.

The save root also contains `input-diagnostics.log`. It records connection/fallback transitions, input or force-feedback calls taking at least 100 ms, and race frames taking at least 250 ms. Times are relative to each session start. A slow frame alone does not identify its cause; nested input/FFB timings help distinguish driver stalls from delays elsewhere. Menu/loading transitions may also produce slow frames.

Rendering diagnostics include scene preparation/submission calls over 50 ms, frame gaps over 100 ms, five-second frame statistics, and advancing physics without a submitted image. Desktop display-rate presentation uses a persistent deadline so an unlucky refresh phase cannot indefinitely suppress rendering. Texture decoding reads the existing CPU VRAM copy to avoid repeated slow reads from GPU upload memory.

Wheel races also log raw/mapped steering, physics steering, paddle requests, gear, speed, lateral motion, yaw, tyre forces and contact flags once per second and on input/gear/contact/jump events. These records distinguish an input spike from a slide or impact with a steady wheel.

Interactive flat mode starts in a borderless fullscreen window. Alt+Enter switches between fullscreen and windowed mode; `--windowed` or `--window WIDTHxHEIGHT` starts windowed. PCVR companion windows and automated checks retain their windowed presentation.
