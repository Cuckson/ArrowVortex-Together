#include <Dialogs/Collaborate.h>

#include <Core/Gui.h>
#include <Core/Widgets.h>
#include <Core/WidgetsLayout.h>

#include <Editor/Common.h>
#include <Editor/Session.h>

#include <cstdlib>

namespace Vortex {

DialogCollaborate::~DialogCollaborate() = default;

DialogCollaborate::DialogCollaborate() {
    myPort = std::to_string(Session::DEFAULT_PORT);
    myName = gSession->getUserName();
    setTitle("COLLABORATE");
    myCreateWidgets();
}

void DialogCollaborate::myCreateWidgets() {
    myLayout.row().col(400);
    myLayout.add<WgLabel>()->text.bind(&myStatus);

    myLayout.row().col(72).col(248).col(80);

    WgLineEdit* name = myLayout.add<WgLineEdit>("Name");
    name->text.bind(&myName);
    name->setMaxLength(Session::MAX_NAME_LENGTH);
    name->onChange.bind(this, &DialogCollaborate::onNameChanged);
    name->setTooltip("Your name, shown to the other users next to your cursor");
    myLayout.addBlank();

    WgLineEdit* port = myLayout.add<WgLineEdit>("Port");
    port->text.bind(&myPort);
    port->setNumerical(true);
    port->setMaxLength(5);
    port->setTooltip(
        "Port to host on (TCP and UDP). It is forwarded automatically with "
        "UPnP if your router allows it");

    WgButton* host = myLayout.add<WgButton>();
    host->text.set("Host");
    host->onPress.bind(this, &DialogCollaborate::onHost);
    host->setTooltip("Start a session with the current song");

    WgLineEdit* code = myLayout.add<WgLineEdit>("Your code");
    code->text.bind(&myCode);
    code->setEditable(false);
    code->setTooltip(
        "Host: the join code to give to guests.\n"
        "Guest: if the host could not be reached directly, the reply code to "
        "send back to the host");

    WgButton* copy = myLayout.add<WgButton>();
    copy->text.set("Copy");
    copy->onPress.bind(this, &DialogCollaborate::onCopy);

    WgLineEdit* join = myLayout.add<WgLineEdit>("Join code");
    join->text.bind(&myJoinCode);
    join->setTooltip("The join code you got from the host");

    WgButton* joinButton = myLayout.add<WgButton>();
    joinButton->text.set("Join");
    joinButton->onPress.bind(this, &DialogCollaborate::onJoin);
    joinButton->setTooltip(
        "Join a session (pastes the join code from the clipboard if the field "
        "is empty). Your simfile is replaced by the host's. If you don't have "
        "the host's song open, it is downloaded to the \"saves\" folder "
        "first");

    WgLineEdit* reply = myLayout.add<WgLineEdit>("Reply code");
    reply->text.bind(&myReplyCode);
    reply->setTooltip(
        "Host: the reply code of a guest that could not connect directly");

    WgButton* accept = myLayout.add<WgButton>();
    accept->text.set("Accept");
    accept->onPress.bind(this, &DialogCollaborate::onAccept);
    accept->setTooltip(
        "Connect to the guest through both routers (UDP hole punching). "
        "Pastes the reply code from the clipboard if the field is empty");

    myGuestList = myLayout.add<WgDroplist>("Guest");
    myGuestList->value.bind(&myGuestIndex);
    myGuestList->setTooltip("Guests in the session (host only)");

    WgButton* kick = myLayout.add<WgButton>();
    kick->text.set("Kick");
    kick->onPress.bind(this, &DialogCollaborate::onKick);
    kick->setTooltip("Remove the selected guest from the session");

    myLayout.row().col(400);
    WgButton* disconnect = myLayout.add<WgButton>();
    disconnect->text.set("Disconnect");
    disconnect->onPress.bind(this, &DialogCollaborate::onDisconnect);
}

void DialogCollaborate::onTick() {
    myStatus = gSession->getStatus();
    myCode = gSession->getRole() == Session::ROLE_HOST
                 ? gSession->getJoinCode()
                 : gSession->getReplyCode();
    myUpdateGuestList();
    EditorDialog::onTick();
}

void DialogCollaborate::myUpdateGuestList() {
    std::vector<uint32_t> ids;
    std::vector<std::string> names;
    if (gSession->getRole() == Session::ROLE_HOST) {
        for (const auto& peer : gSession->getPeers()) {
            ids.push_back(peer.id);
            names.push_back(peer.name);
        }
    }
    if (ids == myGuestIds && names == myGuestNames) return;

    // Keep the same guest selected if they are still there.
    uint32_t selected = UINT32_MAX;
    if (myGuestIndex >= 0 &&
        myGuestIndex < static_cast<int>(myGuestIds.size())) {
        selected = myGuestIds[myGuestIndex];
    }
    myGuestIds = ids;
    myGuestNames = names;
    myGuestIndex = 0;
    myGuestList->clearItems();
    for (size_t i = 0; i < names.size(); ++i) {
        myGuestList->addItem(names[i]);
        if (ids[i] == selected) myGuestIndex = static_cast<int>(i);
    }
}

void DialogCollaborate::onKick() {
    if (gSession->getRole() != Session::ROLE_HOST) {
        HudNote("Only the host can remove guests.");
        return;
    }
    if (myGuestIndex < 0 ||
        myGuestIndex >= static_cast<int>(myGuestIds.size())) {
        HudNote("There are no guests to remove.");
        return;
    }
    gSession->kick(myGuestIds[myGuestIndex]);
}

void DialogCollaborate::onNameChanged() { gSession->setUserName(myName); }

void DialogCollaborate::onHost() {
    gSession->setUserName(myName);
    int port = std::atoi(myPort.c_str());
    if (port <= 0 || port > 65535) port = Session::DEFAULT_PORT;
    gSession->host(port);
}

void DialogCollaborate::onJoin() {
    gSession->setUserName(myName);
    if (myJoinCode.empty()) myJoinCode = GuiMain::getClipboardText();
    gSession->join(myJoinCode);
}

void DialogCollaborate::onAccept() {
    if (myReplyCode.empty()) myReplyCode = GuiMain::getClipboardText();
    if (gSession->acceptReply(myReplyCode)) myReplyCode.clear();
}

void DialogCollaborate::onCopy() {
    if (myCode.empty()) {
        HudNote(gSession->getRole() == Session::ROLE_HOST
                    ? "The join code is not ready yet."
                    : "There is no code to copy.");
        return;
    }
    GuiMain::setClipboardText(myCode);
    HudNote("Copied the code to the clipboard.");
}

void DialogCollaborate::onDisconnect() { gSession->disconnect(); }

};  // namespace Vortex
