/***************************************************************************
 *   Remote control server for SimulIDE (local TCP, JSON lines)            *
 *   Added in fork juniorsk88/SimulIDE-dev (feature/controle-remoto)       *
 *                                                                         *
 *   Start:  simulide -remote [port]   (default 5550, listens 127.0.0.1)   *
 *   Protocol: one JSON object per line, one JSON reply per line.          *
 *   {"cmd":"ping"} {"cmd":"load","path":".."} {"cmd":"start"}             *
 *   {"cmd":"stop"} {"cmd":"pause"} {"cmd":"resume"} {"cmd":"status"}      *
 *   {"cmd":"list"} {"cmd":"props","id":".."}                              *
 *   {"cmd":"get","id":"..","prop":".."}                                   *
 *   {"cmd":"set","id":"..","prop":"..","value":".."}                      *
 *   {"cmd":"probe","id":".."}  (Probe component voltage)                  *
 *   ids: "Comp-1" or "Board-1/InnerComp" (component inside subcircuit)    *
 *   {"cmd":"serial","id":"..","clear":true}  (text received by SerialTerm)*
 *   {"cmd":"serial_send","id":"..","text":"LIGA\n"}  (send to the MCU)   *
 *   {"cmd":"ramp","id":"..","prop":"..","from":x,"to":y,"rate":r}         *
 *        r = units per simulated second                                   *
 *   {"cmd":"ramp_stop","id":"..","prop":".."}                             *
 *   {"cmd":"screenshot","path":"..png","fit":true,"area":"circuit"}     *
 *   {"cmd":"view","editor":false,"panel":false,"fit":true,"maximize":1} *
 *   {"cmd":"quit"}                                                      *
 ***( see copyright.txt file at root folder )*******************************/

#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QApplication>
#include <QFileInfo>
#include <cmath>

#include "remotecontrol.h"
#include "mainwindow.h"
#include "circuitview.h"
#include <QTabWidget>
#include <QPixmap>
#include "circuitwidget.h"
#include "circuit.h"
#include "component.h"
#include "comproperty.h"
#include "simulator.h"
#include "probe.h"
#include "subcircuit.h"
#include "serialterm.h"

RemoteControl* RemoteControl::m_pSelf = nullptr;
QMutex RemoteControl::m_serialMutex;
QHash<QString, QByteArray> RemoteControl::m_serial;

RemoteControl::RemoteControl( quint16 port, QObject* parent )
             : QObject( parent )
{
    m_pSelf = this;
    m_server = new QTcpServer( this );
    connect( m_server, &QTcpServer::newConnection, this, &RemoteControl::newConnection );
    if( m_server->listen( QHostAddress::LocalHost, port ) )
        fprintf( stderr, "RemoteControl: listening on 127.0.0.1:%u\n", port );
    else
        fprintf( stderr, "RemoteControl: ERROR listening on port %u: %s\n", port
               , m_server->errorString().toLocal8Bit().constData() );
    fflush( stderr );

    m_rampTimer.setInterval( 50 );
    connect( &m_rampTimer, &QTimer::timeout, this, &RemoteControl::rampTick );
}

void RemoteControl::serialByte( const QString &compId, uint8_t byte )
{
    if( !m_pSelf ) return;   // remote control not enabled
    QMutexLocker lock( &m_serialMutex );
    QByteArray &buf = m_serial[compId];
    buf.append( (char)byte );
    if( buf.size() > 4*1024*1024 ) buf.remove( 0, buf.size()-2*1024*1024 );
}

void RemoteControl::newConnection()
{
    while( m_server->hasPendingConnections() )
    {
        QTcpSocket* s = m_server->nextPendingConnection();
        connect( s, &QTcpSocket::readyRead, this, &RemoteControl::readClient );
        connect( s, &QTcpSocket::disconnected, s, &QObject::deleteLater );
    }
}

void RemoteControl::readClient()
{
    QTcpSocket* s = qobject_cast<QTcpSocket*>( sender() );
    if( !s ) return;
    while( s->canReadLine() )
    {
        QByteArray line = s->readLine().trimmed();
        if( line.isEmpty() ) continue;
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson( line, &err );
        QJsonObject reply;
        if( err.error != QJsonParseError::NoError || !doc.isObject() )
        {
            reply["ok"] = false;
            reply["error"] = "invalid JSON: "+err.errorString();
        }
        else reply = handle( doc.object() );

        s->write( QJsonDocument( reply ).toJson( QJsonDocument::Compact )+"\n" );
        s->flush();
        if( doc.object().value("cmd").toString() == "quit" )
            QTimer::singleShot( 200, qApp, &QApplication::quit );
    }
}

