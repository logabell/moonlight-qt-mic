#include "commandlineparser.h"

#include <QCommandLineParser>
#include <QRegularExpression>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

static bool inRange(int value, int min, int max)
{
    return value >= min && value <= max;
}

// This method returns key's value from QMap where the key is a QString.
// Key matching is case insensitive.
template <typename T>
static T mapValue(QMap<QString, T> map, QString key)
{
    for(auto& item : map.toStdMap()) {
        if (QString::compare(item.first, key, Qt::CaseInsensitive) == 0) {
            return item.second;
        }
    }
    return T();
}

class CommandLineParser : public QCommandLineParser
{
public:
    enum MessageType {
        Info,
        Error
    };

    void setupCommonOptions()
    {
        setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        addHelpOption();
        addVersionOption();
    }

    void handleHelpAndVersionOptions()
    {
        if (isSet("help")) {
            showInfo(helpText());
        }
        if (isSet("version")) {
            showVersion();
        }
    }

    void handleUnknownOptions()
    {
        if (!unknownOptionNames().isEmpty()) {
            showError(QString("Unknown options: %1").arg(unknownOptionNames().join(", ")));
        }
    }

    void showMessage(QString message, MessageType type) const
    {
        message = message.endsWith('\n') ? message : message + '\n';
        fputs(qPrintable(message), type == Info ? stdout : stderr);
    }

    [[ noreturn ]] void showInfo(QString message) const
    {
        showMessage(message, Info);
        exit(0);
    }

    [[ noreturn ]] void showError(QString message) const
    {
        showMessage(message + "\n\n" + helpText(), Error);
        exit(1);
    }

    int getIntOption(QString name) const
    {
        bool ok;
        int intValue = value(name).toInt(&ok);
        if (!ok) {
            showError(QString("Invalid %1 value: %2").arg(name, value(name)));
        }
        return intValue;
    }

    bool getToggleOptionValue(QString name, bool defaultValue) const
    {
        QRegularExpression re(QString("^(%1|no-%1)$").arg(name));
        QStringList options = optionNames().filter(re);
        if (options.isEmpty()) {
            return defaultValue;
        } else {
            return options.last() == name;
        }
    }

    QString getChoiceOptionValue(QString name) const
    {
        if (!m_Choices[name].contains(value(name), Qt::CaseInsensitive)) {
            showError(QString("Invalid %1 choice: %2").arg(name, value(name)));
        }
        return value(name);
    }

    QPair<int,int> getResolutionOptionValue(QString name) const
    {
        static QRegularExpression re("^(\\d+)x(\\d+)$", QRegularExpression::CaseInsensitiveOption);
        auto match = re.match(value(name));
        if (!match.hasMatch()) {
            showError(QString("Invalid %1 format: %2").arg(name, value(name)));
        }
        return qMakePair(match.captured(1).toInt(), match.captured(2).toInt());
    }

    void addFlagOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Use %1.").arg(descriptiveName)));
    }

    void addToggleOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Use %1.").arg(descriptiveName)));
        addOption(QCommandLineOption("no-" + name, QString("Do not use %1.").arg(descriptiveName)));
    }

    void addValueOption(QString name, QString descriptiveName)
    {
        addOption(QCommandLineOption(name, QString("Specify %1 to use.").arg(descriptiveName), name));
    }

    void addChoiceOption(QString name, QString descriptiveName, QStringList choices)
    {
        addOption(QCommandLineOption(name, QString("Select %1: %2.").arg(descriptiveName, choices.join('/')), name));
        m_Choices[name] = choices;
    }

private:
    QMap<QString, QStringList> m_Choices;
};

