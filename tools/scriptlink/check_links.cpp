/**
 * @file check_links.cpp
 * @brief The script-link reader's guard (CTest `script-link-check`): AssetScriptLink::follow()
 *        and loneFigureIn() on synthetic content-library scripts in a temporary library.
 *
 * Each form a vendor's script takes is written out and followed: the content library's folder
 * link in its two-level and one-level forms, a web link, a loader by a content-relative path in
 * a variable, a loader by "%1".arg(the script's folder), a literal the reader finds with no call
 * around it, an absolute Windows path with escaped backslashes, single quotes, comments that
 * hold calls, a chain of links, a circle, a dead link, a program, an encrypted script and a
 * binary one — and which folders open their character: one figure (plain and gzip), two, none,
 * a wearable alone, a prop alone, and wearables and props beside the character.
 *
 * Usage: PoseStudioScriptLinkCheck                       — the checks; exit code = failures
 *        PoseStudioScriptLinkCheck --follow <script> [<content root>...]
 *                                                        — where a real script leads
 */

#include "assetscriptlink.h"

#include <miniz.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const QString& what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
}

bool samePath(const QString& a, const QString& b) {
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

const char* kindName(AssetScriptLink::Link::Kind k) {
    using K = AssetScriptLink::Link::Kind;
    switch (k) {
    case K::None: return "none";
    case K::Folder: return "folder";
    case K::File: return "file";
    case K::Url: return "url";
    }
    return "?";
}

const char* problemName(AssetScriptLink::Link::Problem p) {
    using P = AssetScriptLink::Link::Problem;
    switch (p) {
    case P::None: return "none";
    case P::Unreadable: return "unreadable";
    case P::NotALink: return "not a link";
    case P::Missing: return "missing";
    }
    return "?";
}

void write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        std::printf("cannot write %s\n", qPrintable(path));
        return;
    }
    f.write(bytes);
}

