// SPDX-License-Identifier: GPL-3.0-or-later
// Iteration 8 item 11 (= F6): on-device description reflow.
//
// WHY THIS EXISTS (see NOTES.md "investigation" for the full trail): BOTW item/weapon/armor/
// quest descriptions are authored with hard '\n' breaks sized for the GAME's own main-screen
// text box. This companion shows the SAME msbt text on the bottom screen at a different width,
// so those breaks land mid-sentence. The runtime's own text path (mod_msbt.cpp + Canvas::
// LayoutText, src/core/mods, read-only -- never edited here) always treats '\n' as a hard
// paragraph break with no reflow option, and a module cannot read back the runtime's OWN
// resolved "msbt:<alias>#<label>" text (get_text() only returns what THIS module itself
// published). So the fix has to happen on this side: this header re-fetches and re-decodes the
// SAME msbt files from the player's own romfs via the existing read_romfs() host call (already
// used for images/fonts -- no runtime change needed), using the SAME LBL1+TXT2 "MsgStdBn"
// layout every Nintendo Switch title's message files use (verified against this title's actual
// files; see also the sibling acnh_msbt.{h,cpp} parsers already shipped in other companions'
// native/modules/ for the same format), then applies the reflow rule and publishes the cleaned
// string as a new text output. No localized text is ever embedded in the package: botw_
// description_table.h carries only the (hash, alias, label) identifiers already public in the
// existing text_map tables, not a single word of Nintendo's text.
//
// Current manifest config hard-codes every alias to its USen ("romfs:/Pack/Bootup_USen.pack#...")
// path (no language table) -- this reflow path reads the exact same files, so it is already
// the companion's one supported language; nothing here is region/language-sensitive, but the
// reflow rule itself still future-proofs for the CJK case (no inter-word spaces) in case that
// ever changes (see Reflow()'s cjk handling and NOTES.md "language scope").
//
// This file duplicates a small, stable, well-documented binary format (LMS "MsgStdBn") -- it
// does not alter or depend on runtime internals, and is independently testable (see
// botw_msbt_reflow_test.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "botw_description_table.h"

