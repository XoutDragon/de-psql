INSERT INTO n SELECT (random()*10000000)::int FROM generate_series(1, 100000);
