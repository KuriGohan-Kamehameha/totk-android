package com.aure.totk.setup;

/** Second-screen art decisions, independent of Android so every stage state can be exercised on the host. */
final class ArtPolicy {
    enum State { DONE, SKIPPED, FAILED, ABSENT, OUTDATED }
    static final String DECLINED = "artDeclined", START_AFTER_BUILD = "startAfterArt";
    private ArtPolicy() { }

    /** Whether this app's second screen uses the art at all (GameBuild.COMPANION): only CompanionView ("map") draws from
     *  it. The Eden Duo page ("duo") reads the game's files itself, so the setup neither offers, builds nor updates the
     *  art (the stage and its code stay for CompanionView, which stays compiled). */
    static boolean used(String companion) {
        return !"duo".equals(companion);
    }

    static State state(boolean skipped, boolean failed, boolean present, boolean current) {
        if (present) return current ? State.DONE : State.OUTDATED;
        if (failed) return State.FAILED;
        return skipped ? State.SKIPPED : State.ABSENT;
    }
    /** An explicit Game data build is available on any device; only automatic setup skips the art. */
    static boolean skipSetup(boolean builtInSecondScreen, boolean explicitStages) {
        return !builtInSecondScreen && !explicitStages;
    }
    static boolean offerBeforePlay(boolean builtInSecondScreen, State state, boolean declined) {
        return builtInSecondScreen && !declined && state != State.DONE && state != State.OUTDATED;
    }
    /** Old-but-present art retains its automatic update on dual-screen devices, even after a missing-art decline. */
    static boolean updateAutomatically(boolean builtInSecondScreen, State state) {
        return builtInSecondScreen && state == State.OUTDATED;
    }
}
