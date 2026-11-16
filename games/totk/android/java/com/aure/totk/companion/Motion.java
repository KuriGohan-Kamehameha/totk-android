package com.aure.totk.companion;

/** Everything of the Purah Pad that moves, as arithmetic on a clock the caller supplies: which view is up (pages,
 *  title, loading, unsupported) and its fade-in, the slide between pages, the pulse when the hearts change, and the
 *  player marker's glide between two position samples. No Android class and no clock of its own, so the host test
 *  (tests/test_companion_motion.py) drives it on a plain JVM.
 *
 *  Nothing here runs by itself. CompanionView asks {@link #nextFrame} after each draw and schedules one more frame only
 *  while something is still moving: an idle screen is not redrawn. Times are milliseconds on one monotonic clock. */
final class Motion {
    /** Page slide, heart pulse, marker glide (a little over the 125 ms between position samples, so it never stalls
     *  between two), view fade-in. */
    static final long SLIDE_MS = 220, PULSE_MS = 700, FOLLOW_MS = 160, FADE_MS = 240;
    /** Between frames while only the marker glides (about 30 a second) and while the loading mark turns (8 a second). */
    static final long FOLLOW_FRAME_MS = 33, LOADING_FRAME_MS = 125;
    /** A jump of the player by more than this many world units (a warp, a load) is not glided over. */
    static final double SNAP_DISTANCE = 80;
    static final int LOADING_STEPS = 8;

    // ---- which view ----
    static final int CONTENT = 0, TITLE = 1, LOADING = 2, UNSUPPORTED = 3;
    /** status: the snapshot's. stale: that snapshot is the last good one kept while the game shows no save (the poller
     *  keeps it for 30 s). gameStatus: CompanionState.status of the common state, which may name a loading screen. */
    static int view(String status, boolean stale, int gameStatus) {
        if ("unsupported".equals(status)) return UNSUPPORTED;
        if ("ready".equals(status)) return stale || gameStatus == CompanionState.LOADING ? LOADING : CONTENT;
        return "title".equals(status) ? TITLE : LOADING;   // starting, or anything not known here
    }
    private int view = LOADING;
    private boolean viewShown;
    private long fadeStart = Long.MIN_VALUE / 2;
    int view() { return view; }
    /** The first view appears at once; a change fades the new one in. */
    void show(int next, long now) {
        if (viewShown && next != view) fadeStart = now;
        if (next != view) { slideStart = Long.MIN_VALUE / 2; if (next != CONTENT) forget(); }
        view = next; viewShown = true;
    }
    /** 0..1: how much of the current view is in. */
    float fade(long now) { return progress(now, fadeStart, FADE_MS); }

    // ---- page slide ----
    private long slideStart = Long.MIN_VALUE / 2;
    private int slideFrom, slideDirection = 1;
    void slide(int from, int to, long now) {
        if (from == to) return;
        slideFrom = from; slideDirection = to > from ? 1 : -1; slideStart = now;
    }
    boolean sliding(long now) { return now - slideStart < SLIDE_MS && now >= slideStart; }
    /** 0..1, eased: 0 the old page is in place, 1 the new one is. 1 when no slide runs. */
    float slide(long now) { return easeOut(progress(now, slideStart, SLIDE_MS)); }
    int slideFrom() { return slideFrom; }
    /** +1: the new page comes in from the right (a higher page number), -1: from the left. */
    int slideDirection() { return slideDirection; }

    // ---- heart pulse ----
    private int life = -1, pulseLow, pulseHigh;
    private boolean lifeKnown;
    private long pulseStart = Long.MIN_VALUE / 2;
    /** Every sample of the hearts (quarters). The first after forget() only sets the baseline. */
    void life(int value, long now) {
        if (lifeKnown && value != life) {
            pulseLow = Math.min(life, value); pulseHigh = Math.max(life, value); pulseStart = now;
        }
        life = value; lifeKnown = true;
    }
    /** 1 at the change, falling to 0 over PULSE_MS; 0 when none runs. */
    float pulse(long now) {
        float t = progress(now, pulseStart, PULSE_MS);
        return t >= 1 ? 0 : (1 - t) * (1 - t);
    }
    /** Whether heart `index` (four quarters each) holds a quarter that changed. */
    boolean pulsed(int index) { return index * 4 < pulseHigh && index * 4 + 4 > pulseLow; }
    /** Drawing scale of heart `index`: the hearts that changed swell, the rest barely. */
    float heartScale(int index, long now) {
        float p = pulse(now);
        return p <= 0 ? 1 : 1 + (pulsed(index) ? .45f : .06f) * p;
    }

