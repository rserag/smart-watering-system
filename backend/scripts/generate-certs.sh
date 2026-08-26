#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CERT_DIR="${BACKEND_DIR}/certs"
LAN_IP=""
LAN_HOSTNAME="$(hostname)"
FORCE=0
NEW_CA=0

usage() {
  echo "Usage: $0 [--ip LAN_IP] [--hostname LOCAL_HOSTNAME] [--force] [--new-ca]"
  echo "  --force renews the server certificate while preserving the CA."
  echo "  --new-ca rotates both CA and server certificate."
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ip) LAN_IP="${2:-}"; shift 2 ;;
    --hostname) LAN_HOSTNAME="${2:-}"; shift 2 ;;
    --force) FORCE=1; shift ;;
    --new-ca) NEW_CA=1; FORCE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

detect_lan_ip() {
  if command -v ipconfig >/dev/null 2>&1; then
    local interface
    interface="$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')"
    [[ -n "${interface}" ]] && ipconfig getifaddr "${interface}" 2>/dev/null && return
  fi
  command -v ip >/dev/null 2>&1 && ip route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src") {print $(i+1); exit}}'
}

[[ -n "${LAN_IP}" ]] || LAN_IP="$(detect_lan_ip)"
if [[ ! "${LAN_IP}" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
  echo "Could not detect a LAN IPv4 address. Supply one with --ip." >&2
  exit 1
fi

mkdir -p "${CERT_DIR}"
umask 077
CA_KEY="${CERT_DIR}/ca.key"; CA_CERT="${CERT_DIR}/ca.crt"
SERVER_KEY="${CERT_DIR}/server.key"; SERVER_CERT="${CERT_DIR}/server.crt"; SERVER_CSR="${CERT_DIR}/server.csr"
if [[ -e "${SERVER_KEY}" || -e "${SERVER_CERT}" ]] && [[ "${FORCE}" -ne 1 ]]; then
  echo "Server certificate already exists. Use --force to renew it." >&2
  exit 1
fi

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TEMP_DIR}"' EXIT
cat >"${TEMP_DIR}/ca.cnf" <<EOF
[req]
prompt = no
distinguished_name = dn
x509_extensions = v3_ca
[dn]
CN = Watering System Local CA
O = Watering System
[v3_ca]
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always,issuer
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,keyCertSign,cRLSign
EOF

if [[ ! -e "${CA_KEY}" || "${NEW_CA}" -eq 1 ]]; then
  openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "${CA_KEY}"
  openssl req -x509 -new -sha256 -days 3650 -key "${CA_KEY}" -config "${TEMP_DIR}/ca.cnf" -out "${CA_CERT}"
fi

cat >"${TEMP_DIR}/server.cnf" <<EOF
[req]
prompt = no
distinguished_name = dn
req_extensions = req_ext
[dn]
CN = ${LAN_IP}
O = Watering System
[req_ext]
subjectAltName = @alt_names
[server_ext]
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer:always
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names
[alt_names]
IP.1 = ${LAN_IP}
IP.2 = 127.0.0.1
DNS.1 = localhost
DNS.2 = ${LAN_HOSTNAME}
EOF

openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "${SERVER_KEY}"
openssl req -new -sha256 -key "${SERVER_KEY}" -config "${TEMP_DIR}/server.cnf" -out "${SERVER_CSR}"
openssl x509 -req -sha256 -days 825 -in "${SERVER_CSR}" -CA "${CA_CERT}" -CAkey "${CA_KEY}" -CAserial "${TEMP_DIR}/ca.srl" -CAcreateserial -extfile "${TEMP_DIR}/server.cnf" -extensions server_ext -out "${SERVER_CERT}"
rm -f "${SERVER_CSR}"
chmod 600 "${CA_KEY}" "${SERVER_KEY}"; chmod 644 "${CA_CERT}" "${SERVER_CERT}"
openssl verify -CAfile "${CA_CERT}" "${SERVER_CERT}"
echo "Created certificate for ${LAN_IP}, localhost, and ${LAN_HOSTNAME}"

