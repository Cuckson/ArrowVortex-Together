#include <Editor/View.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>

#include <Core/Draw.h>
#include <Core/Gui.h>
#include <Core/Utils.h>
#include <Core/Xmr.h>

#include <System/System.h>

#include <Editor/Action.h>
#include <Editor/Editor.h>
#include <Editor/Menubar.h>
#include <Editor/Music.h>
#include <Managers/ChartMan.h>
#include <Managers/MetadataMan.h>
#include <Managers/NoteskinMan.h>
#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>
#include <Managers/TempoMan.h>

#include <Editor/Common.h>
#include <Editor/Editing.h>
#include <Editor/Minimap.h>
#include <Editor/Notefield.h>
#include <Editor/NotefieldPreview.h>
#include <Editor/Selection.h>
#include <Editor/Waveform.h>

namespace Vortex {
namespace {};  // anonymous namespace

// ================================================================================================
// ViewImpl :: member data.

struct ViewImpl : public View, public InputHandler {
    recti rect_;
    double myChartTopY = 0.0;
    double myPixPerSec = 32;
    double myPixPerRow = 16 * BEATS_PER_ROW;
    int myCursorRow = 0;
    double myCursorTime = 0.0, myCursorBeat = 0.0;
    int myHoveredRow = 0;
    double myHoveredTime = 0.0, myHoveredBeat = 0.0;

    int myReceptorY = 192, myReceptorX = 0, myPreviewOffset = 640;
    double myZoomLevel = 8, myScaleLevel = 4;
    bool myIsDraggingReceptors = false, myIsDraggingReceptorsPreview = false;
    bool myUseTimeBasedView = true;
    bool myUseReverseScroll = false;
    bool myUseChartPreview = false;
    double myCustomSnap = 20;  // The selected custom snap.
    // Custom snaps in the snap list, sorted ascending.
    std::vector<double> myCustomSnaps;
    // Snap sequence as a repeating sequence of row steps; empty when unused.
    std::vector<int> mySnapSequence;
    std::vector<int> mySnapSequenceRows;  // Prefix sums, starting at zero.
    SnapType mySnapType = ST_NONE;

    // ================================================================================================
    // ViewImpl :: constructor / destructor.

    ~ViewImpl() = default;

    ViewImpl() {
        vec2i windowSize = gSystem->getWindowSize();
        rect_ = {0, 0, windowSize.x, windowSize.y};
    }

    // ================================================================================================
    // ViewImpl :: load / save settings.

    void loadSettings(XmrNode& settings) {
        XmrNode* view = settings.child("view");
        if (view) {
            view->get("useTimeBasedView", &myUseTimeBasedView);
            view->get("useReverseScroll", &myUseReverseScroll);
            view->get("useChartPreview", &myUseChartPreview);
            view->get("customSnap", &myCustomSnap);
            view->get("zoomLevel", &myZoomLevel);
            view->get("scaleLevel", &myScaleLevel);
            view->get("receptorX", &myReceptorX);
            view->get("receptorY", &myReceptorY);
            view->get("previewOffset", &myPreviewOffset);

            myCustomSnap = ClampCustomSnap(myCustomSnap);
            const char* snaps = view->get("customSnaps");
            if (snaps) {
                for (double snap : ParseCustomSnaps(snaps)) {
                    insertCustomSnap(snap);
                }
            } else if (!isRegularSnap(myCustomSnap)) {
                // Settings from before the snap list had one custom snap.
                insertCustomSnap(myCustomSnap);
            }
            const char* seq = view->get("customSnapSequence");
            if (seq) applySnapSequence(ParseSnapSequence(seq));
            myZoomLevel = std::clamp(myZoomLevel, -2.0, 16.0);
            myScaleLevel = std::clamp(myScaleLevel, 1.0, 10.0);
        }

        updateScrollValues();
    }

