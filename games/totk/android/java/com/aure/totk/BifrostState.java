package com.aure.totk;

import com.aure.totk.companion.CompanionNative;

/** Where the Bifrost bridge reads the game from, and which colours it uses. A game build may replace this file
 *  (java-&lt;game&gt;/, apk/build_dex.py). Tears of the Kingdom: the companion's live snapshot
 *  (runtime/companion_snapshot.c), so there is a state only while the companion is on (TOTK_COMPANION is not 0).
 *  That snapshot has the pause menu flag and the hearts; it has no flag for the map screen or for owning the pad. */
final class BifrostState {
    private BifrostState() { }
    static final int GAME = BifrostLooks.TOTK;
    private static final CompanionNative SOURCE = new CompanionNative();

    /** The bridge's own thread. False: no state (title, loading, companion off). */
    static boolean read(BifrostLooks.State out) {
        if (!CompanionNative.isEnabled()) { out.clear(); return false; }
        return BifrostLooks.fromSnapshot(SOURCE.read(0), out);
    }
}
