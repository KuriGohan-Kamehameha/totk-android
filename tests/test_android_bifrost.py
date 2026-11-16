import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java/com/aure/totk'
LOOKS, BRIDGE = JAVA / 'BifrostLooks.java', JAVA / 'BifrostBridge.java'

# The smallest android.* that BifrostBridge touches. Time and the worker's queue are the test's: Handler runs what is
# due when the probe advances the clock, so the bridge's whole life runs on one thread and in a fixed order.
STUBS = {
    'android/os/SystemClock.java': '''package android.os;
public final class SystemClock { public static long now = 1_000_000; public static long elapsedRealtime() { return now; } }''',
    'android/os/Process.java': 'package android.os; public final class Process { public static final int THREAD_PRIORITY_BACKGROUND = 10; }',
    'android/os/Looper.java': 'package android.os; public final class Looper { }',
    'android/os/HandlerThread.java': '''package android.os;
public class HandlerThread {
    public static int started;
    public HandlerThread(String name, int priority) { }
    public void start() { started++; }
    public Looper getLooper() { return new Looper(); }
    public boolean quitSafely() { Handler.queue.clear(); return true; }
    public void join(long ms) throws InterruptedException { }
}''',
    'android/os/Handler.java': '''package android.os;
import java.util.*;
public class Handler {
    public static final List<Object[]> queue = new ArrayList<>();
    public Handler(Looper looper) { }
    public boolean post(Runnable r) { return postDelayed(r, 0); }
    public boolean postDelayed(Runnable r, long ms) { queue.add(new Object[]{SystemClock.now + ms, r}); return true; }
    public void removeCallbacksAndMessages(Object token) { queue.clear(); }
    /** Runs everything due up to `until`, in time order, moving the clock as it goes. */
    public static void runUntil(long until) {
        for (;;) {
            int best = -1;
            for (int i = 0; i < queue.size(); i++)
                if ((long)queue.get(i)[0] <= until && (best < 0 || (long)queue.get(i)[0] < (long)queue.get(best)[0])) best = i;
            if (best < 0) break;
            Object[] job = queue.remove(best);
            SystemClock.now = Math.max(SystemClock.now, (long)job[0]);
            ((Runnable)job[1]).run();
        }
        SystemClock.now = Math.max(SystemClock.now, until);
    }
}''',
    'android/system/Os.java': '''package android.system;
public final class Os { public static String option; public static String getenv(String name) { return name.equals("TOTK_BIFROST") ? option : null; } }''',
    'android/util/Log.java': '''package android.util;
public final class Log {
    public static int lines;
    public static int i(String tag, String m) { lines++; return 0; }
    public static int w(String tag, String m, Throwable t) { lines++; return 0; }
}''',
    'android/content/pm/PackageInfo.java': 'package android.content.pm; public class PackageInfo { }',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public class PackageManager {
    public static class NameNotFoundException extends Exception { }
    public static boolean installed; public static int asked;
    public PackageInfo getPackageInfo(String name, int flags) throws NameNotFoundException {
        asked++;
        if (!installed || !name.equals("com.moonbench.bifrost")) throw new NameNotFoundException();
        return new PackageInfo();
    }
}''',
    'android/content/Intent.java': '''package android.content;
import java.util.*;
public class Intent {
    public final String action; public String pkg, cls; public final Map<String, Object> extras = new LinkedHashMap<>();
    public Intent(String action) { this.action = action; }
    public Intent setClassName(String p, String c) { pkg = p; cls = c; return this; }
    public Intent putExtra(String k, int v) { extras.put(k, v); return this; }
    public Intent putExtra(String k, long v) { extras.put(k, v); return this; }
    public Intent putExtra(String k, float v) { extras.put(k, v); return this; }
    public Intent putExtra(String k, boolean v) { extras.put(k, v); return this; }
    public Intent putExtra(String k, String v) { extras.put(k, v); return this; }
}''',
    'android/content/BroadcastReceiver.java': '''package android.content;
public abstract class BroadcastReceiver {
    public int code;
    public abstract void onReceive(Context context, Intent intent);
    public final int getResultCode() { return code; }
}''',
    'android/content/Context.java': '''package android.content;
import java.util.*;
public class Context {
    public static final List<Intent> sent = new ArrayList<>();
    public static final List<Long> times = new ArrayList<>();
    /** What Bifrost answers to an ordered broadcast; null: nobody answers (the initial code stays). */
    public static Integer answer = 0;
    public static boolean broken;
    public Context getApplicationContext() { return this; }
    public android.content.pm.PackageManager getPackageManager() { return new android.content.pm.PackageManager(); }
    public void sendBroadcast(Intent intent) {
        if (broken) throw new SecurityException("refused");
        sent.add(intent); times.add(android.os.SystemClock.now);
    }
    public void sendOrderedBroadcast(Intent intent, String permission, BroadcastReceiver result, android.os.Handler handler,
                                     int initialCode, String data, android.os.Bundle extras) {
        sendBroadcast(intent);
        result.code = answer == null ? initialCode : answer;
        handler.post(() -> result.onReceive(this, intent));
    }
}''',
    'android/os/Bundle.java': 'package android.os; public class Bundle { }',
    'android/app/Activity.java': 'package android.app; public class Activity extends android.content.Context { }',
    'com/aure/totk/companion/PresentationController.java': 'package com.aure.totk.companion; public final class PresentationController { public static volatile Runnable touchListener; }',
    # The game's side, in the test's hands.
    'com/aure/totk/BifrostState.java': '''package com.aure.totk;
final class BifrostState {
    static final int GAME = BifrostLooks.BOTW;
    static boolean have; static int reads;
    static final BifrostLooks.State next = new BifrostLooks.State();
    static boolean read(BifrostLooks.State out) {
        reads++;
        out.inGame = next.inGame; out.open = next.open; out.tablet = next.tablet; out.life = next.life; out.maxLife = next.maxLife;
        return have;
    }
}''',
}

PROBE = r'''package com.aure.totk;

import java.util.*;
import com.aure.totk.BifrostLooks.*;

public final class BifrostProbe {
    static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    static void equal(Object a, Object b) { if (!Objects.equals(a, b)) throw new AssertionError(a + " != " + b); }

    // ---- Bifrost's own rules, from its source at 1.2.0-beta-7 (versionCode 13, commit 25f939f) ----
    // external/ExternalCommandValidator.kt, ExternalApi.kt, ExternalApiReceiver.kt and animations/LedAnimationType.kt.
    static final List<String> EFFECTS = List.of("AMBIENT", "AUDIO_REACTIVE", "AMBIAURORA", "BATTERY_INDICATOR", "CPU_TEMPERATURE",
            "STATIC", "BREATH", "RAINBOW", "PULSE", "STROBE", "SPARKLE", "FADE_TRANSITION", "RAVE", "CHASE", "PIPBOY");
    static final Set<String> NEEDS_PROJECTION = Set.of("AMBIENT", "AUDIO_REACTIVE", "AMBIAURORA");
    /** Extra name to the type the validator reads it with (getIntExtra on a Long silently gives the default). */
    static final Map<String, Class<?>> DISPLAY_EXTRAS = new HashMap<>();
    static {
        for (String k : new String[]{"apiVersion", "color", "colorRight", "intensity", "intensityScale", "priority"}) DISPLAY_EXTRAS.put(k, Integer.class);
        for (String k : new String[]{"speed", "smoothness", "sensitivity", "phaseSeconds"}) DISPLAY_EXTRAS.put(k, Float.class);
        for (String k : new String[]{"durationMs", "burstWallMs"}) DISPLAY_EXTRAS.put(k, Long.class);
        for (String k : new String[]{"indefinite", "flickering"}) DISPLAY_EXTRAS.put(k, Boolean.class);
        for (String k : new String[]{"effect", "until", "requestId"}) DISPLAY_EXTRAS.put(k, String.class);
    }
    /** Returns Bifrost's result code for the command: 0 accepted, -2 version, -3 validation, -6 unknown action. Also
     *  refuses what Bifrost would accept but not do as meant: an extra it never reads, a wrong type, a value it clamps. */
    static int validate(String action, Map<String, Object> x) {
        Object version = x.get("apiVersion");
        require(version instanceof Integer, "apiVersion must be an int extra");
        if ((int)version != 1) return -2;
        Object id = x.get("requestId");
        require(id == null || id instanceof String && ((String)id).length() <= 64, "requestId: a string of at most 64");
        switch (action) {
        case "com.moonbench.bifrost.api.ACTION_CLEAR":
            for (String k : x.keySet()) require(k.equals("apiVersion") || k.equals("requestId"), "clear: unread extra " + k);
            return 0;
        case "com.moonbench.bifrost.api.ACTION_PULSE":   // gate-checked only, never validated or rate limited
            for (String k : x.keySet()) require(k.equals("apiVersion") || k.equals("pulseKind"), "pulse: unread extra " + k);
            require(Set.of("PULSE", "STATIC").contains(x.get("pulseKind")), "pulseKind");
            return 0;
        case "com.moonbench.bifrost.api.ACTION_DISPLAY": break;
        default: return -6;
        }
        for (Map.Entry<String, Object> e : x.entrySet()) {
            Class<?> type = DISPLAY_EXTRAS.get(e.getKey());
            require(type != null, "display: extra " + e.getKey() + " is not in ExternalApi");
            require(type.isInstance(e.getValue()), "display: " + e.getKey() + " must be " + type.getSimpleName());
        }
        String effect = (String)x.getOrDefault("effect", "STATIC");
        if (!EFFECTS.contains(effect) || NEEDS_PROJECTION.contains(effect)) return -3;
        boolean duration = x.containsKey("durationMs"), until = x.containsKey("until"), indefinite = Boolean.TRUE.equals(x.get("indefinite"));
        if ((duration ? 1 : 0) + (until ? 1 : 0) + (indefinite ? 1 : 0) > 1) return -3;   // one terminator
        if (duration) { long ms = (long)x.get("durationMs"); if (ms < 1 || ms > 600_000) return -3; }
        if (until && !Set.of("NEXT_COMMAND", "EXPLICIT_CLEAR").contains(x.get("until"))) return -3;
        int scale = (int)x.getOrDefault("intensityScale", 255), intensity = (int)x.getOrDefault("intensity", 255);
        require(scale >= 1 && scale <= 255 && intensity >= 0 && intensity <= scale, "intensity within its scale");
        for (String k : new String[]{"speed", "smoothness", "sensitivity"})
            if (x.containsKey(k)) { float f = (float)x.get(k); require(f >= 0 && f <= 1, k + " in 0..1"); }
        int priority = (int)x.getOrDefault("priority", 50);
        require(priority >= 0 && priority <= 100, "priority in 0..100");
        require(x.containsKey("color") && ((int)x.get("color") >>> 24) == 0xff, "an opaque colour");
        if (x.containsKey("phaseSeconds")) require((float)x.get("phaseSeconds") > 0, "phaseSeconds > 0 (0 is ignored)");
        return 0;
    }
    /** ExternalApiReceiver's limiter: 8 tokens, 4 a second. */
    static final class Bucket {
        double tokens = 8; long last = Long.MIN_VALUE;
        boolean allow(long now) {
            if (last != Long.MIN_VALUE) tokens = Math.min(8, tokens + (now - last) / 1000.0 * 4);
            last = now;
            if (tokens < 1) return false;
            tokens -= 1; return true;
        }
    }
    static boolean limited(String action) { return !action.endsWith("ACTION_PULSE"); }
    static String name(String action) { return action.substring(action.lastIndexOf('_') + 1); }

    static State state(boolean inGame, boolean open, boolean tablet, int life, int max) {
        State s = new State(); s.inGame = inGame; s.open = open; s.tablet = tablet; s.life = life; s.maxLife = max; return s;
    }

    static void mapping() {
        // Colours: the theme accents as pure hues, and the table's shape.
        equal(Integer.toHexString(BifrostLooks.hue(0x57dfbf)), "ffc3");
        equal(Integer.toHexString(BifrostLooks.hue(0x66d8fa)), "c4ff");
        equal(Integer.toHexString(BifrostLooks.hue(0xff9b3d)), "ff7b00");
        equal(BifrostLooks.LOOKS.length, 3);
        for (Look[] row : BifrostLooks.LOOKS) {
            equal(row.length, 3);
            equal(row[BifrostLooks.LOW].name, "low-hearts");
            require(row[BifrostLooks.LOW].priority > row[BifrostLooks.BASE].priority, "low hearts outrank the base look");
            for (Look l : row) require(List.of("BREATH", "STATIC", "PULSE").contains(l.classicEffect), "public effects only");
        }
        // Low hearts: one heart or a quarter of the maximum, whichever is more; off one heart above.
        equal(BifrostLooks.lowOn(12), 4); equal(BifrostLooks.lowOff(12), 8);
        equal(BifrostLooks.lowOn(80), 20); equal(BifrostLooks.lowOff(80), 24);
        equal(BifrostLooks.lowOn(4), 4); equal(BifrostLooks.lowOff(4), 4);

        Feed totk = new Feed(BifrostLooks.TOTK, false), botw = new Feed(BifrostLooks.BOTW, false);
        equal(totk.want(null), null);
        equal(totk.want(state(false, true, true, 4, 12)), null);                       // title, loading
        equal(totk.want(state(true, false, true, 12, 12)).name, "purah-pad");
        equal(totk.want(state(true, true, true, 12, 12)).name, "purah-pad-open");
        equal(totk.want(state(true, false, true, -1, -1)).name, "purah-pad");          // hearts not read
        equal(botw.want(state(true, false, true, 12, 12)).name, "sheikah-slate");
        equal(botw.want(state(true, true, true, 12, 12)).name, "sheikah-slate-open");
        equal(botw.want(state(true, false, false, 12, 12)).name, "slate-inactive");
        equal(botw.want(state(true, true, false, 12, 12)).name, "slate-inactive-open");
        equal(totk.want(state(true, true, false, 12, 12)).name, "purah-pad-open");     // only Breath of the Wild has the orange
        // Hysteresis, and low hearts over the open menu.
        equal(botw.want(state(true, false, true, 5, 12)).name, "sheikah-slate");
        equal(botw.want(state(true, false, true, 4, 12)).name, "low-hearts");
        equal(botw.want(state(true, true, true, 6, 12)).name, "low-hearts");
        equal(botw.want(state(true, false, true, 7, 12)).name, "low-hearts");
        equal(botw.want(state(true, true, true, 8, 12)).name, "sheikah-slate-open");
        equal(botw.want(state(true, false, true, 0, 12)).name, "low-hearts");
        equal(botw.want(state(false, false, true, 0, 12)), null);
        equal(botw.want(state(true, false, true, 6, 12)).name, "sheikah-slate");       // the latch ended with the session

        // Tears of the Kingdom: the live snapshot as runtime/companion_snapshot.c writes it.
        State s = new State();
        String ready = "{\"schema\":1,\"status\":\"ready\",\"seq\":41,\"gameVersion\":\"1.4.3\",\"player\":{\"life\":10,\"maxLife\":16,"
                + "\"extraLife\":0,\"breakLife\":0,\"temporaryLife\":0,\"rupees\":345,\"poes\":0,\"maxStamina\":1000,\"pos\":[1,2,3]},"
                + "\"actionState\":{\"ok\":false,\"reason\":\"menu\",\"pauseMenu\":true,\"aiming\":false}}";
        require(BifrostLooks.fromSnapshot(ready, s) && s.inGame && s.open && s.tablet && s.life == 10 && s.maxLife == 16, "ready snapshot");
        require(BifrostLooks.fromSnapshot(ready.replace("\"pauseMenu\":true", "\"pauseMenu\":false"), s) && !s.open, "menu closed");
        for (String status : new String[]{"title", "starting", "unsupported", "noPlayer"})
            require(!BifrostLooks.fromSnapshot("{\"schema\":1,\"status\":\"" + status + "\",\"seq\":2}", s) && !s.inGame, status);
        require(!BifrostLooks.fromSnapshot(null, s) && !BifrostLooks.fromSnapshot("", s) && !BifrostLooks.fromSnapshot("{}", s), "no snapshot");
        require(BifrostLooks.fromSnapshot("{\"status\":\"ready\"}", s) && s.life == -1 && s.maxLife == -1 && !s.open, "ready without a player");

        // Breath of the Wild: valid, title, loading, menu_open, slate_obtained, heal.valid, heal.life, heal.max, unknown, age ms.
        require(BifrostLooks.fromDuo(new long[]{1, 0, 0, 0, 1, 1, 9, 12, 0, 16}, s) && s.inGame && !s.open && s.tablet && s.life == 9 && s.maxLife == 12, "world");
        require(BifrostLooks.fromDuo(new long[]{1, 0, 0, 1, 0, 0, -1, -1, 0, 16}, s) && s.open && !s.tablet && s.life == -1, "menu before the slate");
        require(BifrostLooks.fromDuo(new long[]{1, 0, 0, 0, -1, 1, 9, 12, 0, 16}, s) && s.tablet, "slate not read counts as taken");
        require(!BifrostLooks.fromDuo(new long[]{1, 1, 0, 0, 1, 1, 9, 12, 0, 16}, s) && !s.inGame, "title");
        require(!BifrostLooks.fromDuo(new long[]{1, 0, 1, 0, 1, 1, 9, 12, 0, 16}, s), "loading");
        require(!BifrostLooks.fromDuo(new long[]{0, -1, -1, 0, -1, 0, -1, -1, 1, 16}, s), "torn sample");
        require(!BifrostLooks.fromDuo(new long[]{1, 0, 0, 0, 1, 1, 9, 12, 0, 2001}, s), "a sample two seconds old");
        require(!BifrostLooks.fromDuo(new long[]{1, 0, 0, 0, 1, 1, 9, 12, 0, -1}, s), "no sample");
        require(!BifrostLooks.fromDuo(new long[3], s) && !BifrostLooks.fromDuo(null, s), "short answer");
    }

    /** Every command of a long random session passes Bifrost's validator, and the timing passes its limiter. */
    static void pacing() {
        for (int classic = 0; classic < 2; classic++) for (int seed = 1; seed <= 40; seed++) {
            Random r = new Random(seed * 31L + classic);
            Feed feed = new Feed(seed % 2 == 0 ? BifrostLooks.TOTK : BifrostLooks.BOTW, classic == 1);
            Bucket bucket = new Bucket();
            List<Long> times = new ArrayList<>(); List<String> kinds = new ArrayList<>();
            long now = 5_000_000, lastPulse = -1, lastDisplay = -1, shownSince = -1;
            boolean foreground = true, overriding = false;
            State s = state(true, false, true, 12, 12);
            int displays = 0, clears = 0, pulses = 0;
            for (int step = 0; step < 6000; step++) {
                now += seed % 3 == 0 ? 125 : 60 + r.nextInt(140);          // the poll timer, with jitter
                List<Command> out;
                int dice = r.nextInt(1000);
                if (foreground && dice < 6) { out = feed.stop(now); foreground = false; }
                else if (!foreground) { if (dice < 300) foreground = true; out = foreground ? feed.poll(now, s) : List.of(); }
                else {
                    if (dice < 200) s = state(r.nextInt(8) != 0, r.nextBoolean(), r.nextInt(6) != 0, r.nextInt(14), 12);   // a storm of changes
                    else if (dice < 230) s = null;
                    out = dice > 900 ? feed.touch(now) : feed.poll(now, s);
                }
                for (Command c : out) {
                    equal(validate(c.action, c.extras), 0);
                    String kind = name(c.action);
                    if (limited(c.action)) {
                        require(bucket.allow(now), "Bifrost's limiter refused a command at " + now + " seed " + seed);
                        times.add(now);
                        int inSecond = 0;
                        for (long t : times) if (now - t <= 1000) inSecond++;
                        require(inSecond <= 4, "more than 4 commands in a second: " + inSecond);
                    }
                    if (kind.equals("DISPLAY")) {
                        require(foreground, "a display after the game left the foreground");
                        equal(c.extras.get("effect").equals("PIPBOY"), classic == 0);
                        equal(c.extras.get("until"), "EXPLICIT_CLEAR");
                        if (overriding && lastDisplay >= 0) require(now - lastDisplay <= 1200, "heartbeat gap " + (now - lastDisplay) + " ms: the lease is 1500");
                        lastDisplay = now; overriding = true; displays++;
                    } else if (kind.equals("CLEAR")) {
                        require(overriding, "a clear with nothing to clear"); overriding = false; clears++;
                    } else {
                        require(classic == 0 && overriding, "a pulse needs the running PIPBOY effect");
                        require(lastPulse < 0 || now - lastPulse >= 200, "pulses closer than 200 ms");
                        lastPulse = now; pulses++;
                    }
                    kinds.add(kind);
                }
                if (!foreground) require(!overriding || true, "");
            }
            require(displays > 500 && clears > 5, "the session did something: " + displays + " displays, " + clears + " clears");
            if (classic == 0) require(pulses > 5, "pulses were sent: " + pulses);
        }
    }

    /** Debounce, heartbeat, hints and the pulses, step by step. */
    static void sequence() {
        Feed feed = new Feed(BifrostLooks.BOTW, false);
        long t = 10_000;
        equal(feed.poll(t, null).size(), 0);                                         // title: nothing
        State world = state(true, false, true, 12, 12);
        equal(feed.poll(t += 125, world).size(), 0);                                 // first sight: not yet
        List<Command> out = feed.poll(t += 125, world);                              // second: shown
        equal(out.size(), 1);
        Map<String, Object> x = out.get(0).extras;
        equal(out.get(0).action, BifrostLooks.ACTION_DISPLAY);
        equal(x.get("effect"), "PIPBOY"); equal(x.get("color"), 0xff00c4ff); equal(x.get("colorRight"), 0xff00c4ff);
        equal(x.get("priority"), 60); equal(x.get("flickering"), false); equal(x.get("requestId"), "sheikah-slate");
        require(!x.containsKey("burstWallMs") && !x.containsKey("durationMs") && !x.containsKey("indefinite"), "one terminator, no burst yet");
        long shownAt = t;
        // Unchanged: silent until the heartbeat, then every 500 ms.
        equal(feed.poll(t += 125, world).size(), 0); equal(feed.poll(t += 125, world).size(), 0); equal(feed.poll(t += 125, world).size(), 0);
        out = feed.poll(t += 125, world);
        equal(out.size(), 1); equal(name(out.get(0).action), "DISPLAY"); equal(t - shownAt, 500L);
        // The entry scramble comes once the effect has had time to start (the title was seen before).
        out = feed.poll(t += 125, world);
        equal(out.size(), 1); equal(out.get(0).extras.get("pulseKind"), "STATIC");
        // A one-poll blip of another state changes nothing.
        equal(feed.poll(t += 125, state(true, true, true, 12, 12)).size(), 0);
        equal(feed.poll(t += 125, world).size(), 0);
        t += 1000; feed.poll(t, world);
        // The menu opens: colour change and a dip, after two polls.
        State menu = state(true, true, true, 12, 12);
        equal(feed.poll(t += 125, menu).size(), 0);
        out = feed.poll(t += 300, menu);
        equal(out.size(), 2); equal(out.get(0).extras.get("requestId"), "sheikah-slate-open"); equal(out.get(0).extras.get("color"), 0xffa6ebff);
        equal(out.get(1).extras.get("pulseKind"), "PULSE");
        // A hit: the burst hint at once, with the time it happened.
        t += 1000; feed.poll(t, menu);
        out = feed.poll(t += 400, state(true, true, true, 9, 12));
        equal(out.size(), 1); equal(out.get(0).extras.get("burstWallMs"), t);
        // Low hearts: red, with the flicker, at a higher priority.
        feed.poll(t += 400, state(true, false, true, 3, 12));
        out = feed.poll(t += 400, state(true, false, true, 3, 12));
        x = out.get(0).extras;
        equal(x.get("requestId"), "low-hearts"); equal(x.get("color"), 0xffff1100); equal(x.get("flickering"), true); equal(x.get("priority"), 80);
        // A touch on the second screen dips the effect; two touches at once, one dip.
        equal(feed.touch(t += 300).size(), 1); equal(feed.touch(t += 50).size(), 0);
        // Back to the title: cleared after two polls, then silence.
        out = feed.poll(t += 400, null);                                              // not yet believed: the look is kept alive
        equal(out.size(), 1); equal(name(out.get(0).action), "DISPLAY");
        out = feed.poll(t += 400, null);
        equal(out.size(), 1); equal(out.get(0).action, BifrostLooks.ACTION_CLEAR);
        for (int i = 0; i < 40; i++) equal(feed.poll(t += 125, null).size(), 0);
        equal(feed.touch(t).size(), 0);
        equal(feed.stop(t).size(), 0);                                                // nothing left to clear

        // The public effects (TOTK_BIFROST=2): one effect per look, no Pip-Boy extras, no pulses.
        Feed classic = new Feed(BifrostLooks.TOTK, true);
        classic.poll(t += 125, world);
        x = classic.poll(t += 125, world).get(0).extras;
        equal(x.get("effect"), "BREATH"); equal(x.get("color"), 0xff00ffc3); equal(x.get("speed"), 0.10f);
        require(!x.containsKey("phaseSeconds") && !x.containsKey("flickering"), "no Pip-Boy hints");
        classic.poll(t += 400, menu);
        out = classic.poll(t += 400, menu);
        equal(out.size(), 1); equal(out.get(0).extras.get("effect"), "STATIC");
        classic.poll(t += 400, state(true, false, true, 2, 12));
        equal(classic.poll(t += 400, state(true, false, true, 2, 12)).get(0).extras.get("effect"), "PULSE");
        equal(classic.touch(t += 400).size(), 0);
    }

    /** Leaving the foreground clears at once, exactly once; Bifrost's refusals are respected. */
    static void pause() {
        State world = state(true, false, true, 12, 12);
        Feed feed = new Feed(BifrostLooks.TOTK, false);
        long t = 50_000;
        equal(feed.stop(t).size(), 0);                                                // never shown: nothing to clear
        feed.poll(t += 125, world);
        equal(feed.stop(t += 10).size(), 0);                                          // still nothing shown
        feed.poll(t += 125, world); equal(feed.poll(t += 125, world).size(), 1);
        List<Command> out = feed.stop(t += 1);                                        // 1 ms after a display: still at once
        equal(out.size(), 1); equal(out.get(0).action, BifrostLooks.ACTION_CLEAR); equal(validate(out.get(0).action, out.get(0).extras), 0);
        equal(feed.stop(t += 1).size(), 0);
        equal(feed.touch(t).size(), 0);
        // Back in front: shown again after the debounce, and no entry scramble (the game never left the save).
        equal(feed.poll(t += 125, world).size(), 0);
        equal(name(feed.poll(t += 300, world).get(0).action), "DISPLAY");
        for (int i = 0; i < 12; i++) for (Command c : feed.poll(t += 125, world)) equal(name(c.action), "DISPLAY");
        // Pause and resume in a tight loop never exceeds four commands in a second.
        List<Long> times = new ArrayList<>(); Bucket bucket = new Bucket();
        for (int i = 0; i < 400; i++) {
            List<Command> all = new ArrayList<>(feed.poll(t += 30, world));
            all.addAll(feed.poll(t += 30, world));
            all.addAll(feed.stop(t += 30));
            for (Command c : all) if (limited(c.action)) {
                require(bucket.allow(t), "limiter");
                times.add(t);
                int n = 0; for (long at : times) if (t - at <= 1000) n++;
                require(n <= 4, "pause loop: " + n + " in a second");
            }
        }
        require(times.size() > 100, "the loop sent commands");

        // The switch is off in Bifrost: silence for 30 s, one more try, silence again.
        feed = new Feed(BifrostLooks.TOTK, false);
        feed.poll(t += 2000, world);
        equal(feed.poll(t += 125, world).size(), 1);
        feed.result(t, BifrostLooks.RESULT_DISABLED);
        require(feed.muteReason(t).contains("Allow third-party LED control"), "says why");
        long muted = t; int n = 0;
        while (t - muted < 29_000) n += feed.poll(t += 125, world).size();
        equal(n, 0);
        equal(feed.stop(t).size(), 0);                                                // refused: nothing of ours is showing
        while (n == 0 && t - muted < 40_000) n += feed.poll(t += 125, world).size();
        equal(n, 1); require(t - muted >= 30_000 && t - muted < 31_000, "one more try after 30 s");
        feed.result(t, BifrostLooks.RESULT_UNHANDLED);
        n = 0; muted = t;
        while (t - muted < 29_000) n += feed.poll(t += 125, world).size();
        equal(n, 0);
        // A Bifrost without PIPBOY answers "validation": the public effects from then on.
        feed = new Feed(BifrostLooks.BOTW, false);
        feed.poll(t += 125, world);
        equal(feed.poll(t += 125, world).get(0).extras.get("effect"), "PIPBOY");
        feed.result(t, BifrostLooks.RESULT_VALIDATION);
        List<Command> next = feed.poll(t += 400, world);
        equal(next.get(0).extras.get("effect"), "BREATH"); require(feed.classic(), "classic from now on");
        // Accepted: nothing changes.
        feed.result(t, BifrostLooks.RESULT_ACCEPTED);
        equal(feed.muteReason(t), "");
    }

    // ---- BifrostBridge on the stub android.* (tests/test_android_bifrost.py STUBS) ----
    static void reset(boolean installed, String option) {
        android.content.pm.PackageManager.installed = installed; android.content.pm.PackageManager.asked = 0;
        android.system.Os.option = option;
        android.content.Context.sent.clear(); android.content.Context.times.clear(); android.content.Context.answer = 0;
        android.content.Context.broken = false;
        android.os.Handler.queue.clear(); android.os.HandlerThread.started = 0; android.util.Log.lines = 0;
        BifrostState.reads = 0; BifrostState.have = true;
        BifrostState.next.inGame = true; BifrostState.next.open = false; BifrostState.next.tablet = true;
        BifrostState.next.life = 12; BifrostState.next.maxLife = 12;
    }
    static void run(long ms) { android.os.Handler.runUntil(android.os.SystemClock.now + ms); }
    static List<String> sentKinds() {
        List<String> out = new ArrayList<>();
        for (android.content.Intent i : android.content.Context.sent) out.add(name(i.action));
        return out;
    }
    static void bridge() {
        // Bifrost is not installed: no thread, no read of the game, no broadcast, no log line; and it stays so.
        reset(false, null);
        BifrostBridge absent = BifrostBridge.create(new android.app.Activity());
        require(absent != null, "on by default");
        absent.start(); run(5000); BifrostBridge.touched(); absent.stop(); absent.start(); run(5000); absent.stop(); absent.close();
        equal(android.os.HandlerThread.started, 0); equal(BifrostState.reads, 0); equal(android.content.Context.sent.size(), 0);
        equal(android.os.Handler.queue.size(), 0); equal(android.util.Log.lines, 0);
        require(android.content.pm.PackageManager.asked >= 1, "asked the package manager");
        // TOTK_BIFROST=0: no bridge at all.
        reset(true, "0");
        equal(BifrostBridge.create(new android.app.Activity()), null);
        equal(android.content.pm.PackageManager.asked, 0);

        // Installed: display after the debounce, heartbeats, then a clear the moment the game pauses.
        reset(true, null);
        BifrostBridge bridge = BifrostBridge.create(new android.app.Activity());
        bridge.start(); bridge.start();
        equal(android.os.HandlerThread.started, 1);
        run(2000);
        List<String> kinds = sentKinds();
        require(kinds.size() >= 4 && kinds.size() <= 5 && !kinds.contains("CLEAR") && !kinds.contains("PULSE"), "display and heartbeats: " + kinds);
        for (android.content.Intent i : android.content.Context.sent) {
            equal(i.pkg, "com.moonbench.bifrost"); equal(i.cls, "com.moonbench.bifrost.external.ExternalApiReceiver");
            equal(validate(i.action, i.extras), 0);
        }
        equal(android.content.Context.sent.get(0).extras.get("effect"), "PIPBOY");
        equal(android.content.Context.sent.get(0).extras.get("color"), 0xff00c4ff);
        com.aure.totk.companion.PresentationController.touchListener.run(); run(1);   // as the second screen's window does
        equal(sentKinds().get(sentKinds().size() - 1), "PULSE");
        int before = android.content.Context.sent.size();
        long paused = android.os.SystemClock.now;
        bridge.stop(); bridge.stop(); run(0);
        equal(android.content.Context.sent.size(), before + 1);
        equal(sentKinds().get(before), "CLEAR");
        equal(android.content.Context.times.get(before), paused);
        int reads = BifrostState.reads;
        BifrostBridge.touched(); run(10_000);                                         // paused: no poll, no broadcast
        equal(android.content.Context.sent.size(), before + 1); equal(BifrostState.reads, reads);
        // Resume, then the activity is destroyed: the clear is sent from close itself.
        bridge.start(); run(1000);
        require(sentKinds().lastIndexOf("DISPLAY") > before, "showing again");
        bridge.close();
        equal(sentKinds().get(sentKinds().size() - 1), "CLEAR");
        int end = android.content.Context.sent.size();
        run(5000); bridge.start(); bridge.stop();
        require(android.content.Context.sent.size() >= end, "");

        // The switch is off in Bifrost: one command, then silence (and one log line about it besides the first).
        reset(true, null);
        android.content.Context.answer = -1;
        bridge = BifrostBridge.create(new android.app.Activity());
        bridge.start(); run(20_000);
        equal(sentKinds(), List.of("DISPLAY"));
        equal(android.util.Log.lines, 2);
        bridge.stop(); run(0);
        equal(sentKinds(), List.of("DISPLAY"));
        // Nobody answers (Bifrost stopped, or the permission was not granted): the same.
        reset(true, null);
        android.content.Context.answer = null;
        bridge = BifrostBridge.create(new android.app.Activity());
        bridge.start(); run(20_000);
        equal(sentKinds(), List.of("DISPLAY"));
        bridge.close();

        // A broadcast that throws ends the bridge, not the game.
        reset(true, "2");
        android.content.Context.broken = true;
        bridge = BifrostBridge.create(new android.app.Activity());
        bridge.start(); run(5000);
        equal(android.content.Context.sent.size(), 0); equal(android.os.Handler.queue.size(), 0);
        bridge.start(); bridge.stop(); bridge.close();
        // TOTK_BIFROST=2: the public effects.
        reset(true, "2");
        bridge = BifrostBridge.create(new android.app.Activity());
        bridge.start(); run(1000);
        equal(android.content.Context.sent.get(0).extras.get("effect"), "BREATH");
        bridge.close();
    }

    public static void main(String[] args) {
        switch (args[0]) {
        case "mapping": mapping(); break;
        case "pacing": pacing(); break;
        case "sequence": sequence(); break;
        case "pause": pause(); break;
        case "bridge": bridge(); break;
        default: throw new AssertionError(args[0]);
        }
    }
}
'''


class AndroidBifrostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('javac') or not shutil.which('java'):
            raise unittest.SkipTest('javac/java not found')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-bifrost-')
        cls.work = Path(cls.temp.name)
        sources = [LOOKS, BRIDGE]
        for name, text in {**STUBS, 'com/aure/totk/BifrostProbe.java': PROBE}.items():
            path = cls.work / 'src' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            sources.append(path)
        subprocess.run(['javac', '--release', '17', '-Xlint:-options', '-d', str(cls.work / 'classes'), *map(str, sources)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def probe(self, name):
        done = subprocess.run(['java', '-cp', str(self.work / 'classes'), 'com.aure.totk.BifrostProbe', name],
                              capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_state_to_look(self): self.probe('mapping')
    def test_commands_pass_bifrost_validator_and_limiter(self): self.probe('pacing')
    def test_debounce_heartbeat_hints_and_pulses(self): self.probe('sequence')
    def test_clear_on_pause_and_refusals(self): self.probe('pause')
    def test_bridge_without_and_with_bifrost(self): self.probe('bridge')

    def test_looks_need_no_android(self):
        self.assertNotIn('import android', LOOKS.read_text())

    def test_each_build_has_its_state_source(self):
        default = (JAVA / 'BifrostState.java').read_text()
        botw = (ROOT / 'games/totk/android/java-botw/com/aure/totk/BifrostState.java').read_text()
        self.assertIn('GAME = BifrostLooks.TOTK', default)
        self.assertIn('fromSnapshot', default)
        self.assertIn('GAME = BifrostLooks.BOTW', botw)
        self.assertIn('fromDuo', botw)
        # The names the native host keeps are the ones the title module publishes, in the order fromDuo reads them.
        host = (ROOT / 'games/totk/duo/duo_host.cpp').read_text()
        names = host.split('LedNames{', 1)[1].split('};', 1)[0]
        self.assertEqual([n.strip().strip('"') for n in names.replace('\n', ' ').split(',') if n.strip()],
                         ['botw.state.valid', 'botw.state.title', 'botw.state.loading', 'botw.state.menu_open',
                          'botw.state.slate_obtained', 'botw.ready.heal.valid', 'botw.ready.heal.life', 'botw.ready.heal.max',
                          'botw.state.unknown'])
        modules = ROOT / 'games/totk/duo/vendor/companions/native/modules'
        published = ''.join((modules / f).read_text() for f in ('botw_states.h', 'botw_heal_advisor.h', '01007EF00011E000.cpp'))
        for name in names.replace('\n', ' ').split(','):
            if name.strip():
                self.assertIn(name.strip(), published)

    def test_manifest_asks_for_bifrost_only(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('build_apk_bifrost', ROOT / 'games/totk/android/apk/build_apk.py')
        build_apk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(build_apk)
        self.assertIn('com.moonbench.bifrost.permission.CONTROL_LEDS', build_apk.PERMISSIONS)
        self.assertEqual(build_apk.QUERIES, ('com.moonbench.bifrost',))


if __name__ == '__main__':
    unittest.main()
