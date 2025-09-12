#!/usr/bin/env bash
# Prepare TDengine schema/data for real-environment tests
# Requires taos client and TDengine 3.x running

set -euo pipefail

DB=${DB:-ib_test}
ST=${ST:-s_metrics}
TAG_LOC=${TAG_LOC:-'beijing'}

sql() { taos -s "$1"; }

# Create database
sql "CREATE DATABASE IF NOT EXISTS ${DB} KEEP 36500;"

# Create STABLE
sql "CREATE STABLE IF NOT EXISTS ${DB}.${ST} (ts TIMESTAMP, value INT) TAGS (loc NCHAR(16));"

# Create subtables
for i in 1 2 3; do
  sql "CREATE TABLE IF NOT EXISTS ${DB}.t${i} USING ${DB}.${ST} TAGS ('${TAG_LOC}-${i}');"
 done

# Insert sample rows
for i in 1 2 3; do
  sql "INSERT INTO ${DB}.t${i} VALUES (now-2m, ${i}0) (now-1m, ${i}1) (now, ${i}2);"
 done

# Create TMQ topic for WAL/offset tests (if supported)
# Note: adjust topic name and privileges as needed
sql "CREATE TOPIC IF NOT EXISTS topic_ib_test AS DATABASE ${DB};" || true

echo "TDengine test setup completed for database=${DB} stable=${ST}"
