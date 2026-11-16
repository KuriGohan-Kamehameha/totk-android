package com.aure.totk.companion;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.graphics.PixelFormat;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** The second screen of the Breath of the Wild build: a Surface the native companion host (duo/duo_host.cpp in
 *  libduo_host.so) draws the Eden Duo page into. This view owns nothing but the Surface: it hands it over when it
 *  exists, takes it back when it goes, and passes touches on. The page itself, its 60 Hz clock and its reads of the
 *  game all live in the native library.
 *
 *  The companion package (pages and code-drawn images; no game art) ships in the APK under assets/duo/ and is unpacked
 *  into the app's files once per installed build, because the native runtime reads it as a folder. */
public final class DuoSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
    private static final String TAG = "TotkDuo";
    private static boolean loaded;
    static {
        // Also what makes the two native methods resolvable; the runtime opens the same library by itself for the game's side.
        try { System.loadLibrary("duo_host"); loaded = true; }
        catch (UnsatisfiedLinkError missing) { loaded = false; }
    }
    static boolean available() { return loaded; }

    /** surface == null: the surface is gone. Returns once the native presenter no longer touches the previous one. */
    private static native void nativeSurface(Surface surface, int width, int height);
    /** The pointers now down, in view pixels; count 0 when all are lifted. */
    private static native void nativeTouch(int[] ids, float[] xs, float[] ys, int count);
    /** What the title module published last, for the stick LEDs (BifrostState): out[0..8] in duo_host.cpp's LedNames
     *  order, out[9] the age of that sample in milliseconds. False before the first sample. */
    private static native boolean nativeLedState(long[] out);
    private static volatile boolean ledState = true;
    /** Any thread. False when there is nothing to read: no library, an older library, or no sample yet. */
    public static boolean gameState(long[] out) {
        if (!loaded || !ledState) return false;
        try { return nativeLedState(out); }
        catch (UnsatisfiedLinkError older) { ledState = false; return false; }
    }

    public DuoSurfaceView(Context context) {
        super(context);
        getHolder().setFormat(PixelFormat.RGBA_8888);
        // Above its window: in the Presentation the default placement (behind the window, seen through a hole) showed
        // black on the Thor although frames were posted (2026-11-16). Nothing else is drawn in this window.
        setZOrderOnTop(true);
        getHolder().addCallback(this);
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (loaded) nativeSurface(holder.getSurface(), width, height);
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        if (loaded) nativeSurface(null, 0, 0);
    }

    private final int[] ids = new int[16];
    private final float[] xs = new float[16], ys = new float[16];
    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!loaded) return false;
        int action = event.getActionMasked();
        // The pointer that is lifting is still listed by its own UP event: leave it out, so native sees it end.
        int lifting = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
        int count = 0;
        if (action != MotionEvent.ACTION_CANCEL) {
            for (int i = 0; i < event.getPointerCount() && count < ids.length; i++) {
                if (i == lifting) continue;
                ids[count] = event.getPointerId(i); xs[count] = event.getX(i); ys[count] = event.getY(i); count++;
            }
        }
        nativeTouch(ids, xs, ys, count);
        return true;
    }

    /** Unpacks assets/duo/ into files/duo/ unless this build's copy is already there. files/duo/.ready, written last,
     *  names the build it came from; the native host waits for it. A stale mark is removed here, before the game's
     *  first frame can look for it, and the copy runs off the main thread. */
    static void prepare(Context context) {
        Context app = context.getApplicationContext();
        File root = new File(app.getFilesDir(), "duo"), ready = new File(root, ".ready");
        String build;
        try {
            PackageInfo info = app.getPackageManager().getPackageInfo(app.getPackageName(), 0);
            build = info.getLongVersionCode() + ":" + info.lastUpdateTime;
        } catch (android.content.pm.PackageManager.NameNotFoundException impossible) { build = "?"; }
        try {
            if (ready.isFile() && build.equals(new String(Files.readAllBytes(ready.toPath()), StandardCharsets.UTF_8))) return;
        } catch (IOException unreadable) { /* unpack again */ }
        if (ready.exists() && !ready.delete()) { Log.w(TAG, "could not clear " + ready); return; }
        final String mark = build;
        Thread unpack = new Thread(() -> {
            try {
                delete(root);
                copy(app.getAssets(), "duo", root);
                Files.write(ready.toPath(), mark.getBytes(StandardCharsets.UTF_8));
            } catch (IOException failed) {
                Log.w(TAG, "companion package not unpacked; the second screen stays empty", failed);
            }
        }, "duo-unpack");
        unpack.setPriority(Thread.MIN_PRIORITY);
        unpack.start();
    }
    private static void delete(File node) {
        File[] children = node.listFiles();
        if (children != null) for (File child : children) delete(child);
        node.delete();
    }
    /** AssetManager.list names files and folders alike; an entry with no children that opens is a file. */
    private static void copy(AssetManager assets, String path, File to) throws IOException {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("cannot create " + to);
            for (String child : children) copy(assets, path + "/" + child, new File(to, child));
            return;
        }
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(to)) {
            byte[] buffer = new byte[1 << 16];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        }
    }
}