static bool parseGlobalActionName(const QString& actionName, GlobalCommandLineParser::ParseResult* result)
{
    const QString action = actionName.toLower();
    if (action == QStringLiteral("quit")) {
        *result = GlobalCommandLineParser::QuitRequested;
        return true;
    }
    if (action == QStringLiteral("stream")) {
        *result = GlobalCommandLineParser::StreamRequested;
        return true;
    }
    if (action == QStringLiteral("pair")) {
        *result = GlobalCommandLineParser::PairRequested;
        return true;
    }
    if (action == QStringLiteral("list")) {
        *result = GlobalCommandLineParser::ListRequested;
        return true;
    }
    if (action == QStringLiteral("usb-lab-list")) {
        *result = GlobalCommandLineParser::UsbLabListRequested;
        return true;
    }
    if (action == QStringLiteral("usb-lab-install")) {
        *result = GlobalCommandLineParser::UsbLabInstallRequested;
        return true;
    }
    if (action == QStringLiteral("usb-lab-export")) {
        *result = GlobalCommandLineParser::UsbLabExportRequested;
        return true;
    }
    if (action == QStringLiteral("usb-lab-tunnel")) {
        *result = GlobalCommandLineParser::UsbLabTunnelRequested;
        return true;
    }

    return false;
}

GlobalCommandLineParser::GlobalCommandLineParser()
{
}

GlobalCommandLineParser::~GlobalCommandLineParser()
{
}

GlobalCommandLineParser::ParseResult GlobalCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Starts Moonlight normally if no arguments are given.\n"
        "\n"
        "Available actions:\n"
        "  list            List the available apps on a host\n"
        "  quit            Quit the currently running app\n"
        "  stream          Start streaming an app\n"
        "  pair            Pair a new host\n"
        "  usb-lab-list    List USB passthrough client status and devices\n"
        "  usb-lab-install Check or launch USB passthrough dependency setup\n"
        "  usb-lab-export  Share a USB device for direct/broker lab validation\n"
        "  usb-lab-tunnel  Open a USB reverse tunnel for lab validation\n"
        "\n"
        "See 'moonlight <action> --help' for help of specific action."
    );
    parser.addPositionalArgument("action", "Action to execute", "<action>");
    parser.parse(args);
    auto posArgs = parser.positionalArguments();

    for (int i = 1; i < args.size(); i++) {
        ParseResult result;
        if (parseGlobalActionName(args.at(i), &result)) {
            return result;
        }
    }

    if (posArgs.isEmpty()) {
        // This method will not return and terminates the process if --version
        // or --help is specified
        parser.handleHelpAndVersionOptions();
        parser.handleUnknownOptions();
        return NormalStartRequested;
    }
    else {
        // If users supply arguments that accept values prior to the "quit"
        // or "stream" positional arguments, we will not be able to correctly
        // parse the value out of the input because this QCommandLineParser
        // doesn't know about all of the options that "quit" and "stream"
        // commands can accept. To work around this issue, we just look
        // for "quit" or "stream" positional arguments anywhere.
        for (int i = 0; i < posArgs.size(); i++) {
            ParseResult result;
            if (parseGlobalActionName(posArgs.at(i), &result)) {
                return result;
            }
        }

        parser.showError(QString("Invalid action"));
    }
}

QuitCommandLineParser::QuitCommandLineParser()
{
}

QuitCommandLineParser::~QuitCommandLineParser()
{
}

void QuitCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Quit the currently running app on the given host."
    );
    parser.addPositionalArgument("quit", "quit running app");
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that host has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);
}

QString QuitCommandLineParser::getHost() const
{
    return m_Host;
}

PairCommandLineParser::PairCommandLineParser()
{
}

PairCommandLineParser::~PairCommandLineParser()
{
}

void PairCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Pair with the specified host."
    );
    parser.addPositionalArgument("pair", "pair host");
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");
    parser.addValueOption("pin", "4 digit pairing PIN");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that host has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);
    m_PredefinedPin = parser.value("pin");
    if (!m_PredefinedPin.isEmpty() && m_PredefinedPin.length() != 4) {
        parser.showError("PIN must be 4 digits");
    }
}

QString PairCommandLineParser::getHost() const
{
    return m_Host;
}

QString PairCommandLineParser::getPredefinedPin() const
{
    return m_PredefinedPin;
}

