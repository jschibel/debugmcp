// Late-loaded DLL used to test deferred (pending) breakpoints.
#include <windows.h>

static int Helper(int v) {
    int doubled = v * 2;
    return doubled + 1;  // LINE_PLUGIN_HELPER
}

extern "C" __declspec(dllexport) int PluginCompute(int v) {
    int r = Helper(v);
    return r;
}
