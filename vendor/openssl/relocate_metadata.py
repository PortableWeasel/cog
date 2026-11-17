#!/usr/bin/env python3
"""Keep installed OpenSSL pkg-config paths relative and preserve its license."""
from pathlib import Path
import shutil
import sys

prefix = Path(sys.argv[1])
# The Cog TLS profile deliberately omits loadable engines/providers. Remove
# leftovers when an existing dependency build switches to that smaller profile.
for name in ("engines-3", "ossl-modules"):
    path = prefix / "lib" / name
    if path.exists():
        shutil.rmtree(path)
for path in (prefix / "lib/pkgconfig").glob("*.pc"):
    text = path.read_text()
    text = text.replace(f"prefix={prefix}\n", "prefix=${pcfiledir}/../..\n")
    path.write_text(text)
rehash = prefix / "bin/c_rehash"
if rehash.exists():
    text = rehash.read_text()
    text = text.replace(f'my $prefix = "{prefix}";',
                        'use FindBin;\nmy $prefix = "$FindBin::Bin/..";')
    rehash.write_text(text)
license_dir = prefix / "share/licenses/openssl"
license_dir.mkdir(parents=True, exist_ok=True)
shutil.copy2(sys.argv[2], license_dir / "LICENSE.txt")