    void saveSettings(XmrNode& settings) override {
        XmrNode* view = settings.child("view");
        if (!view) view = settings.addChild("view");

        view->addAttrib("useTimeBasedView", myUseTimeBasedView);
        view->addAttrib("useReverseScroll", myUseReverseScroll);
        view->addAttrib("useChartPreview", myUseChartPreview);
        view->addAttrib("customSnap", myCustomSnap);
        view->addAttrib("customSnaps",
                        FormatCustomSnaps(myCustomSnaps).c_str());
        view->addAttrib("customSnapSequence",
                        FormatSnapSequence(mySnapSequence).c_str());
        view->addAttrib("zoomLevel", static_cast<long>(myZoomLevel));
        view->addAttrib("scaleLevel", static_cast<long>(myScaleLevel));
        view->addAttrib("receptorX", static_cast<long>(myReceptorX));
        view->addAttrib("receptorY", static_cast<long>(myReceptorY));
        view->addAttrib("previewOffset", static_cast<long>(myPreviewOffset));
    }

    // ================================================================================================
    // ViewImpl :: event handling.

    void onMouseScroll(MouseScroll& evt) override {
        if (evt.handled) return;
        if (gTempo->getTweakMode() &&
            (evt.keyflags & (Keyflag::CTRL | Keyflag::ALT)))
            return;

        if (mySnapType > 0) {
            setCursorRow(snapRow(myCursorRow, evt.up ? SNAP_UP : SNAP_DOWN));
        } else {
            double delta = evt.up ? -128.0 : 128.0;
            if (myUseTimeBasedView) {
                setCursorTime(myCursorTime + delta / myPixPerSec);
            } else {
                setCursorRow(myCursorRow +
                             static_cast<int>(delta / myPixPerRow));
            }
        }
        evt.handled = true;
    }

    void onMousePress(MousePress& evt) override {
        // In mouse mode, left click places notes, so the receptors are locked.
        bool canDrag = !gEditing->hasMouseMode();

        // Dragging the preview receptors.
        if (canDrag && evt.button == Mouse::LMB &&
            isMouseOverReceptorsPreview(evt.x, evt.y) && !evt.handled) {
            myIsDraggingReceptorsPreview = true;
            evt.handled = true;
        }
        // Dragging the receptors.
        else if (canDrag && evt.button == Mouse::LMB &&
                 isMouseOverReceptors(evt.x, evt.y) && !evt.handled) {
            myIsDraggingReceptors = true;
            evt.handled = true;
        }

        if (!evt.handled && evt.button == Mouse::MMB) {
            Vortex::vec2i mouse_pos = gSystem->getMousePos();
            Vortex::ChartOffset ofs = gView->yToOffset(mouse_pos.y);

            setCursorRow(snapRow(offsetToRow(ofs), SnapDir::SNAP_CLOSEST));
            evt.handled = true;
        }
    }

    void onMouseRelease(MouseRelease& evt) override {
        // Finish receptor dragging.
        if (evt.button == Mouse::LMB && myIsDraggingReceptors) {
            myIsDraggingReceptors = false;
        }
        // Finish receptor dragging.
        else if (evt.button == Mouse::LMB && myIsDraggingReceptorsPreview) {
            myIsDraggingReceptorsPreview = false;
        }
    }

    void onKeyPress(KeyPress& evt) override {
        if (evt.handled) return;
        Key::Code kc = evt.key;

        // Starting/pausing the song.
        if (kc == Key::SPACE) {
            if (gMusic->isPaused())
                gMusic->play();
            else
                gMusic->pause();
            evt.handled = true;
            return;
        }
    }

    void onKeyRelease(KeyRelease& evt) override {
        if (evt.handled) return;
    }

    // ================================================================================================
    // ViewImpl :: member function

    double getZoomLevel() const override { return myZoomLevel; }

    double getScaleLevel() const override { return myScaleLevel; }

    double getCustomSnap() const override { return myCustomSnap; }

    const std::vector<double>& getCustomSnaps() const override {
        return myCustomSnaps;
    }

    SnapType getSnapType() const override { return mySnapType; }

    int getSnapQuant() override {
        if (mySnapType < ST_CUSTOM) return 192 / sRowSnapTypes[mySnapType];

        // A sequence repeats on the grid of the greatest common divisor of
        // its steps.
        if (mySnapType == ST_SEQUENCE) {
            int step = 192;
            for (int s : mySnapSequence) step = std::gcd(step, s);
            return 192 / step;
        }

        // A decimal snap repeats after a number of measures; use the smallest
        // per-measure grid that contains every snap position.
        for (int n = 1; n <= 192; ++n) {
            double perMeasure = myCustomSnap * n;
            double rounded = std::round(perMeasure);
            if (rounded > 192.0) break;
            if (std::abs(perMeasure - rounded) < 1e-6) {
                return static_cast<int>(rounded);
            }
        }
        return 192;
    }

