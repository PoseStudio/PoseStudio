// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file assetscriptlink.cpp
 * @brief Implementation of the script-link reader. See assetscriptlink.h.
 *
 * Three steps: TOKENIZE the script (string literals unescaped, names, punctuation; comments and
 * whitespace dropped), EVALUATE the first argument of each recognised call as a path expression,
 * RESOLVE it against the filesystem. Nothing here executes anything: an expression this reader
 * cannot evaluate is simply not a link it follows.
 */

#include "assetscriptlink.h"

#include "hiddennames.h"
#include "figureutils.h"   // figureFileKind — the figure importer's own "is this a character?"
#include "importformats.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QStringDecoder>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace AssetScriptLink {

namespace {

/// A script bigger than this is a program, not a link; it is not read.
constexpr qint64 kMaxScriptBytes = 1 << 20;
// THE WORK IS BOUNDED, whatever the script holds. A double-click reads it on the GUI thread, and
// a script need not have been written to be a link (most are programs; one may be written to
// hang the reader). Three limits do it, none of which a real link or loader comes near:
/// How many tokens one path expression may span. A longer "expression" is not a path: it is
/// not evaluated.
constexpr int kMaxExprTokens = 4096;
/// How many terms one script may have evaluated. A variable's value may refer to other variables
/// kMaxEvalDepth deep, each with many terms: without a limit that is terms to the power of depth.
constexpr int kMaxEvalSteps = 20000;
/// How many calls and file names of one script are tried as its link before the reader gives
/// up: each one that leads nowhere costs a look under every content folder.
constexpr int kMaxTries = 64;
/// How many scripts a chain of links may pass through.
constexpr int kMaxHops = 4;
/// How deep a variable's value may refer to another variable.
constexpr int kMaxEvalDepth = 8;

bool hasSuffix(const QString& path, const char* suffix) {
    return path.endsWith(QLatin1String(suffix), Qt::CaseInsensitive);
}

// ---------------------------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------------------------

struct Token {
    enum Type { Str, Name, Punct, Other } type;
    QString text; ///< a Str's value unescaped; a Name's identifier; a Punct's one character
};

bool isNameStart(QChar c) { return c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$'); }
bool isNameChar(QChar c)  { return isNameStart(c) || c.isDigit(); }

QVector<Token> tokenize(const QString& src) {
    QVector<Token> out;
    const int n = static_cast<int>(src.size());
    int i = 0;
    while (i < n) {
        const QChar c = src.at(i);
        if (c.isSpace() || c == QChar(0xFEFF)) { // whitespace, a byte-order mark
            ++i;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && src.at(i + 1) == QLatin1Char('/')) {
            while (i < n && src.at(i) != QLatin1Char('\n')) ++i;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && src.at(i + 1) == QLatin1Char('*')) {
            const int end = static_cast<int>(src.indexOf(QLatin1String("*/"), i + 2));
            i = (end < 0) ? n : end + 2;
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            QString value;
            ++i;
            while (i < n && src.at(i) != c && src.at(i) != QLatin1Char('\n')) {
                QChar ch = src.at(i++);
                if (ch == QLatin1Char('\\') && i < n) {
                    const QChar e = src.at(i++);
                    switch (e.unicode()) {
                    case 'n': ch = QLatin1Char('\n'); break;
                    case 't': ch = QLatin1Char('\t'); break;
                    case 'r': ch = QLatin1Char('\r'); break;
                    case 'u':
                        if (i + 4 <= n) {
                            bool ok = false;
                            const ushort code = src.mid(i, 4).toUShort(&ok, 16);
                            if (ok) {
                                ch = QChar(code);
                                i += 4;
                                break;
                            }
                        }
                        ch = e;
                        break;
                    default: ch = e; break; // \\ \" \' \/ and anything else: the character itself
                    }
                }
                value.append(ch);
            }
            ++i; // the closing quote (or the line end of an unterminated literal)
            out.push_back({Token::Str, value});
            continue;
        }
        if (isNameStart(c)) {
            const int start = i;
            while (i < n && isNameChar(src.at(i))) ++i;
            out.push_back({Token::Name, src.mid(start, i - start)});
            continue;
        }
        if (c.isDigit()) {
            const int start = i;
            while (i < n && (isNameChar(src.at(i)) || src.at(i) == QLatin1Char('.'))) ++i;
            out.push_back({Token::Other, src.mid(start, i - start)});
            continue;
        }
        out.push_back({Token::Punct, QString(c)});
        ++i;
    }
    return out;
}

bool isPunct(const QVector<Token>& t, int i, char c) {
    return i >= 0 && i < t.size() && t[i].type == Token::Punct && t[i].text.at(0) == QLatin1Char(c);
}
bool isName(const QVector<Token>& t, int i, const char* name) {
    return i >= 0 && i < t.size() && t[i].type == Token::Name && t[i].text == QLatin1String(name);
}

/// The index of the bracket closing the one at @p open, or -1 when it is not closed before
/// @p limit (the end of what the caller is looking at).
int matchingClose(const QVector<Token>& t, int open, int limit) {
    int depth = 0;
    for (int i = open; i < limit && i < t.size(); ++i) {
        if (isPunct(t, i, '(') || isPunct(t, i, '[') || isPunct(t, i, '{')) ++depth;
        if (isPunct(t, i, ')') || isPunct(t, i, ']') || isPunct(t, i, '}')) {
            if (--depth == 0) return i;
        }
    }
    return -1;
}

/// The index of the bracket that the one at @p close closes, or -1 when it does not open at or
/// after @p floor: matchingClose() the other way round, in one pass from the closing bracket.
int matchingOpen(const QVector<Token>& t, int close, int floor) {
    int depth = 0;
    for (int i = close; i >= floor && i >= 0; --i) {
        if (isPunct(t, i, ')') || isPunct(t, i, ']') || isPunct(t, i, '}')) ++depth;
        if (isPunct(t, i, '(') || isPunct(t, i, '[') || isPunct(t, i, '{')) {
            if (--depth == 0) return i;
        }
    }
    return -1;
}

/// The end (exclusive) of the expression starting at @p begin: the first `,` `)` `;` or `}` at
/// bracket depth 0, a `var` beginning the next statement, or the end of the script. An
/// expression that has not ended after kMaxExprTokens is no path: @p begin itself comes back
/// (an empty expression, which evaluates to nothing).
int expressionEnd(const QVector<Token>& t, int begin) {
    int depth = 0;
    const int stop = static_cast<int>(qMin<qsizetype>(t.size(), qsizetype(begin) + kMaxExprTokens));
    for (int i = begin; i < stop; ++i) {
        if (isPunct(t, i, '(') || isPunct(t, i, '[') || isPunct(t, i, '{')) {
            ++depth;
        } else if (isPunct(t, i, ')') || isPunct(t, i, ']') || isPunct(t, i, '}')) {
            if (depth == 0) return i;
            --depth;
        } else if (depth == 0 && (isPunct(t, i, ',') || isPunct(t, i, ';') || isName(t, i, "var"))) {
            return i;
        }
    }
    return stop == t.size() ? stop : begin;
}

// ---------------------------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------------------------

/// A path expression's value. Not ok: this reader cannot say what it is. Anchored: it begins at
/// the script's own folder (its leading term was not a literal); text is what follows.
struct Value {
    bool    ok = false;
    bool    anchored = false;
    QString text;
};

class Evaluator {
public:
    explicit Evaluator(const QVector<Token>& tokens) : t(tokens) {
        // `name = <expr>` (with or without `var`), at token `at`: the expression begins at at + 2
        // and ends at expressionEnd() — looked for only when the variable is read, since a script
        // has many assignments and a link reads one or two of them. Kept per name, in the order
        // they stand in the script.
        for (int i = 0; i + 1 < t.size(); ++i) {
            // A plain `=`: not `==`, and not the second character of `<=`, `>=`, `!=`.
            if (t[i].type != Token::Name || !isPunct(t, i + 1, '=') || isPunct(t, i + 2, '=')) continue;
            assignedAt[t[i].text].push_back(i);
        }
    }

    /// The value of the expression [begin, end), variables read as last assigned before @p at.
    Value range(int begin, int end, int at, int depth = 0) const {
        Value out;
        if (begin >= end || depth > kMaxEvalDepth) return out;
        // Split into terms at the `+`s of bracket depth 0.
        QVector<QPair<int, int>> terms;
        int termBegin = begin;
        int level = 0;
        for (int i = begin; i < end; ++i) {
            if (isPunct(t, i, '(') || isPunct(t, i, '[')) ++level;
            if (isPunct(t, i, ')') || isPunct(t, i, ']')) --level;
            if (level == 0 && isPunct(t, i, '+')) {
                terms.push_back({termBegin, i});
                termBegin = i + 1;
            }
        }
        terms.push_back({termBegin, end});

        for (int k = 0; k < terms.size(); ++k) {
            const Value v = term(terms[k].first, terms[k].second, at, depth);
            if (k == 0) {
                // A leading term this reader cannot evaluate is the folder the script is in —
                // unless it is the whole expression, which is then no path at all.
                if (!v.ok && terms.size() == 1) return {};
                out.ok = true;
                out.anchored = !v.ok || v.anchored;
                out.text = v.ok ? v.text : QString();
            } else {
                if (!v.ok || v.anchored) return {}; // an unknown in the middle of a path
                out.text += v.text;
            }
        }
        return out;
    }

private:
    Value term(int begin, int end, int at, int depth) const {
        if (begin >= end || ++steps > kMaxEvalSteps) return {};
        // ( expr )
        if (isPunct(t, begin, '(') && matchingClose(t, begin, end) == end - 1) {
            return range(begin + 1, end - 1, at, depth + 1);
        }
        // String( expr ) / new String( expr )
        int s = isName(t, begin, "new") ? begin + 1 : begin;
        if (isName(t, s, "String") && isPunct(t, s + 1, '(') && matchingClose(t, s + 1, end) == end - 1) {
            return range(s + 2, end - 1, at, depth + 1);
        }
        // "...%1...".arg( expr ), the literal possibly inside String( ).
        if (end - begin >= 5 && isPunct(t, end - 1, ')')) {
            const int open = matchingOpen(t, end - 1, begin);
            if (open > begin + 1 && isPunct(t, open, '(') && isName(t, open - 1, "arg") && isPunct(t, open - 2, '.')) {
                const Value format = term(begin, open - 2, at, depth + 1);
                const Value arg = range(open + 1, expressionEnd(t, open + 1), at, depth + 1);
                if (!format.ok || format.anchored) return {};
                const int slot = static_cast<int>(format.text.indexOf(QLatin1String("%1")));
                if (slot < 0) return format;
                if (arg.ok && !arg.anchored) {
                    return {true, false, QString(format.text).replace(QLatin1String("%1"), arg.text)};
                }
                if (slot == 0) { // "%1/rest".arg(<the script's folder>)
                    return {true, true, (arg.ok ? arg.text : QString()) + format.text.mid(2)};
                }
                return {};
            }
        }
        if (end - begin != 1) return {};
        if (t[begin].type == Token::Str) return {true, false, t[begin].text};
        if (t[begin].type == Token::Name) {
            // The variable as last assigned before `at`.
            const auto places = assignedAt.constFind(t[begin].text);
            if (places != assignedAt.cend()) {
                const auto after = std::lower_bound(places->cbegin(), places->cend(), at);
                if (after != places->cbegin()) {
                    const int last = *(after - 1);
                    return range(last + 2, expressionEnd(t, last + 2), last, depth + 1);
                }
            }
        }
        return {};
    }

    const QVector<Token>& t;
    QHash<QString, QVector<int>> assignedAt; ///< variable -> the tokens it is assigned at, ascending
    mutable int steps = 0;                   ///< terms evaluated so far (kMaxEvalSteps)
};

// ---------------------------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------------------------

enum class Want { Folder, File };

bool exists(const QString& path, Want want) {
    const QFileInfo info(path);
    if (!info.exists()) return false;
    switch (want) {
    case Want::Folder: return info.isDir();
    case Want::File:   return info.isFile();
    }
    return false;
}

/// A path written in a script: native separators made `/`, surrounding blanks dropped.
QString cleanWritten(QString text) {
    return text.replace(QLatin1Char('\\'), QLatin1Char('/')).trimmed();
}

/// A real absolute path: a drive letter on Windows, a leading `/` elsewhere. A content-relative
/// "/People/…" is not one on Windows (it would name the current drive's root) — and neither is
/// a network share ("//host/share/…"): a script must not make PoseStudio probe a machine of the
/// script's choosing. Such a path is read as content-relative, and found nowhere.
bool isRealAbsolute(const QString& path) {
#ifdef Q_OS_WIN
    return path.size() >= 3 && path.at(0).isLetter() && path.at(1) == QLatin1Char(':');
#else
    return path.startsWith(QLatin1Char('/'));
#endif
}

/// Every place @p v may name, in the order they are tried. @p display gets the path the user
/// should be told about when none exists.
QStringList candidates(const Value& v, const QString& scriptDir, const QStringList& roots,
                       QString* display) {
    QStringList out;
    const QString written = cleanWritten(v.text);
    QString relative = written;
    while (relative.startsWith(QLatin1Char('/'))) relative.remove(0, 1);

    if (v.anchored) {
        out << QDir::cleanPath(relative.isEmpty() ? scriptDir : scriptDir + QLatin1Char('/') + relative);
        if (display) *display = QDir::toNativeSeparators(out.front());
        return out;
    }
    if (display) *display = written;
    if (isRealAbsolute(written)) {
        out << QDir::cleanPath(written);
    }
    if (relative.isEmpty()) return out;
    // The library the script lies in first (its own ancestors), then the configured roots,
    // then the script's own folder for a relative path without a leading slash.
    QDir dir(scriptDir);
    QStringList bases;
    do {
        bases << dir.absolutePath();
    } while (dir.cdUp());
    bases << roots;
    QSet<QString> seen;
    for (const QString& base : std::as_const(bases)) {
        const QString path = QDir::cleanPath(base + QLatin1Char('/') + relative);
        if (seen.contains(path.toLower())) continue;
        seen.insert(path.toLower());
        out << path;
    }
    return out;
}

/// A file PoseStudio opens, by its name: a figure, a model, a splat scene — or another script.
bool isOpenable(const QString& path) {
    using namespace pose::ImportFormats;
    return isModelFile(path) || isFigureRouteFile(path) || isSplatFile(path) || isScriptFile(path);
}

/// Reads a `.dsa` as text, or returns false (too big, not text).
bool readScript(const QString& path, QString* text) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxScriptBytes) return false;
    const QByteArray bytes = file.readAll();
    if (bytes.left(4096).contains('\0')) return false; // binary: encrypted or compiled
    QStringDecoder utf8(QStringDecoder::Utf8);
    *text = utf8.decode(bytes);
    if (utf8.hasError()) *text = QString::fromLatin1(bytes); // an older script in a Latin code page
    return true;
}

