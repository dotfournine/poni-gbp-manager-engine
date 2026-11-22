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

-- ---- Reports and notification rules name a location ------------------------------
--
-- A report and a report schedule are one location's, and a report's
-- recipients are the location_report rules that name that location (D-102
-- point 6). A notification rule names a location or none, every location
-- (point 7). Nothing is deployed: what a development database holds of a
-- client goes to the client's earliest-linked location, the one whose reply
-- settings are the oldest (they were copied when it was linked), as the
-- section on go-live runs chooses it.

CREATE TEMPORARY TABLE client_first_location AS
SELECT DISTINCT ON (l.client_id) l.client_id, l.id AS location_id
  FROM locations l JOIN reply_settings r ON r.location_id = l.id
 WHERE l.client_id IS NOT NULL
 ORDER BY l.client_id, r.created_at, l.id;

-- A schedule of a client that has no location now would report on nothing; it goes.
DELETE FROM report_schedules WHERE client_id NOT IN (SELECT client_id FROM client_first_location);
ALTER TABLE report_schedules ADD COLUMN location_id bigint REFERENCES locations (id);
UPDATE report_schedules s SET location_id = f.location_id FROM client_first_location f WHERE f.client_id = s.client_id;
ALTER TABLE report_schedules ALTER COLUMN location_id SET NOT NULL, DROP COLUMN client_id;
CREATE INDEX report_schedules_location_idx ON report_schedules (location_id);

-- A report of a client that has no location now goes to the first location
-- its numbers list. One without numbers (never built) and without such a
-- client covered nothing; it goes, with its share links.
ALTER TABLE reports ADD COLUMN location_id bigint REFERENCES locations (id);
UPDATE reports p SET location_id = f.location_id FROM client_first_location f WHERE f.client_id = p.client_id;
UPDATE reports p SET location_id = l.id
  FROM locations l
 WHERE p.location_id IS NULL AND jsonb_typeof(p.data->'locations'->0->'location_id') = 'number'
   AND l.id = (p.data->'locations'->0->>'location_id')::bigint;
DELETE FROM report_share_links s USING reports p WHERE p.id = s.report_id AND p.location_id IS NULL;
DELETE FROM reports WHERE location_id IS NULL;
-- A built report's numbers name what they cover: the location, by its title
-- as it is now, where they named the client. Their figures stay as built.
UPDATE reports p
   SET data = (p.data - 'client_id' - 'client_name') || jsonb_build_object('location_id', l.id, 'location_title', l.title)
  FROM locations l
 WHERE l.id = p.location_id AND p.data IS NOT NULL;
ALTER TABLE reports ALTER COLUMN location_id SET NOT NULL, DROP COLUMN client_id;
CREATE INDEX reports_location_idx ON reports (location_id, id DESC);

-- A client's rule becomes the same rule for each of its locations, so that
-- it reaches the same people about the same reviews; its client_report rule
-- becomes a location_report rule per location. A client's rule without a
-- location reached nobody and goes. A rule for every client is one for every
-- location.
ALTER TABLE notification_rules DROP CONSTRAINT notification_rules_event_type_check;
ALTER TABLE notification_rules DROP CONSTRAINT notification_rules_client_report_check;
DROP INDEX notification_rules_scope_idx;
ALTER TABLE notification_rules ADD COLUMN location_id bigint REFERENCES locations (id);
INSERT INTO notification_rules (event_type, location_id, channel, recipients, enabled, created_at, updated_at)
SELECT CASE n.event_type WHEN 'client_report' THEN 'location_report' ELSE n.event_type END, l.id, n.channel, n.recipients, n.enabled,
       n.created_at, n.updated_at
  FROM notification_rules n JOIN locations l ON l.client_id = n.client_id;
DELETE FROM notification_rules WHERE client_id IS NOT NULL;
ALTER TABLE notification_rules DROP COLUMN client_id;
ALTER TABLE notification_rules ADD CONSTRAINT notification_rules_event_type_check
    CHECK (event_type IN ('escalation', 'approval_needed', 'system_problem', 'location_report'));
-- A location_report rule must name a location: one for every location would
-- send each location's report to every other location's recipients.
ALTER TABLE notification_rules ADD CONSTRAINT notification_rules_location_report_check
    CHECK (event_type <> 'location_report' OR location_id IS NOT NULL);
CREATE UNIQUE INDEX notification_rules_scope_idx ON notification_rules (event_type, channel, coalesce(location_id, 0));

-- The log keeps what was sent, under the new name.
ALTER TABLE notification_deliveries DROP CONSTRAINT notification_deliveries_event_type_check;
UPDATE notification_deliveries SET event_type = 'location_report' WHERE event_type = 'client_report';
ALTER TABLE notification_deliveries ADD CONSTRAINT notification_deliveries_event_type_check
    CHECK (event_type IN ('escalation', 'approval_needed', 'system_problem', 'test', 'location_report'));
ALTER TABLE notification_deliveries DROP COLUMN client_id;

DROP TABLE client_first_location;

-- ---- End of the section "Reports and notification rules name a location" -------
