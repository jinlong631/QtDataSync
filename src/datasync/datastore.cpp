#include "datastore.h"
#include "datastore_p.h"

#include <QtJsonSerializer/QJsonSerializer>

using namespace QtDataSync;

DataStore::DataStore(QObject *parent) :
	DataStore(DefaultSetup, parent)
{}

DataStore::DataStore(const QString &setupName, QObject *parent) :
	QObject(parent),
	d(new DataStorePrivate(this, setupName))
{}

DataStore::~DataStore() {}

qint64 DataStore::count(int metaTypeId) const
{
	return d->store->count(d->typeName(metaTypeId));
}

QStringList DataStore::keys(int metaTypeId) const
{
	return d->store->keys(d->typeName(metaTypeId));
}

QVariantList DataStore::loadAll(int metaTypeId) const
{
	auto jsonList = d->store->loadAll(d->typeName(metaTypeId));
	QVariantList resList;
	foreach(auto val, jsonList)
		resList.append(d->serializer->deserialize(val, metaTypeId));
	return resList;
}

QVariant DataStore::load(int metaTypeId, const QString &key) const
{
	auto data = d->store->load({d->typeName(metaTypeId), key});
	return d->serializer->deserialize(data, metaTypeId);
}

void DataStore::save(int metaTypeId, QVariant value)
{
	auto typeName = d->typeName(metaTypeId);
	if(!value.convert(metaTypeId))
		throw InvalidDataException(d->defaults, typeName, QStringLiteral("Failed to convert passed variant to the target type"));

	auto meta = QMetaType::metaObjectForType(metaTypeId);
	if(!meta)
		throw InvalidDataException(d->defaults, typeName, QStringLiteral("Type does not have a meta object"));
	auto userProp = meta->userProperty();
	if(!userProp.isValid())
		throw InvalidDataException(d->defaults, typeName, QStringLiteral("Type does not have a user property"));

	QString key;
	auto flags = QMetaType::typeFlags(metaTypeId);
	if(flags.testFlag(QMetaType::IsGadget))
		key = userProp.readOnGadget(value.data()).toString();
	else if(flags.testFlag(QMetaType::PointerToQObject))
		key = userProp.read(value.value<QObject*>()).toString();
	else if(flags.testFlag(QMetaType::SharedPointerToQObject))
		key = userProp.read(value.value<QSharedPointer<QObject>>().data()).toString();
	else if(flags.testFlag(QMetaType::WeakPointerToQObject))
		key = userProp.read(value.value<QWeakPointer<QObject>>().data()).toString();
	else if(flags.testFlag(QMetaType::TrackingPointerToQObject))
		key = userProp.read(value.value<QPointer<QObject>>().data()).toString();
	else
		throw InvalidDataException(d->defaults, typeName, QStringLiteral("Type is neither a gadget nor a pointer to an object"));

	if(key.isEmpty())
		throw InvalidDataException(d->defaults, typeName, QStringLiteral("Failed to convert user property value to a string"));
	auto json = d->serializer->serialize(value);
	if(!json.isObject())
		throw InvalidDataException(d->defaults, typeName, QStringLiteral("Serialization converted to invalid json type. Only json objects are allowed"));
	d->store->save({typeName, key}, json.toObject());
}

bool DataStore::remove(int metaTypeId, const QString &key)
{
	return d->store->remove({d->typeName(metaTypeId), key});
}

QVariantList DataStore::search(int metaTypeId, const QString &query) const
{
	auto jsonList = d->store->find(d->typeName(metaTypeId), query);
	QVariantList resList;
	foreach(auto val, jsonList)
		resList.append(d->serializer->deserialize(val, metaTypeId));
	return resList;
}

void DataStore::iterate(int metaTypeId, const std::function<bool (QVariant)> &iterator, const std::function<void (const QException &)> &onExcept) const
{
	try {
		auto keyList = keys(metaTypeId);
		foreach(auto key, keyList) {
			if(!iterator(load(metaTypeId, key)))
				break;
		}
	} catch (QException &e) {
		if(onExcept)
			onExcept(e);
		else
			throw;
	}
}

