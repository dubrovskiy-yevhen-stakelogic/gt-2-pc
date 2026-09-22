# Native wheel replay extension (0.4.0)

Original controller frames and their compressed runs keep the PS1 encoding. A frame with flags bit 7 set is a native wheel frame. In this mode all three driving axes are analogue, with no stick response curve or minimum pedal boost.

| Field | Native meaning |
|---|---|
| flags bits 0-6 | clutch bits 1-7 (255 = fully depressed) |
| buttons bits 0-3 | gear mode: 0 legacy automatic, 1 neutral, 2 reverse, 3-9 gears 1-7, 10 forward-only automatic, 11/12 unrestricted direct gear, 13 unrestricted sequential, 15 sequential |
| buttons bit 4 | handbrake |
| buttons bit 5 | clutch bit 0 |
| buttons bits 6-7 | sequential shift up/down; target bits for modes 11/12 |
| steer | upper 8 bits of the 12-bit steering position (2048 = centre) |
| throttle, brake | upper 4 bits of each 10-bit pedal position |
| wheelFine bits 0-3 | lower 4 steering bits |
| wheelFine bits 4-9 | lower 6 accelerator bits |
| wheelFine bits 10-15 | lower 6 brake bits |

A native run with header bit 6 set stores the usual packed pedal byte followed immediately by `wheelFine` as a little-endian uint16. A fine-bit change sets this header bit even if the packed pedal byte is unchanged. Transitioning from a PS1 frame to a wheel frame also writes the fine bits. Repeated frames retain their fine-bit state.

Live automatic wheel input uses mode 10 to prevent reverse selection while coasting backwards. Explicit reverse and forward requests use modes 2 and 3. Mode 0 retains the previous behavior for existing replays. Driving assists modify live input before recording; playback uses the recorded input without applying assistance again.

With Ignore gear-change speed enabled, direct requests use modes 11/12. The target gear is `(mode - 11) * 4 + (buttons >> 6)` (0 reverse, 1..7 forward); the paddle bits are unused by direct requests and carry the target instead. Mode 13 is sequential input with the speed interlock disabled. Decoding restores the ordinary gear mode plus PadRecord flag 0x200, propagated to GearRequest reserved[1] bit 6. Clutch, steering and pedal precision and the stream layout are unchanged. Existing recordings keep their interlock. New unrestricted frames require an updated 0.4.0 build.

The final two bytes of a native stream's capacity store the current fine-bit cache so that ghost lap buffers can reconstruct their stream object between ticks. The existing 17-byte stream-full margin leaves these bytes outside encoded runs. The used-byte counter does not include this cache. Original streams never write it.

Physics pad records retain their 12-byte layout. Flag `0x100` identifies wheel input; flags bits 4-7 carry the low clutch nibble, and the reserved byte carries the high clutch nibble plus the gear mode. Gear requests retain their four-byte layout, with native metadata in the two reserved bytes.

Wheel recordings require the native 0.4.0 reader. They cannot be replayed by the PS1 game or the 0.3.0 port. High-resolution input uses more of the fixed original replay/ghost capacity, so maximum recording length depends on how frequently the controls change.
