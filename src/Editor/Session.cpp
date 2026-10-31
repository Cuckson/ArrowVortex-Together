#include <Editor/Session.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_stdinc.h>

#include <Core/ByteStream.h>
#include <Core/Draw.h>
#include <Core/Text.h>
#include <Core/Utils.h>
#include <Core/Xmr.h>

#include <Editor/Common.h>
#include <Editor/Editor.h>
#include <Editor/History.h>
#include <Editor/Music.h>
#include <Editor/NetSocket.h>
#include <Editor/NetUdp.h>
#include <Editor/NetUpnp.h>
#include <Editor/Notefield.h>
#include <Editor/Selection.h>
#include <Editor/TextOverlay.h>
#include <Editor/View.h>

#include <Managers/ChartMan.h>
#include <Managers/NoteMan.h>
#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>
#include <Managers/TempoMan.h>

#include <Simfile/Chart.h>
#include <Simfile/SegmentGroup.h>
#include <Simfile/Simfile.h>
#include <Simfile/Tempo.h>

#include <System/File.h>
#include <System/System.h>

namespace Vortex {
namespace {

// ================================================================================================
// Protocol.
//
// Every frame is [u32 size][u8 type][payload], where size counts the type
// byte and the payload. Values are written in native (little-endian) byte
// order using WriteStream; note lists use NoteList::encode.
//
// The host relays everything: guests only talk to the host, and the host
// forwards changes to the other guests, tagged with the id of the user that
// made them ("origin"). The host is user 0; guests get ids from 1 upwards.

const uint32_t PROTOCOL_MAGIC = 0x504D5641;  // "AVMP"
const uint16_t PROTOCOL_VERSION = 4;
const uint32_t MAX_FRAME_SIZE = 64 * 1024 * 1024;

const uint32_t HOST_ID = 0;
const int MAX_GUESTS = 16;

enum MessageType : uint8_t {
    MSG_HELLO = 1,              // G->H: magic, version, name, song info.
    MSG_WELCOME = 2,            // H->G: ok, reason, guest id.
    MSG_FULL_STATE = 3,         // H->G: song sections and all charts.
    MSG_PEER_LIST = 4,          // H->G: count, [id, name].
    MSG_NOTE_EDIT = 5,          // both: origin, chart, add, rem, checksum.
    MSG_NOTE_REPLACE = 6,       // both: origin, chart, notes, checksum.
    MSG_STATE = 7,              // both: origin, section, id, data.
    MSG_PRESENCE = 8,           // both: origin, chart, hover, times.
    MSG_RESYNC_REQUEST = 9,     // G->H: chart.
    MSG_DISCONNECT = 10,        // both: reason.
    MSG_SELECTION = 11,         // both: origin, chart, region, selected notes.
    MSG_DOWNLOAD_REQUEST = 12,  // G->H: need music.
    MSG_FILE_DATA = 13,         // H->G: kind, name, size, offset, data.
    MSG_DOWNLOAD_DONE = 14,     // H->G: simfile name, music name.
};

// Result of a join request, sent in the welcome message.
enum WelcomeResult : uint8_t {
    WELCOME_REJECTED = 0,
    WELCOME_OK = 1,
    WELCOME_DOWNLOAD = 2,  // The guest needs to download the song first.
};

// Kinds of files sent to a guest that downloads the song.
enum FileKind : uint8_t {
    FILE_MUSIC = 0,
    FILE_SIMFILE = 1,
    FILE_IMAGE = 2,
};

// Song and chart data other than notes is synced as "sections": each section
// is serialized as a whole, and the last version received by the host wins.
enum SectionType : uint8_t {
    SECTION_SONG_META = 1,    // id 0: title, artist, etc.
    SECTION_TEMPO = 2,        // id 0: song timing, otherwise chart timing.
    SECTION_CHART_PROPS = 3,  // id: chart step artist, difficulty, meter.
};

const int CONNECT_TIMEOUT_MS = 5000;
const int POLL_INTERVAL_MS = 20;
const int STOP_GRACE_MS = 1000;
const int PUNCH_DELAY_MS = 1500;
const int PUNCH_INTERVAL_MS = 250;
const int PUNCH_TIMEOUT_MS = 10 * 60 * 1000;
const int JOIN_CODE_WAIT_MS = 8000;
const int PRESENCE_INTERVAL_MS = 33;
const int SELECTION_INTERVAL_MS = 100;

const uint32_t MAX_SELECTION_SIZE = 1000000;
const uint32_t FILE_CHUNK_SIZE = 1024 * 1024;
const uint64_t MAX_MUSIC_SIZE = 1024ULL * 1024 * 1024;
const uint64_t MAX_OTHER_FILE_SIZE = 32ULL * 1024 * 1024;
const int REJOIN_TIMEOUT_SECONDS = 120;

typedef std::vector<uint8_t> Bytes;
typedef std::chrono::steady_clock Clock;

static uint64_t HashBytes(const uint8_t* data, size_t size) {
    // 64-bit FNV-1a.
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t HashBytes(const Bytes& bytes) {
    return HashBytes(bytes.data(), bytes.size());
}

static Bytes ToBytes(const WriteStream& out) {
    return Bytes(out.data(), out.data() + out.size());
}

static void WriteBytes(WriteStream& out, const Bytes& bytes) {
    out.writeNum(static_cast<uint32_t>(bytes.size()));
    if (!bytes.empty()) out.write(bytes.data(), static_cast<int>(bytes.size()));
}

static Bytes ReadBytes(ReadStream& in) {
    uint32_t size = in.readNum();
    if (!in.success() || size > in.bytesleft()) {
        in.invalidate();
        return Bytes();
    }
    Bytes out(in.pos(), in.pos() + size);
    in.skip(static_cast<int>(size));
    return out;
}

// ================================================================================================
// Song verification.

struct SongInfo {
    bool hasSong = false;
    uint64_t audioHash = 0;
    double duration = 0.0;
    uint64_t audioSize = 0;
};

static bool HashFile(const fs::path& path, uint64_t& out,
                     uint64_t* size = nullptr) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    // 64-bit FNV-1a.
    uint64_t hash = 14695981039346656037ULL;
    uint64_t total = 0;
    std::vector<char> buffer(1 << 16);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto n = static_cast<size_t>(file.gcount());
        for (size_t i = 0; i < n; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ULL;
        }
        total += n;
    }
    out = hash;
    if (size) *size = total;
    return true;
}

static bool GetLocalSongInfo(SongInfo& out, std::string& error) {
    const Simfile* sim = gSimfile->get();
    if (!sim) {
        error = "Open a simfile first.";
        return false;
    }
    if (sim->music.empty()) {
        error = "The simfile has no music file.";
        return false;
    }
    if (!gMusic->getSamples().isCompleted()) {
        error = "Wait for the music to finish loading.";
        return false;
    }
    fs::path path = utf8ToPath(sim->dir) / utf8ToPath(sim->music);
    if (!HashFile(path, out.audioHash, &out.audioSize)) {
        error = "Could not read the music file.";
        return false;
    }
    out.duration = gMusic->getSongLength();
    out.hasSong = true;
    return true;
}

// ================================================================================================
// Song download.

// Returns the name if it is a plain file name that is safe to create in the
// download folder, or an empty string otherwise.
static std::string SafeFileName(const std::string& name) {
    if (name.empty() || name.size() > 200 || name == "." || name == "..") {
        return std::string();
    }
    for (char c : name) {
        auto u = static_cast<unsigned char>(c);
        if (u < 32 || std::strchr("<>:\"/\\|?*", c)) return std::string();
    }
    if (name.back() == '.' || name.back() == ' ') return std::string();
    return name;
}

// Turns the host's song folder name into a folder name that is safe to use.
static std::string SafeFolderName(const std::string& name) {
    std::string out;
    for (char c : name) {
        auto u = static_cast<unsigned char>(c);
        out.push_back(u < 32 || std::strchr("<>:\"/\\|?*", c) ? '_' : c);
    }
    if (out.size() > 100) out.resize(100);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
        out.erase(0, 1);
    }
    return out.empty() ? std::string("song") : out;
}

// Downloaded songs go next to the executable, like the settings. When that is
// read-only (e.g. a system-wide install on Linux), the user's data folder is
// used instead.
static fs::path SavesDir() {
    fs::path dir = utf8ToPath(gSystem->getRunDir()) / "saves";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!ec) return dir;
    char* pref = SDL_GetPrefPath(nullptr, "ArrowVortex");
    if (!pref) return dir;
    fs::path out = utf8ToPath(pref) / "saves";
    SDL_free(pref);
    return out;
}

static bool IsSimfileName(const std::string& name) {
    std::string ext = pathToUtf8(utf8ToPath(name).extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    return ext == ".sm" || ext == ".ssc" || ext == ".dwi" || ext == ".osu";
}

// Returns an empty string if the songs match, or the reason they don't. The
// rest of the simfile (metadata, timing, charts) is copied from the host.
static std::string CompareSongs(const SongInfo& host, const SongInfo& guest) {
    if (host.audioHash != guest.audioHash) {
        return "The music files are different.";
    }
    if (std::abs(host.duration - guest.duration) > 0.001) {
        return std::format(
            "The music length differs (host {:.3f}s, guest {:.3f}s).",
            host.duration, guest.duration);
    }
    return std::string();
}

// ================================================================================================
// Serialization of the synced data.

static uint32_t Checksum(const NoteList& notes) {
    // 32-bit FNV-1a over the note fields.
    uint32_t hash = 2166136261U;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            hash ^= (v >> (i * 8)) & 0xFF;
            hash *= 16777619U;
        }
    };
    for (const Note& n : notes) {
        mix(static_cast<uint32_t>(n.row));
        mix(static_cast<uint32_t>(n.endrow));
        mix(n.col | (n.player << 8) | (n.type << 12) | (n.quant << 16));
    }
    return hash;
}

static Bytes WriteSongMeta(const Simfile* sim) {
    WriteStream out;
    for (const std::string* s :
         {&sim->title, &sim->titleTr, &sim->subtitle, &sim->subtitleTr,
          &sim->artist, &sim->artistTr, &sim->genre, &sim->credit, &sim->banner,
          &sim->background, &sim->cdTitle, &sim->lyricsPath}) {
        out.writeStr(*s);
    }
    out.write<double>(sim->previewStart);
    out.write<double>(sim->previewLength);
    out.write<uint8_t>(sim->isSelectable ? 1 : 0);
    return ToBytes(out);
}

static bool ApplySongMeta(Simfile* sim, const Bytes& data) {
    ReadStream in(data.data(), static_cast<int>(data.size()));
    std::string s[12];
    for (auto& str : s) in.readStr(str);
    double previewStart = in.read<double>();
    double previewLength = in.read<double>();
    uint8_t selectable = in.read<uint8_t>();
    if (!in.success()) return false;

    int changes = VCM_SONG_PROPERTIES_CHANGED;
    if (sim->banner != s[8]) changes |= VCM_BANNER_PATH_CHANGED;
    if (sim->background != s[9]) changes |= VCM_BACKGROUND_PATH_CHANGED;

    std::string* fields[12] = {
        &sim->title,  &sim->titleTr,    &sim->subtitle, &sim->subtitleTr,
        &sim->artist, &sim->artistTr,   &sim->genre,    &sim->credit,
        &sim->banner, &sim->background, &sim->cdTitle,  &sim->lyricsPath};
    for (int i = 0; i < 12; ++i) *fields[i] = s[i];
    sim->previewStart = previewStart;
    sim->previewLength = previewLength;
    sim->isSelectable = selectable != 0;
    gEditor->reportChanges(changes);
    return true;
}

// Writes a tempo, or an empty marker if it is null.
static Bytes WriteTempo(const Tempo* tempo) {
    WriteStream out;
    out.write<uint8_t>(tempo ? 1 : 0);
    if (!tempo) return ToBytes(out);

    out.write<double>(tempo->offset);
    out.write<int32_t>(tempo->displayBpmType);
    out.write<double>(tempo->displayBpmRange.min);
    out.write<double>(tempo->displayBpmRange.max);
    tempo->segments->encode(out);

    out.writeNum(static_cast<uint32_t>(tempo->attacks.size()));
    for (const Attack& a : tempo->attacks) {
        out.write<double>(a.time);
        out.write<double>(a.duration);
        out.writeStr(a.mods);
        out.write<int32_t>(a.unit);
    }
    out.writeNum(static_cast<uint32_t>(tempo->keysounds.size()));
    for (const std::string& k : tempo->keysounds) out.writeStr(k);
    out.writeNum(static_cast<uint32_t>(tempo->misc.size()));
    for (const Property& p : tempo->misc) {
        out.writeStr(p.tag);
        out.writeStr(p.val);
    }
    return ToBytes(out);
}

// Reads a tempo written by WriteTempo. Returns false on bad data; otherwise
// sets hasTempo and, if it is true, fills out.
static bool ReadTempo(const Bytes& data, bool& hasTempo, Tempo& out) {
    ReadStream in(data.data(), static_cast<int>(data.size()));
    hasTempo = in.read<uint8_t>() != 0;
    if (!in.success()) return false;
    if (!hasTempo) return true;

    in.read(out.offset);
    int32_t bpmType = in.read<int32_t>();
    in.read(out.displayBpmRange.min);
    in.read(out.displayBpmRange.max);
    if (bpmType < BPM_ACTUAL || bpmType > BPM_RANDOM) in.invalidate();
    out.displayBpmType = static_cast<DisplayBpm>(bpmType);
    out.segments->clear();
    out.segments->decode(in);

    uint32_t count = in.readNum();
    for (uint32_t i = 0; i < count && in.success(); ++i) {
        Attack a;
        in.read(a.time);
        in.read(a.duration);
        in.readStr(a.mods);
        a.unit = static_cast<AttackUnit>(in.read<int32_t>());
        out.attacks.push_back(a);
    }
    count = in.readNum();
    for (uint32_t i = 0; i < count && in.success(); ++i) {
        out.keysounds.push_back(in.readStr());
    }
    count = in.readNum();
    for (uint32_t i = 0; i < count && in.success(); ++i) {
        Property p;
        in.readStr(p.tag);
        in.readStr(p.val);
        out.misc.push_back(p);
    }
    return in.success();
}