    double getSnapsPerMeasure() override {
        if (mySnapType < ST_CUSTOM) return 192.0 / sRowSnapTypes[mySnapType];
        if (mySnapType == ST_CUSTOM) return myCustomSnap;
        // The average number of positions per measure.
        return 192.0 * static_cast<double>(mySnapSequence.size()) /
               mySnapSequenceRows.back();
    }

    // Custom snaps have a precision of two decimals.
    static double ClampCustomSnap(double snap) {
        snap = std::round(snap * 100.0) / 100.0;
        return std::clamp(snap, 1.0, 192.0);
    }

    // Returns the row of the n-th custom snap position. The positions are
    // spaced evenly from row zero, so decimal snaps continue across measures.
    int customSnapRow(int64_t n) const {
        if (mySnapType == ST_SEQUENCE) {
            auto len = static_cast<int64_t>(mySnapSequence.size());
            int64_t cycle = FloorDiv(n, len);
            return static_cast<int>(cycle * mySnapSequenceRows.back() +
                                    mySnapSequenceRows[n - cycle * len]);
        }
        return static_cast<int>(
            std::round(static_cast<double>(n) * 192.0 / myCustomSnap));
    }

    // Returns the index of the last custom snap position on or before row.
    int64_t customSnapIndexBefore(int row) const {
        if (mySnapType == ST_SEQUENCE) {
            int64_t period = mySnapSequenceRows.back();
            int64_t cycle = FloorDiv(row, period);
            auto offset = static_cast<int>(row - cycle * period);
            // Last prefix sum that is on or before the offset.
            auto it = std::upper_bound(mySnapSequenceRows.begin(),
                                       mySnapSequenceRows.end() - 1, offset);
            auto index = static_cast<int64_t>(it - mySnapSequenceRows.begin());
            return cycle * static_cast<int64_t>(mySnapSequence.size()) + index -
                   1;
        }
        auto n = static_cast<int64_t>(
            std::floor(static_cast<double>(row) * myCustomSnap / 192.0));
        while (customSnapRow(n + 1) <= row) ++n;
        while (customSnapRow(n) > row) --n;
        return n;
    }

    static int64_t FloorDiv(int64_t a, int64_t b) {
        int64_t q = a / b;
        return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
    }

    const std::vector<int>& getCustomSnapSequence() const override {
        return mySnapSequence;
    }

    int getCustomSnapRow(int64_t n) const override { return customSnapRow(n); }

    int64_t getCustomSnapIndexBefore(int row) const override {
        return customSnapIndexBefore(row);
    }

    // Returns true if the snap is one of the regular snaps (4th to 192nd).
    static bool isRegularSnap(double snap) {
        for (int i = ST_4TH; i < ST_CUSTOM; ++i) {
            if (192.0 / sRowSnapTypes[i] == snap) return true;
        }
        return false;
    }

    static bool SameSnap(double a, double b) { return std::abs(a - b) < 1e-6; }

    // Returns the snaps per measure of a regular snap type.
    static double RegularSnap(int type) {
        return type == ST_NONE ? 0.0 : 192.0 / sRowSnapTypes[type];
    }

    // Parses a list of custom snaps (e.g. "20 28 12.5").
    static std::vector<double> ParseCustomSnaps(const char* text) {
        std::vector<double> out;
        while (*text) {
            char* end = nullptr;
            double snap = std::strtod(text, &end);
            if (end == text) {
                ++text;
            } else {
                if (snap >= 1.0 && snap <= 192.0) out.push_back(snap);
                text = end;
            }
        }
        return out;
    }

    static std::string FormatCustomSnaps(const std::vector<double>& snaps) {
        std::string out;
        for (double snap : snaps) {
            if (!out.empty()) out += " ";
            out += FormatSnap(snap);
        }
        return out;
    }

    // Inserts a custom snap into the sorted list; returns false if it is a
    // regular snap or already in the list.
    bool insertCustomSnap(double snap) {
        snap = ClampCustomSnap(snap);
        if (isRegularSnap(snap)) return false;
        auto it = std::lower_bound(myCustomSnaps.begin(), myCustomSnaps.end(),
                                   snap - 1e-6);
        if (it != myCustomSnaps.end() && SameSnap(*it, snap)) return false;
        myCustomSnaps.insert(it, snap);
        return true;
    }

