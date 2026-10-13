#pragma once
// Where to find a monospace TrueType font on Linux: fontconfig's pick first, then common files. Shared by the PC
// viewer's overlay and the host bridge's in-headset performance panel.
#include <cstdio>
#include <string>
#include <vector>

inline std::vector<std::string> mono_font_candidates()
{
    std::vector<std::string> paths;
    if (FILE* pipe = popen("fc-match -f '%{file}' monospace 2>/dev/null", "r")) {
        char path[4096]{};
        if (std::fgets(path, sizeof(path), pipe)) paths.emplace_back(path);
        pclose(pipe);
    }
    for (const char* path : {"/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
                             "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                             "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
                             "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"})
        paths.emplace_back(path);
    return paths;
}
