#include "fennectv/host_client.h"

#include <QElapsedTimer>

namespace fennectv
{

namespace rl = ragtime::link;

bool HostClient::connectTo(const std::string& host, int commandPort, int kissPort, int timeoutMs)
{
    cmd_.connectToHost(QString::fromStdString(host), (quint16)commandPort);
    if (!cmd_.waitForConnected(timeoutMs))
    {
        error_ = "command port " + host + ":" + std::to_string(commandPort) + ": " + cmd_.errorString().toStdString();
        return false;
    }
    kiss_.connectToHost(QString::fromStdString(host), (quint16)kissPort);
    if (!kiss_.waitForConnected(timeoutMs))
    {
        error_ = "KISS port " + host + ":" + std::to_string(kissPort) + ": " + kiss_.errorString().toStdString();
        return false;
    }
    return true;
}

bool HostClient::connected() const
{
    return cmd_.state() == QAbstractSocket::ConnectedState && kiss_.state() == QAbstractSocket::ConnectedState;
}

void HostClient::readAvailable()
{
    if (cmd_.bytesAvailable() > 0)
    {
        QByteArray d = cmd_.readAll();
        std::vector<std::string> got;
        lines_.feed(d.constData(), (size_t)d.size(), got);
        for (auto& l : got)
        {
            rawLines_.push_back(l);
            events_.push_back(rl::parseCommandLine(l));
        }
    }
    if (kiss_.bytesAvailable() > 0)
    {
        QByteArray d = kiss_.readAll();
        std::vector<KissFrame> got;
        kissDecoder_.feed(reinterpret_cast<const uint8_t*>(d.constData()), (size_t)d.size(), got);
        for (auto& f : got) frames_.push_back(std::move(f));
    }
}

std::optional<CommandEvent> HostClient::command(const std::string& line, int timeoutMs)
{
    std::string out = line + "\r";
    cmd_.write(out.data(), (qint64)out.size());
    cmd_.flush();
    // Events already queued belong to before this command.
    size_t seen = events_.size();
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs)
    {
        cmd_.waitForReadyRead(20);
        kiss_.waitForReadyRead(0);
        readAvailable();
        for (size_t i = seen; i < events_.size(); i++)
        {
            const CommandEvent& e = events_[i];
            if (e.type == CommandEvent::Type::Wrong) return std::nullopt;
            if (e.type == CommandEvent::Type::Ok || e.type == CommandEvent::Type::BcastPort) return e;
        }
    }
    return std::nullopt;
}

int HostClient::openGroup(const std::string& group, const std::string& fromCall)
{
    std::string line = "BCAST OPEN " + group;
    if (!fromCall.empty()) line += " FROM " + rl::commandCallsign(fromCall);
    auto e = command(line);
    return e && e->type == CommandEvent::Type::BcastPort ? e->number : -1;
}

bool HostClient::setMode(int port, const std::string& mode, bool dup)
{
    return command("BCAST MODE " + std::to_string(port) + (dup ? " DUP " : " ") + mode).has_value();
}

std::vector<rl::ModeInfo> HostClient::modes()
{
    size_t seen = events_.size();
    std::vector<rl::ModeInfo> out;
    if (!command("MODES")) return out;
    for (size_t i = seen; i < events_.size(); i++)
        if (events_[i].type == CommandEvent::Type::ModeLine) out.push_back(events_[i].modeInfo);
    return out;
}

void HostClient::sendKiss(int port, const std::vector<uint8_t>& frame)
{
    auto b = rl::kissEncode(port, frame);
    kiss_.write(reinterpret_cast<const char*>(b.data()), (qint64)b.size());
    kiss_.flush();
}

void HostClient::sendKissAck(int port, uint16_t tag, const std::vector<uint8_t>& frame)
{
    auto b = rl::kissEncodeAckMode(port, tag, frame);
    kiss_.write(reinterpret_cast<const char*>(b.data()), (qint64)b.size());
    kiss_.flush();
}

void HostClient::poll(int timeoutMs, std::vector<CommandEvent>& events, std::vector<KissFrame>& frames)
{
    if (events_.empty() && frames_.empty())
    {
        QElapsedTimer t;
        t.start();
        do
        {
            cmd_.waitForReadyRead(5);
            kiss_.waitForReadyRead(5);
            readAvailable();
        } while (events_.empty() && frames_.empty() && t.elapsed() < timeoutMs);
    }
    else
    {
        readAvailable();
    }
    events.assign(events_.begin(), events_.end());
    frames.assign(frames_.begin(), frames_.end());
    events_.clear();
    rawLines_.clear();
    frames_.clear();
}

} // namespace fennectv
