package com.aure.totk.companion;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import org.json.JSONArray;
import org.json.JSONObject;

/** World X/Z projection; bitmap tiles are independently bounded by AssetStore's map LRU. */
final class MapRenderer {
    private final AssetStore assets;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final RectF tile = new RectF();
    private final Path path = new Path();
    private final android.util.SparseArray<android.graphics.ColorFilter> tints = new android.util.SparseArray<>();
    private float density;
    double centerX, centerZ;
    float zoom = 2f, pixelsPerWorld;
    boolean following = true;
    String layer = "Ground";
    boolean automaticLayer = true;
    /** The main page's round minimap: a circle, framed by the page itself (no map frame or north plate of its own). */
    boolean round;
    private float cx, cy;
    private double minX, maxX, minZ, maxZ;

    MapRenderer(AssetStore assets, float density) { this.assets = assets; this.density = density; }
    void setUnit(float unit) { density = unit; }
    void recenter() { following = true; automaticLayer = true; }
    /** Where the player is drawn (and followed) in the next draw, in place of the snapshot's last sample: the view glides
     * the marker between two samples (Motion). heading NaN: the snapshot's. */
    private boolean marked;
    private double markedX, markedZ, markedHeading;
    void marker(double x, double z, double heading) { marked = true; markedX = x; markedZ = z; markedHeading = heading; }
    void noMarker() { marked = false; }
    void pan(float dx, float dy) {
        if (pixelsPerWorld <= 0) return;
        following = false;
        centerX -= dx / pixelsPerWorld; centerZ -= dy / pixelsPerWorld;
        centerX = Math.max(minX, Math.min(maxX, centerX)); centerZ = Math.max(minZ, Math.min(maxZ, centerZ));
    }
    void scale(float factor, float focusX, float focusY) {
        float before = zoom;
        zoom = Math.max(1f, Math.min(32f, zoom * factor));
        if (pixelsPerWorld > 0) {
            following = false;
            centerX += (focusX - cx) / pixelsPerWorld * (1 - before / zoom);
            centerZ += (focusY - cy) / pixelsPerWorld * (1 - before / zoom);
        }
    }
    void draw(Canvas canvas, RectF bounds, Snapshot data, boolean mini) {
        JSONObject map = Snapshot.object(assets.index, "map"), world = Snapshot.object(map, "world");
        minX = world.optDouble("minX", -6000); maxX = world.optDouble("maxX", 6000);
        minZ = world.optDouble("minZ", -5000); maxZ = world.optDouble("maxZ", 5000);
        if (maxX <= minX || maxZ <= minZ) return;
        if (automaticLayer || mini) layer = data.player.optString("layer", "Ground");
        double px = marked ? markedX : data.coordinate(0), pz = marked ? markedZ : data.coordinate(2);
        if ((following || mini) && Double.isFinite(px) && Double.isFinite(pz)) { centerX = px; centerZ = pz; }
        cx = bounds.centerX(); cy = bounds.centerY();
        pixelsPerWorld = Math.min(bounds.width() / (float)(maxX - minX), bounds.height() / (float)(maxZ - minZ)) * (mini ? 9 : zoom);
        canvas.save();
        if (mini && (round || !assets.hasDecor("map_frame"))) {
            path.reset(); path.addCircle(cx, cy, bounds.width() / 2, Path.Direction.CW); canvas.clipPath(path);
        } else canvas.clipRect(bounds);
        canvas.drawColor(assets.color("background", Color.rgb(24, 36, 33)));
        paint.setColor(assets.color("accentDim", Color.rgb(48, 68, 60))); paint.setStrokeWidth(density);
        for (int n = -12; n <= 12; n++) {
            float x = x(n * 1000), y = y(n * 1000);
            canvas.drawLine(x, bounds.top, x, bounds.bottom, paint);
            canvas.drawLine(bounds.left, y, bounds.right, y, paint);
        }
        paint.setColor(Color.WHITE);
        Bitmap overview = assets.bitmap("map/" + layer + "/overview.webp", true);
        tile.set(x(minX), y(minZ), x(maxX), y(maxZ));
        if (overview != null) canvas.drawBitmap(overview, null, tile, paint);
        int cols = Math.max(1, map.optInt("cols", 12)), rows = Math.max(1, map.optInt("rows", 10));
        double tw = (maxX - minX) / cols, th = (maxZ - minZ) / rows;
        float tilePixels = (float)tw * pixelsPerWorld;
        if (tilePixels > 200) {
            int lod = tilePixels > 500 ? 1 : 2;
            int firstCol = Math.max(0, (int)Math.floor((centerX + (bounds.left - cx) / pixelsPerWorld - minX) / tw));
            int lastCol = Math.min(cols - 1, (int)Math.floor((centerX + (bounds.right - cx) / pixelsPerWorld - minX) / tw));
            int firstRow = Math.max(0, (int)Math.floor((centerZ + (bounds.top - cy) / pixelsPerWorld - minZ) / th));
            int lastRow = Math.min(rows - 1, (int)Math.floor((centerZ + (bounds.bottom - cy) / pixelsPerWorld - minZ) / th));
            for (int r = firstRow; r <= lastRow; r++) for (int c = firstCol; c <= lastCol; c++) {
                Bitmap image = assets.bitmap("map/" + layer + "/" + lod + "/" + c + "_" + r + ".webp", true);
                if (image == null) continue;
                tile.set(x(minX + c * tw), y(minZ + r * th), x(minX + (c + 1) * tw), y(minZ + (r + 1) * th));
                canvas.drawBitmap(image, null, tile, paint);
            }
        }
        JSONObject dynamic = Snapshot.object(data.root, "map");
        markers(canvas, Snapshot.array(dynamic, "pins"), true, false, bounds);
        markers(canvas, Snapshot.array(dynamic, "stamps"), false, false, bounds);
        if (!mini) markers(canvas, assets.markers, false, true, bounds);
        if (Double.isFinite(px) && Double.isFinite(pz) && layer.equals(data.player.optString("layer", "Ground"))) {
            float sx = x(px), sy = y(pz), radius = (round ? 16 : mini ? 9 : 11) * density;
            canvas.save();
            canvas.rotate((float)(marked && !Double.isNaN(markedHeading) ? markedHeading : data.player.optDouble("heading", 0)), sx, sy);
            Bitmap player = assets.ui("map_player");
            if (player != null) {
                tile.set(sx - radius, sy - radius, sx + radius, sy + radius);
                paint.setColor(Color.WHITE); canvas.drawBitmap(player, null, tile, paint);
            } else {
                path.reset();
                if (data.player.has("heading") && !data.player.isNull("heading")) {
                    path.moveTo(sx, sy - radius); path.lineTo(sx + radius * .65f, sy + radius);
                    path.lineTo(sx, sy + radius * .5f); path.lineTo(sx - radius * .65f, sy + radius); path.close();
                } else path.addCircle(sx, sy, radius * .6f, Path.Direction.CW);
                paint.setStyle(Paint.Style.FILL); paint.setColor(assets.color("gold", 0xfff4dc98)); canvas.drawPath(path, paint);
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2 * density); paint.setColor(0xff132b27); canvas.drawPath(path, paint);
                paint.setStyle(Paint.Style.FILL);
            }
            canvas.restore();
        }
        canvas.restore();
        if (round) return;
        if (!assets.decor(canvas, "map_frame", bounds, density)) {
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2 * density); paint.setColor(assets.color("gold", 0xffaa9562));
            if (mini) canvas.drawCircle(cx, cy, bounds.width() / 2, paint); else canvas.drawRect(bounds, paint);
            paint.setStyle(Paint.Style.FILL);
        }
        float north = (mini ? 48 : 64) * density;
        tile.set(bounds.right - north - 12 * density, bounds.top + 10 * density,
                bounds.right - 12 * density, bounds.top + north * .32f + 10 * density);
        if (!assets.decor(canvas, "map_north", tile, density)) {
            paint.setTypeface(assets.font); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(14 * density);
            paint.setColor(assets.color("text", 0xffe9e5d5)); canvas.drawText("N", tile.centerX(), tile.centerY(), paint);
        }
    }
    private float x(double value) { return cx + (float)(value - centerX) * pixelsPerWorld; }
    private float y(double value) { return cy + (float)(value - centerZ) * pixelsPerWorld; }
    private void markers(Canvas canvas, JSONArray markers, boolean pin, boolean fixed, RectF bounds) {
        for (int i = 0; i < markers.length(); i++) {
            JSONObject marker = markers.optJSONObject(i); if (marker == null) continue;
            String declared = marker.optString("layer", "");
            if (declared.isEmpty() && marker.has("y")) {
                double altitude = marker.optDouble("y", 0);
                declared = altitude > 800 ? "Sky" : altitude < -100 ? "UnderGround" : "Ground";
            }
            if (!declared.isEmpty() && !declared.equals(layer)) continue;
            double wx = marker.optDouble("x", Double.NaN), wz = marker.optDouble("z", Double.NaN);
            if (!Double.isFinite(wx) || !Double.isFinite(wz)) continue;
            float sx = x(wx), sy = y(wz);
            if (!bounds.contains(sx, sy)) continue;
            String type = marker.optString("type", fixed ? "shrine" : "Star");
            Bitmap icon = assets.ui((pin ? "pin_" : fixed ? "marker_" : "stamp_") + type);
            float r = (fixed ? 7 : 9) * density;
            if (icon != null) {
                int color = pin ? pinColor(type) : fixed ? 0xff75d9b9 : 0xfff2d18b;
                android.graphics.ColorFilter tint = tints.get(color);
                if (tint == null) { tint = new android.graphics.PorterDuffColorFilter(color, android.graphics.PorterDuff.Mode.SRC_IN); tints.put(color, tint); }
                paint.setColorFilter(tint);
                tile.set(sx - r, sy - r, sx + r, sy + r); paint.setColor(Color.WHITE); canvas.drawBitmap(icon, null, tile, paint);
                paint.setColorFilter(null);
            } else {
                paint.setColor(pin ? pinColor(type) : fixed ? 0xff75d9b9 : 0xfff2d18b);
                if (pin) { canvas.drawCircle(sx, sy - r / 2, r / 2, paint); canvas.drawLine(sx, sy, sx, sy + r, paint); }
                else { paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(r * 2); canvas.drawText(fixed ? "◇" : "✦", sx, sy + r / 2, paint); }
            }
        }
    }
    private static int pinColor(String type) {
        switch (type) {
            case "Blue": return 0xff69a9ef; case "Green": return 0xff6fce93; case "Yellow": return 0xffefd16c;
            case "Purple": return 0xffc08de2; case "LightBlue": return 0xff81e5ed; default: return 0xffef756c;
        }
    }
}