namespace botw {

// ---- LMS "MsgStdBn" (LBL1 + TXT2) ---------------------------------------------------------
// Same section layout used by every first-party Switch title's message files (the header magic,
// LBL1 hash-bucket label table and TXT2 offset table are Nintendo's own LMS format, not specific
// to this game). Verified against this title's actual ActorType/QuestMsg files: encoding byte 1
// (UTF-16), little-endian. Tags: 0x0E <group u16><type u16><argsize u16><args...> (open),
// 0x0F <group u16><type u16> (close); group==0 && type==0 is ruby (dropped, base text kept, same
// as every other tag here since this package never configures an icon font -- see NOTES.md).
namespace msbt_detail {

inline uint16_t U16(const uint8_t* d, size_t o) {
    uint16_t v;
    std::memcpy(&v, d + o, 2);
    return v;
}
inline uint32_t U32(const uint8_t* d, size_t o) {
    uint32_t v;
    std::memcpy(&v, d + o, 4);
    return v;
}

/// label -> raw UTF-16 code units (tags still in place, as 0x0E/0x0F sequences).
inline std::unordered_map<std::string, std::u16string> ParseLabels(const std::vector<uint8_t>& d) {
    std::unordered_map<std::string, std::u16string> out;
    if (d.size() < 0x20 || std::memcmp(d.data(), "MsgStdBn", 8) != 0 || d[12] != 1) {
        return out; // not UTF-16 LMS, or too short: give up cleanly
    }
    const uint16_t sections = U16(d.data(), 14);
    size_t off = 0x20;
    std::unordered_map<uint32_t, std::string> labels;
    std::vector<std::u16string> texts;
    for (uint16_t s = 0; s < sections && off + 0x10 <= d.size(); ++s) {
        const uint32_t size = U32(d.data(), off + 4);
        const size_t body = off + 0x10;
        if (body + size > d.size()) {
            break;
        }
        if (std::memcmp(d.data() + off, "LBL1", 4) == 0) {
            const uint32_t buckets = U32(d.data(), body);
            for (uint32_t i = 0; i < buckets && body + 4 + size_t(i) * 8 + 8 <= body + size; ++i) {
                const uint32_t count = U32(d.data(), body + 4 + size_t(i) * 8);
                size_t p = body + U32(d.data(), body + 4 + size_t(i) * 8 + 4);
                for (uint32_t k = 0; k < count && p < body + size; ++k) {
                    const uint8_t len = d[p];
                    if (p + 1 + size_t(len) + 4 > body + size) {
                        break;
                    }
                    std::string name(reinterpret_cast<const char*>(d.data() + p + 1), len);
                    labels[U32(d.data(), p + 1 + len)] = std::move(name);
                    p += 1 + len + 4;
                }
            }
        } else if (std::memcmp(d.data() + off, "TXT2", 4) == 0) {
            const uint32_t n = U32(d.data(), body);
            if (4 + size_t(n) * 4 <= size) {
                for (uint32_t i = 0; i < n; ++i) {
                    const size_t start = body + U32(d.data(), body + 4 + size_t(i) * 4);
                    const size_t end =
                        i + 1 < n ? body + U32(d.data(), body + 4 + size_t(i + 1) * 4) : body + size;
                    std::u16string t;
                    for (size_t p = start; p + 2 <= end && p + 2 <= d.size(); p += 2) {
                        t += static_cast<char16_t>(U16(d.data(), p));
                    }
                    texts.push_back(std::move(t));
                }
            }
        }
        off = body + ((size + 15) & ~size_t{15});
    }
    for (auto& [index, name] : labels) {
        if (index < texts.size()) {
            out.emplace(std::move(name), texts[index]);
        }
    }
    return out;
}

inline void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// Tags stripped (ruby's reading dropped, its base text kept as plain text -- same behaviour as
/// the runtime's own Msbt::Parse when no icon font is configured, which is this package's case).
inline std::string ToUtf8(std::u16string_view s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const char16_t u = s[i];
        if (u == 0) {
            break;
        }
        if (u == 0x0E && i + 3 < s.size()) {
            const size_t units = (size_t(s[i + 3]) + 1) / 2;
            i += 4 + units;
            continue;
        }
        if (u == 0x0F && i + 2 < s.size()) {
            i += 3;
            continue;
        }
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            AppendUtf8(out, 0x10000 + ((char32_t(u) - 0xD800) << 10) + (char32_t(s[i + 1]) - 0xDC00));
            i += 2;
            continue;
        }
        AppendUtf8(out, (u >= 0xD800 && u < 0xE000) ? 0xFFFD : u);
        ++i;
    }
    return out;
}

} // namespace msbt_detail

// ---- reflow rule (item 11 / F6) -----------------------------------------------------------
namespace text_reflow {

inline bool IsCjk(char32_t c) {
    return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xFF00 && c <= 0xFFEF);
}
inline bool IsCloser(char32_t c) {
    switch (c) {
    case '"':
    case '\'':
    case 0x201D:
    case 0x2019:
    case ')':
    case 0x3009:
    case 0x300B:
    case 0x300F:
    case ']':
        return true;
    default:
        return false;
    }
}
inline bool IsTerminator(char32_t c) {
    switch (c) {
    case '.':
    case '!':
    case '?':
    case ':':
    case 0x3002: // .
    case 0xFF01: // !
    case 0xFF1F: // ?
    case 0xFF1A: // :
        return true;
    default:
        return false;
    }
}
inline bool IsHyphenOrDash(char32_t c) {
    switch (c) {
    case '-':
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
        return true;
    default:
        return false;
    }
}

/// Decodes the last UTF-8 code point of `s` ("" -> 0).
inline char32_t LastCodepoint(std::string_view s) {
    size_t i = s.size();
    if (i == 0) {
        return 0;
    }
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
        --i;
    }
    --i;
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        return c0;
    }
    const int n = c0 >= 0xF0 ? 3 : c0 >= 0xE0 ? 2 : c0 >= 0xC0 ? 1 : -1;
    if (n < 0 || i + n >= s.size()) {
        return 0xFFFD;
    }
    char32_t cp = c0 & (0x3F >> n);
    for (int k = 1; k <= n; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    return cp;
}
/// First UTF-8 code point of `s` ("" -> 0).
inline char32_t FirstCodepoint(std::string_view s) {
    if (s.empty()) {
        return 0;
    }
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80) {
        return c0;
    }
    const int n = c0 >= 0xF0 ? 3 : c0 >= 0xE0 ? 2 : c0 >= 0xC0 ? 1 : -1;
    if (n < 0 || size_t(n) >= s.size()) {
        return 0xFFFD;
    }
    char32_t cp = c0 & (0x3F >> n);
    for (int k = 1; k <= n; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[k]) & 0x3F);
    }
    return cp;
}

