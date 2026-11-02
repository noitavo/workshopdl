#include "taskbar.h"
#include <QWidget>
#include <qt_windows.h>
#include <shobjidl.h>
#include <wrl.h>

void setTaskbarProgress(QWidget *window, bool active, int percent) {
    if (!window || !window->isVisible())
        return;
    static Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&taskbar))) ||
            FAILED(taskbar->HrInit()))
            taskbar.Reset();
    }
    if (!taskbar)
        return;
    const auto handle = reinterpret_cast<HWND>(window->winId());
    if (!active) {
        taskbar->SetProgressState(handle, TBPF_NOPROGRESS);
    } else if (percent < 0) {
        taskbar->SetProgressState(handle, TBPF_INDETERMINATE);
    } else {
        taskbar->SetProgressState(handle, TBPF_NORMAL);
        taskbar->SetProgressValue(handle, ULONGLONG(percent), 100);
    }
}
