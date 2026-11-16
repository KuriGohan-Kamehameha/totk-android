package com.aure.totk.companion;

import android.content.Context;

/** What the second screen shows, and whether there is anything to show. A game build may replace this file
 *  (java-&lt;game&gt;/, apk/build_dex.py): this one is Tears of the Kingdom's companion, drawn by CompanionView from
 *  the art the setup generated on the device. */
public final class CompanionContent {
    private CompanionContent() { }
    /** Whether this game starts without its tablet (the stick LEDs then have a look for that). */
    public static final boolean TABLET_CAN_BE_MISSING = false;
    /** Call after NativeActivity.onCreate (the runtime options are read there). */
    public static boolean available(Context context) { return CompanionNative.isEnabled() && AssetStore.available(context); }
    static CompanionScreen create(Context context, DataSource source, int initialPage) {
        CompanionScreen screen = new CompanionView(context, source);
        screen.setPage(initialPage);
        return screen;
    }
}