inline std::string_view RstripView(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// True if `line`'s last visible character ends a sentence: a terminator, or one/more closing
/// quotes/brackets preceded by a terminator.
inline bool EndsSentence(std::string_view line) {
    std::string_view s = RstripView(line);
    while (!s.empty() && IsCloser(LastCodepoint(s))) {
        const char32_t c = LastCodepoint(s);
        size_t drop = 1;
        if (c >= 0x80) {
            drop = (c >= 0x800) ? 3 : 2; // every closer used here is 1-3 UTF-8 bytes
        }
        s.remove_suffix(std::min(drop, s.size()));
    }
    return !s.empty() && IsTerminator(LastCodepoint(s));
}

inline bool StartsListOrBullet(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size()) {
        return false;
    }
    const char32_t c = FirstCodepoint(line.substr(i));
    if (c == '-' || c == '*' || c == 0x2022 || c == 0x25CF || c == 0x2023 || c == 0x2043 ||
        c == 0x2460 || c == 0x2776) {
        return true;
    }
    if (c >= '0' && c <= '9') {
        size_t j = i;
        while (j < line.size() && line[j] >= '0' && line[j] <= '9') {
            ++j;
        }
        return j < line.size() && (line[j] == '.' || line[j] == ')');
    }
    return false;
}

/// Number of UTF-8 code points in `s` (the "visible length" the short-line check compares
/// against the wrap-width threshold -- a CJK character counts as one, like a Latin one, even
/// though it is visually wider; see NOTES.md "visible length").
inline size_t CodepointCount(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        i += c0 < 0x80 ? 1 : c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
        ++n;
    }
    return n;
}

