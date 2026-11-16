package com.aure.totk.companion;

import android.content.Context;

/** Breath of the Wild (apk/build_dex.py --game botw160; replaces java/.../CompanionContent.java in that build): the
 *  second screen shows the Eden Duo companion page, drawn by the native companion host (libduo_host.so) into a
 *  DuoSurfaceView. Without that library, or with the companion turned off, there is no second-screen content and the
 *  game runs as it does without one. */
public final class CompanionContent {
    private CompanionContent() { }
    /** Whether this game starts without its tablet (the stick LEDs then have a look for that): the Sheikah Slate. */
    public static final boolean TABLET_CAN_BE_MISSING = true;
    /** Call after NativeActivity.onCreate (the runtime options are read there). Starts unpacking the package. */
    public static boolean available(Context context) {
        if (!CompanionNative.isEnabled() || !DuoSurfaceView.available()) return false;
        DuoSurfaceView.prepare(context);
        return true;
    }
    static CompanionScreen create(Context context, DataSource source, int initialPage) {
        CompanionScreen screen = new DuoSurfaceView(context);
        screen.setPage(initialPage);
        return screen;
    }
}
