#include <QCoreApplication>
#include <QDebug>
#include <QRandomGenerator>
#include <QTimer>

#include <GChannelManager/Gateway.h>
#include <GChannelManager/SimpleFrameCodec.h>

// =====================================================================
//  ДЕМО-ТРАНСПОРТ — НЕ часть контракта.
//  Имитирует узел-ответчик: отвечает на запросы и keep-alive,
//  случайно "теряет" ~40% запросов, чтобы было видно работу повторов.
//  Ваш реальный SerialTransport/UdpTransport реализует тот же ITransport.
// =====================================================================
class DemoPeerTransport : public ITransport
{
    Q_OBJECT
public:
    using ITransport::ITransport;

    State   state() const override { return m_state; }
    QString name()  const override { return QStringLiteral("demo-loopback"); }

public slots:
    void open() override
    {
        m_state = State::Open;
        emit stateChanged(m_state);
        emit opened();
    }
    void close() override
    {
        m_state = State::Closed;
        emit stateChanged(m_state);
        emit closed();
    }
    qint64 send(const QByteArray &data) override
    {
        if (m_state != State::Open)
            return -1;

        m_buf.append(data);
        for (const auto &f : SimpleFrameCodec::parse(m_buf)) {
            if (f.type == SimpleFrameCodec::KeepAliveReq) {
                respond(SimpleFrameCodec::makeFrame(SimpleFrameCodec::KeepAliveReply, 0, {}), 15);
            } else if (f.type == SimpleFrameCodec::Request) {
                if (QRandomGenerator::global()->bounded(100) < 40) {
                    qInfo() << "[peer] drop request" << f.corrId;
                    continue;                                   // потеря пакета
                }
                const QByteArray reply = QByteArray("ACK:") + f.payload;
                respond(SimpleFrameCodec::makeFrame(SimpleFrameCodec::Reply, f.corrId, reply), 30);
            }
        }
        return data.size();
    }

private:
    void respond(const QByteArray &frame, qint32 delayMs)
    {
        QTimer::singleShot(delayMs, this, [this, frame] {
            if (m_state == State::Open)
                emit bytesReceived(frame);
        });
    }

    State      m_state = State::Closed;
    QByteArray m_buf;
};

#include "demo_peer.moc"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Gateway gw;

    // 1) кодек протокола
    gw.setCodec(std::make_unique<SimpleFrameCodec>());

    // 2) создать -> сконфигурировать -> установить транспорт
    //    (реально это был бы SerialTransport{SerialConfig{...}} и т.п.)
    gw.setTransport(std::make_unique<DemoPeerTransport>());

    // 3) настройка повторов
    Gateway::RetryPolicy retry;
    retry.maxRetries    = 4;
    retry.timeout       = std::chrono::milliseconds(300);
    retry.backoffFactor = 1.5;
    gw.setDefaultRetryPolicy(retry);

    // 4) настройка keep-alive (поддержка редкой связи)
    Gateway::KeepAliveConfig ka;
    ka.enabled   = true;
    ka.interval  = std::chrono::milliseconds(1000);
    ka.maxMissed = 3;
    gw.setKeepAliveConfig(ka);

    QObject::connect(&gw, &Gateway::channelStateChanged, [&](Gateway::ChannelState s) {
        qInfo() << "channel ->" << qint32(s);
        if (s == Gateway::ChannelState::Enabled)
            gw.startSession();
    });

    QObject::connect(&gw, &Gateway::sessionStateChanged, [&](Gateway::SessionState s) {
        qInfo() << "session ->" << qint32(s);
        static bool sent = false;
        if (s == Gateway::SessionState::Active && !sent) {
            sent = true;
            for (qint32 i = 1; i <= 3; ++i) {
                auto *r = gw.sendRequest(QByteArray("PING-") + QByteArray::number(i));
                QObject::connect(r, &GatewayRequest::succeeded, [i](const QByteArray &resp) {
                    qInfo() << "  req" << i << "OK:" << resp;
                });
                QObject::connect(r, &GatewayRequest::retrying, [i](qint32 a) {
                    qInfo() << "  req" << i << "retry #" << a;
                });
                QObject::connect(r, &GatewayRequest::failed, [i](GatewayRequest::Error e) {
                    qInfo() << "  req" << i << "FAILED, error =" << qint32(e);
                });
            }
        }
    });

    QObject::connect(&gw, &Gateway::keepAliveEnabledChanged, [](bool on) {
        qInfo() << "keep-alive ->" << (on ? "ON" : "OFF");
    });

    gw.enableChannel();

    // демонстрация управления keep-alive на лету:
    QTimer::singleShot(3500, &gw, [&] { gw.setKeepAliveEnabled(false); });
    QTimer::singleShot(5500, &gw, [&] { gw.setKeepAliveEnabled(true);  });

    QTimer::singleShot(8000, &app, &QCoreApplication::quit);
    return app.exec();
}
