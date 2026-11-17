package com.aure.totk.companion;

import android.content.Context;

import com.aure.totk.setup.GameBuild;

/** What the second screen shows, and whether there is anything to show, by the game's companion variable
 *  (games/games.toml companion, GameBuild.COMPANION):
 *  - "map": TotK Native's own companion, drawn by CompanionView from the art the setup generated on the device (MIT;
 *    compiled into every app, chosen by no game at the moment);
 *  - "duo": the game's Eden Duo page (Breath of the Wild's, Tears of the Kingdom's), drawn by the native companion host
 *    (libduo_host.so, GPL-3.0, only in the APKs of games with games.toml gpl_companion) into a DuoSurfaceView. Without
 *    that library, or with the companion turned off, there is no second-screen content and the game runs as it does
 *    without one.
 *  Both are CompanionScreens; PresentationController knows nothing else of them. */
public final class CompanionContent {
    private CompanionContent() { }
    static final boolean DUO = GameBuild.COMPANION.equals("duo");
    /** Whether this game starts without its tablet (the stick LEDs then have a look for that). */
    public static final boolean TABLET_CAN_BE_MISSING = GameBuild.TABLET_CAN_BE_MISSING;
    /** Call after NativeActivity.onCreate (the runtime options are read there). The Eden Duo page starts unpacking its
     *  package here. */
    public static boolean available(Context context) {
        if (!CompanionNative.isEnabled()) return false;
        if (!DUO) return AssetStore.available(context);
        if (!DuoSurfaceView.available()) return false;
        DuoSurfaceView.prepare(context);
        return true;
    }
    static CompanionScreen create(Context context, DataSource source, int initialPage) {
        CompanionScreen screen = DUO ? new DuoSurfaceView(context) : new CompanionView(context, source);
        screen.setPage(initialPage);
        return screen;
    }
}
