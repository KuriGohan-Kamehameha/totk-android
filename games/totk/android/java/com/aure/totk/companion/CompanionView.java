package com.aure.totk.companion;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import org.json.JSONArray;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.Locale;

/** Responsive, touch-first Purah Pad. There is no fixed design canvas or idle animation loop: what moves (Motion) asks
 * for frames only while it moves. */
public final class CompanionView extends View implements SnapshotPoller.Listener, CompanionScreen {
    private int INK = 0xffe9e5d5, MUTED = 0xffaaa997, GOLD = 0xffdcc08a, GREEN = 0xff77d7b1;
    private int PANEL = 0xc015211e, CURSOR = 0xffeee8cc, BACKGROUND = 0xff0d1a18, ACCENT_DIM = 0xff405247;
    private static final String[] PAGE_KEYS = {"adventure", "inventory", "map"};
    private static final String[] LAYERS = {"Ground", "Sky", "UnderGround"};
    private static final String[] CATEGORY_SYMBOLS = {"⚔", "➶", "◇", "♙", "❋", "♨", "⬡", "✦"};
    private static final String[] CATEGORY_NAMES = {"Weapons", "Bows & Arrows", "Shields", "Armor", "Materials", "Food", "Zonai Devices", "Key Items"};
    private static final String[] PAGE_NAMES = {"Adventure", "Inventory", "Map"}, PAGE_SYMBOLS = {"✥", "▦", "◇"};
    private static final String[] SAGE_IDS = {"Wind", "Fire", "Water", "Electric", "Soul"};
    private static final String[] ITEM_STATS = {"count", "life", "fuseLife", "heal", "level", "time", "price"};
    private static final String[] LAYER_ICONS = {"layer_ground", "layer_sky", "layer_depths"}, LAYER_SYMBOLS = {"▱", "☁", "▽"};
    private static final String[] PAGE_DECOR = {"page_adventure", "page_inventory", "page_map"};
    private static final String[] ACTION_IDS = {"equip", "unequip", "eat", "drink", "use", "hold", "takeOut", "fuseArrow"};
    private static final String[] ACTION_LABELS = {"Equip", "Unequip", "Eat", "Drink", "Use", "Hold", "Take out", "Fuse to arrows"};
    private static final String[] ACTION_DONE = {"Equipped", "Unequipped", "Eaten", "Drunk", "Used", "Now holding it", "Taken out", "Fused to arrows"};
    /** Adventure cards: CODE_JUMP + category * 1000 + index in Snapshot.items(category). */
    private static final int CODE_ACTION = 300, CODE_JUMP = 10000;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Path path = new Path();
    private static final android.graphics.ColorFilter HEART_RED = new android.graphics.PorterDuffColorFilter(0xffe7796b, android.graphics.PorterDuff.Mode.SRC_IN);
    private static final android.graphics.ColorFilter HEART_GOLD = new android.graphics.PorterDuffColorFilter(0xffead375, android.graphics.PorterDuff.Mode.SRC_IN);
    private static final android.graphics.ColorFilter HEART_EMPTY = new android.graphics.PorterDuffColorFilter(0xff657369, android.graphics.PorterDuff.Mode.SRC_IN);
    private static final android.graphics.ColorFilter HEART_BROKEN = new android.graphics.PorterDuffColorFilter(0xffa45660, android.graphics.PorterDuff.Mode.SRC_IN);
    private static final android.graphics.ColorFilter CATEGORY_INK = new android.graphics.PorterDuffColorFilter(0xff26241b, android.graphics.PorterDuff.Mode.SRC_IN);
    private android.graphics.ColorFilter categorySelectedTint;
    private int categorySelectedColor;
    private final ArrayList<Target> targets = new ArrayList<>();
    private final SnapshotPoller poller;
    private AssetStore assets;
    private MapRenderer map, minimap;
    private final ScaleGestureDetector scaleDetector;
    private float density, fontScale;
    private float u, sp;
    private Snapshot data = Snapshot.waiting();
    private boolean stale, dragging, categoryBar;
    private int page, category, selection = -1;
    private float scroll, scrollMax, downX, downY, lastX, lastY;
    private float detailScroll, detailScrollMax;
    private final RectF detailBounds = new RectF();
    private RectF content = new RectF(), mapBounds = new RectF();
    private int pressed = -1;

