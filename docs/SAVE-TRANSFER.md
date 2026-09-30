# PC and Quest save transfer

## Start over without reinstalling

On Quest standalone or PC, enter **Simulation > GT Mode**, open the GT2 overlay,
then choose **Campaign / saves > Start a new campaign**. The confirmation defaults
to **Cancel**. Select the hold row, release A/Enter, then hold it for three seconds.
Releasing the button, moving off the row or losing focus cancels the hold.

This resets the Simulation garage, credits, licences and records to a new game.
Controls, graphics settings, other files on the memory card and slot 2 are kept.
Simulation event unlock is switched off. After saving, continue to the title and
enter GT Mode to play the new campaign. The operation is unavailable during races
or in Arcade mode.

**Restore previous campaign** in the same menu recovers the progress from before
the most recent reset or restore, including progress that was not yet saved.
Restoring also backs up the campaign it replaces. A failed or damaged backup
prevents replacement. Numbered backups are retained beside the active card in
`card1.mcd.campaign-backups/`: `original.bin` is the exact old card when one existed,
and `campaign.sav` contains the live campaign at the time of the operation.
These local backups are not included in the USB transfer below; uninstalling the
Quest app can remove them along with its saves.

GT2 VR 0.3.0 can copy progress in either direction over USB, including from the non-debuggable public Quest APK. The two platforms use identical PS1 memory card images, so no conversion is required. Desktop and PCVR already share the same PC saves.

1. Save in the game's original Save Game menu, then fully close GT2 on both devices.
2. Connect Quest over USB with developer mode enabled and accept USB debugging.
3. From the extracted release run **TRANSFER_QUEST_SAVES_TO_PC.bat** or **TRANSFER_PC_SAVES_TO_QUEST.bat**.
4. Confirm the PC installation folder and transfer direction. The tool finds/downloads ADB, lists changed cards, then asks before replacing them.
5. Start the game and load the transferred save normally.

The public Quest app must be **0.3.0 or newer**. Older public versions do not expose their private saves; update with the same signing key first. Never uninstall the app to resolve a signing mismatch without separately preserving its saves.

The tool transfers `saves/arcade/card1.mcd` and `saves/simulation/card1.mcd`: each entire card includes its saved progress and replays. Missing source cards and identical cards are skipped. Empty or damaged source cards are refused. Custom `--card` / `--card2` paths are not part of this automatic transfer. Nothing is merged: the selected destination card is replaced by the source card.

Before replacing a card, the tool retains source and destination snapshots under the PC game's `save-backups/<timestamp-id>/`. Quest also retains the previous card internally. SHA-256 readback checks verify the copy. Uploads are staged and committed atomically; a running game or a destination changed after inspection causes rejection. A failure on the second disc does not undo a completed first-disc transfer; the output identifies each completed card.

Settings, HD assets, graphics preferences and installer data are not transferred. Use the same disc region on both devices: the tool preserves the card byte for byte and does not convert regional save names. Transfers between different US/European disc releases are not guaranteed to load.

```powershell
.\scripts\transfer-saves.ps1 -Direction QuestToPC -Runtime 'D:\Games\GT2' -Disc simulation
.\scripts\transfer-saves.ps1 -Direction PCToQuest -Runtime 'D:\Games\GT2' -Disc arcade -Serial YOUR_SERIAL
```

Use `-Adb <path>` to choose an existing ADB. `-Yes` accepts the displayed replacement plan non-interactively; validation and backups remain mandatory. Restore a PC backup by closing the game and copying the appropriate `*-destination.mcd` back to that disc's `card1.mcd`; then use the PC-to-Quest tool if restoring Quest. Keep the backup folder until you have loaded and checked the save in-game.
