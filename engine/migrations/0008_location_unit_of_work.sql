-- A location is the unit of work; a client only groups locations (D-102).

-- ---- Locations are switched on by themselves -----------------------------------
--
-- A location has a switch of its own, off at discovery: while it is off the
-- engine only lists it. Its industry, the day its analysis starts from and
-- the audience told to the help with posts are its own, set by a person;
-- industry_key names an industry of the knowledge base by key, without a
-- foreign key (D-084).
ALTER TABLE locations
    ADD COLUMN enabled      boolean NOT NULL DEFAULT false,
    ADD COLUMN industry_key text,
    ADD COLUMN analyze_from date,
    ADD COLUMN audience     text    NOT NULL DEFAULT '' CHECK (char_length(audience) <= 500);

-- Nothing that runs today stops (D-102 point 9): a linked location takes its
-- client's three fields, and one whose client is not archived is switched
-- on. This is the one time anything but a person switches a location on.
UPDATE locations l
   SET industry_key = c.industry_key, analyze_from = c.analyze_from, audience = c.audience, enabled = NOT c.archived
  FROM clients c
 WHERE c.id = l.client_id;

-- Every location has its reply settings, enabled or not (D-093, D-102 point
-- 4): one that has none, because it was never linked, gets the knowledge
-- base's defaults, as discovery now writes them for a new location.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM locations l WHERE NOT EXISTS (SELECT 1 FROM reply_settings r WHERE r.location_id = l.id))
       AND NOT EXISTS (SELECT 1 FROM kb_defaults) THEN
        RAISE EXCEPTION 'a location has no reply settings and no knowledge base is seeded: seed one, then migrate';
    END IF;
END
$$;
INSERT INTO reply_settings (location_id, voice, localization, policy, created_at, updated_at)
SELECT l.id, d.voice, d.localization, d.policy, now(), now()
  FROM locations l CROSS JOIN kb_defaults d
 WHERE NOT EXISTS (SELECT 1 FROM reply_settings r WHERE r.location_id = l.id);

-- The review poll asks for the switch, not for the link.
DROP INDEX locations_poll_idx;
CREATE INDEX locations_poll_idx ON locations (next_review_poll_at) WHERE enabled AND access_status = 'active';

-- Model cost is summed per location (D-057); the index on client_id goes with it.
ALTER TABLE llm_calls DROP COLUMN client_id;

ALTER TABLE clients
    DROP COLUMN industry_key,
    DROP COLUMN analyze_from,
    DROP COLUMN audience;

-- ---- End of the section "Locations are switched on by themselves" --------------
