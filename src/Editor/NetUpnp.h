#pragma once

#include <Editor/NetSocket.h>

#include <mutex>
#include <string>
#include <thread>

namespace Vortex {

/// Asks the router to forward a port to this computer, using UPnP (Internet
/// Gateway Device). Discovery and mapping run on a worker thread, so that
/// hosting does not wait for the router; the mapping is removed again by
/// stop().
class PortMapper {
   public:
    enum State {
        STATE_IDLE,
        STATE_PENDING,  ///< Looking for the router / adding the mapping.
        STATE_MAPPED,   ///< The port is forwarded.
        STATE_FAILED,   ///< No UPnP router, or it refused the mapping.
    };

    struct Result {
        State state = STATE_IDLE;

        /// The router's external address, and the external port that is
        /// forwarded to the local port (TCP and UDP).
        NetEndpoint external;

        /// Why the mapping failed, if it did.
        std::string error;
    };

    ~PortMapper();

    /// Starts forwarding the given local port (TCP and UDP). The external
    /// port is the same if it is free on the router.
    void start(int port);

    /// Removes the mapping, if any, and stops the worker thread. Blocks for at
    /// most a few seconds.
    void stop();

    Result getResult() const;

   private:
    void myRun(const std::stop_token& st, int port);
    void mySetResult(const Result& result);

    std::jthread myThread;
    mutable std::mutex myMutex;
    Result myResult;
};

};  // namespace Vortex