/// One script, not following links to other scripts (follow() does).
Link readOne(const QString& scriptPath, const QStringList& roots) {
    Link link;
    if (!hasSuffix(scriptPath, ".dsa")) {
        link.problem = Link::Problem::Unreadable; // .dse is encrypted, .dsb compiled
        return link;
    }
    QString text;
    if (!readScript(scriptPath, &text)) {
        link.problem = Link::Problem::Unreadable;
        return link;
    }
    const QString scriptDir = QFileInfo(scriptPath).absolutePath();
    const QVector<Token> t = tokenize(text);
    const Evaluator eval(t);

    static const QHash<QString, Want> kCalls = {
        {QStringLiteral("browseToNativeFolder"), Want::Folder},
        {QStringLiteral("openFile"), Want::File},
        {QStringLiteral("openNativeFile"), Want::File},
        {QStringLiteral("importFile"), Want::File},
        {QStringLiteral("loadScene"), Want::File},
    };
    QString firstMissing;
    QString firstUnsupported;
    int tries = 0;
    for (int i = 0; i + 1 < t.size() && tries < kMaxTries; ++i) {
        if (t[i].type != Token::Name || !isPunct(t, i + 1, '(') || isName(t, i - 1, "function")) continue;
        const bool showsUrl = t[i].text == QLatin1String("showURL");
        const auto call = showsUrl ? kCalls.constEnd() : kCalls.constFind(t[i].text);
        if (!showsUrl && call == kCalls.constEnd()) continue; // not a call that leads anywhere
        ++tries;
        const int argBegin = i + 2;
        const int argEnd = expressionEnd(t, argBegin);
        if (showsUrl) {
            const Value v = eval.range(argBegin, argEnd, i);
            const QString url = v.text.trimmed();
            if (v.ok && !v.anchored && (url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) ||
                                        url.startsWith(QLatin1String("http://"), Qt::CaseInsensitive))) {
                link.kind = Link::Kind::Url;
                link.target = url;
                return link;
            }
            continue;
        }
        const Value v = eval.range(argBegin, argEnd, i);
        if (!v.ok) continue;
        QString display;
        bool there = false; // it names something that IS there, but not something to follow
        for (const QString& path : candidates(v, scriptDir, roots, &display)) {
            if (!exists(path, *call)) continue;
            // A file is followed only when PoseStudio itself opens it (see the header): any
            // other would end at the operating system, started by a script.
            if (*call == Want::File && !isOpenable(path)) {
                if (firstUnsupported.isEmpty()) firstUnsupported = QDir::toNativeSeparators(path);
                there = true;
                continue;
            }
            link.kind = (*call == Want::Folder) ? Link::Kind::Folder : Link::Kind::File;
            link.target = path;
            return link;
        }
        if (!there && firstMissing.isEmpty()) firstMissing = display;
    }

    // No recognised call led anywhere: a literal naming a file PoseStudio opens, the loader that
    // builds its path some other way. Read relative to the script first, then as a content path.
    for (const Token& token : t) {
        if (tries >= kMaxTries) break;
        if (token.type != Token::Str) continue;
        QString written = cleanWritten(token.text);
        const bool slot = written.startsWith(QLatin1String("%1"));
        if (slot) written.remove(0, 2);
        if (written.isEmpty() || !isOpenable(written)) continue;
        ++tries;
        QString display;
        for (const bool anchored : {true, false}) {
            if (!anchored && slot) continue; // "%1/x.duf" is relative to whatever fills %1
            for (const QString& path : candidates({true, anchored, written}, scriptDir, roots,
                                                  anchored ? nullptr : &display)) {
                if (exists(path, Want::File)) {
                    link.kind = Link::Kind::File;
                    link.target = path;
                    return link;
                }
            }
        }
        if (firstMissing.isEmpty()) firstMissing = slot ? QDir::toNativeSeparators(scriptDir + written) : display;
    }

    if (!firstUnsupported.isEmpty()) {
        link.problem = Link::Problem::Unsupported;
        link.missing = firstUnsupported;
    } else if (!firstMissing.isEmpty()) {
        link.problem = Link::Problem::Missing;
        link.missing = firstMissing;
    } else {
        link.problem = Link::Problem::NotALink;
    }
    return link;
}

} // namespace

