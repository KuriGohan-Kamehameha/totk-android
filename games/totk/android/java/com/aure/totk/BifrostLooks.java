package com.aure.totk;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** What the stick LEDs show for a game state, and when a command for it may be sent: everything of the Bifrost bridge
 *  that needs no Android class, so the host tests (tests/test_android_bifrost.py) run it on a plain JVM.
 *
 *  Bifrost (com.moonbench.bifrost) drives the AYN Thor's stick rings and takes commands from other apps as broadcasts
 *  (its INTEGRATING.md). This feed is modelled on the one its Fallout 4 Pip-Boy plugin gets from the patched companion
 *  app: one running PIPBOY effect (a phosphor breath) in the second screen's colour, re-sent every half second because
 *  Bifrost reverts an override whose owner is silent for 1.5 s, with the effect's own hints for what happens on the
 *  screen: a flicker flag, a burst time, and out-of-band pulses. The colour is given as the pure hue (white component
 *  removed), as that companion does. Brightness is Bifrost's business: the plugin bundle caps it and scales it with
 *  the second screen's brightness (LivePolicy), so the intensity below only counts when no plugin is installed.
 *
 *  TOTK_BIFROST=2 (or a Bifrost that refuses PIPBOY) uses the effects of the public guide instead: CLASSIC below. */
final class BifrostLooks {
    private BifrostLooks() { }

    // ---- Bifrost's contract (external/ExternalApi.kt; the values are pinned by its ExternalApiContractTest) ----
    static final String PACKAGE = "com.moonbench.bifrost", RECEIVER = "com.moonbench.bifrost.external.ExternalApiReceiver";
    static final String ACTION_DISPLAY = "com.moonbench.bifrost.api.ACTION_DISPLAY",
            ACTION_CLEAR = "com.moonbench.bifrost.api.ACTION_CLEAR", ACTION_PULSE = "com.moonbench.bifrost.api.ACTION_PULSE";
    static final int API_VERSION = 1;
    static final int RESULT_ACCEPTED = 0, RESULT_DISABLED = -1, RESULT_VALIDATION = -3, RESULT_RATE_LIMITED = -5;
    /** The result an ordered broadcast keeps when no receiver set one: Bifrost did not get the command. */
    static final int RESULT_UNHANDLED = 7001;

    // ---- the looks: one table ----
    static final int TOTK = 0, BOTW = 1, BOTW_NO_SLATE = 2;        // palette (row)
    static final int BASE = 0, OPEN = 1, LOW = 2;                  // kind (column)

    static final class Look {
        final String name;
        /** 0xAARRGGBB, both sticks. */
        final int color;
        /** 0..255; replaced by the plugin's brightness policy when one is installed. */
        final int intensity;
        /** PIPBOY's CRT stutter. */
        final boolean flicker;
        final int priority;
        /** The same look with the public effects (TOTK_BIFROST=2). */
        final String classicEffect;
        final float classicSpeed;
        final int classicIntensity;
        Look(String name, int rgb, int intensity, boolean flicker, int priority, String classicEffect, float classicSpeed,
             int classicIntensity) {
            this.name = name; this.color = 0xff000000 | rgb; this.intensity = intensity; this.flicker = flicker;
            this.priority = priority; this.classicEffect = classicEffect; this.classicSpeed = classicSpeed;
            this.classicIntensity = classicIntensity;
        }
    }
    /** The theme accent as an LED hue: the white component removed and the rest scaled to full, as the Pip-Boy
     *  companion does with the game's HUD colour (a pastel reads as white on the rings). */
    static int hue(int rgb) {
        int r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255, min = Math.min(r, Math.min(g, b));
        r -= min; g -= min; b -= min;
        int max = Math.max(r, Math.max(g, b));
        if (max == 0) return 0xffffff;
        return (r * 255 / max) << 16 | (g * 255 / max) << 8 | (b * 255 / max);
    }
    private static final Look LOW_HEARTS = new Look("low-hearts", hue(0xff2a1a), 110, true, 80, "PULSE", 0.8f, 255);
    /** [palette][kind]. Base: the pad's colour as a pure hue. Open (pause menu up): the theme's light selection
     *  colour as it is, which is the same hue with more light. Low hearts: red with the stutter, on any palette.
     *  The app themes: Tears of the Kingdom teal 57dfbf and lime d4ea74, Breath of the Wild blue 66d8fa and a6ebff;
     *  ff9b3d is the Sheikah Slate's inactive orange. */
    static final Look[][] LOOKS = {
        /* TOTK          */ { new Look("purah-pad", hue(0x57dfbf), 77, false, 60, "BREATH", 0.10f, 150),
                              new Look("purah-pad-open", 0xd4ea74, 77, false, 60, "STATIC", 0.5f, 255), LOW_HEARTS },
        /* BOTW          */ { new Look("sheikah-slate", hue(0x66d8fa), 77, false, 60, "BREATH", 0.10f, 150),
                              new Look("sheikah-slate-open", 0xa6ebff, 77, false, 60, "STATIC", 0.5f, 255), LOW_HEARTS },
        /* BOTW_NO_SLATE */ { new Look("slate-inactive", hue(0xff9b3d), 77, false, 60, "BREATH", 0.10f, 150),
                              new Look("slate-inactive-open", 0xffb870, 77, false, 60, "STATIC", 0.5f, 255), LOW_HEARTS },
    };

