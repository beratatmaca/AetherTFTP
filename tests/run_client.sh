#!/usr/bin/env bash
#
# run_client.sh — interactive tftp-hpa (tftp) client peer for GUI testing.
#
# Drives the standard HPA TFTP client so you can transmit (put) files to, and
# receive (get) files from, the AetherTFTP GUI acting as a server. Here
# tftp-hpa is the CLIENT and the GUI is the SERVER.
#
# Layout (two folders, so you can see what came from where):
#   <base>/tftp     the tftp client's local working folder. The payload it
#                   uploads lives here; downloads (get) are saved here.
#   <base>/aether   the GUI server's Root Directory: point the GUI at this so
#                   files the client uploads appear here.
#
# One big, uniquely-named payload file is generated in tftp/ on each run.
# The TFTP client needs no special privileges, so this script does NOT require
# root.
#
# Signals (Ctrl+C / kill) are trapped: the script echoes the directive it
# received and exits cleanly.
#
# Usage:
#   ./run_client.sh [-H HOST] [-p PORT] [-d BASE_DIR] [-h]
#
#   -H HOST      Server host             (default: 127.0.0.1)
#   -p PORT      Server port             (default: 6969)
#   -d BASE_DIR  Sandbox base directory   (default: tests/sandbox_client)
#   -h           Show this help and exit
#
# REPL commands:
#   put <localfile> [remotename]   Upload a file (transmit) from tftp/
#   get <remotefile> [localpath]   Download a file (receive) into tftp/
#   host <addr>                    Change the target host
#   port <n>                       Change the target port
#   ls                             List both folders
#   help                           Show command help
#   quit | exit                    Leave the client
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---- Defaults --------------------------------------------------------------
HOST="127.0.0.1"
PORT=6969
BASE_DIR="${SCRIPT_DIR}/sandbox_client"
PAYLOAD_SIZE=5242880   # 5 MiB
TFTP="$(command -v tftp || echo /usr/bin/tftp)"

usage() {
    sed -n '2,42p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

# ---- Parse arguments -------------------------------------------------------
while getopts ":H:p:d:h" opt; do
    case "${opt}" in
        H) HOST="${OPTARG}" ;;
        p) PORT="${OPTARG}" ;;
        d) BASE_DIR="${OPTARG}" ;;
        h) usage 0 ;;
        :) echo "Error: -${OPTARG} requires an argument" >&2; usage 2 ;;
        \?) echo "Error: unknown option -${OPTARG}" >&2; usage 2 ;;
    esac
done

# ---- Check for the client --------------------------------------------------
if [[ ! -x "${TFTP}" ]]; then
    echo "Error: tftp client not found. Install it with:" >&2
    echo "       sudo apt-get install tftp-hpa" >&2
    exit 1
fi

# ---- Prepare the two folders ------------------------------------------------
BASE_DIR="$(cd "$(dirname "${BASE_DIR}")" 2>/dev/null && pwd)/$(basename "${BASE_DIR}")" || BASE_DIR="${SCRIPT_DIR}/sandbox_client"
TFTP_DIR="${BASE_DIR}/tftp"
AETHER_DIR="${BASE_DIR}/aether"
mkdir -p "${TFTP_DIR}" "${AETHER_DIR}"
touch "${TFTP_DIR}/.gitkeep" "${AETHER_DIR}/.gitkeep"

# ---- Generate one big, uniquely-named payload in tftp/ ----------------------
rm -f "${TFTP_DIR}"/tftp_payload_*.bin 2>/dev/null
PAYLOAD="tftp_payload_$(date +%Y%m%d-%H%M%S).bin"
head -c "${PAYLOAD_SIZE}" /dev/urandom > "${TFTP_DIR}/${PAYLOAD}"
HUMAN_SIZE="$(numfmt --to=iec "${PAYLOAD_SIZE}" 2>/dev/null || echo "${PAYLOAD_SIZE} bytes")"

# ---- Signal handling -------------------------------------------------------
on_signal() {
    local sig="$1"
    echo ""
    echo ">>> Directive received: ${sig} — exiting client."
    echo ">>> tftp/ (client local): ${TFTP_DIR}"
    echo ">>> aether/ (GUI root)  : ${AETHER_DIR}"
    exit 0
}
trap 'on_signal SIGINT'  INT
trap 'on_signal SIGTERM' TERM
trap 'on_signal SIGHUP'  HUP

