package com.aure.totk;

import com.aure.totk.companion.CompanionNative;
import com.aure.totk.companion.DuoSurfaceView;

/** Breath of the Wild (replaces java/.../BifrostState.java in that build): the values the Eden Duo title module
 *  publishes every tick, kept by the native companion host (duo/duo_host.cpp). Without libduo_host.so, or while the
 *  companion is not running, there is no state and the bridge sends nothing. */
final class BifrostState {
    private BifrostState() { }
    static final int GAME = BifrostLooks.BOTW;
    private static final long[] VALUES = new long[BifrostLooks.DUO_VALUES];

    /** The bridge's own thread. False: no state (title, loading, companion not running). */
    static boolean read(BifrostLooks.State out) {
        // Companion off: the companion library is not this class's to load.
        if (!CompanionNative.isEnabled() || !DuoSurfaceView.gameState(VALUES)) { out.clear(); return false; }
        return BifrostLooks.fromDuo(VALUES, out);
    }
}
