/**
 * @file assetscriptlink.h
 * @brief Reads a content-library SCRIPT as the link it usually is. QtCore only, no widgets.
 *
 * Character content arrives with scripts among its presets — `.dsa` (plain text), `.dse`
 * (encrypted), `.dsb` (compiled) — and most of them are LINKS: a character's `.dsa` in a
 * Characters folder is three lines that make the content library browse to the folder holding
 * the character; another is one line, App.showURL(), to the vendor's site; a "loader" opens a
 * preset through the content manager. Double-clicked, the Asset Manager used to hand every one
 * to the operating system, which started the application that owns the extension.
 *
 * PoseStudio never RUNS a script. It READS a plain-text one for the link and follows that
 * (AssetManagerWidget::openScriptLink): a folder is browsed to — and the character in it opened
 * when it is the only one there (loneFigureIn) — a file is opened as its own double-click would
 * open it, a web page goes to the browser. Recognised, the script's comments skipped, the first
 * call in source order that leads somewhere winning:
 *  - browseToNativeFolder(<path>)                                        — a folder;
 *  - openFile / openNativeFile / importFile / loadScene (<path>, ...)   — a file;
 *  - showURL(<url>)                                                     — an http(s) page;
 *  - failing all of those, a string literal naming a file PoseStudio opens (a figure, a model, a
 *    splat scene, another script) — the loader that builds its path some other way.
 * A <path> is an expression of string literals joined by `+`, variables assigned one, `String()`
 * and `"%1…".arg()`. A LEADING term that is not a literal — `sPath + "/Sinn for G8F"`, where sPath
 * is the content library's selected folder (the script's own, when it is double-clicked) or the
 * folder of getScriptFileName() — stands for the script's own folder. A path of literals alone is
 * absolute, or content-relative ("/People/…") and looked for under the script's own ancestors
 * (the library it was found in), then the given content roots. A link to another script is
 * followed in turn, four hops at most and never in a circle.
 */

#ifndef ASSETSCRIPTLINK_H
#define ASSETSCRIPTLINK_H

#include <QString>
#include <QStringList>

namespace AssetScriptLink {

/// Where a script leads, or why it leads nowhere PoseStudio can go.
struct Link {
    enum class Kind {
        None,   ///< nothing to follow: see problem
        Folder, ///< target is an existing folder to browse
        File,   ///< target is an existing file that is not itself a script
        Url,    ///< target is an http(s) URL
    };
    enum class Problem {
        None,       ///< kind is not None
        Unreadable, ///< encrypted or compiled (`.dse` / `.dsb`), or not text
        NotALink,   ///< a program: no folder, file or page in it (or a circle of links)
        Missing,    ///< it names a folder or file that is not there: see missing
    };

    Kind    kind = Kind::None;
    QString target;  ///< the absolute folder or file path, or the URL
    Problem problem = Problem::None;
    QString missing; ///< Problem::Missing: the path the script names (resolved where it could be)
};

/// A content-library script by its extension: `.dsa`, `.dse` or `.dsb`.
bool isScriptFile(const QString& path);

/// Follows @p scriptPath to what it links to. @p contentRoots are the folders a content-relative
/// path may lie under besides the script's own ancestors (the asset libraries and the configured
/// figure content roots).
Link follow(const QString& scriptPath, const QStringList& contentRoots);

/// The one character among @p folder's own files (its subfolders are not looked in), or an empty
/// string when it holds none or several: a folder of one character is that character, one of
/// several is the user's to choose from. Pose, material and shape presets are no figures, a
/// wearable (clothing, hair: a figure fitted onto or attached to another) and a prop (a figure
/// with no skeleton) no character — pose::figureFileKind decides.
QString loneFigureIn(const QString& folder);

} // namespace AssetScriptLink

#endif // ASSETSCRIPTLINK_H