// "id" = top-level component; "subcId/innerId" = component inside a subcircuit/board
Component* RemoteControl::findComponent( QString id )
{
    Circuit* circ = Circuit::self();
    if( !circ ) return nullptr;
    if( !id.contains('/') ) return circ->getCompById( id );
    QString subId = id.section('/', 0, 0 );
    QString inner = id.section('/', 1 );
    SubCircuit* subc = dynamic_cast<SubCircuit*>( circ->getCompById( subId ) );
    if( !subc ) return nullptr;
    for( Component* c : *subc->innerComps() )
        if( c->getUid().section('@', -1 ) == inner || c->idLabel() == inner ) return c;
    return nullptr;
}

double RemoteControl::simTimeS()
{
    if( !Simulator::self() ) return 0;
    return (double)Simulator::self()->circTime()/1e12;
}

static QJsonObject fail( QString msg )
{
    QJsonObject r; r["ok"] = false; r["error"] = msg; return r;
}

QJsonObject RemoteControl::handle( const QJsonObject &c )
{
    QString cmd = c.value("cmd").toString();
    QJsonObject r; r["ok"] = true; r["cmd"] = cmd;

    Circuit* circ = Circuit::self();
    auto findComp = [&]( QString id )->Component* { return findComponent( id ); };

    if( cmd == "ping" ) {
        r["version"] = QApplication::applicationVersion();
    }
    else if( cmd == "load" ) {
        QString path = QFileInfo( c.value("path").toString() ).absoluteFilePath();
        if( !QFileInfo::exists( path ) ) return fail("file not found: "+path );
        m_ramps.clear();
        { QMutexLocker lock( &m_serialMutex ); m_serial.clear(); }
        QString t = MainWindow::self()->windowTitle();      // avoid "save changes?" dialog
        if( t.endsWith('*') ) MainWindow::self()->setWindowTitle( t.left( t.size()-1 ) );
        CircuitWidget::self()->loadCirc( path );
        r["path"] = path;
        r["components"] = circ ? circ->compList()->size() : 0;
    }
    else if( cmd == "start" ) {
        { QMutexLocker lock( &m_serialMutex ); m_serial.clear(); }
        CircuitWidget::self()->powerCircOn();
    }
    else if( cmd == "stop" ) {
        m_ramps.clear();
        CircuitWidget::self()->powerCircOff();
    }
    else if( cmd == "pause" )  Simulator::self()->pauseSim();
    else if( cmd == "resume" ) Simulator::self()->resumeSim();
    else if( cmd == "status" ) {
        r["running"] = Simulator::self()->isRunning();
        r["paused"]  = Simulator::self()->isPaused();
        r["sim_time_s"] = simTimeS();
        r["title"] = MainWindow::self()->windowTitle();
        QJsonArray ra;
        for( const Ramp &rp : m_ramps ) {
            QJsonObject o; o["id"]=rp.id; o["prop"]=rp.prop; o["value"]=rp.value; o["to"]=rp.to;
            ra.append( o );
        }
        r["ramps"] = ra;
    }
    else if( cmd == "list" ) {
        QJsonArray a;
        if( circ ) for( Component* comp : *circ->compList() ) {
            QJsonObject o; o["id"] = comp->getUid(); o["type"] = comp->itemType();
            o["label"] = comp->idLabel();
            if( SubCircuit* sc = dynamic_cast<SubCircuit*>( comp ) ) {
                QJsonArray in;
                for( Component* c : *sc->innerComps() )
                    if( c->itemType() != "Tunnel" && c->itemType() != "Node" )
                        in.append( comp->getUid()+"/"+c->getUid().section('@', -1 ) );
                o["inner"] = in;
            }
            a.append( o );
        }
        r["components"] = a;
    }
    else if( cmd == "props" ) {
        Component* comp = findComp( c.value("id").toString() );
        if( !comp ) return fail("component not found");
        QJsonObject p;
        for( propGroup &g : *comp->properties() )
            for( ComProperty* cp : g.propList )
                p[cp->name()] = comp->getPropStr( cp->name() );
        r["props"] = p;
    }
    else if( cmd == "get" || cmd == "set" ) {
        QString id = c.value("id").toString();
        QString prop = c.value("prop").toString();
        Component* comp = findComp( id );
        if( !comp ) return fail("component not found: "+id );
        if( cmd == "set" ) {
            QJsonValue v = c.value("value");
            QString val = v.isString() ? v.toString()
                        : v.isBool()   ? (v.toBool() ? "true" : "false")
                        : QString::number( v.toDouble(), 'g', 12 );
            m_ramps.remove( id+"/"+prop );
            r["accepted"] = comp->setPropStr( prop, val );
            comp->update();
        }
        r["id"] = id; r["prop"] = prop;
        r["value"] = comp->getPropStr( prop );
    }
    else if( cmd == "probe" ) {
        Probe* p = dynamic_cast<Probe*>( findComp( c.value("id").toString() ) );
        if( !p ) return fail("probe not found");
        r["volt"] = p->voltIn();
    }
    else if( cmd == "serial" ) {
        QMutexLocker lock( &m_serialMutex );
        QString id = c.value("id").toString();
        QByteArray data;
        if( id.isEmpty() ) { for( QByteArray &b : m_serial ) data += b; }
        else data = m_serial.value( id );
        r["text"] = QString::fromLatin1( data );
        if( c.value("clear").toBool() ) {
            if( id.isEmpty() ) m_serial.clear(); else m_serial.remove( id );
        }
    }
    else if( cmd == "serial_send" ) {   // send text to the MCU through a SerialTerm (like typing in its terminal)
        SerialTerm* st = dynamic_cast<SerialTerm*>( findComp( c.value("id").toString() ) );
        if( !st ) return fail("SerialTerm not found");
        st->sendByteArray( c.value("text").toString().toLatin1() );
    }
    else if( cmd == "ramp" ) {
        Ramp rp;
        rp.id = c.value("id").toString();
        rp.prop = c.value("prop").toString();
        Component* comp = findComp( rp.id );
        if( !comp ) return fail("component not found: "+rp.id );
        rp.from = c.contains("from") ? c.value("from").toDouble()
                                     : comp->getPropStr( rp.prop ).split(" ").first().toDouble();
        rp.to = c.value("to").toDouble();
        rp.ratePerS = fabs( c.value("rate").toDouble( 1.0 ) );
        rp.startSimS = simTimeS();
        rp.value = rp.from;
        comp->setPropStr( rp.prop, QString::number( rp.from ) );
        comp->update();
        m_ramps[rp.id+"/"+rp.prop] = rp;
        if( !m_rampTimer.isActive() ) m_rampTimer.start();
        r["id"] = rp.id; r["from"] = rp.from; r["to"] = rp.to; r["rate"] = rp.ratePerS;
    }
    else if( cmd == "ramp_stop" ) {
        m_ramps.remove( c.value("id").toString()+"/"+c.value("prop").toString() );
    }
    else if( cmd == "view" ) {   // {"editor":false,"panel":false,"fit":true,"maximize":true}
        for( QWidget* w : MainWindow::self()->findChildren<QWidget*>() ) {
            QString cn = w->metaObject()->className();
            if( c.contains("editor") && cn == "EditorWindow" ) w->setVisible( c.value("editor").toBool() );
        }
        if( c.contains("panel") ) {
            for( QTabWidget* t : MainWindow::self()->findChildren<QTabWidget*>() )
                if( t->parentWidget() && QString( t->parentWidget()->metaObject()->className() ) == "QSplitter" )
                { t->setVisible( c.value("panel").toBool() ); break; }
        }
        if( c.value("maximize").toBool() ) MainWindow::self()->showMaximized();
        QApplication::processEvents();
        if( c.value("fit").toBool( true ) ) CircuitView::self()->zoomToFit();
        QApplication::processEvents();
    }
    else if( cmd == "screenshot" ) {   // {"path":..,"fit":true,"area":"window"|"circuit"}
        QString path = c.value("path").toString();
        if( c.value("fit").toBool( false ) ) { CircuitView::self()->zoomToFit(); QApplication::processEvents(); }
        QPixmap px = ( c.value("area").toString() == "circuit" ) ? CircuitView::self()->grab()
                                                                  : MainWindow::self()->grab();
        bool ok = px.save( path );
        r["ok"] = ok; r["path"] = path; r["w"] = px.width(); r["h"] = px.height();
    }
    else if( cmd == "quit" ) {
        CircuitWidget::self()->powerCircOff();
    }
    else return fail("unknown cmd: "+cmd );

    return r;
}

void RemoteControl::rampTick()
{
    if( m_ramps.isEmpty() ) { m_rampTimer.stop(); return; }
    if( !Simulator::self()->isRunning() || Simulator::self()->isPaused() ) return;
    double now = simTimeS();
    QStringList done;
    for( auto it = m_ramps.begin(); it != m_ramps.end(); ++it )
    {
        Ramp &rp = it.value();
        Component* comp = findComponent( rp.id );
        if( !comp ) { done.append( it.key() ); continue; }
        double dir = (rp.to >= rp.from) ? 1 : -1;
        double v = rp.from + dir*rp.ratePerS*( now-rp.startSimS );
        if( (dir > 0 && v >= rp.to) || (dir < 0 && v <= rp.to) ) { v = rp.to; done.append( it.key() ); }
        if( fabs( v-rp.value ) >= 0.01 || v == rp.to ) {
            rp.value = v;
            comp->setPropStr( rp.prop, QString::number( v, 'f', 3 ) );
            comp->update();
        }
    }
    for( QString k : done ) m_ramps.remove( k );
}
