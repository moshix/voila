#!/usr/bin/env bash
# gen.sh — regenerate the TLS test fixtures (tests/tls). TEST-ONLY KEYS:
# these private keys are public, in the repository, and protect nothing.
#
# The certificates are valid for 100 years so the fixtures do not expire
# under the test suite; the suite never regenerates them, so it needs no
# openssl. Run this by hand (from this directory) only to replace them.
#
#   ca.pem / ca.key            a self-signed test CA (EC P-256)
#   server.pem / server.key    "localhost", EC P-256, signed by the CA;
#                              the key in SEC 1 form ("EC PRIVATE KEY")
#   rsa.pem / rsa.key          "localhost", RSA 2048, signed by the CA;
#                              the key in PKCS#8 form ("PRIVATE KEY")
#   other-ca.pem               a second, unrelated CA: trusting it instead
#                              of ca.pem must make verification fail
#
# and the leaves the verification POLICY must refuse or accept (all with
# server.key, signed by the CA; `./gen.sh policy` makes only these):
#
#   eku.pem      extendedKeyUsage clientAuth only        refused
#   nosan.pem    CN=localhost and no subjectAltName      refused
#   sha1.pem     signed with ecdsa-with-SHA1              refused
#   ku.pem       keyUsage keyCertSign only               refused
#   wild.pem     *.test.localhost, *.localhost, *.com    one label of the first
#   ipsan.pem    IP:127.0.0.1 and IP:::1                 by address
set -euo pipefail
cd "$(dirname "$0")"
days=36500
ext=$(mktemp)
trap 'rm -f "$ext" ./*.csr ./*.srl other-ca.key' EXIT
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > "$ext"

# leaf NAME DIGEST EXTENSION-LINES: a leaf for server.key, signed by the CA.
leaf() {
    printf "$3" > "$ext"
    openssl req -new -key server.key -subj "/CN=localhost" -out "$1.csr"
    openssl x509 -req -in "$1.csr" -CA ca.pem -CAkey ca.key \
        -CAcreateserial "-$2" -days $days -extfile "$ext" -out "$1.pem"
}
policy() {
    leaf eku sha256 'subjectAltName=DNS:localhost\nextendedKeyUsage=clientAuth\n'
    leaf nosan sha256 'basicConstraints=CA:FALSE\n'
    leaf sha1 sha1 'subjectAltName=DNS:localhost\n'
    leaf ku sha256 'subjectAltName=DNS:localhost\nkeyUsage=keyCertSign\n'
    leaf wild sha256 'subjectAltName=DNS:*.test.localhost,DNS:*.localhost,DNS:*.com\n'
    leaf ipsan sha256 'subjectAltName=IP:127.0.0.1,IP:::1\n'
}
if [ "${1:-}" = policy ]; then policy; exit 0; fi

openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days $days \
    -subj "/CN=Voila Test CA" -out ca.pem

openssl ecparam -name prime256v1 -genkey -noout -out server.key
openssl req -new -key server.key -subj "/CN=localhost" -out server.csr
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
    -sha256 -days $days -extfile "$ext" -out server.pem

openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out rsa.key
openssl req -new -key rsa.key -subj "/CN=localhost" -out rsa.csr
openssl x509 -req -in rsa.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
    -sha256 -days $days -extfile "$ext" -out rsa.pem

openssl ecparam -name prime256v1 -genkey -noout -out other-ca.key
openssl req -x509 -new -key other-ca.key -sha256 -days $days \
    -subj "/CN=Voila Other CA" -out other-ca.pem

policy