StreamCommandLineParser::StreamCommandLineParser()
{
    m_WindowModeMap = {
        {"fullscreen", StreamingPreferences::WM_FULLSCREEN},
        {"windowed",   StreamingPreferences::WM_WINDOWED},
        {"borderless", StreamingPreferences::WM_FULLSCREEN_DESKTOP},
    };
    m_AudioConfigMap = {
        {"stereo",       StreamingPreferences::AC_STEREO},
        {"5.1-surround", StreamingPreferences::AC_51_SURROUND},
        {"7.1-surround", StreamingPreferences::AC_71_SURROUND},
    };
    m_VideoCodecMap = {
        {"auto",  StreamingPreferences::VCC_AUTO},
        {"H.264", StreamingPreferences::VCC_FORCE_H264},
        {"HEVC",  StreamingPreferences::VCC_FORCE_HEVC},
        {"AV1", StreamingPreferences::VCC_FORCE_AV1},
    };
    m_VideoDecoderMap = {
        {"auto",     StreamingPreferences::VDS_AUTO},
        {"software", StreamingPreferences::VDS_FORCE_SOFTWARE},
        {"hardware", StreamingPreferences::VDS_FORCE_HARDWARE},
    };
    m_CaptureSysKeysModeMap = {
        {"never",      StreamingPreferences::CSK_OFF},
        {"fullscreen", StreamingPreferences::CSK_FULLSCREEN},
        {"always",     StreamingPreferences::CSK_ALWAYS},
    };
}

StreamCommandLineParser::~StreamCommandLineParser()
{
}

