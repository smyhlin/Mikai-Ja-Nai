#include "NativePeImage.hpp"

#include <algorithm>
#include <utility>

#include <QtCore/QVector>

namespace mplayer {
namespace native {
namespace {

constexpr quint16 kDosSignature = 0x5A4D;      // "MZ"
constexpr quint32 kNtSignature = 0x00004550;   // "PE\0\0"
constexpr quint16 kMachineAmd64 = 0x8664;
constexpr quint16 kOptionalMagicPe32Plus = 0x20B;
constexpr quint32 kDirectoryEntryImport = 1;
constexpr quint32 kDirectoryEntryDelayImport = 13;
// NumberOfRvaAndSizes sits at 108 and the directory array itself at 112; the
// delay-import entry is the 14th, so fewer than 14 declared means neither the
// eager nor the delayed table can be trusted.
constexpr qsizetype kNumberOfRvaAndSizesOffset = 108;
constexpr qsizetype kDataDirectoryStartOffset = 112;
constexpr qsizetype kImageBaseOffset = 24;             // inside the optional header
constexpr qsizetype kSectionHeaderSize = 40;
constexpr qsizetype kSizeOfHeadersOffset = 60;  // inside the optional header
constexpr qsizetype kExportDirectorySize = 40;         // IMAGE_EXPORT_DIRECTORY size
constexpr qsizetype kExportNamePointerRvaOffset = 32;   // AddressOfNames
constexpr qsizetype kExportOrdinalTableRvaOffset = 36; // AddressOfNameOrdinals
constexpr qsizetype kExportNumberOfNamesOffset = 24;    // NumberOfNames
constexpr qsizetype kExportNumberOfFunctionsOffset = 20; // NumberOfFunctions

/// Little-endian readers over a buffer that is never trusted. A short read
/// yields 0, and the caller checks the length separately before using a value.
quint8 u8(const QByteArray& b, qsizetype at) {
    if (at < 0 || at + 1 > b.size()) return 0;
    return static_cast<quint8>(b.at(at));
}

quint16 u16(const QByteArray& b, qsizetype at) {
    if (at < 0 || at + 2 > b.size()) return 0;
    return quint16(quint8(b.at(at))) | (quint16(quint8(b.at(at + 1))) << 8);
}

quint32 u32(const QByteArray& b, qsizetype at) {
    if (at < 0 || at + 4 > b.size()) return 0;
    return quint32(quint8(b.at(at))) | (quint32(quint8(b.at(at + 1))) << 8) |
           (quint32(quint8(b.at(at + 2))) << 16) | (quint32(quint8(b.at(at + 3))) << 24);
}

quint64 u64(const QByteArray& b, qsizetype at) {
    return quint64(u32(b, at)) | (quint64(u32(b, at + 4)) << 32);
}

struct Section {
    quint32 virtualAddress = 0;
    quint32 virtualSize = 0;
    quint32 rawPointer = 0;
    quint32 rawSize = 0;
};

/// Maps a relative virtual address onto a file offset. An RVA that falls in no
/// section and lands inside the header block maps to itself, which is how a
/// small hand-built test image keeps an import table at RVA 0x80.
qsizetype rvaToOffset(quint32 rva, const QVector<Section>& sections, qsizetype sizeOfHeaders,
                      qsizetype fileSize) {
    for (const Section& section : sections) {
        const quint32 span = std::max(section.virtualSize, section.rawSize);
        if (rva < section.virtualAddress ||
            quint64(rva) >= quint64(section.virtualAddress) + span) continue;
        const quint32 delta = rva - section.virtualAddress;
        if (delta >= section.rawSize) return -1;  // uninitialised tail
        const qsizetype offset = qsizetype(section.rawPointer) + qsizetype(delta);
        return offset < fileSize ? offset : -1;
    }
    if (rva < quint32(sizeOfHeaders) && qsizetype(rva) < fileSize) return qsizetype(rva);
    return -1;
}

/// A NUL-terminated ASCII name inside the image, bounded by the buffer.
QString readName(const QByteArray& b, qsizetype at) {
    if (at < 0 || at >= b.size()) return {};
    qsizetype end = at;
    while (end < b.size() && b.at(end) != '\0') ++end;
    if (end == at) return {};
    return QString::fromLatin1(b.mid(at, end - at));
}

void collect(QStringList& into, const QString& name) {
    if (name.isEmpty()) return;
    const QString lowered = name.toLower();
    if (!into.contains(lowered)) into.append(lowered);
}

}  // namespace

PeImageInfo inspectPeImage(const QByteArray& bytes) {
    PeImageInfo info;
    const auto refuse = [&info](QString reason) {
        info.ok = false;
        info.reason = std::move(reason);
        info.imports.clear();
        info.delayImports.clear();
        return info;
    };

    if (bytes.size() < 64) return refuse(QStringLiteral("file is too small to be a PE image"));
    if (u16(bytes, 0) != kDosSignature) return refuse(QStringLiteral("no MZ signature"));
    const quint32 ntOffset = u32(bytes, 0x3C);
    if (ntOffset == 0 || qsizetype(ntOffset) + 24 > bytes.size())
        return refuse(QStringLiteral("PE header offset is outside the file"));
    if (u32(bytes, ntOffset) != kNtSignature) return refuse(QStringLiteral("no PE signature"));

    const qsizetype fileHeader = qsizetype(ntOffset) + 4;
    info.machine = u16(bytes, fileHeader);
    if (info.machine != kMachineAmd64)
        return refuse(QStringLiteral("machine 0x%1 is not x64 (0x8664)")
                          .arg(info.machine, 4, 16, QLatin1Char('0')));
    const quint16 sectionCount = u16(bytes, fileHeader + 2);
    const quint16 optionalSize = u16(bytes, fileHeader + 16);
    const qsizetype optional = fileHeader + 20;
    if (optional + qsizetype(optionalSize) > bytes.size())
        return refuse(QStringLiteral("optional header runs past the end of the file"));
    if (u16(bytes, optional) != kOptionalMagicPe32Plus)
        return refuse(QStringLiteral("optional header is not PE32+ (64-bit)"));
    if (optionalSize < 112) return refuse(QStringLiteral("optional header has no data directories"));

    const qsizetype directories = optional + kDataDirectoryStartOffset;
    const quint64 imageBase = u64(bytes, optional + kImageBaseOffset);
    const qsizetype sizeOfHeaders = qsizetype(u32(bytes, optional + kSizeOfHeadersOffset));
    const quint32 directoryCount = u32(bytes, optional + kNumberOfRvaAndSizesOffset);
    if (directoryCount <= kDirectoryEntryDelayImport)
        return refuse(QStringLiteral("the image declares only %1 data directories; an import "
                                     "table needs at least %2")
                          .arg(directoryCount)
                          .arg(kDirectoryEntryDelayImport + 1));

    QVector<Section> sections;
    sections.reserve(sectionCount);
    const qsizetype sectionTable = optional + optionalSize;
    for (qsizetype i = 0; i < qsizetype(sectionCount); ++i) {
        const qsizetype at = sectionTable + i * kSectionHeaderSize;
        if (at + kSectionHeaderSize > bytes.size())
            return refuse(QStringLiteral("section header table runs past the end of the file"));
        Section section;
        section.virtualSize = u32(bytes, at + 8);
        section.virtualAddress = u32(bytes, at + 12);
        section.rawSize = u32(bytes, at + 16);
        section.rawPointer = u32(bytes, at + 20);
        sections.append(section);
    }

    const auto directoryRva = [&](quint32 index) {
        return u32(bytes, directories + qsizetype(index) * 8);
    };

    if (const quint32 importRva = directoryRva(kDirectoryEntryImport)) {
        const qsizetype table = rvaToOffset(importRva, sections, sizeOfHeaders, bytes.size());
        if (table < 0) return refuse(QStringLiteral("import directory RVA does not map to a file offset"));
        // Each IMAGE_IMPORT_DESCRIPTOR is 20 bytes and the table ends with a
        // zeroed entry. The name RVA lives at +12.
        for (qsizetype at = table; at + 20 <= bytes.size(); at += 20) {
            if (u32(bytes, at) == 0 && u32(bytes, at + 12) == 0) break;
            const qsizetype nameAt = rvaToOffset(u32(bytes, at + 12), sections, sizeOfHeaders, bytes.size());
            if (nameAt < 0)
                return refuse(QStringLiteral("an imported module name RVA does not map into the file"));
            collect(info.imports, readName(bytes, nameAt));
        }
    }

    if (const quint32 delayRva = directoryRva(kDirectoryEntryDelayImport)) {
        const qsizetype table = rvaToOffset(delayRva, sections, sizeOfHeaders, bytes.size());
        if (table < 0)
            return refuse(QStringLiteral("delay-import directory RVA does not map to a file offset"));
        // IMAGE_DELAYLOAD_DESCRIPTOR is 32 bytes: attributes, name, module,
        // IAT, INT, bound IAT, unload IAT, timestamp. With attributes bit 0 set
        // the name field is an RVA; otherwise it is a VA relative to the image
        // base, which is why the base is needed here at all.
        for (qsizetype at = table; at + 32 <= bytes.size(); at += 32) {
            const quint32 attributes = u32(bytes, at);
            const quint32 nameField = u32(bytes, at + 4);
            if (attributes == 0 && nameField == 0) break;
            quint32 nameRva = nameField;
            if ((attributes & 1u) == 0) {
                if (nameField < imageBase)
                    return refuse(QStringLiteral("delay-import name VA is below the image base"));
                nameRva = quint32(quint64(nameField) - imageBase);
            }
            const qsizetype nameAt = rvaToOffset(nameRva, sections, sizeOfHeaders, bytes.size());
            if (nameAt < 0)
                return refuse(QStringLiteral("a delay-imported module name does not map into the file"));
            collect(info.delayImports, readName(bytes, nameAt));
        }
    }

    // The header promises a stable order, so two reads of the same bytes compare
    // equal regardless of how the descriptors happened to be laid out.
    std::sort(info.imports.begin(), info.imports.end());
    std::sort(info.delayImports.begin(), info.delayImports.end());

    info.ok = true;
    return info;
}

QStringList systemImportExceptions() {
    // Deliberately conservative. Every entry is a component the OS owns; adding
    // a redistributable library here would silently weaken the closure check
    // that rejects a bundle reaching outside its own directory.
    return {
        QStringLiteral("advapi32.dll"),
        QStringLiteral("avicap32.dll"),
        QStringLiteral("avrt.dll"),
        QStringLiteral("bcrypt.dll"),
        QStringLiteral("bcryptprimitives.dll"),
        QStringLiteral("cabinet.dll"),
        QStringLiteral("cfgmgr32.dll"),
        QStringLiteral("combase.dll"),
        QStringLiteral("comctl32.dll"),
        QStringLiteral("comdlg32.dll"),
        QStringLiteral("coremessaging.dll"),
        QStringLiteral("crypt32.dll"),
        QStringLiteral("cryptbase.dll"),
        QStringLiteral("d2d1.dll"),
        QStringLiteral("d3d10.dll"),
        QStringLiteral("d3d10_1.dll"),
        QStringLiteral("d3d11.dll"),
        QStringLiteral("d3d12.dll"),
        QStringLiteral("d3d9.dll"),
        QStringLiteral("d3dcompiler_47.dll"),
        QStringLiteral("dbghelp.dll"),
        QStringLiteral("dcomp.dll"),
        QStringLiteral("dhcpcsvc.dll"),
        QStringLiteral("dnsapi.dll"),
        QStringLiteral("dsound.dll"),
        QStringLiteral("dwmapi.dll"),
        QStringLiteral("dwrite.dll"),
        QStringLiteral("dxgi.dll"),
        QStringLiteral("dxva2.dll"),
        QStringLiteral("evr.dll"),
        QStringLiteral("gdi32.dll"),
        QStringLiteral("gdi32full.dll"),
        QStringLiteral("gdiplus.dll"),
        QStringLiteral("graphicscapture.dll"),
        QStringLiteral("imagehlp.dll"),
        QStringLiteral("imm32.dll"),
        QStringLiteral("iphlpapi.dll"),
        QStringLiteral("kernel32.dll"),
        QStringLiteral("kernelbase.dll"),
        QStringLiteral("mf.dll"),
        QStringLiteral("mfplat.dll"),
        QStringLiteral("mfreadwrite.dll"),
        QStringLiteral("mfuuid.dll"),
        QStringLiteral("msacm32.dll"),
        QStringLiteral("msimg32.dll"),
        QStringLiteral("msvcp_win.dll"),
        QStringLiteral("msvcrt.dll"),
        QStringLiteral("mswsock.dll"),
        QStringLiteral("ncrypt.dll"),
        QStringLiteral("netapi32.dll"),
        QStringLiteral("normaliz.dll"),
        QStringLiteral("nsi.dll"),
        QStringLiteral("ntdll.dll"),
        QStringLiteral("ole32.dll"),
        QStringLiteral("oleaut32.dll"),
        QStringLiteral("opengl32.dll"),
        QStringLiteral("powrprof.dll"),
        QStringLiteral("propsys.dll"),
        QStringLiteral("psapi.dll"),
        QStringLiteral("rpcrt4.dll"),
        QStringLiteral("sechost.dll"),
        QStringLiteral("secur32.dll"),
        QStringLiteral("setupapi.dll"),
        QStringLiteral("shcore.dll"),
        QStringLiteral("shell32.dll"),
        QStringLiteral("shfolder.dll"),
        QStringLiteral("shlwapi.dll"),
        QStringLiteral("sspicli.dll"),
        QStringLiteral("ucrtbase.dll"),
        QStringLiteral("url.dll"),
        QStringLiteral("user32.dll"),
        QStringLiteral("userenv.dll"),
        QStringLiteral("usp10.dll"),
        QStringLiteral("uxtheme.dll"),
        QStringLiteral("version.dll"),
        QStringLiteral("vulkan-1.dll"),
        QStringLiteral("win32u.dll"),
        QStringLiteral("windowscodecs.dll"),
        QStringLiteral("winhttp.dll"),
        QStringLiteral("wininet.dll"),
        QStringLiteral("winmm.dll"),
        QStringLiteral("wintrust.dll"),
        QStringLiteral("wldap32.dll"),
        QStringLiteral("ws2_32.dll"),
        QStringLiteral("wsock32.dll"),
        QStringLiteral("wtsapi32.dll"),
    };
}

PeExportInfo inspectPeExports(const QByteArray& bytes) {
    PeExportInfo result;
    const auto refuse = [&result](QString reason) {
        result.ok = false;
        result.reason = std::move(reason);
        result.exportsMpvClientApiVersion = false;
        return result;
    };
    if (bytes.size() < 64 || u16(bytes, 0) != kDosSignature)
        return refuse(QStringLiteral("missing or truncated DOS header"));
    const qsizetype nt = u32(bytes, 0x3c);
    if (nt == 0 || nt > bytes.size() - 24 || u32(bytes, nt) != kNtSignature)
        return refuse(QStringLiteral("invalid PE header"));
    const qsizetype fileHeader = nt + 4;
    const qsizetype optional = fileHeader + 20;
    const qsizetype optionalSize = u16(bytes, fileHeader + 16);
    if (u16(bytes, fileHeader) != kMachineAmd64 || optionalSize < 112 ||
        optional > bytes.size() - optionalSize ||
        u16(bytes, optional) != kOptionalMagicPe32Plus)
        return refuse(QStringLiteral("invalid x64 optional header"));
    const quint32 directoryCount = u32(bytes, optional + kNumberOfRvaAndSizesOffset);
    if (directoryCount == 0) {
        result.ok = true;
        return result;
    }
    if (optionalSize < kDataDirectoryStartOffset + 8)
        return refuse(QStringLiteral("export directory entry is truncated"));
    const quint32 directoryRva = u32(bytes, optional + kDataDirectoryStartOffset);
    const quint32 directorySize = u32(bytes, optional + kDataDirectoryStartOffset + 4);
    if (directoryRva == 0 && directorySize == 0) {
        result.ok = true;
        return result;
    }
    if (directoryRva == 0 || directorySize < kExportDirectorySize ||
        quint64(directoryRva) + directorySize > (quint64{1} << 32))
        return refuse(QStringLiteral("invalid export directory"));

    const qsizetype sectionCount = u16(bytes, fileHeader + 2);
    const qsizetype sectionTable = optional + optionalSize;
    if (sectionCount > (bytes.size() - sectionTable) / kSectionHeaderSize)
        return refuse(QStringLiteral("section table is truncated"));
    QVector<Section> sections;
    sections.reserve(sectionCount);
    for (qsizetype i = 0; i < sectionCount; ++i) {
        const qsizetype at = sectionTable + i * kSectionHeaderSize;
        sections.append({u32(bytes, at + 12), u32(bytes, at + 8),
                         u32(bytes, at + 20), u32(bytes, at + 16)});
    }
    const qsizetype headers = u32(bytes, optional + kSizeOfHeadersOffset);
    const auto mappedRange = [&](quint32 rva, quint64 length) -> qsizetype {
        const qsizetype offset = rvaToOffset(rva, sections, headers, bytes.size());
        if (offset < 0 || length > quint64(bytes.size() - offset)) return -1;
        for (const Section& section : sections) {
            if (rva < section.virtualAddress) continue;
            const quint64 delta = quint64(rva) - section.virtualAddress;
            if (delta >= std::max(section.virtualSize, section.rawSize)) continue;
            return delta <= section.rawSize && length <= section.rawSize - delta ? offset : -1;
        }
        return quint64(rva) < quint64(headers) &&
                       length <= quint64(headers) - rva ? offset : -1;
    };
    const qsizetype directory = mappedRange(directoryRva, kExportDirectorySize);
    if (directory < 0) return refuse(QStringLiteral("export directory is outside file-backed data"));
    const quint32 functionCount = u32(bytes, directory + kExportNumberOfFunctionsOffset);
    const quint32 nameCount = u32(bytes, directory + kExportNumberOfNamesOffset);
    if (nameCount == 0) {
        result.ok = true;
        return result;
    }
    const qsizetype functions = mappedRange(u32(bytes, directory + 28), quint64(functionCount) * 4);
    const qsizetype names = mappedRange(u32(bytes, directory + kExportNamePointerRvaOffset),
                                       quint64(nameCount) * 4);
    const qsizetype ordinals = mappedRange(u32(bytes, directory + kExportOrdinalTableRvaOffset),
                                          quint64(nameCount) * 2);
    if (functionCount == 0 || functions < 0 || names < 0 || ordinals < 0)
        return refuse(QStringLiteral("export tables are outside file-backed data"));
    constexpr char target[] = "mpv_client_api_version";
    qsizetype nameBudget = bytes.size();
    for (quint32 i = 0; i < nameCount; ++i) {
        const quint16 ordinal = u16(bytes, ordinals + qsizetype(i) * 2);
        if (ordinal >= functionCount)
            return refuse(QStringLiteral("export ordinal exceeds the function table"));
        const quint32 nameRva = u32(bytes, names + qsizetype(i) * 4);
        const qsizetype name = mappedRange(nameRva, 1);
        if (name < 0) return refuse(QStringLiteral("export name is outside file-backed data"));
        qsizetype capacity = bytes.size() - name;
        bool inSection = false;
        for (const Section& section : sections) {
            if (nameRva < section.virtualAddress) continue;
            const quint64 delta = quint64(nameRva) - section.virtualAddress;
            if (delta >= std::max(section.virtualSize, section.rawSize)) continue;
            capacity = std::min(capacity, qsizetype(section.rawSize - delta));
            inSection = true;
            break;
        }
        if (!inSection) capacity = std::min(capacity, headers - qsizetype(nameRva));
        qsizetype length = 0;
        while (length < capacity && bytes.at(name + length) != '\0') {
            if (nameBudget-- == 0)
                return refuse(QStringLiteral("export names exceed the scan budget"));
            ++length;
        }
        if (length == capacity || length == 0)
            return refuse(QStringLiteral("export name is unterminated or empty"));
        if (length != sizeof(target) - 1 ||
            QByteArrayView(bytes.constData() + name, length) != QByteArrayView(target))
            continue;
        const quint32 functionRva = u32(bytes, functions + qsizetype(ordinal) * 4);
        if (functionRva == 0 || mappedRange(functionRva, 1) < 0)
            return refuse(QStringLiteral("mpv client API export has no file-backed address"));
        // A forwarder is not a second core; its actual target is not proved by this image.
        if (functionRva >= directoryRva &&
            quint64(functionRva) < quint64(directoryRva) + directorySize)
            return refuse(QStringLiteral("mpv client API export is forwarded"));
        result.exportsMpvClientApiVersion = true;
    }
    result.ok = true;
    return result;
}

bool isSystemImportException(const QString& dllName) {
    if (dllName.isEmpty()) return false;
    const QString lowered = dllName.toLower();
    if (lowered.startsWith(QLatin1String("api-ms-win-")))
        return true;
    if (lowered.startsWith(QLatin1String("ext-ms-win-"))) return true;
    return systemImportExceptions().contains(lowered);
}

}  // namespace native
}  // namespace mplayer