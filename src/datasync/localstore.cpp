#include "localstore_p.h"
#include "datastore.h"
#include "changecontroller_p.h"
#include "synchelper_p.h"

#include <QtCore/QUrl>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <QtCore/QTemporaryFile>
#include <QtCore/QGlobalStatic>
#include <QtCore/QCoreApplication>
#include <QtCore/QSaveFile>

#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

using namespace QtDataSync;

#define QTDATASYNC_LOG _logger
#define SCOPE_ASSERT() Q_ASSERT_X(scope.database.isValid(), Q_FUNC_INFO, "Cannot use SyncScope after committing it")

Q_GLOBAL_STATIC(LocalStoreEmitter, emitter)

LocalStore::LocalStore(QObject *parent) :
	LocalStore(DefaultSetup, parent)
{}

LocalStore::LocalStore(const QString &setupName, QObject *parent) :
	QObject(parent),
	_defaults(setupName),
	_logger(_defaults.createLogger(staticMetaObject.className(), this)),
	_database(_defaults.aquireDatabase(this)),
	_tableNameCache(),
	_dataCache(_defaults.property(Defaults::CacheSize).toInt())
{
	connect(emitter, &LocalStoreEmitter::dataChanged,
			this, &LocalStore::onDataChange,
			Qt::DirectConnection);
	connect(emitter, &LocalStoreEmitter::dataResetted,
			this, &LocalStore::onDataReset,
			Qt::DirectConnection);

	//make shure the primary table exists
	QWriteLocker _(_defaults.databaseLock());
	if(!_database->tables().contains(QStringLiteral("DataIndex"))) {
		QSqlQuery createQuery(_database);
		createQuery.prepare(QStringLiteral("CREATE TABLE DataIndex ("
										   "	Type		TEXT NOT NULL,"
										   "	Id			TEXT NOT NULL,"
										   "	Version		INTEGER NOT NULL,"
										   "	File		TEXT,"
										   "	Checksum	BLOB,"
										   "	Changed		INTEGER NOT NULL DEFAULT 1,"
										   "	PRIMARY KEY(Type, Id)"
										   ") WITHOUT ROWID;"));
		if(!createQuery.exec()) {
			throw LocalStoreException(_defaults,
									  QByteArrayLiteral("any"),
									  createQuery.executedQuery().simplified(),
									  createQuery.lastError().text());
		}
	}
}

LocalStore::~LocalStore() {}

QJsonObject LocalStore::readJson(const ObjectKey &key, const QString &fileName, int *costs) const
{
	QFile file(filePath(key, fileName));
	if(!file.open(QIODevice::ReadOnly))
		throw LocalStoreException(_defaults, key, file.fileName(), file.errorString());

	auto doc = QJsonDocument::fromBinaryData(file.readAll());
	if(costs)
		*costs = file.size();
	file.close();

	if(!doc.isObject())
		throw LocalStoreException(_defaults, key, file.fileName(), QStringLiteral("File contains invalid json data"));
	return doc.object();
}

quint64 LocalStore::count(const QByteArray &typeName)
{
	QReadLocker _(_defaults.databaseLock());

	QSqlQuery countQuery(_database);
	countQuery.prepare(QStringLiteral("SELECT Count(*) FROM DataIndex WHERE Type = ? AND File IS NOT NULL"));
	countQuery.addBindValue(typeName);
	exec(countQuery, typeName);

	if(countQuery.first())
		return countQuery.value(0).toInt();
	else
		return 0;
}

QStringList LocalStore::keys(const QByteArray &typeName)
{
	QReadLocker _(_defaults.databaseLock());

	QSqlQuery keysQuery(_database);
	keysQuery.prepare(QStringLiteral("SELECT Id FROM DataIndex WHERE Type = ? AND File IS NOT NULL"));
	keysQuery.addBindValue(typeName);
	exec(keysQuery, typeName);

	QStringList resList;
	while(keysQuery.next())
		resList.append(keysQuery.value(0).toString());
	return resList;
}

QList<QJsonObject> LocalStore::loadAll(const QByteArray &typeName)
{
	QReadLocker _(_defaults.databaseLock());

	QSqlQuery loadQuery(_database);
	loadQuery.prepare(QStringLiteral("SELECT Id, File FROM DataIndex WHERE Type = ? AND File IS NOT NULL"));
	loadQuery.addBindValue(typeName);
	exec(loadQuery, typeName);

	QList<QJsonObject> array;
	while(loadQuery.next()) {
		int size;
		ObjectKey key {typeName, loadQuery.value(0).toString()};
		auto json = readJson(key, loadQuery.value(1).toString(), &size);
		array.append(json);
		_dataCache.insert(key, new QJsonObject(json), size);
	}
	return array;
}

