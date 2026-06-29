#include "core/tftp_protocol.h"

#include <QDataStream>
#include <QIODevice>

namespace tftp {

namespace {

// Appends a NUL-terminated string field (RFC 1350 uses zero-terminated ASCII).
void appendCString(QByteArray &out, const QString &value) {
    out.append(value.toUtf8());
    out.append('\0');
}

// Appends the "key\0value\0" pairs that make up the option block shared by
// RRQ/WRQ (RFC 2347) and OACK.
void appendOptions(QByteArray &out, const Options &options) {
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        appendCString(out, it.key());
        appendCString(out, it.value());
    }
}

// Splits a buffer (starting at offset) into NUL-terminated tokens. Returns the
// list of fields; sets ok=false if a field is unterminated.
QList<QByteArray> splitCStrings(const QByteArray &buf, int offset, bool &ok) {
    QList<QByteArray> fields;
    ok = true;
    int start = offset;
    while (start < buf.size()) {
        int nul = buf.indexOf('\0', start);
        if (nul < 0) {
            ok = false;  // Unterminated trailing field.
            break;
        }
        fields.append(buf.mid(start, nul - start));
        start = nul + 1;
    }
    return fields;
}

quint16 readU16(const QByteArray &buf, int offset) {
    return (static_cast<quint8>(buf[offset]) << 8) | static_cast<quint8>(buf[offset + 1]);
}

void appendU16(QByteArray &out, quint16 value) {
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

}  // namespace

QByteArray buildRequest(OpCode op, const QString &filename, const QString &mode, const Options &options) {
    QByteArray out;
    appendU16(out, static_cast<quint16>(op));
    appendCString(out, filename);
    appendCString(out, mode);
    appendOptions(out, options);
    return out;
}

QByteArray buildData(quint16 block, const QByteArray &payload) {
    QByteArray out;
    appendU16(out, static_cast<quint16>(OpCode::DATA));
    appendU16(out, block);
    out.append(payload);
    return out;
}

QByteArray buildAck(quint16 block) {
    QByteArray out;
    appendU16(out, static_cast<quint16>(OpCode::ACK));
    appendU16(out, block);
    return out;
}

QByteArray buildError(ErrorCode code, const QString &message) {
    QByteArray out;
    appendU16(out, static_cast<quint16>(OpCode::ERROR));
    appendU16(out, static_cast<quint16>(code));
    appendCString(out, message);
    return out;
}

QByteArray buildOack(const Options &options) {
    QByteArray out;
    appendU16(out, static_cast<quint16>(OpCode::OACK));
    appendOptions(out, options);
    return out;
}

bool peekOpCode(const QByteArray &datagram, OpCode &outOp) {
    if (datagram.size() < 2)
        return false;
    quint16 raw = readU16(datagram, 0);
    if (raw < static_cast<quint16>(OpCode::RRQ) || raw > static_cast<quint16>(OpCode::OACK))
        return false;
    outOp = static_cast<OpCode>(raw);
    return true;
}

bool parseRequest(const QByteArray &datagram, Request &out) {
    if (datagram.size() < 2)
        return false;
    quint16 raw = readU16(datagram, 0);
    if (raw != static_cast<quint16>(OpCode::RRQ) && raw != static_cast<quint16>(OpCode::WRQ))
        return false;
    out.op = static_cast<OpCode>(raw);

    bool ok = false;
    const QList<QByteArray> fields = splitCStrings(datagram, 2, ok);
    if (!ok || fields.size() < 2)
        return false;  // Need at least filename + mode.

    out.filename = QString::fromUtf8(fields[0]);
    out.mode = QString::fromUtf8(fields[1]).toLower();

    // Remaining fields are option key/value pairs. A trailing unpaired key is
    // ignored (treated as absent) rather than rejected.
    out.options.clear();
    for (int i = 2; i + 1 < fields.size(); i += 2) {
        out.options.insert(QString::fromUtf8(fields[i]).toLower(), QString::fromUtf8(fields[i + 1]));
    }
    return true;
}

bool parseData(const QByteArray &datagram, quint16 &outBlock, QByteArray &outPayload) {
    if (datagram.size() < 4)
        return false;
    if (readU16(datagram, 0) != static_cast<quint16>(OpCode::DATA))
        return false;
    outBlock = readU16(datagram, 2);
    outPayload = datagram.mid(4);
    return true;
}

bool parseAck(const QByteArray &datagram, quint16 &outBlock) {
    if (datagram.size() < 4)
        return false;
    if (readU16(datagram, 0) != static_cast<quint16>(OpCode::ACK))
        return false;
    outBlock = readU16(datagram, 2);
    return true;
}

bool parseError(const QByteArray &datagram, ErrorCode &outCode, QString &outMessage) {
    if (datagram.size() < 4)
        return false;
    if (readU16(datagram, 0) != static_cast<quint16>(OpCode::ERROR))
        return false;
    outCode = static_cast<ErrorCode>(readU16(datagram, 2));
    bool ok = false;
    const QList<QByteArray> fields = splitCStrings(datagram, 4, ok);
    outMessage = fields.isEmpty() ? QString() : QString::fromUtf8(fields[0]);
    return true;
}

bool parseOack(const QByteArray &datagram, Options &outOptions) {
    if (datagram.size() < 2)
        return false;
    if (readU16(datagram, 0) != static_cast<quint16>(OpCode::OACK))
        return false;
    bool ok = false;
    const QList<QByteArray> fields = splitCStrings(datagram, 2, ok);
    if (!ok)
        return false;
    outOptions.clear();
    for (int i = 0; i + 1 < fields.size(); i += 2) {
        outOptions.insert(QString::fromUtf8(fields[i]).toLower(), QString::fromUtf8(fields[i + 1]));
    }
    return true;
}

int clampBlockSize(int requested) {
    if (requested < kMinBlockSize)
        return kMinBlockSize;
    if (requested > kMaxBlockSize)
        return kMaxBlockSize;
    return requested;
}

}  // namespace tftp
