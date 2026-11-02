#include "window.h"
#include "steamsetupdialog.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QStyleFactory>
#include <cwchar>
#include <qt_windows.h>
#include <dbghelp.h>

static_assert(sizeof(void *) == 8, "WorkshopDL++ requires an x64 build.");

namespace {
wchar_t crashFolder[MAX_PATH * 4];

LONG WINAPI writeCrashDump(EXCEPTION_POINTERS *exception) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t path[MAX_PATH * 4 + 64];
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\crash-%04d%02d%02d-%02d%02d%02d.dmp", crashFolder,
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION details{GetCurrentThreadId(), exception, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &details, nullptr,
                          nullptr);
        CloseHandle(file);
    }
    MessageBoxW(nullptr,
                L"WorkshopDL++ stopped unexpectedly. A crash report was saved in the data\\crashes folder.",
                L"WorkshopDL++", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashHandler() {
    const QString folder = dataRoot() + QStringLiteral("/crashes");
    if (!QDir().mkpath(folder))
        return;
    const QString native = QDir::toNativeSeparators(folder);
    if (native.size() >= int(sizeof(crashFolder) / sizeof(crashFolder[0])))
        return;
    crashFolder[native.toWCharArray(crashFolder)] = L'\0';
    SetUnhandledExceptionFilter(writeCrashDump);
}

bool activateRunningInstance() {
    HWND target = nullptr;
    EnumWindows(
        [](HWND candidate, LPARAM result) -> BOOL {
            wchar_t title[32]{}, type[64]{};
            GetWindowTextW(candidate, title, 32);
            GetClassNameW(candidate, type, 64);
            if (std::wcscmp(title, L"WorkshopDL++") != 0 || std::wcsncmp(type, L"Qt", 2) != 0 ||
                !std::wcsstr(type, L"QWindow"))
                return TRUE;
            *reinterpret_cast<HWND *>(result) = candidate;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&target));
    if (!target)
        return false;
    if (IsIconic(target))
        ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
    return true;
}
}

int main(int argc, char **argv) {

    QApplication app(argc, argv);
    installCrashHandler();
    const QColor windowsAccent = app.palette().color(QPalette::Accent);
    QSettings appearance(dataRoot() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    const QString style =
        appearance.value(QStringLiteral("appearance/style"), QStringLiteral("Fusion")).toString();
    if (!QApplication::setStyle(style))
        QApplication::setStyle(QStringLiteral("Fusion"));
    const QString colors =
        appearance.value(QStringLiteral("appearance/colors"), QStringLiteral("dark")).toString();
    const auto scheme = colors == QStringLiteral("dark")    ? Qt::ColorScheme::Dark
                        : colors == QStringLiteral("light") ? Qt::ColorScheme::Light
                                                            : Qt::ColorScheme::Unknown;
    app.styleHints()->setColorScheme(scheme);
    QPalette palette = app.palette();
    if (scheme == Qt::ColorScheme::Dark) {
        palette.setColor(QPalette::Window, QColor(45, 45, 45));
        palette.setColor(QPalette::WindowText, QColor(225, 225, 225));
        palette.setColor(QPalette::Base, QColor(30, 30, 30));
        palette.setColor(QPalette::AlternateBase, QColor(38, 38, 38));
        palette.setColor(QPalette::Text, QColor(225, 225, 225));
        palette.setColor(QPalette::Button, QColor(55, 55, 55));
        palette.setColor(QPalette::ButtonText, QColor(225, 225, 225));
        palette.setColor(QPalette::ToolTipBase, QColor(55, 55, 55));
        palette.setColor(QPalette::ToolTipText, QColor(225, 225, 225));
        palette.setColor(QPalette::Highlight, QColor(42, 116, 170));
        palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        palette.setColor(QPalette::Accent, QColor(90, 170, 225));
        palette.setColor(QPalette::Link, QColor(90, 170, 225));
        palette.setColor(QPalette::PlaceholderText, QColor(155, 155, 155));
        for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, QColor(125, 125, 125));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(65, 65, 65));
    } else if (scheme == Qt::ColorScheme::Light) {
        const std::unique_ptr<QStyle> lightStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        palette = lightStyle->standardPalette();
    }
    QColor accent(appearance.value(QStringLiteral("appearance/accent")).toString());
    if (!accent.isValid())
        accent = windowsAccent;
    if (accent.isValid()) {
        palette.setColor(QPalette::Highlight, accent);
        palette.setColor(QPalette::Accent, accent);
        palette.setColor(QPalette::HighlightedText, accent.lightnessF() > 0.6 ? Qt::black : Qt::white);
    }
    if (scheme != Qt::ColorScheme::Unknown || accent.isValid())
        app.setPalette(palette);
    app.setOrganizationName(QStringLiteral("WorkshopDL"));
    app.setApplicationName(QStringLiteral("WorkshopDL++"));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/branding/workshopdl.png")));
    QTemporaryFile storageProbe(dataRoot() + QStringLiteral("/.write-check-XXXXXX"));
    if (!storageProbe.open()) {
        QMessageBox::critical(nullptr, QStringLiteral("Application folder is read-only"),
                              QStringLiteral("Move the complete WorkshopDL++ folder to a writable location. "
                                             "The application stores its data beside the executable."));
        return 1;
    }
    storageProbe.close();
    storageProbe.remove();
    QLockFile lock(dataRoot() + QStringLiteral("/app.lock"));
    QString links;
    for (const auto &argument : app.arguments().mid(1))
        links += argument + QLatin1Char('\n');
    const QString serverName =
        QStringLiteral("WorkshopDL++-") +
        QString::fromLatin1(QCryptographicHash::hash(dataRoot().toUtf8(), QCryptographicHash::Sha256).toHex());
    if (!lock.tryLock(100)) {
        if (!links.isEmpty()) {
            QLocalSocket socket;
            socket.connectToServer(serverName);
            if (socket.waitForConnected(1000)) {
                socket.write(links.toUtf8());
                socket.waitForBytesWritten(1000);
                socket.disconnectFromServer();
            }
        }
        if (!activateRunningInstance())
            QMessageBox::information(nullptr, QStringLiteral("WorkshopDL++"),
                                     QStringLiteral("WorkshopDL++ is already running."));
        return 0;
    }
    ItemModel queue(nullptr, 10000), library;
    Engine engine(&queue, &library);
    std::unique_ptr<Window> window;
    std::unique_ptr<SteamSetupDialog> setup;
    QLocalServer linkServer;
    linkServer.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(serverName);
    linkServer.listen(serverName);
    QObject::connect(&linkServer, &QLocalServer::newConnection, &app, [&] {
        auto *socket = linkServer.nextPendingConnection();
        QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, &window, &links] {
            const QString text = QString::fromUtf8(socket->read(4 * 1024 * 1024));
            if (window)
                window->queueLinks(text);
            else
                links += text;
        });
    });
    const auto showMainWindow = [&] {
        setup.reset();
        window = std::make_unique<Window>(queue, library, engine);
        window->show();
        if (!parseIds(links).isEmpty())
            window->queueLinks(links);
        links.clear();
        app.setQuitOnLastWindowClosed(true);
    };
    if (engine.needsSteamcmdSetup()) {
        app.setQuitOnLastWindowClosed(false);
        setup = std::make_unique<SteamSetupDialog>(engine);
        QObject::connect(setup.get(), &QDialog::finished, &app,
                         [&] { QTimer::singleShot(0, &app, showMainWindow); });
        setup->open();
        QTimer::singleShot(0, &engine, &Engine::prepareSteamcmd);
    } else {
        showMainWindow();
    }
    return app.exec();
}