// ------------- PRIVATE IMPLEMENTATION -------------

DataStorePrivate::DataStorePrivate(DataStore *q, const QString &setupName) :
	defaults(setupName),
	logger(defaults.createLogger("datastore", q)),
	serializer(defaults.serializer()),
	store(new LocalStore(setupName, q))
{}

QByteArray DataStorePrivate::typeName(int metaTypeId) const
{
	auto name = QMetaType::typeName(metaTypeId);
	if(name)
		return name;
	else
		throw InvalidDataException(defaults, "type_" + QByteArray::number(metaTypeId), QStringLiteral("Not a valid metatype id"));
}

// ------------- Exceptions -------------

DataStoreException::DataStoreException(const Defaults &defaults, const QString &message) :
	Exception(defaults, message)
{}

DataStoreException::DataStoreException(const DataStoreException * const other) :
	Exception(other)
{}



LocalStoreException::LocalStoreException(const Defaults &defaults, const ObjectKey &key, const QString &context, const QString &message) :
	DataStoreException(defaults, message),
	_key(key),
	_context(context)
{}

LocalStoreException::LocalStoreException(const LocalStoreException * const other) :
	DataStoreException(other),
	_key(other->_key),
	_context(other->_context)
{}

ObjectKey LocalStoreException::key() const
{
	return _key;
}

QString LocalStoreException::context() const
{
	return _context;
}

QByteArray LocalStoreException::className() const noexcept
{
	return QTDATASYNC_EXCEPTION_NAME(LocalStoreException);
}

QString LocalStoreException::qWhat() const
{
	QString msg = Exception::qWhat() +
				  QStringLiteral("\n\tContext: %1"
								 "\n\tTypeName: %2")
				  .arg(_context)
				  .arg(QString::fromUtf8(_key.typeName));
	if(!_key.id.isNull())
		msg += QStringLiteral("\n\tKey: %1").arg(_key.id);
	return msg;
}

void LocalStoreException::raise() const
{
	throw (*this);
}

QException *LocalStoreException::clone() const
{
	return new LocalStoreException(this);
}



NoDataException::NoDataException(const Defaults &defaults, const ObjectKey &key) :
	DataStoreException(defaults, QStringLiteral("The requested data does not exist")),
	_key(key)
{}

NoDataException::NoDataException(const NoDataException * const other) :
	DataStoreException(other),
	_key(other->_key)
{}

ObjectKey NoDataException::key() const
{
	return _key;
}

QByteArray NoDataException::className() const noexcept
{
	return QTDATASYNC_EXCEPTION_NAME(NoDataException);
}

QString NoDataException::qWhat() const
{
	return Exception::qWhat() +
			QStringLiteral("\n\tTypeName: %1"
						   "\n\tKey: %2")
			.arg(QString::fromUtf8(_key.typeName))
			.arg(_key.id);
}

void NoDataException::raise() const
{
	throw (*this);
}

QException *NoDataException::clone() const
{
	return new NoDataException(this);
}



InvalidDataException::InvalidDataException(const Defaults &defaults, const QByteArray &typeName, const QString &message) :
	DataStoreException(defaults, message),
	_typeName(typeName)
{}

InvalidDataException::InvalidDataException(const InvalidDataException * const other) :
	DataStoreException(other),
	_typeName(other->_typeName)
{}

QByteArray InvalidDataException::typeName() const
{
	return _typeName;
}

QByteArray InvalidDataException::className() const noexcept
{
	return QTDATASYNC_EXCEPTION_NAME(InvalidDataException);
}

QString InvalidDataException::qWhat() const
{
	return Exception::qWhat() +
			QStringLiteral("\n\tTypeName: %1")
			.arg(QString::fromUtf8(_typeName));
}

void InvalidDataException::raise() const
{
	throw (*this);
}

QException *InvalidDataException::clone() const
{
	return new InvalidDataException(this);
}