# ---- Transfer helpers ------------------------------------------------------
# tftp-hpa one-shot syntax: tftp HOST PORT -m octet -c <put|get> <args...>
do_put() {
    local local_file="$1" remote="${2:-}"
    # Resolve a bare name against the tftp/ folder for convenience.
    [[ -e "${local_file}" ]] || local_file="${TFTP_DIR}/${local_file}"
    if [[ ! -f "${local_file}" ]]; then
        echo "Error: local file not found: ${local_file}" >&2
        return 1
    fi
    # Always pass an explicit remote name (basename by default). tftp-hpa's
    # single-argument 'put <abspath>' form misparses an absolute local path and
    # fails with "No such file or directory"; the two-argument form works.
    [[ -n "${remote}" ]] || remote="$(basename "${local_file}")"
    echo ">>> Transmit (put): ${local_file} -> ${HOST}:${PORT}/${remote}"
    "${TFTP}" "${HOST}" "${PORT}" -m octet -c put "${local_file}" "${remote}"
}

do_get() {
    local remote="$1" dest="${2:-}"
    # Default the local destination into tftp/, keeping the basename.
    [[ -n "${dest}" ]] || dest="${TFTP_DIR}/$(basename "${remote}")"
    echo ">>> Receive (get): ${HOST}:${PORT}/${remote} -> ${dest}"
    "${TFTP}" "${HOST}" "${PORT}" -m octet -c get "${remote}" "${dest}"
}

print_help() {
    cat <<'EOF'
Commands:
  put <localfile> [remotename]   Upload a file (transmit) from tftp/
  get <remotefile> [localpath]   Download a file (receive) into tftp/
  host <addr>                    Change the target host
  port <n>                       Change the target port
  ls                             List both folders
  help                           Show this help
  quit | exit                    Leave the client
EOF
}

# ---- Instructions ----------------------------------------------------------
cat <<EOF

==================================================================
 tftp-hpa CLIENT peer  (tftp-hpa = client, GUI = server)
==================================================================
 Target server : ${HOST}:${PORT}   (change with 'host' / 'port')
 tftp/  (local): ${TFTP_DIR}
 aether/ (GUI root): ${AETHER_DIR}
 Payload       : tftp/${PAYLOAD}  (${HUMAN_SIZE})

 WHAT TO DO (test against the AetherTFTP GUI as a server):
   1. In the GUI, select the "Server" tab.
   2. Set Root Directory to EXACTLY the aether/ folder:
        ${AETHER_DIR}
      Set Port to ${PORT}, then click "Start Server".
   3. Back here:
        put ${PAYLOAD}
            -> transmits the payload; watch it appear in aether/.
        get <name>
            -> receives a file the GUI serves (from aether/) into tftp/.

 Direction matters:
   - 'put' pushes a tftp/ file TO the GUI's root (aether/).
   - 'get' pulls a file FROM the GUI's root (aether/) into tftp/.

 Type 'help' for all commands, Ctrl+C (or 'quit') to exit.
==================================================================
EOF

# ---- REPL ------------------------------------------------------------------
while true; do
    # -r: don't mangle backslashes. Read failure (EOF / Ctrl+D) ends the loop.
    if ! read -r -e -p "tftp(${HOST}:${PORT})> " line; then
        echo ""
        on_signal EOF
    fi
    # shellcheck disable=SC2206  # intentional word-split into argv
    args=(${line})
    cmd="${args[0]:-}"
    case "${cmd}" in
        "")        ;;
        put)       do_put  "${args[1]:-}" "${args[2]:-}" ;;
        get)       do_get  "${args[1]:-}" "${args[2]:-}" ;;
        host)      HOST="${args[1]:-$HOST}"; echo ">>> host set to ${HOST}" ;;
        port)      PORT="${args[1]:-$PORT}"; echo ">>> port set to ${PORT}" ;;
        ls)        echo "-- tftp/ --";   ls -la "${TFTP_DIR}";
                   echo "-- aether/ --"; ls -la "${AETHER_DIR}" ;;
        help|\?)   print_help ;;
        quit|exit) echo ">>> Bye."; exit 0 ;;
        *)         echo "Unknown command: ${cmd} (type 'help')" >&2 ;;
    esac
done
