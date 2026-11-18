-- target_app/seed.sql
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS users;

CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    username VARCHAR(50) NOT NULL,
    tier VARCHAR(20) NOT NULL,
    balance INT NOT NULL,
    active INT NOT NULL
);

CREATE TABLE orders (
    id SERIAL PRIMARY KEY,
    user_id INT NOT NULL,
    status VARCHAR(20) NOT NULL,
    region VARCHAR(20) NOT NULL,
    amount INT NOT NULL,
    tax INT NOT NULL
);

INSERT INTO users (username, tier, balance, active) VALUES
    ('arjun_k', 'GOLD', 1200, 1),
    ('meera_s', 'SILVER', 350, 1),
    ('rohan_v', 'GOLD', 850, 1),
    ('divya_n', 'GOLD', 200, 1),
    ('vikram_p', 'PLATINUM', 3200, 0);

INSERT INTO orders (user_id, status, region, amount, tax) VALUES
    (1, 'PAID', 'APAC', 250, 25),
    (1, 'PENDING', 'APAC', 120, 12),
    (2, 'PAID', 'EU', 400, 40),
    (3, 'PAID', 'APAC', 180, 18),
    (3, 'CANCELLED', 'US', 90, 9),
    (4, 'PAID', 'APAC', 75, 8),
    (5, 'PAID', 'APAC', 600, 60);