QJsonObject LocalStore::load(const ObjectKey &key)
{
	QReadLocker _(_defaults.databaseLock());

	//check if cached
	auto data = _dataCache.object(key);
	if(data)
		return *data;

	QSqlQuery loadQuery(_database);
	loadQuery.prepare(QStringLiteral("SELECT File FROM DataIndex WHERE Type = ? AND Id = ? AND File IS NOT NULL"));
	loadQuery.addBindValue(key.typeName);
	loadQuery.addBindValue(key.id);
	exec(loadQuery, key);

	if(loadQuery.first()) {
		int size;
		auto json = readJson(key, loadQuery.value(0).toString(), &size);
		_dataCache.insert(key, new QJsonObject(json), size);
		return json;
	} else
		throw NoDataException(_defaults, key);
}

void LocalStore::save(const ObjectKey &key, const QJsonObject &data)
{
	//scope for the lock
	{
		QWriteLocker _(_defaults.databaseLock());

		if(!_database->transaction())
			throw LocalStoreException(_defaults, key, _database->databaseName(), _database->lastError().text());

		try {
			//check if the file exists
			QSqlQuery existQuery(_database);
			existQuery.prepare(QStringLiteral("SELECT Version, File FROM DataIndex WHERE Type = ? AND Id = ?"));
			existQuery.addBindValue(key.typeName);
			existQuery.addBindValue(key.id);
			exec(existQuery, key);

			//create the file device to write to
			quint64 version = 1ull;
			bool existing = existQuery.first();
			if(existing)
				version = existQuery.value(0).toULongLong() + 1ull;

			//perform store operation
			auto resFn = storeChangedImpl(_database,
										  key,
										  version,
										  existing ? existQuery.value(1).toString() : QString(),
										  data,
										  true,
										  existing,
										  _);

			//commit database changes
			if(!_database->commit())
				throw LocalStoreException(_defaults, key, _database->databaseName(), _database->lastError().text());

			//after commit action
			resFn();
		} catch(...) {
			_database->rollback();
			throw;
		}
	}

	//own signal
	emit dataChanged(key, false);
}

bool LocalStore::remove(const ObjectKey &key)
{
	//scope for the lock
	{
		QWriteLocker _(_defaults.databaseLock());

		if(!_database->transaction())
			throw LocalStoreException(_defaults, key, _database->databaseName(), _database->lastError().text());

		try {
			//load data of existing entry
			QSqlQuery loadQuery(_database);
			loadQuery.prepare(QStringLiteral("SELECT Version, File FROM DataIndex WHERE Type = ? AND Id = ? AND File IS NOT NULL"));
			loadQuery.addBindValue(key.typeName);
			loadQuery.addBindValue(key.id);
			exec(loadQuery, key);

			if(loadQuery.first()) { //stored -> remove it
				auto version = loadQuery.value(0).toULongLong() + 1;

				//"remove" from db
				QSqlQuery removeQuery(_database);
				removeQuery.prepare(QStringLiteral("UPDATE DataIndex SET Version = ?, File = NULL, Checksum = NULL, Changed = 1 WHERE Type = ? AND Id = ?"));
				removeQuery.addBindValue(version);
				removeQuery.addBindValue(key.typeName);
				removeQuery.addBindValue(key.id);
				exec(removeQuery, key);

				//delete the file
				QFile rmFile(filePath(key, loadQuery.value(1).toString()));
				if(!rmFile.remove())
					throw LocalStoreException(_defaults, key, rmFile.fileName(), rmFile.errorString());

				//commit db
				if(!_database->commit())
					throw LocalStoreException(_defaults, key, _database->databaseName(), _database->lastError().text());

				//update cache
				_dataCache.remove(key);

				//notify others
				emit emitter->dataChanged(this, key, {}, 0);

				//notify change controller
				ChangeController::triggerDataChange(_defaults, _);
			} else { //not stored -> done
				if(!_database->commit())
					throw LocalStoreException(_defaults, key, _database->databaseName(), _database->lastError().text());

				return false;
			}
		} catch(...) {
			_database->rollback();
			throw;
		}
	}

	//own signal
	emit dataChanged(key, true);
	return true;
}