void StreamCommandLineParser::parse(const QStringList &args, StreamingPreferences *preferences)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Starts directly streaming a given app."
    );
    parser.addPositionalArgument("stream", "Start stream");

    // Add other arguments and options
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");
    parser.addPositionalArgument("app", "App to stream", "\"<app>\"");

    parser.addFlagOption("720",  "1280x720 resolution");
    parser.addFlagOption("1080", "1920x1080 resolution");
    parser.addFlagOption("1440", "2560x1440 resolution");
    parser.addFlagOption("4K", "3840x2160 resolution");
    parser.addValueOption("resolution", "custom <width>x<height> resolution");
    parser.addToggleOption("vsync", "V-Sync");
    parser.addValueOption("fps", "FPS");
    parser.addValueOption("bitrate", "bitrate in Kbps");
    parser.addValueOption("packet-size", "video packet size");
    parser.addChoiceOption("display-mode", "display mode", m_WindowModeMap.keys());
    parser.addChoiceOption("audio-config", "audio config", m_AudioConfigMap.keys());
    parser.addToggleOption("multi-controller", "multiple controller support");
    parser.addToggleOption("quit-after", "quit app after session");
    parser.addToggleOption("absolute-mouse", "remote desktop optimized mouse control");
    parser.addToggleOption("mouse-buttons-swap", "left and right mouse buttons swap");
    parser.addToggleOption("touchscreen-trackpad", "touchscreen in trackpad mode");
    parser.addToggleOption("game-optimization", "game optimizations");
    parser.addToggleOption("audio-on-host", "audio on host PC");
    parser.addToggleOption("frame-pacing", "frame pacing");
    parser.addToggleOption("mute-on-focus-loss", "mute audio when Moonlight window loses focus");
    parser.addToggleOption("background-gamepad", "background gamepad input");
    parser.addToggleOption("reverse-scroll-direction", "inverted scroll direction");
    parser.addToggleOption("swap-gamepad-buttons", "swap A/B and X/Y gamepad buttons (Nintendo-style)");
    parser.addToggleOption("keep-awake", "prevent display sleep while streaming");
    parser.addToggleOption("performance-overlay", "show performance overlay");
    parser.addToggleOption("hdr", "HDR streaming");
    parser.addToggleOption("yuv444", "YUV 4:4:4 sampling, if supported");
    parser.addChoiceOption("capture-system-keys", "capture system key combos", m_CaptureSysKeysModeMap.keys());
    parser.addChoiceOption("video-codec", "video codec", m_VideoCodecMap.keys());
    parser.addChoiceOption("video-decoder", "video decoder", m_VideoDecoderMap.keys());

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();

    // Resolve display's width and height
    static QRegularExpression resolutionRexExp("^(720|1080|1440|4K|resolution)$");
    QStringList resoOptions = parser.optionNames().filter(resolutionRexExp);
    bool displaySet = !resoOptions.isEmpty();
    if (displaySet) {
        QString name = resoOptions.last();
        if (name == "720") {
            preferences->width  = 1280;
            preferences->height = 720;
        } else if (name == "1080") {
            preferences->width  = 1920;
            preferences->height = 1080;
        } else if (name == "1440") {
            preferences->width  = 2560;
            preferences->height = 1440;
        } else if (name == "4K") {
            preferences->width  = 3840;
            preferences->height = 2160;
        } else if (name == "resolution") {
            auto resolution = parser.getResolutionOptionValue(name);
            preferences->width  = resolution.first;
            preferences->height = resolution.second;
        }
    }

    // Resolve --fps option
    if (parser.isSet("fps")) {
        preferences->fps = parser.getIntOption("fps");
        if (!inRange(preferences->fps, 10, 480)) {
            fprintf(stderr, "Warning: FPS is out of the supported range (10 - 480 FPS). Performance may suffer!\n");
        }
    }

    // Resolve --bitrate option
    if (parser.isSet("bitrate")) {
        preferences->bitrateKbps = parser.getIntOption("bitrate");
        if (!inRange(preferences->bitrateKbps, 500, 500000)) {
            fprintf(stderr, "Warning: Bitrate is out of the supported range (500 - 500000 Kbps). Performance may suffer!\n");
        }
    } else if (displaySet || parser.isSet("fps")) {
        preferences->bitrateKbps = preferences->getDefaultBitrate(
            preferences->width, preferences->height, preferences->fps, preferences->enableYUV444);
    }

    // Resolve --packet-size option
    if (parser.isSet("packet-size")) {
        preferences->packetSize = parser.getIntOption("packet-size");
        if (preferences->packetSize < 1024) {
            parser.showError("Packet size must be greater than 1024 bytes");
        }
    }

    // Resolve --display option
    if (parser.isSet("display-mode")) {
        preferences->windowMode = mapValue(m_WindowModeMap, parser.getChoiceOptionValue("display-mode"));
    }

    // Resolve --vsync and --no-vsync options
    preferences->enableVsync = parser.getToggleOptionValue("vsync", preferences->enableVsync);

    // Resolve --audio-config option
    if (parser.isSet("audio-config")) {
        preferences->audioConfig = mapValue(m_AudioConfigMap, parser.getChoiceOptionValue("audio-config"));
    }

    // Resolve --multi-controller and --no-multi-controller options
    preferences->multiController = parser.getToggleOptionValue("multi-controller", preferences->multiController);

    // Resolve --quit-after and --no-quit-after options
    preferences->quitAppAfter = parser.getToggleOptionValue("quit-after", preferences->quitAppAfter);

    // Resolve --absolute-mouse and --no-absolute-mouse options
    preferences->absoluteMouseMode = parser.getToggleOptionValue("absolute-mouse", preferences->absoluteMouseMode);

    // Resolve --mouse-buttons-swap and --no-mouse-buttons-swap options
    preferences->swapMouseButtons = parser.getToggleOptionValue("mouse-buttons-swap", preferences->swapMouseButtons);

    // Resolve --touchscreen-trackpad and --no-touchscreen-trackpad options
    preferences->absoluteTouchMode = !parser.getToggleOptionValue("touchscreen-trackpad", !preferences->absoluteTouchMode);

    // Resolve --game-optimization and --no-game-optimization options
    preferences->gameOptimizations = parser.getToggleOptionValue("game-optimization", preferences->gameOptimizations);

    // Resolve --audio-on-host and --no-audio-on-host options
    preferences->playAudioOnHost = parser.getToggleOptionValue("audio-on-host", preferences->playAudioOnHost);

    // Resolve --frame-pacing and --no-frame-pacing options
    preferences->framePacing = parser.getToggleOptionValue("frame-pacing", preferences->framePacing);

    // Resolve --mute-on-focus-loss and --no-mute-on-focus-loss options
    preferences->muteOnFocusLoss = parser.getToggleOptionValue("mute-on-focus-loss", preferences->muteOnFocusLoss);

    // Resolve --background-gamepad and --no-background-gamepad options
    preferences->backgroundGamepad = parser.getToggleOptionValue("background-gamepad", preferences->backgroundGamepad);

    // Resolve --reverse-scroll-direction and --no-reverse-scroll-direction options
    preferences->reverseScrollDirection = parser.getToggleOptionValue("reverse-scroll-direction", preferences->reverseScrollDirection);

    // Resolve --swap-gamepad-buttons and --no-swap-gamepad-buttons options
    preferences->swapFaceButtons = parser.getToggleOptionValue("swap-gamepad-buttons", preferences->swapFaceButtons);

    // Resolve --keep-awake and --no-keep-awake options
    preferences->keepAwake = parser.getToggleOptionValue("keep-awake", preferences->keepAwake);

    // Resolve --performance-overlay and --no-performance-overlay options
    preferences->showPerformanceOverlay = parser.getToggleOptionValue("performance-overlay", preferences->showPerformanceOverlay);

    // Resolve --hdr and --no-hdr options
    preferences->enableHdr = parser.getToggleOptionValue("hdr", preferences->enableHdr);

    // Resolve --yuv444 and --no-yuv444 options
    preferences->enableYUV444 = parser.getToggleOptionValue("yuv444", preferences->enableYUV444);
    
    // Resolve --capture-system-keys option
    if (parser.isSet("capture-system-keys")) {
        preferences->captureSysKeysMode = mapValue(m_CaptureSysKeysModeMap, parser.getChoiceOptionValue("capture-system-keys"));
    }

    // Resolve --video-codec option
    if (parser.isSet("video-codec")) {
        preferences->videoCodecConfig = mapValue(m_VideoCodecMap, parser.getChoiceOptionValue("video-codec"));
    }

    // Resolve --video-decoder option
    if (parser.isSet("video-decoder")) {
        preferences->videoDecoderSelection = mapValue(m_VideoDecoderMap, parser.getChoiceOptionValue("video-decoder"));
    }

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that both host and app has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);

    if (posArgs.length() < 3) {
        parser.showError("App not provided");
    }
    m_AppName = parser.positionalArguments().at(2);
}

