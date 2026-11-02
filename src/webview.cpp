#include "webview.h"
#include "storage.h"
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
struct BrowserView::Impl {
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> core;
    QUrl address;
    bool initializing = false, loading = false, fit = true;
    double scale = 1;
    bool ole = false;
    UINT64 navigation = 0;
    QString failure;
};
namespace {
QKeySequence applicationShortcut(UINT key) {
    if (GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0)
        return {};
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (shift)
        return key == 'O'   ? QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)
               : key == 'V' ? QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V)
                            : QKeySequence();
    if (key >= '1' && key <= '3')
        return QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + int(key - '1')));
    return key == 'L'             ? QKeySequence(Qt::CTRL | Qt::Key_L)
           : key == 'D'           ? QKeySequence(Qt::CTRL | Qt::Key_D)
           : key == VK_OEM_COMMA  ? QKeySequence(Qt::CTRL | Qt::Key_Comma)
                                  : QKeySequence();
}
}
BrowserView::BrowserView(QWidget *parent) : QWidget(parent), d(std::make_unique<Impl>()) {
    d->ole = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    setAttribute(Qt::WA_NativeWindow);
    setFocusPolicy(Qt::StrongFocus);
}
BrowserView::~BrowserView() {
    if (d->controller)
        d->controller->Close();
    d->core.Reset();
    d->controller.Reset();
    if (d->ole)
        CoUninitialize();
}
QUrl BrowserView::url() const {
    return d->address;
}
bool BrowserView::canGoBack() const {
    BOOL b = FALSE;
    if (d->core)
        d->core->get_CanGoBack(&b);
    return b;
}
bool BrowserView::canGoForward() const {
    BOOL b = FALSE;
    if (d->core)
        d->core->get_CanGoForward(&b);
    return b;
}
bool BrowserView::isLoading() const {
    return d->loading;
}
double BrowserView::zoom() const {
    return d->scale;
}
void BrowserView::setUrl(const QUrl &url) {
    d->address = url;
    if (d->core)
        d->core->Navigate(url.toString().toStdWString().c_str());
}
void BrowserView::goBack() {
    if (d->core)
        d->core->GoBack();
}
void BrowserView::goForward() {
    if (d->core)
        d->core->GoForward();
}
void BrowserView::reload() {
    if (d->core)
        d->core->Reload();
    else if (!d->initializing)
        initialize();
}
void BrowserView::stop() {
    if (d->core)
        d->core->Stop();
}
void BrowserView::deleteAllCookies() {
    ComPtr<ICoreWebView2_2> v;
    if (d->core && SUCCEEDED(d->core.As(&v))) {
        ComPtr<ICoreWebView2CookieManager> cookies;
        v->get_CookieManager(&cookies);
        if (cookies)
            cookies->DeleteAllCookies();
    }
}
void BrowserView::setFitWidth(bool on) {
    d->fit = on;
    bounds();
}
void BrowserView::setZoom(double value) {
    d->scale = qBound(0.5, value, 2.0);
    if (d->controller)
        d->controller->put_ZoomFactor(d->scale);
    emit zoomChanged(d->scale);
}
void BrowserView::bounds() {
    if (!d->controller)
        return;
    RECT rect{};
    GetClientRect(reinterpret_cast<HWND>(winId()), &rect);
    d->controller->put_Bounds(rect);
    d->controller->NotifyParentWindowPositionChanged();
    if (d->fit) {
        const auto host = d->address.host();
        const bool steam =
            host == QStringLiteral("steamcommunity.com") || host == QStringLiteral("www.steamcommunity.com");
        setZoom(steam ? qBound(0.65, double(width()) / 1050.0, 1.0) : 1.0);
    }
}
void BrowserView::returnFocus() {
    SetFocus(reinterpret_cast<HWND>(window()->winId()));
}
void BrowserView::initializationFailed(const QString &message) {
    d->initializing = false;
    d->failure = message;
    update();
    emit loadingChanged({BrowserLoadInfo::LoadStatus::Failed, url(), message});
    LPWSTR version = nullptr;
    const HRESULT available = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool installed = SUCCEEDED(available) && version;
    CoTaskMemFree(version);
    if (!installed)
        emit runtimeUnavailable();
}
bool BrowserView::eventFilter(QObject *watched, QEvent *event) {
    if (watched == window() && event->type() == QEvent::Move && d->controller)
        d->controller->NotifyParentWindowPositionChanged();
    return QWidget::eventFilter(watched, event);
}
void BrowserView::paintEvent(QPaintEvent *) {
    if (d->failure.isEmpty())
        return;
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, d->failure);
}
void BrowserView::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    window()->installEventFilter(this);
    if (!d->initializing)
        initialize();
    if (d->controller)
        d->controller->put_IsVisible(TRUE);
    bounds();
}
void BrowserView::hideEvent(QHideEvent *e) {
    if (d->controller)
        d->controller->put_IsVisible(FALSE);
    QWidget::hideEvent(e);
}
void BrowserView::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    bounds();
}
void BrowserView::focusInEvent(QFocusEvent *event) {
    QWidget::focusInEvent(event);
    if (d->controller) {
        const auto reason =
            event->reason() == Qt::BacktabFocusReason ? COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS
            : event->reason() == Qt::TabFocusReason   ? COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT
                                                      : COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC;
        d->controller->MoveFocus(reason);
    }
}
void BrowserView::initialize() {
    d->initializing = true;
    d->failure.clear();
    QPointer<BrowserView> self(this);
    const auto profile = QDir::toNativeSeparators(dataRoot() + QStringLiteral("/WebView2")).toStdWString();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([self](HRESULT result,
                                                                                    ICoreWebView2Environment
                                                                                        *env) -> HRESULT {
            if (!self)
                return S_OK;
            if (FAILED(result) || !env) {
                self->initializationFailed(QStringLiteral("The WebView2 Runtime could not start."));
                return S_OK;
            }
            const HRESULT created = env->CreateCoreWebView2Controller(
                reinterpret_cast<HWND>(self->winId()),
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [self](HRESULT error, ICoreWebView2Controller *controller) -> HRESULT {
                        if (!self) {
                            if (controller)
                                controller->Close();
                            return S_OK;
                        }
                        if (FAILED(error) || !controller) {
                            self->initializationFailed(QStringLiteral("The embedded browser could not start."));
                            return S_OK;
                        }
                        auto &d = *self->d;
                        d.controller = controller;
                        controller->get_CoreWebView2(&d.core);
                        if (!d.core) {
                            controller->Close();
                            d.controller.Reset();
                            self->initializationFailed(QStringLiteral("The embedded browser could not start."));
                            return S_OK;
                        }
                        ComPtr<ICoreWebView2Settings> webSettings;
                        if (SUCCEEDED(d.core->get_Settings(&webSettings)) && webSettings)
                            webSettings->put_AreDevToolsEnabled(FALSE);
                        EventRegistrationToken token;
                        controller->add_AcceleratorKeyPressed(
                            Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                                [self](ICoreWebView2Controller *,
                                       ICoreWebView2AcceleratorKeyPressedEventArgs *args) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    COREWEBVIEW2_KEY_EVENT_KIND kind;
                                    UINT key = 0;
                                    args->get_KeyEventKind(&kind);
                                    args->get_VirtualKey(&key);
                                    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN)
                                        return S_OK;
                                    const QKeySequence keys = applicationShortcut(key);
                                    if (keys.isEmpty())
                                        return S_OK;
                                    args->put_Handled(TRUE);
                                    emit self->acceleratorPressed(keys);
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        d.core->add_ProcessFailed(
                            Callback<ICoreWebView2ProcessFailedEventHandler>(
                                [self](ICoreWebView2 *,
                                       ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                                    args->get_ProcessFailedKind(&kind);
                                    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
                                        return S_OK;
                                    const bool restart =
                                        kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                    QTimer::singleShot(0, self.data(), [self, restart] {
                                        if (!self)
                                            return;
                                        if (restart) {
                                            if (self->d->controller)
                                                self->d->controller->Close();
                                            self->d->core.Reset();
                                            self->d->controller.Reset();
                                            self->d->initializing = false;
                                            self->d->loading = false;
                                        }
                                        self->reload();
                                    });
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        controller->add_MoveFocusRequested(
                            Callback<ICoreWebView2MoveFocusRequestedEventHandler>(
                                [self](ICoreWebView2Controller *,
                                       ICoreWebView2MoveFocusRequestedEventArgs *args) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    COREWEBVIEW2_MOVE_FOCUS_REASON reason;
                                    args->get_Reason(&reason);
                                    args->put_Handled(self->focusNextPrevChild(
                                        reason != COREWEBVIEW2_MOVE_FOCUS_REASON_PREVIOUS));
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        d.core->add_NavigationStarting(
                            Callback<ICoreWebView2NavigationStartingEventHandler>(
                                [self](ICoreWebView2 *,
                                       ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    LPWSTR value = nullptr;
                                    args->get_Uri(&value);
                                    const QUrl url(QString::fromWCharArray(value ? value : L""));
                                    CoTaskMemFree(value);
                                    if (url.scheme() != QStringLiteral("https") &&
                                        url.scheme() != QStringLiteral("http") &&
                                        url != QUrl(QStringLiteral("about:blank"))) {
                                        args->put_Cancel(TRUE);
                                        return S_OK;
                                    }
                                    args->get_NavigationId(&self->d->navigation);
                                    self->d->address = url;
                                    self->d->loading = true;
                                    self->bounds();
                                    emit self->urlChanged(url);
                                    emit self->loadingChanged(
                                        {BrowserLoadInfo::LoadStatus::Started, url, {}});
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        d.core->add_NavigationCompleted(
                            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [self](ICoreWebView2 *,
                                       ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    UINT64 navigation = 0;
                                    args->get_NavigationId(&navigation);
                                    if (navigation != self->d->navigation)
                                        return S_OK;
                                    BOOL success;
                                    args->get_IsSuccess(&success);
                                    COREWEBVIEW2_WEB_ERROR_STATUS status =
                                        COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                    args->get_WebErrorStatus(&status);
                                    if (status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
                                        success = TRUE;
                                    self->d->loading = false;
                                    emit self->loadingChanged(
                                        {success ? BrowserLoadInfo::LoadStatus::Succeeded
                                                 : BrowserLoadInfo::LoadStatus::Failed,
                                         self->url(),
                                         success
                                             ? QString()
                                             : QStringLiteral(
                                                   "Navigation failed. Check the connection and reload.")});
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        d.core->add_SourceChanged(
                            Callback<ICoreWebView2SourceChangedEventHandler>(
                                [self](ICoreWebView2 *core,
                                       ICoreWebView2SourceChangedEventArgs *) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    LPWSTR value = nullptr;
                                    core->get_Source(&value);
                                    self->d->address = QUrl(QString::fromWCharArray(value ? value : L""));
                                    CoTaskMemFree(value);
                                    emit self->urlChanged(self->url());
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        d.core->add_DocumentTitleChanged(
                            Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                [self](ICoreWebView2 *core, IUnknown *) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    LPWSTR value = nullptr;
                                    core->get_DocumentTitle(&value);
                                    emit self->titleChanged(QString::fromWCharArray(value ? value : L""));
                                    CoTaskMemFree(value);
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        d.core->add_HistoryChanged(
                            Callback<ICoreWebView2HistoryChangedEventHandler>([self](ICoreWebView2 *,
                                                                                     IUnknown *) -> HRESULT {
                                if (self)
                                    emit self->urlChanged(self->url());
                                return S_OK;
                            }).Get(),
                            &token);
                        d.core->add_NewWindowRequested(
                            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                [self](ICoreWebView2 *,
                                       ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                                    if (!self)
                                        return S_OK;
                                    LPWSTR value = nullptr;
                                    args->get_Uri(&value);
                                    const QUrl url(QString::fromWCharArray(value ? value : L""));
                                    CoTaskMemFree(value);
                                    args->put_Handled(TRUE);
                                    if (url.scheme() == QStringLiteral("https") ||
                                        url.scheme() == QStringLiteral("http"))
                                        self->setUrl(url);
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        d.controller->add_ZoomFactorChanged(
                            Callback<ICoreWebView2ZoomFactorChangedEventHandler>(
                                [self](ICoreWebView2Controller *c, IUnknown *) -> HRESULT {
                                    if (self) {
                                        double scale;
                                        c->get_ZoomFactor(&scale);
                                        if (qAbs(self->d->scale - scale) > 0.005 && self->d->fit) {
                                            self->d->fit = false;
                                            emit self->fitWidthChanged(false);
                                        }
                                        self->d->scale = scale;
                                        emit self->zoomChanged(scale);
                                    }
                                    return S_OK;
                                })
                                .Get(),
                            &token);
                        controller->put_IsVisible(self->isVisible());
                        self->setZoom(d.scale);
                        self->bounds();
                        if (!self->url().isEmpty())
                            self->setUrl(self->url());
                        return S_OK;
                    })
                    .Get());
            if (FAILED(created))
                self->initializationFailed(QStringLiteral("The embedded browser could not start."));
            return S_OK;
        }).Get());
    if (FAILED(hr))
        initializationFailed(QStringLiteral(
            "The Browse tab requires the Microsoft Edge WebView2 Runtime. Downloads from the Downloads tab "
            "still work."));
}
