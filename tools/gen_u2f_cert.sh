#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'Usage: %s <directory-for-u2f-assets>\n' "$0" >&2
    exit 2
fi

output_dir=$1
mkdir -p "$output_dir"
temporary_dir=$(mktemp -d)
trap 'rm -f "$temporary_dir/attestation-key.pem" "$temporary_dir/attestation-key.der" "$temporary_dir/req.cnf"; rmdir "$temporary_dir"' EXIT

cat >"$temporary_dir/req.cnf" <<'EOF'
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no

[dn]
CN = ESP32 U2F Authenticator

[ext]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
subjectKeyIdentifier = hash
EOF

openssl genpkey \
    -algorithm EC \
    -pkeyopt ec_paramgen_curve:prime256v1 \
    -out "$temporary_dir/attestation-key.pem"
openssl req \
    -new \
    -x509 \
    -sha256 \
    -days 3650 \
    -key "$temporary_dir/attestation-key.pem" \
    -config "$temporary_dir/req.cnf" \
    -outform DER \
    -out "$output_dir/cert.der"
openssl ec \
    -in "$temporary_dir/attestation-key.pem" \
    -outform DER \
    -out "$temporary_dir/attestation-key.der"

python_bin=${PYTHON:-python3}
"$python_bin" - "$temporary_dir/attestation-key.der" "$output_dir/cert_key.u2f" <<'PY'
import pathlib
import sys


def read_tlv(data, offset, expected_tag):
    if offset >= len(data) or data[offset] != expected_tag:
        raise ValueError("Unexpected ASN.1 tag")
    offset += 1
    first_length = data[offset]
    offset += 1
    if first_length < 0x80:
        length = first_length
    else:
        length_bytes = first_length & 0x7F
        if not length_bytes or offset + length_bytes > len(data):
            raise ValueError("Invalid ASN.1 length")
        length = int.from_bytes(data[offset : offset + length_bytes], "big")
        offset += length_bytes
    end = offset + length
    if end > len(data):
        raise ValueError("Truncated ASN.1 value")
    return data[offset:end], end


der = pathlib.Path(sys.argv[1]).read_bytes()
sequence, sequence_end = read_tlv(der, 0, 0x30)
if sequence_end != len(der):
    raise ValueError("Trailing data in EC private key")
_, offset = read_tlv(sequence, 0, 0x02)
private_scalar, offset = read_tlv(sequence, offset, 0x04)
if len(private_scalar) > 32:
    raise ValueError("P-256 private scalar exceeds 32 bytes")
private_scalar = private_scalar.rjust(32, b"\0")

hex_data = " ".join(f"{value:02X}" for value in private_scalar)
pathlib.Path(sys.argv[2]).write_text(
    "Filetype: Flipper U2F Certificate Key File\n"
    "Version: 1\n"
    "Type: 2\n"
    f"Data: {hex_data}\n",
    encoding="ascii",
)
PY

printf 'Wrote cert.der and cert_key.u2f to %s\n' "$output_dir"
