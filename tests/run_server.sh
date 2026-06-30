#!/usr/bin/env bash
#
# run_server.sh — launch a tftp-hpa (in.tftpd) server peer for GUI testing.
#
# Starts the standard HPA TFTP daemon in foreground mode so the AetherTFTP GUI
# (acting as a client) can download from / upload to a known-good, independent
# implementation. Here tftp-hpa is the SERVER and the GUI is the CLIENT.
#
# Layout (two folders, so you can see what came from where):
#   <base>/tftp     served by in.tftpd (chroot). Holds the tftp-side payload
#                   the GUI downloads; GUI uploads land here.
#   <base>/aether   the GUI client's local working folder: save downloads here
#                   and pick uploads from here.
#
# One big, uniquely-named payload file is generated in tftp/ on each run.
#
# in.tftpd drops privileges (setgroups/setuid) on every request and chroots
# with --secure, so it MUST run as root. This script re-runs ITSELF under sudo
# when you are not already root (you will be asked for your password).
#
# Signals (Ctrl+C / kill) are trapped: the script echoes the directive it
# received, stops the daemon, and reports the final folder contents.
#
# Usage:
#   ./run_server.sh [-p PORT] [-d BASE_DIR] [-b BIND] [-h]
#
#   -p PORT      Listen port             (default: 6969; 69 also fine as root)
#   -d BASE_DIR  Sandbox base directory   (default: tests/sandbox_server)
#   -b BIND      Bind address            (default: 0.0.0.0)
#   -h           Show this help and exit
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORIG_ARGS=("$@")   # preserved so we can re-exec verbatim under sudo

# ---- Defaults --------------------------------------------------------------
PORT=6969
BASE_DIR="${SCRIPT_DIR}/sandbox_server"
BIND="0.0.0.0"
PAYLOAD_SIZE=5242880   # 5 MiB
TFTPD="$(command -v in.tftpd || echo /usr/sbin/in.tftpd)"

usage() {
    sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# ---- Parse arguments -------------------------------------------------------
while getopts ":p:d:b:h" opt; do
    case "${opt}" in
        p) PORT="${OPTARG}" ;;
        d) BASE_DIR="${OPTARG}" ;;
        b) BIND="${OPTARG}" ;;
        h) usage 0 ;;
        :) echo "Error: -${OPTARG} requires an argument" >&2; usage 2 ;;
        \?) echo "Error: unknown option -${OPTARG}" >&2; usage 2 ;;
    esac
done

# ---- Check for the daemon --------------------------------------------------
if [[ ! -x "${TFTPD}" ]]; then
    echo "Error: in.tftpd not found. Install it with:" >&2
    echo "       sudo apt-get install tftpd-hpa" >&2
    exit 1
fi

# ---- Elevate to root -------------------------------------------------------
# in.tftpd cannot serve as an unprivileged user (it fails at "set groups for
# user nobody") and --secure needs to chroot, so the whole script must run as
# root. Re-exec under sudo if we are not already there.
if [[ "$(id -u)" -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "Error: this script must run as root, but 'sudo' is not available." >&2
        echo "       Re-run it as root." >&2
        exit 1
    fi
    echo ">>> Root privileges are required for in.tftpd."
    echo ">>> Re-running under sudo (you may be prompted for your password)..."
    exec sudo bash "${BASH_SOURCE[0]}" "${ORIG_ARGS[@]}"
fi

# From here on we are root. SUDO_USER is the human who invoked us (if any), so
# we can hand the sandbox back to them afterwards.
OWNER="${SUDO_USER:-root}"

# ---- Prepare the two folders ------------------------------------------------
BASE_DIR="$(cd "$(dirname "${BASE_DIR}")" 2>/dev/null && pwd)/$(basename "${BASE_DIR}")" || BASE_DIR="${SCRIPT_DIR}/sandbox_server"
TFTP_DIR="${BASE_DIR}/tftp"
AETHER_DIR="${BASE_DIR}/aether"
mkdir -p "${TFTP_DIR}" "${AETHER_DIR}"
touch "${TFTP_DIR}/.gitkeep" "${AETHER_DIR}/.gitkeep"

# ---- Generate one big, uniquely-named payload in tftp/ ----------------------
# Clear any previous payload so only the freshly-named one remains.
rm -f "${TFTP_DIR}"/tftp_payload_*.bin 2>/dev/null
PAYLOAD="tftp_payload_$(date +%Y%m%d-%H%M%S).bin"
head -c "${PAYLOAD_SIZE}" /dev/urandom > "${TFTP_DIR}/${PAYLOAD}"

# Daemon writes uploads as user "nobody" after chroot; make the served
# directory world-writable so WRQ uploads succeed, then hand both folders
# back to the invoking user.
chmod 0777 "${TFTP_DIR}" 2>/dev/null || true
chown -R "${OWNER}":"${OWNER}" "${BASE_DIR}" 2>/dev/null || true

# ---- Connection details ----------------------------------------------------
LAN_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
LAN_IP="${LAN_IP:-<this-machine-ip>}"
HUMAN_SIZE="$(numfmt --to=iec "${PAYLOAD_SIZE}" 2>/dev/null || echo "${PAYLOAD_SIZE} bytes")"

# ---- Signal handling -------------------------------------------------------
SERVER_PID=""
shutdown() {
    local sig="$1"
    echo ""
    echo ">>> Directive received: ${sig} — shutting down in.tftpd."
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null
        wait "${SERVER_PID}" 2>/dev/null
    fi
    echo ">>> tftp/   (served — downloads come from here, uploads arrive here):"
    ls -la "${TFTP_DIR}"
    echo ">>> aether/ (GUI client's local folder):"
    ls -la "${AETHER_DIR}"
    echo ">>> Server stopped."
    exit 0
}
trap 'shutdown SIGINT'  INT
trap 'shutdown SIGTERM' TERM
trap 'shutdown SIGHUP'  HUP

# ---- Build the daemon command ----------------------------------------------
# Flags:
#   -L            foreground, log to stderr (don't detach)
#   -v            verbose logging
#   -c            allow creation of new files (needed for uploads / WRQ)
#   --secure DIR  chroot into DIR; clients then use bare filenames
#   -a addr:port  bind address and port
CMD=("${TFTPD}" -L -v -c --secure "${TFTP_DIR}" -a "${BIND}:${PORT}")

# ---- Instructions ----------------------------------------------------------
cat <<EOF

==================================================================
 tftp-hpa SERVER peer is starting  (tftp-hpa = server, GUI = client)
==================================================================
 Listening on : ${BIND}:${PORT}
 Serving (tftp/) : ${TFTP_DIR}
 GUI local (aether/) : ${AETHER_DIR}
 Payload      : tftp/${PAYLOAD}  (${HUMAN_SIZE})

 WHERE TO CONNECT (in the AetherTFTP GUI):
   1. Open the GUI and select the "Client" tab.
   2. Host: 127.0.0.1   (or ${LAN_IP} from another machine)
      Port: ${PORT}
   3. RECEIVE : Download "${PAYLOAD}" and save it into:
                  ${AETHER_DIR}
                (Now you can see the tftp-side file land in aether/.)
   4. TRANSMIT: Upload any file from aether/. It will appear in:
                  ${TFTP_DIR}

 Stop this server with Ctrl+C.
==================================================================
EOF
echo ">>> ${CMD[*]}"

"${CMD[@]}" &
SERVER_PID=$!

# Wait for the daemon; the signal traps handle clean teardown.
wait "${SERVER_PID}"
