-- Every location has its time zone (D-109): discovery reads it from Google
-- Places when it inserts a new location, and nothing changes it afterwards.
-- Nothing is carried over: a database with a location that has no zone is
-- rebuilt, not filled in.

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM locations WHERE time_zone IS NULL) THEN
        RAISE EXCEPTION 'A location has no time zone: rebuild this database (request 3.G; nothing is carried over)';
    END IF;
END
$$;

ALTER TABLE locations ALTER COLUMN time_zone SET NOT NULL;
ALTER TABLE locations ADD CONSTRAINT locations_time_zone_check CHECK (btrim(time_zone) <> '');