QList<QJsonObject> LocalStore::find(const QByteArray &typeName, const QString &query)
{
	QReadLocker _(_defaults.databaseLock());

	auto searchQuery = query;
	searchQuery.replace(QLatin1Char('*'), QLatin1Char('%'));
	searchQuery.replace(QLatin1Char('?'), QLatin1Char('_'));

	QSqlQuery findQuery(_database);
	findQuery.prepare(QStringLiteral("SELECT Id, File FROM DataIndex WHERE Type = ? AND Id LIKE ? AND File IS NOT NULL"));
	findQuery.addBindValue(typeName);
	findQuery.addBindValue(searchQuery);
	exec(findQuery, typeName);

	QList<QJsonObject> array;
	while(findQuery.next()) {
		int size;
		ObjectKey key {typeName, findQuery.value(0).toString()};
		auto json = readJson(key, findQuery.value(1).toString(), &size);
		array.append(json);
		_dataCache.insert(key, new QJsonObject(json), size);
	}
	return array;
}

void LocalStore::clear(const QByteArray &typeName)
{
	//scope for the lock
	{
		QWriteLocker _(_defaults.databaseLock());

		if(!_database->transaction())
			throw LocalStoreException(_defaults, typeName, _database->databaseName(), _database->lastError().text());

		try {
			QSqlQuery clearQuery(_database);
			clearQuery.prepare(QStringLiteral("UPDATE DataIndex "
											  "SET Version = Version + 1, File = NULL, Checksum = NULL, Changed = 1 "
											  "WHERE Type = ? AND File IS NOT NULL"));
			clearQuery.addBindValue(typeName);
			exec(clearQuery, typeName);

			auto tableDir = typeDirectory(typeName);
			if(!tableDir.removeRecursively()) {
				logWarning() << "Failed to delete cleared data directory for type"
							 << typeName;
			}

			if(!_database->commit())
				throw LocalStoreException(_defaults, typeName, _database->databaseName(), _database->lastError().text());

			//notify others (and self)
			emit emitter->dataResetted(this, typeName);

			//notify change controller
			ChangeController::triggerDataChange(_defaults, _);
		} catch(...) {
			_database->rollback();
			throw;
		}
	}

	//own signal
	emit dataCleared(typeName);
}

void LocalStore::reset()
{
	//scope for the lock
	{
		QWriteLocker _(_defaults.databaseLock());

		if(!_database->transaction())
			throw LocalStoreException(_defaults, {}, _database->databaseName(), _database->lastError().text());

		try {
			QSqlQuery resetQuery(_database);
			resetQuery.prepare(QStringLiteral("DELETE FROM DataIndex"));
			exec(resetQuery);

			//note: resets are local only, so they dont trigger any changecontroller stuff

			auto tableDir = _defaults.storageDir();
			if(tableDir.cd(QStringLiteral("store"))) {
				if(!tableDir.removeRecursively()) //no rollback, as partially removed is possible, better keep junk data...
					logWarning() << "Failed to delete store directory" << tableDir.absolutePath();
			}

			if(!_database->commit())
				throw LocalStoreException(_defaults, QByteArray("<any>"), _database->databaseName(), _database->lastError().text());
		} catch(...) {
			_database->rollback();
			throw;
		}

		//notify others (and self)
		emit emitter->dataResetted(this);
	}

	//own signal
	emit dataResetted();
}

void LocalStore::loadChanges(int limit, const std::function<bool(ObjectKey, quint64, QString)> &visitor)
{
	QReadLocker _(_defaults.databaseLock());
	QSqlQuery readChangesQuery(_database);
	readChangesQuery.prepare(QStringLiteral("SELECT Type, Id, Version, File FROM DataIndex WHERE CHANGED = 1 LIMIT ?"));
	readChangesQuery.addBindValue(limit);
	exec(readChangesQuery);

	while(readChangesQuery.next()) {
		if(!visitor({readChangesQuery.value(0).toByteArray(), readChangesQuery.value(1).toString()},
					readChangesQuery.value(2).toULongLong(),
					readChangesQuery.value(3).toString()))
			break;
	}
}

void LocalStore::markUnchanged(const ObjectKey &key, quint64 version, bool isDelete)
{
	QWriteLocker _(_defaults.databaseLock());
	markUnchangedImpl(_database, key, version, isDelete, _);
}

LocalStore::SyncScope LocalStore::startSync(const ObjectKey &key)
{
	return SyncScope(_defaults, key, this);
}

