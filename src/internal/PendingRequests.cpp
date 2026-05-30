#include "PendingRequests.h"

#include <QTimer>
#include <cmath>

#include "TimerMs.h"

namespace gcm::internal {

PendingRequests::PendingRequests(QObject *parent)
    : QObject(parent)
{}

PendingRequests::~PendingRequests() = default;

quint32 PendingRequests::nextId()
{
    quint32 id = m_nextId++;
    if (m_nextId == 0)
        m_nextId = 1;          // 0 is reserved for keep-alive / fire-and-forget
    return id;
}

std::optional<std::reference_wrapper<PendingRequests::Pending>>
PendingRequests::find(quint32 id)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return std::nullopt;
    return std::ref(it.value());
}

GatewayRequest *PendingRequests::enqueue(const QByteArray &payload, const RetryPolicy &policy)
{
    auto *req = new GatewayRequest(this);
    req->m_id          = nextId();
    req->m_payload     = payload;
    req->m_maxAttempts = 1 + policy.maxRetries;

    if (!m_codec) {
        // should not happen given the correct Gateway call sequence,
        // but guard against it.
        return createPreflightFailed(GatewayRequest::Error::TransportError);
    }

    Pending p;
    p.policy = policy;
    p.frame  = m_codec->encodeRequest(req->m_id, payload);
    p.req    = req;
    p.timer  = new QTimer(this);
    p.timer->setSingleShot(true);

    const quint32 id = req->m_id;
    connect(p.timer, &QTimer::timeout, this, [this, id] { onAttemptTimeout(id); });
    connect(req, &GatewayRequest::cancelRequested, this,
            [this, id] { complete(id, /*ok=*/false, {}, GatewayRequest::Error::Cancelled); });

    m_pending.insert(id, p);

    // First send on the next event-loop iteration: the caller must have
    // time to connect succeeded/failed/retrying.
    QTimer::singleShot(0, this, [this, id] { startAttempt(id); });
    return req;
}

GatewayRequest *PendingRequests::createPreflightFailed(GatewayRequest::Error err)
{
    auto *req = new GatewayRequest(this);
    req->m_id = 0;   // preflight failure — there is no correlation
    QTimer::singleShot(0, this, [this, req, err] {
        req->m_status = GatewayRequest::Status::Failed;
        req->m_error  = err;
        emit requestFailed();
        emit req->failed(err);
        emit req->finished();
        req->deleteLater();
    });
    return req;
}

bool PendingRequests::tryCompleteSuccess(quint32 corrId, const QByteArray &response)
{
    if (!m_pending.contains(corrId))
        return false;
    complete(corrId, /*ok=*/true, response, GatewayRequest::Error::None);
    return true;
}

void PendingRequests::failAll(GatewayRequest::Error err)
{
    const auto ids = m_pending.keys();
    for (quint32 id : ids)
        complete(id, /*ok=*/false, {}, err);
}

void PendingRequests::startAttempt(quint32 id)
{
    auto found = find(id);
    if (!found)
        return;

    if (!m_transport || !m_transport->isOpen()) {
        complete(id, /*ok=*/false, {}, GatewayRequest::Error::TransportError);
        return;
    }

    // Snapshot everything we need before send(): a synchronous (loopback)
    // transport can deliver a reply re-entrantly from inside send(), which
    // runs complete() and erases this Pending. Holding a reference across the
    // call would then dangle. Copy out, send, then re-find for the timer.
    Pending &p = found->get();
    GatewayRequest *req      = p.req;
    const QByteArray frame   = p.frame;
    const RetryPolicy policy = p.policy;

    ++req->m_attempts;
    const auto ms = attemptTimeout(policy, req->m_attempts - 1);

    const qint64 bytes = m_transport->send(frame);
    if (bytes >= 0)
        emit bytesPushed(bytes);

    // send() may have completed (and erased) the request re-entrantly.
    auto still = find(id);
    if (!still)
        return;
    still->get().timer->start(timerMs(ms));
}

void PendingRequests::onAttemptTimeout(quint32 id)
{
    auto found = find(id);
    if (!found)
        return;
    Pending &p = found->get();

    if (p.req->m_attempts >= 1 + p.policy.maxRetries) {
        complete(id, /*ok=*/false, {}, GatewayRequest::Error::Timeout);
    } else {
        emit retryStarted();
        emit p.req->retrying(p.req->m_attempts);
        startAttempt(id);
    }
}

void PendingRequests::complete(quint32 id, bool ok,
                               const QByteArray &response, GatewayRequest::Error err)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
        return;
    Pending p = it.value();
    m_pending.erase(it);

    if (p.timer) { p.timer->stop(); p.timer->deleteLater(); }

    if (ok) {
        p.req->m_response = response;
        p.req->m_status   = GatewayRequest::Status::Succeeded;
        emit requestSucceeded();
        emit p.req->succeeded(response);
    } else {
        p.req->m_status = GatewayRequest::Status::Failed;
        p.req->m_error  = err;
        emit requestFailed();
        emit p.req->failed(err);
    }
    emit p.req->finished();
    p.req->deleteLater();
}

std::chrono::milliseconds
PendingRequests::attemptTimeout(const RetryPolicy &p, qint32 attempt) const
{
    // Floor the factor at 1.0 (no backoff): a value <= 0 would zero the timeout
    // into an immediate-retry storm, and < 1 would shrink it nonsensically.
    const double factor = p.backoffFactor < 1.0 ? 1.0 : p.backoffFactor;
    double t = double(p.timeout.count()) * std::pow(factor, attempt);
    const double cap = double(p.maxTimeout.count());
    if (t > cap)
        t = cap;
    return std::chrono::milliseconds(static_cast<qint64>(std::llround(t)));
}

} // namespace gcm::internal