    // An entry of the snap list that is scrolled through.
    struct SnapEntry {
        SnapType type;
        double custom;
    };

    // Returns the snap list: none, the regular snaps with the custom snaps
    // sorted in between, then the snap sequence if there is one.
    std::vector<SnapEntry> getSnapList() const {
        std::vector<SnapEntry> list = {{ST_NONE, 0.0}};
        auto custom = myCustomSnaps.begin();
        for (int i = ST_4TH; i < ST_CUSTOM; ++i) {
            for (; custom != myCustomSnaps.end() && *custom < RegularSnap(i);
                 ++custom) {
                list.push_back({ST_CUSTOM, *custom});
            }
            list.push_back({static_cast<SnapType>(i), 0.0});
        }
        if (!mySnapSequence.empty()) list.push_back({ST_SEQUENCE, 0.0});
        return list;
    }

    // Returns the index of the current snap in the snap list. A selected
    // custom snap that is not in the list gives the entry before it.
    int getSnapListIndex(const std::vector<SnapEntry>& list) const {
        int before = 0;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const SnapEntry& e = list[i];
            if (e.type == mySnapType &&
                (e.type != ST_CUSTOM || SameSnap(e.custom, myCustomSnap))) {
                return i;
            }
            if (e.type == ST_SEQUENCE) continue;
            double snap = e.type == ST_CUSTOM ? e.custom : RegularSnap(e.type);
            if (snap < myCustomSnap) before = i;
        }
        return before;
    }

    void selectSnapEntry(const SnapEntry& entry) {
        if (entry.type == ST_CUSTOM) {
            setCustomSnap(entry.custom);
        } else {
            setSnapType(entry.type);
        }
    }

    void stepSnap(int delta) override {
        auto list = getSnapList();
        auto size = static_cast<int>(list.size());
        int current = getSnapListIndex(list);
        // A selected custom snap that is not in the list sits right after
        // the entry before it.
        const SnapEntry& e = list[current];
        bool inList = e.type == mySnapType &&
                      (e.type != ST_CUSTOM || SameSnap(e.custom, myCustomSnap));
        if (!inList && delta < 0) ++current;
        int index = ((current + delta) % size + size) % size;
        selectSnapEntry(list[index]);
    }

    // Stores a sequence of row steps and its prefix sums.
    void applySnapSequence(const std::vector<int>& steps) {
        mySnapSequence = steps;
        mySnapSequenceRows.assign(1, 0);
        for (int step : steps) {
            mySnapSequenceRows.push_back(mySnapSequenceRows.back() + step);
        }
    }

    bool isTimeBased() const override { return myUseTimeBasedView; }

    bool hasReverseScroll() const override { return myUseReverseScroll; }

    bool hasChartPreview() const override { return myUseChartPreview; }

    double getPixPerSec() const override { return myPixPerSec; }

    double getPixPerRow() const override { return myPixPerRow; }

    double getPixPerOfs() const override {
        return myUseTimeBasedView ? myPixPerSec : myPixPerRow;
    }

    int getCursorRow() const override { return myCursorRow; }

    double getCursorTime() const override { return myCursorTime; }

    double getCursorBeat() const override { return myCursorBeat; }

    double getCursorOffset() const override {
        return myUseTimeBasedView ? myCursorTime : myCursorRow;
    }

    int getHoveredRow() const override { return myHoveredRow; }

    double getHoveredTime() const override { return myHoveredTime; }

    double getHoveredBeat() const override { return myHoveredBeat; }

    void onChanges(int changes) override {
        if (changes & VCM_TEMPO_CHANGED) {
            myCursorTime = gTempo->rowToTime(myCursorRow);
        }
    }

