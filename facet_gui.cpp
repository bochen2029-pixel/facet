// facet · facet_gui.cpp — the window (milestone 2). Placeholder until the facet rail lands.
#include "app_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace facet {

int run_gui(const Opts&) {
    MessageBoxW(nullptr, L"facet: the window is the next milestone — use the console modes for now.",
                L"facet", MB_OK | MB_ICONINFORMATION);
    return 0;
}

}  // namespace facet
