#include "usbpassthroughmanager.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTextStream>
#include <QVariantMap>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

#ifdef Q_OS_WIN32
#include <qt_windows.h>
#include <shellapi.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

namespace {

UsbPassthroughManager* s_Manager = nullptr;
constexpr quint16 kDefaultUsbipPort = 3240;
constexpr int kBalancedTunnelCopyBufferSize = 64 * 1024;
constexpr int kBalancedTunnelSocketBufferSize = 4 * 1024 * 1024;
constexpr auto kTunnelStartupRetryWindow = std::chrono::seconds(30);
constexpr auto kTunnelStartupRetryDelay = std::chrono::milliseconds(750);
constexpr auto kTunnelStartupWaitSlack = std::chrono::seconds(20);

struct UsbTransportConfig {
    QString profile = QStringLiteral("balanced");
    QString risk = QStringLiteral("standard");
    QString note;
    int socketBufferBytes = kBalancedTunnelSocketBufferSize;
    int copyBufferBytes = kBalancedTunnelCopyBufferSize;
    int readWaitMs = 5;
    int writeTimeoutMs = 1000;
    int writeDrainThresholdBytes = kBalancedTunnelSocketBufferSize / 2;
    bool lowDelay = true;
    bool prioritizeClientToHost = false;
};

struct UsbTunnelState {
    QString host;
    QString token;
    QString busId;
    quint16 hostPort = 0;
    quint16 exporterPort = kDefaultUsbipPort;
    UsbTransportConfig transport;
    std::atomic_bool stopRequested {false};
    std::thread thread;
    std::mutex startupMutex;
    std::condition_variable startupCv;
    bool startupComplete = false;
    bool connectedOnce = false;
    QString startupError;
};

std::mutex& tunnelMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<UsbTunnelState>>& tunnelStates()
{
    static std::vector<std::shared_ptr<UsbTunnelState>> states;
    return states;
}

#ifdef Q_OS_LINUX
std::mutex& exportServerMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unique_ptr<QProcess>& managedUsbipdProcess()
{
    static std::unique_ptr<QProcess> process;
    return process;
}
#endif

QString readTextFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    QTextStream stream(&file);
    return stream.readAll().trimmed();
}

QString driverNameForDevice(const QString& sysfsPath)
{
    QFileInfo driverInfo(sysfsPath + QStringLiteral("/driver"));
    if (!driverInfo.exists() || !driverInfo.isSymLink()) {
        return QString();
    }

    return QFileInfo(driverInfo.symLinkTarget()).fileName();
}

QString classCodeToLabel(const QString& classCode)
{
    const QString normalized = classCode.trimmed().toLower();
    if (normalized == QStringLiteral("01")) {
        return QStringLiteral("audio");
    }
    if (normalized == QStringLiteral("00")) {
        return QStringLiteral("interface");
    }
    if (normalized == QStringLiteral("02")) {
        return QStringLiteral("communications");
    }
    if (normalized == QStringLiteral("03")) {
        return QStringLiteral("hid");
    }
    if (normalized == QStringLiteral("07")) {
        return QStringLiteral("printer");
    }
    if (normalized == QStringLiteral("08")) {
        return QStringLiteral("mass_storage");
    }
    if (normalized == QStringLiteral("0a")) {
        return QStringLiteral("cdc_data");
    }
    if (normalized == QStringLiteral("0b")) {
        return QStringLiteral("smart_card");
    }
    if (normalized == QStringLiteral("0e")) {
        return QStringLiteral("video");
    }
    if (normalized == QStringLiteral("0f")) {
        return QStringLiteral("personal_healthcare");
    }
    if (normalized == QStringLiteral("e0")) {
        return QStringLiteral("wireless_controller");
    }
    if (normalized == QStringLiteral("ef")) {
        return QStringLiteral("miscellaneous");
    }
    if (normalized == QStringLiteral("ff")) {
        return QStringLiteral("vendor_specific");
    }

    return normalized.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("class_") + normalized;
}

QStringList interfaceClassesForDevice(const QString& sysfsPath, const QString& busId)
{
    QStringList interfaces;
    QDir deviceDir(sysfsPath);
    const QString prefix = busId + QStringLiteral(":");
    const QStringList entries = deviceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& entry : entries) {
        if (!entry.startsWith(prefix)) {
            continue;
        }

        const QString classCode = readTextFile(deviceDir.filePath(entry + QStringLiteral("/bInterfaceClass")));
        const QString label = classCodeToLabel(classCode);
        if (!interfaces.contains(label)) {
            interfaces.append(label);
        }
    }

    interfaces.sort();
    return interfaces;
}

int parseUsbInteger(QString value, int fallback = -1)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        return fallback;
    }

    bool ok = false;
    int base = 10;
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        value.remove(0, 2);
        base = 16;
    }
    else if (value.contains(QRegularExpression(QStringLiteral("[a-fA-F]")))) {
        base = 16;
    }

    const int result = value.toInt(&ok, base);
    return ok ? result : fallback;
}

QString normalizedEndpointTransferType(QString value)
{
    value = value.trimmed().toLower();
    value.replace(QLatin1Char('-'), QLatin1Char('_'));
    value.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (value == QStringLiteral("iso")) {
        return QStringLiteral("isochronous");
    }
    if (value == QStringLiteral("intr")) {
        return QStringLiteral("interrupt");
    }
    return value;
}

QString endpointTransferTypeFromAttributes(const QString& typeText, const QString& attributesText)
{
    const QString type = normalizedEndpointTransferType(typeText);
    if (!type.isEmpty()) {
        return type;
    }

    const int attributes = parseUsbInteger(attributesText, -1);
    if (attributes < 0) {
        return QString();
    }

    switch (attributes & 0x3) {
    case 0:
        return QStringLiteral("control");
    case 1:
        return QStringLiteral("isochronous");
    case 2:
        return QStringLiteral("bulk");
    case 3:
        return QStringLiteral("interrupt");
    default:
        return QString();
    }
}

void appendUniqueString(QStringList& values, const QString& value)
{
    if (!value.isEmpty() && !values.contains(value)) {
        values.append(value);
    }
}

struct UsbEndpointMetadata {
    QVariantList endpoints;
    QStringList transferTypes;
    QVariantMap endpointCounts;
    int maxPacketSize = 0;
};

UsbEndpointMetadata endpointMetadataForLinuxDevice(const QString& sysfsPath, const QString& busId)
{
    UsbEndpointMetadata metadata;
    int bulkCount = 0;
    int interruptCount = 0;
    int isochronousCount = 0;
    int controlCount = 0;

    QDir deviceDir(sysfsPath);
    const QString prefix = busId + QStringLiteral(":");
    const QStringList interfaces = deviceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& interfaceEntry : interfaces) {
        if (!interfaceEntry.startsWith(prefix)) {
            continue;
        }

        QDir interfaceDir(deviceDir.filePath(interfaceEntry));
        const QString interfaceClass = classCodeToLabel(readTextFile(interfaceDir.filePath(QStringLiteral("bInterfaceClass"))));
        const QString interfaceSubClass = readTextFile(interfaceDir.filePath(QStringLiteral("bInterfaceSubClass"))).toLower();
        const QString interfaceProtocol = readTextFile(interfaceDir.filePath(QStringLiteral("bInterfaceProtocol"))).toLower();
        const QStringList endpointEntries = interfaceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& endpointEntry : endpointEntries) {
            if (!endpointEntry.startsWith(QStringLiteral("ep_"))) {
                continue;
            }

            const QString endpointPath = interfaceDir.filePath(endpointEntry);
            const QString address = readTextFile(endpointPath + QStringLiteral("/bEndpointAddress"));
            const QString attributes = readTextFile(endpointPath + QStringLiteral("/bmAttributes"));
            const QString type = endpointTransferTypeFromAttributes(readTextFile(endpointPath + QStringLiteral("/type")), attributes);
            const int maxPacketSize = parseUsbInteger(readTextFile(endpointPath + QStringLiteral("/wMaxPacketSize")), 0);

            if (type == QStringLiteral("bulk")) {
                ++bulkCount;
            }
            else if (type == QStringLiteral("interrupt")) {
                ++interruptCount;
            }
            else if (type == QStringLiteral("isochronous")) {
                ++isochronousCount;
            }
            else if (type == QStringLiteral("control")) {
                ++controlCount;
            }
            appendUniqueString(metadata.transferTypes, type);
            metadata.maxPacketSize = std::max(metadata.maxPacketSize, maxPacketSize);

            if (metadata.endpoints.size() < 32) {
                QVariantMap endpoint;
                endpoint[QStringLiteral("interface")] = interfaceEntry;
                endpoint[QStringLiteral("interfaceClass")] = interfaceClass;
                endpoint[QStringLiteral("interfaceSubClass")] = interfaceSubClass;
                endpoint[QStringLiteral("interfaceProtocol")] = interfaceProtocol;
                endpoint[QStringLiteral("address")] = address.isEmpty() ? endpointEntry.mid(3) : address;
                endpoint[QStringLiteral("type")] = type;
                endpoint[QStringLiteral("maxPacketSize")] = maxPacketSize;
                endpoint[QStringLiteral("interval")] = readTextFile(endpointPath + QStringLiteral("/bInterval"));
                metadata.endpoints.append(endpoint);
            }
        }
    }

    metadata.transferTypes.sort();
    metadata.endpointCounts[QStringLiteral("bulk")] = bulkCount;
    metadata.endpointCounts[QStringLiteral("interrupt")] = interruptCount;
    metadata.endpointCounts[QStringLiteral("isochronous")] = isochronousCount;
    metadata.endpointCounts[QStringLiteral("control")] = controlCount;
    return metadata;
}

