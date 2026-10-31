#pragma once

#include <Core/Core.h>

#include <Simfile/NoteList.h>

#include <vector>

namespace Vortex {

/// Peer-to-peer co-editing session between one host and any number of guests.
///
/// The host shares a join code that holds its LAN and public addresses (found
/// with STUN; the port is forwarded with UPnP when the router allows it).
/// Guests try all of them directly; if none works, the guest shows a reply
/// code that the host enters, and both sides punch a hole through their
/// routers over UDP. The host relays every change to
/// the other guests. When a guest joins, its simfile is replaced by the host's
/// (metadata, timing and all charts). A guest that does not have the host's
/// song open first downloads the music and simfile to "saves/<song folder>"
/// next to the executable, opens it, and then joins automatically. After that,
/// everyone can edit notes, timing, chart properties and song metadata; changes
/// are applied locally right away and sent to everyone else. Changes received
/// from others do not create history entries, so undo/redo only reverts your
/// own edits.
///
/// All network I/O runs on a worker thread; received messages are handled on
/// the main thread in tick().
struct Session {
    enum Role {
        ROLE_NONE,
        ROLE_HOST,
        ROLE_GUEST,
    };

    enum State {
        STATE_IDLE,
        STATE_LISTENING,    ///< Hosting, waiting for (more) guests.
        STATE_CONNECTING,   ///< Guest connecting to the host.
        STATE_HANDSHAKE,    ///< Connected, verifying the song.
        STATE_DOWNLOADING,  ///< Guest downloading the host's song files.
        STATE_CONNECTED,    ///< Song verified, changes are synced.
    };

    /// Another user in the session, and where they are in the song.
    struct Peer {
        uint32_t id = 0;
        std::string name;
        uint32_t color = 0;

        /// True if they have the same chart open as the local user.
        bool onChart = false;

        /// True if their mouse is over the notefield.
        bool hasHover = false;

        /// Song time under their mouse, in seconds.
        double hoverTime = 0.0;

        /// Horizontal mouse position relative to the receptor center, in
        /// noteskin units (independent of zoom and UI scale).
        double hoverX = 0.0;

        /// Song time of their cursor (the receptor position), in seconds.
        double viewTime = 0.0;
    };

    static const int DEFAULT_PORT = 7654;
    static const int MAX_NAME_LENGTH = 24;

    static void create(XmrNode& settings);
    static void destroy();

    virtual void saveSettings(XmrNode& settings) = 0;

    /// Called every frame by the editor; handles received messages.
    virtual void tick() = 0;

    /// Called by the editor when changes were made to the simfile.
    virtual void onChanges(int changes) = 0;

    /// Starts hosting a session on the given port (TCP and UDP).
    virtual bool host(int port) = 0;

    /// Joins the session of the given join code.
    virtual bool join(const std::string& code) = 0;

    /// Host: connects to a guest that could not connect directly, using the
    /// reply code the guest got.
    virtual bool acceptReply(const std::string& code) = 0;

    /// Ends the current session, if any.
    virtual void disconnect() = 0;

    /// Host: removes a guest from the session.
    virtual void kick(uint32_t id) = 0;

    /// Sets the name that is shown to the other users.
    virtual void setUserName(const std::string& name) = 0;
    virtual const std::string& getUserName() const = 0;

    virtual Role getRole() const = 0;
    virtual State getState() const = 0;

    /// Returns a human readable description of the session state.
    virtual std::string getStatus() const = 0;

    /// Host: returns the code guests use to join, once it is ready.
    virtual std::string getJoinCode() const = 0;

    /// Guest: returns the code to send to the host when the host could not be
    /// reached directly; empty otherwise.
    virtual std::string getReplyCode() const = 0;

    /// Returns the other users in the session.
    virtual const std::vector<Peer>& getPeers() const = 0;

    /// Called by the notes manager after notes were applied to a chart by a
    /// local edit, undo or redo. Sends the change to the other users.
    virtual void onNotesApplied(const Chart* chart, const NoteList& add,
                                const NoteList& rem) = 0;

    /// Draws the mouse cursors, receptors and selected notes of the other
    /// users over the notefield.
    virtual void drawCursors() = 0;
};

extern Session* gSession;

};  // namespace Vortex
