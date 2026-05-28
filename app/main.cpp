#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QQuickStyle>
#include <QMutex>
#include <QtDebug>
#include <QNetworkProxyFactory>
#include <QPalette>
#include <QFont>
#include <QCursor>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextStream>
#include <QRegularExpression>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <signal.h>
#endif

// Don't let SDL hook our main function, since Qt is already
// doing the same thing. This needs to be before any headers
// that might include SDL.h themselves.
#define SDL_MAIN_HANDLED
#include "SDL_compat.h"

#ifdef HAVE_FFMPEG
#include "streaming/video/ffmpeg.h"
#endif

#if defined(Q_OS_WIN32)
#include "antihookingprotection.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dxgi1_6.h>
#elif defined(Q_OS_LINUX)
#include <openssl/ssl.h>
#include <unistd.h>
#endif

#include "cli/listapps.h"
#include "cli/quitstream.h"
#include "cli/startstream.h"
#include "cli/pair.h"
#include "cli/commandlineparser.h"
#include "path.h"
#include "utils.h"
#include "gui/computermodel.h"
#include "gui/appmodel.h"
#include "backend/autoupdatechecker.h"
#include "backend/computermanager.h"
#include "backend/systemproperties.h"
#include "streaming/session.h"
#include "streaming/usb/usbpassthroughmanager.h"
#include "settings/streamingpreferences.h"
#include "gui/sdlgamepadkeynavigation.h"

#if defined(Q_OS_WIN32)
#define IS_UNSPECIFIED_HANDLE(x) ((x) == INVALID_HANDLE_VALUE || (x) == NULL)

// Log to file or console dynamically for Windows builds
#define LOG_TO_FILE
#elif !defined(QT_DEBUG) && defined(Q_OS_DARWIN)
// Log to file for release Mac builds
#define LOG_TO_FILE
#else
// Log to console for debug Mac builds
#endif

// StreamUtils::setAsyncLogging() exposes control of this to the Session
// class to enable async logging once the stream has started.
//
// FIXME: Clean this up
QAtomicInt g_AsyncLoggingEnabled;

static QElapsedTimer s_LoggerTime;
static QTextStream s_LoggerStream(stderr);
static QThreadPool s_LoggerThread;
static QMutex s_SyncLoggerMutex;
static bool s_SuppressVerboseOutput;
static QRegularExpression k_RikeyRegex("&rikey=\\w+");
static QRegularExpression k_RikeyIdRegex("&rikeyid=[\\d-]+");
static constexpr quint16 k_UsbLabDefaultExporterPort = 3240;
#ifdef LOG_TO_FILE
// Max log file size of 10 MB
static const uint64_t k_MaxLogSizeBytes = 10 * 1024 * 1024;
static QAtomicInteger<uint64_t> s_LogBytesWritten = 0;
static QFile* s_LoggerFile;
#endif

#ifdef HAVE_DRM_MASTER_HOOKS
extern "C" bool g_DisableDrmHooks;
#endif

class LoggerTask : public QRunnable
{
public:
    LoggerTask(const QString& msg) : m_Msg(msg)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        // QTextStream is not thread-safe, so we must lock. This will generally
        // only contend in synchronous logging mode or during a transition
        // between synchronous and asynchronous. Asynchronous won't contend in
        // the common case because we only have a single logging thread.
        QMutexLocker locker(&s_SyncLoggerMutex);
        s_LoggerStream << m_Msg;
        s_LoggerStream.flush();
    }

private:
    QString m_Msg;
};

void logToLoggerStream(QString& message)
{
#if defined(QT_DEBUG) && defined(Q_OS_WIN32)
    // Output log messages to a debugger if attached
    if (IsDebuggerPresent()) {
        thread_local QString lineBuffer;
        lineBuffer += message;
        if (message.endsWith('\n')) {
            OutputDebugStringW(lineBuffer.toStdWString().c_str());
            lineBuffer.clear();
        }
    }
#endif

    // Strip session encryption keys and IVs from the logs
    message.replace(k_RikeyRegex, "&rikey=REDACTED");
    message.replace(k_RikeyIdRegex, "&rikeyid=REDACTED");

#ifdef LOG_TO_FILE
    auto oldLogSize = s_LogBytesWritten.fetchAndAddRelaxed(message.size());
    if (oldLogSize >= k_MaxLogSizeBytes) {
        return;
    }
    else if (oldLogSize >= k_MaxLogSizeBytes - message.size()) {
        // Write one final message
        message = "Log size limit reached!";
    }
#endif

    if (g_AsyncLoggingEnabled) {
        // Queue the log message to be written asynchronously
        s_LoggerThread.start(new LoggerTask(message));
    }
    else {
        // Log the message immediately
        LoggerTask(message).run();
    }
}

static std::atomic_bool s_UsbLabStopRequested {false};

static void usbLabSignalHandler(int)
{
    s_UsbLabStopRequested.store(true, std::memory_order_release);
}