QString deriveDeviceClass(const QString& deviceClass, const QStringList& interfaces)
{
    const QString label = classCodeToLabel(deviceClass);
    if (label != QStringLiteral("interface")) {
        return label;
    }
    if (interfaces.isEmpty()) {
        return QStringLiteral("unknown");
    }
    if (interfaces.size() == 1) {
        return interfaces.first();
    }
    return QStringLiteral("composite");
}

QString normalizedUsbClassLabel(QString value)
{
    value = value.trimmed().toLower();
    value.replace(QLatin1Char('-'), QLatin1Char('_'));
    value.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (value.startsWith(QStringLiteral("0x"))) {
        value.remove(0, 2);
    }
    while (value.size() > 1 && value.startsWith(QLatin1Char('0'))) {
        value.remove(0, 1);
    }
    return value;
}

bool isStorageClassLabel(const QString& value)
{
    const QString normalized = normalizedUsbClassLabel(value);
    return normalized == QStringLiteral("8") ||
           normalized == QStringLiteral("08") ||
           normalized == QStringLiteral("storage") ||
           normalized == QStringLiteral("mass_storage") ||
           normalized == QStringLiteral("massstorage") ||
           normalized == QStringLiteral("usb_mass_storage");
}

bool textSuggestsStorageClass(QString value)
{
    value = value.trimmed().toLower();
    return value.contains(QStringLiteral("mass storage")) ||
           value.contains(QStringLiteral("storage device")) ||
           value.contains(QStringLiteral("disk drive")) ||
           value.contains(QStringLiteral("flash drive")) ||
           value.contains(QStringLiteral("thumb drive"));
}

bool isStorageClassDevice(const QVariantMap& device)
{
    if (isStorageClassLabel(device.value(QStringLiteral("deviceClass")).toString())) {
        return true;
    }

    const QStringList interfaces = device.value(QStringLiteral("interfaces")).toStringList();
    for (const QString& interfaceClass : interfaces) {
        if (isStorageClassLabel(interfaceClass)) {
            return true;
        }
    }

    return textSuggestsStorageClass(device.value(QStringLiteral("description")).toString()) ||
           textSuggestsStorageClass(device.value(QStringLiteral("product")).toString());
}

bool hasClassLabel(const QVariantMap& device, const QString& label)
{
    const auto matches = [&label](const QString& value) {
        const QString normalized = normalizedUsbClassLabel(value);
        if (normalized == label) {
            return true;
        }
        if (label == QStringLiteral("audio")) {
            return normalized == QStringLiteral("1");
        }
        if (label == QStringLiteral("hid")) {
            return normalized == QStringLiteral("3");
        }
        if (label == QStringLiteral("printer")) {
            return normalized == QStringLiteral("7");
        }
        if (label == QStringLiteral("smart_card")) {
            return normalized == QStringLiteral("b");
        }
        if (label == QStringLiteral("video")) {
            return normalized == QStringLiteral("e");
        }
        if (label == QStringLiteral("wireless_controller")) {
            return normalized == QStringLiteral("e0");
        }
        return false;
    };

    if (matches(device.value(QStringLiteral("deviceClass")).toString())) {
        return true;
    }

    const QStringList interfaces = device.value(QStringLiteral("interfaces")).toStringList();
    for (const QString& interfaceClass : interfaces) {
        if (matches(interfaceClass)) {
            return true;
        }
    }
    return false;
}

bool hasTransferType(const QVariantMap& device, const QString& transferType)
{
    const QStringList transferTypes = device.value(QStringLiteral("transferTypes")).toStringList();
    for (const QString& value : transferTypes) {
        if (normalizedEndpointTransferType(value) == transferType) {
            return true;
        }
    }
    return false;
}

UsbTransportConfig transportConfigForProfile(const QString& profile)
{
    UsbTransportConfig config;
    config.profile = profile.isEmpty() ? QStringLiteral("balanced") : profile;

    if (config.profile == QStringLiteral("isochronous")) {
        config.risk = QStringLiteral("experimental");
        config.note = QStringLiteral("Video/audio-style USB devices use larger socket buffers and shorter polling to reduce streaming stalls.");
        config.socketBufferBytes = 8 * 1024 * 1024;
        config.copyBufferBytes = 256 * 1024;
        config.readWaitMs = 1;
        config.writeTimeoutMs = 2500;
        config.writeDrainThresholdBytes = 6 * 1024 * 1024;
        config.prioritizeClientToHost = true;
    }
    else if (config.profile == QStringLiteral("bulk")) {
        config.risk = QStringLiteral("standard");
        config.note = QStringLiteral("Bulk-style USB devices use larger buffers for sustained transfers.");
        config.socketBufferBytes = 8 * 1024 * 1024;
        config.copyBufferBytes = 256 * 1024;
        config.readWaitMs = 2;
        config.writeTimeoutMs = 3000;
        config.writeDrainThresholdBytes = 6 * 1024 * 1024;
    }
    else if (config.profile == QStringLiteral("low-latency")) {
        config.risk = QStringLiteral("stable");
        config.note = QStringLiteral("HID/interrupt-style USB devices use smaller buffers and shorter waits for input latency.");
        config.socketBufferBytes = 1024 * 1024;
        config.copyBufferBytes = 16 * 1024;
        config.readWaitMs = 1;
        config.writeTimeoutMs = 500;
        config.writeDrainThresholdBytes = 256 * 1024;
    }
    else {
        config.profile = QStringLiteral("balanced");
        config.risk = QStringLiteral("standard");
        config.note = QStringLiteral("Balanced USB transport settings are used when endpoint requirements are mixed or unknown.");
    }

    return config;
}

UsbTransportConfig transportConfigForDevice(const QVariantMap& device)
{
    QString profile = device.value(QStringLiteral("transportProfile")).toString();
    if (profile.isEmpty()) {
        const bool isAudioVideo = hasClassLabel(device, QStringLiteral("audio")) ||
                                  hasClassLabel(device, QStringLiteral("video"));
        if (isAudioVideo || hasTransferType(device, QStringLiteral("isochronous"))) {
            profile = QStringLiteral("isochronous");
        }
        else if (isStorageClassDevice(device) ||
                 hasClassLabel(device, QStringLiteral("printer")) ||
                 hasClassLabel(device, QStringLiteral("smart_card")) ||
                 hasTransferType(device, QStringLiteral("bulk"))) {
            profile = QStringLiteral("bulk");
        }
        else if (hasClassLabel(device, QStringLiteral("hid")) ||
                 hasClassLabel(device, QStringLiteral("wireless_controller")) ||
                 hasTransferType(device, QStringLiteral("interrupt"))) {
            profile = QStringLiteral("low-latency");
        }
        else {
            profile = QStringLiteral("balanced");
        }
    }

    return transportConfigForProfile(profile);
}

void applyUsbTransportPolicy(QVariantMap& device)
{
    const UsbTransportConfig transport = transportConfigForDevice(device);
    device[QStringLiteral("transportProfile")] = transport.profile;
    device[QStringLiteral("transportRisk")] = transport.risk;
    device[QStringLiteral("transportNote")] = transport.note;
    device[QStringLiteral("transportSocketBufferBytes")] = transport.socketBufferBytes;
    device[QStringLiteral("transportCopyBufferBytes")] = transport.copyBufferBytes;
    device[QStringLiteral("transportReadWaitMs")] = transport.readWaitMs;
    device[QStringLiteral("transportWriteTimeoutMs")] = transport.writeTimeoutMs;
    device[QStringLiteral("transportWriteDrainThresholdBytes")] = transport.writeDrainThresholdBytes;
    device[QStringLiteral("transportLowDelay")] = transport.lowDelay;
    device[QStringLiteral("transportPrioritizeClientToHost")] = transport.prioritizeClientToHost;
    device[QStringLiteral("transportExperimental")] = transport.risk == QStringLiteral("experimental");
}

QString storageClassBlockReason()
{
    return QStringLiteral("Storage-class USB devices are blocked by default for this USB passthrough MVP.");
}

