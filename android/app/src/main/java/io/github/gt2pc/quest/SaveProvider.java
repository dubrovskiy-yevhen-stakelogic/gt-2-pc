package io.github.gt2pc.quest;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;

/** USB-debugging save exchange. Game data and preferences are never exposed. */
public final class SaveProvider extends ContentProvider {
    private static void authorize() {
        int uid = Binder.getCallingUid();
        if (uid != 0 && uid != 2000 && uid != android.os.Process.myUid())
            throw new SecurityException("Only the USB debugging shell may transfer saves");
    }
    private File card(String mode) {
        if (!"arcade".equals(mode) && !"simulation".equals(mode)) throw new IllegalArgumentException("Invalid disc");
        return new File(getContext().getFilesDir(), "saves/" + mode + "/card1.mcd");
    }
    private File stage(String token) {
        if (token == null || !token.matches("[a-f0-9]{32}")) throw new IllegalArgumentException("Invalid transfer token");
        File dir = new File(getContext().getCacheDir(), "save-transfer");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IllegalStateException("Cannot create staging folder");
        return new File(dir, token + ".mcd");
    }
    private static String hash(byte[] data) throws Exception {
        StringBuilder out = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) out.append(String.format("%02x", b & 255));
        return out.toString();
    }
    private static String digest(File file) throws Exception {
        return file.exists() ? hash(Files.readAllBytes(file.toPath())) : "missing";
    }
    private static void validate(byte[] data) {
        if (data.length != 131072 || data[0] != 'M' || data[1] != 'C') throw new IllegalArgumentException("Invalid memory card");
        boolean occupied = false;
        for (int frame = 0; frame < 16; ++frame) {
            int xor = 0;
            for (int i = 0; i < 128; ++i) xor ^= data[frame * 128 + i] & 255;
            if (xor != 0) throw new IllegalArgumentException("Damaged memory card directory");
            if (frame > 0 && data[frame * 128] == 0x51) occupied = true;
        }
        if (!occupied) throw new IllegalArgumentException("Empty memory cards are not imported");
    }
    private RandomAccessFile lockFile() throws Exception {
        return new RandomAccessFile(new File(getContext().getFilesDir(), "save-transfer.lock"), "rw");
    }
    @Override public boolean onCreate() { return true; }
    @Override public ParcelFileDescriptor openFile(Uri uri, String access) throws FileNotFoundException {
        authorize();
        try (RandomAccessFile guard = lockFile(); FileLock lock = guard.getChannel().tryLock()) {
            if (lock == null) throw new IllegalStateException("Close GT2 VR on the headset first");
            java.util.List<String> parts = uri.getPathSegments();
            if (parts.size() != 2) throw new IllegalArgumentException("Invalid save path");
            if (parts.get(0).equals("card") && access.equals("r"))
                return ParcelFileDescriptor.open(card(parts.get(1)), ParcelFileDescriptor.MODE_READ_ONLY);
            if (parts.get(0).equals("stage") && access.equals("w"))
                return ParcelFileDescriptor.open(stage(parts.get(1)), ParcelFileDescriptor.MODE_WRITE_ONLY |
                    ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_TRUNCATE);
            throw new IllegalArgumentException("Unsupported operation");
        } catch (Exception e) { throw new FileNotFoundException(e.getMessage()); }
    }
    @Override public synchronized Bundle call(String method, String arg, Bundle extras) {
        authorize();
        Bundle result = new Bundle();
        try (RandomAccessFile guard = lockFile(); FileLock lock = guard.getChannel().tryLock()) {
            if (lock == null) throw new IllegalStateException("Close GT2 VR on the headset first");
            File target = card(arg);
            if (method.equals("stat")) {
                result.putString("sha256", digest(target));
            } else if (method.equals("commit")) {
                File source = stage(extras.getString("token"));
                if (source.length() != 131072) throw new IllegalArgumentException("Incomplete memory card upload");
                byte[] data = Files.readAllBytes(source.toPath());
                validate(data);
                if (!hash(data).equals(extras.getString("sha256"))) throw new IllegalArgumentException("Upload checksum mismatch");
                if (!digest(target).equals(extras.getString("expected"))) throw new IllegalStateException("Destination changed; repeat transfer");
                Files.createDirectories(target.getParentFile().toPath());
                if (target.exists()) {
                    File backup = new File(target.getParentFile(), "card1.before-transfer-" + extras.getString("token") + ".mcd");
                    Files.copy(target.toPath(), backup.toPath());
                }
                // Atomic replacement stays on the internal filesystem, preserving the old card on failure.
                Files.move(source.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                result.putString("sha256", digest(target));
            } else throw new IllegalArgumentException("Unsupported method");
            result.putString("status", "ok");
        } catch (Exception e) {
            result.putString("status", "error");
            result.putString("message", e.getMessage());
        }
        return result;
    }
    @Override public String getType(Uri uri) { authorize(); return "application/octet-stream"; }
    @Override public Cursor query(Uri uri, String[] p, String s, String[] a, String order) { throw new UnsupportedOperationException(); }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String s, String[] a) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
}