static void waitForUsbLabStop(int holdSeconds)
{
    s_UsbLabStopRequested.store(false, std::memory_order_release);
    auto previousSigint = std::signal(SIGINT, usbLabSignalHandler);
    auto previousSigterm = std::signal(SIGTERM, usbLabSignalHandler);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(holdSeconds >= 0 ? holdSeconds : 0);
    if (holdSeconds < 0) {
        std::thread inputThread([]() {
            std::string line;
            std::getline(std::cin, line);
            s_UsbLabStopRequested.store(true, std::memory_order_release);
        });
        inputThread.detach();
    }

    while (!s_UsbLabStopRequested.load(std::memory_order_acquire) &&
           (holdSeconds < 0 || std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::signal(SIGINT, previousSigint);
    std::signal(SIGTERM, previousSigterm);
}

static std::string qtToStdString(const QString& value)
{
    return value.toLocal8Bit().constData();
}

static bool usbLabDeviceExportable(const UsbPassthroughManager& manager, const QString& busId, bool* found)
{
    if (found) {
        *found = false;
    }

    for (const QVariant& deviceVariant : manager.devices()) {
        const QVariantMap device = deviceVariant.toMap();
        if (device.value(QStringLiteral("busid")).toString() != busId) {
            continue;
        }

        if (found) {
            *found = true;
        }
        const QString state = device.value(QStringLiteral("state")).toString();
        return state == QStringLiteral("bound") || state == QStringLiteral("attached");
    }

    return false;
}

static QString usbLabYesNo(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

static QString usbLabValueOrDash(const QString& value)
{
    return value.isEmpty() ? QStringLiteral("-") : value;
}

static QVariantMap usbLabStatusPayload(const UsbPassthroughManager& manager, bool exporterTested, bool exporterReady)
{
    QVariantMap payload;
    payload[QStringLiteral("supported")] = manager.isSupported();
    payload[QStringLiteral("backend")] = manager.backend();
    payload[QStringLiteral("dependenciesReady")] = manager.dependenciesReady();
    payload[QStringLiteral("statusMessage")] = manager.statusMessage();
    payload[QStringLiteral("lastError")] = manager.lastError();
    payload[QStringLiteral("usbipPath")] = manager.usbipPath();
    payload[QStringLiteral("usbipdPath")] = manager.usbipdPath();
    payload[QStringLiteral("usbipdServiceState")] = manager.usbipdServiceState();
    payload[QStringLiteral("usbipCoreLoaded")] = manager.usbipCoreLoaded();
    payload[QStringLiteral("usbipHostLoaded")] = manager.usbipHostLoaded();
    payload[QStringLiteral("exporterTested")] = exporterTested;
    if (exporterTested) {
        payload[QStringLiteral("exporterReady")] = exporterReady;
    }
    payload[QStringLiteral("devices")] = manager.devices();
    payload[QStringLiteral("deviceErrors")] = manager.deviceErrors();
    return payload;
}

static QString formatUsbLabStatusHuman(const UsbPassthroughManager& manager, bool exporterTested, bool exporterReady)
{
    QString output;
    QTextStream stream(&output);
    stream << "USB passthrough client status\n";
    stream << "Supported: " << usbLabYesNo(manager.isSupported()) << '\n';
    stream << "Backend: " << usbLabValueOrDash(manager.backend()) << '\n';
    stream << "Dependencies ready: " << usbLabYesNo(manager.dependenciesReady()) << '\n';
    stream << "Status: " << usbLabValueOrDash(manager.statusMessage()) << '\n';
    if (!manager.lastError().isEmpty()) {
        stream << "Last error: " << manager.lastError() << '\n';
    }
    if (!manager.usbipPath().isEmpty()) {
        stream << "usbip: " << manager.usbipPath() << '\n';
    }
    if (!manager.usbipdPath().isEmpty()) {
        stream << "usbipd: " << manager.usbipdPath() << '\n';
    }
    if (!manager.usbipdServiceState().isEmpty()) {
        stream << "usbipd service: " << manager.usbipdServiceState() << '\n';
    }
    if (manager.backend() == QStringLiteral("linux-usbip")) {
        stream << "usbip-core module: " << usbLabYesNo(manager.usbipCoreLoaded()) << '\n';
        stream << "usbip-host module: " << usbLabYesNo(manager.usbipHostLoaded()) << '\n';
    }
    if (exporterTested) {
        stream << "Exporter probe: " << (exporterReady ? QStringLiteral("ready") : QStringLiteral("failed")) << '\n';
    }

    const QVariantList devices = manager.devices();
    stream << "Devices: " << devices.size() << '\n';
    const QVariantMap deviceErrors = manager.deviceErrors();
    for (const QVariant& deviceVariant : devices) {
        const QVariantMap device = deviceVariant.toMap();
        const QString busId = device.value(QStringLiteral("busid")).toString();
        const QString vid = device.value(QStringLiteral("vid")).toString();
        const QString pid = device.value(QStringLiteral("pid")).toString();
        const QString vidPid = (!vid.isEmpty() || !pid.isEmpty()) ? vid + QStringLiteral(":") + pid : QStringLiteral("-");
        QString name = device.value(QStringLiteral("product")).toString();
        const QString vendor = device.value(QStringLiteral("vendor")).toString();
        if (!vendor.isEmpty() && !name.isEmpty()) {
            name = vendor + QStringLiteral(" ") + name;
        }
        if (name.isEmpty()) {
            name = device.value(QStringLiteral("description")).toString();
        }
        stream << "  " << usbLabValueOrDash(busId)
               << "  " << usbLabValueOrDash(device.value(QStringLiteral("state")).toString())
               << "  " << vidPid
               << "  " << usbLabValueOrDash(device.value(QStringLiteral("deviceClass")).toString())
               << "  " << usbLabValueOrDash(name)
               << '\n';

        const QString blockedReason = device.value(QStringLiteral("blockReason")).toString();
        if (!blockedReason.isEmpty()) {
            stream << "    blocked: " << blockedReason << '\n';
        }
        const QString error = deviceErrors.value(busId).toString();
        if (!error.isEmpty()) {
            stream << "    error: " << error << '\n';
        }
    }

    return output;
}

static QVariantMap usbLabInstallPreflight(const UsbPassthroughManager& manager)
{
    QVariantMap preflight;
    preflight[QStringLiteral("available")] = false;
    preflight[QStringLiteral("action")] = QString();
    preflight[QStringLiteral("command")] = QString();
    preflight[QStringLiteral("message")] = QString();

    if (!manager.isSupported()) {
        preflight[QStringLiteral("message")] = QStringLiteral("USB passthrough is not supported on this client OS.");
        return preflight;
    }
    if (manager.dependenciesReady()) {
        preflight[QStringLiteral("available")] = true;
        preflight[QStringLiteral("action")] = QStringLiteral("none");
        preflight[QStringLiteral("message")] = QStringLiteral("USB passthrough dependencies are already ready.");
        return preflight;
    }

#ifdef Q_OS_LINUX
    if (manager.backend() == QStringLiteral("linux-usbip")) {
        const QString modprobePath = QStandardPaths::findExecutable(
            QStringLiteral("modprobe"),
            {
                QStringLiteral("/usr/sbin"),
                QStringLiteral("/sbin"),
                QStringLiteral("/usr/bin"),
                QStringLiteral("/bin"),
            });
        const QString pkexecPath = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        const bool runningAsRoot = geteuid() == 0;
        preflight[QStringLiteral("modprobePath")] = modprobePath;
        preflight[QStringLiteral("pkexecPath")] = pkexecPath;
        preflight[QStringLiteral("runningAsRoot")] = runningAsRoot;
        if (modprobePath.isEmpty()) {
            preflight[QStringLiteral("message")] = QStringLiteral("modprobe was not found.");
        }
        else if (runningAsRoot) {
            preflight[QStringLiteral("available")] = true;
            preflight[QStringLiteral("action")] = QStringLiteral("modprobe");
            preflight[QStringLiteral("command")] = modprobePath + QStringLiteral(" usbip-core usbip-host");
            preflight[QStringLiteral("message")] = QStringLiteral("USB/IP kernel modules can be loaded directly.");
        }
        else if (!pkexecPath.isEmpty()) {
            preflight[QStringLiteral("available")] = true;
            preflight[QStringLiteral("action")] = QStringLiteral("pkexec-modprobe");
            preflight[QStringLiteral("command")] = pkexecPath + QStringLiteral(" ") + modprobePath + QStringLiteral(" usbip-core usbip-host");
            preflight[QStringLiteral("message")] = QStringLiteral("USB/IP kernel modules can be loaded through pkexec.");
        }
        else {
            preflight[QStringLiteral("message")] = QStringLiteral("pkexec was not found. Run sudo modprobe usbip-core usbip-host.");
        }
        return preflight;
    }
#endif

#ifdef Q_OS_WIN32
    if (manager.backend() == QStringLiteral("windows-usbipd-win")) {
        const QString wingetPath = QStandardPaths::findExecutable(QStringLiteral("winget"));
        preflight[QStringLiteral("wingetPath")] = wingetPath;
        preflight[QStringLiteral("packageId")] = QStringLiteral("dorssel.usbipd-win");
        if (wingetPath.isEmpty()) {
            preflight[QStringLiteral("message")] = QStringLiteral("winget was not found. Install usbipd-win from the official releases page.");
        }
        else {
            preflight[QStringLiteral("available")] = true;
            preflight[QStringLiteral("action")] = QStringLiteral("winget-usbipd-win");
            preflight[QStringLiteral("command")] = wingetPath + QStringLiteral(" install --interactive --exact --id dorssel.usbipd-win --accept-package-agreements --accept-source-agreements");
            preflight[QStringLiteral("message")] = QStringLiteral("usbipd-win can be installed through winget with administrator approval.");
        }
        return preflight;
    }
#endif

    preflight[QStringLiteral("message")] = QStringLiteral("USB passthrough dependency installation is not supported on this client backend.");
    return preflight;
}

static QString formatUsbLabInstallHuman(const UsbPassthroughManager& manager, const QVariantMap& preflight, bool dryRun, bool installAttempted, bool installStarted)
{
    QString output;
    QTextStream stream(&output);
    stream << "USB passthrough client dependency setup\n";
    stream << "Dry run: " << usbLabYesNo(dryRun) << '\n';
    stream << "Supported: " << usbLabYesNo(manager.isSupported()) << '\n';
    stream << "Backend: " << usbLabValueOrDash(manager.backend()) << '\n';
    stream << "Dependencies ready: " << usbLabYesNo(manager.dependenciesReady()) << '\n';
    stream << "Install action available: " << usbLabYesNo(preflight.value(QStringLiteral("available")).toBool()) << '\n';
    stream << "Install action: " << usbLabValueOrDash(preflight.value(QStringLiteral("action")).toString()) << '\n';
    const QString command = preflight.value(QStringLiteral("command")).toString();
    if (!command.isEmpty()) {
        stream << "Install command: " << command << '\n';
    }
    stream << "Status: " << usbLabValueOrDash(manager.statusMessage()) << '\n';
    if (!manager.lastError().isEmpty()) {
        stream << "Last error: " << manager.lastError() << '\n';
    }
    const QString message = preflight.value(QStringLiteral("message")).toString();
    if (!message.isEmpty()) {
        stream << "Message: " << message << '\n';
    }
    if (installAttempted) {
        stream << "Install attempted: yes\n";
        stream << "Install started: " << usbLabYesNo(installStarted) << '\n';
    }
    return output;
}

static bool writeUsbLabCommandOutput(const QString& output, const QString& outputPath)
{
    const QString normalizedOutput = output.endsWith('\n') ? output : output + '\n';
    if (!outputPath.isEmpty()) {
        QFile file(outputPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            std::cerr << "Failed to write " << qtToStdString(outputPath) << ": "
                      << qtToStdString(file.errorString()) << std::endl;
            return false;
        }

        file.write(normalizedOutput.toUtf8());
        return true;
    }

    fputs(qPrintable(normalizedOutput), stdout);
    fflush(stdout);
    return true;
}

static bool writeUsbLabJsonState(const QVariantMap& payload, bool finalOutput, bool json, const QString& outputPath)
{
    if (outputPath.isEmpty() && !(json && finalOutput)) {
        return true;
    }

    const QString output = QString::fromUtf8(
        QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson(QJsonDocument::Indented));

    if (!outputPath.isEmpty() && !writeUsbLabCommandOutput(output, outputPath)) {
        return false;
    }

    if (json && finalOutput) {
        return writeUsbLabCommandOutput(output, QString());
    }

    return true;
}

static bool writeUsbLabTunnelState(const UsbLabTunnelCommandLineParser& parser, const QVariantMap& payload, bool finalOutput)
{
    return writeUsbLabJsonState(payload, finalOutput, parser.isJson(), parser.getOutputPath());
}

static bool writeUsbLabInstallState(const UsbLabInstallCommandLineParser& parser, const QVariantMap& payload, bool finalOutput)
{
    return writeUsbLabJsonState(payload, finalOutput, parser.isJson(), parser.getOutputPath());
}

static bool writeUsbLabExportState(const UsbLabExportCommandLineParser& parser, const QVariantMap& payload, bool finalOutput)
{
    return writeUsbLabJsonState(payload, finalOutput, parser.isJson(), parser.getOutputPath());
}

static int runUsbLabInstallCommand(const QStringList& args)
{
    UsbLabInstallCommandLineParser parser;
    parser.parse(args);

    UsbPassthroughManager manager;
    manager.refresh();

    const bool dependenciesReadyBefore = manager.dependenciesReady();
    QVariantMap preflight = usbLabInstallPreflight(manager);
    QVariantMap payload = usbLabStatusPayload(manager, false, false);
    payload[QStringLiteral("command")] = QStringLiteral("usb-lab-install");
    payload[QStringLiteral("state")] = QStringLiteral("starting");
    payload[QStringLiteral("dryRun")] = parser.isDryRun();
    payload[QStringLiteral("dependenciesReadyBefore")] = dependenciesReadyBefore;
    payload[QStringLiteral("dependenciesReadyAfter")] = QVariant();
    payload[QStringLiteral("installAction")] = preflight;
    payload[QStringLiteral("installAttempted")] = false;
    payload[QStringLiteral("installStarted")] = false;
    payload[QStringLiteral("postStatus")] = QVariant();
    payload[QStringLiteral("message")] = QVariant();
    payload[QStringLiteral("exitCode")] = QVariant();

    auto finish = [&](int exitCode, const QString& state, const QString& message, bool installAttempted, bool installStarted) -> int {
        payload[QStringLiteral("state")] = state;
        payload[QStringLiteral("message")] = message;
        payload[QStringLiteral("installAttempted")] = installAttempted;
        payload[QStringLiteral("installStarted")] = installStarted;
        payload[QStringLiteral("dependenciesReadyAfter")] = manager.dependenciesReady();
        payload[QStringLiteral("lastError")] = manager.lastError();
        payload[QStringLiteral("statusMessage")] = manager.statusMessage();
        payload[QStringLiteral("exitCode")] = exitCode;

        if (parser.isJson()) {
            return writeUsbLabInstallState(parser, payload, true) ? exitCode : 10;
        }

        const QString output = formatUsbLabInstallHuman(manager, preflight, parser.isDryRun(), installAttempted, installStarted);
        return writeUsbLabCommandOutput(output, parser.getOutputPath()) ? exitCode : 10;
    };

    if (!manager.isSupported()) {
        return finish(2, QStringLiteral("failed"), QStringLiteral("USB passthrough is not supported on this client OS."), false, false);
    }

    const bool installActionAvailable = preflight.value(QStringLiteral("available")).toBool();
    if (parser.isDryRun()) {
        if (dependenciesReadyBefore) {
            return finish(0, QStringLiteral("ready"), QStringLiteral("USB passthrough dependencies are already ready."), false, false);
        }
        if (installActionAvailable) {
            return finish(0, QStringLiteral("available"), preflight.value(QStringLiteral("message")).toString(), false, false);
        }
        return finish(3, QStringLiteral("failed"), preflight.value(QStringLiteral("message")).toString(), false, false);
    }

    if (dependenciesReadyBefore) {
        return finish(0, QStringLiteral("ready"), QStringLiteral("USB passthrough dependencies are already ready."), false, false);
    }

    const bool installStarted = manager.installDependency();
    manager.refresh();
    payload[QStringLiteral("postStatus")] = usbLabStatusPayload(manager, false, false);

    if (!installStarted) {
        const QString message = manager.lastError().isEmpty() ? QStringLiteral("USB passthrough dependency setup failed.") : manager.lastError();
        return finish(3, QStringLiteral("failed"), message, true, false);
    }

    const QString state = manager.dependenciesReady() ? QStringLiteral("ready") : QStringLiteral("started");
    const QString message = manager.dependenciesReady() ?
                                QStringLiteral("USB passthrough dependencies are ready.") :
                                manager.statusMessage();
    return finish(0, state, message, true, true);
}

static int runUsbLabListCommand(const QStringList& args)
{
    UsbLabListCommandLineParser parser;
    parser.parse(args);

    UsbPassthroughManager manager;
    manager.refresh();

    bool exporterReady = false;
    if (parser.shouldTestExporter()) {
        exporterReady = manager.testExporter();
    }

    QString output;
    if (parser.isJson()) {
        const QVariantMap payload = usbLabStatusPayload(manager, parser.shouldTestExporter(), exporterReady);
        output = QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson(QJsonDocument::Indented));
    }
    else {
        output = formatUsbLabStatusHuman(manager, parser.shouldTestExporter(), exporterReady);
    }

    if (!writeUsbLabCommandOutput(output, parser.getOutputPath())) {
        return 10;
    }
    if (!manager.isSupported()) {
        return 2;
    }
    if (!manager.dependenciesReady()) {
        return 3;
    }
    if (parser.shouldTestExporter() && !exporterReady) {
        return 4;
    }

    return 0;
}

static int runUsbLabExportCommand(const QStringList& args)
{
    UsbLabExportCommandLineParser parser;
    parser.parse(args);

    UsbPassthroughManager manager;
    manager.refresh();

    const QString busId = parser.getBusId();
    QVariantMap payload;
    payload[QStringLiteral("command")] = QStringLiteral("usb-lab-export");
    payload[QStringLiteral("state")] = QStringLiteral("starting");
    payload[QStringLiteral("busid")] = busId;
    payload[QStringLiteral("exporterPort")] = k_UsbLabDefaultExporterPort;
    payload[QStringLiteral("holdSeconds")] = parser.getHoldSeconds() >= 0 ? QVariant(parser.getHoldSeconds()) : QVariant();
    payload[QStringLiteral("bindRequested")] = parser.shouldBind();
    payload[QStringLiteral("boundByCommand")] = false;
    payload[QStringLiteral("supported")] = manager.isSupported();
    payload[QStringLiteral("backend")] = manager.backend();
    payload[QStringLiteral("dependenciesReady")] = manager.dependenciesReady();
    payload[QStringLiteral("statusMessage")] = manager.statusMessage();
    payload[QStringLiteral("exportServerStarted")] = false;
    payload[QStringLiteral("deviceFound")] = false;
    payload[QStringLiteral("exportableBeforeBind")] = false;
    payload[QStringLiteral("exportableAfterBind")] = false;
    payload[QStringLiteral("stopped")] = false;
    payload[QStringLiteral("cleanupDetachOk")] = QVariant();
    payload[QStringLiteral("cleanupUnbindOk")] = QVariant();
    payload[QStringLiteral("lastError")] = manager.lastError();

    auto finish = [&](int exitCode, const QString& state, const QString& message) -> int {
        payload[QStringLiteral("state")] = state;
        payload[QStringLiteral("message")] = message;
        payload[QStringLiteral("lastError")] = manager.lastError();
        payload[QStringLiteral("exitCode")] = exitCode;
        return writeUsbLabExportState(parser, payload, true) ? exitCode : 10;
    };

    if (!manager.isSupported()) {
        const QString message = QStringLiteral("USB passthrough is not supported on this client OS.");
        std::cerr << qtToStdString(message) << std::endl;
        return finish(2, QStringLiteral("failed"), message);
    }
    if (!manager.dependenciesReady()) {
        const QString message = manager.statusMessage();
        std::cerr << qtToStdString(message) << std::endl;
        return finish(3, QStringLiteral("failed"), message);
    }
    if (!manager.startExportServer()) {
        const QString message = manager.lastError();
        std::cerr << qtToStdString(message) << std::endl;
        return finish(4, QStringLiteral("failed"), message);
    }
    payload[QStringLiteral("exportServerStarted")] = true;

    bool found = false;
    bool exportable = usbLabDeviceExportable(manager, busId, &found);
    bool boundByCommand = false;
    payload[QStringLiteral("deviceFound")] = found;
    payload[QStringLiteral("exportableBeforeBind")] = exportable;

    if (!found) {
        const QString message = QStringLiteral("USB device %1 was not found.").arg(busId);
        std::cerr << qtToStdString(message) << std::endl;
        manager.stopExportServer();
        payload[QStringLiteral("exportServerStarted")] = false;
        return finish(5, QStringLiteral("failed"), message);
    }
    if (!exportable && !parser.shouldBind()) {
        const QString message = QStringLiteral("USB device %1 is not shared/exportable. Re-run with --bind to share it for this lab export.").arg(busId);
        std::cerr << qtToStdString(message) << std::endl;
        manager.stopExportServer();
        payload[QStringLiteral("exportServerStarted")] = false;
        return finish(6, QStringLiteral("failed"), message);
    }
    if (!exportable) {
        if (!manager.bindDevice(busId)) {
            const QString message = manager.lastError();
            std::cerr << qtToStdString(message) << std::endl;
            manager.stopExportServer();
            payload[QStringLiteral("exportServerStarted")] = false;
            return finish(7, QStringLiteral("failed"), message);
        }
        boundByCommand = true;
        payload[QStringLiteral("boundByCommand")] = true;
        manager.refresh();
        exportable = usbLabDeviceExportable(manager, busId, &found);
        payload[QStringLiteral("deviceFound")] = found;
        payload[QStringLiteral("exportableAfterBind")] = exportable;
        if (!found || !exportable) {
            const bool unbindOk = manager.unbindDevice(busId);
            payload[QStringLiteral("cleanupUnbindOk")] = unbindOk;
            payload[QStringLiteral("boundByCommand")] = !unbindOk;
            const QString message = QStringLiteral("USB device %1 was not exportable after binding.").arg(busId);
            std::cerr << qtToStdString(message) << std::endl;
            manager.stopExportServer();
            payload[QStringLiteral("exportServerStarted")] = false;
            return finish(8, QStringLiteral("failed"), message);
        }
    }
    else {
        payload[QStringLiteral("exportableAfterBind")] = true;
    }

    payload[QStringLiteral("state")] = QStringLiteral("ready");
    payload[QStringLiteral("message")] = QStringLiteral("USB lab exporter is ready.");
    payload[QStringLiteral("lastError")] = manager.lastError();
    payload[QStringLiteral("exitCode")] = QVariant();
    if (!writeUsbLabExportState(parser, payload, false)) {
        const bool detachOk = manager.detachDevice(busId);
        payload[QStringLiteral("cleanupDetachOk")] = detachOk;
        if (boundByCommand) {
            const bool unbindOk = manager.unbindDevice(busId);
            payload[QStringLiteral("cleanupUnbindOk")] = unbindOk;
            payload[QStringLiteral("boundByCommand")] = !unbindOk;
        }
        manager.stopExportServer();
        payload[QStringLiteral("exportServerStarted")] = false;
        return 10;
    }

    std::cout << "USB lab exporter ready for busid " << qtToStdString(busId)
              << " on local USB/IP port " << k_UsbLabDefaultExporterPort << "." << std::endl;
    std::cout << "Run host direct or broker attach while this process stays open." << std::endl;
    if (parser.getHoldSeconds() >= 0) {
        std::cout << "Exporter will stop automatically after " << parser.getHoldSeconds() << " seconds";
        if (boundByCommand) {
            std::cout << " and unshare the device";
        }
        std::cout << ". Press Ctrl+C to stop early." << std::endl;
    }
    else {
        std::cout << "Press Enter or Ctrl+C to stop exporting";
        if (boundByCommand) {
            std::cout << " and unshare the device";
        }
        std::cout << "." << std::endl;
    }

    waitForUsbLabStop(parser.getHoldSeconds());
    const bool detachOk = manager.detachDevice(busId);
    payload[QStringLiteral("cleanupDetachOk")] = detachOk;
    if (boundByCommand) {
        const bool unbindOk = manager.unbindDevice(busId);
        payload[QStringLiteral("cleanupUnbindOk")] = unbindOk;
        payload[QStringLiteral("boundByCommand")] = !unbindOk;
    }
    manager.stopExportServer();
    payload[QStringLiteral("exportServerStarted")] = false;
    payload[QStringLiteral("stopped")] = true;

    std::cout << "USB lab exporter stopped." << std::endl;
    return finish(0, QStringLiteral("stopped"), QStringLiteral("USB lab exporter stopped."));
}

static int runUsbLabTunnelCommand(const QStringList& args)
{
    UsbLabTunnelCommandLineParser parser;
    parser.parse(args);

    UsbPassthroughManager manager;
    manager.refresh();

    QVariantMap payload;
    payload[QStringLiteral("command")] = QStringLiteral("usb-lab-tunnel");
    payload[QStringLiteral("state")] = QStringLiteral("starting");
    payload[QStringLiteral("host")] = parser.getHost();
    payload[QStringLiteral("hostPort")] = parser.getPort();
    payload[QStringLiteral("busid")] = parser.getBusId();
    payload[QStringLiteral("exporterPort")] = parser.getExporterPort();
    payload[QStringLiteral("holdSeconds")] = parser.getHoldSeconds() >= 0 ? QVariant(parser.getHoldSeconds()) : QVariant();
    payload[QStringLiteral("bindRequested")] = parser.shouldBind();
    payload[QStringLiteral("boundByCommand")] = false;
    payload[QStringLiteral("tokenPresent")] = !parser.getToken().isEmpty();
    payload[QStringLiteral("tokenLength")] = parser.getToken().length();
    payload[QStringLiteral("supported")] = manager.isSupported();
    payload[QStringLiteral("backend")] = manager.backend();
    payload[QStringLiteral("dependenciesReady")] = manager.dependenciesReady();
    payload[QStringLiteral("statusMessage")] = manager.statusMessage();
    payload[QStringLiteral("exportServerStarted")] = false;
    payload[QStringLiteral("deviceFound")] = false;
    payload[QStringLiteral("exportableBeforeBind")] = false;
    payload[QStringLiteral("exportableAfterBind")] = false;
    payload[QStringLiteral("tunnelConnected")] = false;
    payload[QStringLiteral("stopped")] = false;
    payload[QStringLiteral("cleanupDetachOk")] = QVariant();
    payload[QStringLiteral("cleanupUnbindOk")] = QVariant();
    payload[QStringLiteral("lastError")] = manager.lastError();

    auto finish = [&](int exitCode, const QString& state, const QString& message) -> int {
        payload[QStringLiteral("state")] = state;
        payload[QStringLiteral("message")] = message;
        payload[QStringLiteral("lastError")] = manager.lastError();
        payload[QStringLiteral("exitCode")] = exitCode;
        return writeUsbLabTunnelState(parser, payload, true) ? exitCode : 10;
    };

    if (!manager.isSupported()) {
        const QString message = QStringLiteral("USB passthrough is not supported on this client OS.");
        std::cerr << qtToStdString(message) << std::endl;
        return finish(2, QStringLiteral("failed"), message);
    }
    if (!manager.dependenciesReady()) {
        const QString message = manager.statusMessage();
        std::cerr << qtToStdString(message) << std::endl;
        return finish(3, QStringLiteral("failed"), message);
    }
    if (!manager.startExportServer()) {
        const QString message = manager.lastError();
        std::cerr << qtToStdString(message) << std::endl;
        return finish(4, QStringLiteral("failed"), message);
    }
    payload[QStringLiteral("exportServerStarted")] = true;

    const QString busId = parser.getBusId();
    bool found = false;
    bool exportable = usbLabDeviceExportable(manager, busId, &found);
    bool boundByCommand = false;
    payload[QStringLiteral("deviceFound")] = found;
    payload[QStringLiteral("exportableBeforeBind")] = exportable;

    if (!found) {
        const QString message = QStringLiteral("USB device %1 was not found.").arg(busId);
        std::cerr << qtToStdString(message) << std::endl;
        manager.stopExportServer();
        payload[QStringLiteral("exportServerStarted")] = false;
        return finish(5, QStringLiteral("failed"), message);
    }
    if (!exportable && !parser.shouldBind()) {
        const QString message = QStringLiteral("USB device %1 is not shared/exportable. Re-run with --bind to share it for this lab tunnel.").arg(busId);
        std::cerr << qtToStdString(message) << std::endl;
        manager.stopExportServer();
        payload[QStringLiteral("exportServerStarted")] = false;
        return finish(6, QStringLiteral("failed"), message);
    }
    if (!exportable) {
        if (!manager.bindDevice(busId)) {
            const QString message = manager.lastError();
            std::cerr << qtToStdString(message) << std::endl;
            manager.stopExportServer();
            payload[QStringLiteral("exportServerStarted")] = false;
            return finish(7, QStringLiteral("failed"), message);
        }
        boundByCommand = true;
        payload[QStringLiteral("boundByCommand")] = true;
        manager.refresh();
        exportable = usbLabDeviceExportable(manager, busId, &found);
        payload[QStringLiteral("deviceFound")] = found;
        payload[QStringLiteral("exportableAfterBind")] = exportable;
        if (!found || !exportable) {
            if (boundByCommand) {
                manager.unbindDevice(busId);
                payload[QStringLiteral("cleanupUnbindOk")] = manager.lastError().isEmpty();
                payload[QStringLiteral("boundByCommand")] = false;
            }
            const QString message = QStringLiteral("USB device %1 was not exportable after binding.").arg(busId);
            std::cerr << qtToStdString(message) << std::endl;
            manager.stopExportServer();
            payload[QStringLiteral("exportServerStarted")] = false;
            return finish(8, QStringLiteral("failed"), message);
        }
    }
    else {
        payload[QStringLiteral("exportableAfterBind")] = true;
    }

    std::cout << "Opening USB lab reverse tunnel for busid " << qtToStdString(busId)
              << " to " << qtToStdString(parser.getHost()) << ":" << parser.getPort()
              << " via local exporter port " << parser.getExporterPort() << std::endl;
    if (!manager.startTunnel(parser.getHost(), parser.getPort(), parser.getToken(), busId, parser.getExporterPort())) {
        const QString message = manager.lastError();
        if (boundByCommand) {
            const bool unbindOk = manager.unbindDevice(busId);
            payload[QStringLiteral("cleanupUnbindOk")] = unbindOk;
            payload[QStringLiteral("boundByCommand")] = !unbindOk;
        }
        manager.stopExportServer();
        payload[QStringLiteral("exportServerStarted")] = false;
        std::cerr << qtToStdString(message) << std::endl;
        return finish(9, QStringLiteral("failed"), message);
    }
    payload[QStringLiteral("tunnelConnected")] = true;
    payload[QStringLiteral("state")] = QStringLiteral("connected");
    payload[QStringLiteral("message")] = QStringLiteral("USB lab reverse tunnel connected.");
    payload[QStringLiteral("lastError")] = manager.lastError();
    payload[QStringLiteral("exitCode")] = QVariant();
    if (!writeUsbLabTunnelState(parser, payload, false)) {
        manager.stopTunnels();
        const bool detachOk = manager.detachDevice(busId);
        payload[QStringLiteral("cleanupDetachOk")] = detachOk;
        if (boundByCommand) {
            const bool unbindOk = manager.unbindDevice(busId);
            payload[QStringLiteral("cleanupUnbindOk")] = unbindOk;
            payload[QStringLiteral("boundByCommand")] = !unbindOk;
        }
        manager.stopExportServer();
        payload[QStringLiteral("exportServerStarted")] = false;
        return 10;
    }

    if (parser.getHoldSeconds() >= 0) {
        std::cout << "USB lab reverse tunnel connected for " << parser.getHoldSeconds()
                  << " seconds while the host imports the device." << std::endl;
        std::cout << "Press Ctrl+C to stop the tunnel early";
        if (boundByCommand) {
            std::cout << " and unshare the device";
        }
        std::cout << "." << std::endl;
    }
    else {
        std::cout << "USB lab reverse tunnel connected. Keep this process running while the host imports the device." << std::endl;
        std::cout << "Press Enter or Ctrl+C to stop the tunnel";
        if (boundByCommand) {
            std::cout << " and unshare the device";
        }
        std::cout << "." << std::endl;
    }

    waitForUsbLabStop(parser.getHoldSeconds());
    manager.stopTunnels();
    const bool detachOk = manager.detachDevice(busId);
    payload[QStringLiteral("cleanupDetachOk")] = detachOk;
    if (boundByCommand) {
        const bool unbindOk = manager.unbindDevice(busId);
        payload[QStringLiteral("cleanupUnbindOk")] = unbindOk;
        payload[QStringLiteral("boundByCommand")] = !unbindOk;
    }
    manager.stopExportServer();
    payload[QStringLiteral("exportServerStarted")] = false;
    payload[QStringLiteral("tunnelConnected")] = false;
    payload[QStringLiteral("stopped")] = true;

    std::cout << "USB lab reverse tunnel stopped." << std::endl;
    return finish(0, QStringLiteral("stopped"), QStringLiteral("USB lab reverse tunnel stopped."));
}

void sdlLogToDiskHandler(void*, int category, SDL_LogPriority priority, const char* message)
{
    QString priorityTxt;

    switch (priority) {
    case SDL_LOG_PRIORITY_VERBOSE:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Verbose";
        break;
    case SDL_LOG_PRIORITY_DEBUG:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Debug";
        break;
    case SDL_LOG_PRIORITY_INFO:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Info";
        break;
    case SDL_LOG_PRIORITY_WARN:
        if (s_SuppressVerboseOutput) {
            return;
        }
        priorityTxt = "Warn";
        break;
    case SDL_LOG_PRIORITY_ERROR:
        priorityTxt = "Error";
        break;
    case SDL_LOG_PRIORITY_CRITICAL:
        priorityTxt = "Critical";
        break;
    default:
        priorityTxt = "Unknown";
        break;
    }

    QTime logTime = QTime::fromMSecsSinceStartOfDay(s_LoggerTime.elapsed());
    QString txt = QString("%1 - SDL %2 (%3): %4\n").arg(logTime.toString()).arg(priorityTxt).arg(category).arg(message);

    logToLoggerStream(txt);
}

void qtLogToDiskHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    QString typeTxt;

    switch (type) {
    case QtDebugMsg:
        if (s_SuppressVerboseOutput) {
            return;
        }
        typeTxt = "Debug";
        break;
    case QtInfoMsg:
        if (s_SuppressVerboseOutput) {
            return;
        }
        typeTxt = "Info";
        break;
    case QtWarningMsg:
        if (s_SuppressVerboseOutput) {
            return;
        }
        typeTxt = "Warning";
        break;
    case QtCriticalMsg:
        typeTxt = "Critical";
        break;
    case QtFatalMsg:
        typeTxt = "Fatal";
        break;
    }

    QTime logTime = QTime::fromMSecsSinceStartOfDay(s_LoggerTime.elapsed());
    QString txt = QString("%1 - Qt %2: %3\n").arg(logTime.toString()).arg(typeTxt).arg(msg);

    logToLoggerStream(txt);
}

#ifdef HAVE_FFMPEG

void ffmpegLogToDiskHandler(void* ptr, int level, const char* fmt, va_list vl)
{
    char lineBuffer[1024];
    static int printPrefix = 1;

    if ((level & 0xFF) > av_log_get_level()) {
        return;
    }
    else if ((level & 0xFF) > AV_LOG_WARNING && s_SuppressVerboseOutput) {
        return;
    }

    // We need to use the *previous* printPrefix value to determine whether to
    // print the prefix this time. av_log_format_line() will set the printPrefix
    // value to indicate whether the prefix should be printed *next time*.
    bool shouldPrefixThisMessage = printPrefix != 0;

    av_log_format_line(ptr, level, fmt, vl, lineBuffer, sizeof(lineBuffer), &printPrefix);

    if (shouldPrefixThisMessage) {
        QTime logTime = QTime::fromMSecsSinceStartOfDay(s_LoggerTime.elapsed());
        QString txt = QString("%1 - FFmpeg: %2").arg(logTime.toString()).arg(lineBuffer);
        logToLoggerStream(txt);
    }
    else {
        QString txt = QString(lineBuffer);
        logToLoggerStream(txt);
    }
}

#endif

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>

static UINT s_HitUnhandledException = 0;

LONG WINAPI UnhandledExceptionHandler(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
    // Only write a dump for the first unhandled exception
    if (InterlockedCompareExchange(&s_HitUnhandledException, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    WCHAR dmpFileName[MAX_PATH];
    swprintf_s(dmpFileName, L"%ls\\Moonlight-%I64u.dmp",
               (PWCHAR)QDir::toNativeSeparators(Path::getLogDir()).utf16(), QDateTime::currentSecsSinceEpoch());
    QString qDmpFileName = QString::fromUtf16((const char16_t*)dmpFileName);
    HANDLE dumpHandle = CreateFileW(dmpFileName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dumpHandle != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info;

        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = ExceptionInfo;
        info.ClientPointers = FALSE;

        DWORD typeFlags = MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpIgnoreInaccessibleMemory |
                MiniDumpWithUnloadedModules |
                MiniDumpWithThreadInfo;

        if (MiniDumpWriteDump(GetCurrentProcess(),
                               GetCurrentProcessId(),
                               dumpHandle,
                               (MINIDUMP_TYPE)typeFlags,
                               &info,
                               nullptr,
                               nullptr)) {
            qCritical() << "Unhandled exception! Minidump written to:" << qDmpFileName;
        }
        else {
            qCritical() << "Unhandled exception! Failed to write dump:" << GetLastError();
        }

        CloseHandle(dumpHandle);
    }
    else {
        qCritical() << "Unhandled exception! Failed to open dump file:" << qDmpFileName << "with error" << GetLastError();
    }

    // Sleep for a moment to allow the logging thread to finish up before crashing
    if (g_AsyncLoggingEnabled) {
        Sleep(500);
    }

    // Let the program crash and WER collect a dump
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

#ifdef Q_OS_UNIX

static int signalFds[2];

void handleSignal(int sig)
{
    send(signalFds[0], &sig, sizeof(sig), 0);
}

int SDLCALL signalHandlerThread(void* data)
{
    Q_UNUSED(data);

    Session* lastSession = nullptr;
    bool requestedQuit = false;

    int sig;
    while (recv(signalFds[1], &sig, sizeof(sig), MSG_WAITALL) == sizeof(sig)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Received signal: %d", sig);

        Session* session;
        switch (sig) {
        case SIGINT:
        case SIGTERM:
            // Check if we have an active streaming session
            session = Session::get();
            if (session != nullptr) {
                // Exit immediately if we haven't changed state since last attempt
                if (session == lastSession || requestedQuit) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Exiting immediately on second signal");
                    _Exit(1);
                }

                if (sig == SIGTERM) {
                    // If this is a SIGTERM, set the flag to quit
                    session->setShouldExit();
                    requestedQuit = true;
                }

                // Stop the streaming session
                session->interrupt();
                lastSession = session;
            }
            else {
                // Exit immediately if we haven't changed state since last attempt
                if (requestedQuit) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Exiting immediately on second signal");
                    _Exit(1);
                }

                // If we're not streaming, we'll close the whole app
                QCoreApplication::instance()->quit();
                requestedQuit = true;
            }
            break;

        default:
            Q_UNREACHABLE();
        }
    }

    return 0;
}

void configureSignalHandlers()
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, signalFds) == -1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "socketpair() failed: %d",
                     errno);
        return;
    }

    // Create a thread to handle our signals safely outside of signal context
    SDL_Thread* thread = SDL_CreateThread(signalHandlerThread, "Signal Handler", nullptr);
    SDL_DetachThread(thread);

    struct sigaction sa = {};
    sa.sa_handler = handleSignal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

