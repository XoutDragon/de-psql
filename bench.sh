#!/bin/bash
# bench.sh: 1-row, 1,000-row and 1,000,000-row INSERTs into
#   n (no index), b (B-tree) and d (dynamic_ext), 3 runs each.
# Every test starts from a fresh 50,000-row table, so they are comparable.
# Run:  bash bench.sh | tee results.txt
DB=postgres

reset() {
psql -d $DB -q <<'SQL'
DROP TABLE IF EXISTS n, b, d;
CREATE TABLE n (k int); CREATE TABLE b (k int); CREATE TABLE d (k int);
INSERT INTO n SELECT (random()*2e9 - 1e9)::int FROM generate_series(1, 50000);
INSERT INTO b SELECT k FROM n;
INSERT INTO d SELECT k FROM n;
CREATE INDEX b_k ON b (k);
CREATE INDEX d_k ON d USING dynamic_ext (k);
VACUUM ANALYZE n, b, d;
CHECKPOINT;
SQL
}

cat > /tmp/one.sql <<'SQL'
\set k random(-1000000000, 1000000000)
INSERT INTO TBL VALUES (:k);
SQL
cat > /tmp/batch.sql <<'SQL'
INSERT INTO TBL SELECT (random()*2e9 - 1e9)::int FROM generate_series(1, 1000);
SQL

for run in 1 2 3; do
  for t in n b d; do
    reset
    sed "s/TBL/$t/" /tmp/one.sql > /tmp/cur.sql
    echo "run $run  $t  1-row:     $(pgbench -n -d $DB -f /tmp/cur.sql -c 1 -t 5000 2>&1 | grep 'latency average')"

    reset
    sed "s/TBL/$t/" /tmp/batch.sql > /tmp/cur.sql
    echo "run $run  $t  1000-row:  $(pgbench -n -d $DB -f /tmp/cur.sql -c 1 -t 200 2>&1 | grep 'latency average')"

    reset
    echo "run $run  $t  1M-row:    $(psql -d $DB -c '\timing on' \
         -c "INSERT INTO $t SELECT (random()*2e9 - 1e9)::int FROM generate_series(1, 1000000);" | grep '^Time')"
  done
done