std::tuple<LocalStore::ChangeType, quint64, QString, QByteArray> LocalStore::loadChangeInfo(SyncScope &scope)
{
	SCOPE_ASSERT();

	QSqlQuery loadChangeQuery(scope.database);
	loadChangeQuery.prepare(QStringLiteral("SELECT Version, File, Checksum FROM DataIndex WHERE Type = ? AND Id = ?"));
	loadChangeQuery.addBindValue(scope.key.typeName);
	loadChangeQuery.addBindValue(scope.key.id);
	exec(loadChangeQuery);

	if(loadChangeQuery.first()) {
		auto version = loadChangeQuery.value(0).toULongLong();
		auto file = loadChangeQuery.value(1).toString();
		auto checksum = loadChangeQuery.value(2).toByteArray();
		if(file.isNull())
			return std::tuple<LocalStore::ChangeType, quint64, QString, QByteArray>{ExistsDeleted, version, QString(), QByteArray()};
		else
			return std::tuple<LocalStore::ChangeType, quint64, QString, QByteArray>{Exists, version, file, checksum};
	} else
		return std::tuple<LocalStore::ChangeType, quint64, QString, QByteArray>{NoExists, 0, QString(), QByteArray()};
}

void LocalStore::updateVersion(SyncScope &scope, quint64 oldVersion, quint64 newVersion, bool changed)
{
	SCOPE_ASSERT();
	QSqlQuery updateQuery(scope.database);
	updateQuery.prepare(QStringLiteral("UPDATE DataIndex SET Version = ?, Changed = ? WHERE Type = ? AND Id = ? AND Version = ?"));
	updateQuery.addBindValue(newVersion);
	updateQuery.addBindValue(changed);
	updateQuery.addBindValue(scope.key.typeName);
	updateQuery.addBindValue(scope.key.id);
	updateQuery.addBindValue(oldVersion);
	exec(updateQuery, scope.key);

	//notify change controller
	if(changed)
		ChangeController::triggerDataChange(_defaults, scope.lock);
}

void LocalStore::storeChanged(SyncScope &scope, quint64 version, const QString &fileName, const QJsonObject &data, bool changed, LocalStore::ChangeType localState)
{
	SCOPE_ASSERT();
	Q_ASSERT_X(!scope.afterCommit, Q_FUNC_INFO, "Only 1 after commit action can be defined");
	scope.afterCommit = storeChangedImpl(scope.database, scope.key, version, fileName, data, changed, localState != NoExists, scope.lock);
}

void LocalStore::storeDeleted(SyncScope &scope, quint64 version, bool changed, ChangeType localState)
{
	SCOPE_ASSERT();

	QString fileName;
	bool existing;
	switch (localState) {
	case Exists:
	{
		QSqlQuery loadQuery(scope.database);
		loadQuery.prepare(QStringLiteral("SELECT File FROM DataIndex WHERE Type = ? AND Id = ? AND File IS NOT NULL"));
		loadQuery.addBindValue(scope.key.typeName);
		loadQuery.addBindValue(scope.key.id);
		exec(loadQuery, scope.key);

		if(loadQuery.first())
			fileName = filePath(scope.key, loadQuery.value(0).toString());
		Q_FALLTHROUGH();
	}
	case ExistsDeleted:
		existing = true;
		break;
	case NoExists:
		existing = false;
		break;
	default:
		Q_UNREACHABLE();
		break;
	}

	if(existing) {
		QSqlQuery updateQuery(scope.database);
		updateQuery.prepare(QStringLiteral("UPDATE DataIndex SET Version = ?, File = NULL, Checksum = NULL, Changed = ? WHERE Type = ? AND Id = ?"));
		updateQuery.addBindValue(version);
		updateQuery.addBindValue(changed);
		updateQuery.addBindValue(scope.key.typeName);
		updateQuery.addBindValue(scope.key.id);
		exec(updateQuery, scope.key);
	} else {
		QSqlQuery insertQuery(scope.database);
		insertQuery.prepare(QStringLiteral("INSERT INTO DataIndex (Type, Id, Version, File, Checksum, Changed) VALUES(?, ?, ?, NULL, NULL, ?)"));
		insertQuery.addBindValue(scope.key.typeName);
		insertQuery.addBindValue(scope.key.id);
		insertQuery.addBindValue(version);
		insertQuery.addBindValue(changed);
		exec(insertQuery, scope.key);
	}

	//delete the file, if one exists
	if(!fileName.isNull()) {
		QFile rmFile(fileName);
		if(!rmFile.remove())
			throw LocalStoreException(_defaults, scope.key, rmFile.fileName(), rmFile.errorString());
	}

	//notify change controller
	if(changed)
		ChangeController::triggerDataChange(_defaults, scope.lock);

	if(localState == Exists) {
		Q_ASSERT_X(!scope.afterCommit, Q_FUNC_INFO, "Only 1 after commit action can be defined");
		auto key = scope.key;
		scope.afterCommit = [this, key]() {
			//update cache
			_dataCache.remove(key);
			//notify others
			emit emitter->dataChanged(this, key, {}, 0);
		};
	}
}