QString StreamCommandLineParser::getHost() const
{
    return m_Host;
}

QString StreamCommandLineParser::getAppName() const
{
    return m_AppName;
}

ListCommandLineParser::ListCommandLineParser()
{
}

ListCommandLineParser::~ListCommandLineParser()
{
}

void ListCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "List the available apps on the given host."
    );
    parser.addPositionalArgument("list", "list available apps");
    parser.addPositionalArgument("host", "Host computer name, UUID, or IP address", "<host>");

    parser.addFlagOption("csv",     "Print as CSV with additional information");
    parser.addFlagOption("verbose", "Displays additional information");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();


    m_PrintCSV = parser.isSet("csv");
    m_Verbose = parser.isSet("verbose");

    // This method will not return and terminates the process if --version or
    // --help is specified
    parser.handleHelpAndVersionOptions();

    // Verify that host has been provided
    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("Host not provided");
    }
    m_Host = parser.positionalArguments().at(1);
}

QString ListCommandLineParser::getHost() const
{
    return m_Host;
}

bool ListCommandLineParser::isPrintCSV() const
{
    return m_PrintCSV;
}

bool ListCommandLineParser::isVerbose() const
{
    return m_Verbose;
}

UsbLabTunnelCommandLineParser::UsbLabTunnelCommandLineParser()
    : m_Port(0),
      m_ExporterPort(3240),
      m_HoldSeconds(-1),
      m_Bind(false),
      m_Json(false)
{
}

UsbLabTunnelCommandLineParser::~UsbLabTunnelCommandLineParser()
{
}

void UsbLabTunnelCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Open a USB passthrough reverse tunnel to a Vibepollo host for lab validation."
    );
    parser.addPositionalArgument("usb-lab-tunnel", "open USB lab tunnel");
    parser.addPositionalArgument("host", "Vibepollo host address", "<host>");
    parser.addPositionalArgument("port", "Vibepollo USB reverse tunnel port", "<port>");
    parser.addPositionalArgument("token", "Vibepollo USB reverse tunnel token", "<token>");
    parser.addPositionalArgument("busid", "Local USB device bus ID", "<busid>");
    parser.addPositionalArgument("exporter-port", "Local USB/IP exporter port", "[exporter-port]");
    parser.addFlagOption("bind", "binding/sharing the device before opening the tunnel");
    parser.addFlagOption("json", "JSON output for tunnel state");
    parser.addValueOption("hold-seconds", "seconds to keep the tunnel open before stopping automatically");
    parser.addValueOption("output", "path to write tunnel state JSON");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();
    parser.handleHelpAndVersionOptions();

    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 5) {
        parser.showError("host, port, token, and busid are required");
    }
    if (posArgs.length() > 6) {
        parser.showError("too many positional arguments");
    }

    bool ok = false;
    int port = posArgs.at(2).toInt(&ok);
    if (!ok || !inRange(port, 1, 65535)) {
        parser.showError(QString("Invalid port value: %1").arg(posArgs.at(2)));
    }

    int exporterPort = 3240;
    if (posArgs.length() >= 6) {
        exporterPort = posArgs.at(5).toInt(&ok);
        if (!ok || !inRange(exporterPort, 1, 65535)) {
            parser.showError(QString("Invalid exporter-port value: %1").arg(posArgs.at(5)));
        }
    }

    m_Host = posArgs.at(1);
    m_Port = static_cast<quint16>(port);
    m_Token = posArgs.at(3);
    m_BusId = posArgs.at(4);
    m_ExporterPort = static_cast<quint16>(exporterPort);
    m_HoldSeconds = -1;
    if (parser.isSet("hold-seconds")) {
        int holdSeconds = parser.value("hold-seconds").toInt(&ok);
        if (!ok || !inRange(holdSeconds, 0, 86400)) {
            parser.showError(QString("Invalid hold-seconds value: %1").arg(parser.value("hold-seconds")));
        }
        m_HoldSeconds = holdSeconds;
    }
    m_Bind = parser.isSet("bind");
    m_Json = parser.isSet("json");
    m_OutputPath = parser.value("output");
}

QString UsbLabTunnelCommandLineParser::getHost() const
{
    return m_Host;
}

quint16 UsbLabTunnelCommandLineParser::getPort() const
{
    return m_Port;
}

QString UsbLabTunnelCommandLineParser::getToken() const
{
    return m_Token;
}

QString UsbLabTunnelCommandLineParser::getBusId() const
{
    return m_BusId;
}

quint16 UsbLabTunnelCommandLineParser::getExporterPort() const
{
    return m_ExporterPort;
}

int UsbLabTunnelCommandLineParser::getHoldSeconds() const
{
    return m_HoldSeconds;
}

bool UsbLabTunnelCommandLineParser::shouldBind() const
{
    return m_Bind;
}

bool UsbLabTunnelCommandLineParser::isJson() const
{
    return m_Json;
}

QString UsbLabTunnelCommandLineParser::getOutputPath() const
{
    return m_OutputPath;
}

UsbLabInstallCommandLineParser::UsbLabInstallCommandLineParser()
    : m_DryRun(false),
      m_Json(false)
{
}

UsbLabInstallCommandLineParser::~UsbLabInstallCommandLineParser()
{
}

void UsbLabInstallCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Check or launch USB passthrough dependency setup for lab validation."
    );
    parser.addPositionalArgument("usb-lab-install", "check or launch USB passthrough dependency setup");
    parser.addFlagOption("dry-run", "checking dependency setup without launching installers or administrator prompts");
    parser.addFlagOption("json", "JSON output for install state");
    parser.addValueOption("output", "path to write install state JSON");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();
    parser.handleHelpAndVersionOptions();

    auto posArgs = parser.positionalArguments();
    if (posArgs.length() > 1) {
        parser.showError("too many positional arguments");
    }

    m_DryRun = parser.isSet("dry-run");
    m_Json = parser.isSet("json");
    m_OutputPath = parser.value("output");
    if (!m_OutputPath.isEmpty()) {
        m_Json = true;
    }
}