/// A banner/separator line: some banner character (one of "/\\=*_~#", never a prose character
/// like '.', ',' or '-') runs 3+ times in a row (e.g. "/////", "====") -- coordinator refinement
/// 2026-10-18, rule (d). Deliberately excludes '.'/'-' so an ellipsis or an em-dash run is never
/// mistaken for a banner (verified against the real sweep: see NOTES.md).
inline bool IsDividerLine(std::string_view line) {
    constexpr std::string_view DividerChars = "/\\=*_~#";
    // ASCII-only trim (a banner run of '/','=','*','_','~','#' is always ASCII either side of
    // any leading ideographic/tab whitespace -- the scan below only needs the banner run
    // itself, not exact trimmed bounds, so untrimmed leading bytes before it are harmless).
    for (size_t i = 0; i + 2 < line.size(); ++i) {
        if (line[i] == line[i + 1] && line[i] == line[i + 2] &&
            DividerChars.find(line[i]) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

/// A line fully wrapped in one bracket pair, e.g. "[Collaboration: ...]" -- rule (d).
inline bool IsBracketedLine(std::string_view line) {
    std::string_view s = RstripView(line);
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) {
        ++b;
    }
    s = s.substr(b);
    if (s.size() < 2) {
        return false;
    }
    return (s.front() == '[' && s.back() == ']') || (s.front() == '(' && s.back() == ')');
}

inline bool IsIsolatedLine(std::string_view line) {
    return IsDividerLine(line) || IsBracketedLine(line);
}

/// Coordinator refinement 2026-10-18 (replaces the original "previous line ends a sentence"
/// keep-always rule): fraction of `wrap_width` (the alias's 95th-percentile line length, from
/// botw_description_table.h) under which a sentence-ending line counts as SHORT -- a full-width
/// line that happens to end in a period is still almost always a wrap artifact, not a real
/// paragraph end (see NOTES.md sample 1, "Animal_Insect_I_Desc").
constexpr double ShortLineFraction = 0.7;

/// Item 11 / F6, as refined by the coordinator 2026-10-18: joins a hard '\n' with one space
/// unless it is a real paragraph break -- (a) a blank line, (b) the next line starting a
/// list/bullet (including "-X" with no leading space), (c) the previous line ending a sentence
/// AND being SHORT (< ShortLineFraction * wrap_width), or (d) the break sitting next to a
/// banner/divider line or a fully-bracketed credit line, which stays isolated either way.
/// Otherwise joined, with no inserted space across a hyphen/dash break or between two CJK
/// characters (no inter-word spaces in that script). `wrap_width` <= 0 (alias unknown, or every
/// one of its table entries is single-line text) disables the short-line check (rule (c) never
/// fires; (a)/(b)/(d) are unaffected) rather than guessing a width. Pure function: independently
/// unit-tested, and cross-validated against an independent Python implementation for all 1621
/// real description refs (see NOTES.md "cross-validation").
inline std::string Reflow(std::string_view text, int32_t wrap_width = 0) {
    if (text.find('\n') == std::string_view::npos) {
        return std::string(text);
    }
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (true) {
        const size_t nl = text.find('\n', start);
        lines.push_back(text.substr(start, nl == std::string_view::npos ? std::string_view::npos
                                                                         : nl - start));
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    const double threshold =
        wrap_width > 0 ? ShortLineFraction * double(wrap_width)
                       : std::numeric_limits<double>::infinity();
    std::string out(lines[0]);
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string_view prev = lines[i - 1];
        const std::string_view cur = lines[i];
        const bool blank = RstripView(prev).empty() || RstripView(cur).empty();
        const bool keep_short_sentence =
            EndsSentence(prev) && double(CodepointCount(RstripView(prev))) < threshold;
        if (blank || StartsListOrBullet(cur) || IsIsolatedLine(prev) || IsIsolatedLine(cur) ||
            keep_short_sentence) {
            out += '\n';
            out += cur;
            continue;
        }
        const char32_t left = LastCodepoint(out);
        const char32_t right = FirstCodepoint(cur);
        if (!IsHyphenOrDash(left) && !IsCjk(left) && !IsCjk(right)) {
            out += ' ';
        }
        out += cur;
    }
    return out;
}

} // namespace text_reflow

// ---- item 4 (iteration 9, 2026-10-19): non-English / placeholder description fallback ------
// RE/data evidence (NOTES.md "item 4"): the shipped 1.6.0 USen Bootup_USen.pack's
// ActorType/PlayerItem.msbt carries THREE labels per rune actor -- "_Name", "_Desc" and
// "_MenuDesc". Dumping every label in that file (not guessing) shows "_Desc" is, for most
// runes, either raw untranslated Japanese dev text (Obj_IceMaker_Desc, Obj_AmiiboItem_Desc,
// Obj_Magnetglove_Desc, Obj_StopTimer_Desc, Obj_StopTimerLv2_Desc -- exactly the "surprise
// finding" iteration 8's sweep flagged as "unused/debug", which was wrong: these ARE the live
// rune description keys this companion's quick/held rune pickers already bind, via the same
// description table as every other item) or empty (Obj_RemoteBombBall_Desc,
// Obj_RemoteBombCube_Desc, Obj_Camera_Desc), while "_MenuDesc" on the SAME actor is the text
// the native Rune/Sheikah Slate menu actually ships in clean English for most of them
// (Obj_IceMaker_MenuDesc, Obj_AmiiboItem_MenuDesc, Obj_RemoteBombBall_MenuDesc,
// Obj_RemoteBombCube_MenuDesc, Obj_StopTimer_MenuDesc). A "+"-upgrade actor (Obj_
// RemoteBombBall2/Cube2, Obj_StopTimerLv2) has no clean text under ITS OWN "_Desc"/"_MenuDesc"
// either -- the base actor's own "_MenuDesc" (strip the trailing "2"/"Lv2") is the closest
// real, non-fabricated English source and is used instead (the "+" upgrade only changes power/
// reload, not what the rune does, so reusing the base flavour text is the best available
// answer, not a guess: it is the base actor's own shipped sentence, read from the same file).
// Obj_Magnetglove (Magnesis) has NO clean English text anywhere in the file under any of these
// four candidates (every one of them is the identical untranslated Japanese sentence) -- this
// stays "" (fails soft, same policy as an unknown hash/path) rather than fabricate a sentence
// Nintendo never shipped in English. This fallback is applied to EVERY description lookup (not
// just PlayerItem/runes), so it also doubles as the brief's "sweep all description sources"
// safety net: any other "_Desc" label that ever turns out empty/CJK-containing (e.g. the
// pre-existing quest_GanonQuest#QL_GanonQuest_Finished dev note iteration 8 flagged, unreached
// in normal play) now shows blank instead of the wrong-language text, never changes a single
// one of the other ~1,615 already-clean refs (the primary label is tried first and accepted
// immediately whenever it is already non-empty, CJK-free text).
namespace text_reflow {
/// True if `utf8` contains at least one CJK code point (Hiragana/Katakana/CJK ideographs/
/// Hangul/fullwidth forms -- the same ranges IsCjk() above already classifies).
inline bool HasCjk(std::string_view utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c0 = static_cast<unsigned char>(utf8[i]);
        const size_t n = c0 < 0x80 ? 1 : c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
        if (n >= 2 && i + n <= utf8.size()) {
            char32_t cp = c0 & (0x3F >> (n - 1));
            for (size_t k = 1; k < n; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
            }
            if (IsCjk(cp)) {
                return true;
            }
        }
        i += n;
    }
    return false;
}
/// "Obj_RemoteBombBall2" -> "Obj_RemoteBombBall", "Obj_StopTimerLv2" -> "Obj_StopTimer"; "" if
/// `item` carries neither upgrade suffix.
inline std::string StripUpgradeSuffix(std::string_view item) {
    if (item.size() > 3 && item.substr(item.size() - 3) == "Lv2") {
        return std::string(item.substr(0, item.size() - 3));
    }
    if (item.size() > 1 && item.back() == '2' &&
        (item[item.size() - 2] < '0' || item[item.size() - 2] > '9')) {
        return std::string(item.substr(0, item.size() - 1));
    }
    return std::string();
}
} // namespace text_reflow