    // ---- the game state a build can read (BifrostState, one per game) ----
    static final class State {
        /** A save is loaded and the world is running or paused in its own menu: not the title, not a loading screen. */
        boolean inGame;
        /** The pause menu (the pad or slate itself) is open. */
        boolean open;
        /** Breath of the Wild before the Sheikah Slate is taken: false. */
        boolean tablet = true;
        /** Quarter hearts; -1 when not read. */
        int life = -1, maxLife = -1;
        void clear() { inGame = false; open = false; tablet = true; life = -1; maxLife = -1; }
    }
    /** Low hearts start at one heart or a quarter of the maximum (rounded up to a quarter heart), whichever is more,
     *  and end one heart above that (or at full), so a quarter-heart tick at the edge does not toggle the look. */
    static int lowOn(int maxLife) { return Math.max(4, (maxLife + 3) / 4); }
    static int lowOff(int maxLife) { return Math.min(maxLife, lowOn(maxLife) + 4); }

    /** Tears of the Kingdom: the live companion snapshot (runtime/companion_snapshot.c, detail 0). */
    static boolean fromSnapshot(String json, State out) {
        out.clear();
        if (json == null || !"ready".equals(text(json, "status"))) return false;
        out.inGame = true;
        out.open = json.contains("\"pauseMenu\":true");
        out.life = (int)number(json, "life", -1);
        out.maxLife = (int)number(json, "maxLife", -1);
        return true;
    }
    private static String text(String json, String key) {
        String mark = "\"" + key + "\":\"";
        int at = json.indexOf(mark), end = at < 0 ? -1 : json.indexOf('"', at + mark.length());
        return end < 0 ? null : json.substring(at + mark.length(), end);
    }
    private static long number(String json, String key, long missing) {
        String mark = "\"" + key + "\":";
        int at = json.indexOf(mark);
        if (at < 0) return missing;
        int from = at + mark.length(), to = from;
        if (to < json.length() && json.charAt(to) == '-') to++;
        while (to < json.length() && Character.isDigit(json.charAt(to))) to++;
        try { return Long.parseLong(json.substring(from, to)); } catch (NumberFormatException none) { return missing; }
    }

    /** Breath of the Wild: the values duo_host keeps of what the title module published last (duo/duo_host.cpp
     *  LedNames, in this order), then the age of that sample in milliseconds. */
    static final int DUO_VALUES = 10;
    static boolean fromDuo(long[] v, State out) {
        out.clear();
        if (v == null || v.length < DUO_VALUES) return false;
        long valid = v[0], title = v[1], loading = v[2], menu = v[3], slate = v[4], healValid = v[5], life = v[6], max = v[7],
                age = v[9];
        if (age < 0 || age > 2000 || valid != 1 || title != 0 || loading != 0) return false;
        out.inGame = true;
        out.open = menu == 1;
        out.tablet = slate != 0;   // -1 (not read) counts as taken
        if (healValid == 1 && life >= 0 && max >= 4 && max <= 400) { out.life = (int)life; out.maxLife = (int)max; }
        return true;
    }

