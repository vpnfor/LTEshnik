#include <QTest>
#include <QSignalSpy>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>

#include "core/controllers/coreController.h"
#include "ui/controllers/connectionUiController.h"
#include "core/models/api/apiV2ServerConfig.h"
#include "core/models/api/apiConfig.h"
#include "core/utils/constants/configKeys.h"
#include "core/utils/serverConfigUtils.h"
#include "core/utils/containers/containerUtils.h"
#include "vpnConnection.h"
#include "secureQSettings.h"

using namespace amnezia;

// Total wait: kAwgSwitchTimeoutMs + singleShot(1000ms) + margin(1500ms)
static const int kTestWaitMs = ConnectionUiController::kAwgSwitchTimeoutMs + 1000 + 1500;
// Simulated disconnect fires shortly after kAwgSwitchTimeoutMs so the
// singleShot(1000) guard inside onAwgStateTimeout sees isConnectionInProgress == false.
static const int kSimDisconnectMs = ConnectionUiController::kAwgSwitchTimeoutMs + 200;

class TestAwgAutoSwitch : public QObject
{
    Q_OBJECT

private:
    CoreController *m_coreController = nullptr;
    SecureQSettings *m_settings = nullptr;

    // Build a minimal V2 premium AWG server config JSON
    static QJsonObject buildPremiumAwgServerJson()
    {
        // Minimal AWG container config
        QJsonObject clientConfig;
        clientConfig[configKey::clientPrivKey] = "test_client_private_key";
        clientConfig[configKey::clientPubKey]  = "test_client_public_key";
        clientConfig[configKey::serverPubKey]  = "test_server_public_key";
        clientConfig[configKey::pskKey]        = "test_psk_key";
        clientConfig[configKey::clientIp]      = "10.8.1.2";
        clientConfig[configKey::allowedIps]    = QJsonArray::fromStringList({"0.0.0.0/0"});
        clientConfig[configKey::mtu]           = 1280;

        QJsonObject awgConfig;
        awgConfig[configKey::lastConfig]    = QString(QJsonDocument(clientConfig).toJson());
        awgConfig[configKey::port]          = 51820;
        awgConfig[configKey::transportProto] = "udp";

        QJsonObject containerObj;
        containerObj["container"]           = ContainerUtils::containerToString(DockerContainer::Awg);
        containerObj[ContainerUtils::containerToString(DockerContainer::Awg)] = awgConfig;

        QJsonArray containersArray;
        containersArray.append(containerObj);

        // API config: amnezia-premium, empty serviceProtocol = auto mode
        QJsonObject apiConfigObj;
        apiConfigObj["service_type"]     = "amnezia-premium";
        apiConfigObj["service_protocol"] = "";    // empty = auto (AWG default, may switch to VLESS)

        QJsonObject serverJson;
        serverJson[configKey::configVersion]    = serverConfigUtils::ConfigSource::AmneziaGateway;
        serverJson[configKey::name]             = "Test Premium AWG";
        serverJson[configKey::hostName]         = "1.2.3.4";
        serverJson[configKey::containers]       = containersArray;
        serverJson[configKey::defaultContainer] = ContainerUtils::containerToString(DockerContainer::Awg);
        serverJson["api_config"]                = apiConfigObj;

        return serverJson;
    }

private slots:
    void initTestCase()
    {
        QString testOrg = "AmneziaVPN-Test-AwgSwitch-" + QUuid::createUuid().toString();
        m_settings = new SecureQSettings(testOrg, "amnezia-client", nullptr, false);

        auto vpnConnection = QSharedPointer<VpnConnection>::create(nullptr, nullptr);
        m_coreController = new CoreController(vpnConnection, m_settings, nullptr, this);
    }

    void cleanupTestCase()
    {
        m_settings->clearSettings();
        delete m_coreController;
        delete m_settings;
    }

    void init()
    {
        m_settings->clearSettings();
        m_coreController->m_serversRepository->invalidateCache();
    }