#endif

int main(int argc, char *argv[])
{
    SDL_SetMainReady();

    // Set the app version for the QCommandLineParser's showVersion() command
    QCoreApplication::setApplicationVersion(VERSION_STR);

    // Set these here to allow us to use the default QSettings constructor.
    // These also ensure that our cache directory is named correctly. As such,
    // it is critical that these be called before Path::initialize().
    QCoreApplication::setOrganizationName("Moonlight Game Streaming Project");
    QCoreApplication::setOrganizationDomain("moonlight-stream.com");
    QCoreApplication::setApplicationName("Moonlight");

    if (QFile(QDir::currentPath() + "/portable.dat").exists()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir::currentPath());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QDir::currentPath());

        // Initialize paths for portable mode
        Path::initialize(true);
    }
    else {
        // Initialize paths for standard installation
        Path::initialize(false);
    }

    // Override the default QML cache directory with the one we chose
    if (qEnvironmentVariableIsEmpty("QML_DISK_CACHE_PATH")) {
        qputenv("QML_DISK_CACHE_PATH", Path::getQmlCacheDir().toUtf8());
    }

#ifdef Q_OS_WIN32
    // Grab the original std handles before we potentially redirect them later
    HANDLE oldConOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE oldConErr = GetStdHandle(STD_ERROR_HANDLE);
