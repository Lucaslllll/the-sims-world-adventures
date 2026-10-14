package com.ts3wa.port;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Text rasterisation for the CoreGraphics HLE (UIKit string drawing).
 * Uses the system fonts, so every language the game ships (incl. CJK) works.
 */
public final class TextRenderer {
    private static final Paint sRegular = makePaint(false);
    private static final Paint sBold = makePaint(true);

    private static Paint makePaint(boolean bold) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        p.setTypeface(bold ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        p.setColor(Color.WHITE);
        return p;
    }

    private static synchronized Paint paint(float size, boolean bold) {
        Paint p = bold ? sBold : sRegular;
        p.setTextSize(size);
        return p;
    }

    /** Returns {width, lineHeight, ascent}. */
    public static synchronized float[] measure(String text, float size, boolean bold) {
        Paint p = paint(size, bold);
        Paint.FontMetrics fm = p.getFontMetrics();
        return new float[] { p.measureText(text), fm.descent - fm.ascent, -fm.ascent };
    }

    /**
     * Renders text into an 8-bit coverage mask. Layout of the returned bytes:
     * int32 width, int32 height, int32 rowBytes, int32 ascentPx, then rows.
     * The text's top-left corner is at mask pixel (0, 0).
     */
    public static synchronized byte[] render(String text, float size, boolean bold) {
        return renderScaled(text, size, bold, 1);
    }

    /** Native-resolution glyphs with unchanged logical advances and line metrics. */
    public static synchronized byte[] renderScaled(String text, float size, boolean bold, int scale) {
        scale = Math.max(1, Math.min(4, scale));
        Paint p = paint(size, bold);
        Paint.FontMetrics fm = p.getFontMetrics();
        int w = Math.max(1, (int) Math.ceil(p.measureText(text)) + 2) * scale;
        int h = Math.max(1, (int) Math.ceil(fm.descent - fm.ascent) + 2) * scale;
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ALPHA_8);
        Canvas c = new Canvas(bmp);
        c.scale(scale, scale);
        c.drawText(text, 0, -fm.ascent, p);
        int rowBytes = bmp.getRowBytes();
        ByteBuffer out = ByteBuffer.allocate(16 + rowBytes * h).order(ByteOrder.LITTLE_ENDIAN);
        out.putInt(w).putInt(h).putInt(rowBytes).putInt((int) Math.ceil(-fm.ascent) * scale);
        ByteBuffer px = ByteBuffer.allocate(rowBytes * h);
        bmp.copyPixelsToBuffer(px);
        bmp.recycle();
        out.put(px.array());
        return out.array();
    }

    private TextRenderer() {}
}
