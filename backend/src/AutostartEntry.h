/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#pragma once

#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief What a user put in front of the program on the Exec= line of the
 * Linux login item, read back so a rewrite keeps it (Autostart.cpp).
 *
 * The app rewrites its XDG autostart entry at every start when the entry says
 * something else than this version would write (refreshLoginItem). An entry a
 * user had changed to start the app as `env NAME=value … "<program>" …` — the
 * way a host is kept off the Internet with MW_LAN_ONLY=1 — lost that prefix at
 * the first start, and the login after it ran without. The prefix is kept,
 * exactly as written; everything after it is still this version's to write.
 *
 * Pure text, no file and no platform: tested everywhere, used on Linux only.
 */
namespace Autostart::entry {

/// The value of the Exec= key in a desktop entry's text — the first such line,
/// the only group the app writes being [Desktop Entry]. Empty when none.
inline QString execValue(const QString& text)
{
    for (QString line : text.split(QLatin1Char('\n'))) {
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.startsWith(QLatin1String("Exec="))) return line.mid(5);
    }
    return {};
}

namespace detail {

/// One argument of an Exec= value: what it says once both escaping levels are
/// undone, and where it sits in the value as written.
struct Argument
{
    QString value;
    qsizetype begin = 0; ///< first character, in the value as written
};

/// The arguments of an Exec= value, per the Desktop Entry spec: the string
/// escapes (\s \n \t \r \\) first, then double quotes, inside which \" \` \$
/// and \\ stand for the character. False on a value the spec does not allow
/// (an open quote, a quote inside a word, text glued to a closing quote).
inline bool splitExec(const QString& exec, QList<Argument>& out)
{
    // The string level, each character with the place it came from.
    struct Unit
    {
        QChar c;
        qsizetype at;
    };
    QList<Unit> units;
    for (qsizetype i = 0; i < exec.size(); ++i) {
        if (exec[i] == QLatin1Char('\\') && i + 1 < exec.size()) {
            QChar c;
            switch (exec[i + 1].unicode()) {
            case 's': c = QLatin1Char(' '); break;
            case 'n': c = QLatin1Char('\n'); break;
            case 't': c = QLatin1Char('\t'); break;
            case 'r': c = QLatin1Char('\r'); break;
            case '\\': c = QLatin1Char('\\'); break;
            default: break;
            }
            if (!c.isNull()) {
                units.append(Unit{c, i});
                ++i;
                continue;
            }
        }
        units.append(Unit{exec[i], i});
    }

    // The quoting level.
    out.clear();
    qsizetype i = 0;
    while (i < units.size()) {
        if (units[i].c == QLatin1Char(' ')) {
            ++i;
            continue;
        }
        Argument arg;
        arg.begin = units[i].at;
        if (units[i].c == QLatin1Char('"')) {
            ++i;
            bool closed = false;
            while (i < units.size()) {
                const QChar c = units[i].c;
                if (c == QLatin1Char('\\') && i + 1 < units.size()) {
                    const QChar next = units[i + 1].c;
                    if (next == QLatin1Char('"') || next == QLatin1Char('`') ||
                        next == QLatin1Char('$') || next == QLatin1Char('\\')) {
                        arg.value += next;
                        i += 2;
                        continue;
                    }
                }
                if (c == QLatin1Char('"')) {
                    closed = true;
                    ++i;
                    break;
                }
                arg.value += c;
                ++i;
            }
            if (!closed) return false;
            if (i < units.size() && units[i].c != QLatin1Char(' ')) return false;
        } else {
            while (i < units.size() && units[i].c != QLatin1Char(' ')) {
                if (units[i].c == QLatin1Char('"')) return false;
                arg.value += units[i].c;
                ++i;
            }
        }
        out.append(arg);
    }
    return true;
}

/// NAME=value, NAME being what a shell takes for a variable name.
inline bool isAssignment(const QString& arg)
{
    const qsizetype eq = arg.indexOf(QLatin1Char('='));
    if (eq < 1) return false;
    for (qsizetype i = 0; i < eq; ++i) {
        const QChar c = arg[i];
        const bool letter = (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                            (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
                            c == QLatin1Char('_');
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        if (!letter && !(digit && i > 0)) return false;
    }
    return true;
}

} // namespace detail

/// The `env NAME=value …` in front of the program on an Exec= value, exactly
/// as written and with the spaces before the program, so it can be put back in
/// front of what this version writes. Empty unless the value starts with env
/// itself (env, /usr/bin/env or /bin/env — not a program of the same name
/// elsewhere), then one NAME=value or more, then a program. env's own options
/// (-u, -i, -S …) are not read: such a line is rewritten as before.
inline QString envPrefix(const QString& exec)
{
    QList<detail::Argument> args;
    if (!detail::splitExec(exec, args) || args.size() < 3) return {};
    const QString& head = args.front().value;
    if (head != QLatin1String("env") && head != QLatin1String("/usr/bin/env") &&
        head != QLatin1String("/bin/env"))
        return {};
    qsizetype program = 1;
    while (program < args.size() && detail::isAssignment(args[program].value))
        ++program;
    if (program == 1 || program == args.size()) return {};
    return exec.left(args[program].begin);
}

} // namespace Autostart::entry