    // ---- commands ----
    static final class Command {
        final String action;
        /** Extra name to an Integer, Long, Float, Boolean or String: the type is the Intent extra's type. */
        final Map<String, Object> extras = new LinkedHashMap<>();
        /** Sent as an ordered broadcast, for Bifrost's result code. */
        final boolean wantsResult;
        Command(String action, boolean wantsResult) {
            this.action = action; this.wantsResult = wantsResult;
            extras.put("apiVersion", API_VERSION);
        }
        @Override public String toString() { return action.substring(action.lastIndexOf('.') + 1) + extras; }
    }

    /** Decides what to send. One instance per game activity; every call on one thread (or synchronized by the caller).
     *  Times are SystemClock.elapsedRealtime() milliseconds: Bifrost reads burstWallMs on that clock. */
    static final class Feed {
        /** How often poll is meant to be called. */
        static final long POLL_MS = 125;
        /** A look is shown once this many polls in a row ask for it. */
        static final int DEBOUNCE_POLLS = 2;
        /** Re-send: Bifrost's lease is 1500 ms (LEDService.EXTERNAL_LEASE_TIMEOUT_MS). */
        static final long HEARTBEAT_MS = 500;
        /** Between two rate-limited commands, and how many of them any one second may hold (Bifrost: 4 a second
         *  refilled, 8 in a burst, for all callers together). A clear may use the one slot displays leave free. */
        static final long GAP_MS = 300;
        static final int DISPLAYS_PER_SECOND = 3, COMMANDS_PER_SECOND = 4;
        /** ACTION_PULSE is not rate limited by Bifrost; this is ours. */
        static final long PULSE_GAP_MS = 200;
        /** After a refusal: the switch is off in Bifrost, or Bifrost did not answer. */
        static final long MUTE_MS = 30_000, RATE_MUTE_MS = 2_000;
        /** Bifrost starts the effect a moment after the first command: the entry scramble waits for it. */
        static final long ENTRY_PULSE_DELAY_MS = 600;

        private final int game;
        private boolean classic;
        private Look shown, candidate;
        private int candidateCount;
        private boolean low, wasOpen, sawNoGame, dirty, active;
        private int lastLife = -1;
        private long epoch = -1, lastDisplay = Long.MIN_VALUE / 2, lastPulse = Long.MIN_VALUE / 2, burstAt, mutedUntil = Long.MIN_VALUE;
        private long entryPulseAt = -1;
        private final long[] sent = new long[COMMANDS_PER_SECOND];   // times of the last rate-limited commands
        private int sentCount;
        private String muteReason = "";

        /** game: TOTK or BOTW. classic: the public effects instead of PIPBOY. */
        Feed(int game, boolean classic) { this.game = game; this.classic = classic; }

        boolean classic() { return classic; }
        Look shown() { return shown; }
        /** Why nothing is sent for now ("" when sending): for one log line per change. */
        String muteReason(long now) { return now < mutedUntil ? muteReason : ""; }

        Look want(State s) {
            if (s == null || !s.inGame) { low = false; return null; }
            if (s.life >= 0 && s.maxLife >= 4) {
                if (s.life <= lowOn(s.maxLife)) low = true;
                else if (s.life >= lowOff(s.maxLife)) low = false;
            }
            Look[] row = LOOKS[game == BOTW && !s.tablet ? BOTW_NO_SLATE : game];
            return row[low ? LOW : s.open ? OPEN : BASE];
        }

        /** Every POLL_MS while the game is in the foreground; s == null when there is no state to read. */
        List<Command> poll(long now, State s) {
            List<Command> out = new ArrayList<>(2);
            if (epoch < 0) epoch = now;
            Look want = want(s);
            if (want == null) sawNoGame = true;
            if (want != candidate) { candidate = want; candidateCount = 1; } else if (candidateCount < DEBOUNCE_POLLS) candidateCount++;
            boolean pulse = false;
            if (candidate != shown && candidateCount >= DEBOUNCE_POLLS) {
                boolean entering = shown == null && sawNoGame;
                shown = candidate; dirty = true;
                if (shown != null) {
                    boolean open = s.open;
                    if (entering) entryPulseAt = now + ENTRY_PULSE_DELAY_MS;
                    else if (open != wasOpen) pulse = true;        // the pad was raised or put away
                    wasOpen = open; sawNoGame = false;
                }
            }
            // A hit: the burst hint, sent at once (its 150 ms envelope starts at the time it names).
            int life = s != null && s.inGame ? s.life : -1;
            if (!classic && shown != null && life >= 0 && lastLife >= 0 && life < lastLife) { burstAt = now; dirty = true; }
            lastLife = life;

            if (now < mutedUntil) return out;
            if (shown == null) {
                if (active && room(now, COMMANDS_PER_SECOND)) { out.add(clear(now)); }
                return out;
            }
            if ((dirty || now - lastDisplay >= HEARTBEAT_MS) && now - lastSent() >= GAP_MS && room(now, DISPLAYS_PER_SECOND)) {
                out.add(display(now));
            }
            if (!classic && active) {
                if (entryPulseAt >= 0 && now >= entryPulseAt) { entryPulseAt = -1; pulse(now, "STATIC", out); }
                else if (pulse) pulse(now, "PULSE", out);
            }
            return out;
        }

