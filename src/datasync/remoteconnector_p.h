#ifndef REMOTECONNECTOR_P_H
#define REMOTECONNECTOR_P_H

#include <chrono>

#include <QtCore/QObject>
#include <QtCore/QUuid>
#include <QtCore/QTimer>

#include <QtWebSockets/QWebSocket>

#include "qtdatasync_global.h"
#include "controller_p.h"
#include "defaults.h"
#include "cryptocontroller_p.h"
#include "accountmanager.h"

#include "errormessage_p.h"
#include "identifymessage_p.h"
#include "accountmessage_p.h"
#include "welcomemessage_p.h"
#include "changemessage_p.h"
#include "changedmessage_p.h"
#include "devicesmessage_p.h"
#include "removemessage_p.h"
#include "proofmessage_p.h"
#include "grantmessage_p.h"
#include "devicechangemessage_p.h"

class ConnectorStateMachine;

namespace QtDataSync {

class Q_DATASYNC_EXPORT ExportData
{
public:
	ExportData();

	bool trusted;
	QByteArray pNonce;
	QUuid partnerId;
	QByteArray scheme; //scheme used for the cmac
	QByteArray cmac; //over pNonce, partnerId and scheme, with secret (shared pw-derived) key
	QSharedPointer<RemoteConfig> config;

	//in case of trusted: must be additionally encrypted with the same key. Must the contain (trusted, scheme, salt, data)

	QByteArray signData() const;
};

class Q_DATASYNC_EXPORT RemoteConnector : public Controller
{
	Q_OBJECT

	Q_PROPERTY(bool syncEnabled READ isSyncEnabled WRITE setSyncEnabled NOTIFY syncEnabledChanged)
	Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName RESET resetDeviceName NOTIFY deviceNameChanged)

public:
	static const QString keyRemoteEnabled;
	static const QString keyRemoteConfig;
	static const QString keyRemoteUrl;
	static const QString keyAccessKey;
	static const QString keyHeaders;
	static const QString keyKeepaliveTimeout;
	static const QString keyDeviceId;
	static const QString keyDeviceName;
	static const QString keyImport;
	static const QString keyImportKey;
	static const QString keyImportNonce;
	static const QString keyImportPartner;
	static const QString keyImportScheme;
	static const QString keyImportCmac;

	enum RemoteEvent {
		RemoteDisconnected,
		RemoteConnecting,
		RemoteReady,
		RemoteReadyWithChanges
	};
	Q_ENUM(RemoteEvent)

	explicit RemoteConnector(const Defaults &defaults, QObject *parent = nullptr);

	CryptoController *cryptoController() const;

	void initialize(const QVariantHash &params) final;
	void finalize() final;

	std::tuple<ExportData, QByteArray, CryptoPP::SecByteBlock> exportAccount(bool includeServer, const QString &password); // (exportdata, salt, key)

	bool isSyncEnabled() const;
	QString deviceName() const;

public Q_SLOTS:
	void reconnect();
	void disconnect();
	void resync();

	void listDevices();
	void removeDevice(const QUuid &deviceId);
	void resetAccount(bool removeConfig);
	void prepareImport(const ExportData &data, const CryptoPP::SecByteBlock &key);
	void loginReply(const QUuid &deviceId, bool accept);

	void uploadData(const QByteArray &key, const QByteArray &changeData);
	void uploadDeviceData(const QByteArray &key, const QUuid &deviceId, const QByteArray &changeData);
	void downloadDone(const quint64 key);

	void setSyncEnabled(bool syncEnabled);
	void setDeviceName(const QString &deviceName);
	void resetDeviceName();

Q_SIGNALS:
	void finalized();
	void remoteEvent(RemoteEvent event);
	void uploadDone(const QByteArray &key);
	void deviceUploadDone(const QByteArray &key, const QUuid &deviceId);
	void downloadData(const quint64 key, const QByteArray &changeData);
	void prepareAddedData(const QUuid &deviceId);

	void syncEnabledChanged(bool syncEnabled);
	void deviceNameChanged(const QString &deviceName);
	void devicesListed(const QList<DeviceInfo> &devices);
	void loginRequested(const DeviceInfo &deviceInfo);

private Q_SLOTS:
	void connected();
	void disconnected();
	void binaryMessageReceived(const QByteArray &message);
	void error(QAbstractSocket::SocketError error);
	void sslErrors(const QList<QSslError> &errors);
	void ping();

	//statemachine
	void doConnect();
	void doDisconnect();
	void scheduleRetry();
	void onEntryIdleState();
	void onExitActiveState();

private:
	static const QVector<std::chrono::seconds> Timeouts;

	CryptoController *_cryptoController;

	QWebSocket *_socket;

	QTimer *_pingTimer;
	bool _awaitingPing;

	ConnectorStateMachine *_stateMachine;
	int _retryIndex;
	bool _expectChanges;
	//TODO timeout for server downloads?

	QUuid _deviceId;
	mutable QList<DeviceInfo> _deviceCache;
	QHash<QByteArray, CryptoPP::SecByteBlock> _exportsCache;
	QHash<QUuid, QSharedPointer<AsymmetricCryptoInfo>> _activeProofs;

	bool isIdle() const;
	void triggerError(bool canRecover);

	bool checkCanSync(QUrl &remoteUrl);
	bool loadIdentity();
	void tryClose();
	std::chrono::seconds retry();

	QVariant sValue(const QString &key) const;
	RemoteConfig loadConfig() const;
	void storeConfig(const RemoteConfig &config);

	void onError(const ErrorMessage &message);
	void onIdentify(const IdentifyMessage &message);
	void onAccount(const AccountMessage &message, bool checkState = true);
	void onWelcome(const WelcomeMessage &message);
	void onGrant(const GrantMessage &message);
	void onChangeAck(const ChangeAckMessage &message);
	void onDeviceChangeAck(const DeviceChangeAckMessage &message);
	void onChanged(const ChangedMessage &message);
	void onChangedInfo(const ChangedInfoMessage &message);
	void onLastChanged(const LastChangedMessage &message);
	void onDevices(const DevicesMessage &message);
	void onRemoved(const RemovedMessage &message);
	void onProof(const ProofMessage &message);
};

}

#endif // REMOTECONNECTOR_P_H