/// A gzip stream around @p data (raw deflate between the gzip header and its CRC/size trailer).
QByteArray gzip(const QByteArray& data) {
    size_t rawLen = 0;
    void* raw = tdefl_compress_mem_to_heap(data.constData(), static_cast<size_t>(data.size()), &rawLen,
                                           TDEFL_DEFAULT_MAX_PROBES);
    QByteArray out;
    const char header[10] = {'\x1f', '\x8b', 8, 0, 0, 0, 0, 0, 0, '\xff'};
    out.append(header, 10);
    out.append(static_cast<const char*>(raw), static_cast<qsizetype>(rawLen));
    mz_free(raw);
    const auto crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.constData()),
                 static_cast<size_t>(data.size())));
    const auto size = static_cast<std::uint32_t>(data.size());
    for (int i = 0; i < 4; ++i) out.append(static_cast<char>((crc >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; ++i) out.append(static_cast<char>((size >> (8 * i)) & 0xff));
    return out;
}

/// A character preset: the figure node and its skeleton, a chain of 30 bones parented in the file.
QByteArray characterPreset() {
    QByteArray nodes =
        R"({"id":"Figure","url":"/data/x/Figure.dsf#Figure","geometries":[{"url":"/data/x/Figure.dsf#geometry"}]})";
    QByteArray parent = "Figure";
    for (int i = 0; i < 30; ++i) {
        const QByteArray bone = "bone" + QByteArray::number(i);
        nodes += R"(,{"id":")" + bone + R"(","url":"/data/x/Figure.dsf#)" + bone + R"(","parent":"#)" + parent + "\"}";
        parent = bone;
    }
    return R"({"file_version":"0.6.0.0","asset_info":{"type":"scene_subset"},"scene":{"nodes":[)" + nodes + "]}}";
}
const QByteArray kFigurePreset = characterPreset();
const QByteArray kStandaloneProp =
    R"({"file_version":"0.6.0.0","asset_info":{"type":"scene_subset"},"scene":{"nodes":[)"
    R"({"id":"Wand","url":"/data/x/Wand.dsf#Wand","geometries":[{"url":"/data/x/Wand.dsf#geometry"}]}]}})";
const QByteArray kWearablePreset =
    R"({"file_version":"0.6.0.0","asset_info":{"type":"wearable"},"scene":{"nodes":[)"
    R"({"id":"Boots","url":"/data/x/Boots.dsf#Boots","conform_target":"name://@selection:",)"
    R"("geometries":[{"url":"/data/x/Boots.dsf#geometry"}]}]}})";
const QByteArray kPropOnBone =
    R"({"file_version":"0.6.0.0","asset_info":{"type":"wearable"},"scene":{"nodes":[)"
    R"({"id":"Prop","url":"/data/x/Prop.dsf#Prop","parent":"name://@selection/hip:",)"
    R"("geometries":[{"url":"/data/x/Prop.dsf#geometry"}]}]}})";
const QByteArray kShapePreset =
    R"({"file_version":"0.6.0.0","asset_info":{"type":"preset_shape"},"scene":{"modifiers":[{"url":"#Body"}]}})";
const QByteArray kMaterialPreset =
    R"({"file_version":"0.6.0.0","asset_info":{"type":"preset_material"},"scene":{"materials":[{"url":"#Skin","geometry":"#geometry"}]}})";
const QByteArray kBaseFigure =
    R"({"file_version":"0.6.0.0","asset_info":{"type":"figure"},"geometry_library":[{"id":"geometry","vertices":{"count":0,"values":[]}}]})";

QByteArray folderLink(const QString& relative) {
    return QStringLiteral(
               "var oPane = MainWindow.getPaneMgr().findPane(\"DzContentLibraryPane\");\r\n"
               "var sPath = MainWindow.getPaneMgr().findPane(\"DzContentLibraryPane\").getSelectedContainer().getFullPath();\r\n"
               "oPane.browseToNativeFolder(sPath + \"%1\");\r\n")
        .arg(relative)
        .toUtf8();
}

int followOne(const QStringList& args) {
    const QString script = args.at(2);
    const QStringList roots = args.mid(3);
    const AssetScriptLink::Link link = AssetScriptLink::follow(script, roots);
    std::printf("kind:    %s\ntarget:  %s\nproblem: %s\nmissing: %s\n", kindName(link.kind),
                qPrintable(link.kind == AssetScriptLink::Link::Kind::Url ? link.target
                                                                       : QDir::toNativeSeparators(link.target)),
                problemName(link.problem),
                qPrintable(link.missing));
    if (link.kind == AssetScriptLink::Link::Kind::Folder) {
        std::printf("figure:  %s\n",
                    qPrintable(QDir::toNativeSeparators(AssetScriptLink::loneFigureIn(link.target))));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() >= 3 && args.at(1) == QLatin1String("--follow")) {
        return followOne(args);
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("no temporary folder\n");
        return 1;
    }
    using Link = AssetScriptLink::Link;
    const QString lib = tmp.path() + QStringLiteral("/My Library");
    const QString chars = lib + QStringLiteral("/People/Gen Female/Characters");
    const QString product = chars + QStringLiteral("/Vendor Char");
    const QString charFolder = product + QStringLiteral("/Char for Gen");
    const QString charDuf = charFolder + QStringLiteral("/Char.duf");

    write(charDuf, kFigurePreset);
    write(charDuf + QStringLiteral(".png"), "png");
    write(charFolder + QStringLiteral("/Shape Apply.duf"), kShapePreset);
    write(charFolder + QStringLiteral("/Shape Remove.duf"), kShapePreset);
    QDir().mkpath(charFolder + QStringLiteral("/Materials"));
    write(charFolder + QStringLiteral("/Materials/Skin.duf"), kMaterialPreset);

    // The content library's folder link, both levels a vendor ships.
    write(chars + QStringLiteral("/Vendor Char.dsa"), folderLink(QStringLiteral("/Vendor Char/Char for Gen")));
    write(product + QStringLiteral("/Vendor Char.dsa"), folderLink(QStringLiteral("/Char for Gen")));
    // A web link.
    write(product + QStringLiteral("/Vendor-Site.com.dsa"), "App.showURL(\"https://vendor.example.com\");");
    // A loader by a content-relative path held in a variable.
    write(chars + QStringLiteral("/Loader Content.dsa"),
          "// Loads the character\r\nvar sFile = \"/People/Gen Female/Characters/Vendor Char/Char for Gen/Char.duf\";\r\n"
          "var oContentMgr = App.getContentMgr();\r\noContentMgr.openFile( sFile, true );\r\n");
    // A loader by "%1".arg(the script's own folder).
    write(chars + QStringLiteral("/Loader Arg.dsa"),
          "var oFile = new DzFileInfo( getScriptFileName() );\nvar sDir = oFile.path();\n"
          "App.getContentMgr().openNativeFile( String(\"%1/Vendor Char/Char for Gen/Char.duf\").arg( sDir ), false );\n");
    // A literal with no recognised call around it.
    write(chars + QStringLiteral("/Loader Literal.dsa"),
          "var aFiles = [ 'Vendor Char/Char for Gen/Char.duf' ];\nfor (var i = 0; i < aFiles.length; i++) { doLoad(aFiles[i]); }\n");
    // An absolute Windows path with escaped backslashes.
    write(chars + QStringLiteral("/Absolute.dsa"),
          QStringLiteral("App.getContentMgr().openFile(\"%1\");").arg(QDir::toNativeSeparators(charDuf).replace(QLatin1String("\\"), QLatin1String("\\\\"))).toUtf8());
    // Single quotes, and calls in comments that must not be followed.
    write(chars + QStringLiteral("/Commented.dsa"),
          "// App.showURL('https://wrong.example');\n/* oPane.browseToNativeFolder(sPath + '/Nope'); */\n"
          "var sPath = oPane.getSelectedContainer().getFullPath();\noPane.browseToNativeFolder(sPath + '/Vendor Char');\n");
    // A chain: a loader of another script.
    write(chars + QStringLiteral("/Chain.dsa"), "App.getContentMgr().openFile(sPath + \"/Vendor Char/Vendor Char.dsa\");");
    // A circle.
    write(chars + QStringLiteral("/Circle A.dsa"), "openFile(sPath + \"/Circle B.dsa\");");
    write(chars + QStringLiteral("/Circle B.dsa"), "openFile(sPath + \"/Circle A.dsa\");");
    // A dead link, a program, an encrypted script, a binary one.
    write(chars + QStringLiteral("/Dead.dsa"), folderLink(QStringLiteral("/Gone")));
    write(chars + QStringLiteral("/Program.dsa"),
          "this.oProcess = new DzProcess(findAbsContentFilePath( \"Scripts/Tool/Tool.exe\" ));\n"
          "if( !this.oProcess.start() ){ MessageBox.critical( qsTr(\"Could not start the process.\"), qsTr(\"Fatal Error\"), qsTr(\"&OK\") ); }\n");
    write(chars + QStringLiteral("/Encrypted.dse"), "DZEncrypted\x01\x02\x03");
    write(chars + QStringLiteral("/Binary.dsa"), QByteArray("\x00\x01\x02 binary", 10));
    // A script outside the library, linking by a content-relative path.
    const QString outside = tmp.path() + QStringLiteral("/Elsewhere/Outside.dsa");
    write(outside, "App.getContentMgr().openFile(\"/People/Gen Female/Characters/Vendor Char/Char for Gen/Char.duf\");");

    // Folders for loneFigureIn: two figures, one gzip figure, a base figure, presets only.
    write(lib + QStringLiteral("/Two/A.duf"), kFigurePreset);
    write(lib + QStringLiteral("/Two/B.duf"), gzip(kFigurePreset));
    write(lib + QStringLiteral("/Gz/One.duf"), gzip(kFigurePreset));
    write(lib + QStringLiteral("/Gz/Pose.duf"), gzip(kShapePreset));
    write(lib + QStringLiteral("/Base/Figure.dsf"), kBaseFigure);
    write(lib + QStringLiteral("/Wearable/Boots.duf"), kWearablePreset);
    write(lib + QStringLiteral("/Wearable/Leather.duf"), kMaterialPreset);
    write(lib + QStringLiteral("/Dressed/Char.duf"), kFigurePreset);
    write(lib + QStringLiteral("/Dressed/Boots.duf"), gzip(kWearablePreset));
    write(lib + QStringLiteral("/Dressed/Prop.duf"), kPropOnBone);
    write(lib + QStringLiteral("/Dressed/Wand.duf"), kStandaloneProp);
    write(lib + QStringLiteral("/Prop/Wand.duf"), kStandaloneProp);
    write(lib + QStringLiteral("/Prop/Wand in Hand.duf"), kPropOnBone);
    write(lib + QStringLiteral("/Presets/Skin.duf"), kMaterialPreset);
    write(lib + QStringLiteral("/Presets/Shape.duf"), kShapePreset);
    write(lib + QStringLiteral("/Presets/Broken.duf"), "{\"scene\":{\"nodes\":[{\"geometries\":");

    auto expect = [&](const QString& script, Link::Kind kind, const QString& target,
                      Link::Problem problem = Link::Problem::None, const QStringList& roots = {}) {
        const Link link = AssetScriptLink::follow(script, roots);
        const bool ok = link.kind == kind && link.problem == problem &&
                        (target.isEmpty() ? true : (kind == Link::Kind::Url ? link.target == target
                                                                            : samePath(link.target, target)));
        check(ok, QStringLiteral("%1 -> %2 %3 (%4)%5")
                      .arg(QFileInfo(script).fileName(), QString::fromLatin1(kindName(link.kind)),
                           link.kind == Link::Kind::Url ? link.target : QDir::toNativeSeparators(link.target),
                           QString::fromLatin1(problemName(link.problem)),
                           link.missing.isEmpty() ? QString() : QStringLiteral(" missing ") + link.missing));
    };

    expect(chars + QStringLiteral("/Vendor Char.dsa"), Link::Kind::Folder, charFolder);
    expect(product + QStringLiteral("/Vendor Char.dsa"), Link::Kind::Folder, charFolder);
    expect(product + QStringLiteral("/Vendor-Site.com.dsa"), Link::Kind::Url, QStringLiteral("https://vendor.example.com"));
    expect(chars + QStringLiteral("/Loader Content.dsa"), Link::Kind::File, charDuf);
    expect(chars + QStringLiteral("/Loader Arg.dsa"), Link::Kind::File, charDuf);
    expect(chars + QStringLiteral("/Loader Literal.dsa"), Link::Kind::File, charDuf);
    expect(chars + QStringLiteral("/Absolute.dsa"), Link::Kind::File, charDuf);
    expect(chars + QStringLiteral("/Commented.dsa"), Link::Kind::Folder, product);
    expect(chars + QStringLiteral("/Chain.dsa"), Link::Kind::Folder, charFolder);
    expect(chars + QStringLiteral("/Circle A.dsa"), Link::Kind::None, {}, Link::Problem::NotALink);
    expect(chars + QStringLiteral("/Dead.dsa"), Link::Kind::None, {}, Link::Problem::Missing);
    expect(chars + QStringLiteral("/Program.dsa"), Link::Kind::None, {}, Link::Problem::NotALink);
    expect(chars + QStringLiteral("/Encrypted.dse"), Link::Kind::None, {}, Link::Problem::Unreadable);
    expect(chars + QStringLiteral("/Binary.dsa"), Link::Kind::None, {}, Link::Problem::Unreadable);
    expect(outside, Link::Kind::None, {}, Link::Problem::Missing);          // no root to find it under
    expect(outside, Link::Kind::File, charDuf, Link::Problem::None, {lib}); // ... and with the library

    check(AssetScriptLink::isScriptFile(QStringLiteral("a.DSA")) && AssetScriptLink::isScriptFile(QStringLiteral("a.dse")) &&
              AssetScriptLink::isScriptFile(QStringLiteral("a.dsb")) && !AssetScriptLink::isScriptFile(QStringLiteral("a.duf")),
          QStringLiteral("isScriptFile: .dsa .dse .dsb, not .duf"));

    auto expectFigure = [&](const QString& folder, const QString& want) {
        const QString got = AssetScriptLink::loneFigureIn(folder);
        check(want.isEmpty() ? got.isEmpty() : samePath(got, want),
              QStringLiteral("loneFigureIn(%1) = %2").arg(QFileInfo(folder).fileName(),
                                                          got.isEmpty() ? QStringLiteral("(none)") : QFileInfo(got).fileName()));
    };
    expectFigure(charFolder, charDuf);                               // one figure among shape presets
    expectFigure(lib + QStringLiteral("/Gz"), lib + QStringLiteral("/Gz/One.duf")); // gzip
    expectFigure(lib + QStringLiteral("/Base"), lib + QStringLiteral("/Base/Figure.dsf"));
    expectFigure(lib + QStringLiteral("/Two"), {});                  // two: the user chooses
    expectFigure(lib + QStringLiteral("/Presets"), {});              // presets only (and a broken file)
    expectFigure(product, {});                                       // the figure is a level down
    expectFigure(lib + QStringLiteral("/Wearable"), {});             // clothing alone is no character
    expectFigure(lib + QStringLiteral("/Prop"), {});                 // nor is a prop with no skeleton
    expectFigure(lib + QStringLiteral("/Dressed"), lib + QStringLiteral("/Dressed/Char.duf")); // ... beside one

    std::printf("\n%d of %d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures;
}
