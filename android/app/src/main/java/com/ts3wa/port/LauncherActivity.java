package com.ts3wa.port;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * First screen: if the game files are not installed yet, asks the player for
 * their own copy of the game (.ipa or a converted ZIP .obb) and extracts it. Nothing from the
 * original game is shipped with this app.
 */
public class LauncherActivity extends Activity {
    static final String APP_DIR = "sims3wa_iphone.app";
    static final String EXE = "sims3wa_iphone";
    private static final String APP_PREFIX = "Payload/" + APP_DIR + "/";
    private static final int PICK_PACKAGE = 1;

    private TextView mStatus;
    private ProgressBar mProgress;
    private Button mButton;
    private volatile boolean mBusy;

    static File gameDir(Activity a) {
        return new File(a.getExternalFilesDir(null), APP_DIR);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (new File(gameDir(this), EXE).isFile()) {
            startGame();
            return;
        }
        buildUi();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (new File(gameDir(this), EXE).isFile()) startGame();
    }

    private void startGame() {
        Intent game = new Intent(this, GameActivity.class);
        game.putExtra("screen_settings", "com.ts3wa.port.SCREEN_SETTINGS".equals(getIntent().getAction()));
        startActivity(game);
        finish();
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(dp(32), dp(24), dp(32), dp(24));
        root.setBackgroundColor(Color.rgb(24, 32, 48));

        TextView title = new TextView(this);
        title.setText(R.string.import_title);
        title.setTextSize(24);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        TextView body = new TextView(this);
        body.setText(R.string.import_body);
        body.setTextSize(15);
        body.setTextColor(Color.rgb(210, 220, 235));
        body.setPadding(0, dp(16), 0, dp(16));
        root.addView(body);

        mButton = new Button(this);
        mButton.setText(R.string.import_button);
        mButton.setOnClickListener(v -> pickPackage());
        root.addView(mButton);

        mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgress.setMax(1000);
        mProgress.setVisibility(View.GONE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(24));
        lp.topMargin = dp(16);
        root.addView(mProgress, lp);

        mStatus = new TextView(this);
        mStatus.setTextColor(Color.rgb(255, 210, 120));
        mStatus.setTextSize(14);
        mStatus.setPadding(0, dp(8), 0, 0);
        root.addView(mStatus);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(24, 32, 48));
        scroll.addView(root);
        setContentView(scroll);
    }

    private void pickPackage() {
        if (mBusy) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_PACKAGE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_PACKAGE || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        mBusy = true;
        mButton.setEnabled(false);
        mProgress.setProgress(0);
        mProgress.setVisibility(View.VISIBLE);
        mStatus.setText(R.string.import_working);
        new Thread(() -> {
            String error;
            try {
                error = extract(uri);
            } catch (IOException e) {
                deleteRecursive(new File(getExternalFilesDir(null), APP_DIR + ".partial"));
                error = getString(R.string.import_error_io) + " " + e.getMessage();
            }
            final String err = error;
            runOnUiThread(() -> {
                mBusy = false;
                mButton.setEnabled(true);
                if (err == null) {
                    startGame();
                } else {
                    mProgress.setVisibility(View.GONE);
                    mStatus.setText(err);
                }
            });
        }, "game-import").start();
    }

    private long contentLength(Uri uri) {
        try (android.content.res.AssetFileDescriptor fd = getContentResolver().openAssetFileDescriptor(uri, "r")) {
            return fd != null ? fd.getLength() : -1;
        } catch (Exception e) {
            return -1;
        }
    }

    /** IPA and converted OBB use the same ZIP layout. Saves live outside the extracted bundle. */
    private String extract(Uri uri) throws IOException {
        File base = getExternalFilesDir(null);
        File tmp = new File(base, APP_DIR + ".partial");
        deleteRecursive(tmp);
        final long total = contentLength(uri);
        boolean sawApp = false;
        String root = tmp.getCanonicalPath() + File.separator;
        byte[] buf = new byte[1 << 16];
        try (InputStream raw = getContentResolver().openInputStream(uri);
             CountingStream counted = new CountingStream(new BufferedInputStream(raw, 1 << 16));
             ZipInputStream zip = new ZipInputStream(counted)) {
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                String name = e.getName();
                if (!name.startsWith(APP_PREFIX)) continue;
                String rel = name.substring(APP_PREFIX.length());
                if (rel.isEmpty()) continue;
                sawApp = true;
                File out = new File(tmp, rel);
                if (!out.getCanonicalPath().startsWith(root)) {
                    deleteRecursive(tmp);
                    return getString(R.string.import_error_not_package);
                }
                if (e.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null) parent.mkdirs();
                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buf)) > 0) os.write(buf, 0, n);
                }
                if (total > 0) {
                    final int p = (int) (counted.count * 1000 / total);
                    runOnUiThread(() -> mProgress.setProgress(p));
                }
            }
        }
        if (!sawApp) {
            deleteRecursive(tmp);
            return getString(R.string.import_error_not_package);
        }
        File exe = new File(tmp, EXE);
        if (!exe.isFile()) {
            deleteRecursive(tmp);
            return getString(R.string.import_error_wrong_game);
        }
        String check = checkExecutable(exe);
        if (check != null) {
            deleteRecursive(tmp);
            return check;
        }
        File dest = gameDir(this);
        deleteRecursive(dest);
        if (!tmp.renameTo(dest)) return getString(R.string.import_error_io) + " rename";
        return null;
    }

    /** This experimental loader accepts the decrypted thin ARM32 executable. */
    private String checkExecutable(File exe) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(exe, "r")) {
            if (f.length() < 28) return getString(R.string.import_error_invalid_executable);
            int magic = f.readInt();
            long sliceOff = -1;
            long sliceSize = f.length();
            if (magic == 0xCAFEBABE) {
                int n = f.readInt();
                if (n < 0 || n > (f.length() - 8) / 20) return getString(R.string.import_error_invalid_executable);
                for (int i = 0; i < n; i++) {
                    int cpu = f.readInt();
                    f.readInt();  // subtype
                    int off = f.readInt();
                    long size = f.readInt() & 0xFFFFFFFFL;
                    f.readInt();  // align
                    if (cpu == 12) {
                        sliceOff = off & 0xFFFFFFFFL;
                        sliceSize = size;
                    }
                }
            } else if (magic == 0xCEFAEDFE) {
                sliceOff = 0;
            }
            if (sliceOff < 0) return getString(R.string.import_error_no_arm64);
            if (sliceSize < 28 || sliceOff + sliceSize > f.length()) return getString(R.string.import_error_invalid_executable);
            f.seek(sliceOff);
            if (f.readInt() != 0xCEFAEDFE) return getString(R.string.import_error_invalid_executable);
            if (Integer.reverseBytes(f.readInt()) != 12) return getString(R.string.import_error_no_arm64);
            f.seek(sliceOff + 16);
            int ncmds = Integer.reverseBytes(f.readInt());
            long commandsSize = Integer.reverseBytes(f.readInt()) & 0xFFFFFFFFL;
            long end = sliceOff + 28 + commandsSize;
            if (end > sliceOff + sliceSize || ncmds < 0 || ncmds > commandsSize / 8) {
                return getString(R.string.import_error_invalid_executable);
            }
            f.seek(sliceOff + 28);
            for (int i = 0; i < ncmds; i++) {
                long at = f.getFilePointer();
                if (at + 8 > end) return getString(R.string.import_error_invalid_executable);
                int cmd = Integer.reverseBytes(f.readInt());
                int size = Integer.reverseBytes(f.readInt());
                if (size < 8 || at + size > end) return getString(R.string.import_error_invalid_executable);
                if (cmd == 0x21) {  // LC_ENCRYPTION_INFO
                    if (size < 20) return getString(R.string.import_error_invalid_executable);
                    f.seek(at + 16);
                    int cryptid = Integer.reverseBytes(f.readInt());
                    if (cryptid != 0) return getString(R.string.import_error_encrypted);
                }
                f.seek(at + size);
            }
            if (f.getFilePointer() != end) return getString(R.string.import_error_invalid_executable);
        }
        return null;
    }

    private static void deleteRecursive(File f) {
        if (!f.exists()) return;
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteRecursive(k);
        f.delete();
    }

    private static final class CountingStream extends java.io.FilterInputStream {
        volatile long count;

        CountingStream(InputStream in) {
            super(in);
        }

        @Override
        public int read() throws IOException {
            int r = super.read();
            if (r >= 0) count++;
            return r;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            int n = super.read(b, off, len);
            if (n > 0) count += n;
            return n;
        }

        @Override
        public long skip(long n) throws IOException {
            long s = super.skip(n);
            count += s;
            return s;
        }
    }
}