#endif

#ifdef LOG_TO_FILE
    QDir tempDir(Path::getLogDir());

#ifdef Q_OS_WIN32
    // Only log to a file if the user didn't redirect stderr somewhere else
    if (IS_UNSPECIFIED_HANDLE(oldConErr))
#endif
    {
        s_LoggerFile = new QFile(tempDir.filePath(QString("Moonlight-%1.log").arg(QDateTime::currentSecsSinceEpoch())));
        if (s_LoggerFile->open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(stderr) << "Redirecting log output to " << s_LoggerFile->fileName() << Qt::endl;
            s_LoggerStream.setDevice(s_LoggerFile);
        }
    }
#endif

    // Serialize log messages on a single thread
    s_LoggerThread.setMaxThreadCount(1);
    s_LoggerTime.start();

    // Register our logger with all libraries
#if SDL_VERSION_ATLEAST(3, 0, 0)
    SDL_SetLogOutputFunction(sdlLogToDiskHandler, nullptr);
#else
    SDL_LogOutputFunction oldSdlLogFn;
    void* oldSdlLogUserdata;
    SDL_LogGetOutputFunction(&oldSdlLogFn, &oldSdlLogUserdata);
    SDL_LogSetOutputFunction(sdlLogToDiskHandler, nullptr);