    // -----------------------------------------------------------------------
    // Test: AWG stuck in Connecting → auto-switch emits requestSetCurrentProtocol("vless")
    // -----------------------------------------------------------------------
    void testAwgTimeoutSwitchesToVless()
    {
        // 1. Import a fake premium AWG server
        QJsonObject serverJson = buildPremiumAwgServerJson();
        serverConfigUtils::ConfigType configType = serverConfigUtils::configTypeFromJson(serverJson);
        QString serverId = m_coreController->m_serversRepository->addServer(
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            serverJson,
            configType);
        QVERIFY(!serverId.isEmpty());

        m_coreController->m_serversController->setDefaultServer(serverId);
        QCOMPARE(m_coreController->m_serversController->getDefaultServerId(), serverId);

        // 2. Spy on the signal that carries (serverId, protocol)
        QSignalSpy protocolSpy(m_coreController->m_connectionUiController,
                               &ConnectionUiController::requestSetCurrentProtocol);

        // 3. Verify preconditions before triggering
        {
            const QString defId = m_coreController->m_serversController->getDefaultServerId();
            qDebug() << "[AWG test] defaultServerId:" << defId;

            const DockerContainer defContainer = m_coreController->m_serversController->getDefaultContainer(defId);
            qDebug() << "[AWG test] defaultContainer:" << static_cast<int>(defContainer)
                     << "(Awg ==" << static_cast<int>(DockerContainer::Awg) << ")";

            const auto v2 = m_coreController->m_serversController->apiV2Config(defId);
            qDebug() << "[AWG test] hasV2Config:" << v2.has_value();
            if (v2.has_value()) {
                qDebug() << "[AWG test] isPremium:" << v2->isPremium()
                         << "isExternalPremium:" << v2->isExternalPremium()
                         << "serviceProtocol:" << v2->serviceProtocol();
            }
        }

        // Simulate AWG connection stuck in Connecting state
        m_coreController->m_connectionUiController->onConnectionStateChanged(
            Vpn::ConnectionState::Connecting);

        // 4a. After kAwgSwitchTimeoutMs the timer fires and calls closeConnection().
        //     The VPN stub never transitions to Disconnected on its own, so
        //     onAwgStateTimeout's singleShot(1000) guard (isConnectionInProgress) would
        //     return early. Simulate the VPN disconnect at kAwgSwitchTimeoutMs+200ms so
        //     the guard is cleared before the singleShot fires.
        QTimer::singleShot(kSimDisconnectMs, m_coreController->m_connectionUiController,
                           [this]() {
                               m_coreController->m_connectionUiController->onConnectionStateChanged(
                                   Vpn::ConnectionState::Disconnected);
                           });

        // 4b. Wait for: kAwgSwitchTimeoutMs (1s) + singleShot(1s) + margin (1.5s)
        QTest::qWait(kTestWaitMs);

        // 5. Verify auto-switch signal was emitted with "vless"
        QVERIFY2(protocolSpy.count() >= 1,
                 "requestSetCurrentProtocol should be emitted at least once during auto-switch");

        bool foundVless = false;
        for (int i = 0; i < protocolSpy.count(); ++i) {
            if (protocolSpy.at(i).at(1).toString() == QStringLiteral("vless")) {
                foundVless = true;
                QCOMPARE(protocolSpy.at(i).at(0).toString(), serverId);
                break;
            }
        }
        QVERIFY2(foundVless, "requestSetCurrentProtocol should be emitted with 'vless'");
    }

    // -----------------------------------------------------------------------
    // Test: Manual protocol pin (AWG) → timer must NOT start
    // -----------------------------------------------------------------------
    void testPinnedAwgDoesNotAutoSwitch()
    {
        // Server with serviceProtocol = "awg" (user manually pinned)
        QJsonObject serverJson = buildPremiumAwgServerJson();
        QJsonObject apiConfigObj = serverJson["api_config"].toObject();
        apiConfigObj["service_protocol"] = "awg";
        serverJson["api_config"] = apiConfigObj;

        serverConfigUtils::ConfigType configType = serverConfigUtils::configTypeFromJson(serverJson);
        QString serverId = m_coreController->m_serversRepository->addServer(
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            serverJson,
            configType);
        QVERIFY(!serverId.isEmpty());
        m_coreController->m_serversController->setDefaultServer(serverId);

        QSignalSpy protocolSpy(m_coreController->m_connectionUiController,
                               &ConnectionUiController::requestSetCurrentProtocol);

        m_coreController->m_connectionUiController->onConnectionStateChanged(
            Vpn::ConnectionState::Connecting);

        // Simulate disconnect at kAwgSwitchTimeoutMs+200ms so the guard doesn't mask a real signal
        QTimer::singleShot(kSimDisconnectMs, m_coreController->m_connectionUiController,
                           [this]() {
                               m_coreController->m_connectionUiController->onConnectionStateChanged(
                                   Vpn::ConnectionState::Disconnected);
                           });

        // Should NOT auto-switch — timer must not have started at all
        QTest::qWait(kTestWaitMs);

        QVERIFY2(protocolSpy.count() == 0,
                 "No auto-switch should happen when protocol is manually pinned to AWG");
    }
};

QTEST_MAIN(TestAwgAutoSwitch)
#include "testAwgAutoSwitch.moc"