void applyUsbSafetyPolicy(QVariantMap& device)
{
    const bool storageClass = isStorageClassDevice(device);
    device[QStringLiteral("storageClass")] = storageClass;
    device[QStringLiteral("blocked")] = storageClass;
    if (storageClass) {
        device[QStringLiteral("blockReason")] = storageClassBlockReason();
    }
    applyUsbTransportPolicy(device);
}

QString blockedReasonForBusId(const QVariantList& devices, const QString& busId)
{
    for (const QVariant& deviceVariant : devices) {
        const QVariantMap device = deviceVariant.toMap();
        if (device.value(QStringLiteral("busid")).toString() != busId) {
            continue;
        }
        if (device.value(QStringLiteral("blocked")).toBool()) {
            const QString reason = device.value(QStringLiteral("blockReason")).toString();
            return reason.isEmpty() ? storageClassBlockReason() : reason;
        }
        return QString();
    }
    return QString();
}

bool deviceAttachedForBusId(const QVariantList& devices, const QString& busId)
{
    for (const QVariant& deviceVariant : devices) {
        const QVariantMap device = deviceVariant.toMap();
        if (device.value(QStringLiteral("busid")).toString() != busId) {
            continue;
        }

        return device.value(QStringLiteral("attached")).toBool() ||
               device.value(QStringLiteral("state")).toString() == QStringLiteral("attached");
    }

    return false;
}

bool safeTunnelToken(const QString& value)
{
    if (value.size() < 16 || value.size() > 128) {
        return false;
    }
    for (const QChar& ch : value) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
            return false;
        }
    }
    return true;
}

void configureTunnelSocket(QTcpSocket& socket, const UsbTransportConfig& transport)
{
    socket.setSocketOption(QAbstractSocket::LowDelayOption, transport.lowDelay ? 1 : 0);
    socket.setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, transport.socketBufferBytes);
    socket.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, transport.socketBufferBytes);
}

qint64 copyAvailable(QTcpSocket& from, QTcpSocket& to, const UsbTransportConfig& transport)
{
    qint64 copied = 0;
    while (from.bytesAvailable() > 0) {
        const QByteArray data = from.read(qMin<qint64>(from.bytesAvailable(), transport.copyBufferBytes));
        if (data.isEmpty()) {
            return copied;
        }

        qint64 written = 0;
        while (written < data.size()) {
            const qint64 result = to.write(data.constData() + written, data.size() - written);
            if (result < 0) {
                qWarning() << "USB passthrough tunnel write failed" << to.errorString();
                return -1;
            }
            written += result;
            copied += result;
            if (written < data.size() || to.bytesToWrite() > transport.writeDrainThresholdBytes) {
                if (!to.waitForBytesWritten(transport.writeTimeoutMs)) {
                    qWarning() << "USB passthrough tunnel write timed out" << to.errorString();
                    return -1;
                }
            }
        }
    }
    return copied;
}

void sleepUntilStopRequested(const std::shared_ptr<UsbTunnelState>& state, std::chrono::milliseconds delay)
{
    const auto sleepUntil = std::chrono::steady_clock::now() + delay;
    while (!state->stopRequested.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < sleepUntil) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void reportTunnelStartup(const std::shared_ptr<UsbTunnelState>& state, bool connected, const QString& error)
{
    {
        std::lock_guard<std::mutex> lock(state->startupMutex);
        if (state->startupComplete) {
            return;
        }

        state->connectedOnce = connected;
        state->startupError = error;
        state->startupComplete = true;
    }

    state->startupCv.notify_all();
}

void runTunnelThread(std::shared_ptr<UsbTunnelState> state)
{
    const auto retryDeadline = std::chrono::steady_clock::now() + kTunnelStartupRetryWindow;
    unsigned attempt = 0;
    bool tunnelConnectedOnce = false;

    while (!state->stopRequested.load(std::memory_order_acquire)) {
        ++attempt;
        QTcpSocket hostSocket;
        QTcpSocket exporterSocket;

        hostSocket.connectToHost(state->host, state->hostPort);
        if (!hostSocket.waitForConnected(5000)) {
            qWarning() << "USB passthrough tunnel failed to connect to host"
                       << state->host << state->hostPort << "for" << state->busId
                       << "attempt" << attempt << hostSocket.errorString();
        }
        else {
            configureTunnelSocket(hostSocket, state->transport);
            const QByteArray handshake =
                QByteArrayLiteral("VPUSB1 ") +
                state->token.toLatin1() +
                QByteArrayLiteral(" ") +
                state->busId.toLatin1() +
                QByteArrayLiteral("\n");
            hostSocket.write(handshake);
            if (!hostSocket.waitForBytesWritten(5000)) {
                qWarning() << "USB passthrough tunnel failed to send host handshake for"
                           << state->busId << "attempt" << attempt << hostSocket.errorString();
            }
            else {
                exporterSocket.connectToHost(QStringLiteral("127.0.0.1"), state->exporterPort);
                if (!exporterSocket.waitForConnected(5000)) {
                    qWarning() << "USB passthrough tunnel failed to connect to local exporter"
                               << state->exporterPort << "for" << state->busId
                               << "attempt" << attempt << exporterSocket.errorString();
                }
                else {
                    configureTunnelSocket(exporterSocket, state->transport);
                    if (!tunnelConnectedOnce) {
                        tunnelConnectedOnce = true;
                        reportTunnelStartup(state, true, QString());
                    }
                    qInfo() << "USB passthrough tunnel connected for" << state->busId
                            << "via" << state->host << state->hostPort
                            << "profile" << state->transport.profile;

                    QElapsedTimer statsTimer;
                    statsTimer.start();
                    qint64 hostToClientBytes = 0;
                    qint64 clientToHostBytes = 0;
                    bool copyFailed = false;
                    const auto copyDirection = [&](QTcpSocket& from, QTcpSocket& to, qint64& total) {
                        const qint64 result = copyAvailable(from, to, state->transport);
                        if (result < 0) {
                            copyFailed = true;
                            hostSocket.disconnectFromHost();
                            exporterSocket.disconnectFromHost();
                            return false;
                        }
                        total += result;
                        return result > 0;
                    };
                    const auto drainReadyData = [&]() {
                        bool copied = false;
                        if (state->transport.prioritizeClientToHost) {
                            if (exporterSocket.bytesAvailable() > 0) {
                                copied = copyDirection(exporterSocket, hostSocket, clientToHostBytes) || copied;
                            }
                            if (hostSocket.bytesAvailable() > 0) {
                                copied = copyDirection(hostSocket, exporterSocket, hostToClientBytes) || copied;
                            }
                        }
                        else {
                            if (hostSocket.bytesAvailable() > 0) {
                                copied = copyDirection(hostSocket, exporterSocket, hostToClientBytes) || copied;
                            }
                            if (exporterSocket.bytesAvailable() > 0) {
                                copied = copyDirection(exporterSocket, hostSocket, clientToHostBytes) || copied;
                            }
                        }
                        return copied;
                    };
                    while (!state->stopRequested.load(std::memory_order_acquire) &&
                           hostSocket.state() == QAbstractSocket::ConnectedState &&
                           exporterSocket.state() == QAbstractSocket::ConnectedState) {
                        bool copied = drainReadyData();
                        if (!copied && !copyFailed) {
                            QTcpSocket& firstSocket = state->transport.prioritizeClientToHost ? exporterSocket : hostSocket;
                            QTcpSocket& secondSocket = state->transport.prioritizeClientToHost ? hostSocket : exporterSocket;
                            qint64& firstBytes = state->transport.prioritizeClientToHost ? clientToHostBytes : hostToClientBytes;
                            qint64& secondBytes = state->transport.prioritizeClientToHost ? hostToClientBytes : clientToHostBytes;
                            QTcpSocket& firstTarget = state->transport.prioritizeClientToHost ? hostSocket : exporterSocket;
                            QTcpSocket& secondTarget = state->transport.prioritizeClientToHost ? exporterSocket : hostSocket;

                            if (firstSocket.waitForReadyRead(state->transport.readWaitMs)) {
                                copied = copyDirection(firstSocket, firstTarget, firstBytes) || copied;
                            }
                            if (!copyFailed && secondSocket.bytesAvailable() > 0) {
                                copied = copyDirection(secondSocket, secondTarget, secondBytes) || copied;
                            }
                            if (!copied && !copyFailed && secondSocket.waitForReadyRead(state->transport.readWaitMs)) {
                                copyDirection(secondSocket, secondTarget, secondBytes);
                            }
                        }
                        if (copyFailed) {
                            break;
                        }
                        if (statsTimer.elapsed() >= 5000) {
                            qInfo() << "USB passthrough tunnel stats for" << state->busId
                                    << "profile" << state->transport.profile
                                    << "hostToClientBytes" << hostToClientBytes
                                    << "clientToHostBytes" << clientToHostBytes;
                            statsTimer.restart();
                        }
                    }

                    if (state->stopRequested.load(std::memory_order_acquire)) {
                        qInfo() << "USB passthrough tunnel stopped for" << state->busId;
                        return;
                    }

                    qInfo() << "USB passthrough tunnel connection closed for" << state->busId
                            << "profile" << state->transport.profile
                            << "hostToClientBytes" << hostToClientBytes
                            << "clientToHostBytes" << clientToHostBytes
                            << "- reconnecting while the stream remains active";
                }
            }
        }

        if (!tunnelConnectedOnce && std::chrono::steady_clock::now() >= retryDeadline) {
            break;
        }

        sleepUntilStopRequested(state, kTunnelStartupRetryDelay);
    }

    if (!state->stopRequested.load(std::memory_order_acquire) && !tunnelConnectedOnce) {
        reportTunnelStartup(state, false, QStringLiteral("USB passthrough tunnel startup retries exhausted for %1.").arg(state->busId));
        qWarning() << "USB passthrough tunnel startup retries exhausted for" << state->busId;
    }
}

bool looksLikeUsbBusId(const QString& value)
{
    if (!value.contains(QLatin1Char('-')) || value.startsWith(QLatin1Char('-')) || value.endsWith(QLatin1Char('-'))) {
        return false;
    }

    for (const QChar& ch : value) {
        if (!ch.isDigit() && ch != QLatin1Char('-') && ch != QLatin1Char('.')) {
            return false;
        }
    }
    return true;
}

QString hashSerial(const QString& serial)
{
    if (serial.isEmpty()) {
        return QString();
    }

    return QString::fromLatin1(QCryptographicHash::hash(serial.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString hashIdentifier(const QString& value)
{
    if (value.isEmpty()) {
        return QString();
    }

    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString approvalComponent(QString value)
{
    value = value.trimmed().toLower();
    if (value.isEmpty()) {
        return QStringLiteral("unknown");
    }

    QString normalized;
    normalized.reserve(value.size());
    for (const QChar& ch : value) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('.') || ch == QLatin1Char('_') || ch == QLatin1Char('-')) {
            normalized.append(ch);
        }
        else {
            normalized.append(QLatin1Char('_'));
        }
    }
    return normalized;
}

void applyApprovalIdentity(QVariantMap& device, const QString& backend)
{
    const QString busId = device.value(QStringLiteral("busid")).toString();
    const QString vid = device.value(QStringLiteral("vid")).toString();
    const QString pid = device.value(QStringLiteral("pid")).toString();
    const QString serialHash = device.value(QStringLiteral("serialHash")).toString();
    const bool stable = !serialHash.isEmpty() && !vid.isEmpty() && !pid.isEmpty();

    QStringList parts;
    parts << QStringLiteral("usb-approval-v1") << approvalComponent(backend);
    if (stable) {
        parts << approvalComponent(vid)
              << approvalComponent(pid)
              << QStringLiteral("serial")
              << approvalComponent(serialHash);
    }
    else {
        parts << QStringLiteral("bus")
              << approvalComponent(busId.isEmpty() ? device.value(QStringLiteral("id")).toString() : busId)
              << approvalComponent(vid)
              << approvalComponent(pid);
    }

    device[QStringLiteral("approvalId")] = parts.join(QLatin1Char(':'));
    device[QStringLiteral("approvalStable")] = stable;
}

QString valueToString(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString();
}

bool valueToBool(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    return value.isBool() && value.toBool();
}

bool hasNonNullValue(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    return !value.isUndefined() && !value.isNull();
}

#ifdef Q_OS_WIN32
QString quoteWindowsArgument(const QString& argument)
{
    if (argument.isEmpty()) {
        return QStringLiteral("\"\"");
    }

    bool needsQuotes = false;
    for (const QChar& ch : argument) {
        if (ch.isSpace() || ch == QLatin1Char('"') || ch == QLatin1Char('\\')) {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return argument;
    }

    QString quoted = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar& ch : argument) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            quoted += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            quoted += ch;
            backslashes = 0;
            continue;
        }
        quoted += QString(backslashes, QLatin1Char('\\'));
        quoted += ch;
        backslashes = 0;
    }
    quoted += QString(backslashes * 2, QLatin1Char('\\'));
    quoted += QLatin1Char('"');
    return quoted;
}

QString windowsArgumentString(const QStringList& arguments)
{
    QStringList quoted;
    quoted.reserve(arguments.size());
    for (const QString& argument : arguments) {
        quoted.append(quoteWindowsArgument(argument));
    }
    return quoted.join(QLatin1Char(' '));
}

QString windowsLastErrorMessage(DWORD errorCode)
{
    LPWSTR messageBuffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);

    QString message;
    if (size > 0 && messageBuffer) {
        message = QString::fromWCharArray(messageBuffer, static_cast<int>(size)).trimmed();
        LocalFree(messageBuffer);
    }
    if (message.isEmpty()) {
        message = QStringLiteral("Windows error %1").arg(errorCode);
    }
    return message;
}