#endif
    qInstallMessageHandler(qtLogToDiskHandler);
#ifdef HAVE_FFMPEG
    av_log_set_callback(ffmpegLogToDiskHandler);
#endif

#ifdef Q_OS_WIN32
    // Create a crash dump when we crash on Windows
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
#endif

#ifdef LOG_TO_FILE
    // Prune the oldest existing logs if there are more than 10
    QStringList existingLogNames = tempDir.entryList(QStringList("Moonlight-*.log"), QDir::NoFilter, QDir::SortFlag::Time);
    for (int i = 10; i < existingLogNames.size(); i++) {
        qInfo() << "Removing old log file:" << existingLogNames.at(i);
        QFile(tempDir.filePath(existingLogNames.at(i))).remove();
    }
#endif

#if defined(Q_OS_WIN32)
    // Force AntiHooking.dll to be statically imported and loaded
    // by ntdll on Win32 platforms by calling a dummy function.
    AntiHookingDummyImport();
#elif defined(APP_IMAGE)
    // Force libssl.so to be directly linked to our binary, so
    // linuxdeployqt can find it and include it in our AppImage.
    // QtNetwork will pull it in via dlopen().
    SSL_free(nullptr);
#endif

    // We keep this at function scope to ensure it stays around while we're running,
    // because the Qt QPA will need to read it. Since the temporary file is only
    // created when open() is called, this doesn't do any harm for other platforms.
    QTemporaryFile eglfsConfigFile;

    // Avoid using High DPI on EGLFS. It breaks font rendering.
    // https://bugreports.qt.io/browse/QTBUG-64377
    //
    // NB: We can't use QGuiApplication::platformName() here because it is only
    // set once the QGuiApplication is created, which is too late to enable High DPI :(
    if (WMUtils::isRunningWindowManager()) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        // Enable High DPI support on Qt 5.x. It is always enabled on Qt 6.0
        QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        // Enable fractional High DPI scaling on Qt 5.14 and later
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    }
    else {
#ifndef STEAM_LINK
        if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
            qInfo() << "Unable to detect Wayland or X11, so EGLFS will be used by default. Set QT_QPA_PLATFORM to override this.";
            qputenv("QT_QPA_PLATFORM", "eglfs");

            if (!qEnvironmentVariableIsSet("QT_QPA_EGLFS_ALWAYS_SET_MODE")) {
                qInfo() << "Setting display mode by default. Set QT_QPA_EGLFS_ALWAYS_SET_MODE=0 to override this.";

                // The UI doesn't appear on RetroPie without this option.
                qputenv("QT_QPA_EGLFS_ALWAYS_SET_MODE", "1");
            }

            if (!QFile("/dev/dri").exists()) {
                qWarning() << "Unable to find a KMSDRM display device!";
                qWarning() << "On the Raspberry Pi, you must enable the 'fake KMS' driver in raspi-config to use Moonlight outside of the GUI environment.";
            }
            else if (!qEnvironmentVariableIsSet("QT_QPA_EGLFS_KMS_CONFIG")) {
                // HACK: Remove this when Qt is fixed to properly check for display support before picking a card
                QString cardOverride = WMUtils::getDrmCardOverride();
                if (!cardOverride.isEmpty()) {
                    if (eglfsConfigFile.open()) {
                        qInfo() << "Overriding default Qt EGLFS card selection to" << cardOverride;
                        QTextStream(&eglfsConfigFile) << "{ \"device\": \"" << cardOverride << "\" }";
                        qputenv("QT_QPA_EGLFS_KMS_CONFIG", eglfsConfigFile.fileName().toUtf8());
                        eglfsConfigFile.close();
                    }
                }
            }
        }

        // EGLFS uses OpenGLES 2.0, so we will too. Some embedded platforms may not
        // even have working OpenGL implementations, so GLES is the only option.
        // See https://github.com/moonlight-stream/moonlight-qt/issues/868
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
#endif
    }

    bool forceGles;
    if (!Utils::getEnvironmentVariableOverride("FORCE_QT_GLES", &forceGles)) {
        forceGles = WMUtils::isRunningNvidiaProprietaryDriverX11() ||
                    !WMUtils::supportsDesktopGLWithEGL();
    }
    if (forceGles) {
        // The Nvidia proprietary driver causes Qt to render a black window when using
        // the default Desktop GL profile with EGL. AS a workaround, we default to
        // OpenGL ES when running on Nvidia on X11.
        // https://qt-project.atlassian.net/browse/QTBUG-106065
        QSurfaceFormat fmt;
        fmt.setRenderableType(QSurfaceFormat::OpenGLES);
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    // Some ARM and RISC-V embedded devices don't have working GLX which can cause
    // SDL to fail to find a working OpenGL implementation at all. Let's force EGL
    // on all platforms for both SDL and Qt. This also avoids GLX-EGL interop issues
    // when trying to use EGL on the main thread after Qt uses GLX.
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");

#ifdef Q_OS_WIN32
    // Let us see the true VBlank rather than DWM's approximation. We do this here
    // because this API must be called before the first swapchain (which Qt will
    // create when the window is displayed). This is supported on Win11 22H2+.
    auto fnDXGIDisableVBlankVirtualization =
        (decltype(DXGIDisableVBlankVirtualization)*)GetProcAddress(GetModuleHandleW(L"dxgi.dll"),
                                                                   "DXGIDisableVBlankVirtualization");
    if (fnDXGIDisableVBlankVirtualization) {
        fnDXGIDisableVBlankVirtualization();
    }
#endif

#ifdef Q_OS_MACOS
    // This avoids using the default keychain for SSL, which may cause
    // password prompts on macOS.
    qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
#endif

#if defined(Q_OS_WIN32) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!qEnvironmentVariableIsSet("QT_OPENGL")) {
        // On Windows, use ANGLE so we don't have to load OpenGL
        // user-mode drivers into our app. OGL drivers (especially Intel)
        // seem to crash Moonlight far more often than DirectX.
        qputenv("QT_OPENGL", "angle");
    }
