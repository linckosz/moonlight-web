/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "XkbTextMap.h"

#include "../../core/Log.h"

#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <linux/input-event-codes.h>
#include <sstream>

namespace mw::native::input {
namespace {

/// The soname, for the reason X11Pointer gives: libxkbcommon.so is the -dev
/// symlink, .so.0 is what every desktop has. Wayland compositors, GTK and Qt all
/// link it, so on a machine with a graphical session it is already resident.
constexpr const char* kSoname = "libxkbcommon.so.0";

/// XKB's own types, spelled out so no xkbcommon header is needed to declare
/// them. All six are uint32_t in the public API and have been since 0.1.
using XkbContext = void;
using XkbKeymap = void;
using XkbKeycode = uint32_t;
using XkbKeysym = uint32_t;
using XkbIndex = uint32_t; ///< layout, level and modifier indices alike
using XkbModMask = uint32_t;

constexpr XkbIndex kModInvalid = 0xFFFFFFFFu; // XKB_MOD_INVALID

/// The five names, laid out exactly as struct xkb_rule_names.
struct RuleNames
{
    const char* rules;
    const char* model;
    const char* layout;
    const char* variant;
    const char* options;
};

struct Api
{
    XkbContext* (*contextNew)(int) = nullptr;
    void (*contextUnref)(XkbContext*) = nullptr;
    XkbKeymap* (*keymapFromNames)(XkbContext*, const RuleNames*, int) = nullptr;
    void (*keymapUnref)(XkbKeymap*) = nullptr;
    XkbKeycode (*minKeyCode)(XkbKeymap*) = nullptr;
    XkbKeycode (*maxKeyCode)(XkbKeymap*) = nullptr;
    XkbIndex (*numLevels)(XkbKeymap*, XkbKeycode, XkbIndex) = nullptr;
    int (*symsByLevel)(XkbKeymap*, XkbKeycode, XkbIndex, XkbIndex, const XkbKeysym**) = nullptr;
    XkbIndex (*modGetIndex)(XkbKeymap*, const char*) = nullptr;
    uint32_t (*keysymToUtf32)(XkbKeysym) = nullptr;
    /// Optional: added in libxkbcommon 1.0. Without it the level convention
    /// below is used instead, which is right for every ordinary four-level
    /// layout and is what this would have had to assume anyway.
    size_t (*modsForLevel)(XkbKeymap*, XkbKeycode, XkbIndex, XkbIndex, XkbModMask*,
                           size_t) = nullptr;
};

/// Which modifiers we are willing to hold down to reach a level.
///
/// Shift and Mod5 (AltGr, the third level on every European layout) and nothing
/// else. Lock is refused on purpose: reaching a capital by toggling CapsLock
/// would leave the host's keyboard in a state the viewer never asked for and
/// cannot see. Control and Mod1 have no business producing a printable
/// character, and a layout that says otherwise is one we decline to guess at.
struct ModIndices
{
    XkbIndex shift = kModInvalid;
    XkbIndex mod5 = kModInvalid;
};

bool hasMod(XkbModMask mask, XkbIndex index)
{
    return index != kModInvalid && index < 32 && (mask & (1u << index)) != 0;
}

/// A modifier mask as keys to press, or false when it asks for one we refuse.
bool translateMask(XkbModMask mask, const ModIndices& mods, XkbStroke& stroke, int& count)
{
    count = 0;
    XkbModMask left = mask;
    if (hasMod(mask, mods.shift)) {
        stroke.mods[count++] = KEY_LEFTSHIFT;
        left &= ~(1u << mods.shift);
    }
    if (hasMod(mask, mods.mod5)) {
        if (count >= 2) return false;
        stroke.mods[count++] = KEY_RIGHTALT;
        left &= ~(1u << mods.mod5);
    }
    // Anything still standing is a modifier we will not press.
    return left == 0;
}

/// The keys to hold for @p level of @p keycode, by asking the keymap when it can
/// answer and by the four-level convention when it cannot.
bool strokeForLevel(const Api& api, XkbKeymap* keymap, XkbKeycode keycode, XkbIndex level,
                    const ModIndices& mods, XkbStroke& stroke)
{
    stroke.mods[0] = 0;
    stroke.mods[1] = 0;

    if (api.modsForLevel) {
        XkbModMask masks[4] = {0, 0, 0, 0};
        const size_t got = api.modsForLevel(keymap, keycode, 0, level, masks, 4);
        // Several masks can reach the same level (Shift and Shift+NumLock, say).
        // The cheapest wins: fewer keys held is fewer ways to disturb the host.
        int best = -1;
        XkbStroke candidate;
        for (size_t i = 0; i < got; ++i) {
            int count = 0;
            XkbStroke attempt;
            attempt.code = stroke.code;
            if (!translateMask(masks[i], mods, attempt, count)) continue;
            if (best < 0 || count < best) {
                best = count;
                candidate = attempt;
            }
            if (best == 0) break;
        }
        if (best < 0) return false;
        stroke.mods[0] = candidate.mods[0];
        stroke.mods[1] = candidate.mods[1];
        return true;
    }

    switch (level) {
    case 0: return true;
    case 1: stroke.mods[0] = KEY_LEFTSHIFT; return true;
    case 2: stroke.mods[0] = KEY_RIGHTALT; return true;
    case 3:
        stroke.mods[0] = KEY_LEFTSHIFT;
        stroke.mods[1] = KEY_RIGHTALT;
        return true;
    default: return false;
    }
}

/// The dead keysyms we know how to use. XKB numbers them from 0xFE50 and the
/// five here are the ones a Latin layout actually carries; a dead key we do not
/// name is simply never used, which costs the characters that needed it and
/// nothing else.
enum : uint32_t
{
    kDeadGrave = 0xFE50,
    kDeadAcute = 0xFE51,
    kDeadCircumflex = 0xFE52,
    kDeadTilde = 0xFE53,
    kDeadDiaeresis = 0xFE57,
    kDeadCedilla = 0xFE5B,
};

/// An accented letter as the dead key and the plain letter it is built from.
///
/// Latin-1 and the handful past it that French, Spanish, German and Portuguese
/// need. Full Unicode decomposition would be a data table an order of magnitude
/// larger for characters no soft keyboard sends to a desktop; what matters is
/// that a viewer typing `ê` on a phone gets `ê` on a French host, where the
/// keyboard has no key for it and never did — a person types it the same way.
struct Composed
{
    char32_t precomposed;
    uint32_t dead;
    char32_t base;
};

constexpr Composed kComposed[] = {
    {U'À', kDeadGrave, U'A'},      {U'Á', kDeadAcute, U'A'},      {U'Â', kDeadCircumflex, U'A'},
    {U'Ã', kDeadTilde, U'A'},      {U'Ä', kDeadDiaeresis, U'A'},  {U'Ç', kDeadCedilla, U'C'},
    {U'È', kDeadGrave, U'E'},      {U'É', kDeadAcute, U'E'},      {U'Ê', kDeadCircumflex, U'E'},
    {U'Ë', kDeadDiaeresis, U'E'},  {U'Ì', kDeadGrave, U'I'},      {U'Í', kDeadAcute, U'I'},
    {U'Î', kDeadCircumflex, U'I'}, {U'Ï', kDeadDiaeresis, U'I'},  {U'Ñ', kDeadTilde, U'N'},
    {U'Ò', kDeadGrave, U'O'},      {U'Ó', kDeadAcute, U'O'},      {U'Ô', kDeadCircumflex, U'O'},
    {U'Õ', kDeadTilde, U'O'},      {U'Ö', kDeadDiaeresis, U'O'},  {U'Ù', kDeadGrave, U'U'},
    {U'Ú', kDeadAcute, U'U'},      {U'Û', kDeadCircumflex, U'U'}, {U'Ü', kDeadDiaeresis, U'U'},
    {U'Ý', kDeadAcute, U'Y'},      {U'Ÿ', kDeadDiaeresis, U'Y'},  {U'à', kDeadGrave, U'a'},
    {U'á', kDeadAcute, U'a'},      {U'â', kDeadCircumflex, U'a'}, {U'ã', kDeadTilde, U'a'},
    {U'ä', kDeadDiaeresis, U'a'},  {U'ç', kDeadCedilla, U'c'},    {U'è', kDeadGrave, U'e'},
    {U'é', kDeadAcute, U'e'},      {U'ê', kDeadCircumflex, U'e'}, {U'ë', kDeadDiaeresis, U'e'},
    {U'ì', kDeadGrave, U'i'},      {U'í', kDeadAcute, U'i'},      {U'î', kDeadCircumflex, U'i'},
    {U'ï', kDeadDiaeresis, U'i'},  {U'ñ', kDeadTilde, U'n'},      {U'ò', kDeadGrave, U'o'},
    {U'ó', kDeadAcute, U'o'},      {U'ô', kDeadCircumflex, U'o'}, {U'õ', kDeadTilde, U'o'},
    {U'ö', kDeadDiaeresis, U'o'},  {U'ù', kDeadGrave, U'u'},      {U'ú', kDeadAcute, U'u'},
    {U'û', kDeadCircumflex, U'u'}, {U'ü', kDeadDiaeresis, U'u'},  {U'ý', kDeadAcute, U'y'},
    {U'ÿ', kDeadDiaeresis, U'y'},
};

std::string envOrEmpty(const char* name)
{
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

/// rules/model/layout/variant/options for THIS host.
///
/// The environment first — a session that sets XKB_DEFAULT_LAYOUT has said what
/// it wants — then /etc/default/keyboard, then nothing at all, which leaves
/// libxkbcommon to its own default. The names are passed explicitly rather than
/// letting libxkbcommon read the environment itself: it reads it through
/// secure_getenv, and this process may be running with an ambient capability,
/// where glibc hands back nothing.
XkbNames resolveNames(std::string& source)
{
    XkbNames names;
    names.rules = envOrEmpty("XKB_DEFAULT_RULES");
    names.model = envOrEmpty("XKB_DEFAULT_MODEL");
    names.layout = envOrEmpty("XKB_DEFAULT_LAYOUT");
    names.variant = envOrEmpty("XKB_DEFAULT_VARIANT");
    names.options = envOrEmpty("XKB_DEFAULT_OPTIONS");
    if (!names.layout.empty()) {
        source = "XKB_DEFAULT_LAYOUT";
        return names;
    }

    std::ifstream file("/etc/default/keyboard");
    if (file) {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        XkbNames fromFile;
        if (parseKeyboardConfig(buffer.str(), fromFile)) {
            source = "/etc/default/keyboard";
            return fromFile;
        }
    }

    source = "libxkbcommon default";
    return names;
}

const char* orNull(const std::string& value)
{
    return value.empty() ? nullptr : value.c_str();
}

} // namespace

XkbTextMap::~XkbTextMap()
{
    close();
}

bool XkbTextMap::open()
{
    if (isOpen()) return true;

    // RTLD_LOCAL so nothing else in this process picks up XKB symbols by
    // accident, RTLD_NOW so a mismatched library fails here, once, rather than
    // at the first keystroke.
    if (!m_Lib) m_Lib = ::dlopen(kSoname, RTLD_NOW | RTLD_LOCAL);
    if (!m_Lib) {
        log::info(std::string("[native] input: ") + kSoname +
                  " not available — a soft keyboard's text cannot be typed on this host (keys "
                  "still work)");
        return false;
    }

    Api api;
    api.contextNew = reinterpret_cast<decltype(api.contextNew)>(::dlsym(m_Lib, "xkb_context_new"));
    api.contextUnref =
        reinterpret_cast<decltype(api.contextUnref)>(::dlsym(m_Lib, "xkb_context_unref"));
    api.keymapFromNames = reinterpret_cast<decltype(api.keymapFromNames)>(
        ::dlsym(m_Lib, "xkb_keymap_new_from_names"));
    api.keymapUnref =
        reinterpret_cast<decltype(api.keymapUnref)>(::dlsym(m_Lib, "xkb_keymap_unref"));
    // min_keycode / max_keycode, not min_key_code: the two neighbouring calls
    // are key_get_syms_by_level and num_levels_for_key, which do spell it in two
    // words, and guessing consistency here cost a build.
    api.minKeyCode =
        reinterpret_cast<decltype(api.minKeyCode)>(::dlsym(m_Lib, "xkb_keymap_min_keycode"));
    api.maxKeyCode =
        reinterpret_cast<decltype(api.maxKeyCode)>(::dlsym(m_Lib, "xkb_keymap_max_keycode"));
    api.numLevels =
        reinterpret_cast<decltype(api.numLevels)>(::dlsym(m_Lib, "xkb_keymap_num_levels_for_key"));
    api.symsByLevel = reinterpret_cast<decltype(api.symsByLevel)>(
        ::dlsym(m_Lib, "xkb_keymap_key_get_syms_by_level"));
    api.modGetIndex =
        reinterpret_cast<decltype(api.modGetIndex)>(::dlsym(m_Lib, "xkb_keymap_mod_get_index"));
    api.keysymToUtf32 =
        reinterpret_cast<decltype(api.keysymToUtf32)>(::dlsym(m_Lib, "xkb_keysym_to_utf32"));
    api.modsForLevel = reinterpret_cast<decltype(api.modsForLevel)>(
        ::dlsym(m_Lib, "xkb_keymap_key_get_mods_for_level"));

    if (!api.contextNew || !api.contextUnref || !api.keymapFromNames || !api.keymapUnref ||
        !api.minKeyCode || !api.maxKeyCode || !api.numLevels || !api.symsByLevel ||
        !api.modGetIndex || !api.keysymToUtf32) {
        log::warning(std::string("[native] input: ") + kSoname +
                     " loaded but does not export the keymap calls — text will not be typed");
        close();
        return false;
    }

    std::string source;
    const XkbNames names = resolveNames(source);
    const RuleNames rules = {orNull(names.rules), orNull(names.model), orNull(names.layout),
                             orNull(names.variant), orNull(names.options)};

    XkbContext* context = api.contextNew(0 /* XKB_CONTEXT_NO_FLAGS */);
    if (!context) {
        log::warning("[native] input: xkb_context_new failed — text will not be typed");
        close();
        return false;
    }
    XkbKeymap* keymap = api.keymapFromNames(context, &rules, 0 /* XKB_KEYMAP_COMPILE_NO_FLAGS */);
    if (!keymap) {
        api.contextUnref(context);
        log::warning(std::string("[native] input: no keymap for layout \"") +
                     (names.layout.empty() ? std::string("(default)") : names.layout) +
                     "\" — text will not be typed");
        close();
        return false;
    }

    ModIndices mods;
    mods.shift = api.modGetIndex(keymap, "Shift");
    mods.mod5 = api.modGetIndex(keymap, "Mod5");

    // Walk the keymap once, ascending. The first key able to produce a character
    // is the one kept, and its lowest level with it: that is the plain, unshifted
    // key a person would reach for, and on a keyboard where a digit lives on the
    // shifted level of the top row (AZERTY) it is still found there rather than
    // on the numeric pad, which sorts later.
    const XkbKeycode first = api.minKeyCode(keymap);
    const XkbKeycode last = api.maxKeyCode(keymap);
    for (XkbKeycode keycode = first; keycode <= last && keycode < 0x1000; ++keycode) {
        // XKB numbers keys from 8; evdev, which uinput speaks, numbers them from
        // 0. The offset is the X protocol's, and it is not going to change.
        if (keycode < 8) continue;
        const uint16_t evdev = static_cast<uint16_t>(keycode - 8);

        const XkbIndex levels = api.numLevels(keymap, keycode, 0);
        for (XkbIndex level = 0; level < levels; ++level) {
            const XkbKeysym* syms = nullptr;
            const int count = api.symsByLevel(keymap, keycode, 0, level, &syms);
            if (count <= 0 || !syms) continue;

            XkbStroke stroke;
            stroke.code = evdev;
            if (!strokeForLevel(api, keymap, keycode, level, mods, stroke)) continue;

            for (int i = 0; i < count; ++i) {
                const uint32_t cp = api.keysymToUtf32(syms[i]);
                // Zero means the keysym is not a character at all — a modifier,
                // F5, or a dead key. The dead keys are worth keeping: they carry
                // no character themselves and are the only route to the accented
                // letters this keyboard has no key for.
                if (cp == 0) {
                    if (syms[i] >= kDeadGrave && syms[i] <= kDeadCedilla)
                        m_Dead.emplace(syms[i], stroke);
                    continue;
                }
                m_Table.emplace(static_cast<char32_t>(cp), stroke);
            }
        }
    }

    api.keymapUnref(keymap);
    api.contextUnref(context);

    m_Description = (names.layout.empty() ? std::string("us") : names.layout) +
                    (names.variant.empty() ? std::string() : "+" + names.variant) + " (from " +
                    source + ")";

    if (m_Table.empty()) {
        log::warning("[native] input: the keymap for " + m_Description +
                     " types no character at all — text will not be typed");
        close();
        return false;
    }
    return true;
}

void XkbTextMap::close()
{
    m_Table.clear();
    m_Dead.clear();
    m_Description.clear();
    if (m_Lib) ::dlclose(m_Lib);
    m_Lib = nullptr;
}

bool XkbTextMap::find(char32_t cp, XkbStroke out[2], int& count) const
{
    count = 0;

    // A newline reaches us as U+000A; the key that produces one carries Return,
    // which XKB spells U+000D. The two are the same intent from a soft keyboard.
    if (cp == U'\n') cp = U'\r';

    const auto direct = m_Table.find(cp);
    if (direct != m_Table.end()) {
        out[0] = direct->second;
        count = 1;
        return true;
    }

    // No key for it: an accent this layout builds rather than carries. Both
    // halves have to be reachable — a layout with the dead key but not the base
    // letter would type half a character, which is worse than nothing.
    for (const Composed& entry : kComposed) {
        if (entry.precomposed != cp) continue;
        const auto dead = m_Dead.find(entry.dead);
        const auto base = m_Table.find(entry.base);
        if (dead == m_Dead.end() || base == m_Table.end()) return false;
        out[0] = dead->second;
        out[1] = base->second;
        count = 2;
        return true;
    }
    return false;
}

} // namespace mw::native::input