    void tick() override {
        vec2i mpos = gSystem->getMousePos();
        vec2i windowSize = gSystem->getWindowSize();
        Coords notefieldCoords = getNotefieldCoords();
        rect_ = {0, 0, windowSize.x, windowSize.y};

        handleInputs(gSystem->getEvents());

        // handle receptor dragging.
        if (myIsDraggingReceptors) {
            myReceptorX = mpos.x - CenterX(rect_);
            myReceptorY = mpos.y - rect_.y;
        }
        // handle preview receptor dragging.
        else if (myIsDraggingReceptorsPreview) {
            auto mx = mpos.x - CenterX(rect_) - myReceptorX;
            auto ofs = (mx << 8) / gSystem->applyScaleFactor(64 * myScaleLevel);
            myPreviewOffset = ofs;
        }

        // Set cursor to arrows when hovering over/dragging the receptors.
        bool canDrag = !gEditing->hasMouseMode();
        if (myIsDraggingReceptors || myIsDraggingReceptorsPreview ||
            (canDrag && (isMouseOverReceptors(mpos.x, mpos.y) ||
                         isMouseOverReceptorsPreview(mpos.x, mpos.y)))) {
            gSystem->setCursor(Cursor::SIZE_ALL);
        }

        // Update the cursor time.
        if (!gMusic->isPaused()) {
            int endrow = gSimfile->getEndRow();
            double begintime = gTempo->rowToTime(0);
            double endtime = gTempo->rowToTime(endrow);
            if (myCursorTime > endtime) {
                myCursorTime = endtime;
                myCursorRow = endrow;
                myCursorBeat = endrow * BEATS_PER_ROW;
                gMusic->pause();
            } else {
                double time = gMusic->getPlayTime();
                myCursorTime = std::clamp(time, begintime, endtime);
                myCursorBeat = gTempo->timeToBeat(myCursorTime);
                myCursorRow = static_cast<int>(myCursorBeat * ROWS_PER_BEAT);
            }
        }

        // Update the cursor row.
        if (mpos.x >= notefieldCoords.xl && mpos.x <= notefieldCoords.xr) {
            auto offsetrow = gView->offsetToRow(gView->yToOffset(mpos.y));
            int row = gView->snapRow(offsetrow, SNAP_CLOSEST);
            if (myHoveredRow != row) {
                myHoveredRow = row;
                myHoveredBeat = static_cast<double>(row) / ROWS_PER_BEAT;
                myHoveredTime = gTempo->rowToTime(row);
            }
        } else {
            myHoveredRow = myHoveredBeat = myHoveredTime = -1;
        }

        // Clamp the receptor X and Y to the view region.
        int minRecepX = rect_.x - rect_.w / 2;
        int maxRecepX = RightX(rect_) - rect_.w / 2;
        myReceptorY = std::clamp(myReceptorY, rect_.y, BottomY(rect_));
        myReceptorX = std::clamp(myReceptorX, minRecepX, maxRecepX);

        // Store the y-position of time zero.
        if (myUseTimeBasedView) {
            myChartTopY = std::floor(static_cast<double>(myReceptorY) -
                                     myCursorTime * myPixPerSec);
        } else {
            myChartTopY =
                std::floor(static_cast<double>(myReceptorY) -
                           myCursorBeat * ROWS_PER_BEAT * myPixPerRow);
        }
    }

    void updateScrollValues() {
        myPixPerSec = std::round(21.077 * std::pow(1.518, myZoomLevel));
        myPixPerRow =
            std::round(11.588 * std::pow(1.48, myZoomLevel)) * BEATS_PER_ROW;
        if (myUseReverseScroll) {
            myPixPerSec = -myPixPerSec;
            myPixPerRow = -myPixPerRow;
        }
    }

    void toggleReverseScroll() override {
        myUseReverseScroll = !myUseReverseScroll;
        myReceptorY = rect_.h - myReceptorY;
        updateScrollValues();
        gMenubar->update(Menubar::USE_REVERSE_SCROLL);
    }

    void toggleChartPreview() override {
        myUseChartPreview = !myUseChartPreview;
        gMenubar->update(Menubar::USE_CHART_PREVIEW);
    }

    void setTimeBased(bool enabled) override {
        if (myUseTimeBasedView != enabled) {
            myUseTimeBasedView = enabled;
            gMenubar->update(Menubar::VIEW_MODE);
            gEditor->reportChanges(VCM_VIEW_CHANGED);
        }
    }

    void setZoomLevel(double level) override {
        level = std::clamp(level, -2.0, 16.0);
        if (myZoomLevel != level) {
            myZoomLevel = level;
            updateScrollValues();
            gEditor->reportChanges(VCM_ZOOM_CHANGED);
        }
    }

    void setScaleLevel(double level) override {
        level = std::clamp(level, 1.0, 10.0);
        if (myScaleLevel != level) {
            myScaleLevel = level;
            gEditor->reportChanges(VCM_ZOOM_CHANGED);
        }
    }