#endif

#if !defined(Q_OS_WIN32) || QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Moonlight requires the non-threaded renderer because we depend
    // on being able to control the render thread by blocking in the
    // main thread (and pumping events from the main thread when needed).
    // That doesn't work with the threaded renderer which causes all
    // sorts of odd behavior depending on the platform.
    //
    // NB: Windows defaults to the "windows" non-threaded render loop on
    // Qt 5 and the threaded render loop on Qt 6.
    qputenv("QSG_RENDER_LOOP", "basic");
#endif

#if defined(Q_OS_DARWIN) && defined(QT_DEBUG)
    // Enable Metal valiation for debug builds
    qputenv("MTL_DEBUG_LAYER", "1");
    qputenv("MTL_SHADER_VALIDATION", "1");
#endif

    // We don't want system proxies to apply to us
    QNetworkProxyFactory::setUseSystemConfiguration(false);

    // Clear any default application proxy
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    QNetworkProxy::setApplicationProxy(noProxy);

    // Register custom metatypes for use in signals
    qRegisterMetaType<NvApp>("NvApp");

    // Allow the display to sleep by default. We will manually use SDL_DisableScreenSaver()
    // and SDL_EnableScreenSaver() when appropriate. This hint must be set before
    // initializing the SDL video subsystem to have any effect.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    // We use MMAL to render on Raspberry Pi, so we do not require DRM master.
    SDL_SetHint(SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER, "0");

    // Use Direct3D 9Ex to avoid a deadlock caused by the D3D device being reset when
    // the user triggers a UAC prompt. This option controls the software/SDL renderer.
    // The DXVA2 renderer uses Direct3D 9Ex itself directly.
    SDL_SetHint(SDL_HINT_WINDOWS_USE_D3D9EX, "1");

    if (SDL_InitSubSystem(SDL_INIT_TIMER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_TIMER) failed: %s",
                     SDL_GetError());
        return -1;
    }

