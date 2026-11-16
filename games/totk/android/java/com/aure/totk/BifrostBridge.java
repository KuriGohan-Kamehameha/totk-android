package com.aure.totk;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.os.SystemClock;
import android.system.Os;
import android.util.Log;
import java.util.List;
import java.util.Map;

/** The stick LEDs follow the game's tablet: while the game is in the foreground, tells Bifrost (the AYN Thor's LED
 *  app, when it is installed) what to show. BifrostLooks decides what and when; BifrostState reads the game; this
 *  class owns the thread and the broadcasts. Without Bifrost there is no thread, no read of the game and no broadcast.
 *
 *  TOTK_BIFROST (totk.env): 0 off; 2 the public effects instead of the Pip-Boy feed; anything else, or unset, on.
 *  Nothing here runs on a game thread, and no failure leaves this class: after one, the bridge stays off. */
final class BifrostBridge {
    private static final String TAG = "TotkBifrost";
    private static volatile BifrostBridge current;
    private final Context context;
    private final BifrostLooks.Feed feed;
    private final BifrostLooks.State state = new BifrostLooks.State();
    private HandlerThread thread;
    private volatile Handler worker;
    private volatile boolean running, dead;
    private String said = "";
    private volatile int generation;

    /** After NativeActivity.onCreate (totk.env is read there). Null when turned off. */
    static BifrostBridge create(Activity activity) {
        String option = Os.getenv("TOTK_BIFROST");
        if ("0".equals(option)) return null;
        com.aure.totk.companion.PresentationController.touchListener = BifrostBridge::touched;
        return new BifrostBridge(activity.getApplicationContext(), "2".equals(option));
    }
    private BifrostBridge(Context context, boolean classic) {
        this.context = context;
        feed = new BifrostLooks.Feed(BifrostState.GAME, classic);
    }

    /** UI thread: the game is in the foreground and running. */
    void start() {
        if (running || dead) return;
        try {
            if (worker == null) {
                // The manifest's <queries> entry makes Bifrost visible here. Not installed: nothing is started.
                context.getPackageManager().getPackageInfo(BifrostLooks.PACKAGE, 0);
                thread = new HandlerThread("totk-bifrost", Process.THREAD_PRIORITY_BACKGROUND);
                thread.start();
                worker = new Handler(thread.getLooper());
                Log.i(TAG, "Bifrost is installed: the stick LEDs follow the game" + (feed.classic() ? " (public effects)" : ""));
            }
        } catch (android.content.pm.PackageManager.NameNotFoundException absent) {
            return;
        } catch (Throwable failed) {
            fail(failed); return;
        }
        running = true;
        current = this;
        int token = ++generation;
        Handler queue = worker;
        queue.post(new Runnable() {
            @Override public void run() {
                if (token != generation || dead) return;
                try {
                    boolean have = BifrostState.read(state);
                    long now = SystemClock.elapsedRealtime();
                    List<BifrostLooks.Command> commands;
                    String reason;
                    synchronized (feed) { commands = feed.poll(now, have ? state : null); reason = feed.muteReason(now); }
                    send(commands);
                    if (!reason.equals(said)) { said = reason; if (!reason.isEmpty()) Log.i(TAG, reason + "; trying again later"); }
                } catch (Throwable failed) { fail(failed); return; }
                if (token == generation) queue.postDelayed(this, BifrostLooks.Feed.POLL_MS);
            }
        });
    }

    /** UI thread: the game paused, stopped or lost the foreground. The LEDs go back to Bifrost at once. */
    void stop() {
        if (!running) return;
        running = false;
        int token = ++generation;
        Handler queue = worker;
        if (queue == null) return;
        queue.post(() -> {   // the poll loop ends by its token
            if (token != generation) return;   // started again before this ran: the feed just continues
            try { send(stopped()); } catch (Throwable failed) { fail(failed); }
        });
    }

    /** UI thread, before the process ends: the clear is sent here, since the worker may never run again. */
    void close() {
        running = false;
        ++generation;
        if (current == this) current = null;
        HandlerThread ending = thread;
        thread = null;
        Handler queue = worker;
        worker = null;
        if (ending == null) return;
        if (queue != null) queue.removeCallbacksAndMessages(null);
        ending.quitSafely();
        try { ending.join(200); } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
        try { send(stopped()); } catch (Throwable failed) { fail(failed); }
    }
    private List<BifrostLooks.Command> stopped() {
        synchronized (feed) { return feed.stop(SystemClock.elapsedRealtime()); }
    }

    /** UI thread: the second screen was touched (the effect dips, as the Pip-Boy's does on menu navigation). */
    static void touched() {
        BifrostBridge bridge = current;
        if (bridge == null || !bridge.running) return;
        Handler queue = bridge.worker;
        int token = bridge.generation;
        if (queue == null) return;
        queue.post(() -> {
            if (token != bridge.generation || bridge.dead) return;
            try {
                List<BifrostLooks.Command> commands;
                synchronized (bridge.feed) { commands = bridge.feed.touch(SystemClock.elapsedRealtime()); }
                bridge.send(commands);
            } catch (Throwable failed) { bridge.fail(failed); }
        });
    }

    private void fail(Throwable failed) {
        if (!dead) Log.w(TAG, "the Bifrost bridge is off for this session", failed);
        dead = true; running = false;
    }

    private void send(List<BifrostLooks.Command> commands) {
        for (BifrostLooks.Command command : commands) {
            Intent intent = new Intent(command.action).setClassName(BifrostLooks.PACKAGE, BifrostLooks.RECEIVER);
            for (Map.Entry<String, Object> extra : command.extras.entrySet()) {
                Object value = extra.getValue();
                if (value instanceof Integer) intent.putExtra(extra.getKey(), (int)(Integer)value);
                else if (value instanceof Long) intent.putExtra(extra.getKey(), (long)(Long)value);
                else if (value instanceof Float) intent.putExtra(extra.getKey(), (float)(Float)value);
                else if (value instanceof Boolean) intent.putExtra(extra.getKey(), (boolean)(Boolean)value);
                else intent.putExtra(extra.getKey(), String.valueOf(value));
            }
            Handler queue = worker;
            if (!command.wantsResult || queue == null) { context.sendBroadcast(intent); continue; }
            // Ordered only for the result code: Bifrost says when its switch is off, and the feed then waits.
            context.sendOrderedBroadcast(intent, null, new BroadcastReceiver() {
                @Override public void onReceive(Context c, Intent i) {
                    try { synchronized (feed) { feed.result(SystemClock.elapsedRealtime(), getResultCode()); } } catch (Throwable ignored) { /* stays as it is */ }
                }
            }, queue, BifrostLooks.RESULT_UNHANDLED, null, null);
        }
    }
}
