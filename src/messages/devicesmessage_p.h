#ifndef QTDATASYNC_DEVICESMESSAGE_P_H
#define QTDATASYNC_DEVICESMESSAGE_P_H

#include <tuple>

#include <QtCore/QUuid>
#include <QtCore/QList>

#include "message_p.h"

namespace QtDataSync {

class Q_DATASYNC_EXPORT ListDevicesMessage : public Message
{
	Q_GADGET

protected:
	const QMetaObject *getMetaObject() const override;
};

class Q_DATASYNC_EXPORT DevicesMessage : public Message
{
	Q_GADGET

	Q_PROPERTY(QList<QtDataSync::DevicesMessage::DeviceInfo> devices MEMBER devices)

public:
	using DeviceInfo = std::tuple<QUuid, Utf8String, QByteArray>; // (deviceid, name, fingerprint)

	QList<DeviceInfo> devices;

protected:
	const QMetaObject *getMetaObject() const override;
};

}

Q_DECLARE_METATYPE(QtDataSync::ListDevicesMessage)
Q_DECLARE_METATYPE(QtDataSync::DevicesMessage)
Q_DECLARE_METATYPE(QtDataSync::DevicesMessage::DeviceInfo)

#endif // QTDATASYNC_DEVICESMESSAGE_P_H