QString windowsServiceStateToString(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED:
        return QStringLiteral("stopped");
    case SERVICE_START_PENDING:
        return QStringLiteral("start_pending");
    case SERVICE_STOP_PENDING:
        return QStringLiteral("stop_pending");
    case SERVICE_RUNNING:
        return QStringLiteral("running");
    case SERVICE_CONTINUE_PENDING:
        return QStringLiteral("continue_pending");
    case SERVICE_PAUSE_PENDING:
        return QStringLiteral("pause_pending");
    case SERVICE_PAUSED:
        return QStringLiteral("paused");
    default:
        return QStringLiteral("unknown");
    }
}
#endif

QString hardwareIdPart(const QString& instanceId, const QString& name)
{
    static const QRegularExpression hardwareIdRegex(QStringLiteral("VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})"));
    const QRegularExpressionMatch match = hardwareIdRegex.match(instanceId);
    if (!match.hasMatch()) {
        return QString();
    }

    if (name == QStringLiteral("vid")) {
        return match.captured(1).toLower();
    }
    if (name == QStringLiteral("pid")) {
        return match.captured(2).toLower();
    }
    return QString();
}

} // namespace

UsbPassthroughManager::UsbPassthroughManager(QObject* parent)
    : QObject(parent)
    , m_DependenciesReady(false)
    , m_UsbipCoreLoaded(false)
    , m_UsbipHostLoaded(false)
    , m_Backend(QStringLiteral("unsupported"))
    , m_QmlEngine(nullptr)
{
    refresh();
}

UsbPassthroughManager* UsbPassthroughManager::get(QQmlEngine* qmlEngine)
{
    if (!s_Manager) {
        s_Manager = new UsbPassthroughManager();
    }
    if (qmlEngine && !s_Manager->m_QmlEngine) {
        s_Manager->m_QmlEngine = qmlEngine;
    }
    return s_Manager;
}

bool UsbPassthroughManager::isSupported() const
{
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN32)
    return true;
#else
    return false;
#endif
}

QString UsbPassthroughManager::backend() const
{
    return m_Backend;
}

bool UsbPassthroughManager::dependenciesReady() const
{
    return m_DependenciesReady;
}

QString UsbPassthroughManager::statusMessage() const
{
    return m_StatusMessage;
}

QString UsbPassthroughManager::lastError() const
{
    return m_LastError;
}

QString UsbPassthroughManager::usbipPath() const
{
    return m_UsbipPath;
}

QString UsbPassthroughManager::usbipdPath() const
{
    return m_UsbipdPath;
}

QString UsbPassthroughManager::usbipdServiceState() const
{
    return m_UsbipdServiceState;
}

bool UsbPassthroughManager::usbipCoreLoaded() const
{
    return m_UsbipCoreLoaded;
}

bool UsbPassthroughManager::usbipHostLoaded() const
{
    return m_UsbipHostLoaded;
}

QVariantList UsbPassthroughManager::devices() const
{
    return m_Devices;
}

QVariantMap UsbPassthroughManager::deviceErrors() const
{
    return m_DeviceErrors;
}