bool UsbLabInstallCommandLineParser::isDryRun() const
{
    return m_DryRun;
}

bool UsbLabInstallCommandLineParser::isJson() const
{
    return m_Json;
}

QString UsbLabInstallCommandLineParser::getOutputPath() const
{
    return m_OutputPath;
}

UsbLabExportCommandLineParser::UsbLabExportCommandLineParser()
    : m_HoldSeconds(-1),
      m_Bind(false),
      m_Json(false)
{
}

UsbLabExportCommandLineParser::~UsbLabExportCommandLineParser()
{
}

void UsbLabExportCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "Share a local USB device through the client USB/IP exporter for lab validation."
    );
    parser.addPositionalArgument("usb-lab-export", "share USB device for lab validation");
    parser.addPositionalArgument("busid", "Local USB device bus ID", "<busid>");
    parser.addFlagOption("bind", "binding/sharing the device before exporting it");
    parser.addFlagOption("json", "JSON output for export state");
    parser.addValueOption("hold-seconds", "seconds to keep the exporter open before stopping automatically");
    parser.addValueOption("output", "path to write export state JSON");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();
    parser.handleHelpAndVersionOptions();

    auto posArgs = parser.positionalArguments();
    if (posArgs.length() < 2) {
        parser.showError("busid is required");
    }
    if (posArgs.length() > 2) {
        parser.showError("too many positional arguments");
    }

    m_BusId = posArgs.at(1);
    bool ok = false;
    m_HoldSeconds = -1;
    if (parser.isSet("hold-seconds")) {
        int holdSeconds = parser.value("hold-seconds").toInt(&ok);
        if (!ok || !inRange(holdSeconds, 0, 86400)) {
            parser.showError(QString("Invalid hold-seconds value: %1").arg(parser.value("hold-seconds")));
        }
        m_HoldSeconds = holdSeconds;
    }
    m_Bind = parser.isSet("bind");
    m_Json = parser.isSet("json");
    m_OutputPath = parser.value("output");
}

QString UsbLabExportCommandLineParser::getBusId() const
{
    return m_BusId;
}

int UsbLabExportCommandLineParser::getHoldSeconds() const
{
    return m_HoldSeconds;
}

bool UsbLabExportCommandLineParser::shouldBind() const
{
    return m_Bind;
}

bool UsbLabExportCommandLineParser::isJson() const
{
    return m_Json;
}

QString UsbLabExportCommandLineParser::getOutputPath() const
{
    return m_OutputPath;
}

UsbLabListCommandLineParser::UsbLabListCommandLineParser()
    : m_Json(false),
      m_TestExporter(false)
{
}

UsbLabListCommandLineParser::~UsbLabListCommandLineParser()
{
}

void UsbLabListCommandLineParser::parse(const QStringList &args)
{
    CommandLineParser parser;
    parser.setupCommonOptions();
    parser.setApplicationDescription(
        "\n"
        "List USB passthrough client dependency status and local USB devices for lab validation."
    );
    parser.addPositionalArgument("usb-lab-list", "list USB passthrough client status and devices");
    parser.addFlagOption("json", "JSON output");
    parser.addFlagOption("test-exporter", "a localhost USB/IP exporter connectivity probe");
    parser.addValueOption("output", "path to write command output instead of stdout");

    if (!parser.parse(args)) {
        parser.showError(parser.errorText());
    }

    parser.handleUnknownOptions();
    parser.handleHelpAndVersionOptions();

    auto posArgs = parser.positionalArguments();
    if (posArgs.length() > 1) {
        parser.showError("too many positional arguments");
    }

    m_Json = parser.isSet("json");
    m_TestExporter = parser.isSet("test-exporter");
    m_OutputPath = parser.value("output");
}

bool UsbLabListCommandLineParser::isJson() const
{
    return m_Json;
}

bool UsbLabListCommandLineParser::shouldTestExporter() const
{
    return m_TestExporter;
}

QString UsbLabListCommandLineParser::getOutputPath() const
{
    return m_OutputPath;
}