// The chart tempo is only written if the chart has split timing, so that an
// empty chart tempo and no chart tempo serialize the same.
static Bytes WriteChartTempo(const Chart* chart) {
    return WriteTempo(chart->hasTempo() ? chart->tempo : nullptr);
}

static void SetChartTempo(Chart* chart, bool hasTempo, const Tempo& tempo) {
    if (hasTempo) {
        if (!chart->tempo) chart->tempo = new Tempo;
        chart->tempo->copy(&tempo);
    } else if (chart->tempo) {
        // Clear instead of deleting; history entries can point to it.
        Tempo empty;
        chart->tempo->copy(&empty);
    }
}

struct ChartProps {
    std::string artist;
    int32_t difficulty = DIFF_CHALLENGE;
    int32_t meter = 1;
};

static Bytes WriteChartProps(const Chart* chart) {
    WriteStream out;
    out.writeStr(chart->artist);
    out.write<int32_t>(chart->difficulty);
    out.write<int32_t>(chart->meter);
    return ToBytes(out);
}

static bool ReadChartProps(const Bytes& data, ChartProps& out) {
    ReadStream in(data.data(), static_cast<int>(data.size()));
    in.readStr(out.artist);
    in.read(out.difficulty);
    in.read(out.meter);
    if (out.difficulty < 0 || out.difficulty >= NUM_DIFFICULTIES) {
        in.invalidate();
    }
    return in.success();
}

static void SetChartProps(Chart* chart, const ChartProps& props) {
    chart->artist = props.artist;
    chart->difficulty = static_cast<Difficulty>(props.difficulty);
    chart->meter = props.meter;
}

static uint64_t SectionKey(SectionType type, uint32_t id) {
    return (static_cast<uint64_t>(type) << 32) | id;
}

// ================================================================================================
// Drawing helpers.

static const uint32_t PeerColors[] = {
    RGBAtoColor32(255, 170, 60, 255),  RGBAtoColor32(80, 200, 255, 255),
    RGBAtoColor32(140, 230, 90, 255),  RGBAtoColor32(255, 110, 160, 255),
    RGBAtoColor32(190, 140, 255, 255), RGBAtoColor32(255, 230, 80, 255),
    RGBAtoColor32(90, 230, 200, 255),  RGBAtoColor32(255, 120, 90, 255),
};

static uint32_t PeerColor(uint32_t id) {
    return PeerColors[id % (sizeof(PeerColors) / sizeof(PeerColors[0]))];
}

// A mouse pointer; 'X' is the outline, '.' the fill.
static const char* const CursorShape[] = {
    "X",        "XX",       "X.X",       "X..X",       "X...X",       "X....X",
    "X.....X",  "X......X", "X.......X", "X........X", "X.....XXXXX", "X..X..X",
    "X.X X..X", "XX  X..X", "X    X..X", "     X..X",  "      XX",
};

static void DrawPointer(int x, int y, uint32_t color) {
    int s = std::max(1, gSystem->applyScaleFactor(1));
    uint32_t outline = RGBAtoColor32(0, 0, 0, 150);
    uint32_t fill = Color32a(color, 140);
    int row = 0;
    for (const char* line : CursorShape) {
        int len = static_cast<int>(strlen(line));
        for (int col = 0; col < len;) {
            char c = line[col];
            int end = col;
            while (end < len && line[end] == c) ++end;
            if (c != ' ') {
                Draw::fill({x + col * s, y + row * s, (end - col) * s, s},
                           c == 'X' ? outline : fill);
            }
            col = end;
        }
        ++row;
    }
}

static void DrawNameTag(int x, int y, const std::string& name, uint32_t color,
                        Text::Align align) {
    TextStyle style;
    style.textColor = Colors::white;
    style.textFlags = Text::SINGLE_LINE;
    Text::arrange(align, style, name.c_str());
    vec2i size = Text::getSize();
    int pad = gSystem->applyScaleFactor(3);
    int w = size.x + pad * 2, h = size.y + pad;
    int bx = x;
    if (align == Text::MR) bx = x - w;
    recti box = {bx, y - h / 2, w, h};
    Draw::fill(box, Color32a(color, 110));
    Text::draw(recti{box.x + pad, box.y, size.x, box.h});
}

// Noteskin units are scaled by the zoom level and the UI scale.
static double PixelsPerUnit() {
    return gSystem->applyScaleFactor(gView->applyZoom(256)) / 256.0;
}

// ================================================================================================
// Join and reply codes.
//
// A join code tells a guest how to reach the host: the host's LAN addresses,
// its public address (as seen by a STUN server, and the port forwarded by
// UPnP, if any) and a random session token. If the guest cannot connect
// directly, it shows a reply code with its own public address; once the host
// enters it, both sides send UDP packets to each other until the routers let
// them through ("hole punching").
//
// Code bytes: [u8 kind][u8 version][payload][u8 checksum], written as
// Crockford base32 in dash-separated groups. Values are big-endian.

enum CodeKind : uint8_t {
    CODE_JOIN = 1,
    CODE_REPLY = 2,
};

const uint8_t CODE_VERSION = 1;
const size_t MAX_CODE_ADDRESSES = 6;
const char* const CODE_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

struct JoinCode {
    uint32_t token = 0;
    uint16_t port = 0;          // Local TCP/UDP port of the host.
    NetEndpoint stun;           // Public address of the host's UDP socket.
    NetEndpoint upnp;           // Router address and forwarded port.
    std::vector<uint32_t> lan;  // Local addresses of the host.
};

struct ReplyCode {
    uint32_t token = 0;
    uint16_t port = 0;          // Local UDP port of the guest.
    NetEndpoint stun;           // Public address of the guest's UDP socket.
    std::vector<uint32_t> lan;  // Local addresses of the guest.
};

class CodeWriter {
   public:
    explicit CodeWriter(CodeKind kind) {
        u8(kind);
        u8(CODE_VERSION);
    }
    void u8(uint8_t v) { myBytes.push_back(v); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v >> 8));
        u8(static_cast<uint8_t>(v));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v >> 16));
        u16(static_cast<uint16_t>(v));
    }
    void endpoint(const NetEndpoint& ep) {
        u32(ep.ip);
        u16(ep.port);
    }
    void addresses(const std::vector<uint32_t>& ips) {
        size_t n = std::min(ips.size(), MAX_CODE_ADDRESSES);
        u8(static_cast<uint8_t>(n));
        for (size_t i = 0; i < n; ++i) u32(ips[i]);
    }

    // Appends the checksum and returns the code text.
    std::string finish() {
        u8(static_cast<uint8_t>(HashBytes(myBytes)));
        std::string text;
        uint32_t buffer = 0;
        int bits = 0;
        for (uint8_t b : myBytes) {
            buffer = (buffer << 8) | b;
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                text.push_back(CODE_ALPHABET[(buffer >> bits) & 31]);
            }
        }
        if (bits > 0) {
            text.push_back(CODE_ALPHABET[(buffer << (5 - bits)) & 31]);
        }
        std::string grouped;
        for (size_t i = 0; i < text.size(); ++i) {
            if (i > 0 && i % 6 == 0) grouped.push_back('-');
            grouped.push_back(text[i]);
        }
        return grouped;
    }

   private:
    Bytes myBytes;
};

class CodeReader {
   public:
    // Decodes the text; check kind() and ok() afterwards.
    explicit CodeReader(const std::string& text) {
        uint32_t buffer = 0;
        int bits = 0;
        for (char ch : text) {
            if (ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' ||
                ch == '\n') {
                continue;
            }
            char c =
                static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (c == 'O') c = '0';
            if (c == 'I' || c == 'L') c = '1';
            const char* p = c ? strchr(CODE_ALPHABET, c) : nullptr;
            if (!p) {
                myOk = false;
                return;
            }
            buffer = (buffer << 5) | static_cast<uint32_t>(p - CODE_ALPHABET);
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                myBytes.push_back(static_cast<uint8_t>(buffer >> bits));
            }
        }
        if (myBytes.size() < 3 ||
            static_cast<uint8_t>(HashBytes(
                myBytes.data(), myBytes.size() - 1)) != myBytes.back()) {
            myOk = false;
            return;
        }
        myBytes.pop_back();
        myKind = u8();
        myVersion = u8();
    }

    bool valid() const { return myOk; }
    bool ok() const { return myOk && myPos == myBytes.size(); }
    uint8_t kind() const { return myKind; }
    uint8_t version() const { return myVersion; }

    uint8_t u8() {
        if (myPos >= myBytes.size()) {
            myOk = false;
            return 0;
        }
        return myBytes[myPos++];
    }
    uint16_t u16() {
        uint16_t hi = u8();
        return static_cast<uint16_t>((hi << 8) | u8());
    }
    uint32_t u32() {
        uint32_t hi = u16();
        return (hi << 16) | u16();
    }
    NetEndpoint endpoint() {
        NetEndpoint ep;
        ep.ip = u32();
        ep.port = u16();
        return ep;
    }
    std::vector<uint32_t> addresses() {
        std::vector<uint32_t> out;
        uint8_t n = u8();
        if (n > MAX_CODE_ADDRESSES) myOk = false;
        for (uint8_t i = 0; i < n && myOk; ++i) out.push_back(u32());
        return out;
    }

   private:
    Bytes myBytes;
    size_t myPos = 0;
    uint8_t myKind = 0;
    uint8_t myVersion = 0;
    bool myOk = true;
};

static std::string EncodeJoinCode(const JoinCode& code) {
    CodeWriter out(CODE_JOIN);
    out.u32(code.token);
    out.u16(code.port);
    out.endpoint(code.stun);
    out.endpoint(code.upnp);
    out.addresses(code.lan);
    return out.finish();
}

static std::string EncodeReplyCode(const ReplyCode& code) {
    CodeWriter out(CODE_REPLY);
    out.u32(code.token);
    out.u16(code.port);
    out.endpoint(code.stun);
    out.addresses(code.lan);
    return out.finish();
}

// Returns an error message, or an empty string if the code has the expected
// kind and version.
static std::string CheckCode(const CodeReader& in, CodeKind expected) {
    if (!in.valid()) {
        return "That is not a valid code; make sure it was copied completely.";
    }
    if (in.kind() != expected) {
        return expected == CODE_JOIN
                   ? "That is a reply code; the host enters it under Reply."
                   : "That is a join code; guests enter it under Join.";
    }
    if (in.version() != CODE_VERSION) {
        return "The code is from a different ArrowVortex version.";
    }
    return std::string();
}

static std::string DecodeJoinCode(const std::string& text, JoinCode& code) {
    CodeReader in(text);
    std::string error = CheckCode(in, CODE_JOIN);
    if (!error.empty()) return error;
    code.token = in.u32();
    code.port = in.u16();
    code.stun = in.endpoint();
    code.upnp = in.endpoint();
    code.lan = in.addresses();
    if (!in.ok() || code.port == 0) return "That is not a valid join code.";
    return std::string();
}

static std::string DecodeReplyCode(const std::string& text, ReplyCode& code) {
    CodeReader in(text);
    std::string error = CheckCode(in, CODE_REPLY);
    if (!error.empty()) return error;
    code.token = in.u32();
    code.port = in.u16();
    code.stun = in.endpoint();
    code.lan = in.addresses();
    if (!in.ok()) return "That is not a valid reply code.";
    return std::string();
}

static void AddUnique(std::vector<NetEndpoint>& list, const NetEndpoint& ep) {
    if (ep.isValid() && std::find(list.begin(), list.end(), ep) == list.end()) {
        list.push_back(ep);
    }
}

static std::vector<uint32_t> LocalIps() {
    std::vector<uint32_t> ips;
    for (const auto& text : NetSocket::localAddresses()) {
        uint32_t ip = 0;
        if (NetEndpoint::parseIp(text, ip)) ips.push_back(ip);
    }
    return ips;
}

static uint32_t RandomU32() {
    std::random_device rd;
    uint32_t v = 0;
    while (v == 0) v = rd();
    return v;
}

// How a guest can reach the host, from the join code.
struct HostAddress {
    uint32_t token = 0;
    std::vector<NetEndpoint> tcp;  // Tried directly, all at once.
    std::vector<NetEndpoint> udp;  // Used for hole punching.
};

static HostAddress ToHostAddress(const JoinCode& code) {
    HostAddress out;
    out.token = code.token;
    for (uint32_t ip : code.lan) {
        AddUnique(out.tcp, {ip, code.port});
        AddUnique(out.udp, {ip, code.port});
    }
    // The UPnP port, and the host's port on its public address (in case it
    // was forwarded by hand).
    AddUnique(out.tcp, code.upnp);
    if (code.stun.ip) {
        AddUnique(out.tcp, {code.stun.ip, code.port});
        AddUnique(out.tcp, {code.stun.ip, code.upnp.port});
    }
    AddUnique(out.udp, code.stun);
    AddUnique(out.udp, code.upnp);
    if (code.stun.ip) AddUnique(out.udp, {code.stun.ip, code.upnp.port});
    return out;
}

// ================================================================================================
// NetLink: owns the worker thread and the message queues.
//
// Connections are either TCP (direct) or a UdpStream over the host's UDP
// socket (hole punched). The host listens for both on the same port number;
// a guest first tries every TCP address from the join code at once, while
// also punching the host's UDP addresses. Whichever works first is used.

struct NetEvent {
    enum Type {
        CONNECTED,  // A peer connected (host) / connect succeeded (guest).
        LOST,       // A peer connection ended.
        FAILED,     // Connecting failed (guest).
        FRAME,      // A complete frame was received; data = [type][payload].
        STUN_DONE,  // The public UDP address is known (or not found).
        DIRECT_FAILED,  // Guest: no direct connection; hole punching only.
    };
    Type type = FRAME;
    uint32_t peer = 0;
    Bytes data;
    std::string text;
    NetEndpoint endpoint;  // STUN_DONE: public address (invalid if unknown).
    bool flag = false;     // STUN_DONE: symmetric NAT. CONNECTED: via UDP.
};

class NetLink {
   public:
    ~NetLink() { stop(); }

