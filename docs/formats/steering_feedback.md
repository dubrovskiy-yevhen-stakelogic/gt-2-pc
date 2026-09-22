# Steering feedback moment model

This native presentation/input extension consumes the existing car state without changing vehicle physics, tyre forces, handling or replay formats. It replaces the previous normalized lateral-force feedback and the rejected artificial centering spring.

## Inputs and units

`tools/gt2game/wheel_feedback.h` reads each front wheel's lateral force, load, contact velocity, steering angle and combined-slip lateral capacity. The cornering stiffness is taken from the first positive sample of the native slip-angle force curve. The maximum available factor is bounded by that curve's peak and the current `slipRatioGrip`.

The unit conversion follows the existing setup code: `body.mass = mappedWeight * 4`, `body.weight = mass * 9.8`, and each wheel's initial load is half the axle sprung weight plus the unsprung static load. Load and force values are divided by four for this model. Angles are converted with `2*pi/4096`; velocities with `1/4096`. These are interpretations of the current native port, not newly recovered disc facts.

## Uniform-pressure brush

For a patch extending from leading edge s=0 to trailing edge s=2a, lateral bristle shear grows with distance from the leading edge and is capped by available friction:

`q(s) = min(C_alpha * abs(tan(alpha)) * s / (2*a*a), lateral_capacity / (2*a))`.

The adhesive fraction is `u = min(1, lateral_capacity / (2*C_alpha*abs(tan(alpha))))`. Integrating shear and its moment about the patch centre gives:

`pneumatic_trail = a * (u/2 - u*u/3) / (1 - u/2)`.

At small slip, trail is a/3. It tends to zero as the adhesive region vanishes. This trail is multiplied by **GT2's actual lateral force**, preserving the game's force authority rather than introducing a second tyre force into handling. The reference half-length scales with sqrt(load/static_load), capped at twice the reference length. This contact-length law and the reference dimension are modeling assumptions.

This is an independently implemented uniform-pressure brush, not a fitted Pacejka model or the parabolic-pressure Fiala formulation. [MapleSim's tire-model documentation](https://www.maplesoft.com/support/help/MapleSim/view.aspx?path=TireComponentLibrary/FialaTire) provides background on slip, friction saturation and self-aligning moments; its implementation was not copied. Numerical quadrature of the shear distribution independently checks the closed-form trail in our tests.

## Steering geometry and output

For each front wheel, the mechanical contribution is `Fy * mechanical_trail`; the pneumatic contribution is `Fy * pneumatic_trail * sign(rolling_velocity)`. Reversing swaps the pneumatic patch's leading/trailing edges, while mechanical geometry stays fixed. The sum is divided by the steering ratio, computed from configured physical wheel half-travel divided by the car's road-wheel lock. GT2's positive lateral force maps to physical clockwise/right torque, verified through the native slip-angle/force routines in the tests. Axis calibration reversal is applied at hardware output.

Default assumptions are 900 degrees physical rotation, 30 mm effective mechanical trail, 80 mm reference patch half-length, and 40 Nm full-scale model torque. They are editable and saved in `steering_model`; they are **not measured parameters for each GT2 car**. The 40 Nm reference avoids flattening routine cornering torque at the device cap in the initial probe; exceptional loads can still clip. Motor strength remains independently limited by the saved FFB gain.

Damping uses physical axis movement before game steering deadzone/curve processing, measured over elapsed time. It is passive, independent of aligning-force inversion. The output slew limit uses elapsed time rather than a fixed increment per callback. Catch-up calls closer than 1 ms do not amplify derivatives; focus/device loss, menus, pause, disabled force and zero gain stop feedback. The existing 200 ms driver effect timeout remains.

## Validation and limits

Tests cover integrated brush shear, left/right sign, native countersteering and off-centre equilibrium, individual-wheel unloading, zero grip, load-dependent force reduction, saturation, reverse travel, steering ratio, invalid inputs and saved parameters. Diagnostic records expose per-wheel mechanical/pneumatic moments, trail, loads, handwheel moment and device output. `GT2_FFB_TRACE=1` also enables model diagnostics during hardware-free game probes; it never enables motors.

The source simulation still advances at 30 Hz. This extension does not supply measured caster/kingpin geometry, scrub-radius torque steer, static tyre twist, suspension jacking, steering-linkage compliance or vehicle-specific power steering. Hardware driving remains necessary to assess sign, strength, oscillation and steering return on a real base.
