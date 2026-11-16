package com.aure.totk.companion;

import android.content.Context;
import android.view.View;

/** What the second screen shows, and whether there is anything to show. A game build may replace this file
 *  (java-&lt;game&gt;/, apk/build_dex.py): this one is Tears of the Kingdom's companion, drawn by CompanionView from
 *  the art the setup generated on the device. */
public final class CompanionContent {
    private CompanionContent() { }
    /** Call after NativeActivity.onCreate (the runtime options are read there). */
    public static boolean available(Context context) { return CompanionNative.isEnabled() && AssetStore.available(context); }
    static View create(Context context, DataSource source, int initialPage) {
        CompanionView view = new CompanionView(context, source);
        view.setPage(initialPage);
        return view;
    }
}
