package com.aure.totk;

import com.aure.totk.companion.CompanionNative;
import com.aure.totk.companion.CompanionState;
import com.aure.totk.setup.GameBuild;

/** Where the Bifrost bridge reads the game from, and which colours it uses: the second screen's common game state
 *  (runtime/companion_state.h, CompanionState), the same in every game build. Tears of the Kingdom fills it from the
 *  companion's snapshot reader, Breath of the Wild from the values its Eden Duo module publishes every tick; either
 *  way there is a state only while the companion is on (TOTK_COMPANION is not 0), and in Breath of the Wild only with
 *  the companion library running. Without one the bridge sends nothing. */
final class BifrostState {
    private BifrostState() { }
    /** The colour table, by the game's bifrost_looks variable (games/games.toml). */
    static final int GAME = "BOTW".equals(GameBuild.BIFROST_LOOKS) ? BifrostLooks.BOTW : BifrostLooks.TOTK;
    private static final CompanionNative SOURCE = new CompanionNative();
    private static final long[] WORDS = new long[CompanionState.FIELDS];
    private static final CompanionState STATE = new CompanionState();

    /** The bridge's own thread. False: no state (title, loading, companion off or not running). */
    static boolean read(BifrostLooks.State out) {
        if (!CompanionNative.isEnabled() || !SOURCE.state(WORDS) || !STATE.set(WORDS)) { out.clear(); return false; }
        return BifrostLooks.fromState(STATE, out);
    }
}
