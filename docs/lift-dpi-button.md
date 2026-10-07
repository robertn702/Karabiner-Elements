# Logitech Lift for Mac DPI button as `button6` (ROB-319)

This fork-only change lets Karabiner-Core-Service report the small DPI (cursor speed) button behind the scroll wheel of a
Logitech Lift for Mac as `pointing_button: button6`.
Mapping `button6` to `right_control` gives hold-to-dictate for VoiceInk while **Modify events** stays enabled for the mouse
and all other mouse mappings keep working.

## How it works

- Karabiner-Core-Service already seizes the mouse, so a separate HID reader cannot see it.
  The change talks HID++ 2.0 to the device from the same process, using the IOHIDDevice that Karabiner already opened.
- Only a device with vendor ID `0x046D`, product ID `0xB031` (Lift for Mac over Bluetooth LE), which Karabiner is
  modifying, and whose device entry has `"logitech_lift_dpi_button_as_button6": true` is touched. It is off by default.
- After the device is grabbed, `ROOT.getFeature(0x1B04)` discovers the `REPROG_CONTROLS_V4` feature index (not hard-coded),
  then `setCidReporting` **temporarily** diverts control `0x00FD`. The firmware then stops changing the cursor speed and
  sends HID++ notifications instead. They become `button6` key_down/key_up events in the normal Karabiner pipeline
  (deduplicated, tracked like a physical button of the Lift).
- Temporary diversion is undone (`setCidReporting` with divert off) before Karabiner closes the device: when the device is
  ungrabbed (Modify events off, EventViewer "ignore all devices"), when the setting is turned off, and when
  Karabiner-Core-Service tears down for sleep, quit, or restart. On wake or reconnect the device is grabbed again and the
  button is diverted again.
- Held output is released through the paths Karabiner already uses for physical buttons: releasing the button, turning
  the setting off (an explicit `button6` key_up), or `device_ungrabbed` on disconnect/ungrab/teardown.
- If the device does not answer (3 attempts, 2 s each) or rejects a request, any divert request is undone immediately
  and the button is left to the firmware (cursor speed) until the next grab (reconnect, wake, Karabiner restart, or
  toggling the setting). Nothing is retried in a loop.
- Karabiner must be modifying the Lift (`"ignore": false`, the default for pointing devices is `true`); otherwise the
  flag has no effect.
- HID++ writes use synchronous `IOHIDDeviceSetReport` on Karabiner's HID run loop thread, as the hardware-proven prototype
  did. Replies are handled asynchronously; the event dispatcher never waits for a reply. Writes happen only at
  grab (2) and release (1), not per button press.

## Configuration

1. In `~/.config/karabiner/karabiner.json`, find the device entry of the Lift in the selected profile (it already exists
   if Modify events is enabled for the Lift) and add the flag:

    ```json
    {
        "identifiers": {
            "is_pointing_device": true,
            "product_id": 45105,
            "vendor_id": 1133
        },
        "ignore": false,
        "logitech_lift_dpi_button_as_button6": true
    }
    ```

    Keep the existing `identifiers` exactly as they are in your file; only add the last key.

2. Add a complex modification rule (Right Option is intentionally not used):

    ```json
    {
        "description": "Lift DPI button: hold Right Control (VoiceInk hold-to-dictate)",
        "manipulators": [
            {
                "type": "basic",
                "from": {
                    "pointing_button": "button6",
                    "modifiers": { "optional": ["any"] }
                },
                "to": [{ "key_code": "right_control" }],
                "conditions": [
                    {
                        "type": "device_if",
                        "identifiers": [{ "vendor_id": 1133, "product_id": 45105 }]
                    }
                ]
            }
        ]
    }
    ```

Without the rule, the DPI button acts as mouse button 6. To disable the feature, remove the flag or set it to `false`;
the button returns to changing cursor speed immediately.

Do not let SteerMouse or Logi Options+ also manage the DPI button: another HID++ client may undo the diversion.

## Build (no installation)

Requirements: Xcode, `xcodegen`, `cmake`, initialized submodules.
In the ROB-319 workspace they are workspace-local: `.scratch/venv/bin` (cmake, clang-format) and
`.scratch/tools/xcodegen/bin`.