    /** Request per drawn action button (code CODE_ACTION + index), rebuilt every frame. */
    private final ArrayList<ItemActions.Request> buttonRequests = new ArrayList<>();
    private ItemActions.State actionView;
    private boolean revealSelection;
    /** What moves, and the clock value of the frame being drawn. */
    private final Motion motion = new Motion();
    private long frameNow;
    private int gameStatus = CompanionState.NONE;
    /** The page a slide leaves, as it was drawn last. */
    private final android.graphics.Picture outgoing = new android.graphics.Picture();
    private boolean outgoingRecorded;
    private final Runnable frame = this::invalidate;
    private static final class Target {
        final RectF box; final int code;
        Target(RectF box, int code) { this.box = new RectF(box); this.code = code; }
    }
    public CompanionView(Context context, DataSource source) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        fontScale = getResources().getDisplayMetrics().scaledDensity / density;
        u = density; sp = u * fontScale;
        poller = new SnapshotPoller(source, this);
        setBackgroundColor(0xff0d1a18);
        setFocusable(false);
        setContentDescription("Purah Pad companion: Adventure, Inventory, Map");
        scaleDetector = new ScaleGestureDetector(context, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScale(ScaleGestureDetector detector) {
                if (page != 2 || map == null) return false;
                map.scale(detector.getScaleFactor(), detector.getFocusX(), detector.getFocusY()); invalidate(); return true;
            }
        });
    }
    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        assets = new AssetStore(getContext(), this::invalidate);
        map = new MapRenderer(assets, u); minimap = new MapRenderer(assets, u);
        poller.start();
    }
    @Override protected void onDetachedFromWindow() {
        poller.stop();
        removeCallbacks(frame);
        if (assets != null) { assets.close(); assets = null; }
        super.onDetachedFromWindow();
    }
    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        updatePanelUnit(w, h);
    }
    @Override protected void onConfigurationChanged(android.content.res.Configuration configuration) {
        super.onConfigurationChanged(configuration);
        updatePanelUnit(getWidth(), getHeight());
        invalidate();
    }
    private void updatePanelUnit(int w, int h) {
        density = getResources().getDisplayMetrics().density;
        fontScale = getResources().getDisplayMetrics().scaledDensity / density;
        // A small high-dpi companion panel must not inherit a phone's oversized logical UI.
        u = Math.max(.55f * density, Math.min(density, Math.min(w, h) / 700f));
        sp = u * fontScale;
        if (map != null) { map.setUnit(u); minimap.setUnit(u); }
    }
    @Override public void changed(Snapshot value, boolean loading) {
        data = value; stale = loading;
        long now = android.os.SystemClock.uptimeMillis();
        motion.show(Motion.view(value.status, loading, gameStatus), now);
        if (motion.view() == Motion.CONTENT) {
            if (value.player.has("life")) motion.life(value.player.optInt("life"), now);
            motion.target(value.coordinate(0), value.coordinate(2), value.player.optDouble("heading", Double.NaN), now);
        }
        setContentDescription("Purah Pad, " + value.status + (loading ? ", loading; last known data" : "")
                + ", " + PAGE_NAMES[page]);
        invalidate();
    }
    @Override public View view() { return this; }
    @Override public int page() { return page; }
    /** The common state names a loading screen where the game side can tell one; the rest of it this view already has,
     * in more detail, from its own snapshot. */
    @Override public void onGameState(CompanionState state) {
        if (state.status == gameStatus) return;
        gameStatus = state.status;
        motion.show(Motion.view(data.status, stale, gameStatus), android.os.SystemClock.uptimeMillis());
        invalidate();
    }
    @Override public void setPage(int value) {
        int next = Math.max(0, Math.min(2, value));
        // The page being left slides out as it looked: recorded once, before its scroll and selection are dropped.
        if (next != page && assets != null && actionView != null && motion.view() == Motion.CONTENT && content.height() > 0) {
            // Drawing a page also lists its touch targets and action requests: those of the frame on screen stay.
            ArrayList<Target> keptTargets = new ArrayList<>(targets);
            ArrayList<ItemActions.Request> keptRequests = new ArrayList<>(buttonRequests);
            frameNow = android.os.SystemClock.uptimeMillis();
            drawPage(outgoing.beginRecording(getWidth(), getHeight()), page);
            outgoing.endRecording();
            targets.clear(); targets.addAll(keptTargets);
            buttonRequests.clear(); buttonRequests.addAll(keptRequests);
            outgoingRecorded = true;
            motion.slide(page, next, android.os.SystemClock.uptimeMillis());
        }
        page = next; scroll = 0; selection = -1; poller.requestFull(); invalidate();
    }
    private void drawPage(Canvas c, int which) {
        if (which == 0) adventure(c);
        else if (which == 1) inventory(c);
        else fullMap(c);
    }
    /** One more frame only while something moves (Motion.nextFrame): at the display's rate for the short transitions,
     * slower for the marker's glide and the loading mark. */
    private void schedule() {
        boolean pages = motion.view() == Motion.CONTENT;
        long delay = motion.nextFrame(frameNow, pages && page == 0, pages && page != 1);
        removeCallbacks(frame);
        if (delay == 0) postInvalidateOnAnimation();
        else if (delay > 0) postDelayed(frame, delay);
    }
    private void marker(MapRenderer renderer) {
        if (motion.markerKnown()) renderer.marker(motion.markerX(frameNow), motion.markerZ(frameNow), motion.heading(frameNow));
        else renderer.noMarker();
    }
    private float dp(float value) { return value * u; }
    /** Interactive controls are at least 44 real dp tall, whatever the panel unit. */
    private float touch() { return Math.max(dp(48), 44 * density); }
    private RectF rect(float left, float top, float right, float bottom) { return new RectF(left, top, right, bottom); }
    private void panel(Canvas c, RectF box) {
        if (assets.decor(c, "window", box, u)) return;
        paint.setColor(PANEL); c.drawRect(box, paint);
    }
    private void divider(Canvas c, float left, float right, float y) {
        if (!assets.decor(c, "divider", rect(left, y, right, y + dp(1)), u)) {
            paint.setColor(ACCENT_DIM); paint.setStrokeWidth(dp(1)); c.drawLine(left, y, right, y, paint);
        }
    }
    private void tab(Canvas c, Bitmap image, String fallback, RectF bounds, int code, boolean selected) {
        boolean onCategoryBar = code >= 100 && code < 108 && categoryBar;
        if (onCategoryBar) {
            if (selected && !assets.decor(c, "tab_selected", bounds, u)) {
                paint.setColor(GOLD); c.drawRect(bounds.left, bounds.bottom - dp(3), bounds.right, bounds.bottom, paint);
            }
        } else if (!assets.decor(c, selected ? "tab_selected" : "tab", bounds, u)) {
            paint.setColor(selected ? 0x80546654 : 0x40101916); c.drawRect(bounds, paint);
            if (selected) {
                paint.setColor(CURSOR); c.drawRect(bounds.left, bounds.bottom - dp(3), bounds.right, bounds.bottom, paint);
            }
        }
        float side = Math.min(dp(30), bounds.height() - dp(14));
        RectF iconBounds = rect(bounds.centerX() - side / 2, bounds.centerY() - side / 2,
                bounds.centerX() + side / 2, bounds.centerY() + side / 2);
        String part = code < 3 ? PAGE_DECOR[code] : code >= 200 && code < 203 ? LAYER_ICONS[code - 200] : "";
        if (code >= 100 && code < 108 && image != null) {
            if (categorySelectedTint == null || categorySelectedColor != INK) {
                categorySelectedColor = INK;
                categorySelectedTint = new android.graphics.PorterDuffColorFilter(INK, android.graphics.PorterDuff.Mode.SRC_IN);
            }
            paint.setColor(Color.WHITE); paint.setColorFilter(onCategoryBar && !selected ? CATEGORY_INK : categorySelectedTint);
            c.drawBitmap(image, null, iconBounds, paint); paint.setColorFilter(null);
        } else if (!assets.decor(c, part, iconBounds, u)) icon(c, image, iconBounds, fallback, selected ? INK : onCategoryBar ? 0xff26241b : MUTED);
        targets.add(new Target(bounds, code));
    }
    private void text(Canvas c, String text, float x, float baseline, float size, int color, float width) {
        if (c == null || text == null || width <= 0) return;
        String line = text.replace('\n', ' ');
        paint.setTypeface(assets.typeface(line)); paint.setTextSize(size * sp); paint.setColor(color); paint.setTextAlign(Paint.Align.LEFT);
        if (paint.measureText(line) > width) {
            int end = paint.breakText(line, true, Math.max(0, width - paint.measureText("…")), null);
            line = line.substring(0, end) + "…";
        }
        c.drawText(line, x, baseline, paint);
    }
    private float wrap(Canvas c, String text, float x, float top, float width, float size, int color, int maxLines) {
        String rest = text == null ? "" : text.replaceAll("<[^>]+>", "").replace('\n', ' ');
        paint.setTypeface(assets.typeface(rest)); paint.setTextSize(size * sp);
        for (int line = 0; line < maxLines && !rest.isEmpty(); line++) {
            int end = paint.breakText(rest, true, width, null);
            if (end == 0) break;
            if (end < rest.length()) { int space = rest.lastIndexOf(' ', end); if (space > end / 2) end = space; }
            String part = rest.substring(0, end);
            text(c, line == maxLines - 1 ? rest : part, x, top + size * sp, size, color, width);
            top += size * sp * 1.35f; rest = rest.substring(end).stripLeading();
        }
        return top;
    }
    private void button(Canvas c, String title, RectF bounds, int code, boolean selected) {
        if (!assets.decor(c, selected ? "tab_selected" : "tab", bounds, u)) {
            paint.setColor(selected ? 0xff355e4d : PANEL); c.drawRect(bounds, paint);
            paint.setColor(selected ? GREEN : ACCENT_DIM); paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(1));
            c.drawRect(bounds, paint); paint.setStyle(Paint.Style.FILL);
        }
        text(c, title, bounds.left + dp(10), bounds.centerY() + dp(5), 13, INK, bounds.width() - dp(20));
        targets.add(new Target(bounds, code));
    }
    @Override protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (assets == null) return;
        targets.clear(); buttonRequests.clear();
        actionView = poller.actions.state();
        frameNow = android.os.SystemClock.uptimeMillis();
        INK = assets.color("text", 0xffe9e5d5); MUTED = assets.color("textDim", 0xffaaa997);
        GOLD = assets.color("gold", 0xffdcc08a); GREEN = assets.color("accent", 0xff77d7b1);
        PANEL = assets.color("panel", 0xc015211e); CURSOR = assets.color("cursor", 0xffeee8cc);
        BACKGROUND = assets.color("background", 0xff0d1a18); ACCENT_DIM = assets.color("accentDim", 0xff405247);
        float w = getWidth(), h = getHeight(), margin = dp(h < dp(400) ? 10 : 22), tabHeight = dp(52);
        paint.setColor(BACKGROUND); c.drawRect(0, 0, w, h, paint);
        assets.decor(c, "background", rect(0, 0, w, h), u);
        assets.decor(c, "background_pattern", rect(0, 0, w, h), u);
        assets.decor(c, "vignette", rect(0, 0, w, h), u);
        float ornamentWidth = Math.min(w * .24f, h * .45f);
        float leftHeight = ornamentWidth / assets.decorAspect("deco_left", .9f);
        float rightHeight = ornamentWidth / assets.decorAspect("deco_right", .7f);
        assets.decor(c, "deco_left", rect(0, h - leftHeight, ornamentWidth, h), u);
        assets.decor(c, "deco_right", rect(w - ornamentWidth, h - rightHeight, w, h), u);
        assets.decor(c, "footer", rect(margin, h - dp(1), w - margin, h), u);
        float fade = motion.fade(frameNow);
        if (motion.view() != Motion.CONTENT) {
            // Title, loading, unsupported: views of their own on the whole screen, with nothing to touch.
            content.set(margin, margin, w - margin, h - margin);
            int layer = fade < 1 ? c.saveLayerAlpha(0, 0, w, h, Math.round(255 * fade)) : c.save();
            if (content.height() > 0) stateView(c, motion.view());
            c.restoreToCount(layer);
            scrollMax = 0; outgoingRecorded = false;
            schedule();
            return;
        }
        float tabTop = dp(6), tw = Math.min(dp(128), (w - margin * 2) / 3);
        float tabLeft = (w - tw * 3) / 2;
        for (int i = 0; i < 3; i++) tab(c, assets.pageIcon(PAGE_KEYS[i]), PAGE_SYMBOLS[i],
                rect(tabLeft + i * tw, tabTop, tabLeft + (i + 1) * tw, tabTop + tabHeight), i, page == i);
        if (!assets.decor(c, "header_line", rect(margin, dp(64), w - margin, dp(65)), u)) divider(c, margin, w - margin, dp(64));
        content.set(margin, dp(78), w - margin, h - dp(22));
        if (content.height() <= 0) return;
        int save = fade < 1 ? c.saveLayerAlpha(content, Math.round(255 * fade)) : c.save();
        c.clipRect(content);
        float slide = motion.slide(frameNow);
        if (slide < 1 && outgoingRecorded) {
            // The page left behind moves out as recorded, the new one moves in live (its touch targets are already final).
            float span = content.width() * motion.slideDirection();
            c.save(); c.translate(-span * slide, 0); c.drawPicture(outgoing); c.restore();
            c.translate(span * (1 - slide), 0);
        } else outgoingRecorded = false;
        drawPage(c, page);
        c.restoreToCount(save);
        schedule();
    }
    /** Title, loading (also the start, before the game's state can be read) or unsupported: the game's logo over one
     * heading and a few words. Loading adds a turning mark, the only thing here that moves. */
    private void stateView(Canvas c, int view) {
        boolean unsupported = view == Motion.UNSUPPORTED, loading = view == Motion.LOADING;
        boolean starting = loading && !"ready".equals(data.status);
        String heading = unsupported ? "Unsupported game build" : loading ? starting ? "Starting" : "Loading" : "Waiting for a save";
        String words = unsupported ? "The companion cannot read this game version. Your game remains on the main screen."
                : starting ? "Starting the adventure. Load a save on the main screen to connect your Purah Pad."
                : loading ? "Your Purah Pad returns as soon as the game is back."
                : "Choose Continue or load a save on the main screen. Your inventory and map will appear here.";
        float width = Math.min(content.width() - dp(48), dp(500)), x = content.centerX() - width / 2;
        float mark = loading ? dp(52) : 0;
        if (getHeight() < dp(400)) {
            if (loading) loadingMark(c, x + dp(14), content.top + dp(28), dp(10));
            text(c, heading, x + (loading ? dp(36) : 0), content.top + dp(35), 20, INK, width - (loading ? dp(36) : 0));
            wrap(c, unsupported || loading ? words : "Load a supported save on the main screen to connect the companion.",
                    x, content.top + dp(48), width, 13, MUTED, 3);
            return;
        }
        float y = content.centerY() - dp(20) - mark / 2;
        Bitmap logo = assets.ui("logo");
        if (logo != null) {
            float lw = Math.min(width, dp(380)), lh = Math.min(Math.min(dp(160), y - content.top), lw * logo.getHeight() / logo.getWidth());
            lw = lh * logo.getWidth() / logo.getHeight();
            paint.setColor(Color.WHITE); c.drawBitmap(logo, null, rect(content.centerX() - lw / 2, y - lh, content.centerX() + lw / 2, y), paint);
        } else {
            text(c, "THE LEGEND OF ZELDA", x, y - dp(22), 15, GOLD, width);
            text(c, "TEARS OF THE KINGDOM", x, y + dp(10), 25, INK, width);
        }
        if (loading) { loadingMark(c, content.centerX(), y + dp(48), dp(16)); y += mark; }
        assets.decor(c, "title_plate", rect(x - dp(12), y + dp(32), x + width + dp(12), y + dp(78)), u);
        text(c, heading, x, y + dp(62), 23, INK, width);
        wrap(c, words, x, y + dp(82), width, 15, MUTED, 4);
    }
    /** Eight dots on a ring; the leading one steps round eight times a second (Motion.loadingStep). */
    private void loadingMark(Canvas c, float cx, float cy, float radius) {
        int lead = Motion.loadingStep(frameNow);
        paint.setStyle(Paint.Style.FILL);
        for (int i = 0; i < Motion.LOADING_STEPS; i++) {
            int behind = Math.floorMod(lead - i, Motion.LOADING_STEPS);
            double angle = Math.PI * 2 * i / Motion.LOADING_STEPS - Math.PI / 2;
            paint.setColor((GOLD & 0x00ffffff) | Math.max(40, 255 - behind * 36) << 24);
            c.drawCircle(cx + radius * (float)Math.cos(angle), cy + radius * (float)Math.sin(angle), radius * (behind == 0 ? .2f : .15f), paint);
        }
    }
    private void adventure(Canvas c) {
        boolean columns = content.width() >= dp(600);
        float gap = dp(24), leftWidth = columns ? content.width() * .54f : content.width();
        float rightWidth = columns ? content.width() - leftWidth - gap : content.width();
        float rightX = columns ? content.left + leftWidth + gap : content.left;
        float bx = content.left + dp(12), innerWidth = leftWidth - dp(24), halfWidth = innerWidth / 2;
        int heartColumns = Math.max(1, (int)(innerWidth / dp(22)));
        int heartCount = Math.min(60, (Math.max(0, data.player.optInt("maxLife")) + Math.max(0, data.player.optInt("extraLife")) + 3) / 4);
        float heartsHeight = dp(22 * Math.max(1, (heartCount + heartColumns - 1) / heartColumns));
        int wheelColumns = Math.max(1, (int)((halfWidth - dp(8)) / dp(28)));
        int cellColumns = Math.max(1, (int)(halfWidth / dp(25)));
        float capacityHeight = Math.max(dp(24 * Math.max(1, (capacityCount("maxStamina", "extraStamina") + wheelColumns - 1) / wheelColumns)),
                dp(18 * Math.max(1, (capacityCount("maxEnergy", "extraEnergy") + cellColumns - 1) / cellColumns)));
        float gaugeHeight = heartsHeight + capacityHeight + dp(72);
        float cardWidth = Math.min(dp(106), (innerWidth - dp(16)) / 3);
        float gearHeight = cardWidth + dp(58), abilityHeight = dp(112);
        float leftTotal = gaugeHeight + gearHeight + abilityHeight + dp(32);
        float mapSize = Math.min(rightWidth - dp(16), dp(296));
        float armorWidth = Math.min(dp(92), (rightWidth - dp(24)) / 3);
        float rightTotal = mapSize + armorWidth + dp(160);
        float total = columns ? Math.max(leftTotal, rightTotal) : leftTotal + gap + rightTotal;
        scrollMax = Math.max(0, total - content.height()); scroll = Math.min(scroll, scrollMax);
        c.save(); c.clipRect(content); c.translate(0, -scroll);
        float y = content.top + dp(10);
        hearts(c, bx, y, innerWidth);
        float by = y + heartsHeight + dp(16);
        wheels(c, bx, by, halfWidth - dp(8));
        cells(c, bx + halfWidth, by, halfWidth);
        float currencyY = by + capacityHeight + dp(15);
        icon(c, assets.ui("rupee"), rect(bx, currencyY, bx + dp(20), currencyY + dp(24)), "◆", GREEN);
        text(c, number(data.player, "rupees"), bx + dp(28), currencyY + dp(20), 21, INK, halfWidth - dp(32));
        icon(c, assets.ui("poe"), rect(bx + halfWidth, currencyY, bx + halfWidth + dp(20), currencyY + dp(24)), "◈", GREEN);
        text(c, number(data.player, "poes"), bx + halfWidth + dp(28), currencyY + dp(20), 21, INK, halfWidth - dp(32));
        y = content.top + gaugeHeight;
        divider(c, bx, bx + innerWidth, y);
        y += dp(16);
        JSONArray arrows = Snapshot.array(Snapshot.object(data.root, "pouch"), "arrows");
        JSONObject arrow = arrows.optJSONObject(0);
        for (int kind = 0; kind < 3; kind++) {
            JSONArray list = Snapshot.array(Snapshot.object(data.root, "pouch"), Snapshot.CATEGORIES[kind]);
            JSONObject equipped = null;
            int index = -1;
            for (int i = 0; i < list.length(); i++) {
                JSONObject item = list.optJSONObject(i);
                if (item != null && item.optBoolean("equipped")) { equipped = item; index = i; break; }
            }
            equipment(c, equipped, CATEGORY_SYMBOLS[kind], bx + kind * (cardWidth + dp(8)), y, cardWidth,
                    kind == 1 && arrow != null ? "×" + number(arrow, "count") : "", CODE_JUMP + kind * 1000 + index);
        }
        y += gearHeight;
        divider(c, bx, bx + innerWidth, y);
        y += dp(18);
        String ability = data.player.optString("ability", "None");
        icon(c, assets.ui("ability_" + ability), rect(bx, y, bx + dp(40), y + dp(40)), "✥", GREEN);
        text(c, assets.label("abilities", ability, abilityName(ability)), bx + dp(52), y + dp(27), 19, INK, innerWidth - dp(52));
        JSONArray sages = Snapshot.array(data.root, "sages");
        float sy = y + dp(58), sw = innerWidth / 5;
        for (int i = 0; i < SAGE_IDS.length; i++) {
            JSONObject sage = new JSONObject();
            for (int s = 0; s < sages.length(); s++) {
                JSONObject found = sages.optJSONObject(s);
                if (found != null && SAGE_IDS[i].equals(found.optString("id"))) sage = found;
            }
            boolean joined = sage.optBoolean("joined");
            int save = c.saveLayerAlpha(rect(bx + i * sw, sy, bx + (i + 1) * sw, sy + dp(42)), joined ? 255 : 70);
            icon(c, assets.ui("sage_" + SAGE_IDS[i]), rect(bx + i * sw + dp(6), sy, bx + i * sw + dp(38), sy + dp(32)), "◇", GREEN);
            if (sage.optBoolean("summoned")) { paint.setColor(GOLD); c.drawCircle(bx + i * sw + dp(40), sy + dp(3), dp(3), paint); }
            if (sage.optDouble("cooldown", 0) > 0) text(c, Math.round(sage.optDouble("cooldown")) + "s", bx + i * sw, sy + dp(42), 10, GOLD, sw);
            c.restoreToCount(save);
        }
        JSONObject buff = Snapshot.object(data.player, "cookBuff");
        if (buff.has("id") || buff.has("type")) {
            String type = buff.optString("type");
            icon(c, assets.ui("effect_" + type), rect(bx, sy + dp(48), bx + dp(20), sy + dp(68)), "✦", GOLD);
            text(c, (type.isEmpty() ? "Meal effect" : assets.label("effects", type, AssetStore.humanize(type)))
                    + " " + buff.optInt("level") + " · " + duration(buff.optDouble("time")), bx + dp(28), sy + dp(65), 12, GOLD, innerWidth - dp(28));
        }
        float ry = columns ? content.top + dp(6) : content.top + leftTotal + gap;
        float mx = rightX + rightWidth / 2;
        marker(minimap);
        minimap.draw(c, rect(mx - mapSize / 2, ry, mx + mapSize / 2, ry + mapSize), data, true);
        text(c, coordinates(), rightX + dp(12), ry + mapSize + dp(26), 14, INK, rightWidth - dp(24));
        float bw = (rightWidth - dp(32)) / 2;
        button(c, "− Map", rect(rightX + dp(12), ry + mapSize + dp(40), rightX + dp(12) + bw, ry + mapSize + dp(88)), 20, false);
        button(c, "+ Menu", rect(rightX + dp(20) + bw, ry + mapSize + dp(40), rightX + rightWidth - dp(12), ry + mapSize + dp(88)), 21, false);
        float armorY = ry + mapSize + dp(110);
        divider(c, rightX + dp(12), rightX + rightWidth - dp(12), armorY - dp(10));
        JSONArray worn = data.items(3);
        float armorX = rightX + (rightWidth - armorWidth * 3 - dp(16)) / 2;
        int shown = 0;
        for (int i = 0; i < worn.length() && shown < 3; i++) {
            JSONObject item = worn.optJSONObject(i);
            if (item == null || !item.optBoolean("equipped")) continue;
            equipment(c, item, "◇", armorX + shown * (armorWidth + dp(8)), armorY, armorWidth, "", CODE_JUMP + 3 * 1000 + i);
            shown++;
        }
        if (shown == 0) text(c, data.root.optJSONObject("pouch") != null ? "No equipped armor" : "Equipment unavailable",
                rightX + dp(12), armorY + dp(34), 13, MUTED, rightWidth - dp(24));
        c.restore();
        for (int i = 3; i < targets.size(); i++) targets.get(i).box.offset(0, -scroll);
        scrollIndicator(c);
    }
    private int capacityCount(String baseKey, String extraKey) {
        return (int)Math.min(32, Math.ceil(Math.max(0, data.player.optDouble(baseKey, 0)) / 1000)
                + Math.ceil(Math.max(0, data.player.optDouble(extraKey, 0)) / 1000));
    }
    private void wheels(Canvas c, float x, float y, float width) {
        if (!data.player.has("maxStamina")) { text(c, "—", x, y + dp(18), 14, MUTED, width); return; }
        double base = Math.max(0, data.player.optDouble("maxStamina", 0)) / 1000;
        double extra = Math.max(0, data.player.optDouble("extraStamina", 0)) / 1000;
        int baseCount = (int)Math.ceil(base), count = capacityCount("maxStamina", "extraStamina");
        int cols = Math.max(1, (int)(width / dp(28)));
        paint.setStyle(Paint.Style.FILL);
        for (int i = 0; i < count; i++) {
            float cx = x + dp(11) + i % cols * dp(28), cy = y + dp(11) + i / cols * dp(24);
            RectF wheel = rect(cx - dp(10), cy - dp(10), cx + dp(10), cy + dp(10));
            assets.decor(c, "gauge_stamina_empty", wheel, u);
            int color = i < baseCount ? assets.color("stamina", GREEN) : GOLD;
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(3));
            paint.setColor((color & 0x00ffffff) | 0x40000000); c.drawOval(wheel, paint);
            paint.setStyle(Paint.Style.FILL);
            float fraction = (float)Math.min(1, i < baseCount ? base - i : extra - (i - baseCount));
            if (fraction < 1 || !assets.decor(c, i < baseCount ? "gauge_stamina" : "gauge_stamina_extra", wheel, u)) {
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(3)); paint.setStrokeCap(Paint.Cap.ROUND);
                paint.setColor(color); c.drawArc(wheel, -90, fraction * 360, false, paint);
                paint.setStrokeCap(Paint.Cap.BUTT); paint.setStyle(Paint.Style.FILL);
            }
        }
        paint.setStyle(Paint.Style.FILL);
        if (count == 0) text(c, "0 wheels", x, y + dp(18), 12, MUTED, width);
    }
    private void cells(Canvas c, float x, float y, float width) {
        if (!data.player.has("maxEnergy")) { text(c, "—", x, y + dp(18), 14, MUTED, width); return; }
        double base = Math.max(0, data.player.optDouble("maxEnergy", 0)) / 1000;
        double extra = Math.max(0, data.player.optDouble("extraEnergy", 0)) / 1000;
        int baseCount = (int)Math.ceil(base), count = capacityCount("maxEnergy", "extraEnergy");
        int cols = Math.max(1, (int)(width / dp(25)));
        for (int i = 0; i < count; i++) {
            float bx = x + i % cols * dp(25), by = y + dp(5) + i / cols * dp(18);
            RectF cell = rect(bx, by, bx + dp(20), by + dp(12));
            if (!assets.decor(c, "gauge_battery_empty", cell, u)) {
                paint.setColor(0xff4a5a4d); c.drawRect(cell, paint);
                c.drawRect(bx + dp(20), by + dp(4), bx + dp(22), by + dp(8), paint);
            }
            float fraction = (float)Math.min(1, i < baseCount ? base - i : extra - (i - baseCount));
            c.save(); c.clipRect(cell.left, cell.top, cell.left + cell.width() * fraction, cell.bottom);
            if (!assets.decor(c, i < baseCount ? "gauge_battery" : "gauge_battery_extra", cell, u)) {
                Bitmap battery = assets.ui("battery");
                if (battery != null) icon(c, battery, cell, "", GREEN);
                else { paint.setColor(i < baseCount ? assets.color("energy", GREEN) : GOLD); c.drawRect(bx + dp(2), by + dp(2), bx + dp(18), by + dp(10), paint); }
            }
            c.restore();
        }
        if (count == 0) text(c, "0 cells", x, y + dp(18), 12, MUTED, width);
    }
    private static String duration(double seconds) {
        long whole = Math.max(0, (long)seconds);
        return String.format(Locale.ROOT, "%02d:%02d", whole / 60, whole % 60);
    }
    private void hearts(Canvas c, float x, float y, float width) {
        int max = Math.max(0, data.player.optInt("maxLife", 0)), life = Math.max(0, data.player.optInt("life", 0));
        int broken = Math.max(0, data.player.optInt("breakLife", 0)), extra = Math.max(0, data.player.optInt("extraLife", 0));
        if (!data.player.has("maxLife")) { text(c, "Health unavailable", x, y + dp(18), 14, MUTED, width); return; }
        int columns = Math.max(1, (int)(width / dp(22))), total = Math.min(60, (max + extra + 3) / 4);
        for (int i = 0; i < total; i++) {
            float hx = x + i % columns * dp(22), hy = y + i / columns * dp(22);
            boolean yellow = i * 4 >= max;
            int filled = Math.max(0, Math.min(4, (yellow ? max + extra : life) - i * 4));
            boolean gloom = !yellow && i * 4 + 4 > max - broken;
            RectF box = rect(hx, hy, hx + dp(18), hy + dp(18));
            // A change of the hearts shows for a moment: the ones that changed swell and settle (Motion.heartScale).
            float swell = motion.heartScale(i, frameNow);
            c.save();
            if (swell != 1) c.scale(swell, swell, box.centerX(), box.centerY());
            heart(c, box, 0xff404941);
            if (filled > 0) { c.save(); c.clipRect(box.left, box.top, box.left + box.width() * filled / 4, box.bottom); heart(c, box, yellow ? 0xffead375 : 0xffe7796b); c.restore(); }
            if (gloom) {
                Bitmap damaged = assets.ui("heart_broken");
                if (damaged != null) {
                    paint.setColor(Color.WHITE); paint.setColorFilter(HEART_BROKEN);
                    c.drawBitmap(damaged, null, box, paint); paint.setColorFilter(null);
                } else {
                    heart(c, box, 0xff462e35); paint.setColor(0xffa45660); paint.setStrokeWidth(dp(1.5f));
                    c.drawLine(hx + dp(9), hy + dp(3), hx + dp(6), hy + dp(9), paint);
                    c.drawLine(hx + dp(6), hy + dp(9), hx + dp(11), hy + dp(14), paint);
                }
            }
            c.restore();
        }
    }
    private void heart(Canvas c, RectF b, int color) {
        Bitmap image = assets.ui(color == 0xffead375 ? "heart_yellow" : color == 0xffe7796b ? "heart" : "heart_empty");
        if (image != null && color != 0xff462e35) {
            paint.setColor(Color.WHITE);
            paint.setColorFilter(color == 0xffe7796b ? HEART_RED : color == 0xffead375 ? HEART_GOLD : HEART_EMPTY);
            c.drawBitmap(image, null, b, paint); paint.setColorFilter(null); return;
        }
        float x = b.left, y = b.top, w = b.width(), h = b.height(); path.reset();
        path.moveTo(x + w / 2, y + h * .9f); path.cubicTo(x - w * .4f, y + h * .3f, x + w * .1f, y - h * .3f, x + w / 2, y + h * .22f);
        path.cubicTo(x + w * .9f, y - h * .3f, x + w * 1.4f, y + h * .3f, x + w / 2, y + h * .9f);
        paint.setColor(color); c.drawPath(path, paint);
    }
    private void meter(Canvas c, float x, float y, float width, double value, double maximum, int color) {
        RectF bar = rect(x, y, x + width, y + dp(3));
        paint.setColor(0xff18211b); c.drawRect(bar, paint);
        bar.right = x + width * (float)Math.max(0, Math.min(1, value / Math.max(1, maximum)));
        paint.setColor(color); c.drawRect(bar, paint);
    }
    private void equipment(Canvas c, JSONObject item, String fallback, float x, float y, float width, String footer, int code) {
        RectF slot = rect(x, y, x + width, y + width);
        slot(c, slot);
        if (item == null) {
            icon(c, null, rect(x + width * .25f, y + width * .25f, x + width * .75f, y + width * .75f), fallback, MUTED);
            return;
        }
        String actor = item.optString("name");
        icon(c, assets.icon(actor), rect(x + dp(8), y + dp(8), x + width - dp(8), y + width - dp(8)), "◇", GOLD);
        equipped(c, slot);
        if (!footer.isEmpty()) text(c, footer, x + dp(6), y + width - dp(8), 12, INK, width - dp(12));
        wrap(c, assets.itemName(actor), x, y + width + dp(5), width, 11, INK, 2);
        // Jumps to the item on the Inventory page; the card plus its name is at least one touch target tall.
        targets.add(new Target(rect(x, y, x + width, Math.max(y + width + dp(30), y + touch())), code));
    }
    private void slot(Canvas c, RectF bounds) {
        if (!assets.decor(c, "slot", bounds, u, 165)) {
            paint.setColor(0x7c1c241f); c.drawRect(bounds, paint);
        }
        paint.setColor((INK & 0x00ffffff) | 0x50000000); paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(.7f));
        c.drawRect(bounds, paint); paint.setStyle(Paint.Style.FILL);
    }
    private void equipped(Canvas c, RectF bounds) {
        RectF mark = rect(bounds.right - dp(22), bounds.top + dp(4), bounds.right - dp(4), bounds.top + dp(22));
        text(c, "◆", mark.left, mark.bottom - dp(2), 13, INK, mark.width());
    }
    private void icon(Canvas c, Bitmap image, RectF box, String fallback, int color) {
        if (image != null) { paint.setColor(Color.WHITE); c.drawBitmap(image, null, box, paint); }
        else text(c, fallback, box.left + box.width() * .15f, box.top + box.height() * .78f, Math.min(26, box.height() / sp * .8f), color, box.width());
    }
    private void inventory(Canvas c) {
        float gap = dp(8), ch = dp(50);
        int tabCols = content.width() >= dp(440) ? 8 : 4;
        float cw = (content.width() - gap * (tabCols - 1)) / tabCols;
        int tabRows = 8 / tabCols;
        categoryBar = assets.decor(c, "tab_bar", rect(content.left, content.top, content.right, content.top + ch * tabRows), u);
        for (int i = 0; i < 8; i++) {
            float x = content.left + i % tabCols * (cw + gap), y = content.top + i / tabCols * (ch + gap);
            tab(c, assets.ui("cat_" + Snapshot.CATEGORIES[i]), CATEGORY_SYMBOLS[i], rect(x, y, x + cw, y + ch), 100 + i, category == i);
        }
        float titleY = content.top + tabRows * (ch + gap) + dp(30);
        text(c, assets.label("ui", Snapshot.CATEGORIES[category], CATEGORY_NAMES[category]), content.left + dp(8), titleY, 25, INK, content.width());
        divider(c, content.left, content.right, titleY + dp(12));
        float top = titleY + dp(28);
        JSONArray items = data.items(category);
        if (items.length() > 0 && (selection < 0 || selection >= items.length())) { selection = 0; detailScroll = 0; }
        JSONObject selected = items.optJSONObject(selection);
        boolean columns = content.width() >= dp(600);
        float detailWidth = columns ? Math.min(dp(340), content.width() * .38f) : content.width();
        float actionHeight = selected == null ? 0 : itemActions(null, 0, 0, detailWidth - dp(36), selected, selection);
        float stackedDetail = Math.min(dp(218) + actionHeight, content.height() * (actionHeight > 0 ? .55f : .38f));
        RectF grid = rect(content.left, top, columns ? content.right - detailWidth - dp(24) : content.right,
                columns ? content.bottom : content.bottom - stackedDetail - dp(12));
        RectF detail = columns ? rect(grid.right + dp(24), top, content.right, content.bottom)
                : rect(content.left, grid.bottom + dp(12), content.right, content.bottom);
        // Action buttons stay pinned under the scrollable item information.
        RectF info = new RectF(detail);
        if (actionHeight > 0) info.bottom = Math.max(detail.top + dp(56), detail.bottom - actionHeight);
        detailBounds.set(info);
        if (!assets.softDecor(c, "info_panel", detail, u)) panel(c, detail);
        JSONObject pouch = data.root.optJSONObject("pouch");
        boolean categoryKnown = pouch != null && (pouch.optJSONArray(Snapshot.CATEGORIES[category]) != null || category == 1 && pouch.optJSONArray("arrows") != null);
        double durabilityScale = 1;
        for (int i = 0; i < items.length(); i++) {
            JSONObject item = items.optJSONObject(i);
            if (item != null) durabilityScale = Math.max(durabilityScale, item.optDouble("life", 0));
        }
        float inset = dp(6);
        int cols = Math.max(1, Math.min(6, (int)((grid.width() - inset * 2 + gap) / dp(88))));
        float cell = Math.min(dp(118), (grid.width() - inset * 2 - gap * (cols - 1)) / cols);
        int rows = (items.length() + cols - 1) / cols;
        scrollMax = Math.max(0, rows * (cell + gap) + inset * 2 - grid.height());
        if (revealSelection && selection >= 0) scroll = selection / cols * (cell + gap) + inset - (grid.height() - cell) / 2;
        revealSelection = false;
        scroll = Math.max(0, Math.min(scrollMax, scroll));
        c.save(); c.clipRect(grid);
        for (int i = 0; i < items.length(); i++) {
            JSONObject item = items.optJSONObject(i); if (item == null) continue;
            float x = grid.left + inset + i % cols * (cell + gap), y = grid.top + inset + i / cols * (cell + gap) - scroll;
            RectF box = rect(x, y, x + cell, y + cell);
            if (box.bottom < grid.top || box.top > grid.bottom) continue;
            slot(c, box);
            String actor = item.optString("name");
            icon(c, assets.icon(actor), rect(x + dp(9), y + dp(5), x + cell - dp(9), y + cell - dp(10)), "◇", GOLD);
            if (item.has("count")) text(c, "×" + number(item, "count"), x + dp(7), y + cell - dp(10), 14, INK, cell - dp(14));
            if (item.optBoolean("equipped")) equipped(c, box);
            String fuse = item.optString("fuse", "");
            if (!fuse.isEmpty() && !fuse.equals("null")) icon(c, assets.icon(fuse),
                    rect(x + cell - dp(31), y + cell - dp(34), x + cell - dp(5), y + cell - dp(8)), "+", GREEN);
            if (item.has("life")) {
                // No invented maximum: absent maxLife compares durability within this category.
                meter(c, x + dp(8), y + cell - dp(6), cell - dp(16), item.optDouble("life", 0),
                        item.optDouble("maxLife", durabilityScale), item.optDouble("life", 0) < 5 ? 0xffdc806b : INK);
            }
            if (i == selection) {
                if (!assets.decor(c, "slot_selected", box, u)) {
                    paint.setColor(CURSOR); paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(2)); c.drawRect(box, paint); paint.setStyle(Paint.Style.FILL);
                }
            }
            RectF hit = new RectF(box); if (hit.intersect(grid)) targets.add(new Target(hit, 1000 + i));
        }
        c.restore();
        if (selected != null) {
            // Keep scrolled text clear of the rule above the pinned actions.
            itemDetail(c, actionHeight > 0 ? rect(info.left, info.top, info.right, info.bottom - dp(8)) : info, selected);
            if (actionHeight > 0) {
                divider(c, detail.left + dp(18), detail.right - dp(18), info.bottom);
                itemActions(c, detail.left + dp(18), info.bottom, detail.width() - dp(36), selected, selection);
            }
        }
        else {
            detailScroll = detailScrollMax = 0;
            wrap(c, categoryKnown ? "No items in this category" : "Pouch temporarily unavailable",
                    detail.left + dp(18), detail.top + dp(20), detail.width() - dp(36), 16, MUTED, 4);
        }
        scrollIndicator(c);
    }
    /** The selected item's game actions: state banner, buttons, disabled reasons and the last result. With c == null
     * this only measures. Returns the height used below top. */
    private float itemActions(Canvas c, float x, float top, float width, JSONObject item, int index) {
        JSONArray list = item.optJSONArray("actions");
        if (list == null) return 0;
        JSONObject state = Snapshot.object(data.root, "actionState");
        String categoryKey = data.itemCategory(category, index), actor = item.optString("name");
        int slot = item.optInt("slot", -1), partition = data.root.optInt("partition", -1);
        long generation = data.root.optLong("pouchGen", -1);
        boolean identity = slot >= 0 && partition >= 0 && generation >= 0 && !actor.isEmpty();
        ItemActions.State current = actionView;
        boolean settled = current.phase == ItemActions.Phase.DONE || current.phase == ItemActions.Phase.REFUSED
                || current.phase == ItemActions.Phase.FAILED;
        // The result stays visible in its category even when the item now offers nothing (an equipped weapon).
        boolean showResult = settled && current.request != null && (current.request.category.equals(Snapshot.CATEGORIES[category])
                || category == 1 && current.request.category.equals("arrows"));
        if (list.length() == 0 && !showResult) return 0;
        boolean anyEnabled = false;
        StringBuilder menuVerbs = new StringBuilder();
        for (int k = 0; k < list.length(); k++) {
            JSONObject action = list.optJSONObject(k);
            if (action == null) continue;
            if (action.optBoolean("enabled")) anyEnabled = true;
            else if ("openInventory".equals(action.optString("reason")))
                menuVerbs.append(menuVerbs.length() > 0 ? " or " : "").append(actionLabel(action.optString("id")).toLowerCase(Locale.ROOT));
        }
        String stateReason = state.optString("reason", "");
        boolean blocked = list.length() > 0 && state.has("ok") && !state.optBoolean("ok") && !anyEnabled;
        boolean shortcut = !blocked && menuVerbs.length() > 0;
        String banner = blocked ? reasonText(stateReason, "") : shortcut ? "Open the inventory (+) to " + menuVerbs : null;
        float y = top + dp(12), gap = dp(8), bh = touch();
        if (banner != null) {
            float textTop = y + dp(8);
            float bottom = wrap(null, banner, x + dp(12), textTop, width - dp(24), 14, GOLD, 3);
            RectF plate = rect(x, y, x + width, Math.max(bottom + dp(6), y + dp(36)));
            if (c != null) {
                // A gold rule marks a notice; plates are reserved for things that can be tapped.
                paint.setColor(GOLD); c.drawRect(plate.left, plate.top + dp(4), plate.left + dp(3), plate.bottom - dp(4), paint);
                wrap(c, banner, x + dp(12), textTop, width - dp(24), 14, GOLD, 3);
            }
            y = plate.bottom + gap;
        }
        boolean busy = current.busy();
        float bx = x, rowTop = y;
        int count = list.length() + (shortcut ? 1 : 0);
        for (int k = 0; k < count; k++) {
            boolean menu = shortcut && k == 0;
            JSONObject action = menu ? null : list.optJSONObject(k - (shortcut ? 1 : 0));
            if (!menu && action == null) continue;
            String id = menu ? "" : action.optString("id");
            String label = menu ? "+ Menu" : actionLabel(id);
            boolean progress = !menu && busy && current.request.action.equals(id) && current.request.sameItem(categoryKey, slot, actor);
            paint.setTypeface(assets.typeface(label)); paint.setTextSize(14 * sp);
            float bw = Math.min(width, Math.max(Math.max(dp(88), bh), paint.measureText(label) + dp(progress ? 64 : 36)));
            if (bx > x && bx + bw > x + width + 1) { bx = x; rowTop += bh + gap; }
            RectF box = rect(bx, rowTop, bx + bw, rowTop + bh);
            bx += bw + gap;
            if (c == null) continue;
            boolean enabled = menu || action.optBoolean("enabled") && identity;
            int code = -1;
            if (menu) code = stale ? -1 : 21;
            else if (enabled && !busy && !stale) {
                code = CODE_ACTION + buttonRequests.size();
                buttonRequests.add(new ItemActions.Request(id, categoryKey, slot, actor, partition, generation));
            }
            actionButton(c, label, box, code, menu || progress || enabled && !busy, progress);
        }
        if (count > 0) y = rowTop + bh;
        for (int k = 0; k < list.length(); k++) {
            JSONObject action = list.optJSONObject(k);
            if (action == null || action.optBoolean("enabled")) continue;
            String reason = action.optString("reason", ""), id = action.optString("id");
            if (blocked && reason.equals(stateReason) || shortcut && reason.equals("openInventory")) continue;
            y = wrap(c, actionLabel(id) + ": " + reasonText(reason, actionLabel(id).toLowerCase(Locale.ROOT)),
                    x, y + dp(6), width, 12, MUTED, 3);
        }
        if (anyEnabled && !identity) y = wrap(c, "Waiting for the item's details from the game", x, y + dp(6), width, 12, MUTED, 2);
        if (busy && list.length() > 0 && !current.request.sameItem(categoryKey, slot, actor))
            y = wrap(c, "Waiting for the game: " + actionLabel(current.request.action) + " " + assets.itemName(current.request.actor),
                    x, y + dp(6), width, 12, MUTED, 2);
        if (showResult) {
            boolean done = current.phase == ItemActions.Phase.DONE;
            String what = current.request.sameItem(categoryKey, slot, actor) ? "" : " · " + assets.itemName(current.request.actor);
            String line = done ? actionDone(current.request.action) + what
                    : (current.phase == ItemActions.Phase.REFUSED ? "Refused: " : "Didn't work: ")
                    + reasonText(current.reason, actionLabel(current.request.action).toLowerCase(Locale.ROOT)) + what;
            y = wrap(c, (done ? "✓ " : "✕ ") + line, x, y + dp(8), width, 13, done ? GREEN : 0xffdc806b, 3);
        }
        return y + dp(14) - top;
    }
    private void actionButton(Canvas c, String label, RectF box, int code, boolean active, boolean progress) {
        int save = c.saveLayerAlpha(box, active ? 255 : 120);
        if (!assets.decor(c, progress ? "tab_selected" : active ? "title_plate" : "tab", box, u)) {
            paint.setColor(progress ? 0xff355e4d : PANEL); c.drawRect(box, paint);
            paint.setColor(progress ? GREEN : ACCENT_DIM); paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(1));
            c.drawRect(box, paint); paint.setStyle(Paint.Style.FILL);
        }
        paint.setTypeface(assets.typeface(label)); paint.setTextSize(14 * sp);
        float left = box.left + dp(progress ? 44 : 14), textWidth = Math.min(paint.measureText(label), box.right - dp(14) - left);
        float tx = Math.max(left, box.centerX() - textWidth / 2);
        if (progress) {
            // Bounded animation: only while this request waits for the game.
            float r = dp(9), cx = box.left + dp(24), cy = box.centerY();
            float angle = android.os.SystemClock.uptimeMillis() % 1000 * .36f;
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(2.5f)); paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setColor((INK & 0x00ffffff) | 0x40000000); c.drawCircle(cx, cy, r, paint);
            paint.setColor(GOLD); c.drawArc(rect(cx - r, cy - r, cx + r, cy + r), angle, 100, false, paint);
            paint.setStrokeCap(Paint.Cap.BUTT); paint.setStyle(Paint.Style.FILL);
            postInvalidateOnAnimation();
        }
        text(c, label, tx, box.centerY() + 5 * sp, 14, INK, box.right - dp(10) - tx);
        c.restoreToCount(save);
        if (code >= 0) targets.add(new Target(box, code));
    }
    private static String actionLabel(String id) {
        for (int i = 0; i < ACTION_IDS.length; i++) if (ACTION_IDS[i].equals(id)) return ACTION_LABELS[i];
        return AssetStore.humanize(id);
    }
    private static String actionDone(String id) {
        for (int i = 0; i < ACTION_IDS.length; i++) if (ACTION_IDS[i].equals(id)) return ACTION_DONE[i];
        return "Done";
    }
    /** Native reason codes (actionState, per-action and result) in plain words; unknown codes are humanized. */
    private static String reasonText(String code, String verb) {
        switch (code == null ? "" : code) {
            case "": return "Not possible right now";
            case "unsupported": return "Not supported on this game build";
            case "title": return "Load a save first";
            case "loading": return "Wait for loading to finish";
            case "noPlayer": return "Link isn't in the world right now";
            case "event": return "In a cutscene";
            case "menu": return "Close the game menu first";
            case "saving": return "The game is saving";
            case "stale": return "The game isn't running right now";
            case "notAvailableYet": return "Not available from the companion yet";
            case "weaponLocked": return "Weapons can't be changed right now";
            case "notAiming": return "Aim your bow to fuse";
            case "aiming": return "Not while aiming";
            case "noArrows": return "No arrows left";
            case "fullHearts": return "Your hearts are already full";
            case "notEdible": return "This can't be eaten";
            case "batteryFull": return "Your battery is already full";
            case "noBattery": return "You don't have the battery yet";
            case "noStock": return "None left";
            case "cannotHold": return "This can't be held";
            case "openInventory": return "Open the inventory (+) to " + verb;
            case "infoPending": return "Reading the item's details";
            case "itemMoved": return "The item moved; try again";
            case "gameRefused": return "The game didn't allow it";
            case "timeout": case "noAnswer": return "No response from the game";
            case "teardown": return "The game closed";
            case "noEffect": return "Nothing happened";
            case "busy": return "Another action is still running";
            case "notNow": return "Not possible right now";
            case "invalid": return "The request was not valid";
            default: return AssetStore.humanize(code);
        }
    }
    private void itemDetail(Canvas c, RectF bounds, JSONObject item) {
        c.save(); c.clipRect(bounds); c.translate(0, -detailScroll);
        float x = bounds.left + dp(18), y = bounds.top + dp(16), width = bounds.width() - dp(36);
        String actor = item.optString("name");
        y = wrap(c, assets.itemName(actor), x, y, width, 22, INK, 3) + dp(12);
        divider(c, x, x + width, y);
        y += dp(14);
        String description = assets.itemDescription(actor);
        if (!description.isEmpty()) y = wrap(c, description, x, y, width, 14, INK, 60) + dp(16);
        String fuse = item.optString("fuse", "");
        boolean fused = !fuse.isEmpty() && !fuse.equals("null");
        for (String key : ITEM_STATS) {
            if (!item.has(key)) continue;
            if (key.equals("fuseLife") && fused) {
                y = fuseDetail(c, item, fuse, x, y, width);
                continue;
            }
            String label = key.equals("life") ? "Durability" : key.equals("fuseLife") ? "Fuse durability"
                    : key.equals("heal") ? "Recovery (¼ ♥)" : key.equals("count") ? "Quantity" : key.equals("level") ? "Effect level"
                    : key.equals("time") ? "Duration" : key.equals("price") ? "Price" : AssetStore.humanize(key);
            Bitmap stat = assets.ui(key.equals("heal") ? "heart" : key.equals("price") ? "rupee"
                    : key.equals("life") ? "cat_" + Snapshot.CATEGORIES[Math.min(category, 2)]
                    : key.equals("fuseLife") ? "ability_OneTouchBond" : "stat_" + key);
            // Quantity and level have no game glyph: no icon rather than a placeholder dot.
            icon(c, stat, rect(x, y, x + dp(20), y + dp(20)), key.equals("life") ? "◇" : key.equals("time") ? "◷" : "", GOLD);
            String value = key.equals("time") ? duration(item.optDouble("time", 0)) : number(item, key);
            y = Math.max(y + dp(27), wrap(c, label + "  " + value, x + dp(28), y, width - dp(28), 13, INK, 3)) + dp(5);
        }
        if (fused && !item.has("fuseLife")) y = fuseDetail(c, item, fuse, x, y, width);
        JSONObject effect = item.optJSONObject("effect");
        String type = effect == null ? item.optString("effect", "") : effect.optString("type");
        if (!type.isEmpty()) {
            icon(c, assets.ui("effect_" + type), rect(x, y, x + dp(22), y + dp(22)), "✦", GOLD);
            y = Math.max(y + dp(28), wrap(c, assets.label("effects", type, AssetStore.humanize(type))
                    + (effect == null ? "" : "  " + effect.optString("value")), x + dp(30), y, width - dp(30), 13, GOLD, 4)) + dp(8);
        }
        detailScrollMax = Math.max(0, y - bounds.bottom + dp(16));
        c.restore();
    }
    private float fuseDetail(Canvas c, JSONObject item, String fuse, float x, float y, float width) {
        float top = y;
        icon(c, assets.icon(fuse), rect(x, y + dp(3), x + dp(34), y + dp(37)), "+", GOLD);
        y = wrap(c, assets.itemName(fuse), x + dp(44), y, width - dp(44), 13, INK, 3);
        if (item.has("fuseLife")) y = wrap(c, "Fuse durability  " + number(item, "fuseLife"),
                x + dp(44), y + dp(3), width - dp(44), 12, MUTED, 3);
        return Math.max(top + dp(42), y) + dp(8);
    }
    private void fullMap(Canvas c) {
        float bh = dp(50), bw = Math.min(dp(82), content.width() / 4);
        String selected = map.automaticLayer ? data.player.optString("layer", "Ground") : map.layer;
        for (int i = 0; i < LAYERS.length; i++) tab(c, assets.ui(LAYER_ICONS[i]), LAYER_SYMBOLS[i],
                rect(content.left + i * bw, content.top, content.left + (i + 1) * bw, content.top + bh), 200 + i, LAYERS[i].equals(selected));
        tab(c, assets.ui("map_player"), "⌖", rect(content.right - bw, content.top, content.right, content.top + bh), 203, map.following);
        text(c, selected.equals("UnderGround") ? "Depths" : selected, content.left + dp(8), content.top + bh + dp(30), 24, INK, content.width() * .5f);
        String position = coordinates();
        paint.setTypeface(assets.typeface(position)); paint.setTextSize(11 * sp);
        float coordinateWidth = Math.min(paint.measureText(position), content.width() * .55f - dp(8));
        text(c, position, content.right - dp(8) - coordinateWidth, content.top + bh + dp(28), 11, MUTED, coordinateWidth);
        mapBounds.set(content.left, content.top + bh + dp(46), content.right, content.bottom);
        marker(map);
        map.draw(c, mapBounds, data, false);
        scrollMax = 0;
    }
    private void scrollIndicator(Canvas c) {
        if (scrollMax <= 0) return;
        float track = content.height(), thumb = Math.max(dp(24), track * track / (track + scrollMax));
        float y = content.top + (track - thumb) * scroll / scrollMax;
        assets.decor(c, "scrollbar_track", rect(content.right - dp(4), content.top, content.right, content.bottom), u);
        if (!assets.decor(c, "scrollbar_thumb", rect(content.right - dp(4), y, content.right, y + thumb), u)) {
            paint.setColor(GOLD); c.drawRect(content.right - dp(3), y, content.right, y + thumb, paint);
        }
    }
    private String coordinates() {
        double x = data.coordinate(0), y = data.coordinate(1), z = data.coordinate(2);
        if (!Double.isFinite(x) || !Double.isFinite(y) || !Double.isFinite(z)) return "Position unavailable";
        return String.format(Locale.ROOT, "%+.0f  %+.0f  %+.0f", x, y, z);
    }
    private static String number(JSONObject value, String key) {
        if (!value.has(key) || value.isNull(key)) return "—";
        double n = value.optDouble(key, Double.NaN);
        return Double.isFinite(n) ? String.format(Locale.ROOT, "%,.0f", n) : "—";
    }
    private static String abilityName(String key) {
        switch (key) {
            case "UltraHand": return "Ultrahand"; case "OneTouchBond": return "Fuse"; case "CeilingClipper": return "Ascend";
            case "ReverseRecorder": return "Recall"; case "AutoBuilder": return "Autobuild"; case "None": return "None";
            default: return AssetStore.humanize(key);
        }
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if (motion.view() != Motion.CONTENT) { pressed = -1; dragging = false; return true; }   // nothing to touch there
        scaleDetector.onTouchEvent(event);
        float x = event.getX(), y = event.getY();
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = lastX = x; downY = lastY = y; dragging = false; pressed = -1;
                for (int i = targets.size() - 1; i >= 0; i--) if (targets.get(i).box.contains(x, y) && (i < 3 || content.contains(x, y))) { pressed = targets.get(i).code; break; }
                return true;
            case MotionEvent.ACTION_POINTER_DOWN: dragging = true; return true;
            case MotionEvent.ACTION_MOVE:
                if (Math.hypot(x - downX, y - downY) > dp(8)) dragging = true;
                if (dragging && !scaleDetector.isInProgress() && content.contains(downX, downY)) {
                    if (page == 2 && mapBounds.contains(downX, downY)) map.pan(x - lastX, y - lastY);
                    else if (page == 1 && detailBounds.contains(downX, downY)) detailScroll = Math.max(0, Math.min(detailScrollMax, detailScroll - (y - lastY)));
                    else if (page != 2) scroll = Math.max(0, Math.min(scrollMax, scroll - (y - lastY)));
                    invalidate();
                }
                lastX = x; lastY = y; return true;
            case MotionEvent.ACTION_UP:
                if (!dragging && pressed >= 0) { activate(pressed); performClick(); }
                else if (page == 1 && Math.abs(x - downX) > dp(80) && Math.abs(x - downX) > Math.abs(y - downY) * 1.5f) {
                    category = (category + (x < downX ? 1 : 7)) % 8; selection = -1; scroll = 0; poller.requestFull(); invalidate();
                }
                return true;
            case MotionEvent.ACTION_CANCEL: pressed = -1; return true;
            default: return true;
        }
    }
    private void activate(int code) {
        if (code < 3) setPage(code);
        else if (code == 20 || code == 21) poller.action(1, code == 20 ? 11 : 10);
        else if (code >= CODE_JUMP) {
            // Adventure equipment card: show that item on the Inventory page.
            setPage(1);
            category = (code - CODE_JUMP) / 1000; selection = (code - CODE_JUMP) % 1000; detailScroll = 0; revealSelection = true;
        }
        else if (code >= 1000) { selection = code - 1000; detailScroll = 0; }
        else if (code >= CODE_ACTION && code < CODE_ACTION + buttonRequests.size()) poller.actions.tap(buttonRequests.get(code - CODE_ACTION));
        else if (code >= 200 && code <= 202) { map.layer = LAYERS[code - 200]; map.automaticLayer = false; }
        else if (code == 203) map.recenter();
        else if (code >= 100 && code < 108) { category = code - 100; scroll = 0; selection = -1; poller.requestFull(); }
        invalidate();
    }
    @Override public boolean performClick() { super.performClick(); return true; }
}
