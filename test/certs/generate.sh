#!/bin/bash
# Generate self-signed test certificates for TLS tests.
# Uses ECDSA P-256 (secp256r1) for PicoTLS minicrypto compatibility.
# Keys are in PKCS#8 format ("BEGIN PRIVATE KEY") as required by PicoTLS.
# Not part of the build — run manually if certs need regenerating.
set -euo pipefail
cd "$(dirname "$0")"

# CA (100-year validity, ECDSA P-256, PKCS#8 key)
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:prime256v1 -out ca.key

openssl req -x509 -new -key ca.key -nodes \
    -out ca.crt \
    -days 36500 -subj "/CN=CSP Test CA"

# Server cert signed by CA (CN=localhost, SAN=127.0.0.1,::1, ECDSA P-256)
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:prime256v1 -out server.key

openssl req -new -key server.key -nodes \
    -out server.csr \
    -subj "/CN=localhost"

openssl x509 -req -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days 36500 \
    -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1")

rm -f server.csr ca.srl
echo "Generated: ca.crt ca.key server.crt server.key (ECDSA P-256, PKCS#8)"
