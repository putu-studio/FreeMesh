"""Check update.txt the same way the box does: public key from main/app_ota_key.h, raw r||s signature
over every byte before the "sig " line, and each file's size and SHA-256. Also confirms that a changed list is refused.
Usage: python verify-update.py <folder with update.txt> <path to app_ota_key.h>
"""
import hashlib
import os
import re
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

folder, header = sys.argv[1], sys.argv[2]
with open(header) as f:
    pub = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", f.read()))
key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), pub)
with open(os.path.join(folder, "update.txt"), "rb") as f:
    text = f.read()
cut = text.index(b"\nsig ") + 1
sig = bytes.fromhex(text[cut + 4:].strip().decode())
key.verify(encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")), text[:cut], ec.ECDSA(hashes.SHA256()))
print("signature OK (%d signed bytes, list is %d bytes)" % (cut, len(text)))
for line in text[:cut].decode().splitlines()[1:]:
    name, ver, size, sha, fn = line.split()
    data = open(os.path.join(folder, fn), "rb").read()
    ok = len(data) == int(size) and hashlib.sha256(data).hexdigest() == sha
    print("%-11s %-8s %s" % (name, ver, "OK" if ok else "MISMATCH"))
try:
    bad = bytearray(text[:cut]); bad[20] ^= 1
    key.verify(encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")), bytes(bad), ec.ECDSA(hashes.SHA256()))
    print("PROBLEM: a changed list still verified")
except Exception:
    print("a changed list is refused: OK")
