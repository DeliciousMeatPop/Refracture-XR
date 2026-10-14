#pragma once
// Where to find a monospace TrueType font on Linux: fontconfig's pick first, then common files. Shared by the PC
// viewer's overlay and the host bridge's in-headset performance panel.
#include <cstdio>
#include <string>
#include <vector>

// semibold: the heavier face (as Consolas Semibold on Windows), for text a headset shows small; then the regular one.
inline std::vector<std::string> mono_font_candidates(bool semibold = false)
{
    std::vector<std::string> paths;
    const auto match = [&](const char* command) {
        if (FILE* pipe = popen(command, "r")) {
            char path[4096]{};
            if (std::fgets(path, sizeof(path), pipe)) paths.emplace_back(path);
            pclose(pipe);
        }
    };
    if (semibold) {
        match("fc-match -f '%{file}' monospace:weight=demibold 2>/dev/null");
        for (const char* path : {"/usr/share/fonts/google-noto/NotoSansMono-SemiBold.ttf",
                                 "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Bold.ttf",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
                                 "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
                                 "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Bold.ttf",
                                 "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf"})
            paths.emplace_back(path);
    }
    match("fc-match -f '%{file}' monospace 2>/dev/null");
    for (const char* path : {"/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
                             "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                             "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
                             "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"})
        paths.emplace_back(path);
    return paths;
}