    void setSnapType(int type) override {
        if (type < 0 || type >= NUM_SNAP_TYPES) type = ST_NONE;
        if (type == ST_SEQUENCE && mySnapSequence.empty()) type = ST_NONE;
        if (mySnapType != type) {
            mySnapType = static_cast<SnapType>(type);
            if (mySnapType == ST_SEQUENCE) {
                HudNote("Snap: Sequence (%s)",
                        FormatSnapSequence(mySnapSequence, ", ").c_str());
            } else if (mySnapType == ST_CUSTOM) {
                HudNote("Snap: Custom (%s)", FormatSnap(myCustomSnap).c_str());
            } else {
                HudNote("Snap: %s", ToString(mySnapType));
            }
        }
    }

    void setCustomSnap(double size) override {
        size = ClampCustomSnap(size);
        // If the custom snap is a non-custom value, set the snap to that value
        // instead
        for (int i = ST_4TH; i < ST_CUSTOM; i++) {
            if (192.0 / sRowSnapTypes[i] == size) {
                setSnapType(i);
                return;
            }
        }
        if (myCustomSnap != size || mySnapType != ST_CUSTOM) {
            myCustomSnap = size;
            mySnapType = ST_NONE;  // Shows the snap note for the new value.
            setSnapType(ST_CUSTOM);
        }
    }

    void addCustomSnap(double snap) override {
        if (snap < 1.0 || snap > 192.0) return;
        insertCustomSnap(snap);
        setCustomSnap(snap);
    }

    void removeCustomSnap(double snap) override {
        auto it = std::find_if(myCustomSnaps.begin(), myCustomSnaps.end(),
                               [snap](double s) { return SameSnap(s, snap); });
        if (it == myCustomSnaps.end()) return;
        // If the removed snap is selected, select the entry before it.
        bool selected = mySnapType == ST_CUSTOM && SameSnap(myCustomSnap, *it);
        std::vector<SnapEntry> list = getSnapList();
        int index = getSnapListIndex(list);
        myCustomSnaps.erase(it);
        if (selected) selectSnapEntry(list[std::max(index - 1, 0)]);
    }

    void setCustomSnapSequence(const std::vector<int>& steps) override {
        if (steps.empty()) {
            applySnapSequence({});
            if (mySnapType == ST_SEQUENCE) setSnapType(ST_NONE);
            return;
        }
        if (steps == mySnapSequence && mySnapType == ST_SEQUENCE) return;
        applySnapSequence(steps);
        mySnapType = ST_NONE;  // Shows the snap note for the new sequence.
        setSnapType(ST_SEQUENCE);
    }

    void setCursorTime(double time) override {
        double begintime = gTempo->rowToTime(0);
        double endtime = gTempo->rowToTime(gSimfile->getEndRow());
        myCursorTime = std::clamp(time, begintime, endtime);
        myCursorBeat = gTempo->timeToBeat(myCursorTime);
        myCursorRow = static_cast<int>(myCursorBeat * ROWS_PER_BEAT);
        gMusic->seek(myCursorTime);
    }

    void setCursorRow(int row) override {
        myCursorRow = std::clamp(row, 0, gSimfile->getEndRow());
        myCursorBeat = myCursorRow * BEATS_PER_ROW;
        myCursorTime = gTempo->rowToTime(myCursorRow);
        gMusic->seek(myCursorTime);
    }

    void setCursorOffset(ChartOffset ofs) override {
        if (myUseTimeBasedView) {
            setCursorTime(ofs);
        } else {
            setCursorRow(static_cast<int>(ofs + 0.5));
        }
    }

    void setCursorToNextInterval(int rows) override {
        if (gView->hasReverseScroll()) rows = rows * -1;

        int rowsInInterval = abs(rows);

        if (rows < 0) {
            int delta = myCursorRow % rowsInInterval;
            if (delta == 0 ||
                (delta < rowsInInterval / 2 && !gMusic->isPaused())) {
                delta += rowsInInterval;
            }
            setCursorRow(myCursorRow - delta);
        } else {
            int delta = rowsInInterval - myCursorRow % rowsInInterval;
            setCursorRow(myCursorRow + delta);
        }
    }

