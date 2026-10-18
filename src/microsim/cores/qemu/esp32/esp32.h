/***************************************************************************
 *   Copyright (C) 2025 by Santiago González                               *
 *                                                                         *
 ***( see copyright.txt file at root folder )*******************************/

#pragma once

#include "qemudevice.h"
#include "qemutwi.h"
#include "esp32pin.h"

class LibraryItem;
//class esp32Pin;
class Esp32Gpio;
class Esp32IoMux;
class Esp32Led;

class Esp32 : public QemuDevice
{
    public:
        Esp32( QString type, QString id, QString device );
        ~Esp32();

        void stamp() override;

        QString extraAP()  { return m_extraAP; }
        void setExtraAP( QString a ){ m_extraAP = a; }

        int  serialPort() { return m_serialPort; }
        void setSerialPort( int p ){ m_serialPort = p; }
        int  httpPort() { return m_httpPort; }
        void setHttpPort( int p ){ m_httpPort = p; }

        bool strapPins() { return m_strapPins; }
        void setStrapPins( bool s ){ m_strapPins = s; }

    protected:
        Pin* addPin( QString id, QString type, QString label,
                    int n, int x, int y, int angle, int length=8, double space=0 ) override;

        bool createArgs() override;

         void updtFrequency() override;

        void createMatrix();

        QString m_extraAP;
        int     m_extraCh;
        bool    m_strapPins;
        int     m_serialPort = 0;
        int     m_httpPort = 0;

        uint32_t m_cpuFreq;
        uint32_t m_apbFreq;

        Esp32Gpio* m_gpio;
        Esp32IoMux* m_iomux;

        Esp32Led* m_leds;
};