    // Host: accepts any number of peers, with ids starting at 1. The UDP
    // socket (optional) is used for STUN and hole punched connections from
    // guests that know the token.
    bool startHost(NetSocket listener, NetSocket udp, uint32_t token,
                   std::string& error) {
        stop();
        try {
            myThread =
                std::jthread([this, l = std::move(listener), u = std::move(udp),
                              token](std::stop_token st) mutable {
                    myToken = token;
                    StunClient stun;
                    if (u.isValid()) stun.start();
                    myRun(st, &l, u.isValid() ? &u : nullptr, stun, {});
                });
        } catch (const std::system_error& e) {
            error = e.what();
            return false;
        }
        return true;
    }

    // Guest: connects to the host, which gets peer id HOST_ID.
    bool startGuest(HostAddress host, NetSocket udp, std::string& error) {
        stop();
        try {
            myThread =
                std::jthread([this, h = std::move(host),
                              u = std::move(udp)](std::stop_token st) mutable {
                    myToken = h.token;
                    myGuestLoop(st, h, u);
                });
        } catch (const std::system_error& e) {
            error = e.what();
            return false;
        }
        return true;
    }

    // Stops the worker thread, after trying to flush pending frames.
    void stop() {
        if (myThread.joinable()) {
            myThread.request_stop();
            myThread.join();
        }
        std::lock_guard<std::mutex> lock(myMutex);
        myInbox.clear();
        myOutbox.clear();
        myDrops.clear();
        myNewTargets.clear();
    }

    void send(uint32_t peer, Bytes frame) {
        std::lock_guard<std::mutex> lock(myMutex);
        myOutbox.push_back({peer, std::move(frame)});
    }

    // Closes a peer connection once its pending frames are sent.
    void dropPeer(uint32_t peer) {
        std::lock_guard<std::mutex> lock(myMutex);
        myDrops.push_back(peer);
    }

    // Host: starts punching towards a guest (from its reply code).
    void addPunchTargets(const std::vector<NetEndpoint>& targets) {
        std::lock_guard<std::mutex> lock(myMutex);
        myNewTargets.insert(myNewTargets.end(), targets.begin(), targets.end());
    }

    bool poll(NetEvent& out) {
        std::lock_guard<std::mutex> lock(myMutex);
        if (myInbox.empty()) return false;
        out = std::move(myInbox.front());
        myInbox.pop_front();
        return true;
    }

   private:
    struct Outgoing {
        uint32_t peer;
        Bytes frame;
    };

    struct Conn {
        uint32_t id = 0;
        NetSocket socket;                   // TCP connections.
        std::unique_ptr<UdpStream> stream;  // Hole punched connections.
        Bytes readBuf, writeBuf;
        size_t writePos = 0;
        bool dropping = false;
        Clock::time_point dropTime;

        bool hasPendingWrites() const {
            if (stream) return stream->hasPendingWrites();
            return writePos < writeBuf.size();
        }
    };

    // Host: an address a guest's reply code asked us to punch towards.
    struct PunchTarget {
        NetEndpoint endpoint;
        Clock::time_point expires;
    };

    void myPush(NetEvent evt) {
        std::lock_guard<std::mutex> lock(myMutex);
        myInbox.push_back(std::move(evt));
    }

    void myPush(NetEvent::Type type, uint32_t peer, std::string text) {
        NetEvent evt;
        evt.type = type;
        evt.peer = peer;
        evt.text = std::move(text);
        myPush(std::move(evt));
    }

    void myPushStun(const StunClient& stun) {
        NetEvent evt;
        evt.type = NetEvent::STUN_DONE;
        evt.endpoint = stun.getResult();
        evt.flag = stun.isSymmetric();
        myPush(std::move(evt));
    }

    static int ElapsedMs(Clock::time_point since) {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                  since)
                .count());
    }

    static uint32_t PunchToken(const uint8_t* data, int size) {
        if (size < UdpStream::HEADER_SIZE + 4) return 0;
        const uint8_t* p = data + UdpStream::HEADER_SIZE;
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    // Tries all direct addresses and hole punching at the same time, then
    // runs the connection that worked.
    void myGuestLoop(const std::stop_token& st, const HostAddress& host,
                     NetSocket& udp) {
        struct Attempt {
            NetEndpoint endpoint;
            NetSocket socket;
        };
        std::vector<Attempt> attempts;
        std::string error;
        for (const auto& ep : host.tcp) {
            Attempt a;
            a.endpoint = ep;
            if (a.socket.beginConnect(ep, error)) {
                attempts.push_back(std::move(a));
            }
        }

        StunClient stun;
        if (udp.isValid()) stun.start();
        bool stunReported = false;
        bool directFailed = false;
        uint32_t connection = RandomU32();
        std::vector<NetEndpoint> punchTo = host.udp;
        auto start = Clock::now();
        Clock::time_point lastPunch;
        uint8_t buf[2048];

        while (!st.stop_requested()) {
            std::vector<NetSocket::PollItem> items;
            for (auto& a : attempts) items.push_back({&a.socket, true, 0});
            if (udp.isValid()) items.push_back({&udp, false, 0});
            if (items.empty()) {
                myPush(NetEvent::FAILED, HOST_ID,
                       "Could not reach the host. " + error);
                return;
            }
            if (!NetSocket::pollAll(items, POLL_INTERVAL_MS)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(POLL_INTERVAL_MS));
            }

            // Direct connections.
            for (size_t i = 0; i < attempts.size(); ++i) {
                if (!(items[i].result &
                      (NetSocket::POLL_WRITE | NetSocket::POLL_ERROR))) {
                    continue;
                }
                if (attempts[i].socket.finishConnect(error)) {
                    NetEvent evt;
                    evt.type = NetEvent::CONNECTED;
                    evt.peer = HOST_ID;
                    evt.text = attempts[i].endpoint.toString();
                    myPush(std::move(evt));
                    std::vector<Conn> conns(1);
                    conns[0].id = HOST_ID;
                    conns[0].socket = std::move(attempts[i].socket);
                    attempts.clear();
                    udp.close();
                    myRun(st, nullptr, nullptr, stun, std::move(conns));
                    return;
                }
                attempts[i].socket.close();
            }
            std::erase_if(attempts,
                          [](const Attempt& a) { return !a.socket.isValid(); });
            if (ElapsedMs(start) > CONNECT_TIMEOUT_MS) attempts.clear();
            if (attempts.empty() && !directFailed) {
                directFailed = true;
                if (!udp.isValid()) continue;  // Reports FAILED above.
                myPush(NetEvent::DIRECT_FAILED, HOST_ID, std::string());
            }
            if (!udp.isValid()) continue;

            // Hole punching.
            NetEndpoint from;
            int n;
            while ((n = udp.recvFrom(buf, sizeof(buf), from)) > 0) {
                if (stun.onPacket(buf, n, from)) continue;
                UdpStream::Kind kind;
                uint32_t id;
                if (!UdpStream::parseHeader(buf, n, kind, id)) continue;
                if (kind == UdpStream::KIND_PUNCH &&
                    PunchToken(buf, n) == host.token) {
                    // The host punches towards us; answer from here so that
                    // its router lets our punches through.
                    AddUnique(punchTo, from);
                    UdpStream::sendPunch(udp, from, connection, host.token);
                } else if (kind == UdpStream::KIND_ACCEPT && id == connection) {
                    NetEvent evt;
                    evt.type = NetEvent::CONNECTED;
                    evt.peer = HOST_ID;
                    evt.text = from.toString();
                    evt.flag = true;
                    myPush(std::move(evt));
                    std::vector<Conn> conns(1);
                    conns[0].id = HOST_ID;
                    conns[0].stream =
                        std::make_unique<UdpStream>(connection, from);
                    attempts.clear();
                    myRun(st, nullptr, &udp, stun, std::move(conns));
                    return;
                }
            }
            stun.update(udp);
            if (!stunReported && stun.isDone()) {
                stunReported = true;
                myPushStun(stun);
            }
            // Give direct connections a head start; they are faster.
            bool punch = attempts.empty() || ElapsedMs(start) > PUNCH_DELAY_MS;
            if (punch && ElapsedMs(lastPunch) >= PUNCH_INTERVAL_MS) {
                lastPunch = Clock::now();
                for (const auto& ep : punchTo) {
                    UdpStream::sendPunch(udp, ep, connection, host.token);
                }
            }
            if (ElapsedMs(start) > PUNCH_TIMEOUT_MS) {
                myPush(NetEvent::FAILED, HOST_ID,
                       "Timed out waiting for the host to accept the reply "
                       "code.");
                return;
            }
        }
    }

    static Conn* myFind(std::vector<Conn>& conns, uint32_t id) {
        for (auto& c : conns) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    static Conn* myFindStream(std::vector<Conn>& conns, const NetEndpoint& from,
                              uint32_t connection) {
        for (auto& c : conns) {
            if (c.stream && c.stream->getPeer() == from &&
                c.stream->getConnection() == connection) {
                return &c;
            }
        }
        return nullptr;
    }

    static void myClose(Conn& c, NetSocket* udp) {
        if (c.stream && udp) {
            for (int i = 0; i < 2; ++i) {
                UdpStream::sendControl(*udp, c.stream->getPeer(),
                                       UdpStream::KIND_CLOSE,
                                       c.stream->getConnection());
            }
        }
    }

    // Moves frames between the connections and the queues. With a listener,
    // new connections are accepted until stopped; otherwise the loop ends
    // when the single connection ends.
    void myRun(const std::stop_token& st, NetSocket* listener, NetSocket* udp,
               StunClient& stun, std::vector<Conn> conns) {
        uint32_t nextId = HOST_ID + 1;
        Bytes chunk(1 << 16);
        bool stopping = false;
        Clock::time_point stopTime;
        bool stunReported = !listener || !udp;
        std::vector<PunchTarget> targets;
        Clock::time_point lastPunch;

        while (true) {
            // Collect outgoing frames, drop requests and punch targets.
            {
                std::lock_guard<std::mutex> lock(myMutex);
                for (auto& out : myOutbox) {
                    Conn* c = myFind(conns, out.peer);
                    if (!c || c->dropping) continue;
                    if (c->stream) {
                        c->stream->write(out.frame.data(), out.frame.size());
                    } else {
                        c->writeBuf.insert(c->writeBuf.end(), out.frame.begin(),
                                           out.frame.end());
                    }
                }
                myOutbox.clear();
                for (uint32_t id : myDrops) {
                    Conn* c = myFind(conns, id);
                    if (c && !c->dropping) {
                        c->dropping = true;
                        c->dropTime = Clock::now();
                    }
                }
                myDrops.clear();
                for (const auto& ep : myNewTargets) {
                    auto expires = Clock::now() +
                                   std::chrono::milliseconds(PUNCH_TIMEOUT_MS);
                    auto it = std::find_if(
                        targets.begin(), targets.end(),
                        [&](const PunchTarget& t) { return t.endpoint == ep; });
                    if (it != targets.end()) {
                        it->expires = expires;
                    } else {
                        targets.push_back({ep, expires});
                    }
                }
                myNewTargets.clear();
            }

            // On stop, close once everything is sent (or after a short grace
            // period).
            if (st.stop_requested()) {
                if (!stopping) {
                    stopping = true;
                    stopTime = Clock::now();
                }
                bool pending = std::any_of(
                    conns.begin(), conns.end(),
                    [](const Conn& c) { return c.hasPendingWrites(); });
                if (!pending || ElapsedMs(stopTime) > STOP_GRACE_MS) {
                    for (auto& c : conns) myClose(c, udp);
                    return;
                }
            }

            // Close dropped connections once their frames are sent.
            for (auto it = conns.begin(); it != conns.end();) {
                if (it->dropping && (!it->hasPendingWrites() ||
                                     ElapsedMs(it->dropTime) > STOP_GRACE_MS)) {
                    myClose(*it, udp);
                    myPush(NetEvent::LOST, it->id, "Connection closed.");
                    it = conns.erase(it);
                } else {
                    ++it;
                }
            }
            if (!listener && conns.empty()) return;

            std::vector<NetSocket::PollItem> items;
            if (listener) items.push_back({listener, false, 0});
            if (udp) items.push_back({udp, false, 0});
            size_t first = items.size();
            std::vector<size_t> tcpConns;
            for (size_t i = 0; i < conns.size(); ++i) {
                if (conns[i].stream) continue;
                tcpConns.push_back(i);
                items.push_back(
                    {&conns[i].socket, conns[i].hasPendingWrites(), 0});
            }
            if (!NetSocket::pollAll(items, POLL_INTERVAL_MS)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(POLL_INTERVAL_MS));
                continue;
            }

            std::vector<std::pair<uint32_t, std::string>> lost;
            for (size_t i = 0; i < tcpConns.size(); ++i) {
                Conn& c = conns[tcpConns[i]];
                std::string reason =
                    myService(c, items[first + i].result, chunk);
                if (!reason.empty()) lost.emplace_back(c.id, reason);
            }
            if (udp) {
                myServiceUdp(*udp, listener != nullptr, conns, nextId, stun,
                             lost);
            }
            for (auto& [id, reason] : lost) {
                auto it = std::find_if(
                    conns.begin(), conns.end(),
                    [id = id](const Conn& c) { return c.id == id; });
                if (it == conns.end()) continue;
                myClose(*it, udp);
                conns.erase(it);
                myPush(NetEvent::LOST, id, reason);
            }

            if (udp) {
                // Host: punch towards guests that sent a reply code, until
                // they are connected.
                if (ElapsedMs(lastPunch) >= PUNCH_INTERVAL_MS) {
                    lastPunch = Clock::now();
                    std::erase_if(targets, [](const PunchTarget& t) {
                        return Clock::now() > t.expires;
                    });
                    for (const auto& t : targets) {
                        bool connected = std::any_of(
                            conns.begin(), conns.end(), [&](const Conn& c) {
                                return c.stream &&
                                       c.stream->getPeer() == t.endpoint;
                            });
                        if (!connected) {
                            UdpStream::sendPunch(*udp, t.endpoint, 0, myToken);
                        }
                    }
                }
                stun.update(*udp);
                if (!stunReported && stun.isDone()) {
                    stunReported = true;
                    myPushStun(stun);
                }
            }

            if (listener && (items[0].result & NetSocket::POLL_READ)) {
                NetSocket sock = listener->accept();
                if (sock.isValid()) {
                    Conn c;
                    c.id = nextId++;
                    c.socket = std::move(sock);
                    conns.push_back(std::move(c));
                    myPush(NetEvent::CONNECTED, conns.back().id, std::string());
                }
            }
        }
    }

    // Reads all pending datagrams, and updates the hole punched connections.
    void myServiceUdp(NetSocket& udp, bool isHost, std::vector<Conn>& conns,
                      uint32_t& nextId, StunClient& stun,
                      std::vector<std::pair<uint32_t, std::string>>& lost) {
        uint8_t buf[2048];
        NetEndpoint from;
        int n;
        for (int count = 0;
             count < 4096 && (n = udp.recvFrom(buf, sizeof(buf), from)) > 0;
             ++count) {
            if (stun.onPacket(buf, n, from)) continue;
            UdpStream::Kind kind;
            uint32_t id;
            if (!UdpStream::parseHeader(buf, n, kind, id)) continue;
            Conn* c = myFindStream(conns, from, id);

            if (kind == UdpStream::KIND_PUNCH) {
                // Host: a guest that knows the token; connect (once) and
                // tell it so. Punches from a host (id 0) are ignored.
                if (!isHost || id == 0 || PunchToken(buf, n) != myToken) {
                    continue;
                }
                if (!c) {
                    Conn conn;
                    conn.id = nextId++;
                    conn.stream = std::make_unique<UdpStream>(id, from);
                    conns.push_back(std::move(conn));
                    c = &conns.back();
                    NetEvent evt;
                    evt.type = NetEvent::CONNECTED;
                    evt.peer = c->id;
                    evt.text = from.toString();
                    evt.flag = true;
                    myPush(std::move(evt));
                }
                c->stream->onAlive();
                UdpStream::sendControl(udp, from, UdpStream::KIND_ACCEPT, id);
                continue;
            }
            if (!c) continue;
            if (kind == UdpStream::KIND_CLOSE) {
                lost.emplace_back(c->id,
                                  "The other side closed the connection.");
                continue;
            }
            if (kind == UdpStream::KIND_DATA || kind == UdpStream::KIND_ACK) {
                c->stream->onPacket(kind, buf + UdpStream::HEADER_SIZE,
                                    n - UdpStream::HEADER_SIZE, c->readBuf);
                if (!myParseFrames(c->id, c->readBuf)) {
                    lost.emplace_back(c->id, "Protocol error.");
                }
            } else {
                c->stream->onAlive();
            }
        }
        for (auto& c : conns) {
            if (!c.stream) continue;
            c.stream->update(udp);
            if (c.stream->isDead()) {
                lost.emplace_back(c.id, "Connection timed out.");
            }
        }
    }

    // Reads and writes one connection. Returns the reason it ended, or an
    // empty string if it is still alive.
    std::string myService(Conn& c, int flags, Bytes& chunk) {
        if (flags & NetSocket::POLL_ERROR) return "Connection error.";

        if (flags & NetSocket::POLL_READ) {
            int n = c.socket.recv(chunk.data(), static_cast<int>(chunk.size()));
            if (n < 0) return "The other side closed the connection.";
            c.readBuf.insert(c.readBuf.end(), chunk.begin(), chunk.begin() + n);
            if (!myParseFrames(c.id, c.readBuf)) return "Protocol error.";
        }

        if (c.hasPendingWrites() && (flags & NetSocket::POLL_WRITE)) {
            int n =
                c.socket.send(c.writeBuf.data() + c.writePos,
                              static_cast<int>(c.writeBuf.size() - c.writePos));
            if (n < 0) return "Connection error.";
            c.writePos += static_cast<size_t>(n);
            if (c.writePos == c.writeBuf.size()) {
                c.writeBuf.clear();
                c.writePos = 0;
            }
        }
        return std::string();
    }

    // Extracts complete frames from the buffer. Returns false on bad data.
    bool myParseFrames(uint32_t peer, Bytes& buf) {
        size_t pos = 0;
        while (buf.size() - pos >= 4) {
            uint32_t size = static_cast<uint32_t>(buf[pos]) |
                            (static_cast<uint32_t>(buf[pos + 1]) << 8) |
                            (static_cast<uint32_t>(buf[pos + 2]) << 16) |
                            (static_cast<uint32_t>(buf[pos + 3]) << 24);
            if (size == 0 || size > MAX_FRAME_SIZE) return false;
            if (buf.size() - pos - 4 < size) break;

            NetEvent evt;
            evt.type = NetEvent::FRAME;
            evt.peer = peer;
            auto begin = buf.begin() + static_cast<ptrdiff_t>(pos + 4);
            evt.data.assign(begin, begin + size);
            {
                std::lock_guard<std::mutex> lock(myMutex);
                myInbox.push_back(std::move(evt));
            }
            pos += 4 + size;
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(pos));
        return true;
    }

    std::jthread myThread;
    std::mutex myMutex;
    std::deque<NetEvent> myInbox;
    std::deque<Outgoing> myOutbox;
    std::vector<uint32_t> myDrops;
    std::vector<NetEndpoint> myNewTargets;
    uint32_t myToken = 0;  // Only used by the worker thread.
};

