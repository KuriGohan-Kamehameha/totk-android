# Bifrost external API v2: presets, reactive modes and functions on demand (2026-11-16)

Goal: let the game apps' LED bridge (`BifrostBridge.java`) hand the stick rings to a preset of the user's choice during
cutscenes, including Bifrost's screen- and sound-reactive modes, which the v1 API refuses (result -3).

**Code:** branch [`external-api-presets`](https://github.com/P1R4N351/Bifrost/tree/external-api-presets) on the
P1R4N351 fork of [Pollux-MoonBench/Bifrost](https://github.com/Pollux-MoonBench/Bifrost) (GPLv3), one commit on 1.3.1
(`1baddf1`). 82/82 unit tests pass (24 new). Not yet run on a device; no upstream pull request opened yet.

## Finding: Bifrost cannot tell callers apart (existing bug, v1)

Inside a manifest-registered `BroadcastReceiver`, `Binder.getCallingUid()` is Bifrost's own uid, so every external
caller looks like `com.moonbench.bifrost` (the maintainers' own comment in `LivePolicy.kt` notes this). Consequences
today: all callers share one rate-limit bucket, any app's `ACTION_CLEAR` can end any other app's override, and the
Fallout mirror-mode check (which expects the Fallout package) probably never fires (unconfirmed on a device).
v2 identifies callers by a caller-created `PendingIntent` (`callerToken`, Bifrost reads only its creator package) or
Android 14 share-identity.

## The v2 API

`API_VERSION = 2`, `MIN_API_VERSION = 1`. v1 callers get exactly the old behaviour; an older Bifrost answers -2 to
`apiVersion = 2`, which is how callers detect support.

| Action | Extras | Effect |
|---|---|---|
| `ACTION_APPLY_PRESET` | `presetName`, `priority`, one of `durationMs` / `until` / `indefinite`, optional `intensity` | Runs a saved preset (reactive ones included) as the caller's override; on its end Bifrost returns to what ran before. Re-sending the same name renews the lease. |
| `ACTION_DISPLAY` (v2) | as v1 | Reactive effects allowed, subject to permission and capture |
| `ACTION_INVOKE` | `function` = `BRIGHTNESS` / `PULSE` / `PAUSE` / `RESUME` | Brightness of the caller's override, pulse, dark override, clear |
| `ACTION_QUERY` (ordered) | none | JSON: version, enabled, service running, verified caller, capture state, presets (name, effect, owner, allowed, available), effects, functions |

New result codes (v2 only): -7 not permitted, -8 needed capture unavailable, -9 LED service not running, 1 unknown
preset (or `BRIGHTNESS` with no own override). Rate limit (8 burst, 4/s) and the 1.5 s lease are unchanged; a verified
caller gets its own bucket.

**User control.** The existing "Allow third-party LED control" still gates everything. New: "Let apps start presets
and reactive modes" (off by default), or a per-app grant declared by a Plugin Store plugin:
`"externalAccess": {"<package>": {"presets": [...], "effects": [...]}}` in the `.bfplugin` manifest, removed with the
plugin. AMBIENT needs Bifrost's accessibility capture (or a MediaProjection token); AUDIO_REACTIVE and AMBIAURORA need
a MediaProjection consent granted since the service started; otherwise -8 and the LEDs do not change.

## Known weak points (for the upstream discussion)

1. A `PendingIntent` can be borrowed by an app with notification-listener access (worst case: choosing LED presets).
2. The permission is `normal` level and the new switch is global; the Plugin Store does not show what a plugin grants.
3. Revert now always restarts the resolved state when app profiles are on (before: only after PIPBOY), so a
   non-PIPBOY override cannot keep running; this changes behaviour for every override.
4. The receiver re-reads the preset list on each `ACTION_APPLY_PRESET` heartbeat (about once a second).
5. Each external start of an audio effect builds a new MediaProjection from the stored token, as app-profile switching
   already does; Android 14+ allows a token once.

## How the game bridge would use it

Create one `callerToken` and send it on every intent; `ACTION_QUERY` at start (fall back to v1 behaviour on -2/-1);
on a cutscene (the second screen's dim signal, see [led-stamina-cutscene.md](led-stamina-cutscene.md)) stop the
PIPBOY feed and send `ACTION_APPLY_PRESET` with the user's chosen preset every 500 ms until it ends; then resume the
PIPBOY `ACTION_DISPLAY`. On -7/-8/1 fall back to fading the bridge's own colours and `ACTION_CLEAR`.