    void setCursorToStream(bool top) override {
        if (gNotes->empty()) return;
        if (gView->hasReverseScroll()) top = !top;

        auto first = gNotes->begin(), n = first;
        auto last = gNotes->end() - 1;
        if (first >= last) return;

        if (top) {
            // Advance to the first note on/after the cursor row.
            while (n != last && n->row < myCursorRow) ++n;
            if (n != first) {
                // Go back to the first note before it with different interval.
                int interval = n->row - (n - 1)->row;
                while (n != first && (n->row - (n - 1)->row) == interval) --n;
            }
        } else {
            // Advance to the last note before/on the cursor row.
            while (n != last && (n + 1)->row <= myCursorRow) ++n;
            if (n != last) {
                // Advance to the first note after it with a different interval.
                int interval = n->row - (n + 1)->row;
                while (n != last && (n->row - (n + 1)->row) == interval) ++n;
            }
        }

        setCursorRow(n->row);
    }

    void setCursorToSelection(bool top) override {
        if (gView->hasReverseScroll()) top = !top;

        auto region = gSelection->getSelectedRegion();
        if (!region.isEmpty()) {
            setCursorRow(top ? region.beginRow : region.endRow);
        } else {
            NoteList notes;
            gSelection->getSelectedNotes(notes);
            if (notes.size())
                setCursorRow(top ? notes.begin()->row : (notes.end() - 1)->row);
        }
    }

    int getRowY(int row) const {
        if (myUseTimeBasedView) {
            return static_cast<int>(myChartTopY +
                                    gTempo->rowToTime(row) * myPixPerSec);
        } else {
            return static_cast<int>(myChartTopY +
                                    static_cast<double>(row) * myPixPerRow);
        }
    }

    int getTimeY(double time) const {
        if (myUseTimeBasedView) {
            return static_cast<int>(myChartTopY + time * myPixPerSec);
        } else {
            return static_cast<int>(myChartTopY + gTempo->timeToBeat(time) *
                                                      ROWS_PER_BEAT *
                                                      myPixPerRow);
        }
    }

    Coords getReceptorCoords() const override {
        Coords out;
        auto noteskin = gNoteskin->get();
        out.y = rect_.y + myReceptorY;
        out.xc = rect_.x + rect_.w / 2 + myReceptorX;
        if (noteskin) {
            out.xl =
                out.xc + gSystem->applyScaleFactor(applyZoom(noteskin->leftX));
            out.xr =
                out.xc + gSystem->applyScaleFactor(applyZoom(noteskin->rightX));
        } else {
            int w = gSystem->applyScaleFactor(applyZoom(128));
            out.xl = out.xc - w;
            out.xr = out.xc + w;
        }
        return out;
    }

    Coords getNotefieldCoords() const override {
        Coords out = getReceptorCoords();
        int ofs = gView->applyZoom(32);
        out.xl -= ofs, out.xr += ofs;
        if (myUseTimeBasedView && gNotefield->hasShowWaveform()) {
            int w = gWaveform->getWidth() / 2;
            out.xl = std::min(out.xl, out.xc - w - 4);
            out.xr = std::max(out.xr, out.xc + w + 4);
        }
        return out;
    }

    int columnToX(int col) const override {
        auto noteskin = gNoteskin->get();
        int cx = rect_.x + rect_.w / 2 + myReceptorX;
        if (!noteskin) return cx;
        int x = (col < gStyle->getNumCols()) ? noteskin->colX[col] : 0;
        return cx + gSystem->applyScaleFactor(applyZoom(x));
    }

    int rowToY(int row) const override {
        if (myUseTimeBasedView) {
            return static_cast<int>(myChartTopY +
                                    gTempo->rowToTime(row) * myPixPerSec);
        } else {
            return static_cast<int>(myChartTopY +
                                    static_cast<double>(row) * myPixPerRow);
        }
    }

    int timeToY(double time) const override {
        if (myUseTimeBasedView) {
            return static_cast<int>(myChartTopY + time * myPixPerSec);
        } else {
            return static_cast<int>(myChartTopY + gTempo->timeToBeat(time) *
                                                      ROWS_PER_BEAT *
                                                      myPixPerRow);
        }
    }

    int offsetToY(ChartOffset ofs) override {
        if (myUseTimeBasedView) {
            return static_cast<int>(myChartTopY + ofs * myPixPerSec);
        } else {
            return static_cast<int>(myChartTopY + ofs * myPixPerRow);
        }
    }