        /** The second screen was touched. */
        List<Command> touch(long now) {
            List<Command> out = new ArrayList<>(1);
            if (!classic && active && shown != null && now >= mutedUntil) pulse(now, "PULSE", out);
            return out;
        }

        /** The game paused, stopped or lost the foreground: give the LEDs back now. If this second is already full the
         *  clear is not sent and Bifrost's lease returns them within 1.5 s. */
        List<Command> stop(long now) {
            List<Command> out = new ArrayList<>(1);
            shown = null; candidate = null; candidateCount = 0; low = false; lastLife = -1; sawNoGame = false; dirty = false;
            entryPulseAt = -1;
            if (active && room(now, COMMANDS_PER_SECOND)) out.add(clear(now));
            active = false;
            return out;
        }

        /** Bifrost's answer to a display command. */
        void result(long now, int code) {
            if (code == RESULT_ACCEPTED) return;
            if (code == RESULT_VALIDATION && !classic) { classic = true; dirty = true; return; }   // no PIPBOY there
            active = false; dirty = true;
            if (code == RESULT_RATE_LIMITED) { mutedUntil = now + RATE_MUTE_MS; muteReason = "Bifrost is rate limiting"; }
            else {
                mutedUntil = now + MUTE_MS;
                muteReason = code == RESULT_DISABLED ? "\"Allow third-party LED control\" is off in Bifrost"
                        : code == RESULT_UNHANDLED ? "Bifrost did not take the command" : "Bifrost refused the command (" + code + ")";
            }
        }

        private long lastSent() { return sentCount == 0 ? Long.MIN_VALUE / 2 : sent[(sentCount - 1) % sent.length]; }
        /** Fewer than `limit` rate-limited commands in the second that ends now. */
        private boolean room(long now, int limit) {
            if (sentCount < limit) return true;
            return now - sent[(sentCount - limit) % sent.length] > 1000;
        }
        private void count(long now) { sent[sentCount % sent.length] = now; sentCount++; if (sentCount >= 2 * sent.length) sentCount -= sent.length; }

        private Command display(long now) {
            Look look = shown;
            Command c = new Command(ACTION_DISPLAY, true);
            c.extras.put("effect", classic ? look.classicEffect : "PIPBOY");
            c.extras.put("color", look.color);
            c.extras.put("colorRight", look.color);
            c.extras.put("intensity", classic ? look.classicIntensity : look.intensity);
            c.extras.put("intensityScale", 255);
            c.extras.put("speed", classic ? look.classicSpeed : 0.5f);
            c.extras.put("priority", look.priority);
            c.extras.put("until", "EXPLICIT_CLEAR");
            if (!classic) {
                c.extras.put("phaseSeconds", Math.max(0.001f, (now - epoch) / 1000f));
                c.extras.put("flickering", look.flicker);
                if (burstAt > 0) c.extras.put("burstWallMs", burstAt);
            }
            c.extras.put("requestId", look.name);
            count(now); lastDisplay = now; dirty = false; active = true;
            return c;
        }
        private Command clear(long now) {
            Command c = new Command(ACTION_CLEAR, false);
            c.extras.put("requestId", "clear");
            count(now); active = false; dirty = false;
            return c;
        }
        private void pulse(long now, String kind, List<Command> out) {
            if (now - lastPulse < PULSE_GAP_MS) return;
            Command c = new Command(ACTION_PULSE, false);
            c.extras.put("pulseKind", kind);
            lastPulse = now;
            out.add(c);
        }
    }
}
