package com.aure.totk.companion;

/** The second screen's common game state: the few facts every game's companion and the app's shell agree on, as the
 *  native side keeps them (runtime/companion_state.h). One getter delivers it, {@link DataSource#state}: FIELDS words
 *  in the order of the constants below. Tears of the Kingdom fills it from its snapshot reader, Breath of the Wild from
 *  what its Eden Duo module publishes; the stick-LED bridge and the shell (PresentationController) read the same thing.
 *  No Android class is used here, so host tests run it on a plain JVM. */
public final class CompanionState {
    /** status: nothing read; the game's state is not readable yet; another game build; title screen; loading screen
     *  (only where the game side can tell it from the title); a save is loaded. */
    public static final int NONE = 0, STARTING = 1, UNSUPPORTED = 2, TITLE = 3, LOADING = 4, READY = 5;
    /** Word order of {@link DataSource#state}; AGE_MS is how old the sample is. */
    public static final int STATUS = 0, IN_GAME = 1, MENU_OPEN = 2, TABLET_OWNED = 3, LIFE = 4, MAX_LIFE = 5, PAGE = 6,
            AGE_MS = 7, FIELDS = 8;
    /** A sample older than this says nothing about the game now (its source stopped ticking). */
    public static final long MAX_AGE_MS = 2000;

    public int status = NONE;
    /** A save is loaded and the world is running or paused in its own menu: not the title, not a loading screen. */
    public boolean inGame;
    /** The game's pause menu (the pad or slate itself) is up. */
    public boolean menuOpen;
    /** False only where the tablet can be missing and is: Breath of the Wild before the Sheikah Slate is taken. */
    public boolean tabletOwned = true;
    /** Quarter hearts; -1 when not read. */
    public int life = -1, maxLife = -1;
    /** The second screen's current page as its screen numbers them; -1 unknown. */
    public int page = -1;

    public void clear() { status = NONE; inGame = false; menuOpen = false; tabletOwned = true; life = -1; maxLife = -1; page = -1; }

    /** Takes the getter's words. False (and cleared) when they say nothing: too few, no sample, or one too old. */
    public boolean set(long[] v) {
        clear();
        if (v == null || v.length < FIELDS) return false;
        long age = v[AGE_MS], s = v[STATUS];
        if (age < 0 || age > MAX_AGE_MS || s <= NONE || s > READY) return false;
        status = (int)s;
        inGame = status == READY && v[IN_GAME] == 1;
        menuOpen = inGame && v[MENU_OPEN] == 1;
        tabletOwned = !inGame || v[TABLET_OWNED] != 0;
        if (inGame) { life = (int)v[LIFE]; maxLife = (int)v[MAX_LIFE]; }
        page = (int)v[PAGE];
        return true;
    }

    /** What a change in it means to a screen: everything but the sample's age. */
    public long key() {
        return status | (inGame ? 8 : 0) | (menuOpen ? 16 : 0) | (tabletOwned ? 32 : 0)
                | (long)(life & 0xffff) << 8 | (long)(maxLife & 0xffff) << 24 | (long)(page & 0xff) << 40;
    }
}