/// Every alias used by the description table is either "botw_<X>" (ActorType/<X>.msbt) or
/// "quest_<Name>" (QuestMsg/QL_<Name>.msbt) -- verified against the full, current description
/// table (see NOTES.md "path derivation"); both resolve under the one USen message archive this
/// package already hard-codes for every alias (prototype/dualscreen/manifest.json's "msbt" dict
/// has no language table at all -- this companion has always shown USen text regardless of the
/// player's own language setting). Returns "" for an alias outside those two shapes so callers
/// fail soft onto the existing text_map rendering instead of guessing a wrong path.
inline std::string BuildMsbtPath(std::string_view alias) {
    constexpr std::string_view Archive =
        "romfs:/Pack/Bootup_USen.pack#Message/Msg_USen.product.ssarc#";
    if (alias.starts_with("botw_")) {
        return std::string(Archive) + "ActorType/" + std::string(alias.substr(5)) + ".msbt";
    }
    if (alias.starts_with("quest_")) {
        return std::string(Archive) + "QuestMsg/QL_" + std::string(alias.substr(6)) + ".msbt";
    }
    // Iteration 9 integrator fix (item 4, 2026-10-19): the Sheikah Slate rune-select screen's
    // OWN layout file (LayoutMsg/AppTool_00.msbt) -- confirmed by dumping every non-empty label
    // in it (not guessing): labels 0000/0001=Cryonis short/long, 0003/0004=Camera short/long,
    // 0006/0007=amiibo, 0009/0010=Magnesis short/long, 0012/0013=Remote Bomb, 0014/0015=Remote
    // Bomb+, 0018/0019=Stasis, 0020/0021=Stasis+, 0024/0025=Master Cycle Zero -- clean English
    // for every one of them, including Camera and Magnesis, which ActorType/PlayerItem.msbt
    // (the alias every rune's description_table entry actually points at) has NO clean text for
    // under any of _Desc/_MenuDesc/base-actor fallback (see the Lookup() fallback chain below).
    // This IS the native text the Rune menu shows (it is literally the rune-select UI's own
    // message file), just sourced from a different msbt than the per-item Desc fields.
    if (alias.starts_with("layout_")) {
        return std::string(Archive) + "LayoutMsg/" + std::string(alias.substr(7)) + ".msbt";
    }
    return std::string();
}

/// Iteration 9 integrator fix (item 4): the rune-select screen's own long-form label (AppTool_00,
/// see BuildMsbtPath above) for the handful of PlayerItem actors that have NO clean English text
/// anywhere under their own _Desc/_MenuDesc chain. Returns "" for any other actor (the existing
/// chain already resolves those; this is a last-resort, not a general replacement).
inline std::string_view AppToolFallbackLabel(std::string_view item) {
    if (item == "Obj_Camera") return "0004";
    if (item == "Obj_Magnetglove") return "0010";
    return "";
}

// ---- fetch + decode + reflow, cached -------------------------------------------------------
/// One module-wide cache: path -> parsed labels (raw UTF-16, tags kept), and hash -> the final
/// reflowed UTF-8 string (computed once; game text is static for the session).
class DescriptionCache {
public:
    /// Same calling convention as EdenDsmodHostApi::read_romfs (mod_module_host.cpp already
    /// resolves the full "<pack>#<sarc>#<member>" compound path transparently, same as image/
    /// font reads): size-only probe with output==nullptr, then a real read.
    using ReadRomfs = size_t (*)(void* userdata, const char* path, uint64_t offset, void* output,
                                  size_t size);

