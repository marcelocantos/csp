#!/bin/bash
# Generate self-signed test certificates for TLS tests.
# Not part of the build — run manually if certs need regenerating.
set -euo pipefail
cd "$(dirname "$0")"

# CA (100-year validity)
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout ca.key -out ca.crt \
    -days 36500 -subj "/CN=CSP Test CA"

# Server cert signed by CA (CN=localhost, SAN=127.0.0.1,::1)
openssl req -newkey rsa:2048 -nodes \
    -keyout server.key -out server.csr \
    -subj "/CN=localhost"

openssl x509 -req -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days 36500 \
    -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1")

rm -f server.csr ca.srl
echo "Generated: ca.crt ca.key server.crt server.key"