    ChartOffset yToOffset(int viewY) const override {
        return (static_cast<ChartOffset>(viewY) - myChartTopY) / getPixPerOfs();
    }

    ChartOffset rowToOffset(int row) const override {
        return myUseTimeBasedView ? gTempo->rowToTime(row)
                                  : static_cast<ChartOffset>(row);
    }

    ChartOffset timeToOffset(double time) const override {
        return myUseTimeBasedView ? time
                                  : (gTempo->timeToBeat(time) * ROWS_PER_BEAT);
    }

    int offsetToRow(ChartOffset ofs) const override {
        return myUseTimeBasedView ? gTempo->timeToRow(ofs)
                                  : static_cast<int>(ofs + 0.5);
    }

    double offsetToTime(ChartOffset ofs) const override {
        return myUseTimeBasedView ? ofs
                                  : gTempo->beatToTime(ofs * BEATS_PER_ROW);
    }

    int getWidth() const override { return rect_.w; }

    int getHeight() const override { return rect_.h; }

    const recti& getRect() const override { return rect_; }

    void adjustForPreview(bool enabled) override {
        myReceptorX +=
            applyZoom(enabled ? -myPreviewOffset : myPreviewOffset) / 2;
    }

    int getPreviewOffset() const override {
        return gSystem->applyScaleFactor(applyZoom(myPreviewOffset));
    }

    int applyZoom(int v) const override {
        return (v * static_cast<int>(64 * myScaleLevel)) >> 8;
    }

    int getNoteScale() const override { return 64 * myScaleLevel; }

    int snapRow(int row, SnapDir dir) override {
        if (dir == SNAP_CLOSEST) {
            if (isAlignedToSnap(row)) {
                return row;
            } else {
                int uprow = snapRow(row, SNAP_UP);
                int downrow = snapRow(row, SNAP_DOWN);
                int up = abs(row - uprow), down = abs(row - downrow);
                return (up < down) ? uprow : downrow;
            }
        } else {
            // Up and down are switched when reverse scroll is enabled.
            if (gView->hasReverseScroll()) {
                switch (dir) {
                    case SNAP_UP:
                        dir = SNAP_DOWN;
                        break;
                    case SNAP_DOWN:
                        dir = SNAP_UP;
                        break;
                    default:
                        break;
                };
            }

            // Bump the row by one so the snap will jump to the next position if
            // the given row is equal to the snap row.
            row = row + ((dir == SNAP_UP) ? -1 : 1);

            // Custom snaps and sequences.
            if (mySnapType >= ST_CUSTOM) {
                int64_t n = customSnapIndexBefore(row);
                if (dir != SNAP_UP && customSnapRow(n) < row) ++n;
                row = customSnapRow(n);
            } else  // Regular case, snap is divisible by 192.
            {
                int snap = sRowSnapTypes[mySnapType];
                if (row % snap && dir != SNAP_UP) row += snap;
                row -= row % snap;
            }

            return row;
        }
    }

    bool isAlignedToSnap(int row) override {
        int snap = sRowSnapTypes[mySnapType];

        // Special case, custom snapping.
        if (snap == 0) {
            return customSnapRow(customSnapIndexBefore(row)) == row;
        }

        return (row % snap == 0);
    }

    bool isMouseOverReceptors(int x, int y) const {
        if (!GuiMain::isCapturingMouse()) {
            auto c = getReceptorCoords();
            int dy = applyZoom(gChart->isClosed() ? 8 : 32);
            return (x >= c.xl && x < c.xr && abs(y - myReceptorY) <= dy);
        }
        return false;
    }

    bool isMouseOverReceptorsPreview(int x, int y) const {
        if (!GuiMain::isCapturingMouse()) {
            auto c = getReceptorCoords();
            int dy = applyZoom(gChart->isClosed() ? 8 : 32);
            return (x >= c.xl + getPreviewOffset() &&
                    x < c.xr + getPreviewOffset() &&
                    abs(y - gNotefieldPreview->getY()) <= dy);
        }
        return false;
    }

};  // ViewImpl

// ================================================================================================
// View API.

View* gView = nullptr;

void View::create(XmrNode& settings) {
    gView = new ViewImpl;
    static_cast<ViewImpl*>(gView)->loadSettings(settings);
}

void View::destroy() {
    delete static_cast<ViewImpl*>(gView);
    gView = nullptr;
}

};  // namespace Vortex
