// A data2g-host's command and KISS ports, for the command-line tools:
// blocking Qt sockets, no event loop. Opens the FENNECTV group and hands
// back command events and KISS frames as they come.
#ifndef FENNECTV_HOST_CLIENT_H
#define FENNECTV_HOST_CLIENT_H

#include <QTcpSocket>

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "ragtime/link/Data2GLink.h"

namespace fennectv
{

using ragtime::link::CommandEvent;
using ragtime::link::KissFrame;

class HostClient
{
public:
    // False with error() set if either port won't connect within timeoutMs.
    bool connectTo(const std::string& host, int commandPort, int kissPort, int timeoutMs = 3000);
    const std::string& error() const { return error_; }

    // Sends a command line and waits for the first event that answers it
    // (BCAST PORT, OK, WRONG, MODE lines up to OK...); every event read on
    // the way is kept for poll(). Nothing on a timeout or WRONG.
    std::optional<CommandEvent> command(const std::string& line, int timeoutMs = 3000);

    // BCAST OPEN group [FROM call] -> the KISS port, or -1.
    int openGroup(const std::string& group, const std::string& fromCall = "");
    bool setMode(int port, const std::string& mode, bool dup = false);
    // MODES, as the host lists them.
    std::vector<ragtime::link::ModeInfo> modes();

    void sendKiss(int port, const std::vector<uint8_t>& frame);
    void sendKissAck(int port, uint16_t tag, const std::vector<uint8_t>& frame);

    // Waits up to timeoutMs for anything, then returns all that arrived.
    void poll(int timeoutMs, std::vector<CommandEvent>& events, std::vector<KissFrame>& frames);

    bool connected() const;

private:
    void readAvailable();

    QTcpSocket cmd_, kiss_;
    ragtime::link::LineSplitter lines_;
    ragtime::link::KissDecoder kissDecoder_;
    std::deque<CommandEvent> events_;
    std::deque<std::string> rawLines_;
    std::deque<KissFrame> frames_;
    std::string error_;
};

} // namespace fennectv

#endif