// ================================================================================================
// SessionImpl.

struct Presence {
    uint32_t chart = 0;
    bool hasHover = false;
    double hoverTime = 0.0;
    double hoverX = 0.0;
    double viewTime = 0.0;

    bool operator==(const Presence& o) const {
        return chart == o.chart && hasHover == o.hasHover &&
               hoverTime == o.hoverTime && hoverX == o.hoverX &&
               viewTime == o.viewTime;
    }
};

struct PeerInfo {
    uint32_t id = 0;
    std::string name;
    bool joined = false;       // Host: passed the song check.
    bool downloading = false;  // Host: downloading the song files.
    Presence presence;

    // Selected notes and region, on the chart with session id selChart.
    uint32_t selChart = 0;
    int regionBegin = 0, regionEnd = 0;
    std::vector<RowCol> selection;
};

// Guest: state of a song download from the host.
struct Download {
    std::string folder;
    std::string musicName;
    uint64_t musicHash = 0;
    uint64_t musicSize = 0;
    bool needMusic = true;
    fs::path dir;

    // The file that is being received.
    std::ofstream file;
    std::string fileName;
    uint64_t fileSize = 0;
    uint64_t fileWritten = 0;
    uint64_t totalReceived = 0;
};

// Sync state of a chart that exists for everyone in the session.
struct ChartSync {
    uint32_t knownChecksum = 0;  // Checksum after the last sent/applied edit.
    int pendingEdits = 0;        // Guest: own edits not yet echoed by host.
    bool resyncRequested = false;
};

// A chart received in a full state, before it is applied.
struct ChartData {
    uint32_t id = 0;
    const Style* style = nullptr;
    ChartProps props;
    bool hasTempo = false;
    std::unique_ptr<Tempo> tempo;
    NoteList notes;
};

struct SessionImpl : public Session {
    NetLink myLink;
    bool myNetReady = false;

    Role myRole = ROLE_NONE;
    State myState = STATE_IDLE;

    std::string myName;
    uint32_t myId = HOST_ID;

    std::string myAddress;

    // Session token from the join code; guests must know it to connect.
    uint32_t myToken = 0;
    // The public address of our UDP socket, as seen by a STUN server.
    bool myStunDone = false;
    bool mySymmetricNat = false;
    NetEndpoint myPublicAddress;
    std::vector<uint32_t> myLocalIps;

    // Host: how guests can reach us.
    PortMapper myPortMapper;
    int myPort = 0;
    bool myHasUdp = false;
    Clock::time_point myHostStart;
    std::string myJoinCode;

    // Guest: the join code, our UDP port (kept when joining again, so that
    // the host's punches still reach us) and the reply code for the host.
    std::string myJoinCodeText;
    uint16_t myGuestPort = 0;
    bool myDirectFailed = false;
    std::string myReplyCode;

    const Simfile* mySimfile = nullptr;
    SongInfo mySong;

    // Guest: true once the host's full state has been applied.
    bool myHasState = false;

    std::vector<PeerInfo> myPeerInfo;
    std::vector<Peer> myPeers;

    // Charts that exist for everyone in the session, by session id.
    std::unordered_map<const Chart*, uint32_t> myChartIds;
    std::unordered_map<uint32_t, ChartSync> myChartSync;
    uint32_t myNextChartId = 1;

    // Hashes of the last sent/applied version of each section.
    std::unordered_map<uint64_t, uint64_t> mySectionHashes;
    // Guest: sections sent to the host that were not echoed back yet.
    std::unordered_map<uint64_t, int> myPendingSections;
    bool mySectionsDirty = false;

    Presence myPresence;
    Clock::time_point myPresenceTime;
    bool myForcePresence = false;

    // Hash of the last sent selection message.
    uint64_t mySelectionHash = 0;
    Clock::time_point mySelectionTime;
    bool myForceSelection = false;

    // Host: song folder name and music file name offered for download.
    std::string mySongFolder;
    std::string myMusicName;

    // Guest: song download, and the join that follows it.
    std::unique_ptr<Download> myDownload;
    bool myRejoinPending = false;
    std::string myRejoinCode;
    Clock::time_point myRejoinDeadline;

    // ================================================================================================
    // SessionImpl :: constructor and destructor.

    ~SessionImpl() {
        disconnect();
        if (myNetReady) NetSocket::shutdown();
    }

    explicit SessionImpl(XmrNode& settings) {
        XmrNode* session = settings.child("session");
        if (session) setUserName(session->get("name", ""));
    }

    void saveSettings(XmrNode& settings) override {
        XmrNode* session = settings.addChild("session");
        session->addAttrib("name", myName.c_str());
    }

    static std::string SanitizeName(const std::string& name) {
        std::string clean;
        for (char c : name) {
            if (static_cast<unsigned char>(c) >= 32) clean.push_back(c);
        }
        while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        if (clean.size() > MAX_NAME_LENGTH) {
            // Cut before the start of the character that does not fit.
            size_t end = MAX_NAME_LENGTH;
            while (end > 0 &&
                   (static_cast<uint8_t>(clean[end]) & 0xC0) == 0x80) {
                --end;
            }
            clean.resize(end);
        }
        return clean;
    }

    void setUserName(const std::string& name) override {
        myName = SanitizeName(name);
    }

    const std::string& getUserName() const override { return myName; }

    // Starts the socket library on first use.
    bool myStartNetworking() {
        if (!myNetReady) myNetReady = NetSocket::startup();
        if (!myNetReady) HudError("Networking is not available.");
        return myNetReady;
    }

    // ================================================================================================
    // SessionImpl :: sending.

    static Bytes MakeFrame(MessageType type, const WriteStream& payload) {
        auto size = static_cast<uint32_t>(payload.size() + 1);
        Bytes frame(4 + size);
        frame[0] = static_cast<uint8_t>(size);
        frame[1] = static_cast<uint8_t>(size >> 8);
        frame[2] = static_cast<uint8_t>(size >> 16);
        frame[3] = static_cast<uint8_t>(size >> 24);
        frame[4] = type;
        if (payload.size() > 0) {
            memcpy(frame.data() + 5, payload.data(), payload.size());
        }
        return frame;
    }

    void mySendTo(uint32_t peer, MessageType type, const WriteStream& payload) {
        myLink.send(peer, MakeFrame(type, payload));
    }

    // Host: sends to every guest that joined, except one.
    void myBroadcast(MessageType type, const WriteStream& payload,
                     uint32_t except = UINT32_MAX) {
        Bytes frame = MakeFrame(type, payload);
        for (auto& peer : myPeerInfo) {
            if (peer.joined && peer.id != except) myLink.send(peer.id, frame);
        }
    }

    // Sends a change made by the local user: guests send it to the host, the
    // host sends it to all guests.
    void mySendChange(MessageType type, const WriteStream& payload) {
        if (myRole == ROLE_HOST) {
            myBroadcast(type, payload);
        } else {
            mySendTo(HOST_ID, type, payload);
        }
    }

    void mySendDisconnect(uint32_t peer, const std::string& reason) {
        WriteStream out;
        out.writeStr(reason);
        mySendTo(peer, MSG_DISCONNECT, out);
    }

    // ================================================================================================
    // SessionImpl :: public API.

    bool host(int port) override {
        disconnect();
        if (!myStartNetworking()) return false;

        std::string error;
        if (!GetLocalSongInfo(mySong, error)) {
            HudError("Cannot host: %s", error.c_str());
            return false;
        }
        {
            const Simfile* sim = gSimfile->get();
            fs::path dir = utf8ToPath(sim->dir);
            if (!dir.has_filename()) dir = dir.parent_path();
            mySongFolder = pathToUtf8(dir.filename());
            myMusicName = pathToUtf8(utf8ToPath(sim->music).filename());
        }
        NetSocket listener;
        if (!listener.listen(port, error)) {
            HudError("Cannot host: %s", error.c_str());
            return false;
        }
        // UDP on the same port, for STUN and hole punching. Hosting works
        // without it, but only for guests that can connect directly.
        NetSocket udp;
        std::string udpError;
        myHasUdp = udp.openUdp(port, udpError);
        if (!myHasUdp) HudWarning("No hole punching: %s", udpError.c_str());

        myToken = RandomU32();
        if (!myLink.startHost(std::move(listener), std::move(udp), myToken,
                              error)) {
            HudError("Cannot host: %s", error.c_str());
            return false;
        }
        myPort = port;
        myLocalIps = LocalIps();
        myHostStart = Clock::now();
        myJoinCode.clear();
        myStunDone = false;
        mySymmetricNat = false;
        myPublicAddress = NetEndpoint();
        myPortMapper.start(port);

        myRole = ROLE_HOST;
        myState = STATE_LISTENING;
        myId = HOST_ID;
        mySimfile = gSimfile->get();
        myHasState = true;
        myResetSyncState();
        myAssignChartIds();
        myStoreSectionHashes();
        HudInfo("Hosting session; preparing the join code...");
        return true;
    }

