package io.github.gt2pc.quest;
import android.app.NativeActivity;
import android.os.Bundle;
import java.io.File;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;

public final class GT2Activity extends NativeActivity {
    private RandomAccessFile saveGuard;
    private FileLock saveLock;
    @Override public void onCreate(Bundle state) {
        try {
            saveGuard = new RandomAccessFile(new File(getFilesDir(), "save-transfer.lock"), "rw");
            saveLock = saveGuard.getChannel().tryLock();
            if (saveLock == null) throw new IllegalStateException("A save transfer is in progress. Try again when it finishes.");
        } catch (Exception e) { throw new IllegalStateException("Cannot lock game saves", e); }
        super.onCreate(state);
    }
    @Override public void onDestroy() {
        super.onDestroy();
        try { if (saveLock != null) saveLock.release(); if (saveGuard != null) saveGuard.close(); }
        catch (Exception ignored) { }
    }
}