bool isScriptFile(const QString& path) {
    return hasSuffix(path, ".dsa") || hasSuffix(path, ".dse") || hasSuffix(path, ".dsb");
}

Link follow(const QString& scriptPath, const QStringList& contentRoots) {
    QSet<QString> visited;
    QString current = QFileInfo(scriptPath).absoluteFilePath();
    for (int hop = 0; hop < kMaxHops; ++hop) {
        visited.insert(QDir::cleanPath(current).toLower());
        const Link link = readOne(current, contentRoots);
        if (link.kind != Link::Kind::File || !isScriptFile(link.target)) {
            return link;
        }
        current = link.target;
        if (visited.contains(QDir::cleanPath(current).toLower())) break; // a circle of links
    }
    Link circle;
    circle.problem = Link::Problem::NotALink;
    return circle;
}

QString loneFigureIn(const QString& folder) {
    const QFileInfoList files =
        QDir(folder).entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
    QString found;
    for (const QFileInfo& info : files) {
        if (HiddenNames::isHidden(info.fileName())) continue; // hidden from the browser: not the folder's character
        if (!pose::ImportFormats::isNativeFigureFile(info.fileName())) continue;
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = file.readAll();
        // A wearable (clothing, hair) is fitted onto a figure and a prop has no skeleton: on
        // their own neither is a character.
        if (pose::figureFileKind(std::vector<std::uint8_t>(data.cbegin(), data.cend())) !=
            pose::FigureFileKind::Character) {
            continue;
        }
        if (!found.isEmpty()) return {}; // two characters: the folder is the user's to choose from
        found = info.absoluteFilePath();
    }
    return found;
}

} // namespace AssetScriptLink
