package io.taowen.anhyprland.smoke;

import android.app.Activity;
import android.os.Bundle;
import android.view.*;
import android.util.Log;
import java.io.*;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("smoke"); }
    private boolean started;
    private native int run(Surface surface, int width, int height, String home, String libraries);
    private native int window(Surface surface, int width, int height);
    private native int pointer(float x, float y, int button, boolean pressed);
    private native int key(int scanCode, boolean pressed);
    private native int stop();

    private void copyAssets(String path, File destination) throws IOException {
        String[] entries = getAssets().list(path);
        if (entries.length > 0) {
            destination.mkdirs();
            for (String entry : entries)
                copyAssets(path.isEmpty() ? entry : path + "/" + entry, new File(destination, entry));
        } else {
            destination.getParentFile().mkdirs();
            try (InputStream in = getAssets().open(path); OutputStream out = new FileOutputStream(destination)) {
                byte[] bytes = new byte[16384];
                int n;
                while ((n = in.read(bytes)) != -1) out.write(bytes, 0, n);
            }
        }
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            copyAssets("runtime", getFilesDir());
            new File(getFilesDir(), "runtime").mkdirs();
        } catch (IOException error) { throw new RuntimeException(error); }
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        view.setFocusableInTouchMode(true);
        view.setOnTouchListener((v, event) -> {
            int action = event.getActionMasked();
            pointer(event.getX(), event.getY(), action == MotionEvent.ACTION_MOVE ? 0 : 272,
                    action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL);
            return true;
        });
        setContentView(view);
        view.requestFocus();
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { Log.i("anhyprland-smoke", "Surface created"); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (started) { window(holder.getSurface(), width, height); return; }
        started = true;
        Surface surface = holder.getSurface();
        new Thread(() -> Log.i("anhyprland-smoke", "Compositor returned " +
                run(surface, width, height, getFilesDir().getAbsolutePath(), getApplicationInfo().nativeLibraryDir)),
                "anhyprland").start();
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { Log.i("anhyprland-smoke", "Surface destroyed"); window(null, 0, 0); }
    private int evdevCode(KeyEvent event) {
        if (event.getScanCode() != 0) return event.getScanCode();
        int code = event.getKeyCode();
        int[] letters = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
        if (code >= KeyEvent.KEYCODE_A && code <= KeyEvent.KEYCODE_Z) return letters[code - KeyEvent.KEYCODE_A];
        if (code >= KeyEvent.KEYCODE_1 && code <= KeyEvent.KEYCODE_9) return 2 + code - KeyEvent.KEYCODE_1;
        switch (code) {
            case KeyEvent.KEYCODE_0: return 11;
            case KeyEvent.KEYCODE_ENTER: return 28;
            case KeyEvent.KEYCODE_DEL: return 14;
            case KeyEvent.KEYCODE_TAB: return 15;
            case KeyEvent.KEYCODE_SPACE: return 57;
            case KeyEvent.KEYCODE_ESCAPE: return 1;
            case KeyEvent.KEYCODE_DPAD_UP: return 103;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 108;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 105;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 106;
            default: return 0;
        }
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int code = evdevCode(event);
        if (code != 0 && event.getRepeatCount() == 0) {
            key(code, event.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public void onBackPressed() { stop(); finish(); }
    @Override protected void onDestroy() { stop(); super.onDestroy(); }
}
