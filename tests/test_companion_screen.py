"""The second screen's shell contract (CompanionScreen): both games' screens behind one interface, the page round
trip, and the shell's pump that publishes the page to the common state and tells the screen what the game does.

The real CompanionView (Tears of the Kingdom), DuoSurfaceView (Breath of the Wild), the one CompanionContent (which
picks by GameBuild.COMPANION, generated from games/games.toml for each game) and PresentationController's pump run here
on a plain JVM over the smallest android.* that compiles them."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'games/totk/android'
JAVA = ANDROID / 'java/com/aure/totk/companion'
sys.path.insert(0, str(ROOT / 'tools'))
import games  # noqa: E402

# android.* and org.json as far as the two screens touch them. Nothing draws: the probe never attaches a view.
ANDROID_STUBS = {
    'android/content/Context.java': '''package android.content;
public class Context {
    public Context getApplicationContext() { return this; }
    public java.io.File getFilesDir() { return new java.io.File("."); }
    public android.content.res.AssetManager getAssets() { return new android.content.res.AssetManager(); }
    public android.content.pm.PackageManager getPackageManager() { return new android.content.pm.PackageManager(); }
    public String getPackageName() { return "probe"; }
}''',
    'android/content/pm/PackageInfo.java': 'package android.content.pm; public class PackageInfo { public long lastUpdateTime; public long getLongVersionCode() { return 1; } }',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public class PackageManager {
    public static class NameNotFoundException extends Exception { }
    public PackageInfo getPackageInfo(String name, int flags) throws NameNotFoundException { return new PackageInfo(); }
}''',
    'android/content/res/AssetManager.java': '''package android.content.res;
public class AssetManager {
    public String[] list(String path) { return new String[0]; }
    public java.io.InputStream open(String path) throws java.io.IOException { throw new java.io.IOException(path); }
}''',
    'android/content/res/Configuration.java': 'package android.content.res; public class Configuration { }',
    'android/content/res/Resources.java': 'package android.content.res; public class Resources { public android.util.DisplayMetrics getDisplayMetrics() { return new android.util.DisplayMetrics(); } }',
    'android/util/DisplayMetrics.java': 'package android.util; public class DisplayMetrics { public float density = 2, scaledDensity = 2; }',
    'android/util/Log.java': 'package android.util; public final class Log { public static int w(String t, String m) { return 0; } public static int w(String t, String m, Throwable e) { return 0; } }',
    'android/os/SystemClock.java': 'package android.os; public final class SystemClock { public static long now = 1_000_000; public static long uptimeMillis() { return now; } }',
    'android/graphics/PixelFormat.java': 'package android.graphics; public class PixelFormat { public static final int RGBA_8888 = 1; }',
    'android/graphics/Bitmap.java': 'package android.graphics; public class Bitmap { public int getWidth() { return 1; } public int getHeight() { return 1; } }',
    'android/graphics/Color.java': 'package android.graphics; public class Color { public static final int WHITE = -1; }',
    'android/graphics/ColorFilter.java': 'package android.graphics; public class ColorFilter { }',
    'android/graphics/PorterDuff.java': 'package android.graphics; public class PorterDuff { public enum Mode { SRC_IN } }',
    'android/graphics/PorterDuffColorFilter.java': 'package android.graphics; public class PorterDuffColorFilter extends ColorFilter { public PorterDuffColorFilter(int color, PorterDuff.Mode mode) { } }',
    'android/graphics/Typeface.java': 'package android.graphics; public class Typeface { }',
    'android/graphics/Path.java': '''package android.graphics;
public class Path {
    public void reset() { } public void moveTo(float x, float y) { } public void cubicTo(float a, float b, float c, float d, float e, float f) { }
}''',
    'android/graphics/RectF.java': '''package android.graphics;
public class RectF {
    public float left, top, right, bottom;
    public RectF() { }
    public RectF(float l, float t, float r, float b) { set(l, t, r, b); }
    public RectF(RectF o) { set(o.left, o.top, o.right, o.bottom); }
    public void set(float l, float t, float r, float b) { left = l; top = t; right = r; bottom = b; }
    public void set(RectF o) { set(o.left, o.top, o.right, o.bottom); }
    public float width() { return right - left; } public float height() { return bottom - top; }
    public float centerX() { return (left + right) / 2; } public float centerY() { return (top + bottom) / 2; }
    public boolean contains(float x, float y) { return x >= left && x < right && y >= top && y < bottom; }
    public void offset(float dx, float dy) { left += dx; right += dx; top += dy; bottom += dy; }
    public boolean intersect(RectF o) { return true; }
}''',
    'android/graphics/Paint.java': '''package android.graphics;
public class Paint {
    public static final int ANTI_ALIAS_FLAG = 1, FILTER_BITMAP_FLAG = 2;
    public enum Style { FILL, STROKE } public enum Align { LEFT, CENTER } public enum Cap { ROUND, BUTT }
    public Paint(int flags) { }
    public void setColor(int c) { } public void setStrokeWidth(float w) { } public void setStyle(Style s) { } public void setTextAlign(Align a) { }
    public void setStrokeCap(Cap c) { } public void setTextSize(float s) { } public Typeface setTypeface(Typeface t) { return t; }
    public ColorFilter setColorFilter(ColorFilter f) { return f; }
    public float measureText(String text) { return text.length(); }
    public int breakText(String text, boolean forward, float max, float[] measured) { return text.length(); }
}''',
    'android/graphics/Picture.java': '''package android.graphics;
public class Picture { public Canvas beginRecording(int w, int h) { return new Canvas(); } public void endRecording() { } }''',
    'android/graphics/Canvas.java': '''package android.graphics;
public class Canvas {
    public int save() { return 0; } public void restore() { } public void restoreToCount(int n) { }
    public int saveLayerAlpha(RectF b, int a) { return 0; } public int saveLayerAlpha(float l, float t, float r, float b, int a) { return 0; }
    public boolean clipRect(RectF r) { return true; } public boolean clipRect(float l, float t, float r, float b) { return true; }
    public void translate(float x, float y) { } public void scale(float x, float y, float px, float py) { }
    public void drawRect(RectF r, Paint p) { } public void drawRect(float l, float t, float r, float b, Paint p) { }
    public void drawLine(float a, float b, float c, float d, Paint p) { } public void drawText(String s, float x, float y, Paint p) { }
    public void drawBitmap(Bitmap b, android.graphics.Rect s, RectF d, Paint p) { } public void drawCircle(float x, float y, float r, Paint p) { }
    public void drawOval(RectF r, Paint p) { } public void drawArc(RectF r, float a, float b, boolean c, Paint p) { }
    public void drawPath(Path path, Paint p) { } public void drawPicture(Picture p) { }
}''',
    'android/graphics/Rect.java': 'package android.graphics; public class Rect { }',
    'android/view/MotionEvent.java': '''package android.view;
public class MotionEvent {
    public static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3, ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6;
    public int getActionMasked() { return 0; } public int getActionIndex() { return 0; } public int getPointerCount() { return 0; }
    public int getPointerId(int i) { return 0; } public float getX() { return 0; } public float getY() { return 0; }
    public float getX(int i) { return 0; } public float getY(int i) { return 0; }
}''',
    'android/view/ScaleGestureDetector.java': '''package android.view;
public class ScaleGestureDetector {
    public static class SimpleOnScaleGestureListener { public boolean onScale(ScaleGestureDetector detector) { return false; } }
    public ScaleGestureDetector(android.content.Context context, SimpleOnScaleGestureListener listener) { }
    public boolean onTouchEvent(MotionEvent event) { return true; } public boolean isInProgress() { return false; }
    public float getScaleFactor() { return 1; } public float getFocusX() { return 0; } public float getFocusY() { return 0; }
}''',
    'android/view/View.java': '''package android.view;
public class View {
    public static int invalidated;
    private final android.content.Context context;
    public View(android.content.Context context) { this.context = context; }
    public android.content.Context getContext() { return context; }
    public android.content.res.Resources getResources() { return new android.content.res.Resources(); }
    public void setBackgroundColor(int c) { } public void setFocusable(boolean f) { } public void setContentDescription(CharSequence d) { }
    public void invalidate() { invalidated++; } public void postInvalidateOnAnimation() { }
    public boolean postDelayed(Runnable r, long ms) { return true; } public boolean removeCallbacks(Runnable r) { return true; }
    public int getWidth() { return 0; } public int getHeight() { return 0; }
    public boolean performClick() { return true; } public boolean onTouchEvent(MotionEvent e) { return false; }
    protected void onAttachedToWindow() { } protected void onDetachedFromWindow() { } protected void onDraw(android.graphics.Canvas c) { }
    protected void onSizeChanged(int w, int h, int ow, int oh) { } protected void onConfigurationChanged(android.content.res.Configuration c) { }
}''',
    'android/view/Surface.java': 'package android.view; public class Surface { }',
    'android/view/SurfaceHolder.java': '''package android.view;
public interface SurfaceHolder {
    interface Callback { void surfaceCreated(SurfaceHolder h); void surfaceChanged(SurfaceHolder h, int f, int w, int hh); void surfaceDestroyed(SurfaceHolder h); }
    void setFormat(int format); void addCallback(Callback c); Surface getSurface();
}''',
    'android/view/SurfaceView.java': '''package android.view;
public class SurfaceView extends View {
    public SurfaceView(android.content.Context context) { super(context); }
    public void setZOrderOnTop(boolean top) { }
    public SurfaceHolder getHolder() {
        return new SurfaceHolder() {
            public void setFormat(int format) { } public void addCallback(Callback c) { } public Surface getSurface() { return new Surface(); }
        };
    }
}''',
    'org/json/JSONArray.java': '''package org.json;
public class JSONArray { public int length() { return 0; } public JSONObject optJSONObject(int i) { return null; } public Object opt(int i) { return null; } }''',
    'org/json/JSONObject.java': '''package org.json;
public class JSONObject {
    public boolean has(String k) { return false; } public boolean isNull(String k) { return true; }
    public int optInt(String k) { return 0; } public int optInt(String k, int d) { return d; } public long optLong(String k, long d) { return d; }
    public double optDouble(String k) { return Double.NaN; } public double optDouble(String k, double d) { return d; }
    public boolean optBoolean(String k) { return false; } public String optString(String k) { return ""; } public String optString(String k, String d) { return d; }
    public JSONObject optJSONObject(String k) { return null; } public JSONArray optJSONArray(String k) { return null; }
}''',
}

# The Tears of the Kingdom screen's own collaborators, as far as an unattached view touches them.
TOTK_STUBS = {
    'com/aure/totk/companion/AssetStore.java': '''package com.aure.totk.companion;
import android.graphics.*;
public final class AssetStore {
    public org.json.JSONObject index = new org.json.JSONObject();
    public AssetStore(android.content.Context context, Runnable changed) { }
    public static boolean available(android.content.Context context) { return true; }
    public static String humanize(String key) { return key; }
    public boolean decor(Canvas c, String name, RectF b, float u) { return false; } public boolean decor(Canvas c, String name, RectF b, float u, int o) { return false; }
    public boolean softDecor(Canvas c, String name, RectF b, float u) { return false; } public float decorAspect(String name, float d) { return d; }
    public int color(String name, int d) { return d; } public Bitmap ui(String name) { return null; } public Bitmap icon(String name) { return null; }
    public Bitmap pageIcon(String name) { return null; } public Typeface typeface(String text) { return null; }
    public String label(String g, String k, String d) { return d; } public String itemName(String a) { return a; } public String itemDescription(String a) { return ""; }
    public void close() { }
}''',
    'com/aure/totk/companion/MapRenderer.java': '''package com.aure.totk.companion;
final class MapRenderer {
    boolean following = true, automaticLayer = true; String layer = "Ground";
    MapRenderer(AssetStore assets, float unit) { }
    void setUnit(float u) { } void recenter() { } void pan(float dx, float dy) { } void scale(float f, float x, float y) { }
    void marker(double x, double z, double heading) { } void noMarker() { }
    void draw(android.graphics.Canvas c, android.graphics.RectF b, Snapshot data, boolean mini) { }
}''',
    'com/aure/totk/companion/Snapshot.java': '''package com.aure.totk.companion;
import org.json.*;
public final class Snapshot {
    public static final String[] CATEGORIES = {"weapons", "bows", "shields", "armor", "materials", "food", "zonai", "keyItems"};
    public final JSONObject root = new JSONObject(), player = new JSONObject();
    public String status = "starting";
    public static Snapshot waiting() { return new Snapshot(); }
    public static JSONObject object(JSONObject v, String k) { return new JSONObject(); } public static JSONArray array(JSONObject v, String k) { return new JSONArray(); }
    public JSONArray items(int category) { return new JSONArray(); } public String itemCategory(int category, int index) { return CATEGORIES[category]; }
    public double coordinate(int axis) { return Double.NaN; }
}''',
    'com/aure/totk/companion/CompanionNative.java':
        'package com.aure.totk.companion; public final class CompanionNative { public static boolean isEnabled() { return true; } }',
    'com/aure/totk/companion/SnapshotPoller.java': '''package com.aure.totk.companion;
public final class SnapshotPoller {
    public interface Listener { void changed(Snapshot snapshot, boolean stale); }
    public static int fullRequests;
    public final ItemActions actions = new ItemActions();
    public SnapshotPoller(DataSource source, Listener listener) { }
    public void start() { } public void stop() { } public void requestFull() { fullRequests++; } public void action(int kind, int arg) { }
}''',
}

SCREEN_PROBE = r'''package com.aure.totk.companion;

public final class ScreenProbe {
    static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    /** SnapshotPoller's request count (a stub of the Tears of the Kingdom build only, so by name). */
    static int fullRequests() throws ReflectiveOperationException {
        return Class.forName("com.aure.totk.companion.SnapshotPoller").getField("fullRequests").getInt(null);
    }
    public static void main(String[] args) throws ReflectiveOperationException {
        android.content.Context context = new android.content.Context();
        DataSource source = new DataSource() {
            @Override public String read(int detail) { return null; }
            @Override public boolean send(int kind, int arg) { return false; }
        };
        boolean botw = args[0].equals("botw");
        require(CompanionContent.TABLET_CAN_BE_MISSING == botw, "the build says whether its tablet can be missing");
        for (int initial = 0; initial < 3; initial++) {
            CompanionScreen screen = CompanionContent.create(context, source, initial);
            require(screen.view() == screen, "the screen is its own view");
            require(screen.getClass().getSimpleName().equals(botw ? "DuoSurfaceView" : "CompanionView"), "this build's screen");
            CompanionState state = new CompanionState();
            screen.onGameState(state);                                    // no state at all is a state a screen takes
            require(state.set(new long[]{CompanionState.READY, 1, 1, 1, 12, 12, initial, 0}), "state");
            screen.onGameState(state);
            if (botw) {
                // The native page keeps its own pages: none to report, and a request changes nothing here.
                require(screen.page() == -1, "no page of its own");
                screen.setPage(2); require(screen.page() == -1, "still none");
                continue;
            }
            require(screen.page() == initial, "created on the page asked for");
            for (int page = 0; page < 3; page++) { screen.setPage(page); require(screen.page() == page, "page round trip " + page); }
            screen.setPage(7); require(screen.page() == 2, "beyond the last page: the last");
            screen.setPage(-3); require(screen.page() == 0, "before the first: the first");
            int redraws = android.view.View.invalidated, fulls = fullRequests();
            screen.setPage(1);
            require(android.view.View.invalidated == redraws + 1 && fullRequests() == fulls + 1, "a page change redraws once and asks for the whole snapshot");
            // A loading screen named by the common state is a change of view: one redraw; the same state again: none.
            state.set(new long[]{CompanionState.LOADING, 0, 0, 1, -1, -1, 1, 0});
            redraws = android.view.View.invalidated;
            screen.onGameState(state); screen.onGameState(state);
            require(android.view.View.invalidated == redraws + 1, "told twice, redrawn once");
            require(screen.page() == 1, "the game's state does not move the page");
        }
        System.out.println("pass: companion screen " + args[0]);
    }
}
'''

# PresentationController's pump (the section between its two markers) around a queue the probe runs by hand.
PUMP_PRELUDE = r'''package com.aure.totk.companion;

import java.util.*;

public final class PumpProbe {
    static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    static final class Handler {
        final List<Object[]> queue = new ArrayList<>();
        long now;
        void post(Runnable r) { postDelayed(r, 0); }
        void postDelayed(Runnable r, long ms) { queue.add(new Object[]{now + ms, r}); }
        void removeCallbacks(Runnable r) { queue.removeIf(job -> job[1] == r); }
        void runUntil(long until) {
            for (;;) {
                int best = -1;
                for (int i = 0; i < queue.size(); i++) if ((long)queue.get(i)[0] <= until && (best < 0 || (long)queue.get(i)[0] < (long)queue.get(best)[0])) best = i;
                if (best < 0) break;
                Object[] job = queue.remove(best);
                now = Math.max(now, (long)job[0]);
                ((Runnable)job[1]).run();
            }
            now = until;
        }
    }
    static final class Source implements DataSource {
        long[] words; int reads; final List<Integer> pages = new ArrayList<>();
        @Override public String read(int detail) { return null; }
        @Override public boolean send(int kind, int arg) { return false; }
        @Override public boolean state(long[] out) { reads++; if (words == null) return false; System.arraycopy(words, 0, out, 0, words.length); return true; }
        @Override public void page(int page) { pages.add(page); }
    }
    static final class Screen implements CompanionScreen {
        int page; final List<String> told = new ArrayList<>();
        @Override public android.view.View view() { return null; }
        @Override public void setPage(int value) { page = value; }
        @Override public int page() { return page; }
        @Override public void onGameState(CompanionState s) { told.add(s.status + ":" + s.inGame + ":" + s.menuOpen + ":" + s.life + ":" + s.page); }
    }
    final Handler main = new Handler();
    final Source source = new Source();
    boolean closed;
'''
PUMP_TESTS = r'''
    public static void main(String[] args) {
        PumpProbe shell = new PumpProbe();
        Screen screen = new Screen();
        screen.page = 1;
        shell.screen(screen);
        require(shell.source.reads == 0, "nothing read before the queue runs");
        shell.main.runUntil(0);
        require(shell.source.pages.equals(List.of(1)), "the screen's page is published at once");
        require(screen.told.equals(List.of("0:false:false:-1:-1")), "a new screen is told even that there is no state");
        // A save, as the getter gives it (the page comes back in it).
        shell.source.words = new long[]{CompanionState.READY, 1, 0, 1, 12, 12, 1, 0};
        shell.main.runUntil(1000);
        require(screen.told.size() == 2 && screen.told.get(1).equals("5:true:false:12:1"), "told once when the state appears: " + screen.told);
        require(shell.source.reads == 5, "four looks a second: " + shell.source.reads);
        // The sample's age moves, nothing else: not told again.
        shell.source.words[CompanionState.AGE_MS] = 100;
        shell.main.runUntil(2000);
        require(screen.told.size() == 2, "an unchanged state is not repeated");
        // The menu opens, a heart goes.
        shell.source.words[CompanionState.MENU_OPEN] = 1; shell.main.runUntil(2250);
        shell.source.words[CompanionState.LIFE] = 8; shell.main.runUntil(2500);
        require(screen.told.subList(2, 4).equals(List.of("5:true:true:12:1", "5:true:true:8:1")), "each change once: " + screen.told);
        // The screen moves to another page: published once, within one look.
        screen.setPage(2);
        shell.main.runUntil(2750); shell.main.runUntil(3500);
        require(shell.source.pages.equals(List.of(1, 2)), "page published on change only: " + shell.source.pages);
        // The source goes quiet (an old sample): the screen is told there is no state.
        shell.source.words[CompanionState.AGE_MS] = CompanionState.MAX_AGE_MS + 1;
        shell.main.runUntil(3750);
        require(screen.told.get(screen.told.size() - 1).equals("0:false:false:-1:-1"), "a stale sample is no state");
        // The screen goes away: the pump stops, and the page is unpublished.
        int reads = shell.source.reads, told = screen.told.size();
        shell.screen(null);
        shell.main.runUntil(10_000);
        require(shell.source.reads == reads && screen.told.size() == told && shell.main.queue.isEmpty(), "no screen: no looks");
        require(shell.source.pages.equals(List.of(1, 2, -1)), "no screen: no page");
        // A screen without pages of its own (Breath of the Wild) publishes none.
        Screen paged = new Screen(); paged.page = -1;
        shell.screen(paged); shell.main.runUntil(11_000);
        require(shell.source.pages.equals(List.of(1, 2, -1)), "nothing to publish");
        require(paged.told.size() == 1, "told the state once");
        shell.closed = true; reads = shell.source.reads;
        shell.main.runUntil(20_000);
        require(shell.source.reads == reads, "a closed shell stops looking");
        System.out.println("pass: companion shell pump");
    }
}
'''


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


class CompanionScreenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not cls.javac or not cls.java:
            raise unittest.SkipTest('a JDK is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='totk-screen-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)

    def write(self, folder, files):
        paths = []
        for name, text in files.items():
            path = self.work / folder / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            paths.append(path)
        return paths

    def build(self, name, sources):
        classes = self.work / (name + '-classes')
        done = subprocess.run([self.javac, '--release', '17', '-Xlint:-options', '-d', str(classes), *map(str, sources)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        return classes

    def run_probe(self, classes, main, *args):
        done = subprocess.run([self.java, '-cp', str(classes), main, *args], capture_output=True, text=True, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout

    def test_interface_is_the_ports_own(self):
        text = (JAVA / 'CompanionScreen.java').read_text()
        for method in ('View view();', 'void setPage(int page);', 'int page();', 'void onGameState(CompanionState state);'):
            self.assertIn(method, text)
        controller = (JAVA / 'PresentationController.java').read_text()
        self.assertIn('CompanionScreen created = CompanionContent.create(getContext(), source, initialPage);', controller)
        self.assertIn('setContentView(created.view());', controller)
        self.assertNotIn('CompanionView', controller)      # the shell knows the interface, not a game's screen
        self.assertNotIn('DuoSurfaceView', controller)

    def screen_classes(self, game):
        """Both screens and the one CompanionContent, compiled with this game's generated GameBuild."""
        shared = [JAVA / n for n in ('CompanionScreen.java', 'CompanionState.java', 'DataSource.java', 'ItemActions.java', 'Motion.java',
                                     'CompanionView.java', 'DuoSurfaceView.java', 'CompanionContent.java')]
        sources = self.write(game, {**ANDROID_STUBS, **TOTK_STUBS, 'com/aure/totk/companion/ScreenProbe.java': SCREEN_PROBE,
                                    'com/aure/totk/setup/GameBuild.java': games.java(game)})
        return self.build(game, sources + shared)

    def test_totk_screen_page_round_trip(self):
        classes = self.screen_classes('totk')
        self.assertIn('pass: companion screen totk', self.run_probe(classes, 'com.aure.totk.companion.ScreenProbe', 'totk'))

    def test_botw_screen_keeps_no_page_and_needs_no_library(self):
        # No libduo_host.so on the host: the class still loads and the screen still stands behind the interface.
        classes = self.screen_classes('botw')
        self.assertIn('pass: companion screen botw', self.run_probe(classes, 'com.aure.totk.companion.ScreenProbe', 'botw'))

    def test_screen_is_chosen_by_the_game_variable(self):
        content = (JAVA / 'CompanionContent.java').read_text()
        self.assertIn('GameBuild.COMPANION.equals("duo")', content)
        self.assertEqual({games.values(g)['companion'] for g in ('totk', 'botw')}, {'map', 'duo'})
        self.assertFalse((ANDROID / 'java-botw').exists())   # no per-game file overrides

    def test_shell_publishes_the_page_and_tells_the_screen(self):
        controller = (JAVA / 'PresentationController.java').read_text()
        pump = section(controller, '    // ---- the screen shown (CompanionScreen', '    /** These presentation displays')
        view = {'android/view/View.java': 'package android.view; public class View { }'}
        sources = self.write('pump', {**view, 'com/aure/totk/companion/PumpProbe.java': PUMP_PRELUDE + pump + PUMP_TESTS})
        classes = self.build('pump', sources + [JAVA / n for n in ('CompanionScreen.java', 'CompanionState.java', 'DataSource.java')])
        self.assertIn('pass: companion shell pump', self.run_probe(classes, 'com.aure.totk.companion.PumpProbe'))


if __name__ == '__main__':
    unittest.main()