    bool join(const std::string& text) override {
        disconnect();
        myRejoinPending = false;
        if (!myStartNetworking()) return false;

        JoinCode code;
        std::string error = DecodeJoinCode(text, code);
        if (!error.empty()) {
            HudError("Cannot join: %s", error.c_str());
            return false;
        }

        if (gHistory->hasUnsavedChanges()) {
            HudError(
                "Save your changes before joining; the host's simfile will "
                "replace yours.");
            return false;
        }
        // Without the song, the guest downloads it from the host.
        mySong = SongInfo();
        const Simfile* sim = gSimfile->get();
        if (sim && !sim->music.empty() && !GetLocalSongInfo(mySong, error)) {
            HudError("Cannot join: %s", error.c_str());
            return false;
        }
        // Reuse the previous UDP port if possible: when joining again after a
        // download, the host still punches towards it.
        NetSocket udp;
        if (!udp.openUdp(myGuestPort, error) &&
            (myGuestPort == 0 || !udp.openUdp(0, error))) {
            HudWarning("No hole punching: %s", error.c_str());
        }
        if (udp.isValid()) myGuestPort = udp.localEndpoint().port;
        if (!myLink.startGuest(ToHostAddress(code), std::move(udp), error)) {
            HudError("Cannot join: %s", error.c_str());
            return false;
        }

        myJoinCodeText = text;
        myToken = code.token;
        myLocalIps = LocalIps();
        myDirectFailed = false;
        myReplyCode.clear();
        myStunDone = false;
        mySymmetricNat = false;
        myPublicAddress = NetEndpoint();
        myAddress = "the host";
        myRole = ROLE_GUEST;
        myState = STATE_CONNECTING;
        mySimfile = gSimfile->get();
        myHasState = false;
        myResetSyncState();
        HudInfo("Connecting to the host...");
        return true;
    }

    void disconnect() override {
        myRejoinPending = false;
        if (myRole == ROLE_NONE) return;
        if (myRole == ROLE_HOST) {
            for (auto& peer : myPeerInfo) {
                mySendDisconnect(peer.id, "The host ended the session.");
            }
        } else if (myState == STATE_HANDSHAKE || myState == STATE_DOWNLOADING ||
                   myState == STATE_CONNECTED) {
            mySendDisconnect(HOST_ID, "Left the session.");
        }
        myEndSession();
        HudInfo("Session ended.");
    }

    bool acceptReply(const std::string& text) override {
        if (myRole != ROLE_HOST) {
            HudError("Only the host can accept reply codes.");
            return false;
        }
        if (!myHasUdp) {
            HudError("Hole punching is not available (no UDP port).");
            return false;
        }
        ReplyCode code;
        std::string error = DecodeReplyCode(text, code);
        if (!error.empty()) {
            HudError("%s", error.c_str());
            return false;
        }
        if (code.token != myToken) {
            HudError("That reply code is for a different session.");
            return false;
        }
        std::vector<NetEndpoint> targets;
        AddUnique(targets, code.stun);
        for (uint32_t ip : code.lan) AddUnique(targets, {ip, code.port});
        myLink.addPunchTargets(targets);
        HudInfo("Connecting to the guest...");
        return true;
    }

    void kick(uint32_t id) override {
        if (myRole != ROLE_HOST) return;
        PeerInfo* peer = myFindPeer(id);
        if (!peer || !peer->joined) return;
        mySendDisconnect(id, "You were removed from the session by the host.");
        myLink.dropPeer(id);
        myRemovePeer(id, "was removed from the session.");
    }

    void myResetSyncState() {
        myPeerInfo.clear();
        myPeers.clear();
        myChartIds.clear();
        myChartSync.clear();
        myNextChartId = 1;
        mySectionHashes.clear();
        myPendingSections.clear();
        mySectionsDirty = false;
        myPresence = Presence();
        myForcePresence = true;
        mySelectionHash = 0;
        myForceSelection = true;
    }

    void myEndSession() {
        myLink.stop();
        myPortMapper.stop();
        myJoinCode.clear();
        myReplyCode.clear();
        myDownload.reset();
        myRole = ROLE_NONE;
        myState = STATE_IDLE;
        mySimfile = nullptr;
        myHasState = false;
        myResetSyncState();
    }

    Role getRole() const override { return myRole; }

    State getState() const override { return myState; }

    std::string myUserList() const {
        std::string names;
        for (auto& peer : myPeers) {
            if (!names.empty()) names += ", ";
            names += peer.name;
        }
        return names;
    }

    std::string getStatus() const override {
        switch (myState) {
            case STATE_IDLE:
                return "Not connected.";
            case STATE_LISTENING: {
                if (myJoinCode.empty()) {
                    return "Hosting. Preparing the join code (UPnP, STUN)...";
                }
                if (myPeers.empty()) {
                    return "Hosting, waiting for guests. " + myReachability();
                }
                return std::format("Hosting, {} guest{}: {}", myPeers.size(),
                                   myPeers.size() == 1 ? "" : "s",
                                   myUserList());
            }
            case STATE_CONNECTING:
                if (!myDirectFailed) return "Connecting to the host...";
                if (!myReplyCode.empty()) {
                    return "No direct connection. Send your reply code to the "
                           "host.";
                }
                if (!myStunDone) {
                    return "No direct connection. Finding your public "
                           "address...";
                }
                return "Could not reach the host, and your public address "
                       "is unknown.";
            case STATE_HANDSHAKE:
                return "Verifying song...";
            case STATE_DOWNLOADING: {
                if (!myDownload || myDownload->fileName.empty()) {
                    return "Downloading the song from the host...";
                }
                const Download& d = *myDownload;
                return std::format(
                    "Downloading {} ({:.1f} / {:.1f} MB)...", d.fileName,
                    static_cast<double>(d.fileWritten) / (1024.0 * 1024.0),
                    static_cast<double>(d.fileSize) / (1024.0 * 1024.0));
            }
            case STATE_CONNECTED:
                if (!myHasState) return "Receiving the host's simfile...";
                return "Connected to " + myAddress + ". Users: " + myUserList();
        }
        return std::string();
    }

    std::string getJoinCode() const override {
        return myRole == ROLE_HOST ? myJoinCode : std::string();
    }

    std::string getReplyCode() const override {
        return myRole == ROLE_GUEST && myState == STATE_CONNECTING
                   ? myReplyCode
                   : std::string();
    }

    // Host: describes how guests on the internet can reach us.
    std::string myReachability() const {
        auto upnp = myPortMapper.getResult();
        if (myUsableUpnp(upnp)) {
            return std::format("UPnP forwarded port {}.", upnp.external.port);
        }
        if (!myPublicAddress.isValid()) {
            return "Public address unknown; only LAN guests can join.";
        }
        if (mySymmetricNat) {
            return "No UPnP, and your router may block hole punching.";
        }
        return "No UPnP; guests may need to send a reply code.";
    }

    // A UPnP mapping on a router that is itself behind another NAT does not
    // help guests on the internet.
    bool myUsableUpnp(const PortMapper::Result& upnp) const {
        if (upnp.state != PortMapper::STATE_MAPPED) return false;
        uint32_t ip = upnp.external.ip ? upnp.external.ip : myPublicAddress.ip;
        return ip != 0 && !NetEndpoint::isPrivateIp(ip);
    }

