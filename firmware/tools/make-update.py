"""Make a signed FreeMesh update bundle (the files a box downloads to update itself).

Run with the ESP-IDF python (it has the "cryptography" package), from the firmware folder:

  python tools/make-update.py keygen
      Creates the signing key once and writes its public half to main/app_ota_key.h.
      The secret half stays outside the project (default: <home>/.freemesh/update-signing-key.pem).
      Anyone who has that file can make updates that every box accepts: back it up and never share it.

  python tools/make-update.py bundle --number 2 --freemesh build/firmware.bin=0.10.1 ^
         --meshtastic tools/bins/meshtastic-freemesh.bin=2.8.3 --meshcore tools/bins/meshcore-freemesh.bin=d2096bd
      Copies the three programs into build/update/ and writes the signed list "update.txt" next to them.
      --number must go up with every published bundle (boxes refuse a lower number).

  python tools/make-update.py serve
      For testing only: serves build/update/ on this PC (http://<this PC>:8080/update.txt).

The list (update.txt) is plain text, one program per line:
  freemesh-update 1 <number>
  <program> <version> <size in bytes> <sha256> <file name>
  sig <ECDSA P-256 signature of everything above, as 128 hex digits (r then s)>
"""
import argparse
import functools
import hashlib
import http.server
import os
import re
import shutil
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)
DEFAULT_KEY = os.path.join(os.path.expanduser("~"), ".freemesh", "update-signing-key.pem")
DEFAULT_OUT = os.path.join(FIRMWARE, "build", "update")
KEY_HEADER = os.path.join(FIRMWARE, "main", "app_ota_key.h")
SLOT_SIZE = {"freemesh": 0x200000, "meshtastic": 0x280000, "meshcore": 0x200000}   # Must match partitions.csv.
SAFE = re.compile(r"^[A-Za-z0-9._-]+$")


def load_key(path):
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def public_bytes(key):
    return key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def write_header(pub):
    rows = []
    for i in range(0, len(pub), 13):
        rows.append("    " + ", ".join("0x%02x" % b for b in pub[i:i + 13]) + ",")
    text = (
        "// The PUBLIC half of the update signing key. Made by tools/make-update.py keygen. Safe to share.\n"
        "// The box only installs an update list signed with the matching secret half (kept off this project).\n"
        "#pragma once\n#include <stdint.h>\n\n"
        "static const uint8_t OTA_PUBKEY[65] = {\n" + "\n".join(rows) + "\n};\n"
    )
    with open(KEY_HEADER, "w", newline="\n") as f:
        f.write(text)


def cmd_keygen(a):
    if os.path.exists(a.key):
        key = load_key(a.key)
        print("Key already exists, kept it:", a.key)
    else:
        os.makedirs(os.path.dirname(a.key), exist_ok=True)
        key = ec.generate_private_key(ec.SECP256R1())
        pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption())
        with open(a.key, "wb") as f:
            f.write(pem)
        print("Made a new signing key:", a.key)
        print("BACK IT UP somewhere safe. Without it you cannot update boxes that trust it.")
    write_header(public_bytes(key))
    print("Wrote the public half to", KEY_HEADER)


def cmd_bundle(a):
    key = load_key(a.key)
    os.makedirs(a.out, exist_ok=True)
    lines = ["freemesh-update 1 %d" % a.number]
    for name in ("freemesh", "meshtastic", "meshcore"):
        spec = getattr(a, name)
        if "=" not in spec:
            sys.exit("--%s needs file=version, like build/firmware.bin=0.10.1" % name)
        path, version = spec.rsplit("=", 1)
        if not SAFE.match(version) or len(version) > 23:
            sys.exit("Version '%s': use only letters, digits, dot, dash, underscore (max 23)" % version)
        with open(path, "rb") as f:
            data = f.read()
        if data[:1] != b"\xe9":
            sys.exit("%s is not an ESP program file (it must start with byte E9)" % path)
        if len(data) > SLOT_SIZE[name]:
            sys.exit("%s is %d bytes, its slot holds %d" % (path, len(data), SLOT_SIZE[name]))
        out_name = "%s-%s.bin" % (name, version)
        shutil.copyfile(path, os.path.join(a.out, out_name))
        lines.append("%s %s %d %s %s" % (name, version, len(data), hashlib.sha256(data).hexdigest(), out_name))
        print("%-11s %-10s %9d bytes (%d%% of its slot)" % (name, version, len(data), 100 * len(data) // SLOT_SIZE[name]))
    body = ("\n".join(lines) + "\n").encode()
    r, s = decode_dss_signature(key.sign(body, ec.ECDSA(hashes.SHA256())))
    sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    with open(os.path.join(a.out, "update.txt"), "wb") as f:
        f.write(body + b"sig " + sig.hex().encode() + b"\n")
    key.public_key().verify(key.sign(body, ec.ECDSA(hashes.SHA256())), body, ec.ECDSA(hashes.SHA256()))
    print("Bundle number %d is in %s (list: update.txt)" % (a.number, a.out))


def cmd_serve(a):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=a.out)
    print("Serving %s on port %d (Ctrl+C stops). Boxes use http://<this PC's address>:%d/update.txt" % (a.out, a.port, a.port))
    http.server.ThreadingHTTPServer(("0.0.0.0", a.port), handler).serve_forever()


def main():
    p = argparse.ArgumentParser(description="Make a signed FreeMesh update bundle")
    p.add_argument("--key", default=DEFAULT_KEY, help="the secret signing key file (default: %(default)s)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen").set_defaults(fn=cmd_keygen)
    b = sub.add_parser("bundle")
    b.add_argument("--number", type=int, required=True, help="bundle number, must go up every time")
    b.add_argument("--freemesh", required=True, help="file=version")
    b.add_argument("--meshtastic", required=True, help="file=version")
    b.add_argument("--meshcore", required=True, help="file=version")
    b.add_argument("--out", default=DEFAULT_OUT)
    b.set_defaults(fn=cmd_bundle)
    s = sub.add_parser("serve")
    s.add_argument("--out", default=DEFAULT_OUT)
    s.add_argument("--port", type=int, default=8080)
    s.set_defaults(fn=cmd_serve)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