void UsbPassthroughManager::refresh()
{
#ifdef Q_OS_LINUX
    m_Backend = QStringLiteral("linux-usbip");
    m_UsbipPath = QStandardPaths::findExecutable(QStringLiteral("usbip"));
    m_UsbipdPath = QStandardPaths::findExecutable(QStringLiteral("usbipd"));
    m_UsbipdServiceState.clear();
    m_UsbipCoreLoaded = QFileInfo::exists(QStringLiteral("/sys/module/usbip_core"));
    m_UsbipHostLoaded = QFileInfo::exists(QStringLiteral("/sys/module/usbip_host"));
    m_DependenciesReady = !m_UsbipPath.isEmpty() && !m_UsbipdPath.isEmpty() && m_UsbipCoreLoaded && m_UsbipHostLoaded;
    m_Devices = enumerateLinuxDevices();

    if (m_DependenciesReady) {
        m_StatusMessage = tr("USB/IP is ready.");
    }
    else if (m_UsbipPath.isEmpty() || m_UsbipdPath.isEmpty()) {
        m_StatusMessage = tr("USB/IP tools were not found. Install the usbip package for this distribution.");
    }
    else if (!m_UsbipCoreLoaded || !m_UsbipHostLoaded) {
        m_StatusMessage = tr("USB/IP kernel modules are not loaded. Load usbip-core and usbip-host before binding devices.");
    }
    else {
        m_StatusMessage = tr("USB/IP is not ready.");
    }
#elif defined(Q_OS_WIN32)
    m_Backend = QStringLiteral("windows-usbipd-win");
    m_UsbipPath.clear();
    m_UsbipCoreLoaded = false;
    m_UsbipHostLoaded = false;
    m_UsbipdPath = resolveWindowsUsbipdPath();
    m_UsbipdServiceState = queryWindowsUsbipdServiceState();
    const bool serviceReady =
        m_UsbipdServiceState.isEmpty() ||
        m_UsbipdServiceState == QStringLiteral("running") ||
        m_UsbipdServiceState == QStringLiteral("unknown");
    m_DependenciesReady = !m_UsbipdPath.isEmpty() && serviceReady;
    m_Devices = m_DependenciesReady ? enumerateWindowsDevices() : QVariantList();

    if (m_UsbipdPath.isEmpty()) {
        m_StatusMessage = tr("usbipd-win was not found. Install usbipd-win to enable Windows USB passthrough.");
    }
    else if (m_UsbipdServiceState == QStringLiteral("missing")) {
        m_StatusMessage = tr("usbipd-win service was not found. Repair usbipd-win, then refresh USB passthrough.");
    }
    else if (!serviceReady) {
        m_StatusMessage = tr("usbipd-win service is %1. Start or repair usbipd-win, then refresh USB passthrough.").arg(m_UsbipdServiceState);
    }
    else if (!m_DependenciesReady) {
        m_StatusMessage = tr("usbipd-win is not ready.");
    }
    else {
        m_StatusMessage = tr("usbipd-win is ready.");
    }
#else
    m_Backend = QStringLiteral("unsupported");
    m_UsbipPath.clear();
    m_UsbipdPath.clear();
    m_UsbipdServiceState.clear();
    m_UsbipCoreLoaded = false;
    m_UsbipHostLoaded = false;
    m_DependenciesReady = false;
    m_Devices.clear();
    m_StatusMessage = tr("USB passthrough is not supported on this client OS yet.");
#endif

    pruneDeviceErrors();
    emit statusChanged();
    emit devicesChanged();
}

bool UsbPassthroughManager::startExportServer()
{
#ifdef Q_OS_LINUX
    if (!m_DependenciesReady) {
        setLastError(tr("USB/IP is not ready. Refresh status and check the dependency message."));
        return false;
    }
    if (linuxUsbipdResponds()) {
        setLastError(QString());
        return true;
    }

    return startManagedLinuxUsbipd();
#elif defined(Q_OS_WIN32)
    if (!m_DependenciesReady) {
        setLastError(tr("usbipd-win is not ready."));
        return false;
    }
    return true;
#else
    setLastError(tr("USB passthrough export is not supported on this client OS yet."));
    return false;
#endif
}

void UsbPassthroughManager::stopExportServer()
{
#ifdef Q_OS_LINUX
    std::lock_guard<std::mutex> lock(exportServerMutex());
    auto& process = managedUsbipdProcess();
    if (!process || process->state() == QProcess::NotRunning) {
        return;
    }

    process->terminate();
    if (!process->waitForFinished(5000)) {
        process->kill();
        process->waitForFinished(2000);
    }

    process.reset();
    m_StatusMessage = tr("USB/IP exporter stopped.");
    setLastError(QString());
    emit statusChanged();
#endif
}

bool UsbPassthroughManager::bindDevice(const QString& busId)
{
#if defined(Q_OS_LINUX)
    if (!validateBusId(busId)) {
        return false;
    }
    const QString blockReason = blockedReasonForBusId(m_Devices, busId);
    if (!blockReason.isEmpty()) {
        setLastError(blockReason);
        setDeviceError(busId, blockReason);
        return false;
    }
    if (!m_DependenciesReady) {
        const QString error = tr("USB/IP is not ready. Refresh status and check the dependency message.");
        setLastError(error);
        setDeviceError(busId, error);
        return false;
    }

    const bool ok = runUsbipCommand({QStringLiteral("bind"), QStringLiteral("-b"), busId});
    refresh();
    setDeviceError(busId, ok ? QString() : m_LastError);
    return ok;
#elif defined(Q_OS_WIN32)
    if (!validateBusId(busId)) {
        return false;
    }
    const QString blockReason = blockedReasonForBusId(m_Devices, busId);
    if (!blockReason.isEmpty()) {
        setLastError(blockReason);
        setDeviceError(busId, blockReason);
        return false;
    }
    if (m_UsbipdPath.isEmpty()) {
        const QString error = tr("usbipd-win was not found.");
        setLastError(error);
        setDeviceError(busId, error);
        return false;
    }

    const bool ok = runUsbipdCommand({QStringLiteral("bind"), QStringLiteral("--busid"), busId});
    refresh();
    setDeviceError(busId, ok ? QString() : m_LastError);
    return ok;
#else
    Q_UNUSED(busId)
    setLastError(tr("USB passthrough binding is not supported on this client OS yet."));
    return false;
#endif
}

bool UsbPassthroughManager::detachDevice(const QString& busId)
{
#if defined(Q_OS_WIN32)
    if (!validateBusId(busId)) {
        return false;
    }
    if (m_UsbipdPath.isEmpty()) {
        const QString error = tr("usbipd-win was not found.");
        setLastError(error);
        setDeviceError(busId, error);
        return false;
    }

    refresh();
    if (!deviceAttachedForBusId(m_Devices, busId)) {
        setLastError(QString());
        setDeviceError(busId, QString());
        return true;
    }

    const bool ok = runUsbipdCommand({QStringLiteral("detach"), QStringLiteral("--busid"), busId});
    refresh();
    setDeviceError(busId, ok ? QString() : m_LastError);
    return ok;
#elif defined(Q_OS_LINUX)
    if (!validateBusId(busId)) {
        return false;
    }

    setLastError(QString());
    setDeviceError(busId, QString());
    return true;
#else
    Q_UNUSED(busId)
    setLastError(tr("USB passthrough detach is not supported on this client OS yet."));
    return false;
#endif
}

bool UsbPassthroughManager::unbindDevice(const QString& busId)
{
#if defined(Q_OS_LINUX)
    if (!validateBusId(busId)) {
        return false;
    }
    if (m_UsbipPath.isEmpty()) {
        const QString error = tr("usbip command was not found.");
        setLastError(error);
        setDeviceError(busId, error);
        return false;
    }

    const bool ok = runUsbipCommand({QStringLiteral("unbind"), QStringLiteral("-b"), busId});
    refresh();
    setDeviceError(busId, ok ? QString() : m_LastError);
    return ok;
#elif defined(Q_OS_WIN32)
    if (!validateBusId(busId)) {
        return false;
    }
    if (m_UsbipdPath.isEmpty()) {
        const QString error = tr("usbipd-win was not found.");
        setLastError(error);
        setDeviceError(busId, error);
        return false;
    }

    if (!detachDevice(busId)) {
        return false;
    }

    const bool ok = runUsbipdCommand({QStringLiteral("unbind"), QStringLiteral("--busid"), busId});
    refresh();
    setDeviceError(busId, ok ? QString() : m_LastError);
    return ok;
#else
    Q_UNUSED(busId)
    setLastError(tr("USB passthrough unbinding is not supported on this client OS yet."));
    return false;
#endif
}

bool UsbPassthroughManager::installDependency()
{
#ifdef Q_OS_LINUX
    const QString modprobePath = QStandardPaths::findExecutable(
        QStringLiteral("modprobe"),
        {
            QStringLiteral("/usr/sbin"),
            QStringLiteral("/sbin"),
            QStringLiteral("/usr/bin"),
            QStringLiteral("/bin"),
        });
    if (modprobePath.isEmpty()) {
        setLastError(tr("modprobe was not found. Install USB/IP kernel modules for this distribution, then refresh USB passthrough."));
        return false;
    }

    const QStringList moduleArgs {
        QStringLiteral("usbip-core"),
        QStringLiteral("usbip-host"),
    };

    if (geteuid() == 0) {
        QProcess modprobe;
        modprobe.start(modprobePath, moduleArgs);
        if (!modprobe.waitForStarted(5000)) {
            setLastError(tr("Failed to start modprobe for USB/IP kernel modules."));
            return false;
        }
        if (!modprobe.waitForFinished(30000)) {
            modprobe.kill();
            modprobe.waitForFinished(1000);
            setLastError(tr("Timed out while loading USB/IP kernel modules."));
            return false;
        }
        if (modprobe.exitStatus() != QProcess::NormalExit || modprobe.exitCode() != 0) {
            const QString error = QString::fromLocal8Bit(modprobe.readAllStandardError()).trimmed();
            setLastError(error.isEmpty() ?
                             tr("Failed to load USB/IP kernel modules.") :
                             tr("Failed to load USB/IP kernel modules: %1").arg(error));
            return false;
        }

        refresh();
        setLastError(QString());
        return m_DependenciesReady;
    }

    const QString pkexecPath = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexecPath.isEmpty()) {
        setLastError(tr("pkexec was not found. Run 'sudo modprobe usbip-core usbip-host', then refresh USB passthrough."));
        return false;
    }

    QStringList args;
    args << modprobePath << moduleArgs;
    if (!QProcess::startDetached(pkexecPath, args)) {
        setLastError(tr("Failed to launch administrator prompt for USB/IP kernel modules."));
        return false;
    }

    m_StatusMessage = tr("Administrator prompt started to load USB/IP kernel modules. Refresh USB passthrough after it completes.");
    setLastError(QString());
    emit statusChanged();
    return true;
