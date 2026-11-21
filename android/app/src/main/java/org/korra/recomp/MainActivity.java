package org.korra.recomp;

import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import com.google.androidgamesdk.GameActivity;

import java.io.File;

/**
 * GameActivity shell for the Korra recompilation.
 *
 * <p>Responsibilities, in order:
 * <ol>
 *   <li>let GameActivity create the SurfaceView and start the native thread
 *       (the library named by the android.app.lib_name meta-data, libkorra.so);</li>
 *   <li>on first run, ask for the game folder through the system file picker
 *       (Storage Access Framework) and copy its GameData tree into
 *       {@code Android/data/org.korra.recomp/files/game/};</li>
 *   <li>tell the native side when the data is in place.</li>
 * </ol>
 *
 * <p>No storage permission is requested: SAF hands the app a tree URI and the
 * copy is done by the app into its own external files directory. The copy is
 * ~2.2 GB (data000.cpk + data001.cpk + the loose sound/movie folders) and runs
 * on a worker thread with toasts as progress, which is enough for a first-run
 * flow; a progress bar belongs to the M3 session if the toasts are not enough.
 *
 * <p>The native side never touches the SAF URI. It only sees a plain directory
 * path, which is the contract docs/ANDROID_BRINGUP.md describes for the file
 * layer.
 */
public class MainActivity extends GameActivity {
    private static final String TAG = "Korra";
    private static final int REQUEST_PICK_TREE = 0x4b4f; // 'KO'

    /** Marker written after a complete copy; the native side also checks it. */
    static final String INSTALL_MARKER = "install.complete";

    private TextView overlay;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile boolean copying;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        overlay = new TextView(this);
        overlay.setTextColor(Color.WHITE);
        overlay.setBackgroundColor(0xCC000000);
        overlay.setTextSize(20f);
        overlay.setGravity(Gravity.CENTER);
        overlay.setPadding(32, 32, 32, 32);
        addContentView(overlay,
                new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));
        overlay.setOnClickListener(v -> pickGameFolder());

        /* Internal storage, not getExternalFilesDir(): Android 17 refuses the
         * app write access to its own external files dir, and mkdirs() there
         * fails outright - the installer reported "cannot create
         * .../files/game/GameData" on the S24. Internal storage is also what the
         * native side probes first. Only the destination moved; the SAF picker
         * read side is unchanged. */
        File internal = getFilesDir();
        if (internal == null) {
            showMessage("App files directory is unavailable.", false);
            return;
        }
        File gameRoot = new File(internal, "game");
        File gameData = new File(gameRoot, "GameData");
        if (GameDataInstaller.isInstalled(gameRoot)) {
            overlay.setVisibility(TextView.GONE);
            Log.i(TAG, "game data present at " + gameData.getAbsolutePath());
            nativeGameDataReady(gameData.getAbsolutePath());
        } else {
            showMessage(getString(R.string.pick_prompt) + "\n\n"
                    + getString(R.string.pick_hint), true);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
    }

    private void showMessage(String text, boolean clickable) {
        overlay.setText(text);
        overlay.setVisibility(TextView.VISIBLE);
        overlay.setClickable(clickable);
    }

    private void pickGameFolder() {
        if (copying) {
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_PICK_TREE);
        } catch (RuntimeException e) {
            // Some vendors ship a device without a documents provider.
            showMessage(getString(R.string.copy_failed, e.toString()), true);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_TREE || resultCode != RESULT_OK || data == null) {
            return;
        }
        Uri tree = data.getData();
        if (tree == null) {
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // Not fatal: the copy starts now while the grant is live.
        }

        File internal = getFilesDir();
        if (internal == null) {
            return;
        }
        File gameRoot = new File(internal, "game");
        if (gameRoot.getUsableSpace() < GameDataInstaller.REQUIRED_BYTES) {
            showMessage(getString(R.string.no_space,
                    GameDataInstaller.human(GameDataInstaller.REQUIRED_BYTES),
                    GameDataInstaller.human(gameRoot.getUsableSpace())), true);
            return;
        }

        copying = true;
        showMessage(getString(R.string.copying, "0 MB"), false);
        new Thread(() -> {
            try {
                GameDataInstaller.Result result = GameDataInstaller.copy(
                        this, tree, gameRoot, (files, bytes) ->
                                ui.post(() -> overlay.setText(
                                        getString(R.string.copying,
                                                GameDataInstaller.human(bytes)))));
                ui.post(() -> {
                    copying = false;
                    overlay.setVisibility(TextView.GONE);
                    Toast.makeText(this,
                            getString(R.string.copy_done, GameDataInstaller.human(result.bytes)),
                            Toast.LENGTH_LONG).show();
                    Log.i(TAG, "installed " + result.files + " files, " + result.bytes + " bytes");
                    nativeGameDataReady(new File(gameRoot, "GameData").getAbsolutePath());
                });
            } catch (Exception e) {
                Log.e(TAG, "install failed", e);
                ui.post(() -> {
                    copying = false;
                    String message = e.getMessage() == null ? e.toString() : e.getMessage();
                    showMessage(getString(R.string.copy_failed, message), true);
                    nativeInstallFailed(message);
                });
            }
        }, "korra-install").start();
    }

    // Implemented in android/app/src/main/cpp/install_android.c.
    private native void nativeGameDataReady(String gameDataPath);

    private native void nativeInstallFailed(String reason);
}