```shell
git submodule update --init --recursive --depth 1
export PATH="$PWD/.scratch/venv/bin:$PWD/.scratch/tools/xcodegen/bin:$PATH"

# Tests (no signing needed)
make -C tests/src/lift_dpi_button
make -C tests/src/core_configuration
make -C tests

# Unsigned builds of the changed targets (no install)
export PQRS_ORG_CODE_SIGN_IDENTITY=dummy PQRS_ORG_INSTALLER_CODE_SIGN_IDENTITY=dummy
make -C src/core/CoreService
make -C src/lib/libkrbn
```

## Package for a hardware test

Background services only run when signed. This machine has Apple Development identities only (no Developer ID), which
the upstream README supports for local builds:

```shell
security find-identity -v -p codesigning   # pick the "Apple Development" hash
export PQRS_ORG_CODE_SIGN_IDENTITY=<hash>
export PQRS_ORG_INSTALLER_CODE_SIGN_IDENTITY=<hash>
make package                                # creates Karabiner-Elements-16.1.8.dmg
```

Important version caveat: this branch is based on the fork's `main` (16.1.8, July 2026). The installed Karabiner is
16.3.0, which moved `src/core/CoreService` to `src/apps/CoreService`, no longer uses
`pqrs::osx::iokit_hid_queue_value_monitor` in `entry.hpp`, and changed packaging. Installing this build is a downgrade.
A mechanical port check against `v16.3.0` showed that the new headers, `device.hpp`, and the tests apply cleanly, but
the `entry.hpp` wiring conflicts and needs manual adaptation. Decide between testing this 16.1.8-based build (run the
official uninstaller first so no 16.3.0 components remain) or porting the change to 16.3.0 before the hardware test.

## Install and rollback (manual, by Robert)

Before installing:

```shell
cp ~/.config/karabiner/karabiner.json ~/.config/karabiner/karabiner.json.before-rob-319
```

Install the built pkg from the dmg. Because the signer changes, macOS permissions must be granted again. Follow the
README's "Step 4" reset: disable the two Karabiner background services in System Settings, remove Accessibility, restart,
then open Karabiner-Elements and grant Input Monitoring, Accessibility, background services, and the driver extension
when prompted.

Rollback:

1. Fast: set `"logitech_lift_dpi_button_as_button6": false` (or remove it). The button is undiverted immediately.
2. If the button stays inert after a crash (diversion not undone), switch the mouse off and on or disconnect and
   reconnect it in Bluetooth settings; temporary diversion does not survive a reconnect.
3. Full: restore `karabiner.json.before-rob-319`, run the uninstaller (Karabiner-Elements Settings > Misc > Uninstall),
   and reinstall the official Karabiner-Elements dmg from https://karabiner-elements.pqrs.org/. Then repeat the
   permission reset for the official signer.

## Manual acceptance checklist

Run `tail -f /var/log/karabiner/core_service.log` while testing. Expected lines: `Lift DPI button: diverted
(REPROG_CONTROLS_V4 feature index 0x..)` after the Lift is grabbed. Warnings with `Lift DPI button:` describe failures.

- [ ] EventViewer shows `button6` down on press and up on release, once each.
- [ ] Hold DPI: VoiceInk records; release: it stops. No beep, no character typed.
- [ ] Ten quick press/release cycles: no stuck Right Control (check with EventViewer or by typing afterwards).
- [ ] Left/right/middle/back/forward buttons and scrolling behave as before.
- [ ] Existing held-right-click modifier shortcuts still work, including while DPI is not pressed.
- [ ] Hold DPI, click another button, keep holding: dictation continues; release: Right Control is released.
- [ ] Turn the Lift off while holding DPI: Right Control is released; turn it on: DPI works again.
- [ ] Sleep and wake the Mac: DPI works after wake.
- [ ] Set the flag to `false`: DPI changes cursor speed again; set it back to `true`: DPI is `button6` again.
- [ ] Turn off Modify events for the Lift: DPI changes cursor speed again.
- [ ] Quit Karabiner-Elements (or restart Karabiner-Core-Service): DPI changes cursor speed again.
