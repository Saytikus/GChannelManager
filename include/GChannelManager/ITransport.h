#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include "GChannelManager_global.h"

// =====================================================================
//  Контракт транспорта.
//  Реализации (сериал-порт, UDP, RUDP...) пишутся отдельно и НЕ входят
//  в этот файл. Гейтвей знает только об этом интерфейсе.
//
//  Транспорт асинхронный: send() лишь ставит данные в очередь отправки,
//  а входящие байты приходят через сигнал bytesReceived().
// =====================================================================
class GCHANNELMANAGER_EXPORT ITransport : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Closed,   // закрыт
        Opening,  // открывается
        Open,     // готов к обмену
        Closing,  // закрывается
        Error     // ошибка
    };
    Q_ENUM(State)

    explicit ITransport(QObject *parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    [[nodiscard]] virtual State   state() const = 0;
    [[nodiscard]] virtual QString name()  const = 0;   // для логов/диагностики
    [[nodiscard]] bool isOpen() const { return state() == State::Open; }

public slots:
    // Открыть/закрыть канал связи. Асинхронно: результат — через opened()/closed().
    virtual void open()  = 0;
    virtual void close() = 0;

    // Поставить данные в очередь отправки.
    // Возвращает кол-во принятых байт или -1 при ошибке.
    virtual qint64 send(const QByteArray &data) = 0;

signals:
    void stateChanged(ITransport::State state);
    void opened();
    void closed();
    void bytesReceived(const QByteArray &data);   // сырые входящие байты
    void errorOccurred(const QString &message);
};
