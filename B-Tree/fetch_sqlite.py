#!/usr/bin/env python3
"""
fetch_sqlite.py
Downloads the SQLite amalgamation into db/ directory.
Run this once before building: python fetch_sqlite.py
"""
import urllib.request
import zipfile
import os
import sys

SQLITE_URL = "https://www.sqlite.org/2024/sqlite-amalgamation-3450200.zip"
DEST_DIR   = os.path.join(os.path.dirname(__file__), "db")

def main():
    zip_path = os.path.join(DEST_DIR, "sqlite_amal.zip")
    print(f"Downloading SQLite amalgamation from {SQLITE_URL} ...")
    try:
        urllib.request.urlretrieve(SQLITE_URL, zip_path)
    except Exception as e:
        print(f"ERROR: {e}")
        print("Please manually download sqlite-amalgamation-*.zip from https://www.sqlite.org/download.html")
        print(f"and extract sqlite3.h and sqlite3.c into the db/ directory.")
        sys.exit(1)

    print("Extracting...")
    with zipfile.ZipFile(zip_path, 'r') as zf:
        for member in zf.namelist():
            if member.endswith("sqlite3.h") or member.endswith("sqlite3.c"):
                # Strip the subdirectory prefix
                data = zf.read(member)
                fname = os.path.basename(member)
                out_path = os.path.join(DEST_DIR, fname)
                with open(out_path, "wb") as f:
                    f.write(data)
                print(f"  Extracted: {out_path}")

    os.remove(zip_path)
    print("Done. You can now build the project.")

if __name__ == "__main__":
    main()