void LocalStore::markUnchanged(SyncScope &scope, quint64 oldVersion, bool isDelete)
{
	SCOPE_ASSERT();
	markUnchangedImpl(scope.database, scope.key, oldVersion, isDelete, scope.lock);
}

void LocalStore::commitSync(SyncScope &scope)
{
	SCOPE_ASSERT();

	if(!scope.database->commit())
		throw LocalStoreException(_defaults, scope.key, scope.database->databaseName(), scope.database->lastError().text());

	if(scope.afterCommit)
		scope.afterCommit();

	scope.database = DatabaseRef(); //clear the ref, so it won't rollback
	scope.lock.unlock(); //unlock, wont be used anymore
}

int LocalStore::cacheSize() const
{
	return _dataCache.maxCost();
}

void LocalStore::setCacheSize(int cacheSize)
{
	_dataCache.setMaxCost(cacheSize);
}

void LocalStore::resetCacheSize()
{
	_dataCache.setMaxCost(_defaults.property(Defaults::CacheSize).toInt());
}

void LocalStore::onDataChange(QObject *origin, const ObjectKey &key, const QJsonObject &data, int size)
{
	if(origin != this) {
		if(_dataCache.contains(key)) {
			if(data.isEmpty())
				_dataCache.remove(key);
			else
				_dataCache.insert(key, new QJsonObject(data), size);
		}

		QMetaObject::invokeMethod(this, "dataChanged", Qt::QueuedConnection,
								  Q_ARG(QtDataSync::ObjectKey, key),
								  Q_ARG(bool, data.isEmpty()));
	}
}

void LocalStore::onDataReset(QObject *origin, const QByteArray &typeName)
{
	if(typeName.isNull()) {
		_tableNameCache.clear();
		_dataCache.clear();

		if(origin != this)
			QMetaObject::invokeMethod(this, "dataResetted", Qt::QueuedConnection);
	} else {
		_tableNameCache.remove(typeName);
		foreach(auto key, _dataCache.keys()) {
			if(key.typeName == typeName)
				_dataCache.remove(key);
		}

		if(origin != this) {
			QMetaObject::invokeMethod(this, "dataCleared", Qt::QueuedConnection,
									  Q_ARG(QByteArray, typeName));
		}
	}
}

QDir LocalStore::typeDirectory(const ObjectKey &key) const
{
	auto encName = QUrl::toPercentEncoding(QString::fromUtf8(key.typeName))
				   .replace('%', '_');
	auto tName = QStringLiteral("store/data_%1").arg(QString::fromUtf8(encName));
	auto tableDir = _defaults.storageDir();
	if(!tableDir.mkpath(tName) || !tableDir.cd(tName)) {
		throw LocalStoreException(_defaults, key, tName, QStringLiteral("Failed to create directory"));
	} else
		return tableDir;
}

QString LocalStore::filePath(const QDir &typeDir, const QString &baseName) const
{
	return typeDir.absoluteFilePath(baseName + QStringLiteral(".dat"));
}

QString LocalStore::filePath(const ObjectKey &key, const QString &baseName) const
{
	return filePath(typeDirectory(key), baseName);
}

void LocalStore::exec(QSqlQuery &query, const ObjectKey &key) const
{
	if(!query.exec()) {
		throw LocalStoreException(_defaults,
								  key,
								  query.executedQuery().simplified(),
								  query.lastError().text());
	}
}