#ifdef STEAM_LINK
    // Steam Link requires that we initialize video before creating our
    // QGuiApplication in order to configure the framebuffer correctly.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return -1;
    }
#endif

    // Use atexit() to ensure SDL_Quit() is called. This avoids
    // racing with object destruction where SDL may be used.
    atexit(SDL_Quit);

    // Avoid the default behavior of changing the timer resolution to 1 ms.
    // We don't want this all the time that Moonlight is open. We will set
    // it manually when we start streaming.
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");

    // Disable minimize on focus loss by default. Users seem to want this off by default.
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    // SDL 2.0.12 changes the default behavior to use the button label rather than the button
    // position as most other software does. Set this back to 0 to stay consistent with prior
    // releases of Moonlight.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");

    // Disable relative mouse scaling to renderer size or logical DPI. We want to send
    // the mouse motion exactly how it was given to us.
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SCALING, "0");

    // Set our app name for SDL to use with PulseAudio and PipeWire. This matches what we
    // provide as our app name to libsoundio too. On SDL 2.0.18+, SDL_APP_NAME is also used
    // for screensaver inhibitor reporting.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Moonlight");
    SDL_SetHint(SDL_HINT_APP_NAME, "Moonlight");

    // We handle capturing the mouse ourselves when it leaves the window, so we don't need
    // SDL doing it for us behind our backs.
    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");

    // SDL will try to lock the mouse cursor on Wayland if it's not visible in order to
    // support applications that assume they can warp the cursor (which isn't possible
    // on Wayland). We don't want this behavior because it interferes with seamless mouse
    // mode when toggling between windowed and fullscreen modes by unexpectedly locking
    // the mouse cursor.
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_EMULATE_MOUSE_WARP, "0");

#ifdef QT_DEBUG
    // Allow thread naming using exceptions on debug builds. SDL doesn't use SEH
    // when throwing the exceptions, so we don't enable it for release builds out
    // of caution.
    SDL_SetHint(SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING, "0");
#endif

    // Enable fast parameter checks on SDL 3.4.0+. We don't abuse the API by passing
    // incorrect objects, so we don't need additional expensive parameter checks.
    SDL_SetHint("SDL_INVALID_PARAM_CHECKS", "1");

    // Disable hotplug detection for SDL_GetKeyboards() and SDL_GetMice(). We don't
    // use this functionality and it can cause hangs when querying broken devices.
    SDL_SetHint("SDL_WINDOWS_DETECT_DEVICE_HOTPLUG", "0");

    QGuiApplication app(argc, argv);

#ifdef Q_OS_UNIX
    // Register signal handlers to arbitrate between SDL and Qt.
    // NB: This has to be done after the QGuiApplication is constructed to
    // ensure Qt has already installed its VT signals before we override
    // some of them with our own.
    configureSignalHandlers();
#endif

#ifdef Q_OS_WIN32
    // If we don't have stdout or stderr handles (which will normally be the case
    // since we're a /SUBSYSTEM:WINDOWS app), attach to our parent console and use
    // that for stdout and stderr.
    //
    // If we do have stdout or stderr handles, that means the user has used standard
    // handle redirection. In that case, we don't want to override those handles.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // If we didn't have an old stdout/stderr handle, use the new CONOUT$ handle
        if (IS_UNSPECIFIED_HANDLE(oldConOut)) {
            FILE* fp;
            if (freopen_s(&fp, "CONOUT$", "w", stdout) == 0) {
                setvbuf(fp, NULL, _IONBF, 0);
            }
            else {
                freopen_s(&fp, "NUL", "w", stdout);
            }
        }
        if (IS_UNSPECIFIED_HANDLE(oldConErr)) {
            FILE* fp;
            if (freopen_s(&fp, "CONOUT$", "w", stderr) == 0) {
                setvbuf(fp, NULL, _IONBF, 0);
            }
            else {
                freopen_s(&fp, "NUL", "w", stderr);
            }
        }
    }
#endif

    GlobalCommandLineParser parser;
    GlobalCommandLineParser::ParseResult commandLineParserResult = parser.parse(app.arguments());
    switch (commandLineParserResult) {
    case GlobalCommandLineParser::ListRequested:
    case GlobalCommandLineParser::UsbLabInstallRequested:
    case GlobalCommandLineParser::UsbLabListRequested:
    case GlobalCommandLineParser::UsbLabExportRequested:
    case GlobalCommandLineParser::UsbLabTunnelRequested:
        // Don't log to the console since it will jumble the command output
        s_SuppressVerboseOutput = true;
        break;
    default:
        break;
    }

    SDL_version compileVersion;
    SDL_VERSION(&compileVersion);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Compiled with SDL %d.%d.%d",
                compileVersion.major, compileVersion.minor, compileVersion.patch);

    SDL_version runtimeVersion;
    SDL_GetVersion(&runtimeVersion);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Running with SDL %d.%d.%d",
                runtimeVersion.major, runtimeVersion.minor, runtimeVersion.patch);

    // Apply the initial translation based on user preference
    StreamingPreferences::get()->retranslate();

    // Trickily declare the translation for dialog buttons
    QCoreApplication::translate("QPlatformTheme", "&Yes");
    QCoreApplication::translate("QPlatformTheme", "&No");
    QCoreApplication::translate("QPlatformTheme", "OK");
    QCoreApplication::translate("QPlatformTheme", "Help");
    QCoreApplication::translate("QPlatformTheme", "Cancel");

    // After the QGuiApplication is created, the platform stuff will be initialized
    // and we can set the SDL video driver to match Qt.
    if (QGuiApplication::platformName() == "xcb") {
        if (WMUtils::isRunningWayland()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Detected XWayland. This will probably break hardware decoding! Try running with QT_QPA_PLATFORM=wayland or switch to X11.");
        }
        qputenv("SDL_VIDEODRIVER", "x11");
    }
    else if (QGuiApplication::platformName().startsWith("wayland")) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Detected Wayland");
        qputenv("SDL_VIDEODRIVER", "wayland");
    }