#elif defined(Q_OS_WIN32)
    const QString wingetPath = QStandardPaths::findExecutable(QStringLiteral("winget"));
    if (wingetPath.isEmpty()) {
        setLastError(tr("winget was not found. Install usbipd-win from https://github.com/dorssel/usbipd-win/releases, then refresh USB passthrough."));
        return false;
    }

    const QString command = QStringLiteral(
        "%1 install --interactive --exact --id dorssel.usbipd-win"
        " --accept-package-agreements --accept-source-agreements"
        " & echo."
        " & echo Return to Moonlight and click Refresh after the installer exits.");
    const QString expandedCommand = command.arg(quoteWindowsArgument(QDir::toNativeSeparators(wingetPath)));
    const std::wstring parameters = windowsArgumentString({QStringLiteral("/k"), expandedCommand}).toStdWString();

    SHELLEXECUTEINFOW execInfo = {};
    execInfo.cbSize = sizeof(execInfo);
    execInfo.hwnd = nullptr;
    execInfo.lpVerb = L"runas";
    execInfo.lpFile = L"cmd.exe";
    execInfo.lpParameters = parameters.c_str();
    execInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execInfo)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            setLastError(tr("usbipd-win installer approval was cancelled."));
            return false;
        }

        setLastError(tr("Failed to launch usbipd-win installer: %1").arg(windowsLastErrorMessage(error)));
        return false;
    }

    m_StatusMessage = tr("usbipd-win installer started. Refresh USB passthrough after it completes.");
    setLastError(QString());
    emit statusChanged();
    return true;
#else
    setLastError(tr("USB passthrough dependency installation is not supported on this client OS yet."));
    return false;
#endif
}

bool UsbPassthroughManager::testExporter()
{
    refresh();

#ifdef Q_OS_LINUX
    if (!m_DependenciesReady) {
        setLastError(tr("USB/IP is not ready. Refresh status and check the dependency message."));
        return false;
    }
    if (!startExportServer()) {
        return false;
    }
    const bool responded = linuxUsbipdResponds();
    stopExportServer();
    if (!responded) {
        setLastError(tr("USB/IP exporter did not respond on 127.0.0.1:3240."));
        return false;
    }

    m_StatusMessage = tr("USB/IP exporter responded on 127.0.0.1:3240.");
    setLastError(QString());
    emit statusChanged();
    return true;
#elif defined(Q_OS_WIN32)
    if (!m_DependenciesReady) {
        setLastError(tr("usbipd-win is not ready."));
        return false;
    }

    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), kDefaultUsbipPort);
    if (!socket.waitForConnected(5000)) {
        setLastError(tr("usbipd-win did not accept a localhost USB/IP connection on 127.0.0.1:3240: %1").arg(socket.errorString()));
        return false;
    }

    socket.disconnectFromHost();
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        socket.waitForDisconnected(1000);
    }

    m_StatusMessage = tr("usbipd-win accepted a localhost USB/IP connection on 127.0.0.1:3240.");
    setLastError(QString());
    emit statusChanged();
    return true;
#else
    setLastError(tr("USB passthrough export is not supported on this client OS yet."));
    return false;
#endif
}

