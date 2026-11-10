INSERT INTO b SELECT (random()*100000)::int FROM generate_series(1, 1000);
