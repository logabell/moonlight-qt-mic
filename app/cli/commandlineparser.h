#pragma once

#include "settings/streamingpreferences.h"

#include <QMap>
#include <QString>

class GlobalCommandLineParser
{
public:
    enum ParseResult {
        NormalStartRequested,
        StreamRequested,
        QuitRequested,
        PairRequested,
        ListRequested,
        UsbLabInstallRequested,
        UsbLabListRequested,
        UsbLabExportRequested,
        UsbLabTunnelRequested,
    };

    GlobalCommandLineParser();
    virtual ~GlobalCommandLineParser();

    ParseResult parse(const QStringList &args);

};

class QuitCommandLineParser
{
public:
    QuitCommandLineParser();
    virtual ~QuitCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;

private:
    QString m_Host;
};

class PairCommandLineParser
{
public:
    PairCommandLineParser();
    virtual ~PairCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    QString getPredefinedPin() const;

private:
    QString m_Host;
    QString m_PredefinedPin;
};

class StreamCommandLineParser
{
public:
    StreamCommandLineParser();
    virtual ~StreamCommandLineParser();

    void parse(const QStringList &args, StreamingPreferences *preferences);

    QString getHost() const;
    QString getAppName() const;

private:
    QString m_Host;
    QString m_AppName;
    QMap<QString, StreamingPreferences::WindowMode> m_WindowModeMap;
    QMap<QString, StreamingPreferences::AudioConfig> m_AudioConfigMap;
    QMap<QString, StreamingPreferences::VideoCodecConfig> m_VideoCodecMap;
    QMap<QString, StreamingPreferences::VideoDecoderSelection> m_VideoDecoderMap;
    QMap<QString, StreamingPreferences::CaptureSysKeysMode> m_CaptureSysKeysModeMap;
};

class ListCommandLineParser
{
public:
    ListCommandLineParser();
    virtual ~ListCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    bool isPrintCSV() const;
    bool isVerbose() const;

private:
    QString m_Host;
    bool m_PrintCSV;
    bool m_Verbose;
};

class UsbLabTunnelCommandLineParser
{
public:
    UsbLabTunnelCommandLineParser();
    virtual ~UsbLabTunnelCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    quint16 getPort() const;
    QString getToken() const;
    QString getBusId() const;
    quint16 getExporterPort() const;
    int getHoldSeconds() const;
    bool shouldBind() const;
    bool isJson() const;
    QString getOutputPath() const;

private:
    QString m_Host;
    quint16 m_Port;
    QString m_Token;
    QString m_BusId;
    quint16 m_ExporterPort;
    int m_HoldSeconds;
    bool m_Bind;
    bool m_Json;
    QString m_OutputPath;
};

class UsbLabInstallCommandLineParser
{
public:
    UsbLabInstallCommandLineParser();
    virtual ~UsbLabInstallCommandLineParser();

    void parse(const QStringList &args);

    bool isDryRun() const;
    bool isJson() const;
    QString getOutputPath() const;

private:
    bool m_DryRun;
    bool m_Json;
    QString m_OutputPath;
};

class UsbLabExportCommandLineParser
{
public:
    UsbLabExportCommandLineParser();
    virtual ~UsbLabExportCommandLineParser();

    void parse(const QStringList &args);

    QString getBusId() const;
    int getHoldSeconds() const;
    bool shouldBind() const;
    bool isJson() const;
    QString getOutputPath() const;

private:
    QString m_BusId;
    int m_HoldSeconds;
    bool m_Bind;
    bool m_Json;
    QString m_OutputPath;
};

class UsbLabListCommandLineParser
{
public:
    UsbLabListCommandLineParser();
    virtual ~UsbLabListCommandLineParser();

    void parse(const QStringList &args);

    bool isJson() const;
    bool shouldTestExporter() const;
    QString getOutputPath() const;

private:
    bool m_Json;
    bool m_TestExporter;
    QString m_OutputPath;
};
