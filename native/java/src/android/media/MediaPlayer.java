package android.media;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.net.Uri;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.Map;
import refract.Runtime;

/**
 * Refract Native has no video decoding yet: every data source fails to open, as an unsupported file does on
 * Android, so games skip the video (UE4's MediaPlayer14 reports the open as failed) instead of waiting on a
 * player that never prepares. The other methods stay the stub's.
 */
public class MediaPlayer {
    private static boolean logged;

    public MediaPlayer() {}

    static IOException unsupported(String what) {
        synchronized (MediaPlayer.class) {
            if (!logged) Runtime.log(5, "refract", "video playback is not supported yet; failing " + what);
            logged = true;
        }
        return new IOException("Refract Native: video playback is not supported");
    }

    public void setDataSource(String path) throws IOException {
        throw unsupported("MediaPlayer.setDataSource(" + path + ")");
    }
    public void setDataSource(Context context, Uri uri) throws IOException {
        throw unsupported("MediaPlayer.setDataSource(" + uri + ")");
    }
    public void setDataSource(Context context, Uri uri, Map<String, String> headers) throws IOException {
        throw unsupported("MediaPlayer.setDataSource(" + uri + ")");
    }
    public void setDataSource(FileDescriptor fd) throws IOException {
        throw unsupported("MediaPlayer.setDataSource(fd)");
    }
    public void setDataSource(FileDescriptor fd, long offset, long length) throws IOException {
        throw unsupported("MediaPlayer.setDataSource(fd, " + offset + ", " + length + ")");
    }
    public void setDataSource(AssetFileDescriptor afd) throws IOException {
        throw unsupported("MediaPlayer.setDataSource(asset)");
    }
    public void setDataSource(MediaDataSource source) {
        throw new IllegalArgumentException("Refract Native: video playback is not supported");
    }
}