    // Host: builds the join code once STUN and UPnP are done (or took too
    // long). It is updated if UPnP finishes later.
    void myUpdateJoinCode() {
        auto upnp = myPortMapper.getResult();
        auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - myHostStart)
                          .count();
        if (waited < JOIN_CODE_WAIT_MS &&
            ((myHasUdp && !myStunDone) ||
             upnp.state == PortMapper::STATE_PENDING)) {
            return;
        }
        JoinCode code;
        code.token = myToken;
        code.port = static_cast<uint16_t>(myPort);
        code.stun = myPublicAddress;
        code.lan = myLocalIps;
        if (myUsableUpnp(upnp)) {
            code.upnp = upnp.external;
            if (!code.upnp.ip) code.upnp.ip = myPublicAddress.ip;
        }
        std::string text = EncodeJoinCode(code);
        if (text == myJoinCode) return;
        bool updated = !myJoinCode.empty();
        myJoinCode = text;
        if (updated) {
            HudInfo("The join code was updated.");
        } else {
            HudInfo("Hosting session. %s", myReachability().c_str());
        }
    }

    // Guest: shows the reply code once direct connections failed and our
    // public address is known.
    void myUpdateReplyCode() {
        if (!myDirectFailed || !myStunDone || !myReplyCode.empty()) return;
        if (!myPublicAddress.isValid()) {
            HudError(
                "Could not reach the host directly, and your public address "
                "could not be found (STUN).");
            return;
        }
        ReplyCode code;
        code.token = myToken;
        code.port = myGuestPort;
        code.stun = myPublicAddress;
        code.lan = myLocalIps;
        myReplyCode = EncodeReplyCode(code);
        HudInfo(
            "Could not reach the host directly. Copy your reply code and send "
            "it to the host.");
        if (mySymmetricNat) {
            HudWarning("Your router may block hole punching.");
        }
    }

    const std::vector<Peer>& getPeers() const override { return myPeers; }

    // Returns true if changes can be exchanged with the other users.
    bool myIsSyncing() const {
        if (myRole == ROLE_HOST) return true;
        return myRole == ROLE_GUEST && myState == STATE_CONNECTED && myHasState;
    }

    // ================================================================================================
    // SessionImpl :: chart ids.

    Simfile* mySim() { return gSimfile->getForRemoteEdit(); }

    uint32_t myChartId(const Chart* chart) const {
        if (!chart) return 0;
        auto it = myChartIds.find(chart);
        return it == myChartIds.end() ? 0 : it->second;
    }

    // Returns the chart with the given id, if it is still in the simfile.
    Chart* myFindChart(uint32_t id) {
        Simfile* sim = mySim();
        if (!sim || id == 0) return nullptr;
        for (Chart* chart : sim->charts) {
            if (myChartId(chart) == id) return chart;
        }
        return nullptr;
    }

    void mySetChartId(const Chart* chart, uint32_t id) {
        myChartIds[chart] = id;
        ChartSync& sync = myChartSync[id];
        sync = ChartSync();
        sync.knownChecksum = Checksum(chart->notes);
    }

    // Host: gives every chart in the simfile an id. Charts added later get an
    // id when the next guest joins, and only exist for guests that joined
    // after that.
    void myAssignChartIds() {
        Simfile* sim = mySim();
        if (!sim) return;
        for (const Chart* chart : sim->charts) {
            if (myChartId(chart) == 0) mySetChartId(chart, myNextChartId++);
        }
    }

    // ================================================================================================
    // SessionImpl :: sections.

    // Returns the local version of a section, or false if it does not exist.
    bool myWriteSection(SectionType type, uint32_t id, Bytes& out) {
        Simfile* sim = mySim();
        if (!sim) return false;
        switch (type) {
            case SECTION_SONG_META:
                out = WriteSongMeta(sim);
                return true;
            case SECTION_TEMPO: {
                if (id == 0) {
                    out = WriteTempo(sim->tempo);
                    return true;
                }
                Chart* chart = myFindChart(id);
                if (!chart) return false;
                out = WriteChartTempo(chart);
                return true;
            }
            case SECTION_CHART_PROPS: {
                Chart* chart = myFindChart(id);
                if (!chart) return false;
                out = WriteChartProps(chart);
                return true;
            }
        }
        return false;
    }

    // Applies a section received from another user. Returns false if the
    // data is invalid or refers to an unknown chart.
    bool myApplySection(SectionType type, uint32_t id, const Bytes& data) {
        Simfile* sim = mySim();
        if (!sim) return false;
        switch (type) {
            case SECTION_SONG_META:
                return ApplySongMeta(sim, data);
            case SECTION_TEMPO: {
                Tempo tempo;
                bool hasTempo = false;
                if (!ReadTempo(data, hasTempo, tempo)) return false;
                if (id == 0) {
                    if (!hasTempo) return false;
                    sim->tempo->copy(&tempo);
                    gTempo->onRemoteTempoChanged(sim->tempo);
                    return true;
                }
                Chart* chart = myFindChart(id);
                if (!chart) return false;
                SetChartTempo(chart, hasTempo, tempo);
                gTempo->onRemoteTempoChanged(chart->tempo);
                return true;
            }
            case SECTION_CHART_PROPS: {
                ChartProps props;
                Chart* chart = myFindChart(id);
                if (!chart || !ReadChartProps(data, props)) return false;
                SetChartProps(chart, props);
                gSimfile->onRemoteChartsChanged(nullptr, false);
                return true;
            }
        }
        return false;
    }

    template <typename F>
    void myForEachSection(F&& func) {
        func(SECTION_SONG_META, 0);
        func(SECTION_TEMPO, 0);
        Simfile* sim = mySim();
        if (!sim) return;
        for (const Chart* chart : sim->charts) {
            uint32_t id = myChartId(chart);
            if (id == 0) continue;
            func(SECTION_TEMPO, id);
            func(SECTION_CHART_PROPS, id);
        }
    }

    // Remembers the current local version of every section as synced.
    void myStoreSectionHashes() {
        myForEachSection([this](SectionType type, uint32_t id) {
            Bytes data;
            if (myWriteSection(type, id, data)) {
                mySectionHashes[SectionKey(type, id)] = HashBytes(data);
            }
        });
    }

    static void WriteStateMessage(WriteStream& out, uint32_t origin,
                                  SectionType type, uint32_t id,
                                  const Bytes& data) {
        out.write<uint32_t>(origin);
        out.write<uint8_t>(type);
        out.write<uint32_t>(id);
        WriteBytes(out, data);
    }

    // Sends the sections that were changed locally.
    void myFlushSections() {
        if (!mySectionsDirty || !myIsSyncing()) return;
        mySectionsDirty = false;
        myForEachSection([this](SectionType type, uint32_t id) {
            Bytes data;
            if (!myWriteSection(type, id, data)) return;
            uint64_t key = SectionKey(type, id);
            uint64_t hash = HashBytes(data);
            auto it = mySectionHashes.find(key);
            if (it != mySectionHashes.end() && it->second == hash) return;
            mySectionHashes[key] = hash;

            WriteStream out;
            WriteStateMessage(out, myId, type, id, data);
            mySendChange(MSG_STATE, out);
            if (myRole == ROLE_GUEST) ++myPendingSections[key];
        });
    }

    // Applies a section and marks it as synced.
    void myApplyAndStoreSection(SectionType type, uint32_t id,
                                const Bytes& data) {
        if (!myApplySection(type, id, data)) return;
        gHistory->markExternalChange();
        Bytes local;
        if (myWriteSection(type, id, local)) {
            mySectionHashes[SectionKey(type, id)] = HashBytes(local);
        }
    }

    void myHandleState(uint32_t sender, ReadStream& in) {
        uint32_t origin = in.read<uint32_t>();
        auto type = static_cast<SectionType>(in.read<uint8_t>());
        uint32_t id = in.read<uint32_t>();
        Bytes data = ReadBytes(in);
        if (!in.success()) return;
        uint64_t key = SectionKey(type, id);

        if (myRole == ROLE_HOST) {
            // The last version received by the host wins. The host's version
            // is sent back to everyone, including the sender, so that it
            // always gets an echo.
            auto it = mySectionHashes.find(key);
            if (it == mySectionHashes.end() || it->second != HashBytes(data)) {
                myApplyAndStoreSection(type, id, data);
            }
            Bytes current;
            if (!myWriteSection(type, id, current)) current = data;
            WriteStream out;
            WriteStateMessage(out, sender, type, id, current);
            myBroadcast(MSG_STATE, out);
            return;
        }

        int& pending = myPendingSections[key];
        if (origin == myId) {
            // The host's echo of our own change.
            if (pending > 0) --pending;
            if (pending > 0) return;
        } else if (pending > 0) {
            // Our own pending change will overwrite this on the host.
            return;
        }
        auto it = mySectionHashes.find(key);
        if (it != mySectionHashes.end() && it->second == HashBytes(data)) {
            return;
        }
        myApplyAndStoreSection(type, id, data);
    }

    // ================================================================================================
    // SessionImpl :: notes.

    void onNotesApplied(const Chart* chart, const NoteList& add,
                        const NoteList& rem) override {
        if (!myIsSyncing() || (add.empty() && rem.empty())) return;
        uint32_t id = myChartId(chart);
        if (id == 0) return;

        ChartSync& sync = myChartSync[id];
        sync.knownChecksum = Checksum(chart->notes);

        WriteStream out;
        out.write<uint32_t>(myId);
        out.write<uint32_t>(id);
        add.encode(out, false);
        rem.encode(out, false);
        out.write<uint32_t>(sync.knownChecksum);
        mySendChange(MSG_NOTE_EDIT, out);
        if (myRole == ROLE_GUEST) ++sync.pendingEdits;
    }

    void mySendReplace(uint32_t origin, uint32_t id, const Chart* chart,
                       uint32_t to) {
        WriteStream out;
        out.write<uint32_t>(origin);
        out.write<uint32_t>(id);
        chart->notes.encode(out, false);
        out.write<uint32_t>(Checksum(chart->notes));
        if (to == UINT32_MAX) {
            mySendChange(MSG_NOTE_REPLACE, out);
        } else {
            mySendTo(to, MSG_NOTE_REPLACE, out);
        }
    }

    // Sends the full notes of charts that were changed without going through
    // onNotesApplied (e.g. inserting rows).
    void myCheckNoteChanges() {
        Simfile* sim = mySim();
        if (!sim || !myIsSyncing()) return;
        for (const Chart* chart : sim->charts) {
            uint32_t id = myChartId(chart);
            if (id == 0) continue;
            ChartSync& sync = myChartSync[id];
            uint32_t checksum = Checksum(chart->notes);
            if (checksum == sync.knownChecksum) continue;
            sync.knownChecksum = checksum;
            mySendReplace(myId, id, chart, UINT32_MAX);
            if (myRole == ROLE_GUEST) ++sync.pendingEdits;
        }
    }

    void myHandleNotes(MessageType type, uint32_t sender, ReadStream& in) {
        uint32_t origin = in.read<uint32_t>();
        uint32_t id = in.read<uint32_t>();
        NoteList add, rem;
        add.decode(in, 0);
        if (type == MSG_NOTE_EDIT) rem.decode(in, 0);
        uint32_t checksum = in.read<uint32_t>();
        if (!in.success()) return;
        if (myRole == ROLE_HOST) origin = sender;

        Chart* chart = myFindChart(id);
        ChartSync& sync = myChartSync[id];
        bool own = myRole == ROLE_GUEST && origin == myId;
        if (own) {
            if (sync.pendingEdits > 0) --sync.pendingEdits;
        } else if (chart) {
            // Changes from others are applied without history entries, so
            // undo/redo never reverts them.
            if (type == MSG_NOTE_EDIT) {
                gNotes->applyRemoteEdit(chart, add, rem);
            } else {
                gNotes->replaceNotes(chart, add);
                sync.resyncRequested = false;
            }
            gHistory->markExternalChange();
            sync.knownChecksum = Checksum(chart->notes);
        }

        if (myRole == ROLE_HOST) {
            if (!chart) return;
            // Relay the change to everyone, as acknowledgement for the sender.
            WriteStream out;
            out.write<uint32_t>(origin);
            out.write<uint32_t>(id);
            add.encode(out, false);
            if (type == MSG_NOTE_EDIT) rem.encode(out, false);
            out.write<uint32_t>(sync.knownChecksum);
            myBroadcast(type, out);
            return;
        }

        // Concurrent edits can end up applied in a different order than on
        // the host; ask for the host's notes when they differ.
        if (chart && sync.pendingEdits == 0 && !sync.resyncRequested &&
            Checksum(chart->notes) != checksum) {
            sync.resyncRequested = true;
            WriteStream out;
            out.write<uint32_t>(id);
            mySendTo(HOST_ID, MSG_RESYNC_REQUEST, out);
        }
    }

    // ================================================================================================
    // SessionImpl :: change notifications.

    void onChanges(int changes) override {
        if (myRole == ROLE_NONE) return;

        // The session is bound to the simfile that was open when it started.
        if ((changes & VCM_FILE_CHANGED) || gSimfile->get() != mySimfile) {
            if (myRole == ROLE_HOST) {
                for (auto& peer : myPeerInfo) {
                    mySendDisconnect(peer.id, "The host closed the song.");
                }
            } else {
                mySendDisconnect(HOST_ID, "Closed the song.");
            }
            myEndSession();
            HudInfo("Session ended because the song was closed.");
            return;
        }

        if (!myIsSyncing()) return;

        if (changes & (VCM_TEMPO_CHANGED | VCM_SONG_PROPERTIES_CHANGED |
                       VCM_CHART_PROPERTIES_CHANGED | VCM_CHART_LIST_CHANGED |
                       VCM_BANNER_PATH_CHANGED | VCM_BACKGROUND_PATH_CHANGED)) {
            mySectionsDirty = true;
        }
        if (changes & (VCM_NOTES_CHANGED | VCM_CHART_LIST_CHANGED)) {
            myCheckNoteChanges();
        }
        if (changes & VCM_CHART_CHANGED) {
            myForcePresence = true;
            myForceSelection = true;
        }
    }

    // ================================================================================================
    // SessionImpl :: presence.

    Presence myLocalPresence() {
        Presence p;
        if (!gSimfile->isOpen()) return p;
        p.chart = myChartId(gChart->get());
        p.viewTime = gView->getCursorTime();

        // The last mouse position is kept when the mouse leaves the window.
        vec2i mouse = gSystem->getMousePos();
        recti view = gView->getRect();
        if (SDL_GetMouseFocus() && IsInside(view, mouse.x, mouse.y) &&
            !gTextOverlay->isOpen()) {
            p.hasHover = true;
            p.hoverTime = gView->offsetToTime(gView->yToOffset(mouse.y));
            p.hoverX = (mouse.x - gView->getReceptorCoords().xc) /
                       std::max(PixelsPerUnit(), 0.001);
        }
        return p;
    }

    static void WritePresence(WriteStream& out, uint32_t origin,
                              const Presence& p) {
        out.write<uint32_t>(origin);
        out.write<uint32_t>(p.chart);
        out.write<uint8_t>(p.hasHover ? 1 : 0);
        out.write<double>(p.hoverTime);
        out.write<double>(p.hoverX);
        out.write<double>(p.viewTime);
    }

    void mySendPresence() {
        if (!myIsSyncing()) return;
        Presence p = myLocalPresence();
        auto now = Clock::now();
        if (!myForcePresence) {
            if (p == myPresence) return;
            if (now - myPresenceTime <
                std::chrono::milliseconds(PRESENCE_INTERVAL_MS)) {
                return;
            }
        }
        myForcePresence = false;
        myPresence = p;
        myPresenceTime = now;
        WriteStream out;
        WritePresence(out, myId, p);
        mySendChange(MSG_PRESENCE, out);
    }

    void myHandlePresence(uint32_t sender, ReadStream& in) {
        uint32_t origin = in.read<uint32_t>();
        Presence p;
        in.read(p.chart);
        p.hasHover = in.read<uint8_t>() != 0;
        in.read(p.hoverTime);
        in.read(p.hoverX);
        in.read(p.viewTime);
        if (!in.success()) return;
        if (myRole == ROLE_HOST) origin = sender;

        PeerInfo* peer = myFindPeer(origin);
        if (!peer) return;
        peer->presence = p;

        if (myRole == ROLE_HOST) {
            WriteStream out;
            WritePresence(out, origin, p);
            myBroadcast(MSG_PRESENCE, out, origin);
        }
    }

    // Rebuilds the list returned by getPeers().
    void myUpdatePeers() {
        myPeers.clear();
        uint32_t chart = myChartId(gChart->get());
        for (auto& info : myPeerInfo) {
            if (myRole == ROLE_HOST && !info.joined) continue;
            Peer peer;
            peer.id = info.id;
            peer.name = info.name;
            peer.color = PeerColor(info.id);
            peer.onChart = chart != 0 && info.presence.chart == chart;
            peer.hasHover = info.presence.hasHover;
            peer.hoverTime = info.presence.hoverTime;
            peer.hoverX = info.presence.hoverX;
            peer.viewTime = info.presence.viewTime;
            myPeers.push_back(peer);
        }
    }

    PeerInfo* myFindPeer(uint32_t id) {
        for (auto& peer : myPeerInfo) {
            if (peer.id == id) return &peer;
        }
        return nullptr;
    }

    // ================================================================================================
    // SessionImpl :: selection.

    static void WriteSelection(WriteStream& out, uint32_t origin,
                               uint32_t chart, int regionBegin, int regionEnd,
                               const std::vector<RowCol>& notes) {
        out.write<uint32_t>(origin);
        out.write<uint32_t>(chart);
        out.write<int32_t>(regionBegin);
        out.write<int32_t>(regionEnd);
        auto count = static_cast<uint32_t>(
            std::min<size_t>(notes.size(), MAX_SELECTION_SIZE));
        out.writeNum(count);
        for (uint32_t i = 0; i < count; ++i) {
            out.writeNum(static_cast<uint32_t>(std::max(0, notes[i].row)));
            out.write<uint8_t>(static_cast<uint8_t>(notes[i].col));
        }
    }

    // Writes the selection of the local user.
    void myWriteLocalSelection(WriteStream& out) {
        uint32_t chart = myChartId(gChart->get());
        std::vector<RowCol> notes;
        int begin = 0, end = 0;
        if (chart != 0 && gChart->isOpen()) {
            for (const auto& note : *gNotes) {
                if (note.isSelected) notes.push_back({note.row, note.col});
            }
            auto region = gSelection->getSelectedRegion();
            begin = region.beginRow;
            end = region.endRow;
        }
        WriteSelection(out, myId, chart, begin, end, notes);
    }

    // Sends the selected notes and region when they changed, so that other
    // users can see what is highlighted.
    void mySendSelection() {
        if (!myIsSyncing()) return;
        auto now = Clock::now();
        if (!myForceSelection &&
            now - mySelectionTime <
                std::chrono::milliseconds(SELECTION_INTERVAL_MS)) {
            return;
        }
        mySelectionTime = now;
        WriteStream out;
        myWriteLocalSelection(out);
        uint64_t hash = HashBytes(out.data(), static_cast<size_t>(out.size()));
        if (!myForceSelection && hash == mySelectionHash) return;
        myForceSelection = false;
        mySelectionHash = hash;
        mySendChange(MSG_SELECTION, out);
    }

    void myHandleSelection(uint32_t sender, ReadStream& in) {
        uint32_t origin = in.read<uint32_t>();
        uint32_t chart = in.read<uint32_t>();
        int regionBegin = in.read<int32_t>();
        int regionEnd = in.read<int32_t>();
        uint32_t count = in.readNum();
        if (!in.success() || count > MAX_SELECTION_SIZE) return;
        std::vector<RowCol> notes;
        notes.reserve(std::min<uint32_t>(count, in.bytesleft() / 2 + 1));
        for (uint32_t i = 0; i < count && in.success(); ++i) {
            RowCol n;
            n.row = static_cast<int>(in.readNum());
            n.col = in.read<uint8_t>();
            notes.push_back(n);
        }
        if (!in.success()) return;
        if (myRole == ROLE_HOST) origin = sender;

        PeerInfo* peer = myFindPeer(origin);
        if (!peer) return;
        peer->selChart = chart;
        peer->regionBegin = regionBegin;
        peer->regionEnd = regionEnd;
        peer->selection = std::move(notes);

        if (myRole == ROLE_HOST) {
            WriteStream out;
            WriteSelection(out, origin, chart, regionBegin, regionEnd,
                           peer->selection);
            myBroadcast(MSG_SELECTION, out, origin);
        }
    }

    // ================================================================================================
    // SessionImpl :: receiving.

    void tick() override {
        if (myRejoinPending) myCheckRejoin();
        if (myRole == ROLE_NONE) return;

        // Send local changes before applying remote ones, so that pending
        // changes are known when the host's echoes arrive.
        myFlushSections();

        NetEvent evt;
        while (myRole != ROLE_NONE && myLink.poll(evt)) {
            if (myRole == ROLE_HOST) {
                myHandleHostEvent(evt);
            } else {
                myHandleGuestEvent(evt);
            }
        }
        if (myRole == ROLE_NONE) return;
        if (myRole == ROLE_HOST) myUpdateJoinCode();

        mySendPresence();
        mySendSelection();
        myUpdatePeers();
    }

    // ================================================================================================
    // SessionImpl :: host.

    std::string myUniqueName(std::string name, uint32_t id) const {
        if (name.empty()) name = "Guest " + std::to_string(id);
        std::string base = name;
        for (int n = 2;; ++n) {
            bool taken = (name == myHostName());
            for (auto& peer : myPeerInfo) {
                taken |= peer.joined && peer.id != id && peer.name == name;
            }
            if (!taken) return name;
            name = base + " (" + std::to_string(n) + ")";
        }
    }

    std::string myHostName() const {
        return myName.empty() ? std::string("Host") : myName;
    }

    void myBroadcastPeerList() {
        WriteStream out;
        uint32_t count = 1;
        for (auto& peer : myPeerInfo) count += peer.joined ? 1 : 0;
        out.write<uint32_t>(count);
        out.write<uint32_t>(HOST_ID);
        out.writeStr(myHostName());
        for (auto& peer : myPeerInfo) {
            if (!peer.joined) continue;
            out.write<uint32_t>(peer.id);
            out.writeStr(peer.name);
        }
        myBroadcast(MSG_PEER_LIST, out);
    }

    void myRemovePeer(uint32_t id, const char* message) {
        auto it = std::find_if(myPeerInfo.begin(), myPeerInfo.end(),
                               [id](const PeerInfo& p) { return p.id == id; });
        if (it == myPeerInfo.end()) return;
        bool joined = it->joined;
        std::string name = it->name;
        myPeerInfo.erase(it);
        if (joined) {
            HudInfo("%s %s", name.c_str(), message);
            myBroadcastPeerList();
        }
    }

    void mySendFullState(uint32_t peer) {
        myAssignChartIds();
        Simfile* sim = mySim();
        WriteStream out;
        WriteBytes(out, WriteSongMeta(sim));
        WriteBytes(out, WriteTempo(sim->tempo));
        out.write<uint32_t>(static_cast<uint32_t>(sim->charts.size()));
        for (const Chart* chart : sim->charts) {
            out.write<uint32_t>(myChartId(chart));
            out.writeStr(chart->style ? chart->style->id : std::string());
            WriteBytes(out, WriteChartProps(chart));
            WriteBytes(out, WriteChartTempo(chart));
            chart->notes.encode(out, false);
        }
        mySendTo(peer, MSG_FULL_STATE, out);
    }

    void myHandleHello(PeerInfo& peer, ReadStream& in) {
        uint32_t magic = in.read<uint32_t>();
        uint16_t version = in.read<uint16_t>();
        std::string name;
        SongInfo guestSong;
        if (in.success() && magic == PROTOCOL_MAGIC &&
            version == PROTOCOL_VERSION) {
            in.readStr(name);
            guestSong.hasSong = in.read<uint8_t>() != 0;
            in.read(guestSong.audioHash);
            in.read(guestSong.duration);
        }
        uint32_t token = in.read<uint32_t>();

        std::string reason;
        int numJoined = 0;
        for (auto& p : myPeerInfo) numJoined += p.joined ? 1 : 0;
        if (!in.success() || magic != PROTOCOL_MAGIC) {
            reason = "Not an ArrowVortex session.";
        } else if (version != PROTOCOL_VERSION) {
            reason = std::format(
                "Protocol version mismatch (host {}, guest {}). Use the same "
                "ArrowVortex version.",
                PROTOCOL_VERSION, version);
        } else if (token != myToken) {
            reason = "Invalid join code.";
        } else if (numJoined >= MAX_GUESTS) {
            reason = "The session is full.";
        }

        // A guest without the same song downloads it first.
        bool download =
            reason.empty() &&
            (!guestSong.hasSong || !CompareSongs(mySong, guestSong).empty());

        WriteStream out;
        out.write<uint8_t>(!reason.empty() ? WELCOME_REJECTED
                           : download      ? WELCOME_DOWNLOAD
                                           : WELCOME_OK);
        out.writeStr(reason);
        out.write<uint32_t>(peer.id);
        if (download) {
            out.writeStr(mySongFolder);
            out.writeStr(myMusicName);
            out.write<uint64_t>(mySong.audioHash);
            out.write<uint64_t>(mySong.audioSize);
        }
        mySendTo(peer.id, MSG_WELCOME, out);

        if (download) {
            peer.downloading = true;
            peer.name = SanitizeName(name);
            HudInfo("%s is downloading the song.",
                    peer.name.empty() ? "A guest" : peer.name.c_str());
            return;
        }

        if (!reason.empty()) {
            uint32_t id = peer.id;
            myLink.dropPeer(id);
            myRemovePeer(id, "was rejected.");
            HudWarning("Rejected a guest: %s", reason.c_str());
            return;
        }

        peer.name = myUniqueName(SanitizeName(name), peer.id);
        peer.joined = true;

        mySendFullState(peer.id);
        myBroadcastPeerList();

        // Let the new guest know where everyone is.
        {
            WriteStream presence;
            WritePresence(presence, HOST_ID, myLocalPresence());
            mySendTo(peer.id, MSG_PRESENCE, presence);
        }
        for (auto& other : myPeerInfo) {
            if (!other.joined || other.id == peer.id) continue;
            WriteStream presence;
            WritePresence(presence, other.id, other.presence);
            mySendTo(peer.id, MSG_PRESENCE, presence);
        }

        // And what everyone has selected.
        {
            WriteStream selection;
            myWriteLocalSelection(selection);
            mySendTo(peer.id, MSG_SELECTION, selection);
        }
        for (auto& other : myPeerInfo) {
            if (!other.joined || other.id == peer.id) continue;
            WriteStream selection;
            WriteSelection(selection, other.id, other.selChart,
                           other.regionBegin, other.regionEnd, other.selection);
            mySendTo(peer.id, MSG_SELECTION, selection);
        }
        HudInfo("%s joined the session.", peer.name.c_str());
    }

    // Sends one file in chunks. Returns false if it could not be read.
    bool mySendFile(uint32_t peer, FileKind kind, const fs::path& path,
                    const std::string& name, uint64_t maxSize) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        auto end = file.tellg();
        if (end < 0) return false;
        auto size = static_cast<uint64_t>(end);
        if (size > maxSize) return false;
        file.seekg(0);

        Bytes chunk(FILE_CHUNK_SIZE);
        uint64_t offset = 0;
        do {
            auto n = static_cast<uint32_t>(
                std::min<uint64_t>(FILE_CHUNK_SIZE, size - offset));
            file.read(reinterpret_cast<char*>(chunk.data()), n);
            if (static_cast<uint64_t>(file.gcount()) != n) return false;

            WriteStream out;
            out.write<uint8_t>(kind);
            out.writeStr(name);
            out.write<uint64_t>(size);
            out.write<uint64_t>(offset);
            out.writeNum(n);
            if (n > 0) out.write(chunk.data(), static_cast<int>(n));
            mySendTo(peer, MSG_FILE_DATA, out);
            offset += n;
        } while (offset < size);
        return true;
    }

    // Sends the music, simfile and images to a guest that does not have the
    // song.
    void mySendSongFiles(uint32_t peer, bool needMusic) {
        const Simfile* sim = mySim();
        if (!sim) return;
        fs::path dir = utf8ToPath(sim->dir);

        if (needMusic &&
            !mySendFile(peer, FILE_MUSIC, dir / utf8ToPath(sim->music),
                        myMusicName, MAX_MUSIC_SIZE)) {
            mySendDisconnect(peer, "The host could not read the music file.");
            myLink.dropPeer(peer);
            return;
        }

        // The simfile as it was last saved; the current state is sent when
        // the guest joins.
        std::string simfileName;
        if (IsSimfileName(sim->file) &&
            mySendFile(peer, FILE_SIMFILE, dir / utf8ToPath(sim->file),
                       sim->file, MAX_OTHER_FILE_SIZE)) {
            simfileName = sim->file;
        }

        std::vector<std::string> sent = {myMusicName, simfileName};
        for (const std::string* image :
             {&sim->banner, &sim->background, &sim->cdTitle}) {
            if (SafeFileName(*image).empty()) continue;
            if (std::find(sent.begin(), sent.end(), *image) != sent.end()) {
                continue;
            }
            if (mySendFile(peer, FILE_IMAGE, dir / utf8ToPath(*image), *image,
                           MAX_OTHER_FILE_SIZE)) {
                sent.push_back(*image);
            }
        }

        WriteStream out;
        out.writeStr(simfileName);
        out.writeStr(myMusicName);
        mySendTo(peer, MSG_DOWNLOAD_DONE, out);
    }

    void myHandleHostEvent(NetEvent& evt) {
        switch (evt.type) {
            case NetEvent::CONNECTED: {
                PeerInfo peer;
                peer.id = evt.peer;
                myPeerInfo.push_back(peer);
                return;
            }
            case NetEvent::LOST:
                myRemovePeer(evt.peer, "left the session.");
                return;
            case NetEvent::STUN_DONE:
                myStunDone = true;
                myPublicAddress = evt.endpoint;
                mySymmetricNat = evt.flag;
                return;
            case NetEvent::FAILED:
            case NetEvent::DIRECT_FAILED:
                return;
            case NetEvent::FRAME:
                break;
        };

        PeerInfo* peer = myFindPeer(evt.peer);
        if (!peer || evt.data.empty()) return;

        auto type = static_cast<MessageType>(evt.data[0]);
        ReadStream in(evt.data.data() + 1,
                      static_cast<int>(evt.data.size() - 1));

        if (!peer->joined) {
            if (type == MSG_HELLO && !peer->downloading) {
                myHandleHello(*peer, in);
            } else if (type == MSG_DOWNLOAD_REQUEST && peer->downloading) {
                uint8_t needMusic = in.read<uint8_t>();
                if (in.success()) mySendSongFiles(peer->id, needMusic != 0);
            } else if (type == MSG_DISCONNECT) {
                uint32_t id = evt.peer;
                myLink.dropPeer(id);
                myRemovePeer(id, "left the session.");
            }
            return;
        }

        switch (type) {
            case MSG_NOTE_EDIT:
            case MSG_NOTE_REPLACE:
                myHandleNotes(type, evt.peer, in);
                return;
            case MSG_STATE:
                myHandleState(evt.peer, in);
                return;
            case MSG_PRESENCE:
                myHandlePresence(evt.peer, in);
                return;
            case MSG_SELECTION:
                myHandleSelection(evt.peer, in);
                return;
            case MSG_RESYNC_REQUEST: {
                uint32_t id = in.read<uint32_t>();
                const Chart* chart = myFindChart(id);
                if (in.success() && chart) {
                    mySendReplace(HOST_ID, id, chart, evt.peer);
                }
                return;
            }
            case MSG_DISCONNECT: {
                uint32_t id = evt.peer;
                myLink.dropPeer(id);
                myRemovePeer(id, "left the session.");
                return;
            }
            default:
                return;
        }
    }

    // ================================================================================================
    // SessionImpl :: guest.

    void myHandleGuestEvent(NetEvent& evt) {
        switch (evt.type) {
            case NetEvent::STUN_DONE:
                myStunDone = true;
                myPublicAddress = evt.endpoint;
                mySymmetricNat = evt.flag;
                myUpdateReplyCode();
                return;
            case NetEvent::DIRECT_FAILED:
                myDirectFailed = true;
                myUpdateReplyCode();
                return;
            case NetEvent::CONNECTED: {
                myState = STATE_HANDSHAKE;
                myAddress = evt.text + (evt.flag ? " (hole punched)" : "");
                myReplyCode.clear();
                WriteStream out;
                out.write<uint32_t>(PROTOCOL_MAGIC);
                out.write<uint16_t>(PROTOCOL_VERSION);
                out.writeStr(myName);
                out.write<uint8_t>(mySong.hasSong ? 1 : 0);
                out.write<uint64_t>(mySong.audioHash);
                out.write<double>(mySong.duration);
                out.write<uint32_t>(myToken);
                mySendTo(HOST_ID, MSG_HELLO, out);
                return;
            }
            case NetEvent::FAILED:
                HudError("Could not join: %s", evt.text.c_str());
                myEndSession();
                return;
            case NetEvent::LOST:
                HudError("Lost connection to the host: %s", evt.text.c_str());
                myEndSession();
                return;
            case NetEvent::FRAME:
                break;
        };
        if (evt.data.empty()) return;

        auto type = static_cast<MessageType>(evt.data[0]);
        ReadStream in(evt.data.data() + 1,
                      static_cast<int>(evt.data.size() - 1));

        switch (type) {
            case MSG_WELCOME: {
                uint8_t result = in.read<uint8_t>();
                std::string reason = in.readStr();
                uint32_t id = in.read<uint32_t>();
                if (in.success() && result == WELCOME_OK) {
                    myId = id;
                    myState = STATE_CONNECTED;
                } else if (in.success() && result == WELCOME_DOWNLOAD) {
                    myId = id;
                    myStartDownload(in);
                } else {
                    HudError("Join rejected: %s", reason.c_str());
                    myEndSession();
                }
                return;
            }
            case MSG_FILE_DATA:
                if (myState == STATE_DOWNLOADING) myHandleFileData(in);
                return;
            case MSG_DOWNLOAD_DONE:
                if (myState == STATE_DOWNLOADING) myFinishDownload(in);
                return;
            case MSG_FULL_STATE:
                myApplyFullState(in);
                return;
            case MSG_PEER_LIST:
                myHandlePeerList(in);
                return;
            case MSG_DISCONNECT: {
                std::string reason = in.readStr();
                HudInfo("Session ended: %s", reason.c_str());
                myEndSession();
                return;
            }
            default:
                break;
        }

        if (!myHasState) return;
        switch (type) {
            case MSG_NOTE_EDIT:
            case MSG_NOTE_REPLACE:
                myHandleNotes(type, HOST_ID, in);
                return;
            case MSG_STATE:
                myHandleState(HOST_ID, in);
                return;
            case MSG_PRESENCE:
                myHandlePresence(HOST_ID, in);
                return;
            case MSG_SELECTION:
                myHandleSelection(HOST_ID, in);
                return;
            default:
                return;
        }
    }

    // ================================================================================================
    // SessionImpl :: guest song download.

    void myDownloadFailed(const std::string& reason) {
        HudError("Song download failed: %s", reason.c_str());
        disconnect();
    }

    void myStartDownload(ReadStream& in) {
        auto d = std::make_unique<Download>();
        in.readStr(d->folder);
        in.readStr(d->musicName);
        in.read(d->musicHash);
        in.read(d->musicSize);
        if (!in.success() || SafeFileName(d->musicName).empty()) {
            myDownloadFailed("the host sent an invalid music file name.");
            return;
        }

        // Each song gets its own folder. Reuse a folder that has the same
        // music, otherwise pick a new name.
        fs::path saves = SavesDir();
        std::string base = SafeFolderName(d->folder);
        std::error_code ec;
        for (int n = 1;; ++n) {
            std::string name =
                n == 1 ? base : base + " (" + std::to_string(n) + ")";
            fs::path dir = saves / utf8ToPath(name);
            if (!fs::exists(dir, ec)) {
                d->dir = dir;
                break;
            }
            uint64_t hash = 0;
            fs::path music = dir / utf8ToPath(d->musicName);
            if (fs::exists(music, ec) && HashFile(music, hash) &&
                hash == d->musicHash) {
                d->dir = dir;
                d->needMusic = false;
                break;
            }
            if (n > 1000) {
                myDownloadFailed("could not find a free folder name.");
                return;
            }
        }
        fs::create_directories(d->dir, ec);
        if (ec) {
            myDownloadFailed("could not create the folder \"" +
                             pathToUtf8(d->dir) + "\".");
            return;
        }

        myState = STATE_DOWNLOADING;
        WriteStream out;
        out.write<uint8_t>(d->needMusic ? 1 : 0);
        mySendTo(HOST_ID, MSG_DOWNLOAD_REQUEST, out);
        if (d->needMusic) {
            HudInfo("Downloading the song from the host (%.1f MB)...",
                    static_cast<double>(d->musicSize) / (1024.0 * 1024.0));
        } else {
            HudInfo("You already have the music; downloading the simfile...");
        }
        myDownload = std::move(d);
    }

    void myHandleFileData(ReadStream& in) {
        Download& d = *myDownload;
        uint8_t kind = in.read<uint8_t>();
        std::string name = in.readStr();
        uint64_t size = in.read<uint64_t>();
        uint64_t offset = in.read<uint64_t>();
        uint32_t n = in.readNum();
        if (!in.success() || n > static_cast<uint32_t>(in.bytesleft()) ||
            kind > FILE_IMAGE || SafeFileName(name).empty() ||
            size >
                (kind == FILE_MUSIC ? MAX_MUSIC_SIZE : MAX_OTHER_FILE_SIZE) ||
            offset + n > size || (kind == FILE_MUSIC && name != d.musicName)) {
            myDownloadFailed("received invalid file data.");
            return;
        }

        if (offset == 0) {
            d.file.close();
            fs::path path = d.dir / utf8ToPath(name);
            std::error_code ec;
            // Keep a backup of a simfile that would be overwritten.
            if (kind == FILE_SIMFILE && fs::exists(path, ec)) {
                fs::path backup = path;
                backup += ".bak";
                fs::rename(path, backup, ec);
            }
            d.file.open(path, std::ios::binary | std::ios::trunc);
            if (!d.file) {
                myDownloadFailed("could not write \"" + pathToUtf8(path) +
                                 "\".");
                return;
            }
            d.fileName = name;
            d.fileSize = size;
            d.fileWritten = 0;
        } else if (name != d.fileName || offset != d.fileWritten) {
            myDownloadFailed("received file data out of order.");
            return;
        }

        if (n > 0) {
            d.file.write(reinterpret_cast<const char*>(in.pos()), n);
            in.skip(static_cast<int>(n));
        }
        if (!d.file) {
            myDownloadFailed("could not write \"" + d.fileName + "\".");
            return;
        }
        d.fileWritten += n;
        d.totalReceived += n;
        if (d.fileWritten == d.fileSize) d.file.close();
    }

    void myFinishDownload(ReadStream& in) {
        std::string simfileName = in.readStr();
        std::string musicName = in.readStr();
        Download& d = *myDownload;
        d.file.close();
        if (!in.success() || musicName != d.musicName ||
            (!simfileName.empty() && SafeFileName(simfileName).empty())) {
            myDownloadFailed("received invalid data.");
            return;
        }
        std::error_code ec;
        fs::path music = d.dir / utf8ToPath(musicName);
        if (!fs::exists(music, ec)) {
            myDownloadFailed("the music file is missing.");
            return;
        }
        fs::path open = music;
        if (!simfileName.empty()) {
            fs::path simfile = d.dir / utf8ToPath(simfileName);
            if (fs::exists(simfile, ec)) open = simfile;
        }

        // Rejoin once the song is open and its music is loaded.
        std::string code = myJoinCodeText;
        std::string folder = pathToUtf8(d.dir);
        mySendDisconnect(HOST_ID, "Downloaded the song.");
        myEndSession();
        HudInfo("Saved the song to \"%s\".", folder.c_str());

        if (!gEditor->openSimfile(open)) {
            HudError("Could not open the downloaded song.");
            return;
        }
        // The simfile can refer to music in another folder; use the
        // downloaded music then.
        const Simfile* sim = gSimfile->get();
        if (open != music &&
            (!sim || sim->music.empty() ||
             !fs::exists(utf8ToPath(sim->dir) / utf8ToPath(sim->music), ec))) {
            if (!gEditor->openSimfile(music)) {
                HudError("Could not open the downloaded song.");
                return;
            }
        }
        myRejoinPending = true;
        myRejoinCode = code;
        myRejoinDeadline =
            Clock::now() + std::chrono::seconds(REJOIN_TIMEOUT_SECONDS);
    }

    // Joins the session again after downloading the song, once the music
    // has finished loading.
    void myCheckRejoin() {
        if (gSimfile->isClosed()) {
            myRejoinPending = false;
            HudError("The downloaded song was closed; not joining.");
            return;
        }
        if (gMusic->getSamples().isCompleted()) {
            myRejoinPending = false;
            join(myRejoinCode);
            return;
        }
        if (Clock::now() > myRejoinDeadline) {
            myRejoinPending = false;
            HudError("Timed out waiting for the music to load; not joining.");
        }
    }

    void myHandlePeerList(ReadStream& in) {
        uint32_t count = in.read<uint32_t>();
        std::vector<PeerInfo> list;
        for (uint32_t i = 0; i < count && in.success(); ++i) {
            PeerInfo info;
            in.read(info.id);
            in.readStr(info.name);
            info.joined = true;
            if (info.id == myId) continue;
            if (PeerInfo* old = myFindPeer(info.id)) {
                info.presence = old->presence;
                info.selChart = old->selChart;
                info.regionBegin = old->regionBegin;
                info.regionEnd = old->regionEnd;
                info.selection = old->selection;
            }
            list.push_back(info);
        }
        if (!in.success()) return;

        for (auto& info : list) {
            if (!myFindPeer(info.id) && myHasState) {
                HudInfo("%s joined the session.", info.name.c_str());
            }
        }
        for (auto& old : myPeerInfo) {
            bool kept =
                std::any_of(list.begin(), list.end(),
                            [&](const PeerInfo& p) { return p.id == old.id; });
            if (!kept) HudInfo("%s left the session.", old.name.c_str());
        }
        myPeerInfo = std::move(list);
    }

    // Replaces the local simfile data with the host's.
    void myApplyFullState(ReadStream& in) {
        Bytes meta = ReadBytes(in);
        Bytes songTempoData = ReadBytes(in);
        uint32_t count = in.read<uint32_t>();
        std::vector<ChartData> charts;
        std::string missingStyle;
        for (uint32_t i = 0; i < count && in.success(); ++i) {
            ChartData data;
            in.read(data.id);
            std::string styleId = in.readStr();
            Bytes props = ReadBytes(in);
            Bytes tempo = ReadBytes(in);
            data.notes.decode(in, 0);
            if (!in.success()) break;
            data.style = gStyle->findStyle(styleId);
            if (!data.style) missingStyle = styleId;
            data.tempo = std::make_unique<Tempo>();
            if (!ReadChartProps(props, data.props) ||
                !ReadTempo(tempo, data.hasTempo, *data.tempo)) {
                in.invalidate();
            }
            charts.push_back(std::move(data));
        }
        Tempo songTempo;
        bool hasSongTempo = false;
        if (!in.success() ||
            !ReadTempo(songTempoData, hasSongTempo, songTempo) ||
            !hasSongTempo) {
            HudError("Received invalid simfile data from the host.");
            disconnect();
            return;
        }
        if (!missingStyle.empty()) {
            HudError("The host's simfile uses a style you don't have (%s).",
                     missingStyle.c_str());
            disconnect();
            return;
        }

        uint64_t before = myHashEverything();

        // History entries describe changes to the old data.
        gHistory->clearAll();

        Simfile* sim = mySim();
        ApplySongMeta(sim, meta);
        sim->tempo->copy(&songTempo);

        // Reuse the existing chart objects where possible.
        std::vector<Chart*> old = sim->charts;
        std::vector<Chart*> list;
        const Chart* open = gChart->get();
        int openIndex = 0;
        for (size_t i = 0; i < old.size(); ++i) {
            if (old[i] == open) openIndex = static_cast<int>(i);
        }
        for (size_t i = 0; i < charts.size(); ++i) {
            Chart* chart = i < old.size() ? old[i] : new Chart;
            ChartData& data = charts[i];
            chart->style = data.style;
            SetChartProps(chart, data.props);
            SetChartTempo(chart, data.hasTempo, *data.tempo);
            chart->notes = data.notes;
            list.push_back(chart);
            mySetChartId(chart, data.id);
        }
        sim->charts = list;

        const Chart* reopen = nullptr;
        if (!list.empty()) {
            reopen =
                list[std::min(openIndex, static_cast<int>(list.size()) - 1)];
        }
        gSimfile->onRemoteChartsChanged(reopen, true);
        for (size_t i = charts.size(); i < old.size(); ++i) delete old[i];

        gTempo->onRemoteTempoChanged(nullptr);
        gEditor->reportChanges(
            VCM_SONG_PROPERTIES_CHANGED | VCM_BANNER_PATH_CHANGED |
            VCM_BACKGROUND_PATH_CHANGED | VCM_CHART_PROPERTIES_CHANGED);

        if (myHashEverything() != before) gHistory->markExternalChange();

        myStoreSectionHashes();
        myHasState = true;
        myForcePresence = true;
        myForceSelection = true;
        HudInfo("Joined the session at %s.", myAddress.c_str());
    }

    // Returns a hash of all synced data, to detect if anything changed.
    uint64_t myHashEverything() {
        Simfile* sim = mySim();
        if (!sim) return 0;
        WriteStream out;
        WriteBytes(out, WriteSongMeta(sim));
        WriteBytes(out, WriteTempo(sim->tempo));
        for (const Chart* chart : sim->charts) {
            out.writeStr(chart->style ? chart->style->id : std::string());
            WriteBytes(out, WriteChartProps(chart));
            WriteBytes(out, WriteChartTempo(chart));
            out.write<uint32_t>(Checksum(chart->notes));
        }
        return HashBytes(out.data(), static_cast<size_t>(out.size()));
    }

    // ================================================================================================
    // SessionImpl :: drawing.

    // Draws what the other users have selected on the open chart.
    void myDrawSelections() {
        uint32_t chart = myChartId(gChart->get());
        if (chart == 0) return;
        auto coords = gView->getReceptorCoords();
        for (const PeerInfo& info : myPeerInfo) {
            if (myRole == ROLE_HOST && !info.joined) continue;
            if (info.selChart != chart) continue;
            uint32_t color = PeerColor(info.id);
            if (info.regionEnd > info.regionBegin) {
                int t = gView->rowToY(info.regionBegin);
                int b = gView->rowToY(info.regionEnd);
                if (t > b) std::swap(t, b);
                recti r = {coords.xl, t, coords.xr - coords.xl, b - t};
                Draw::fill(r, Color32a(color, 40));
                Draw::outline(r, Color32a(color, 170));
            }
            gNotefield->drawPeerSelection(info.selection, Color32a(color, 230));
        }
    }

    // Draws the receptors of the other users, which show where notes are
    // placed when they press a note key.
    void myDrawReceptors() {
        recti view = gView->getRect();
        auto coords = gView->getReceptorCoords();
        int gap = gSystem->applyScaleFactor(gView->applyZoom(48));
        for (const Peer& peer : myPeers) {
            if (!peer.onChart) continue;
            int y = gView->timeToY(peer.viewTime);
            if (y < view.y - 64 || y > view.y + view.h + 64) continue;
            gNotefield->drawPeerReceptors(y, peer.color);
            DrawNameTag(coords.xr + gap, y, peer.name, peer.color, Text::ML);
        }
    }

    void drawCursors() override {
        if (myPeers.empty() || gTextOverlay->isOpen()) return;
        recti view = gView->getRect();
        int xc = gView->getReceptorCoords().xc;
        double unit = PixelsPerUnit();
        Renderer::pushScissorRect(view);
        myDrawSelections();
        myDrawReceptors();
        for (const Peer& peer : myPeers) {
            if (!peer.onChart || !peer.hasHover) continue;
            int x = xc + static_cast<int>(std::lround(peer.hoverX * unit));
            int y = gView->timeToY(peer.hoverTime);
            if (y < view.y - 32 || y > view.y + view.h + 32) continue;
            DrawPointer(x, y, peer.color);
            int s = std::max(1, gSystem->applyScaleFactor(1));
            DrawNameTag(x + 12 * s, y + 18 * s, peer.name, peer.color,
                        Text::ML);
        }
        Renderer::popScissorRect();
    }
};

};  // anonymous namespace

// ================================================================================================
// Session API.

Session* gSession = nullptr;

void Session::create(XmrNode& settings) {
    gSession = new SessionImpl(settings);
}

void Session::destroy() {
    delete static_cast<SessionImpl*>(gSession);
    gSession = nullptr;
}

};  // namespace Vortex