    // ---- player marker ----
    private boolean markerKnown;
    private double fromX, fromZ, toX, toZ, fromHeading = Double.NaN, toHeading = Double.NaN;
    private long followStart = Long.MIN_VALUE / 2;
    /** Every position sample (world X/Z, heading in degrees or NaN). Glides from where the marker is drawn now. */
    void target(double x, double z, double heading, long now) {
        if (!finite(x) || !finite(z)) { markerKnown = false; return; }
        boolean snap = !markerKnown || Math.hypot(x - toX, z - toZ) > SNAP_DISTANCE;
        if (!snap && x == toX && z == toZ && same(heading, toHeading)) return;   // an unchanged sample moves nothing
        if (snap) { fromX = x; fromZ = z; fromHeading = heading; followStart = Long.MIN_VALUE / 2; }
        else { fromX = markerX(now); fromZ = markerZ(now); fromHeading = heading(now); followStart = now; }
        toX = x; toZ = z; toHeading = heading; markerKnown = true;
    }
    boolean markerKnown() { return markerKnown; }
    double markerX(long now) { return fromX + (toX - fromX) * progress(now, followStart, FOLLOW_MS); }
    double markerZ(long now) { return fromZ + (toZ - fromZ) * progress(now, followStart, FOLLOW_MS); }
    /** Degrees, turning the short way round; NaN while the game gives none. */
    double heading(long now) {
        if (!finite(toHeading)) return Double.NaN;
        if (!finite(fromHeading)) return toHeading;
        double turn = ((toHeading - fromHeading) % 360 + 540) % 360 - 180;
        return fromHeading + turn * progress(now, followStart, FOLLOW_MS);
    }
    /** The hearts and the marker start afresh (a load, the title): the next samples neither pulse nor glide. */
    void forget() { lifeKnown = false; markerKnown = false; pulseStart = followStart = Long.MIN_VALUE / 2; }

    // ---- loading mark ----
    /** Which of the LOADING_STEPS dots leads now. */
    static int loadingStep(long now) { return (int)Math.floorMod(now / LOADING_FRAME_MS, (long)LOADING_STEPS); }

    // ---- is anything moving ----
    /** When the next frame is due: -1 never (nothing moves), 0 with the next display frame, otherwise in that many
     *  milliseconds. heartsVisible / markerVisible: whether the page drawn now shows them at all. */
    long nextFrame(long now, boolean heartsVisible, boolean markerVisible) {
        if (sliding(now) || active(now, fadeStart, FADE_MS)) return 0;
        if (view != CONTENT) return view == LOADING ? LOADING_FRAME_MS - Math.floorMod(now, LOADING_FRAME_MS) : -1;
        if (heartsVisible && active(now, pulseStart, PULSE_MS)) return 0;
        return markerVisible && markerKnown && active(now, followStart, FOLLOW_MS) ? FOLLOW_FRAME_MS : -1;
    }
    boolean animating(long now, boolean heartsVisible, boolean markerVisible) { return nextFrame(now, heartsVisible, markerVisible) >= 0; }

    private static boolean active(long now, long start, long duration) { return now >= start && now - start < duration; }
    private static float progress(long now, long start, long duration) {
        if (now <= start) return 0;
        return now - start >= duration ? 1 : (now - start) / (float)duration;
    }
    static float easeOut(float t) { float r = 1 - t; return 1 - r * r * r; }
    private static boolean finite(double v) { return !Double.isNaN(v) && !Double.isInfinite(v); }
    private static boolean same(double a, double b) { return a == b || Double.isNaN(a) && Double.isNaN(b); }
}