#ifndef STEAM_LINK
    // Force use of the KMSDRM backend for SDL when using Qt platform plugins
    // that directly draw to the display without a windowing system.
    else if (QGuiApplication::platformName() == "eglfs" || QGuiApplication::platformName() == "linuxfb") {
        qputenv("SDL_VIDEODRIVER", "kmsdrm");
    }
#endif

#ifdef HAVE_DRM_MASTER_HOOKS
    // Only use the Qt-SDL DRM master interoperability hooks if Qt is using KMS
    g_DisableDrmHooks = QGuiApplication::platformName() != "eglfs";
#endif

#ifdef STEAM_LINK
    // Qt 5.9 from the Steam Link SDK is not able to load any fonts
    // since the Steam Link doesn't include any of the ones it looks
    // for. We know it has NotoSans so we will explicitly ask for that.
    if (app.font().family().isEmpty()) {
        qWarning() << "SL HACK: No default font - using NotoSans";

        QFont fon("NotoSans");
        app.setFont(fon);
    }

    // Move the mouse to the bottom right so it's invisible when using
    // gamepad-only navigation.
    QCursor().setPos(0xFFFF, 0xFFFF);
#elif !SDL_VERSION_ATLEAST(2, 0, 11) && defined(Q_OS_LINUX) && (defined(__arm__) || defined(__aarch64__))
    if (qgetenv("SDL_VIDEO_GL_DRIVER").isEmpty() && QGuiApplication::platformName() == "eglfs") {
        // Look for Raspberry Pi GLES libraries. SDL 2.0.10 and earlier needs some help finding
        // the correct libraries for the KMSDRM backend if not compiled with the RPI backend enabled.
        if (SDL_LoadObject("libbrcmGLESv2.so") != nullptr) {
            qputenv("SDL_VIDEO_GL_DRIVER", "libbrcmGLESv2.so");
        }
        else if (SDL_LoadObject("/opt/vc/lib/libbrcmGLESv2.so") != nullptr) {
            qputenv("SDL_VIDEO_GL_DRIVER", "/opt/vc/lib/libbrcmGLESv2.so");
        }
    }
#endif

#ifndef Q_OS_DARWIN
    // Set the window icon except on macOS where we want to keep the
    // modified macOS 11 style rounded corner icon.
    app.setWindowIcon(QIcon(":/res/moonlight.svg"));
#endif

    // This is necessary to show our icon correctly on Wayland
    app.setDesktopFileName("com.moonlight_stream.Moonlight");
    qputenv("SDL_VIDEO_WAYLAND_WMCLASS", "com.moonlight_stream.Moonlight");
    qputenv("SDL_VIDEO_X11_WMCLASS", "com.moonlight_stream.Moonlight");

    // Register our C++ types for QML
    qmlRegisterType<ComputerModel>("ComputerModel", 1, 0, "ComputerModel");
    qmlRegisterType<AppModel>("AppModel", 1, 0, "AppModel");
    qmlRegisterUncreatableType<Session>("Session", 1, 0, "Session", "Session cannot be created from QML");
    qmlRegisterSingletonType<ComputerManager>("ComputerManager", 1, 0,
                                              "ComputerManager",
                                              [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                  return new ComputerManager(StreamingPreferences::get(qmlEngine));
                                              });
    qmlRegisterSingletonType<AutoUpdateChecker>("AutoUpdateChecker", 1, 0,
                                                "AutoUpdateChecker",
                                                [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                    return new AutoUpdateChecker();
                                                });
    qmlRegisterSingletonType<SystemProperties>("SystemProperties", 1, 0,
                                               "SystemProperties",
                                               [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                   return new SystemProperties();
                                               });
    qmlRegisterSingletonType<SdlGamepadKeyNavigation>("SdlGamepadKeyNavigation", 1, 0,
                                                      "SdlGamepadKeyNavigation",
                                                      [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                          return new SdlGamepadKeyNavigation(StreamingPreferences::get(qmlEngine));
                                                      });
    qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0,
                                                   "StreamingPreferences",
                                                   [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                       return StreamingPreferences::get(qmlEngine);
                                                   });
    qmlRegisterSingletonType<UsbPassthroughManager>("UsbPassthrough", 1, 0,
                                                    "UsbPassthroughManager",
                                                    [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                        return UsbPassthroughManager::get(qmlEngine);
                                                    });

    // Create the identity manager on the main thread
    IdentityManager::get();

    // We require the Material theme
    QQuickStyle::setStyle("Material");

    // Our icons are styled for a dark theme, so we do not allow the user to override this
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");

    // These are defaults that we allow the user to override
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_ACCENT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "Purple");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_VARIANT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_PRIMARY")) {
        // Qt 6.9 began to use a different shade of Material.Indigo when we use a dark theme
        // (which is all the time). The new color looks washed out, so manually specify the
        // old primary color unless the user overrides it themselves.
        qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#3F51B5");
    }

    QQmlApplicationEngine engine;
    QString initialView;
    bool hasGUI = true;
    bool exitAfterCommand = false;
    int commandExitCode = 0;

    switch (commandLineParserResult) {
    case GlobalCommandLineParser::NormalStartRequested:
        initialView = "qrc:/gui/PcView.qml";
        break;
    case GlobalCommandLineParser::StreamRequested:
        {
            initialView = "qrc:/gui/CliStartStreamSegue.qml";
            StreamingPreferences* preferences = StreamingPreferences::get();
            StreamCommandLineParser streamParser;
            streamParser.parse(app.arguments(), preferences);
            QString host    = streamParser.getHost();
            QString appName = streamParser.getAppName();
            auto launcher   = new CliStartStream::Launcher(host, appName, preferences, &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::QuitRequested:
        {
            initialView = "qrc:/gui/CliQuitStreamSegue.qml";
            QuitCommandLineParser quitParser;
            quitParser.parse(app.arguments());
            auto launcher = new CliQuitStream::Launcher(quitParser.getHost(), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::PairRequested:
        {
            initialView = "qrc:/gui/CliPair.qml";
            PairCommandLineParser pairParser;
            pairParser.parse(app.arguments());
            auto launcher = new CliPair::Launcher(pairParser.getHost(), pairParser.getPredefinedPin(), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::ListRequested:
        {
            ListCommandLineParser listParser;
            listParser.parse(app.arguments());
            auto launcher = new CliListApps::Launcher(listParser.getHost(), listParser, &app);
            launcher->execute(new ComputerManager(StreamingPreferences::get()));
            hasGUI = false;
            break;
        }
    case GlobalCommandLineParser::UsbLabListRequested:
        {
            commandExitCode = runUsbLabListCommand(app.arguments());
            exitAfterCommand = true;
            hasGUI = false;
            break;
        }
    case GlobalCommandLineParser::UsbLabInstallRequested:
        {
            commandExitCode = runUsbLabInstallCommand(app.arguments());
            exitAfterCommand = true;
            hasGUI = false;
            break;
        }
    case GlobalCommandLineParser::UsbLabExportRequested:
        {
            commandExitCode = runUsbLabExportCommand(app.arguments());
            exitAfterCommand = true;
            hasGUI = false;
            break;
        }
    case GlobalCommandLineParser::UsbLabTunnelRequested:
        {
            commandExitCode = runUsbLabTunnelCommand(app.arguments());
            exitAfterCommand = true;
            hasGUI = false;
            break;
        }
    }

    if (hasGUI) {
        engine.rootContext()->setContextProperty("initialView", initialView);
        engine.rootContext()->setContextProperty("runConfigChecks", commandLineParserResult == GlobalCommandLineParser::NormalStartRequested);

        // Load the main.qml file
        engine.load(QUrl(QStringLiteral("qrc:/gui/main.qml")));
        if (engine.rootObjects().isEmpty())
            return -1;
    }

    int err = exitAfterCommand ? commandExitCode : app.exec();

    // Give worker tasks time to properly exit. Fixes PendingQuitTask
    // sometimes freezing and blocking process exit.
    QThreadPool::globalInstance()->waitForDone(30000);

    // Restore the default logger for all libraries before shutting down ours
#if SDL_VERSION_ATLEAST(3, 0, 0)
    SDL_SetLogOutputFunction(SDL_GetDefaultLogOutputFunction(), nullptr);
#else
    SDL_LogSetOutputFunction(oldSdlLogFn, oldSdlLogUserdata);
#endif
    qInstallMessageHandler(nullptr);
#ifdef HAVE_FFMPEG
    av_log_set_callback(av_log_default_callback);
#endif

    // We should not be in async logging mode anymore
    Q_ASSERT(g_AsyncLoggingEnabled == 0);

    // Wait for pending log messages to be printed
    s_LoggerThread.waitForDone();

#ifdef Q_OS_WIN32
    // Without an explicit flush, console redirection for the list command
    // doesn't work reliably (sometimes the target file contains no text).
    fflush(stderr);
    fflush(stdout);
#endif

    return err;
}
