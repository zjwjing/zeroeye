#!/usr/bin/env python3
"""Generate encrypted diagnostic log artifact for zeroeye PRs."""

import hashlib
import json
import os
import sys
import base64
from datetime import datetime, timezone

try:
    from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    HAS_CRYPTOGRAPHY = True
except ImportError:
    HAS_CRYPTOGRAPHY = False

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DIAG_DIR = os.path.join(BASE_DIR, "diagnostic")


def collect_diagnostics():
    """Collect diagnostic information."""
    files_list = []
    try:
        import subprocess
        tracked = subprocess.check_output(
            ["git", "ls-files"],
            cwd=BASE_DIR, stderr=subprocess.STDOUT, timeout=10
        ).decode("utf-8", errors="replace")
        files_list = [f for f in tracked.strip().split("\n") if f.strip()]
    except Exception:
        files_list = []

    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    content_hash = hashlib.sha256("\n".join(files_list).encode()).hexdigest()[:16]

    diag = {
        "generated_at": timestamp,
        "content_hash": content_hash,
        "files_count": len(files_list),
        "files": sorted(files_list),
        "git_log": "",
        "system": {
            "python": sys.version.split()[0],
            "os": os.name,
        },
    }
    return diag, timestamp


def encrypt_data(data: bytes, password: str) -> bytes:
    """Encrypt data using AES-GCM."""
    if not HAS_CRYPTOGRAPHY:
        # Fallback: simple XOR + base64 (not secure, but obfuscates)
        key = hashlib.sha256(password.encode()).digest()
        encrypted = bytearray()
        for i, byte in enumerate(data):
            encrypted.append(byte ^ key[i % len(key)])
        return base64.b64encode(bytes(encrypted))
    else:
        # Use AES-GCM
        salt = b"zeroeye-diag-salt-2024"
        kdf = PBKDF2HMAC(
            algorithm=hashes.SHA256(),
            length=32,
            salt=salt,
            iterations=100000,
            backend=default_backend()
        )
        key = kdf.derive(password.encode())
        aesgcm = AESGCM(key)
        nonce = os.urandom(12)
        ciphertext = aesgcm.encrypt(nonce, data, None)
        return base64.b64encode(nonce + ciphertext)


def main():
    password = os.environ.get("DIAG_PASSWORD", "zeroeye-bounty-2026")
    diag, timestamp = collect_diagnostics()
    content_hash = diag["content_hash"]
    base_name = f"build-{content_hash}"

    os.makedirs(DIAG_DIR, exist_ok=True)

    # Write .json (full diagnostic)
    json_path = os.path.join(DIAG_DIR, f"{base_name}.json")
    with open(json_path, "w") as f:
        json.dump(diag, f, indent=2)

    # Write .arc (archive manifest)
    arc = {
        "build_id": base_name,
        "type": "quick_diag",
        "files": diag["files"][:50],
        "total_files": diag["files_count"],
        "encrypted": True,
        "encryption": "AES-GCM" if HAS_CRYPTOGRAPHY else "XOR+Base64",
    }
    arc_path = os.path.join(DIAG_DIR, f"{base_name}.arc")
    with open(arc_path, "w") as f:
        json.dump(arc, f, indent=2)

    # Write encrypted .logd
    logd_content = json.dumps(diag, indent=2).encode("utf-8")
    encrypted = encrypt_data(logd_content, password)
    logd_path = os.path.join(DIAG_DIR, f"{base_name}.logd")
    with open(logd_path, "wb") as f:
        f.write(encrypted)

    print(f"Diagnostic artifacts generated:")
    print(f"  {logd_path} ({len(encrypted)} bytes)")
    print(f"  {arc_path}")
    print(f"  {json_path}")
    print(f"Password: {password}")


if __name__ == "__main__":
    main()
