/***************************************************************************
 *   Remote control server for SimulIDE (local TCP, JSON lines)            *
 *   Added in fork juniorsk88/SimulIDE-dev (feature/controle-remoto)       *
 ***( see copyright.txt file at root folder )*******************************/

#ifndef REMOTECONTROL_H
#define REMOTECONTROL_H

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QJsonObject>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

class RemoteControl : public QObject
{
    Q_OBJECT
    public:
        RemoteControl( quint16 port, QObject* parent=nullptr );

 static RemoteControl* self() { return m_pSelf; }
 static void serialByte( const QString &compId, uint8_t byte ); // Called from SerialTerm (any thread)

        QJsonObject handle( const QJsonObject &cmd );

    private slots:
        void newConnection();
        void readClient();
        void rampTick();

    private:
        struct Ramp {
            QString id, prop;
            double from, to, ratePerS; // rate in units per simulated second
            double startSimS;
            double value;
        };
        double simTimeS();

        QTcpServer* m_server;
        QTimer m_rampTimer;
        QHash<QString, Ramp> m_ramps; // key: id/prop

 static RemoteControl* m_pSelf;
 static QMutex m_serialMutex;
 static QHash<QString, QByteArray> m_serial;
};
#endif
