package org.model3recomp.scudrace;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.view.HapticFeedbackConstants;
import android.view.View;

import org.libsdl.app.SDLActivity;

// SDL's Java side loads libSDL2.so, then the game (libmain.so), and calls its
// SDL_main. The touch controls (src/platform_sdl.c) call the two helpers below
// from the game's thread.
public final class MainActivity extends SDLActivity {
    // A label in the system's bold font, white on transparent, cropped to its
    // ink with a pixel to spare: { width, height, ARGB pixels... }.
    public static int[] renderText(String text, float px) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        paint.setTextSize(px);
        paint.setColor(Color.WHITE);
        Rect ink = new Rect();
        paint.getTextBounds(text, 0, text.length(), ink);
        int w = Math.max(1, ink.width() + 2), h = Math.max(1, ink.height() + 2);
        Bitmap bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        new Canvas(bitmap).drawText(text, 1 - ink.left, 1 - ink.top, paint);
        int[] out = new int[2 + w * h];
        out[0] = w;
        out[1] = h;
        bitmap.getPixels(out, 2, w, 0, 0, w, h);
        bitmap.recycle();
        return out;
    }

    // A key's click under the finger, as the system's touch feedback setting allows.
    public static void tick() {
        final View view = mSurface;
        if (view != null)
            view.post(() -> view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY));
    }
}