bool UsbPassthroughManager::startTunnel(const QString& host, quint16 hostPort, const QString& token, const QString& busId, quint16 exporterPort)
{
    if (host.isEmpty() || hostPort == 0 || !validateBusId(busId) || !safeTunnelToken(token)) {
        setLastError(tr("Invalid USB passthrough tunnel parameters."));
        return false;
    }

    auto state = std::make_shared<UsbTunnelState>();
    state->host = host;
    state->hostPort = hostPort;
    state->token = token;
    state->busId = busId;
    state->exporterPort = exporterPort == 0 ? kDefaultUsbipPort : exporterPort;
    for (const QVariant& deviceVariant : m_Devices) {
        const QVariantMap device = deviceVariant.toMap();
        if (device.value(QStringLiteral("busid")).toString() == busId) {
            state->transport = transportConfigForDevice(device);
            break;
        }
    }
    qInfo() << "USB passthrough tunnel transport selected for" << busId
            << "profile" << state->transport.profile
            << "risk" << state->transport.risk
            << "socketBufferBytes" << state->transport.socketBufferBytes
            << "copyBufferBytes" << state->transport.copyBufferBytes
            << "readWaitMs" << state->transport.readWaitMs;

    {
        std::lock_guard<std::mutex> lock(tunnelMutex());
        auto& states = tunnelStates();
        for (auto it = states.begin(); it != states.end();) {
            if ((*it)->busId == busId) {
                (*it)->stopRequested.store(true, std::memory_order_release);
                if ((*it)->thread.joinable()) {
                    (*it)->thread.join();
                }
                it = states.erase(it);
            }
            else {
                ++it;
            }
        }

        try {
            state->thread = std::thread(runTunnelThread, state);
        }
        catch (const std::system_error& e) {
            setLastError(QString::fromLocal8Bit(e.what()));
            return false;
        }
        states.push_back(state);
    }

    QString startupError;
    bool startupConnected = false;
    {
        std::unique_lock<std::mutex> startupLock(state->startupMutex);
        const bool startupReported = state->startupCv.wait_for(
            startupLock,
            kTunnelStartupRetryWindow + kTunnelStartupWaitSlack,
            [&state]() {
                return state->startupComplete;
            });

        startupConnected = startupReported && state->connectedOnce;
        if (!startupConnected) {
            startupError = state->startupError;
        }
    }

    if (!startupConnected) {
        state->stopRequested.store(true, std::memory_order_release);
        if (state->thread.joinable()) {
            state->thread.join();
        }

        {
            std::lock_guard<std::mutex> lock(tunnelMutex());
            auto& states = tunnelStates();
            for (auto it = states.begin(); it != states.end();) {
                if (it->get() == state.get()) {
                    it = states.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        if (startupError.isEmpty()) {
            startupError = tr("USB passthrough tunnel did not connect within the startup window.");
        }
        setLastError(startupError);
        return false;
    }

    setLastError(QString());
    return true;
}

void UsbPassthroughManager::stopTunnels()
{
    std::vector<std::shared_ptr<UsbTunnelState>> states;
    {
        std::lock_guard<std::mutex> lock(tunnelMutex());
        states.swap(tunnelStates());
    }

    for (const auto& state : states) {
        state->stopRequested.store(true, std::memory_order_release);
    }
    for (const auto& state : states) {
        if (state->thread.joinable()) {
            state->thread.join();
        }
    }
}

QVariantList UsbPassthroughManager::enumerateLinuxDevices() const
{
    QVariantList devices;

#ifdef Q_OS_LINUX
    QDir usbDir(QStringLiteral("/sys/bus/usb/devices"));
    if (!usbDir.exists()) {
        return devices;
    }

    const QStringList entries = usbDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& entry : entries) {
        if (entry.startsWith(QStringLiteral("usb")) || !looksLikeUsbBusId(entry)) {
            continue;
        }

        const QString path = usbDir.filePath(entry);
        const QString vid = readTextFile(path + QStringLiteral("/idVendor")).toLower();
        const QString pid = readTextFile(path + QStringLiteral("/idProduct")).toLower();
        if (vid.isEmpty() || pid.isEmpty()) {
            continue;
        }

        const QString product = readTextFile(path + QStringLiteral("/product"));
        const QString vendor = readTextFile(path + QStringLiteral("/manufacturer"));
        const QString serial = readTextFile(path + QStringLiteral("/serial"));
        const QString deviceClassCode = readTextFile(path + QStringLiteral("/bDeviceClass"));
        const QStringList interfaces = interfaceClassesForDevice(path, entry);
        const UsbEndpointMetadata endpointMetadata = endpointMetadataForLinuxDevice(path, entry);
        const QString driver = driverNameForDevice(path);

        QVariantMap device;
        device[QStringLiteral("id")] = entry + QStringLiteral(":") + vid + QStringLiteral(":") + pid;
        device[QStringLiteral("busid")] = entry;
        device[QStringLiteral("vid")] = vid;
        device[QStringLiteral("pid")] = pid;
        device[QStringLiteral("vendor")] = vendor;
        device[QStringLiteral("product")] = product;
        device[QStringLiteral("serialHash")] = hashSerial(serial);
        device[QStringLiteral("serialPresent")] = !serial.isEmpty();
        device[QStringLiteral("deviceClass")] = deriveDeviceClass(deviceClassCode, interfaces);
        device[QStringLiteral("interfaces")] = interfaces;
        device[QStringLiteral("transferTypes")] = endpointMetadata.transferTypes;
        device[QStringLiteral("endpointCounts")] = endpointMetadata.endpointCounts;
        device[QStringLiteral("endpointMaxPacketSize")] = endpointMetadata.maxPacketSize;
        device[QStringLiteral("endpoints")] = endpointMetadata.endpoints;
        device[QStringLiteral("speed")] = readTextFile(path + QStringLiteral("/speed"));
        device[QStringLiteral("clientOs")] = QStringLiteral("linux");
        device[QStringLiteral("backend")] = QStringLiteral("linux-usbip");
        device[QStringLiteral("requiresAdmin")] = true;
        device[QStringLiteral("driver")] = driver;
        device[QStringLiteral("state")] = driver == QStringLiteral("usbip-host") ? QStringLiteral("bound") : QStringLiteral("available");
        applyApprovalIdentity(device, QStringLiteral("linux-usbip"));
        applyUsbSafetyPolicy(device);

        devices.append(device);
    }
#endif

    return devices;
}

QVariantList UsbPassthroughManager::enumerateWindowsDevices()
{
    QVariantList devices;

#ifdef Q_OS_WIN32
    if (m_UsbipdPath.isEmpty()) {
        return devices;
    }

    QProcess process;
    process.start(m_UsbipdPath, {QStringLiteral("state")});
    if (!process.waitForStarted(5000)) {
        setLastError(tr("Failed to start usbipd-win."));
        m_DependenciesReady = false;
        return devices;
    }
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished(1000);
        setLastError(tr("usbipd-win state command timed out."));
        m_DependenciesReady = false;
        return devices;
    }

    const QByteArray output = process.readAllStandardOutput();
    const QString error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        setLastError(error.isEmpty() ? tr("usbipd-win state command failed.") : error);
        m_DependenciesReady = false;
        return devices;
    }

    QJsonParseError parseError = {};
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setLastError(tr("usbipd-win returned invalid state JSON."));
        m_DependenciesReady = false;
        return devices;
    }

    const QJsonArray usbipdDevices = document.object().value(QStringLiteral("Devices")).toArray();
    for (const QJsonValue& value : usbipdDevices) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject object = value.toObject();
        const QString busId = valueToString(object, QStringLiteral("BusId"));
        const QString instanceId = valueToString(object, QStringLiteral("InstanceId"));
        const QString description = valueToString(object, QStringLiteral("Description"));
        const QString deviceClass = valueToString(object, QStringLiteral("DeviceClass"));
        const QString className = deviceClass.isEmpty() ? valueToString(object, QStringLiteral("Class")) : deviceClass;
        const QString persistedGuid = valueToString(object, QStringLiteral("PersistedGuid"));
        const QString stubInstanceId = valueToString(object, QStringLiteral("StubInstanceId"));
        const bool attached = hasNonNullValue(object, QStringLiteral("ClientIPAddress"));
        const bool connected = !busId.isEmpty();
        const bool bound = !persistedGuid.isEmpty();

        QVariantMap device;
        device[QStringLiteral("id")] = !busId.isEmpty() ? busId : hashIdentifier(instanceId);
        device[QStringLiteral("busid")] = busId;
        device[QStringLiteral("vid")] = hardwareIdPart(instanceId, QStringLiteral("vid"));
        device[QStringLiteral("pid")] = hardwareIdPart(instanceId, QStringLiteral("pid"));
        device[QStringLiteral("vendor")] = QString();
        device[QStringLiteral("product")] = description;
        device[QStringLiteral("description")] = description;
        device[QStringLiteral("serialHash")] = hashIdentifier(instanceId);
        device[QStringLiteral("serialPresent")] = !instanceId.isEmpty();
        device[QStringLiteral("deviceClass")] = className.isEmpty() ? QStringLiteral("unknown") : className;
        device[QStringLiteral("interfaces")] = QStringList();
        device[QStringLiteral("speed")] = QString();
        device[QStringLiteral("clientOs")] = QStringLiteral("windows");
        device[QStringLiteral("backend")] = QStringLiteral("windows-usbipd-win");
        device[QStringLiteral("requiresAdmin")] = true;
        device[QStringLiteral("driver")] = stubInstanceId.isEmpty() ? QString() : QStringLiteral("usbipd-win");
        device[QStringLiteral("connected")] = connected;
        device[QStringLiteral("bound")] = bound;
        device[QStringLiteral("attached")] = attached;
        device[QStringLiteral("forced")] = valueToBool(object, QStringLiteral("IsForced"));

        if (!connected) {
            device[QStringLiteral("state")] = QStringLiteral("disconnected");
        }
        else if (attached) {
            device[QStringLiteral("state")] = QStringLiteral("attached");
        }
        else if (bound) {
            device[QStringLiteral("state")] = QStringLiteral("bound");
        }
        else {
            device[QStringLiteral("state")] = QStringLiteral("available");
        }
        applyApprovalIdentity(device, QStringLiteral("windows-usbipd-win"));
        applyUsbSafetyPolicy(device);

        devices.append(device);
    }

    setLastError(QString());
#endif

    return devices;
}

QString UsbPassthroughManager::resolveWindowsUsbipdPath() const
{
#ifdef Q_OS_WIN32
    const QString pathExecutable = QStandardPaths::findExecutable(QStringLiteral("usbipd"));
    if (!pathExecutable.isEmpty()) {
        return pathExecutable;
    }

    const QStringList candidates = {
        QStringLiteral("C:/Program Files/usbipd-win/usbipd.exe"),
        QStringLiteral("C:/Program Files (x86)/usbipd-win/usbipd.exe")
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
#endif

    return QString();
}

QString UsbPassthroughManager::queryWindowsUsbipdServiceState() const
{
#ifdef Q_OS_WIN32
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return QStringLiteral("unknown");
    }

    SC_HANDLE service = OpenServiceW(scm, L"usbipd", SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        return error == ERROR_SERVICE_DOES_NOT_EXIST ? QStringLiteral("missing") : QStringLiteral("unknown");
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD bytesNeeded = 0;
    const bool queried = QueryServiceStatusEx(
        service,
        SC_STATUS_PROCESS_INFO,
        reinterpret_cast<LPBYTE>(&status),
        sizeof(status),
        &bytesNeeded);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    return queried ? windowsServiceStateToString(status.dwCurrentState) : QStringLiteral("unknown");
#else
    return QString();
#endif
}

bool UsbPassthroughManager::linuxUsbipdResponds()
{
#ifdef Q_OS_LINUX
    if (m_UsbipPath.isEmpty()) {
        return false;
    }

    QProcess process;
    process.start(m_UsbipPath, {QStringLiteral("list"), QStringLiteral("-r"), QStringLiteral("127.0.0.1")});
    if (!process.waitForStarted(3000)) {
        return false;
    }
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }

    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
#else
    return false;
#endif
}

bool UsbPassthroughManager::startManagedLinuxUsbipd()
{
#ifdef Q_OS_LINUX
    std::lock_guard<std::mutex> lock(exportServerMutex());

    if (linuxUsbipdResponds()) {
        setLastError(QString());
        return true;
    }

    auto& process = managedUsbipdProcess();
    if (process && process->state() != QProcess::NotRunning) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (linuxUsbipdResponds()) {
                setLastError(QString());
                return true;
            }
            if (process->waitForFinished(250)) {
                break;
            }
        }

        if (process->state() != QProcess::NotRunning) {
            setLastError(tr("Managed usbipd process started but did not respond on 127.0.0.1:3240."));
            return false;
        }
    }

    QString executable = m_UsbipdPath;
    QStringList processArguments;
    bool elevated = false;

    if (geteuid() != 0) {
        const QString pkexecPath = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (pkexecPath.isEmpty()) {
            setLastError(tr("pkexec was not found. Run 'sudo usbipd', then refresh USB passthrough."));
            return false;
        }

        executable = pkexecPath;
        processArguments << m_UsbipdPath;
        elevated = true;
    }

    process = std::make_unique<QProcess>();
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start(executable, processArguments);
    if (!process->waitForStarted(elevated ? 60000 : 5000)) {
        const QString error = process->errorString();
        process.reset();
        setLastError(elevated ?
                         tr("Failed to start administrator prompt for usbipd: %1").arg(error) :
                         tr("Failed to start usbipd: %1").arg(error));
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + (elevated ? std::chrono::seconds(60) : std::chrono::seconds(10));
    while (std::chrono::steady_clock::now() < deadline) {
        if (linuxUsbipdResponds()) {
            setLastError(QString());
            return true;
        }

        if (process->waitForFinished(250)) {
            const QString output = QString::fromLocal8Bit(process->readAll()).trimmed();
            if (linuxUsbipdResponds()) {
                process.reset();
                setLastError(QString());
                return true;
            }

            process.reset();
            setLastError(output.isEmpty() ?
                             tr("usbipd exited before accepting localhost USB/IP connections.") :
                             tr("usbipd exited before accepting localhost USB/IP connections: %1").arg(output));
            return false;
        }
    }

    process->terminate();
    if (!process->waitForFinished(2000)) {
        process->kill();
        process->waitForFinished(1000);
    }
    process.reset();
    setLastError(elevated ?
                     tr("Timed out waiting for administrator-approved usbipd to accept localhost USB/IP connections.") :
                     tr("Timed out waiting for usbipd to accept localhost USB/IP connections."));
    return false;
#else
    return false;
#endif
}

