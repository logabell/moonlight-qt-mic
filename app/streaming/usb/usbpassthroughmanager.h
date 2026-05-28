#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class UsbPassthroughManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool supported READ isSupported CONSTANT)
    Q_PROPERTY(QString backend READ backend NOTIFY statusChanged)
    Q_PROPERTY(bool dependenciesReady READ dependenciesReady NOTIFY statusChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)
    Q_PROPERTY(QString usbipPath READ usbipPath NOTIFY statusChanged)
    Q_PROPERTY(QString usbipdPath READ usbipdPath NOTIFY statusChanged)
    Q_PROPERTY(QString usbipdServiceState READ usbipdServiceState NOTIFY statusChanged)
    Q_PROPERTY(bool usbipCoreLoaded READ usbipCoreLoaded NOTIFY statusChanged)
    Q_PROPERTY(bool usbipHostLoaded READ usbipHostLoaded NOTIFY statusChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantMap deviceErrors READ deviceErrors NOTIFY deviceErrorsChanged)

public:
    explicit UsbPassthroughManager(QObject* parent = nullptr);

    static UsbPassthroughManager* get(QQmlEngine* qmlEngine = nullptr);

    bool isSupported() const;
    QString backend() const;
    bool dependenciesReady() const;
    QString statusMessage() const;
    QString lastError() const;
    QString usbipPath() const;
    QString usbipdPath() const;
    QString usbipdServiceState() const;
    bool usbipCoreLoaded() const;
    bool usbipHostLoaded() const;
    QVariantList devices() const;
    QVariantMap deviceErrors() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool startExportServer();
    Q_INVOKABLE void stopExportServer();
    Q_INVOKABLE bool bindDevice(const QString& busId);
    Q_INVOKABLE bool detachDevice(const QString& busId);
    Q_INVOKABLE bool unbindDevice(const QString& busId);
    Q_INVOKABLE bool installDependency();
    Q_INVOKABLE bool testExporter();
    Q_INVOKABLE bool startTunnel(const QString& host, quint16 hostPort, const QString& token, const QString& busId, quint16 exporterPort);
    Q_INVOKABLE void stopTunnels();

signals:
    void statusChanged();
    void devicesChanged();
    void deviceErrorsChanged();

private:
    QVariantList enumerateLinuxDevices() const;
    QVariantList enumerateWindowsDevices();
    QString resolveWindowsUsbipdPath() const;
    QString queryWindowsUsbipdServiceState() const;
    bool linuxUsbipdResponds();
    bool startManagedLinuxUsbipd();
    bool runUsbipCommand(const QStringList& arguments);
    bool runUsbipdCommand(const QStringList& arguments);
    bool validateBusId(const QString& busId);
    void setLastError(const QString& error);
    void setDeviceError(const QString& busId, const QString& error);
    void pruneDeviceErrors();

    bool m_DependenciesReady;
    bool m_UsbipCoreLoaded;
    bool m_UsbipHostLoaded;
    QString m_Backend;
    QString m_StatusMessage;
    QString m_LastError;
    QString m_UsbipPath;
    QString m_UsbipdPath;
    QString m_UsbipdServiceState;
    QVariantList m_Devices;
    QVariantMap m_DeviceErrors;
    QQmlEngine* m_QmlEngine;
};
