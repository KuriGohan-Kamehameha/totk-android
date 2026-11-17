package com.aure.totk.setup;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.SystemClock;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

/** Canvas-drawn menu widgets (ports of the mockup components in theme.css / common.js). Interactive widgets are
 * focusable so a D-pad or gamepad alone drives every screen; touch works on the same views. */
final class Widgets {
    private Widgets() { }

    // ---------- layout ----------

    /** Places children at 1920 x 1080 design coordinates scaled by Theme.scale and centred in the window. */
    static final class Board extends ViewGroup {
        static final int START = 0, END = 1, CENTER = 2;
        static final float WRAP = -1;
        static final class Spot extends ViewGroup.LayoutParams {
            final float x, y, w, h; final int ax, ay;
            Spot(float x, float y, float w, float h, int ax, int ay) { super(-2, -2); this.x = x; this.y = y; this.w = w; this.h = h; this.ax = ax; this.ay = ay; }
        }
        Board(Context context) { super(context); setClipChildren(false); setClipToPadding(false); }
        /** x/y name the child's start/end/centre edge per anchor; w/h in design pixels or WRAP. */
        <T extends View> T put(T view, float x, float y, float w, float h, int ax, int ay) { addView(view, new Spot(x, y, w, h, ax, ay)); return view; }
        <T extends View> T put(T view, float x, float y, float w, float h) { return put(view, x, y, w, h, START, START); }
        float originX() { return (getWidth() - Theme.DESIGN_W * Theme.scale) / 2; }
        float originY() { return (getHeight() - Theme.DESIGN_H * Theme.scale) / 2; }
        @Override protected void onMeasure(int ws, int hs) {
            int width = MeasureSpec.getSize(ws), height = MeasureSpec.getSize(hs);
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                Spot spot = (Spot) child.getLayoutParams();
                child.measure(spot.w < 0 ? MeasureSpec.makeMeasureSpec(width, MeasureSpec.AT_MOST) : MeasureSpec.makeMeasureSpec(Theme.u(spot.w), MeasureSpec.EXACTLY),
                        spot.h < 0 ? MeasureSpec.makeMeasureSpec(height, MeasureSpec.AT_MOST) : MeasureSpec.makeMeasureSpec(Theme.u(spot.h), MeasureSpec.EXACTLY));
            }
            setMeasuredDimension(width, height);
        }
        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
            float ox = originX(), oy = originY();
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                Spot spot = (Spot) child.getLayoutParams();
                int w = child.getMeasuredWidth(), h = child.getMeasuredHeight();
                float x = ox + Theme.f(spot.x), y = oy + Theme.f(spot.y);
                if (spot.ax == END) x -= w; else if (spot.ax == CENTER) x -= w / 2f;
                if (spot.ay == END) y -= h; else if (spot.ay == CENTER) y -= h / 2f;
                child.layout(Math.round(x), Math.round(y), Math.round(x) + w, Math.round(y) + h);
            }
        }
    }

    static LinearLayout column(Context context) {
        LinearLayout l = new LinearLayout(context); l.setOrientation(LinearLayout.VERTICAL); l.setClipChildren(false); l.setClipToPadding(false); return l;
    }
    /** Rows keep CAP design pixels on each side for the selection's diamond end-caps, so the caps stay inside the row's
     * own bounds; a row is therefore laid out CAP wider than its visible box on both sides. */
    static final float CAP = 7;
    static LinearLayout.LayoutParams rowParams(float height, float gap) { return margins(size(-1, height), -CAP, 0, -CAP, gap); }
    static LinearLayout row(Context context) { LinearLayout l = new LinearLayout(context); l.setOrientation(LinearLayout.HORIZONTAL); l.setGravity(Gravity.CENTER_VERTICAL); return l; }
    static LinearLayout.LayoutParams size(float w, float h) { return new LinearLayout.LayoutParams(w < 0 ? (int) w : Theme.u(w), h < 0 ? (int) h : Theme.u(h)); }
    static LinearLayout.LayoutParams margins(LinearLayout.LayoutParams p, float left, float top, float right, float bottom) {
        p.setMargins(Theme.u(left), Theme.u(top), Theme.u(right), Theme.u(bottom)); return p;
    }
    static View space(Context context) { return new View(context); }

    // ---------- backdrop, frame, emblem ----------

    /** Vignette plus procedural topographic contour lines (common.js contours(): same generator and seeds) and the
     * tablet frame. Deterministic: a screen's seed always draws the same lines. */
    static final class Backdrop extends View {
        private static final float[][] CENTERS = {{1430, 470, 26, 30, 24, 1}, {260, 930, 18, 40, 26, .8f}, {880, 60, 12, 60, 28, .6f}};
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final List<Path> paths = new ArrayList<>();
        private final List<int[]> styles = new ArrayList<>();   // {color, width*10}
        private final RectF box = new RectF();
        Backdrop(Context context, int seed) {
            super(context);
            long[] state = {seed & 0xffffffffL};
            for (float[] center : CENTERS) {
                float cx = center[0], cy = center[1], base = center[3], step = center[4], op = center[5];
                double[][] h = new double[4][];
                int[] ks = {1, 2, 3, 5};
                for (int j = 0; j < 4; j++) { double amp = (0.05 + next(state) * 0.08) / ks[j] * 2; h[j] = new double[]{ks[j], amp, next(state) * 6.28}; }
                for (int i = 0; i < (int) center[2]; i++) {
                    double radius = base + i * step;
                    Path path = new Path();
                    for (int t = 0; t <= 96; t++) {
                        double a = t / 96.0 * Math.PI * 2, m = 1;
                        for (double[] k : h) m += k[1] * Math.sin(k[0] * a + k[2] + i * 0.07 * k[0]);
                        float x = (float) (cx + Math.cos(a) * radius * m * 1.25), y = (float) (cy + Math.sin(a) * radius * m * 0.82);
                        if (t == 0) path.moveTo(x, y); else path.lineTo(x, y);
                    }
                    path.close();
                    paths.add(path);
                    boolean major = i % 5 == 4;
                    int color = major ? Theme.alpha(Theme.tint(0xffaabe96), .12f * op) : Theme.alpha(Theme.tint(0xff96af8c), .055f * op);
                    styles.add(new int[]{color, major ? 14 : 10});
                }
            }
        }
        private static double next(long[] state) {
            state[0] = (state[0] * 1664525L + 1013904223L) & 0xffffffffL;
            return state[0] / 4294967296.0;
        }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), s = Theme.scale;
            float ox = (w - Theme.DESIGN_W * s) / 2, oy = (h - Theme.DESIGN_H * s) / 2;
            // radial-gradient(ellipse 70% 80% at 68% 42%, #1c2c26 0, #101915 48%, #070b0a 100%)
            RadialGradient vignette = new RadialGradient(0, 0, 1, new int[]{Theme.tint(0xff1c2c26), Theme.tint(0xff101915), Theme.tint(0xff070b0a)}, new float[]{0, .48f, 1}, Shader.TileMode.CLAMP);
            Matrix m = new Matrix();
            m.setScale(Theme.DESIGN_W * s * .7f, Theme.DESIGN_H * s * .8f);
            m.postTranslate(ox + Theme.DESIGN_W * s * .68f, oy + Theme.DESIGN_H * s * .42f);
            vignette.setLocalMatrix(m);
            paint.setStyle(Paint.Style.FILL); paint.setShader(vignette);
            c.drawRect(0, 0, w, h, paint);
            paint.setShader(null);
            c.save(); c.translate(ox, oy); c.scale(s, s);
            paint.setStyle(Paint.Style.STROKE);
            for (int i = 0; i < paths.size(); i++) {
                paint.setColor(styles.get(i)[0]); paint.setStrokeWidth(styles.get(i)[1] / 10f);
                c.drawPath(paths.get(i), paint);
            }
            // .frame: the tablet bezel, inset 26 with 34 px corners
            box.set(26, 26, Theme.DESIGN_W - 26, Theme.DESIGN_H - 26);
            for (int i = 0; i < 10; i++) {   // inset 0 0 80px rgba(0,0,0,.55), approximated by fading inner strokes
                paint.setColor(Theme.alpha(0xff000000, .1f * (1 - i / 10f))); paint.setStrokeWidth(8);
                RectF inner = new RectF(box); inner.inset(8 + i * 8, 8 + i * 8);
                c.drawRoundRect(inner, Math.max(4, 34 - i * 8), Math.max(4, 34 - i * 8), paint);
            }
            paint.setStrokeWidth(5); paint.setColor(Theme.tint(0xe6121b17));
            RectF band = new RectF(box); band.inset(4, 4); c.drawRoundRect(band, 30, 30, paint);
            paint.setStrokeWidth(1); paint.setColor(Theme.tint(0xff26332c));
            band.inset(3, 3); c.drawRoundRect(band, 27, 27, paint);
            paint.setColor(0x99000000); band.set(box); band.inset(1.5f, 1.5f); c.drawRoundRect(band, 32, 32, paint);
            paint.setStrokeWidth(2); paint.setColor(Theme.tint(0xff2f3c35)); c.drawRoundRect(box, 34, 34, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new LinearGradient(900, 0, 1020, 0, new int[]{0, Theme.tint(0xff4b5c52), 0}, null, Shader.TileMode.CLAMP));
            c.drawRoundRect(900, 23, 1020, 27, 2, 2, paint);
            c.drawRoundRect(900, Theme.DESIGN_H - 27, 1020, Theme.DESIGN_H - 23, 2, 2, paint);
            paint.setShader(null);
            c.restore();
        }
    }

    /** The app emblem (common.js emblem(): an original ring device with ticks, spiral and a floating island). */
    static final class Emblem extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        Emblem(Context context) { super(context); setLayerType(LAYER_TYPE_SOFTWARE, null); }
        @Override protected void onDraw(Canvas c) {
            float size = Math.min(getWidth(), getHeight());
            if (size <= 0) return;
            c.save();
            c.translate((getWidth() - size) / 2, (getHeight() - size) / 2);
            float k = size / 360f;
            c.scale(k, k);
            draw(c, paint, 360);
            c.restore();
        }
        /** Draws the emblem into a size x size box at the origin (also used for the launcher shortcut glyph). */
        static void draw(Canvas c, Paint paint, float size) {
            float cc = size / 2, r = cc - 8, g = 5 * size / 360f;
            paint.reset(); paint.setAntiAlias(true);
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new RadialGradient(cc, size * .45f, size * .6f, Theme.tint(0xff1f3a31), Theme.tint(0xff0c1411), Shader.TileMode.CLAMP));
            c.drawCircle(cc, cc, r, paint);
            paint.setShader(null);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
            paint.setColor(Theme.ZONAI); paint.setStrokeWidth(4); paint.setShadowLayer(2 * g, 0, 0, Theme.ZONAI);
            c.drawCircle(cc, cc, r, paint);
            paint.clearShadowLayer();
            paint.setColor(Theme.alpha(Theme.GOLD_DIM, .85f)); paint.setStrokeWidth(2); c.drawCircle(cc, cc, r - 28, paint);
            paint.setColor(Theme.alpha(Theme.ZONAI, .8f)); paint.setStrokeCap(Paint.Cap.BUTT);
            for (int i = 0; i < 48; i++) {
                double a = i / 48.0 * Math.PI * 2;
                float l = i % 4 == 0 ? 16 : 7, cos = (float) Math.cos(a), sin = (float) Math.sin(a);
                c.drawLine(cc + cos * (r - 10), cc + sin * (r - 10), cc + cos * (r - 10 - l), cc + sin * (r - 10 - l), paint);
            }
            if ("sheikah_eye".equals(GameBuild.EMBLEM)) { sheikahEye(c, paint, cc, r / 470f, g); return; }
            Path spiral = new Path();
            for (int t = 0; t <= 220; t++) {
                double a = t / 220.0 * Math.PI * 5.2, rr = 8 + t * 0.255;
                float x = (float) (cc + Math.cos(a) * rr), y = (float) (cc + 34 + Math.sin(a) * rr * 0.55);
                if (t == 0) spiral.moveTo(x, y); else spiral.lineTo(x, y);
            }
            paint.setColor(Theme.alpha(Theme.ZONAI, .18f)); paint.setStrokeWidth(1.5f); c.drawPath(spiral, paint);
            paint.setStrokeCap(Paint.Cap.ROUND);
            Path island = new Path();
            island.moveTo(cc - 70, cc - 52); island.lineTo(cc + 70, cc - 52); island.lineTo(cc + 34, cc - 18); island.lineTo(cc, cc + 2); island.lineTo(cc - 34, cc - 18); island.close();
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.tint(0xff14231e)); c.drawPath(island, paint);
            paint.setStyle(Paint.Style.STROKE); paint.setColor(Theme.GOLD); paint.setStrokeWidth(4); paint.setShadowLayer(g, 0, 0, Theme.GOLD);
            c.drawPath(island, paint);
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.ZONAI); paint.setShadowLayer(g, 0, 0, Theme.ZONAI);
            c.drawCircle(cc, cc - 86, 11, paint);
            Path peaks = new Path();
            peaks.moveTo(cc - 100, cc + 92); peaks.lineTo(cc - 42, cc + 24); peaks.lineTo(cc - 12, cc + 56); peaks.lineTo(cc + 30, cc + 2); peaks.lineTo(cc + 104, cc + 92);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(7); c.drawPath(peaks, paint);
            paint.setColor(Theme.GOLD); paint.setStrokeWidth(3); paint.setShadowLayer(g, 0, 0, Theme.GOLD);
            c.drawLine(cc - 120, cc + 100, cc + 120, cc + 100, paint);
            paint.setStyle(Paint.Style.FILL);
            Theme.diamond(c, cc, cc + 119, 9.9f, paint);
            paint.clearShadowLayer();
        }
        /** BotW Native's emblem inside the ring: the Sheikah eye of app-assets/icon/make_icon_botw.py (its 1024 grid, where
         *  the outer ring has radius 470; k converts to this canvas). Triangular lashes standing on the upper lid's own
         *  circle, lids whose ends curl, a ringed iris with a gold pupil and a long teardrop. */
        private static void sheikahEye(Canvas c, Paint paint, float cc, float k, float g) {
            final float ey = -21;
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
            paint.setColor(Theme.ZONAI); paint.setShadowLayer(g, 0, 0, Theme.ZONAI);
            Path p = new Path();
            bow(p, cc, k, -215, 215, ey, ey - 125);                       // upper lid
            paint.setStrokeWidth(26 * k); c.drawPath(p, paint);
            p.reset(); arc(p, cc, k, -249, ey, 34, 0, -150); arc(p, cc, k, 249, ey, 34, 180, 150);   // the curls
            paint.setStrokeWidth(22 * k); c.drawPath(p, paint);
            p.reset(); bow(p, cc, k, -178, 178, ey + 4, ey + 112);        // lower lid
            paint.setStrokeWidth(20 * k); c.drawPath(p, paint);
            paint.setStrokeWidth(18 * k); c.drawCircle(cc, cc + ey * k, 84 * k, paint);    // iris
            paint.setStrokeWidth(13 * k); c.drawCircle(cc, cc + ey * k, 48 * k, paint);
            paint.setStyle(Paint.Style.FILL);
            float lidR = (215f * 215f + 125f * 125f) / (2 * 125f), lidCy = ey - 125 + lidR;
            float[][] lashes = {{-30, 84}, {0, 100}, {30, 84}};
            for (float[] l : lashes) {                                    // lashes, pointing out from the lid's circle
                float base = lidR + 47;
                Path t = new Path();
                t.moveTo(cc + px(base + l[1], l[0]) * k, cc + (lidCy + py(base + l[1], l[0])) * k);
                t.lineTo(cc + px(base, l[0] - 8.5f) * k, cc + (lidCy + py(base, l[0] - 8.5f)) * k);
                t.lineTo(cc + px(base, l[0] + 8.5f) * k, cc + (lidCy + py(base, l[0] + 8.5f)) * k);
                t.close();
                c.drawPath(t, paint);
            }
            Path drop = new Path();                                       // the teardrop: a stem widening to a round end
            drop.moveTo(cc - 11 * k, cc + (ey + 112) * k); drop.lineTo(cc + 11 * k, cc + (ey + 112) * k);
            drop.lineTo(cc + 26 * k, cc + (ey + 300) * k); drop.lineTo(cc - 26 * k, cc + (ey + 300) * k); drop.close();
            c.drawPath(drop, paint);
            c.drawCircle(cc, cc + (ey + 300) * k, 27 * k, paint);
            paint.setColor(Theme.GOLD); paint.setShadowLayer(g, 0, 0, Theme.GOLD);
            c.drawCircle(cc, cc + ey * k, 20 * k, paint);                 // pupil
            paint.clearShadowLayer();
        }
        /** x and y of a point at distance d, degrees clockwise from straight up (the icon script's polar()). */
        private static float px(float d, float deg) { return (float) (d * Math.sin(Math.toRadians(deg))); }
        private static float py(float d, float deg) { return (float) (-d * Math.cos(Math.toRadians(deg))); }
        /** A circular arc in grid units: centre (x, y), radius rr, from a0 sweeping by sweep degrees (clockwise from +x). */
        private static void arc(Path p, float cc, float k, float x, float y, float rr, float a0, float sweep) {
            p.arcTo(new RectF(cc + (x - rr) * k, cc + (y - rr) * k, cc + (x + rr) * k, cc + (y + rr) * k), a0, sweep, true);
        }
        /** The circular arc from (x0, yEnd) to (x1, yEnd) through (0, yMid). */
        private static void bow(Path p, float cc, float k, float x0, float x1, float yEnd, float yMid) {
            float h = Math.abs(x1 - x0) / 2, s = yEnd - yMid, rr = (h * h + s * s) / (2 * Math.abs(s));
            float cy = s > 0 ? yMid + rr : yMid - rr, span = (float) Math.toDegrees(Math.asin(h / rr));
            if (s > 0) arc(p, cc, k, 0, cy, rr, 270 - span, 2 * span);
            else arc(p, cc, k, 0, cy, rr, 90 + span, -2 * span);
        }
    }

    /** A line icon (Icons) in the current colour. */
    static final class Icon extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private String name;
        private int color;
        Icon(Context context, String name, int color) {
            super(context); this.name = name; this.color = color;
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
        }
        Icon color(int color) { if (this.color != color) { this.color = color; invalidate(); } return this; }
        Icon icon(String name) { if (!name.equals(this.name)) { this.name = name; invalidate(); } return this; }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth() - getPaddingLeft() - getPaddingRight(), h = getHeight() - getPaddingTop() - getPaddingBottom(), size = Math.min(w, h);
            draw(c, name, getPaddingLeft() + (w - size) / 2, getPaddingTop() + (h - size) / 2, size, color, paint);
        }
        static void draw(Canvas c, String name, float x, float y, float size, int color, Paint paint) {
            c.save(); c.translate(x, y); c.scale(size / Icons.BOX, size / Icons.BOX);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
            paint.setColor(color); paint.setStrokeWidth(Icons.STROKE);
            c.drawPath(Icons.get(name), paint);
            c.restore();
        }
    }

    // ---------- surfaces ----------

    /** .panel: dark glass with a 1 px rule border, an inner shade and (notch) the two corner brackets. */
    static final class Panel extends LinearLayout {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();
        private final boolean notch;
        Panel(Context context, boolean notch, float padH, float padV) {
            super(context);
            this.notch = notch;
            setOrientation(VERTICAL); setWillNotDraw(false);
            setClipChildren(false); setClipToPadding(false);   // rows' end-caps and glow reach into the padding
            setPadding(Theme.u(padH), Theme.u(padV), Theme.u(padH), Theme.u(padV));
        }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), r = Theme.f(18);
            box.set(0, 0, w, h);
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.GLASS);
            paint.setShadowLayer(Theme.f(20), 0, Theme.f(18), 0x59000000);
            c.drawRoundRect(box, r, r, paint);
            paint.clearShadowLayer();
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Theme.f(4)); paint.setColor(Theme.tint(0x590a100e));
            box.inset(Theme.f(2.5f), Theme.f(2.5f)); c.drawRoundRect(box, r - Theme.f(2), r - Theme.f(2), paint);
            paint.setStrokeWidth(Theme.f(1)); paint.setColor(0x0affffff);
            c.drawLine(r, Theme.f(1.5f), w - r, Theme.f(1.5f), paint);
            box.set(Theme.f(.5f), Theme.f(.5f), w - Theme.f(.5f), h - Theme.f(.5f));
            paint.setColor(Theme.RULE); c.drawRoundRect(box, r, r, paint);
            if (notch) {
                paint.setStrokeWidth(Theme.f(2)); paint.setColor(Theme.NOTCH);
                float m = Theme.f(11), l = Theme.f(22), k = Theme.f(8);
                Path p = new Path();
                p.moveTo(m, m + l); p.lineTo(m, m + k); p.quadTo(m, m, m + k, m); p.lineTo(m + l, m);
                c.drawPath(p, paint);
                p.reset();
                p.moveTo(w - m, h - m - l); p.lineTo(w - m, h - m - k); p.quadTo(w - m, h - m, w - m - k, h - m); p.lineTo(w - m - l, h - m);
                c.drawPath(p, paint);
            }
        }
    }

    /** .rule: a 1 px line, with (joint) a small diamond at its centre. */
    static final class Rule extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final boolean joint;
        private final int color;
        Rule(Context context, boolean joint, int color) { super(context); this.joint = joint; this.color = color; }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), y = getHeight() / 2f;
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Math.max(1, Theme.f(1))); paint.setColor(color);
            c.drawLine(0, y, w, y, paint);
            if (!joint) return;
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.DEEP); Theme.diamond(c, w / 2, y, Theme.f(6), paint);
            paint.setStyle(Paint.Style.STROKE); paint.setColor(Theme.tint(0xff6f7d62)); Theme.diamond(c, w / 2, y, Theme.f(6), paint);
        }
    }

    /** .chip: a pill with a glowing status dot. */
    static final class Chip extends TextView {
        static final int IDLE = 0, BUSY = 1, OK = 2, WARN = 3, ERROR = 4;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();
        private int kind;
        Chip(Context context) {
            super(context);
            setTypeface(Theme.regular); setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, Theme.f(17)); setIncludeFontPadding(false);
            setTextColor(Theme.TEXT2); setSingleLine(true); setEllipsize(TextUtils.TruncateAt.END);
            setPadding(Theme.u(35), Theme.u(9), Theme.u(16), Theme.u(9));
            setBackground(null);
        }
        Chip set(int kind, String text) { this.kind = kind; setText(text); invalidate(); return this; }
        Chip size(float text) {
            setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, Theme.f(text));
            setPadding(Theme.u(text * 1.9f), Theme.u(text * .45f), Theme.u(text * .9f), Theme.u(text * .45f));
            return this;
        }
        private int dot() {
            switch (kind) {
                case OK: return Theme.ZONAI;
                case WARN: case BUSY: return Theme.AMBER;
                case ERROR: return Theme.WARN;
                default: return Theme.TEXT3;
            }
        }
        @Override protected void onDraw(Canvas c) {
            c.save(); c.translate(getScrollX(), getScrollY());
            box.set(Theme.f(.5f), Theme.f(.5f), getWidth() - Theme.f(.5f), getHeight() - Theme.f(.5f));
            float r = box.height() / 2;
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.tint(0xcc101815)); c.drawRoundRect(box, r, r, paint);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(1)); paint.setColor(Theme.RULE); c.drawRoundRect(box, r, r, paint);
            paint.setStyle(Paint.Style.FILL); paint.setColor(dot());
            if (kind != IDLE) paint.setShadowLayer(Theme.f(6), 0, 0, Theme.alpha(dot(), .5f));
            c.drawCircle(getPaddingLeft() - getTextSize() * .85f, box.centerY(), Theme.f(4.5f), paint);
            paint.clearShadowLayer();
            c.restore();
            super.onDraw(c);
        }
    }

    // ---------- button prompts ----------

    /** .btn: a parchment key cap (A/B/X/Y circles, L/R/− pills, +, and the D-pad left/right glyph). */
    static final class Key extends View {
        static final String DPAD = "dpad";
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String label;
        private final float size;
        Key(Context context, String label, float size) { super(context); this.label = label; this.size = size; }
        boolean pill() { return label.equals("L") || label.equals("R") || label.equals("−"); }
        @Override protected void onMeasure(int ws, int hs) {
            float h = pill() ? size * .84f : size;
            float w = pill() ? Math.max(h, size * .63f + 2 * size * .32f) : size;
            setMeasuredDimension(Math.round(Theme.f(w) + Theme.f(4)), Math.round(Theme.f(h) + Theme.f(4)));
        }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), ring = Theme.f(2);
            RectF box = new RectF(ring, ring, w - ring, h - ring);
            float radius = pill() ? Theme.f(size * .26f) : box.height() / 2;
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(ring * 2); paint.setColor(0x80000000);
            c.drawRoundRect(box, radius, radius, paint);
            paint.setStyle(Paint.Style.FILL); paint.setColor(isEnabled() ? Theme.KEY : 0xff6b6a5f);
            c.drawRoundRect(box, radius, radius, paint);
            paint.setColor(Theme.KEY_INK);
            if (label.equals(DPAD)) {
                float cx = w / 2, cy = h / 2, k = box.height() / 24;
                Path p = new Path();
                p.moveTo(cx - 9 * k, cy); p.lineTo(cx - 3 * k, cy - 5 * k); p.lineTo(cx - 3 * k, cy + 5 * k); p.close();
                p.moveTo(cx + 9 * k, cy); p.lineTo(cx + 3 * k, cy - 5 * k); p.lineTo(cx + 3 * k, cy + 5 * k); p.close();
                c.drawPath(p, paint);
                return;
            }
            boolean plus = label.equals("+") || label.equals("−");
            if (plus) {   // drawn, not typed: the font's + and − sit off-centre in a small circle
                float cx = w / 2, cy = h / 2, arm = box.height() * (label.equals("+") ? .27f : .24f);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(size * .06f)); paint.setStrokeCap(Paint.Cap.ROUND);
                c.drawLine(cx - arm, cy, cx + arm, cy, paint);
                if (label.equals("+")) c.drawLine(cx, cy - arm, cx, cy + arm, paint);
                return;
            }
            paint.setTypeface(Theme.semibold); paint.setTextSize(Theme.f(size * (pill() ? .42f : .5f))); paint.setTextAlign(Paint.Align.CENTER);
            Paint.FontMetrics fm = paint.getFontMetrics();
            c.drawText(label, w / 2, h / 2 - (fm.ascent + fm.descent) / 2, paint);
        }
    }

    /** One bottom prompt: key cap + label; tappable, so touch reaches the same action. */
    static final class Prompt extends LinearLayout {
        final Key key;
        final TextView text;
        Prompt(Context context, String key, String label, View.OnClickListener click) {
            super(context);
            setOrientation(HORIZONTAL); setGravity(Gravity.CENTER_VERTICAL);
            this.key = new Key(context, key, 38);
            addView(this.key);
            text = Theme.text(context, label, false, 22, Theme.TEXT2);
            addView(text, margins(size(-2, -2), 12, 0, 0, 0));
            if (click != null) { setClickable(true); setOnClickListener(click); }
            setFocusable(false);
        }
        @Override public void setEnabled(boolean enabled) { super.setEnabled(enabled); key.setEnabled(enabled); setAlpha(enabled ? 1 : .45f); }
        @Override protected void dispatchSetPressed(boolean pressed) { super.dispatchSetPressed(pressed); setAlpha(pressed ? .6f : isEnabled() ? 1 : .45f); }
    }

    /** The home screen's on/off switch (.toggle). */
    static final class Toggle extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private boolean on;
        Toggle(Context context) { super(context); }
        void set(boolean on) { if (this.on != on) { this.on = on; invalidate(); } }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), r = h / 2;
            paint.setStyle(Paint.Style.FILL); paint.setColor(on ? Theme.tint(0xff4d5a2c) : Theme.tint(0xff26312c));
            c.drawRoundRect(0, 0, w, h, r, r, paint);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(1)); paint.setColor(on ? Theme.SEL_DIM : Theme.RULE);
            c.drawRoundRect(Theme.f(.5f), Theme.f(.5f), w - Theme.f(.5f), h - Theme.f(.5f), r, r, paint);
            paint.setStyle(Paint.Style.FILL); paint.setColor(on ? Theme.SEL : Theme.tint(0xff79806f));
            if (on) paint.setShadowLayer(Theme.f(8), 0, 0, Theme.SEL_GLOW);
            float knob = h - Theme.f(10);
            c.drawCircle(on ? w - Theme.f(5) - knob / 2 : Theme.f(5) + knob / 2, h / 2, knob / 2, paint);
            paint.clearShadowLayer();
        }
    }

    /** A text action for touch and D-pad alike (Install, Start game): a rounded key, lime when primary; focus glows
     * with the diamond end-caps of a selected row. */
    static final class Action extends TextView {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();
        private final boolean primary;
        Action(Context context, String label, boolean primary, View.OnClickListener click) {
            super(context);
            this.primary = primary;
            setText(label); setTypeface(Theme.semibold); setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, Theme.f(22));
            setIncludeFontPadding(false); setGravity(Gravity.CENTER); setSingleLine(true);
            setTextColor(primary ? Theme.KEY_INK : Theme.TEXT);
            setPadding(Theme.u(36), Theme.u(16), Theme.u(36), Theme.u(16));
            setMinWidth(Theme.u(200));
            setFocusable(true); setClickable(true); setOnClickListener(click); setDefaultFocusHighlightEnabled(false);
            setBackground(null);
        }
        @Override public void setEnabled(boolean enabled) { super.setEnabled(enabled); setAlpha(enabled ? 1 : .4f); setFocusable(enabled); }
        @Override protected void drawableStateChanged() { super.drawableStateChanged(); invalidate(); }
        @Override protected void onDraw(Canvas c) {
            c.save(); c.translate(getScrollX(), getScrollY());
            float r = Theme.f(14);
            box.set(Theme.f(8), Theme.f(2), getWidth() - Theme.f(8), getHeight() - Theme.f(2));
            boolean lit = isFocused() || isPressed();
            paint.setStyle(Paint.Style.FILL);
            if (primary) paint.setShader(new LinearGradient(0, box.top, 0, box.bottom, Theme.SEL, Theme.SEL_DIM, Shader.TileMode.CLAMP));
            else paint.setColor(Theme.GLASS2);
            if (lit) paint.setShadowLayer(Theme.f(14), 0, 0, Theme.SEL_GLOW);
            c.drawRoundRect(box, r, r, paint);
            paint.setShader(null); paint.clearShadowLayer();
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(lit ? 2 : 1));
            paint.setColor(lit ? (primary ? Theme.TEXT_LIT : Theme.SEL) : primary ? Theme.SEL : Theme.RULE);
            c.drawRoundRect(box, r, r, paint);
            if (lit) {
                paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SEL); paint.setShadowLayer(Theme.f(6), 0, 0, Theme.SEL_GLOW);
                Theme.diamond(c, box.left, box.centerY(), Theme.f(7), paint);
                Theme.diamond(c, box.right, box.centerY(), Theme.f(7), paint);
                paint.clearShadowLayer();
            }
            c.restore();
            super.onDraw(c);
        }
    }

    // ---------- selectable rows ----------

    /** What a row does with left/right and A. */
    interface Adjust { void adjust(int direction); }

    /** .row: a selectable list row (label + hint, optional leading icon circle, a value on the right). Focus draws the
     * lime selection; D-pad left/right adjusts the value; A clicks (a setting row steps forward). */
    static final class Row extends LinearLayout {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();
        final TextView label, hint;
        private Icon icon;
        private final float radius;
        private View value;
        private Adjust adjust;
        private boolean dimmed, iconCircle;
        private float circle;
        private int labelColor = Theme.TEXT;
        /** A row whose label keeps its own colour (Delete in red) when not selected. */
        Row labelColor(int color) { labelColor = color; label.setTextColor(color); return this; }
        Row(Context context, String labelText, String hintText, float labelSize, float hintSize) {
            super(context);
            setOrientation(HORIZONTAL); setGravity(Gravity.CENTER_VERTICAL); setWillNotDraw(false);
            setFocusable(true); setClickable(true); setDefaultFocusHighlightEnabled(false);
            setPadding(Theme.u(30 + CAP), 0, Theme.u(30 + CAP), 0);
            radius = Theme.f(14);
            LinearLayout text = column(context);
            label = Theme.text(context, labelText, false, labelSize, Theme.TEXT);
            label.setSingleLine(true); label.setEllipsize(TextUtils.TruncateAt.END);
            text.addView(label);
            hint = Theme.text(context, hintText == null ? "" : hintText, false, hintSize, Theme.TEXT3);
            hint.setSingleLine(true); hint.setEllipsize(TextUtils.TruncateAt.END);
            if (hintText == null) hint.setVisibility(GONE);
            text.addView(hint, margins(size(-1, -2), 0, 4, 0, 0));
            addView(text, new LayoutParams(0, -2, 1));
        }
        /** Leading icon in a ring of the given diameter (home menu: 62 px ring, 30 px glyph). */
        Row icon(String name, float circle, float glyph) {
            iconCircle = true; this.circle = circle;
            icon = new Icon(getContext(), name, Theme.ZONAI);
            LinearLayout.LayoutParams p = margins(size(glyph, glyph), (circle - glyph) / 2, 0, 26 + (circle - glyph) / 2, 0);
            addView(icon, 0, p);
            return this;
        }
        /** Right-hand content (a Selector, Slider, or a status text). */
        Row value(View view) {
            if (value != null) removeView(value);
            value = view;
            if (view != null) addView(view, new LayoutParams(-2, -2));
            return this;
        }
        Row adjust(Adjust adjust) { this.adjust = adjust; return this; }
        /** Shown at reduced strength (.row.off) but still focusable, so its detail panel can say why. */
        Row dimmed(boolean dimmed) { this.dimmed = dimmed; setAlpha(dimmed ? .42f : 1); return this; }
        boolean dimmed() { return dimmed; }
        void step(int direction) { if (adjust != null && !dimmed) adjust.adjust(direction); }
        boolean lit() { return isFocused() || isPressed(); }
        @Override protected void drawableStateChanged() {
            super.drawableStateChanged();
            boolean lit = lit();
            label.setTextColor(lit ? Theme.TEXT_LIT : labelColor);
            label.setShadowLayer(lit ? Theme.f(12) : 0, 0, 0, lit ? 0x40fffad2 : 0);
            if (icon != null) icon.color(lit ? Theme.SEL : Theme.ZONAI);
            if (value instanceof Selector) ((Selector) value).lit(lit);
            if (value instanceof Slider) ((Slider) value).lit(lit);
            invalidate();
        }
        @Override public boolean onKeyDown(int code, KeyEvent event) {
            if (adjust != null && (code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT)) {
                step(code == KeyEvent.KEYCODE_DPAD_LEFT ? -1 : 1);
                return true;
            }
            return super.onKeyDown(code, event);
        }
        @Override protected void onDraw(Canvas c) {
            box.set(Theme.f(CAP), 0, getWidth() - Theme.f(CAP), getHeight());
            if (lit()) Theme.selection(c, box, radius, paint);
            if (iconCircle && icon != null) {
                float cx = icon.getLeft() + icon.getWidth() / 2f, cy = icon.getTop() + icon.getHeight() / 2f, r = Theme.f(circle / 2);
                boolean lit = lit();
                paint.setStyle(Paint.Style.FILL);
                paint.setShader(new RadialGradient(cx, cy, r, new int[]{Theme.withAlpha(Theme.ZONAI, 0x1a), 0}, new float[]{0, .7f}, Shader.TileMode.CLAMP));
                c.drawCircle(cx, cy, r, paint);
                paint.setShader(null);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(1.5f));
                paint.setColor(lit ? Theme.withAlpha(Theme.SEL, 0xb3) : Theme.tint(0xff3f5149));
                if (lit) paint.setShadowLayer(Theme.f(8), 0, 0, Theme.SEL_GLOW);
                c.drawCircle(cx, cy, r, paint);
                paint.clearShadowLayer();
            }
        }
    }

    /** ‹ value › with pips (.value): tapping the left or right half steps the owning row. */
    static final class Selector extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private String text = "";
        private int count, index;
        private boolean lit;
        private final Row row;
        Selector(Context context, Row row) { super(context); this.row = row; setClickable(true); }
        Selector set(String text, int count, int index) { this.text = text; this.count = count; this.index = index; requestLayout(); invalidate(); return this; }
        void lit(boolean lit) { if (this.lit != lit) { this.lit = lit; invalidate(); } }
        @Override protected void onMeasure(int ws, int hs) {
            paint.setTypeface(Theme.regular); paint.setTextSize(Theme.f(25));
            float w = Math.max(Theme.f(300), paint.measureText(text) + Theme.f(2 * 13 + 2 * 20 + 20));
            setMeasuredDimension(Math.round(w), Theme.u(25 * 1.36f + (count > 0 ? 12 : 0)));
        }
        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event.getAction() == MotionEvent.ACTION_UP && isEnabled()) {
                if (!row.isFocused()) row.requestFocus();
                row.step(event.getX() < getWidth() / 2f ? -1 : 1);
            }
            return true;
        }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), cy = Theme.f(25 * .68f);
            paint.setTypeface(Theme.regular); paint.setTextSize(Theme.f(25)); paint.setTextAlign(Paint.Align.CENTER);
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.TEXT);
            Paint.FontMetrics fm = paint.getFontMetrics();
            c.drawText(text, w / 2, cy - (fm.ascent + fm.descent) / 2, paint);
            float half = paint.measureText(text) / 2, gap = Theme.f(20), aw = Theme.f(13), ah = Theme.f(9);
            paint.setColor(lit ? Theme.SEL : Theme.alpha(Theme.TEXT2, .8f));
            Path arrows = new Path();
            float lx = w / 2 - half - gap;
            arrows.moveTo(lx - aw, cy); arrows.lineTo(lx, cy - ah); arrows.lineTo(lx, cy + ah); arrows.close();
            float rx = w / 2 + half + gap;
            arrows.moveTo(rx + aw, cy); arrows.lineTo(rx, cy - ah); arrows.lineTo(rx, cy + ah); arrows.close();
            c.drawPath(arrows, paint);
            if (count <= 0) return;
            float pw = Theme.f(22), ph = Theme.f(4), pg = Theme.f(6), total = count * pw + (count - 1) * pg, x = w / 2 - total / 2, y = getHeight() - ph;
            for (int i = 0; i < count; i++) {
                boolean on = i == index;
                paint.setColor(on ? (lit ? Theme.SEL : Theme.TEXT2) : Theme.PIP);
                if (on && lit) paint.setShadowLayer(Theme.f(6), 0, 0, Theme.SEL_GLOW);
                c.drawRoundRect(x, y, x + pw, y + ph, ph / 2, ph / 2, paint);
                paint.clearShadowLayer();
                x += pw + pg;
            }
        }
    }

    /** .slider with its number: drag or tap sets the value; the owning row's left/right steps it. */
    static final class Slider extends View {
        interface Listener { void set(int value); }
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Row row;
        private final Listener listener;
        private int value;
        private boolean lit;
        Slider(Context context, Row row, Listener listener) { super(context); this.row = row; this.listener = listener; setClickable(true); }
        Slider set(int value) { this.value = value; invalidate(); return this; }
        void lit(boolean lit) { if (this.lit != lit) { this.lit = lit; invalidate(); } }
        @Override protected void onMeasure(int ws, int hs) { setMeasuredDimension(Theme.u(300 + 24 + 60), Theme.u(36)); }
        @Override public boolean onTouchEvent(MotionEvent event) {
            if (!isEnabled()) return true;
            if (event.getAction() == MotionEvent.ACTION_DOWN) { getParent().requestDisallowInterceptTouchEvent(true); if (!row.isFocused()) row.requestFocus(); }
            float x = (event.getX() - Theme.f(13)) / (Theme.f(300) - Theme.f(26));
            int next = Math.max(0, Math.min(100, Math.round(x * 100)));
            if (next != value) listener.set(next);
            return true;
        }
        @Override protected void onDraw(Canvas c) {
            float cy = getHeight() / 2f, left = 0, width = Theme.f(300), track = Theme.f(8);
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.TRACK);
            c.drawRoundRect(left, cy - track / 2, left + width, cy + track / 2, track / 2, track / 2, paint);
            float x = left + Theme.f(13) + (width - Theme.f(26)) * value / 100f;
            paint.setShader(new LinearGradient(left, 0, x, 0, Theme.SEL_DIM, Theme.SEL, Shader.TileMode.CLAMP));
            c.drawRoundRect(left, cy - track / 2, x, cy + track / 2, track / 2, track / 2, paint);
            paint.setShader(null);
            paint.setColor(Theme.SEL); paint.setShadowLayer(Theme.f(lit ? 12 : 4), 0, 0, Theme.SEL_GLOW);
            c.drawCircle(x, cy, Theme.f(18), paint);
            paint.clearShadowLayer();
            paint.setColor(Theme.INK); c.drawCircle(x, cy, Theme.f(16), paint);
            paint.setColor(0xfff4efd8); c.drawCircle(x, cy, Theme.f(13), paint);
            paint.setTypeface(Theme.regular); paint.setTextSize(Theme.f(25)); paint.setTextAlign(Paint.Align.RIGHT); paint.setColor(Theme.TEXT);
            Paint.FontMetrics fm = paint.getFontMetrics();
            c.drawText(String.valueOf(value), getWidth(), cy - (fm.ascent + fm.descent) / 2, paint);
        }
    }

    /** .meter: five slanted cost blocks; amber, or Zonai teal when the cost is low. */
    static final class Meter extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int on;
        Meter(Context context) { super(context); }
        Meter set(int on) { this.on = on; invalidate(); return this; }
        @Override protected void onMeasure(int ws, int hs) { setMeasuredDimension(Theme.u(5 * 26 + 4 * 6 + 6), Theme.u(12)); }
        @Override protected void onDraw(Canvas c) {
            float w = Theme.f(26), h = Theme.f(10), gap = Theme.f(6), skew = h * (float) Math.tan(Math.toRadians(18));
            boolean low = on <= 1;
            for (int i = 0; i < 5; i++) {
                float x = Theme.f(3) + i * (w + gap), y = Theme.f(1);
                Path p = new Path();
                p.moveTo(x + skew, y); p.lineTo(x + w, y); p.lineTo(x + w - skew, y + h); p.lineTo(x, y + h); p.close();
                boolean lit = i < on;
                paint.setColor(lit ? (low ? Theme.ZONAI : Theme.AMBER) : Theme.tint(0xff2f3b35));
                if (lit) paint.setShadowLayer(Theme.f(5), 0, 0, low ? Theme.ZONAI_GLOW : 0x59e3ad55);
                c.drawPath(p, paint);
                paint.clearShadowLayer();
            }
        }
    }

    /** A header tab (.tab): icon over label; the selected tab glows and has a diamond under it. Tap selects. */
    static final class Tab extends LinearLayout {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Icon icon;
        private final TextView label;
        private boolean on;
        Tab(Context context, String iconName, String text, View.OnClickListener click) {
            super(context);
            setOrientation(VERTICAL); setGravity(Gravity.CENTER); setWillNotDraw(false);
            setClickable(true); setOnClickListener(click); setFocusable(false);
            setPadding(0, 0, 0, Theme.u(16));   // the 70 px tab; the diamond is drawn below it
            icon = new Icon(context, iconName, Theme.TEXT3);
            addView(icon, size(30, 30));
            label = Theme.text(context, text, false, 15, Theme.TEXT3);
            addView(label, margins(size(-2, -2), 0, 5, 0, 0));
        }
        void on(boolean on) { this.on = on; icon.color(on ? Theme.TEXT : Theme.TEXT3); label.setTextColor(on ? Theme.TEXT : Theme.TEXT3); invalidate(); }
        @Override protected void onDraw(Canvas c) {
            if (!on) return;
            float w = getWidth(), h = getHeight() - Theme.f(16), r = Theme.f(14);
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new LinearGradient(0, 0, 0, h, Theme.withAlpha(Theme.SEL, 0x24), Theme.withAlpha(Theme.SEL, 0x05), Shader.TileMode.CLAMP));
            c.drawRoundRect(0, 0, w, h, r, r, paint);
            paint.setShader(null);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(1.5f)); paint.setColor(Theme.withAlpha(Theme.SEL, 0x8c));
            paint.setShadowLayer(Theme.f(11), 0, 0, Theme.withAlpha(Theme.SEL, 0x2e));
            c.drawRoundRect(Theme.f(.75f), Theme.f(.75f), w - Theme.f(.75f), h - Theme.f(.75f), r, r, paint);
            paint.clearShadowLayer();
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SEL); paint.setShadowLayer(Theme.f(5), 0, 0, Theme.SEL_GLOW);
            Theme.diamond(c, w / 2, h + Theme.f(7), Theme.f(7), paint);
            paint.clearShadowLayer();
        }
        @Override protected void onMeasure(int ws, int hs) {
            super.onMeasure(ws, MeasureSpec.makeMeasureSpec(Theme.u(86), MeasureSpec.EXACTLY));
        }
    }

    /** A preset card: "In use" badge, icon + name, caption, and "Recommended for this device" at the bottom. Laid out
     * CAP wider than its visible box on both sides, like a Row, for the selection's end-caps. */
    static final class PresetCard extends LinearLayout {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();
        private final TextView badge;
        private boolean active;
        PresetCard(Context context, String name, String caption, String recommendText, String inUse, View.OnClickListener click) {
            super(context);
            setOrientation(VERTICAL); setWillNotDraw(false);
            setFocusable(true); setClickable(true); setOnClickListener(click); setDefaultFocusHighlightEnabled(false);
            setPadding(Theme.u(18 + CAP), Theme.u(12), Theme.u(14 + CAP), Theme.u(12));
            badge = Theme.text(context, inUse, true, 13, Theme.ZONAI);
            badge.setLetterSpacing(.12f); badge.setGravity(Gravity.END);
            badge.setVisibility(INVISIBLE);
            addView(badge, size(-1, -2));
            LinearLayout head = row(context);
            head.addView(new Icon(context, "perf", Theme.ZONAI), size(26, 26));
            TextView title = Theme.text(context, name, false, 24, Theme.TEXT);
            title.setSingleLine(true);
            head.addView(title, margins(size(-2, -2), 12, 0, 0, 0));
            addView(head, margins(size(-1, -2), 0, 2, 0, 0));
            TextView text = Theme.text(context, caption, false, 17, Theme.TEXT3);
            text.setSingleLine(true); text.setEllipsize(TextUtils.TruncateAt.END);
            addView(text, margins(size(-1, -2), 0, 10, 0, 0));
            addView(space(context), new LayoutParams(-1, 0, 1));
            TextView recommend = Theme.text(context, recommendText == null ? "" : recommendText, false, 14, Theme.AMBER);
            recommend.setLineSpacing(0, 1.1f);
            recommend.setVisibility(recommendText == null ? GONE : VISIBLE);
            addView(recommend);
        }
        PresetCard active(boolean active) { this.active = active; badge.setVisibility(active ? VISIBLE : INVISIBLE); invalidate(); return this; }
        @Override protected void drawableStateChanged() { super.drawableStateChanged(); invalidate(); }
        @Override protected void onDraw(Canvas c) {
            float l = Theme.f(CAP), w = getWidth() - l, h = getHeight(), r = Theme.f(16), e = Theme.f(.75f);
            box.set(l, 0, w, h);
            boolean lit = isFocused() || isPressed();
            paint.setStyle(Paint.Style.FILL);
            if (active) paint.setShader(new LinearGradient(0, 0, 0, h, Theme.withAlpha(Theme.ZONAI, 0x1f), Theme.tint(0xcc141f1b), Shader.TileMode.CLAMP));
            else paint.setColor(Theme.GLASS2);
            c.drawRoundRect(box, r, r, paint);
            paint.setShader(null);
            if (lit) Theme.selection(c, box, r, paint);
            else {
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(active ? 1.5f : 1)); paint.setColor(active ? Theme.withAlpha(Theme.ZONAI, 0xb3) : Theme.RULE);
                if (active) paint.setShadowLayer(Theme.f(11), 0, 0, Theme.withAlpha(Theme.ZONAI, 0x26));
                c.drawRoundRect(l + e, e, w - e, h - e, r, r, paint);
                paint.clearShadowLayer();
            }
        }
    }

    // ---------- setup ----------

    /** One step of the setup rail (.step): numbered circle (check when done, lime when current), title and text, and
     * the connector down to the next step. */
    static final class Step extends LinearLayout {
        static final int TODO = 0, CURRENT = 1, DONE = 2, FAILED = 3;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final int state, number;
        private final boolean last;
        Step(Context context, int number, String title, String text, int state, boolean last) {
            super(context);
            this.state = state; this.number = number; this.last = last;
            setOrientation(HORIZONTAL); setWillNotDraw(false);
            LinearLayout body = column(context);
            TextView t = Theme.text(context, title, false, 26, state == CURRENT ? Theme.TEXT_LIT : Theme.TEXT);
            body.addView(t, margins(size(-1, -2), 0, 6, 0, 0));
            TextView d = Theme.text(context, text, false, 18, state == FAILED ? Theme.WARN : Theme.TEXT3);
            d.setLineSpacing(0, 1.3f);
            body.addView(d, margins(size(-1, -2), 0, 6, 0, 0));
            addView(body, margins(new LayoutParams(-1, -2), 66, 0, 0, 0));
        }
        @Override protected void onDraw(Canvas c) {
            float cx = Theme.f(22), cy = Theme.f(22), r = Theme.f(21);
            if (!last) {
                paint.setStyle(Paint.Style.FILL);
                if (state == DONE) paint.setShader(new LinearGradient(0, cy + Theme.f(28), 0, getHeight(), Theme.ZONAI, Theme.tint(0xff2f6f60), Shader.TileMode.CLAMP));
                else paint.setColor(Theme.tint(0xff33433b));
                c.drawRect(cx - Theme.f(1), cy + Theme.f(28), cx + Theme.f(1), getHeight() - Theme.f(6), paint);
                paint.setShader(null);
            }
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(state == CURRENT ? Theme.SEL : Theme.DEEP);
            if (state == CURRENT) paint.setShadowLayer(Theme.f(9), 0, 0, Theme.SEL_GLOW);
            c.drawCircle(cx, cy, r, paint);
            paint.clearShadowLayer();
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(2));
            int ring = state == DONE ? Theme.ZONAI : state == CURRENT ? Theme.SEL : state == FAILED ? Theme.WARN : Theme.tint(0xff44554c);
            paint.setColor(ring);
            if (state == DONE) paint.setShadowLayer(Theme.f(6), 0, 0, Theme.ZONAI_GLOW);
            c.drawCircle(cx, cy, r, paint);
            paint.clearShadowLayer();
            if (state == DONE) { Icon.draw(c, "check", cx - Theme.f(12), cy - Theme.f(12), Theme.f(24), Theme.ZONAI, paint); return; }
            paint.setStyle(Paint.Style.FILL); paint.setTypeface(Theme.semibold); paint.setTextSize(Theme.f(20)); paint.setTextAlign(Paint.Align.CENTER);
            paint.setColor(state == CURRENT ? Theme.KEY_INK : state == FAILED ? Theme.WARN : Theme.TEXT3);
            Paint.FontMetrics fm = paint.getFontMetrics();
            c.drawText(state == FAILED ? "!" : String.valueOf(number), cx, cy - (fm.ascent + fm.descent) / 2, paint);
        }
    }

    /** A file or route card (.card): icon box, label + hint, and a state column on the right. */
    static final class Card extends LinearLayout {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF box = new RectF();
        private final Icon icon;
        final TextView label, hint, state, detail;
        private int stateColor = Theme.TEXT2;
        Card(Context context, String iconName, String labelText, String hintText, View.OnClickListener click) {
            super(context);
            setOrientation(HORIZONTAL); setGravity(Gravity.CENTER_VERTICAL); setWillNotDraw(false);
            setFocusable(true); setClickable(true); setOnClickListener(click); setDefaultFocusHighlightEnabled(false);
            setPadding(Theme.u(30), Theme.u(18), Theme.u(30), Theme.u(18));
            icon = new Icon(context, iconName, Theme.ZONAI);
            int pad = Theme.u(19);
            icon.setPadding(pad, pad, pad, pad);
            addView(icon, margins(size(78, 78), 0, 0, 28, 0));
            LinearLayout body = column(context);
            label = Theme.text(context, labelText, false, 28, Theme.TEXT);
            body.addView(label);
            hint = Theme.text(context, hintText, false, 18, Theme.TEXT3);
            hint.setLineSpacing(0, 1.25f);
            body.addView(hint, margins(size(-1, -2), 0, 6, 0, 0));
            addView(body, new LayoutParams(0, -2, 1));
            LinearLayout right = column(context);
            right.setGravity(Gravity.END);
            state = Theme.text(context, "", false, 19, Theme.TEXT2);
            state.setGravity(Gravity.END);
            // Wrap-content width (not the column's default match-parent): a fixed-width TextView keeps the width of its
            // previous, shorter text when only the text changes, which clipped "✓ RomFS image (1 part, …)" to one line.
            right.addView(state, size(-2, -2));
            detail = Theme.text(context, "", false, 16, Theme.TEXT3);
            detail.setGravity(Gravity.END); detail.setMaxLines(3); detail.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            right.addView(detail, margins(size(-2, -2), 0, 4, 0, 0));
            addView(right, margins(new LayoutParams(-2, -2), 20, 0, 0, 0));
            right.setMinimumWidth(Theme.u(200));
        }
        Card state(String text, int color, String small) {
            state.setText(text); stateColor = color; state.setTextColor(lit() ? (color == Theme.TEXT2 ? Theme.SEL : color) : color);
            detail.setText(small == null ? "" : small); detail.setVisibility(small == null || small.isEmpty() ? GONE : VISIBLE);
            requestLayout();   // the right column is re-measured for the new state text
            return this;
        }
        /** The right column's width cap, so long file names wrap instead of squeezing the label. */
        Card stateWidth(float max) { detail.setMaxWidth(Theme.u(max)); state.setMaxWidth(Theme.u(max)); return this; }
        private boolean lit() { return isFocused() || isPressed(); }
        @Override protected void drawableStateChanged() {
            super.drawableStateChanged();
            boolean lit = lit();
            icon.color(lit ? Theme.SEL : Theme.ZONAI);
            if (state != null) state.setTextColor(lit && stateColor == Theme.TEXT2 ? Theme.SEL : stateColor);
            invalidate();
        }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), r = Theme.f(16);
            box.set(0, 0, w, h);
            boolean lit = lit();
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.GLASS2); c.drawRoundRect(box, r, r, paint);
            if (lit) {
                paint.setShader(new LinearGradient(0, 0, w, 0, new int[]{Theme.SEL_FILL, 0}, new float[]{0, .7f}, Shader.TileMode.CLAMP));
                c.drawRoundRect(box, r, r, paint);
                paint.setShader(null);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(2)); paint.setColor(Theme.SEL); paint.setShadowLayer(Theme.f(13), 0, 0, Theme.SEL_GLOW);
                c.drawRoundRect(Theme.f(1), Theme.f(1), w - Theme.f(1), h - Theme.f(1), r, r, paint);
                paint.clearShadowLayer();
            } else {
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(1)); paint.setColor(Theme.RULE);
                c.drawRoundRect(Theme.f(.5f), Theme.f(.5f), w - Theme.f(.5f), h - Theme.f(.5f), r, r, paint);
            }
            float ix = icon.getLeft() - icon.getPaddingLeft() + icon.getPaddingLeft(), iy = icon.getTop();
            RectF ib = new RectF(ix, iy, ix + icon.getWidth(), iy + icon.getHeight());
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new RadialGradient(ib.centerX(), ib.centerY(), ib.width() / 2, new int[]{Theme.withAlpha(Theme.ZONAI, 0x1f), 0}, new float[]{0, .7f}, Shader.TileMode.CLAMP));
            c.drawRoundRect(ib, Theme.f(16), Theme.f(16), paint);
            paint.setShader(null);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(1.5f)); paint.setColor(lit ? Theme.withAlpha(Theme.SEL, 0xb3) : Theme.tint(0xff3f5149));
            ib.inset(Theme.f(.75f), Theme.f(.75f));
            c.drawRoundRect(ib, Theme.f(16), Theme.f(16), paint);
        }
    }

    /** The storage line (.store .bar): used space, what setup will add (hatched), and the rest. */
    static final class StorageBar extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float used, adding;
        private boolean enough = true;
        StorageBar(Context context) { super(context); }
        void set(long total, long free, long need) {
            used = total <= 0 ? 0 : Math.max(0, Math.min(1, (total - free) / (float) total));
            adding = total <= 0 ? 0 : Math.max(0, Math.min(1 - used, need / (float) total));
            enough = free >= need;
            invalidate();
        }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), r = h / 2;
            c.save();
            Path clip = new Path(); clip.addRoundRect(0, 0, w, h, r, r, Path.Direction.CW); c.clipPath(clip);
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.tint(0xff26312c)); c.drawRect(0, 0, w, h, paint);
            paint.setColor(Theme.ZONAI); c.drawRect(0, 0, w * used, h, paint);
            float x0 = w * used, x1 = w * (used + adding);
            paint.setColor(enough ? Theme.tint(0xff9fb257) : Theme.WARN); c.drawRect(x0, 0, x1, h, paint);
            paint.setColor(enough ? Theme.SEL : 0xfff0a080); paint.setStrokeWidth(Theme.f(6)); paint.setStyle(Paint.Style.STROKE);
            c.clipRect(x0, 0, x1, h);
            for (float x = x0 - h; x < x1 + h; x += Theme.f(17)) c.drawLine(x, h, x + h, 0, paint);
            c.restore();
        }
    }

    /** The install ring (06-setup-install): 72 outer ticks lit up to the progress, a gold ring and the lime arc. */
    static final class Ring extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.Rect bounds = new android.graphics.Rect();
        private float fraction;
        private String big = "", stage = "", eta = "";
        Ring(Context context) { super(context); }
        /** fraction < 0 = no total yet (rotating segment); big is the percentage or "…", stage and eta the lines below. */
        void set(float fraction, String big, String stage, String eta) {
            if (this.fraction == fraction && this.big.equals(big) && this.stage.equals(stage) && this.eta.equals(eta)) return;
            this.fraction = fraction; this.big = big; this.stage = stage; this.eta = eta; invalidate();
        }
        @Override protected void onDraw(Canvas c) {
            float size = Math.min(getWidth(), getHeight()), k = size / 560f, cx = getWidth() / 2f, cy = getHeight() / 2f, r = 240 * k;
            boolean sweep = fraction < 0;
            float p = sweep ? 0 : Math.min(1, fraction);
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new RadialGradient(cx, cy, r - 30 * k, Theme.tint(0xff1d3830), Theme.tint(0x000d1512), Shader.TileMode.CLAMP));
            c.drawCircle(cx, cy, r - 30 * k, paint);
            paint.setShader(null);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeWidth(3 * k);
            for (int i = 0; i < 72; i++) {
                double a = i / 72.0 * Math.PI * 2 - Math.PI / 2;
                float l = (i % 6 == 0 ? 18 : 9) * k, cos = (float) Math.cos(a), sin = (float) Math.sin(a);
                paint.setColor(i / 72f < p ? Theme.ZONAI : Theme.tint(0xff33433b));
                c.drawLine(cx + cos * r, cy + sin * r, cx + cos * (r - l), cy + sin * (r - l), paint);
            }
            paint.setStrokeWidth(2 * k); paint.setColor(Theme.alpha(Theme.GOLD_DIM, .6f));
            c.drawCircle(cx, cy, r - 40 * k, paint);
            float ar = r - 56 * k;
            paint.setStrokeWidth(10 * k); paint.setColor(Theme.tint(0xff26332d)); c.drawCircle(cx, cy, ar, paint);
            RectF oval = new RectF(cx - ar, cy - ar, cx + ar, cy + ar);
            paint.setColor(Theme.SEL); paint.setShadowLayer(8 * k, 0, 0, Theme.SEL_GLOW);
            if (sweep) {   // unknown total: a slow rotating segment
                float t = (SystemClock.uptimeMillis() % 2400) / 2400f;
                c.drawArc(oval, t * 360 - 90, 70, false, paint);
                postInvalidateOnAnimation();
            } else if (p > 0) c.drawArc(oval, -90, p * 360, false, paint);
            paint.clearShadowLayer();
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.GOLD);
            Theme.diamond(c, cx, cy - r + 56 * k, 11 * k, paint);
            // The percentage is centred on the ring by its drawn glyph bounds (Align.CENTER uses the advance width, whose
            // side bearings leave "%" off-centre); the lines below are centred horizontally the same way and sit on the
            // font's line box, so descenders do not make them jump.
            paint.setStyle(Paint.Style.FILL); paint.setTextAlign(Paint.Align.LEFT);
            paint.setTypeface(Theme.semibold); paint.setTextSize(96 * k); paint.setColor(0xfff6f0d8);
            paint.setShadowLayer(30 * k, 0, 0, Theme.ZONAI_GLOW);
            centred(c, big, cx, cy - 26 * k, true);
            paint.clearShadowLayer();
            paint.setTypeface(Theme.regular); paint.setTextSize(22 * k); paint.setColor(Theme.TEXT2);
            centred(c, stage, cx, cy + 50 * k, false);
            paint.setTextSize(18 * k); paint.setColor(Theme.TEXT3);
            centred(c, eta, cx, cy + 86 * k, false);
        }
        private void centred(Canvas c, String text, float x, float y, boolean ink) {
            if (text.isEmpty()) return;
            paint.getTextBounds(text, 0, text.length(), bounds);
            Paint.FontMetrics fm = paint.getFontMetrics();
            c.drawText(text, x - bounds.exactCenterX(), y - (ink ? bounds.exactCenterY() : (fm.ascent + fm.descent) / 2), paint);
        }
    }

    /** One install step (.st): state circle, title + detail (+ "optional"), a mini progress bar, status on the right. */
    static final class StageRow extends LinearLayout {
        static final int PENDING = 0, ACTIVE = 1, DONE = 2, FAILED = 3, SKIPPED = 4;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final TextView detail, status;
        private final View mini;
        private int state;
        private float fraction;
        StageRow(Context context, String name, String optionalTag) {
            super(context);
            setOrientation(HORIZONTAL); setGravity(Gravity.CENTER_VERTICAL); setWillNotDraw(false);
            LinearLayout body = column(context);
            LinearLayout title = row(context);
            title.setGravity(Gravity.BOTTOM);
            title.addView(Theme.text(context, name, false, 24, Theme.TEXT));
            if (optionalTag != null) {
                TextView tag = Theme.text(context, optionalTag.toUpperCase(java.util.Locale.ROOT), false, 14, Theme.TEXT3);
                tag.setLetterSpacing(.14f);
                title.addView(tag, margins(size(-2, -2), 12, 0, 0, 3));
            }
            body.addView(title);
            detail = Theme.text(context, "", false, 17, Theme.TEXT3);
            detail.setSingleLine(true); detail.setEllipsize(TextUtils.TruncateAt.END);
            body.addView(detail, margins(size(-1, -2), 0, 3, 0, 0));
            mini = new View(context) {
                @Override protected void onDraw(Canvas c) {
                    float w = getWidth(), h = getHeight();
                    paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.tint(0xff2a3530)); c.drawRoundRect(0, 0, w, h, h / 2, h / 2, paint);
                    float end = fraction < 0 ? w : w * Math.min(1, fraction);
                    float start = 0;
                    if (fraction < 0) {
                        float t = (SystemClock.uptimeMillis() % 1600) / 1600f;
                        start = Math.max(0, (t * 1.4f - .4f) * w); end = Math.min(w, t * 1.4f * w);
                        postInvalidateOnAnimation();
                    }
                    if (end > start) { paint.setColor(Theme.SEL); c.drawRoundRect(start, 0, end, h, h / 2, h / 2, paint); }
                }
            };
            body.addView(mini, margins(size(520, 5), 0, 10, 0, 0));
            addView(body, margins(new LayoutParams(0, -2, 1), 62, 0, 0, 0));
            status = Theme.text(context, "", false, 19, Theme.TEXT3);
            status.setGravity(Gravity.END);
            addView(status, margins(new LayoutParams(-2, -2), 12, 0, 0, 0));
            status.setMinWidth(Theme.u(180));
        }
        void set(int state, float fraction, String statusText, String detailText) {
            this.state = state; this.fraction = fraction;
            status.setText(statusText);
            status.setTextColor(state == DONE ? Theme.ZONAI : state == ACTIVE ? Theme.SEL : state == FAILED ? Theme.WARN : state == SKIPPED ? Theme.AMBER : Theme.TEXT3);
            detail.setText(detailText == null ? "" : detailText);
            detail.setVisibility(detailText == null || detailText.isEmpty() ? GONE : VISIBLE);
            mini.setVisibility(state == ACTIVE ? VISIBLE : GONE);
            mini.invalidate(); invalidate();
        }
        @Override protected void onDraw(Canvas c) {
            float cx = Theme.f(20), cy = getHeight() / 2f, r = Theme.f(19);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Theme.f(2));
            int ring = state == DONE ? Theme.ZONAI : state == ACTIVE ? Theme.SEL : state == FAILED ? Theme.WARN : state == SKIPPED ? Theme.AMBER : Theme.tint(0xff44554c);
            paint.setColor(ring);
            if (state == ACTIVE) paint.setShadowLayer(Theme.f(7), 0, 0, Theme.SEL_GLOW);
            c.drawCircle(cx, cy, r, paint);
            paint.clearShadowLayer();
            if (state == DONE) Icon.draw(c, "check", cx - Theme.f(11), cy - Theme.f(11), Theme.f(22), Theme.ZONAI, paint);
            else if (state == ACTIVE) {
                float t = (SystemClock.uptimeMillis() % 1400) / 1400f, pulse = (float) (.5 + .5 * Math.sin(t * Math.PI * 2));
                paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SEL); paint.setShadowLayer(Theme.f(3 + 5 * pulse), 0, 0, Theme.SEL_GLOW);
                Theme.diamond(c, cx, cy, Theme.f(8.5f), paint);
                paint.clearShadowLayer();
                postInvalidateOnAnimation();
            } else if (state == FAILED || state == SKIPPED) {
                paint.setStrokeWidth(Theme.f(2)); paint.setStrokeCap(Paint.Cap.ROUND);
                c.drawLine(cx - Theme.f(7), cy, cx + Theme.f(7), cy, paint);
            }
            paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.RULE_SOFT);
            c.drawRect(0, getHeight() - Math.max(1, Theme.f(1)), getWidth(), getHeight(), paint);
        }
    }

    /** "GPU cost  ▰▰▱▱▱" style key/value rows (.facts): a fixed key column and any value view. */
    static LinearLayout fact(Context context, String key, View value) {
        LinearLayout row = row(context);
        TextView k = Theme.text(context, key, false, 20, Theme.TEXT3);
        row.addView(k, size(220, -2));
        row.addView(value, new LinearLayout.LayoutParams(0, -2, 1));
        row.setPadding(0, Theme.u(9), 0, Theme.u(9));
        return row;
    }

    /** Save slots (Saves detail panel, import preview, backups): a grid of the slot's picture from caption.sav, its
     * number and date. A slot without a picture shows a plain tile. Two columns at full size; when the space the panel
     * leaves is smaller (small screens, long lead text), the grid takes two or three columns, whichever fits larger,
     * and scales down so it stays inside the panel. */
    static final class SlotGrid extends View {
        static final class Cell {
            final android.graphics.Bitmap picture; final String title, time;
            Cell(android.graphics.Bitmap picture, String title, String time) { this.picture = picture; this.title = title; this.time = time; }
        }
        static final float W = 224, H = 126, GAP = 24, LABEL = 50;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final android.text.TextPaint label = new android.text.TextPaint(Paint.ANTI_ALIAS_FLAG);
        private final List<Cell> cells = new ArrayList<>();
        private final RectF box = new RectF();
        private final Path clip = new Path();
        private int columns = 2;
        private float scale = 1;
        SlotGrid(Context context) { super(context); }
        SlotGrid set(List<Cell> list) { cells.clear(); cells.addAll(list); requestLayout(); invalidate(); return this; }
        private static float width(int cols) { return Theme.f(cols * W + (cols - 1) * GAP); }
        private int rows(int cols) { return (cells.size() + cols - 1) / cols; }
        @Override protected void onMeasure(int ws, int hs) {
            int wMode = MeasureSpec.getMode(ws), hMode = MeasureSpec.getMode(hs);
            float availW = wMode == MeasureSpec.UNSPECIFIED ? Float.MAX_VALUE : MeasureSpec.getSize(ws);
            float availH = hMode == MeasureSpec.UNSPECIFIED ? Float.MAX_VALUE : MeasureSpec.getSize(hs);
            columns = 2; scale = 1;
            float best = -1;
            for (int cols = 2; cols <= 3; cols++) {
                float s = Math.min(1, Math.min(availW / width(cols), availH / Math.max(1, Theme.f(rows(cols) * (H + LABEL)))));
                if (s > best + 1e-3f) { best = s; columns = cols; scale = s; }
                if (s >= 1) break;   // two columns at full size need no more
            }
            int w = (int) Math.ceil(width(columns) * scale), h = (int) Math.ceil(Theme.f(rows(columns) * (H + LABEL)) * scale);
            setMeasuredDimension(wMode == MeasureSpec.EXACTLY ? MeasureSpec.getSize(ws) : Math.min(w, (int) Math.min(availW, Integer.MAX_VALUE)),
                    hMode == MeasureSpec.EXACTLY ? MeasureSpec.getSize(hs) : Math.min(h, (int) Math.min(availH, Integer.MAX_VALUE)));
        }
        @Override protected void onDraw(Canvas c) {
            if (scale <= 0) return;
            c.save();
            c.scale(scale, scale);
            float r = Theme.f(8);
            for (int i = 0; i < cells.size(); i++) {
                Cell cell = cells.get(i);
                float x = Theme.f((i % columns) * (W + GAP)), y = Theme.f((i / columns) * (H + LABEL));
                box.set(x, y, x + Theme.f(W), y + Theme.f(H));
                paint.setStyle(Paint.Style.FILL); paint.setColor(Theme.SLATE);
                c.drawRoundRect(box, r, r, paint);
                if (cell.picture != null) {
                    c.save();
                    clip.rewind();
                    clip.addRoundRect(box, r, r, Path.Direction.CW);
                    c.clipPath(clip);
                    c.drawBitmap(cell.picture, null, box, paint);
                    c.restore();
                } else {
                    paint.setColor(Theme.TEXT3);
                    Icon.draw(c, "save", box.centerX() - Theme.f(18), box.centerY() - Theme.f(18), Theme.f(36), Theme.TEXT3, paint);
                }
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Math.max(1, Theme.f(1))); paint.setColor(Theme.RULE);
                c.drawRoundRect(box, r, r, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setTypeface(Theme.semibold); paint.setTextSize(Theme.f(19)); paint.setColor(Theme.TEXT);
                c.drawText(cell.title, x, y + Theme.f(H + 24), paint);
                label.setTypeface(Theme.regular); label.setTextSize(Theme.f(16)); label.setColor(Theme.TEXT2);
                c.drawText(TextUtils.ellipsize(cell.time, label, Theme.f(W), TextUtils.TruncateAt.END).toString(), x, y + Theme.f(H + 44), label);
            }
            c.restore();
        }
    }
}