    /// hash: the SAME identity hash (actor_key()/quest key()) the caller already publishes for
    /// selection. Returns "" when unknown (hash not in the generated table) or the file/label
    /// could not be read -- callers keep their existing text_map/text_bind widget as a fallback
    /// in that case (see NOTES.md "fallback").
    const std::string& Lookup(ReadRomfs read_romfs, void* userdata, uint64_t hash) {
        static const std::string empty;
        const auto it = by_hash_.find(hash);
        if (it != by_hash_.end()) {
            return it->second;
        }
        const DescriptionRef* ref = description_ref(hash);
        if (ref == nullptr) {
            return empty;
        }
        const std::string path = BuildMsbtPath(ref->alias);
        if (path.empty()) {
            return empty;
        }
        const auto& labels = Labels(read_romfs, userdata, path);
        const std::string label(ref->label);
        const int32_t wrap_width = alias_wrap_width(ref->alias);
        std::string cleaned;
        // Item 4 fallback chain (see the block comment above): accept the primary label only
        // when its decoded text is non-empty and CJK-free; otherwise, for a "_Desc" label, try
        // its sibling "_MenuDesc", then the base actor's "_Desc"/"_MenuDesc" (upgrade suffix
        // stripped). Never shows CJK/placeholder text -- an all-candidates-bad lookup fails
        // soft to "" exactly like an unknown hash/path already does.
        auto try_label = [&](const std::string& lbl) {
            const auto it = labels.find(lbl);
            if (it == labels.end()) {
                return false;
            }
            const std::string utf8 = msbt_detail::ToUtf8(it->second);
            if (utf8.empty() || text_reflow::HasCjk(utf8)) {
                return false;
            }
            cleaned = text_reflow::Reflow(utf8, wrap_width);
            return true;
        };
        if (!try_label(label) && label.size() > 5 && label.substr(label.size() - 5) == "_Desc") {
            const std::string item = label.substr(0, label.size() - 5);
            if (!try_label(item + "_MenuDesc")) {
                const std::string base = text_reflow::StripUpgradeSuffix(item);
                bool ok = !base.empty() && (try_label(base + "_Desc") || try_label(base + "_MenuDesc"));
                if (!ok) {
                    // Last resort (item 4 integrator fix): the rune-select screen's own message
                    // file, for the few actors with no clean text anywhere under their own alias.
                    const std::string_view fallback_label = AppToolFallbackLabel(item);
                    if (!fallback_label.empty()) {
                        const auto& tool_labels = Labels(read_romfs, userdata, BuildMsbtPath("layout_AppTool_00"));
                        const auto it2 = tool_labels.find(std::string(fallback_label));
                        if (it2 != tool_labels.end()) {
                            const std::string utf8 = msbt_detail::ToUtf8(it2->second);
                            if (!utf8.empty() && !text_reflow::HasCjk(utf8)) {
                                cleaned = text_reflow::Reflow(utf8, 0);
                            }
                        }
                    }
                }
            }
        }
        return by_hash_.emplace(hash, std::move(cleaned)).first->second;
    }

private:
    const std::unordered_map<std::string, std::u16string>&
    Labels(ReadRomfs read_romfs, void* userdata, const std::string& path) {
        auto it = by_path_.find(path);
        if (it != by_path_.end()) {
            return it->second;
        }
        std::vector<uint8_t> bytes;
        if (read_romfs != nullptr) {
            const size_t size = read_romfs(userdata, path.c_str(), 0, nullptr, 0);
            if (size > 0 && size < (64u << 20)) { // sanity cap: no message file is this large
                bytes.resize(size);
                const size_t got = read_romfs(userdata, path.c_str(), 0, bytes.data(), size);
                bytes.resize(got);
            }
        }
        auto parsed = bytes.empty() ? std::unordered_map<std::string, std::u16string>{}
                                     : msbt_detail::ParseLabels(bytes);
        return by_path_.emplace(path, std::move(parsed)).first->second;
    }

    std::unordered_map<std::string, std::unordered_map<std::string, std::u16string>> by_path_;
    std::unordered_map<uint64_t, std::string> by_hash_;
};

} // namespace botw
