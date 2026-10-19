package android.media;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.net.Uri;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.Map;

/** No media demuxing yet (see MediaPlayer): every data source fails to open with an IOException. */
public final class MediaExtractor {
    public MediaExtractor() {}

    public void setDataSource(String path) throws IOException {
        throw MediaPlayer.unsupported("MediaExtractor.setDataSource(" + path + ")");
    }
    public void setDataSource(String path, Map<String, String> headers) throws IOException {
        throw MediaPlayer.unsupported("MediaExtractor.setDataSource(" + path + ")");
    }
    public void setDataSource(Context context, Uri uri, Map<String, String> headers) throws IOException {
        throw MediaPlayer.unsupported("MediaExtractor.setDataSource(" + uri + ")");
    }
    public void setDataSource(FileDescriptor fd) throws IOException {
        throw MediaPlayer.unsupported("MediaExtractor.setDataSource(fd)");
    }
    public void setDataSource(FileDescriptor fd, long offset, long length) throws IOException {
        throw MediaPlayer.unsupported("MediaExtractor.setDataSource(fd, " + offset + ", " + length + ")");
    }
    public void setDataSource(AssetFileDescriptor afd) throws IOException {
        throw MediaPlayer.unsupported("MediaExtractor.setDataSource(asset)");
    }
    public void setDataSource(MediaDataSource source) throws IOException {
        throw MediaPlayer.unsupported("MediaExtractor.setDataSource(source)");
    }
    public int getTrackCount() { return 0; }
    public void release() {}
}
