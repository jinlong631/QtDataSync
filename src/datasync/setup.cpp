#include "defaults.h"
#include "setup.h"
#include "setup_p.h"
#include "defaults_p.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QLockFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QThreadStorage>

using namespace QtDataSync;

static void initCleanupHandlers();
Q_COREAPP_STARTUP_FUNCTION(initCleanupHandlers)

const QString QtDataSync::DefaultSetup(QStringLiteral("DefaultSetup"));

void Setup::setCleanupTimeout(unsigned long timeout)
{
	SetupPrivate::timeout = timeout;
}

void Setup::removeSetup(const QString &name, bool waitForFinished)
{
	QMutexLocker _(&SetupPrivate::setupMutex);
	if(SetupPrivate::engines.contains(name)) {
		auto &info = SetupPrivate::engines[name];
		if(info.engine) {
			//TODO QMetaObject::invokeMethod(info.engine, "finalize", Qt::QueuedConnection);
			info.engine = nullptr;
		}

		if(waitForFinished) {
			if(!info.thread->wait(SetupPrivate::timeout)) {
				info.thread->terminate();
				info.thread->wait(100);
			}
			info.thread->deleteLater();
			QCoreApplication::processEvents();//required to perform queued events
		}
	}
}

Setup::Setup() :
	d(new SetupPrivate())
{
	d->serializer->setAllowDefaultNull(true);//support null gadgets
}

Setup::~Setup(){}

QString Setup::localDir() const
{
	return d->localDir;
}

QJsonSerializer *Setup::serializer() const
{
	return d->serializer.data();
}

QVariant Setup::property(const QByteArray &key) const
{
	return d->properties.value(key);
}

std::function<void (QString, bool, QString)> Setup::fatalErrorHandler() const
{
	return d->fatalErrorHandler;
}

Setup &Setup::setLocalDir(QString localDir)
{
	d->localDir = localDir;
	return *this;
}

Setup &Setup::setSerializer(QJsonSerializer *serializer)
{
	d->serializer.reset(serializer);
	return *this;
}

Setup &Setup::setProperty(const QByteArray &key, const QVariant &data)
{
	d->properties.insert(key, data);
	return *this;
}

Setup &Setup::setFatalErrorHandler(const std::function<void (QString, bool, QString)> &fatalErrorHandler)
{
	d->fatalErrorHandler = fatalErrorHandler;
	return *this;
}

void Setup::create(const QString &name)
{
	QMutexLocker _(&SetupPrivate::setupMutex);
	if(SetupPrivate::engines.contains(name))
		throw SetupExistsException(name);

	QDir storageDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
	if(!storageDir.cd(d->localDir)) {
		storageDir.mkpath(d->localDir);
		storageDir.cd(d->localDir);
		QFile::setPermissions(storageDir.absolutePath(),
							  QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser);
	}

	auto lockFile = new QLockFile(storageDir.absoluteFilePath(QStringLiteral(".lock")));
	if(!lockFile->tryLock())
		throw SetupLockedException(name);//TODO add extended lock info

	//create defaults
	DefaultsPrivate::createDefaults(name, storageDir, d->properties, d->serializer.take());

	//TODO create engine
	ExchangeEngine *engine = nullptr;

	auto thread = new QThread();
//	engine->moveToThread(thread);
//	QObject::connect(thread, &QThread::started,
//					 engine, &ExchangeEngine::initialize);
	QObject::connect(thread, &QThread::finished,
					 /*engine, &ExchangeEngine::deleteLater);
	QObject::connect(engine, &ExchangeEngine::destroyed,*/ qApp, [lockFile](){
		lockFile->unlock();
		delete lockFile;
	}, Qt::DirectConnection);
	QObject::connect(thread, &QThread::finished, thread, [name, thread](){
		QMutexLocker _(&SetupPrivate::setupMutex);
		SetupPrivate::engines.remove(name);
		DefaultsPrivate::removeDefaults(name);
		thread->deleteLater();
	}, Qt::QueuedConnection);
	thread->start();
	SetupPrivate::engines.insert(name, {thread, engine});
}

// ------------- Private Implementation -------------

QMutex SetupPrivate::setupMutex(QMutex::Recursive);
QHash<QString, SetupPrivate::SetupInfo> SetupPrivate::engines;
unsigned long SetupPrivate::timeout = ULONG_MAX;

void SetupPrivate::cleanupHandler()
{
	QMutexLocker _(&setupMutex);
	foreach (auto info, engines) {
		if(info.engine) {
			//TODO QMetaObject::invokeMethod(info.engine, "finalize", Qt::QueuedConnection);
			info.engine = nullptr;
		}
		//DEBUG
		info.thread->quit();
	}
	foreach (auto info, engines) {
		if(!info.thread->wait(timeout)) {
			info.thread->terminate();
			info.thread->wait(100);
		}
		info.thread->deleteLater();
	}
	engines.clear();
	DefaultsPrivate::clearDefaults();
}

SetupPrivate::SetupPrivate() :
	localDir(QStringLiteral("./qtdatasync_localstore")),
	serializer(new QJsonSerializer()),
	properties(),
	fatalErrorHandler(QtDataSync::defaultFatalErrorHandler)
{}

SetupPrivate::SetupInfo::SetupInfo() :
	SetupInfo(nullptr, nullptr)
{}

SetupPrivate::SetupInfo::SetupInfo(QThread *thread, ExchangeEngine *engine) :
	thread(thread),
	engine(engine)
{}

static QThreadStorage<bool> retryStore;
void QtDataSync::defaultFatalErrorHandler(QString error, bool recoverable, QString setupName)
{
	qCritical().noquote() << "Setup" << setupName << "received fatal error:\n" << error;

	if(recoverable && !retryStore.hasLocalData())
		retryStore.setLocalData(true);
	else
		recoverable = false;

	auto name = setupName.toUtf8();
	//TODO
//	auto engine = SetupPrivate::engine(setupName);
//	if(recoverable && engine) {
//		qCritical("Trying recovery for \"%s\" after fatal error via resync", name.constData());
//		QMetaObject::invokeMethod(engine, "triggerResync", Qt::QueuedConnection);
//	} else
		qFatal("Unrecoverable error for \"%s\" - killing application", name.constData());
}

// ------------- Exceptions -------------

SetupException::SetupException(const QString &setupName, const QString &message) :
	Exception(setupName, message)
{}

SetupException::SetupException(const SetupException * const other) :
	Exception(other)
{}

SetupExistsException::SetupExistsException(const QString &setupName) :
	SetupException(setupName, QStringLiteral("Failed to create setup! A setup with the given name already exists!"))
{}

SetupExistsException::SetupExistsException(const SetupExistsException *const other) :
	SetupException(other)
{}

void SetupExistsException::raise() const
{
	throw (*this);
}

QException *SetupExistsException::clone() const
{
	return new SetupExistsException(this);
}

SetupLockedException::SetupLockedException(const QString &setupName) :
	SetupException(setupName, QStringLiteral("Failed to lock the storage directory! Is it already locked by another process?"))
{}

SetupLockedException::SetupLockedException(const SetupLockedException *const other) :
	SetupException(other)
{}

void SetupLockedException::raise() const
{
	throw (*this);
}

QException *SetupLockedException::clone() const
{
	return new SetupLockedException(this);
}

// ------------- Application hooks -------------

static void initCleanupHandlers()
{
	qRegisterMetaType<QFutureInterface<QVariant>>("QFutureInterface<QVariant>");
	qRegisterMetaType<QThread*>();
	qAddPostRoutine(SetupPrivate::cleanupHandler);
}
