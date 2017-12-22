#ifndef CHANGECONTROLLER_P_H
#define CHANGECONTROLLER_P_H

#include <QtCore/QObject>
#include <QtCore/QMutex>

#include <QtSql/QSqlDatabase>

#include "qtdatasync_global.h"
#include "objectkey.h"
#include "controller_p.h"

namespace QtDataSync {

class Q_DATASYNC_EXPORT ChangeController : public Controller
{
	Q_OBJECT

public:
	struct Q_DATASYNC_EXPORT ChangeInfo {
		ObjectKey key;
		quint64 version;
		QByteArray checksum;

		ChangeInfo();
		ChangeInfo(const ObjectKey &key, quint64 version, const QByteArray &checksum = {});
	};

	explicit ChangeController(const Defaults &defaults, QObject *parent = nullptr);

	void initialize() final;

	static void triggerDataChange(Defaults defaults, const QWriteLocker &);

public Q_SLOTS:
	void setUploadingEnabled(bool uploading);
	void clearUploads();

	void uploadDone(const QByteArray &key);

Q_SIGNALS:
	void uploadingChanged(bool uploading);
	void uploadChange(const QByteArray &key, const QByteArray &changeData);

private Q_SLOTS:
	void changeTriggered();
	void uploadNext();

private:
	struct Q_DATASYNC_EXPORT UploadInfo {
		ObjectKey key;
		quint64 version;
		bool isDelete;
	};

	class Q_DATASYNC_EXPORT CachedObjectKey : public ObjectKey {
	public:
		CachedObjectKey();
		CachedObjectKey(const ObjectKey &other);
		CachedObjectKey(const QByteArray &hash);

		QByteArray hashed() const;

		bool operator==(const CachedObjectKey &other) const;
		bool operator!=(const CachedObjectKey &other) const;

	private:
		mutable QByteArray _hash;
	};

	static const int UploadLimit;

	DatabaseRef _database;
	bool _uploadingEnabled;

	QHash<CachedObjectKey, UploadInfo> _activeUploads;

	bool canUpload();
	void exec(QSqlQuery &query, const ObjectKey &key = ObjectKey{"any"}) const;
};

}

Q_DECLARE_METATYPE(QtDataSync::ChangeController::ChangeInfo)

#endif // CHANGECONTROLLER_P_H
