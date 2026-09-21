package io.github.gt2pc.quest;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import java.io.File;
import java.io.IOException;
import java.io.FileInputStream;
public final class ImportAccessReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        setResultCode(1);
        setResultData("GT2_DATA_DIRECTORY=FAIL");
        boolean verify = "io.github.gt2pc.quest.VERIFY_DATA".equals(intent.getAction());
        if (!verify && !"io.github.gt2pc.quest.PREPARE_DATA".equals(intent.getAction())) return;
        try {
            File external = context.getExternalFilesDir(null);
            if (external == null) return;
            File root = external.getCanonicalFile();
            String paths = intent.getStringExtra("paths");
            if (paths != null) {
                if (paths.length() > 8192) return;
                for (String path : paths.split(",")) {
                    if (!path.matches("(arcade|simulation)(/[A-Za-z0-9_.-]+)*") && !(verify && (path.equals("startup.gtm") || path.equals("startup-hd.gtm")))) return;
                    File directory = new File(root, path).getCanonicalFile();
                    if (!directory.getPath().startsWith(root.getPath() + File.separator) || !directory.equals(new File(root, path).getAbsoluteFile())) return;
                    if (verify) {
                        if (!directory.isFile()) return;
                        try (FileInputStream stream = new FileInputStream(directory)) { if (stream.read() < 0) return; }
                    } else if (!directory.isDirectory() && !directory.mkdirs()) return;
                }
            }
            setResultCode(0);
            setResultData(root.getAbsolutePath());
        } catch (IOException | SecurityException failure) { setResultData("GT2_DATA_DIRECTORY=FAIL"); }
    }
}
