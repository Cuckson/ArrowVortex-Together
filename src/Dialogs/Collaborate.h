#pragma once

#include <Dialogs/Dialog.h>

#include <Core/Widgets.h>
#include <Core/WidgetsLayout.h>

#include <vector>

namespace Vortex {

class DialogCollaborate : public EditorDialog {
   public:
    ~DialogCollaborate();
    DialogCollaborate();

    void onTick() override;

   private:
    void myCreateWidgets();
    void onHost();
    void onJoin();
    void onAccept();
    void onCopy();
    void onDisconnect();
    void onNameChanged();
    void onKick();
    void myUpdateGuestList();

    std::string myName;
    std::string myPort;
    std::string myJoinCode;
    std::string myReplyCode;
    std::string myCode;
    std::string myStatus;

    WgDroplist* myGuestList = nullptr;
    int myGuestIndex = 0;
    std::vector<uint32_t> myGuestIds;
    std::vector<std::string> myGuestNames;
};

};  // namespace Vortex