std::function<void()> LocalStore::storeChangedImpl(const DatabaseRef &db, const ObjectKey &key, quint64 version, const QString &fileName, const QJsonObject &data, bool changed, bool existing, const QWriteLocker &lock)
{
	auto tableDir = typeDirectory(key);
	QScopedPointer<QFileDevice> device;
	std::function<bool(QFileDevice*)> commitFn;

	if(existing && !fileName.isNull()) {
		auto file = new QSaveFile(filePath(tableDir, fileName));
		device.reset(file);
		if(!file->open(QIODevice::WriteOnly))
			throw LocalStoreException(_defaults, key, file->fileName(), file->errorString());
		commitFn = [](QFileDevice *d){
			return static_cast<QSaveFile*>(d)->commit();
		};
	} else {
		auto fileName = QStringLiteral("%1XXXXXX")
						.arg(QString::fromUtf8(QUuid::createUuid().toRfc4122().toHex()));
		auto file = new QTemporaryFile(filePath(tableDir, fileName));
		device.reset(file);
		if(!file->open())
			throw LocalStoreException(_defaults, key, file->fileName(), file->errorString());
		commitFn = [](QFileDevice *d){
			auto f = static_cast<QTemporaryFile*>(d);
			f->close();
			if(f->error() == QFile::NoError) {
				f->setAutoRemove(false);
				return true;
			} else
				return false;
		};
	}

	//write the data & get the hash
	device->write(QJsonDocument(data).toBinaryData());
	if(device->error() != QFile::NoError)
		throw LocalStoreException(_defaults, key, device->fileName(), device->errorString());

	//save key in database
	QFileInfo info(device->fileName());
	if(existing) {
		QSqlQuery updateQuery(db);
		updateQuery.prepare(QStringLiteral("UPDATE DataIndex SET Version = ?, File = ?, Checksum = ?, Changed = ? WHERE Type = ? AND Id = ?"));
		updateQuery.addBindValue(version);
		updateQuery.addBindValue(tableDir.relativeFilePath(info.completeBaseName())); //still update file, in case it was set to NULL
		updateQuery.addBindValue(SyncHelper::jsonHash(data));
		updateQuery.addBindValue(changed);
		updateQuery.addBindValue(key.typeName);
		updateQuery.addBindValue(key.id);
		exec(updateQuery, key);
	} else {
		QSqlQuery insertQuery(db);
		insertQuery.prepare(QStringLiteral("INSERT INTO DataIndex (Type, Id, Version, File, Checksum, Changed) VALUES(?, ?, ?, ?, ?, ?)"));
		insertQuery.addBindValue(key.typeName);
		insertQuery.addBindValue(key.id);
		insertQuery.addBindValue(version);
		insertQuery.addBindValue(tableDir.relativeFilePath(info.completeBaseName()));
		insertQuery.addBindValue(SyncHelper::jsonHash(data));
		insertQuery.addBindValue(changed);
		exec(insertQuery, key);
	}

	//complete the file-save (last before commit!)
	if(!commitFn(device.data()))
		throw LocalStoreException(_defaults, key, device->fileName(), device->errorString());

	//notify change controller
	if(changed)
		ChangeController::triggerDataChange(_defaults, lock);

	return [this, key, data, info]() {
		//update cache
		_dataCache.insert(key, new QJsonObject(data), info.size());
		//notify others
		emit emitter->dataChanged(this, key, data, info.size());
	};
}

void LocalStore::markUnchangedImpl(const DatabaseRef &db, const ObjectKey &key, quint64 version, bool isDelete, const QWriteLocker &)
{
	QSqlQuery completeQuery(db);
	if(isDelete && !_defaults.property(Defaults::PersistDeleted).toBool())
		completeQuery.prepare(QStringLiteral("DELETE FROM DataIndex WHERE Type = ? AND Id = ? AND Version = ? AND File IS NULL"));
	else
		completeQuery.prepare(QStringLiteral("UPDATE DataIndex SET Changed = 0 WHERE Type = ? AND Id = ? AND Version = ?"));
	completeQuery.addBindValue(key.typeName);
	completeQuery.addBindValue(key.id);
	completeQuery.addBindValue(version);
	exec(completeQuery);
}

// ------------- SyncScope -------------

LocalStore::SyncScope::SyncScope(const Defaults &defaults, const ObjectKey &key, LocalStore *owner) :
	key(key),
	database(defaults.aquireDatabase(owner)),
	lock(defaults.databaseLock()),
	afterCommit()
{
	if(!database->transaction())
		throw LocalStoreException(defaults, key, database->databaseName(), database->lastError().text());
}

LocalStore::SyncScope::~SyncScope()
{
	if(database.isValid())
		database->rollback();
}

// ------------- Emitter -------------

LocalStoreEmitter::LocalStoreEmitter(QObject *parent) :
	QObject(parent)
{
	auto coreThread = qApp->thread();
	if(thread() != coreThread)
		moveToThread(coreThread);
}