bool UsbPassthroughManager::runUsbipCommand(const QStringList& arguments)
{
    if (m_UsbipPath.isEmpty()) {
        setLastError(tr("usbip command was not found."));
        return false;
    }

    QString executable = m_UsbipPath;
    QStringList processArguments = arguments;
    bool elevated = false;

#ifdef Q_OS_LINUX
    const bool requiresAdmin =
        !arguments.isEmpty() &&
        (arguments.first() == QStringLiteral("bind") || arguments.first() == QStringLiteral("unbind"));
    if (requiresAdmin && geteuid() != 0) {
        const QString pkexecPath = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (pkexecPath.isEmpty()) {
            setLastError(tr("pkexec was not found. Run 'sudo usbip %1', then refresh USB passthrough.").arg(arguments.join(QLatin1Char(' '))));
            return false;
        }

        executable = pkexecPath;
        processArguments = QStringList {m_UsbipPath};
        processArguments << arguments;
        elevated = true;
    }
#endif

    QProcess process;
    process.start(executable, processArguments);
    if (!process.waitForStarted(5000)) {
        setLastError(elevated ?
                         tr("Failed to start administrator prompt for usbip.") :
                         tr("Failed to start usbip."));
        return false;
    }
    if (!process.waitForFinished(elevated ? 60000 : 15000)) {
        process.kill();
        process.waitForFinished(1000);
        setLastError(elevated ?
                         tr("usbip command timed out while waiting for administrator approval or completion.") :
                         tr("usbip command timed out."));
        return false;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const QString error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (!error.isEmpty() || !output.isEmpty()) {
            setLastError(error.isEmpty() ? output : error);
        }
        else {
            setLastError(elevated ?
                             tr("usbip administrator command failed with exit code %1.").arg(process.exitCode()) :
                             tr("usbip command failed with exit code %1.").arg(process.exitCode()));
        }
        return false;
    }

    setLastError(QString());
    return true;
}

bool UsbPassthroughManager::runUsbipdCommand(const QStringList& arguments)
{
    if (m_UsbipdPath.isEmpty()) {
#ifdef Q_OS_WIN32
        setLastError(tr("usbipd-win was not found."));
#else
        setLastError(tr("usbipd was not found."));
#endif
        return false;
    }

#ifdef Q_OS_WIN32
    const std::wstring file = QDir::toNativeSeparators(m_UsbipdPath).toStdWString();
    const std::wstring parameters = windowsArgumentString(arguments).toStdWString();
    const QString commandName = arguments.isEmpty() ? QString() : arguments.first();
    const bool requiresAdmin =
        commandName == QStringLiteral("bind") ||
        commandName == QStringLiteral("unbind");

    SHELLEXECUTEINFOW execInfo = {};
    execInfo.cbSize = sizeof(execInfo);
    execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    execInfo.hwnd = nullptr;
    execInfo.lpVerb = requiresAdmin ? L"runas" : nullptr;
    execInfo.lpFile = file.c_str();
    execInfo.lpParameters = parameters.c_str();
    execInfo.nShow = SW_HIDE;

    if (!ShellExecuteExW(&execInfo)) {
        const DWORD error = GetLastError();
        if (requiresAdmin && error == ERROR_CANCELLED) {
            setLastError(tr("usbipd-win administrator approval was cancelled."));
        }
        else if (requiresAdmin) {
            setLastError(tr("Failed to start usbipd-win with administrator approval: %1").arg(windowsLastErrorMessage(error)));
        }
        else {
            setLastError(tr("Failed to start usbipd-win: %1").arg(windowsLastErrorMessage(error)));
        }
        return false;
    }
    if (!execInfo.hProcess) {
        setLastError(tr("usbipd-win command did not return a process handle."));
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(execInfo.hProcess, 60000);
    if (waitResult == WAIT_TIMEOUT) {
        CloseHandle(execInfo.hProcess);
        setLastError(requiresAdmin ?
                         tr("usbipd-win command timed out while waiting for administrator approval or completion.") :
                         tr("usbipd-win command timed out."));
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        const DWORD error = GetLastError();
        CloseHandle(execInfo.hProcess);
        setLastError(tr("usbipd-win command wait failed: %1").arg(windowsLastErrorMessage(error)));
        return false;
    }

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(execInfo.hProcess, &exitCode)) {
        const DWORD error = GetLastError();
        CloseHandle(execInfo.hProcess);
        setLastError(tr("usbipd-win command exit status was unavailable: %1").arg(windowsLastErrorMessage(error)));
        return false;
    }
    CloseHandle(execInfo.hProcess);

    if (exitCode != 0) {
        setLastError(tr("usbipd-win command failed with exit code %1.").arg(static_cast<qulonglong>(exitCode)));
        return false;
    }

    setLastError(QString());
    return true;
#else
    QString executable = m_UsbipdPath;
    QStringList processArguments = arguments;
    bool elevated = false;

#ifdef Q_OS_LINUX
    if (geteuid() != 0 && arguments.contains(QStringLiteral("-D"))) {
        const QString pkexecPath = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (pkexecPath.isEmpty()) {
            setLastError(tr("pkexec was not found. Run 'sudo usbipd -D', then refresh USB passthrough."));
            return false;
        }

        executable = pkexecPath;
        processArguments = QStringList {m_UsbipdPath};
        processArguments << arguments;
        elevated = true;
    }
#endif

    QProcess process;
    process.start(executable, processArguments);
    if (!process.waitForStarted(5000)) {
        setLastError(elevated ?
                         tr("Failed to start administrator prompt for usbipd.") :
                         tr("Failed to start usbipd."));
        return false;
    }
    if (!process.waitForFinished(elevated ? 60000 : 15000)) {
        process.kill();
        process.waitForFinished(1000);
        setLastError(elevated ?
                         tr("usbipd command timed out while waiting for administrator approval or completion.") :
                         tr("usbipd command timed out."));
        return false;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const QString error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (!error.isEmpty() || !output.isEmpty()) {
            setLastError(error.isEmpty() ? output : error);
        }
        else {
            setLastError(elevated ?
                             tr("usbipd administrator command failed with exit code %1.").arg(process.exitCode()) :
                             tr("usbipd command failed with exit code %1.").arg(process.exitCode()));
        }
        return false;
    }

    setLastError(QString());
    return true;
#endif
}

bool UsbPassthroughManager::validateBusId(const QString& busId)
{
    if (!looksLikeUsbBusId(busId)) {
        setLastError(tr("Invalid USB bus ID."));
        return false;
    }

    return true;
}

void UsbPassthroughManager::setLastError(const QString& error)
{
    if (m_LastError == error) {
        return;
    }

    m_LastError = error;
    emit statusChanged();
}

void UsbPassthroughManager::setDeviceError(const QString& busId, const QString& error)
{
    if (busId.isEmpty()) {
        return;
    }

    if (error.isEmpty()) {
        if (!m_DeviceErrors.contains(busId)) {
            return;
        }
        m_DeviceErrors.remove(busId);
        emit deviceErrorsChanged();
        return;
    }

    if (m_DeviceErrors.value(busId).toString() == error) {
        return;
    }

    m_DeviceErrors.insert(busId, error);
    emit deviceErrorsChanged();
}

void UsbPassthroughManager::pruneDeviceErrors()
{
    QStringList presentBusIds;
    for (const QVariant& deviceVariant : m_Devices) {
        const QString busId = deviceVariant.toMap().value(QStringLiteral("busid")).toString();
        if (!busId.isEmpty()) {
            presentBusIds.append(busId);
        }
    }

    bool changed = false;
    for (auto it = m_DeviceErrors.begin(); it != m_DeviceErrors.end();) {
        if (!presentBusIds.contains(it.key())) {
            it = m_DeviceErrors.erase(it);
            changed = true;
        }
        else {
            ++it;
        }
    }

    if (changed) {
        emit deviceErrorsChanged();
    }
}
