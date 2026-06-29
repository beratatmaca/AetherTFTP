#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

/**
 * @file tftp_protocol.h
 * @brief Pure TFTP packet definitions and (de)serialisation.
 *
 * This layer is pure data: no sockets, no Qt event loop. It is responsible
 * only for turning the structures below into wire bytes and back, in strict
 * conformance with:
 *   - RFC 1350 — The TFTP Protocol (Revision 2)
 *   - RFC 2347 — TFTP Option Extension
 *   - RFC 2348 — TFTP Blocksize Option
 *   - RFC 2349 — TFTP Timeout Interval and Transfer Size Options
 */

namespace tftp {

/** @brief TFTP opcodes (RFC 1350 §5; OACK from RFC 2347). */
enum class OpCode : quint8 {
    RRQ = 1,    ///< Read request.
    WRQ = 2,    ///< Write request.
    DATA = 3,   ///< Data block.
    ACK = 4,    ///< Acknowledgement.
    ERROR = 5,  ///< Error.
    OACK = 6,   ///< Option acknowledgement (RFC 2347).
};

/** @brief TFTP error codes (RFC 1350 §5; OptionRefused from RFC 2347). */
enum class ErrorCode : quint8 {
    NotDefined = 0,  ///< Not defined; see the error message (if any).
    FileNotFound = 1,
    AccessViolation = 2,
    DiskFull = 3,
    IllegalOperation = 4,
    UnknownTransferId = 5,
    FileAlreadyExists = 6,
    NoSuchUser = 7,
    OptionRefused = 8,  ///< Terminate option negotiation (RFC 2347).
};

constexpr const char *kModeOctet = "octet";        ///< Octet (binary) mode, RFC 1350 §1.
constexpr const char *kModeNetascii = "netascii";  ///< netascii mode (out of scope).

constexpr const char *kOptBlksize = "blksize";  ///< Blocksize option key (RFC 2348).
constexpr const char *kOptTimeout = "timeout";  ///< Timeout option key (RFC 2349).
constexpr const char *kOptTsize = "tsize";      ///< Transfer-size option key (RFC 2349).

constexpr int kDefaultBlockSize = 512;  ///< RFC 1350 fixed block size.
constexpr int kMinBlockSize = 8;        ///< RFC 2348 lower bound.
constexpr int kMaxBlockSize = 65464;    ///< RFC 2348 upper bound.
constexpr int kDefaultPort = 69;        ///< IANA well-known TFTP port.

/**
 * @brief Ordered option set with lowercase keys.
 *
 * Insertion order is preserved so an OACK echoes options back in a stable,
 * testable sequence. Keys must always be stored lowercased (RFC 2347 §2).
 */
using Options = QMap<QString, QString>;

/** @brief A parsed read or write request (RRQ or WRQ). */
struct Request {
    OpCode op = OpCode::RRQ;  ///< RRQ or WRQ only.
    QString filename;         ///< Requested file name.
    QString mode;             ///< Lowercased transfer mode (e.g. "octet").
    Options options;          ///< Negotiated options (may be empty).
};

/// @name Serialisation (structure : wire datagram)
/// @{

/**
 * @brief Serialise an RRQ or WRQ datagram.
 * @param op RRQ or WRQ.
 * @param filename File name field.
 * @param mode Transfer mode string (e.g. "octet").
 * @param options Optional RFC 2347 option set to append.
 * @return The ready-to-send datagram payload.
 */
QByteArray buildRequest(OpCode op, const QString &filename, const QString &mode, const Options &options = {});

/**
 * @brief Serialise a DATA packet.
 * @param block Block number (1-based, wraps at 65535).
 * @param payload Block payload (0..blocksize bytes).
 * @return The DATA datagram.
 */
QByteArray buildData(quint16 block, const QByteArray &payload);

/**
 * @brief Serialise an ACK packet.
 * @param block Block number being acknowledged.
 * @return The ACK datagram.
 */
QByteArray buildAck(quint16 block);

/**
 * @brief Serialise an ERROR packet.
 * @param code TFTP error code.
 * @param message Human-readable error string.
 * @return The ERROR datagram.
 */
QByteArray buildError(ErrorCode code, const QString &message);

/**
 * @brief Serialise an OACK packet (RFC 2347).
 * @param options Accepted option set to echo back.
 * @return The OACK datagram.
 */
QByteArray buildOack(const Options &options);

/// @}
/// @name Parsing (wire datagram : structure)
/// @{

/**
 * @brief Read a datagram's opcode without a full parse.
 * @param datagram Raw datagram bytes.
 * @param[out] outOp Receives the opcode on success.
 * @return @c true if the opcode is valid and in range.
 */
bool peekOpCode(const QByteArray &datagram, OpCode &outOp);

/**
 * @brief Parse an RRQ or WRQ datagram, including any options.
 * @param datagram Raw datagram bytes.
 * @param[out] out Receives the parsed request on success.
 * @return @c true on success, @c false if malformed.
 */
bool parseRequest(const QByteArray &datagram, Request &out);

/**
 * @brief Parse a DATA packet.
 * @param datagram Raw datagram bytes.
 * @param[out] outBlock Receives the block number.
 * @param[out] outPayload Receives the block payload.
 * @return @c true on success, @c false if malformed.
 */
bool parseData(const QByteArray &datagram, quint16 &outBlock, QByteArray &outPayload);

/**
 * @brief Parse an ACK packet.
 * @param datagram Raw datagram bytes.
 * @param[out] outBlock Receives the acknowledged block number.
 * @return @c true on success, @c false if malformed.
 */
bool parseAck(const QByteArray &datagram, quint16 &outBlock);

/**
 * @brief Parse an ERROR packet.
 * @param datagram Raw datagram bytes.
 * @param[out] outCode Receives the error code.
 * @param[out] outMessage Receives the error message (may be empty).
 * @return @c true on success, @c false if malformed.
 */
bool parseError(const QByteArray &datagram, ErrorCode &outCode, QString &outMessage);

/**
 * @brief Parse an OACK packet's option set (RFC 2347).
 * @param datagram Raw datagram bytes.
 * @param[out] outOptions Receives the parsed options (lowercase keys).
 * @return @c true on success, @c false if malformed.
 */
bool parseOack(const QByteArray &datagram, Options &outOptions);

/// @}

/**
 * @brief Clamp a requested block size into the RFC 2348 valid range.
 * @param requested Requested block size in bytes.
 * @return @p requested clamped to [kMinBlockSize, kMaxBlockSize].
 */
int clampBlockSize(int requested);

}  // namespace tftp
